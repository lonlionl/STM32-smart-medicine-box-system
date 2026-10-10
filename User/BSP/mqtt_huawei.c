/**
  ******************************************************************************
  * @file    mqtt_huawei.c
  * @brief   华为云 IoTDA MQTT 接入层 (驱动 ESP-01S AT 固件, 裸机轮询实现)
  ******************************************************************************
  * 【本模块的职责】
  *   把 STM32 的本地业务(开药盒、定时规则、服药记录)翻译成华为云 IoTDA 能听懂的
  *   MQTT 报文, 并把云端下发的命令/属性设置翻译回本地动作。它不直接碰硬件:
  *   - 不操作 USART 寄存器, 不解析 AT 应答的字节流 —— 那是 esp01s_at.c 的活;
  *   - 不操作电磁锁/舵机 —— 那是 app.c 的活(App_OpenBox / App_RemoteSetTimer);
  *   - 不操作 Flash —— 那是 flash.c 的活(Flash_SaveConfig)。
  *   本文件只负责"协议语义": 什么时候连、连不上怎么退避、报文长什么样、
  *   request_id 怎么带回去、定时规则怎么拆成 7 条属性。
  *
  * 【物理链路与分层】
  *   mqtt_huawei.c  (本文件, 协议语义 / 状态机 / JSON 拼装与取值)
  *        |  调用 ESP_CmdSend / ESP_PubRaw / ESP_CmdBusy / ESP_CmdResult
  *        v
  *   esp01s_at.c    (AT 命令收发 / URC 分发 / 512 字节分片 / 通道忙闲)
  *        |  USART2 (PA2=TX, PA3=RX), AT 固件
  *        v
  *   ESP-01S  ---WiFi(TCP 1883)--->  华为云 IoTDA
  *
  * 【运行时模型: 纯裸机, 无 RTOS】
  *   1ms SysTick 只做计时(Timer_GetTick 返回毫秒计数, 32 位自然回绕);
  *   主循环反复调用 MQTT_Process(), 它是唯一入口:
  *       MQTT_Process() -> ESP_Process() 先跑底层收发, 再跑本文件的状态机 switch。
  *   因此本文件里任何一个函数都**不允许阻塞等待**, 只能"发一次命令 -> 下次进来查结果",
  *   超时也靠比较 tick 差值实现。所有耗时动作都必须拆成跨 tick 的小步骤。
  *
  * 【数据流 · 上行(STM32 -> 云)】
  *   app.c 检测到定时/远程触发吃药
  *     -> MQTT_PublishRecord(box, mode)   走"属性上报"主题, 报 box_num/chufa_text/chufa_time
  *   用户按键/界面改了定时, 或收到云端属性设置
  *     -> MQTT_PublishSchedule()          走"属性上报"主题, 分 7 条报 schedule_1..7
  *   收到云端命令后必须回应
  *     -> ReplyCommand() / ReplyPropSet() 走"命令响应/属性设置响应"主题, 且带 request_id
  *   关盒动作完成
  *     -> MQTT_PublishClose()             报 box_num=0 让手机 App 复位显示
  *
  * 【数据流 · 下行(云 -> STM32)】
  *   esp01s_at.c 收到 +MQTTSUBRECV URC 并拆出 (topic, data, len)
  *     -> 回调 MqttOnData(topic, data, len)   <-- 本文件唯一的业务入口
  *        - data 里含 "command_name":"remote_open_box" -> 解析 target_box -> App_OpenBox()
  *        - data 里含 "command_name":"set_schedule"    -> 解析 box/day/hour -> App_RemoteSetTimer()
  *        - topic 里含 "properties/set/request_id="    -> 解析 schedule_1..7 -> 写 g_Cfg + 存 Flash
  *        三条分支最后都要"回响应", 否则华为云判超时(错误码 IOTDA.014111)。
  *
  * 【本模块涉及的 5 个主题(Topic)清单】
  *   # 用途         方向   主题模板(前缀 $oc/devices/{device_id})
  *   1 属性上报     上行   /sys/properties/report
  *                        上报服药记录、box_num/chufa_time、schedule_1..7
  *   2 命令订阅     下行   /sys/commands/#
  *                        通配符订阅。云端下发时真实主题是 /sys/commands/request_id={rid},
  *                        而订阅发生在收到命令之前 —— 那时根本不知道 rid, 只能订 '#'
  *   3 属性设置订阅 下行   /sys/properties/set/#
  *                        同上, 手机 App 改定时走这里
  *   4 命令响应     上行   /sys/commands/response/request_id={rid}
  *                        同步命令必须回应, 且 rid 必须与下发时逐字节一致
  *   5 属性设置响应 上行   /sys/properties/set/response/request_id={rid}
  *                        收到 set 后回 {"result_code":0} 表示处理完毕
  *   注意: 2/3 用 '#' 通配订阅, 4/5 必须用具体 rid 拼接 —— 这就是 ParseRequestId()
  *   存在的理由, 也是 g_CmdRequestId / g_ReplyRequestId 双缓冲存在的理由。
  *
  * 【与 esp01s_at.c 的分工边界(容易混淆, 明确一下)】
  *   ESP_CmdSend(cmd, expect, timeoutMs) 发 AT 命令并登记期望应答; 返回 1=已受理,
  *       0=通道被占(上一单还没结束) —— 注意它**不表示命令成功**, 成功与否要另查
  *       ESP_CmdResult() (0=进行中, 1=成功, 2=失败/超时)。本文件所有状态都遵循
  *       "先发一次 -> 轮询 ESP_CmdBusy() -> 读 ESP_CmdResult()"的三段式。
  *   ESP_PubRaw(topic, data, len, qos, done) 两步法发布(TCP 发送 AT+MQTTPUBRAW,
  *       等 '>' 提示符, 再灌裸字节); 返回 1=已受理, 0=通道忙(背压)。
  *   ESP_CmdBusy()  AT 通道忙闲查询, 本文件用它做背压判断, 决定重试还是暂存。
  *
  * 【贯穿全文件的三个关键坑(后面每处都会再强调)】
  *   坑1 · 512 字节单报文上限: ESP-01S 的 AT 固件把一条 AT+MQTTPUBRAW 报文钉死
  *        在 512 字节, 超了直接发不出去。所以 7 盒 x 7 天 x 4 时间点的整表不能一条报完,
  *        必须拆成 schedule_1..schedule_7 七条属性, 每条=一盒=恰好 56 个数字
  *        (7 天 x 4 时间点 x (时,分), 逗号分隔, 周一->周日, 小时 255 表示该槽未设置)。
  *   坑2 · 同步命令必须回响应: 云端下发命令后会等设备的响应报文, 不回就判超时
  *        (IOTDA.014111)。因此 MqttOnData 的每条分支末尾都必须发响应, **即使解析失败、
  *        药盒号越界也要回**(回的是 result_code, 不是"成功开盒"的承诺)。
  *   坑3 · request_id 必须原样带回, 且它曾经被栈溢出踩坏过: 早期把 1KB 的接收缓冲
  *        放在栈上, 溢出正好覆盖了相邻的 request_id 缓冲区, 导致响应主题里的 rid 变成
  *        垃圾, 云端一直判超时。现在的对策有两条 ——
  *        (a) 接收缓冲 buf[2050] 与解析缓冲都做了长度上限裁剪(len 上限 2048);
  *        (b) 专门增加 g_ReplyRequestId 作为 g_CmdRequestId 的**快照副本**:
  *            g_CmdRequestId 会被后续操作(新的命令/属性上报)反复覆写,
  *            而响应可能在发布通道忙时被推迟重试, 等到真正发出去时原值早已不在,
  *            所以必须在"收到命令的那一刻"就把 rid 拷进 g_ReplyRequestId 冻住。
  *
  * 【不丢任务的背压策略(裸机没有队列, 只能靠暂存标志)】
  *   ESP_PubRaw 返回 0 时不能把任务丢掉, 而是置位标志, 下一个 tick 重试:
  *     g_PendingReply    = 1  命令响应当前没发出去, 待重试
  *     g_PendingSchedule = 1  定时整表还没报完, 待续报
  *     g_SchedIdx        = 0..6 整表报到第几盒了(逐盒推进, 每 tick 一盒)
  *   重试还额外用 REPLY_RETRY_GAP_MS 拉开间隔: ESP-01S 固件拒绝两条紧挨着的 PUBRAW。
  ******************************************************************************
  */
#include "mqtt_huawei.h"
#include "esp01s_at.h"
#include "rtc.h"
#include "app.h"
#include "menu.h"
#include "timer.h"
#include "flash.h"      /* g_Cfg: timer config (7 boxes x 7 days) */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ================================================================
 * 连接状态机状态枚举 (12 个状态)
 * ----------------------------------------------------------------
 * 正常上电后的推进路径(箭头=成功):
 *   IDLE -> AT_INIT -> ATE0 -> WIFI_MODE -> WIFI_JOIN -> NTP_CFG -> NTP_WAIT
 *        -> MQTT_CLEAN -> MQTT_CONN -> MQTT_SUB -> READY
 * 任意一步失败 -> RETRY_WAIT(退避 RETRY_INTERVAL_MS) -> 回到某个安全状态重来。
 *
 * 每个状态的统一处理模式(见下方 MQTT_Process):
 *   1) g_CmdFirst==1 时发一次命令(SendStateCmd)并清标志 —— 保证只发一次;
 *   2) 之后每个 tick 查 ESP_CmdBusy(), 通道不忙就读 ESP_CmdResult();
 *   3) result==1 走成功分支 GotoState(下一个状态), 否则走失败分支(重发或退避)。
 * 这种"发一次 + 轮询"的写法是裸机无 RTOS 环境下的必要约束: 不能在这里死等。
 * ================================================================ */
typedef enum
{
    MQTT_STATE_IDLE = 0,        /* idle (waiting to start) */
    MQTT_STATE_AT_INIT,         /* probe AT */
    MQTT_STATE_ATE0,            /* disable echo */
    MQTT_STATE_WIFI_MODE,       /* set station mode */
    MQTT_STATE_WIFI_JOIN,       /* join WiFi */
    MQTT_STATE_NTP_CFG,         /* configure NTP server */
    MQTT_STATE_NTP_WAIT,        /* wait NTP time */
    MQTT_STATE_MQTT_CLEAN,      /* clean any leftover MQTT state */
    MQTT_STATE_MQTT_CONN,       /* connect Huawei Cloud */
    MQTT_STATE_MQTT_SUB,        /* subscribe command topic */
    MQTT_STATE_READY,           /* ready */
    MQTT_STATE_RETRY_WAIT       /* retry wait after failure */
} MqttState_t;

static MqttState_t g_State = MQTT_STATE_IDLE;   /* 当前状态; 只允许通过 GotoState()/EnterRetryWait() 修改, 以便同步刷新计时基准 */
static MqttState_t g_RetryState = MQTT_STATE_WIFI_JOIN; /* RETRY_WAIT 退避结束后要跳回的目标状态; 初值取 WIFI_JOIN 是因为最常见的断线是 WiFi 掉线 */
static uint32_t    g_StateTick = 0;  /* 状态计时基准(ms)。两种口径: 正常状态=进入时刻, RETRY_WAIT=到期时刻(即"何时该醒") */
static uint8_t     g_CmdFirst = 1;   /* 本状态是否还没发过命令。1=待发(下一次 MQTT_Process 发), 0=已发过, 只轮询结果 —— 防止每 tick 重复发同一条 AT */
static uint8_t     g_NtpDone = 0;    /* MqttOnNtp 回调是否已成功校时并写入 RTC。NTP_WAIT 用它区分"命令回了但时间无效"的情况 */
static uint8_t     g_SubStep = 0;   /* 0=send CONN, 1=send SUB, 2=wait SUBACK */
                                    /* MQTT_SUB 状态内部的子步骤游标: 0/1/2 分别把"建连 -> 订阅命令主题 -> 订阅属性设置主题"三步串成一条链。
                                       订阅有两条独立主题而状态只有一个, 所以必须用游标记录走到哪一步。
                                       每次 GotoState() 都会把它清零, 避免上一次失败残留的游标串味 */

/* Topics (built at init) —— 上电时用 HUAWEI_DEVICE_ID 拼好, 之后只读不改 */
static char g_TopicProp[120];   /* properties report */
                                /* 上行属性上报主题: $oc/devices/{id}/sys/properties/report
                                   服药记录、box_num=0 关盒通知、schedule_1..7 定时整表都发到这里。
                                   长度 120 是按前缀+设备 ID 留足余量的静态分配 —— 全程在 .bss 里,
                                   不放栈上, 就是吸取了坑3(request_id 被栈溢出踩坏)的教训 */
static char g_TopicCmdSub[120]; /* command subscribe */
                                /* 下行命令订阅主题: $oc/devices/{id}/sys/commands/#
                                   末尾用 '#' 而不是具体的 request_id={rid}: 订阅动作发生在
                                   上电连接阶段, 那时还不知道云端将来会用哪个 rid 下发, 只能用通配符 */
static char g_TopicPropSet[120]; /* properties/set subscribe */
                                /* 下行属性设置订阅主题: $oc/devices/{id}/sys/properties/set/#
                                   手机 App 修改定时规则走这条; 同样只能通配订阅 */

/* Pending remote command request id (for command response) */
static char g_CmdRequestId[40];      /* 最近一条下行命令的 request_id(工作副本)。
                                        40 字节足够: 华为云 rid 实际是 UUID 形态(36 字符)左右,
                                        这里多留几位给结尾的 '\0' 和偶发引号。
                                        ? 它是"工作副本", 会被下一条命令直接覆写 —— 真正用于发响应的
                                        是 g_ReplyRequestId(快照), 原因见文件头"坑3" */
static uint8_t g_PendingReply = 0;   /* 1 = command reply queued, retry in loop */
                                     /* 命令响应尚未发出(发布通道忙), 需要下一个 tick 重试。
                                        置位点: ReplyCommand 返回 0 / ReplyDone(result=0);
                                        清位点: ReplyCommand 真正受理成功。READY 状态每 tick 检查它 */
static uint8_t g_ReplyType = 0;      /* 0=remote_open_box 1=set_schedule */
                                     /* 待发响应的类型, 决定响应体用哪份 JSON:
                                        open_box 回 {"open_result":"success"}, set_schedule 回 {"status":"success"}。
                                        之所以要记住类型: 重试可能发生在好几个 tick 之后, 那时
                                        已经无法从上下文知道当初回的是哪条命令, 只能靠这个标志 */
static uint8_t g_PendingSchedule = 0;/* 1 = schedule report queued (retry in loop) */
                                     /* 定时整表(schedule_1..7)还没报完, 需要继续推进。
                                        置位点: 收到 set_schedule / 属性设置成功后;
                                        清位点: MQTT_PublishSchedule() 报完第 7 盒 */
static uint8_t g_SchedIdx = 0;      /* 0..6 next box to report */
                                    /* 定时整表上报进度: 下一个要上报的盒下标(0..6)。
                                        因为一条报文装不下 7 盒, 只能每 tick 推进一盒(见坑1) */

/* ==================== internal helpers ==================== */

/* ----------------------------------------------------------------
 * GotoState - 切换状态并复位本状态的执行上下文
 * 参数: s  要进入的目标状态
 * 返回: 无
 *
 * 原理与设计思路:
 *   状态机的正确性依赖"进入状态时上下文是干净的"。本函数把三件事绑在一起做,
 *   防止调用者漏掉其中之一:
 *     1) g_State = s              记录当前状态;
 *     2) g_StateTick = 当前毫秒   重置超时/退避的计时基准(否则新状态会带着
 *                                 旧状态的时间戳, 表现为"刚进来就超时");
 *     3) g_CmdFirst = 1           宣告"新状态还没发命令", 让 MQTT_Process 去发一次;
 *     4) g_SubStep = 0            复位 MQTT_SUB 的子步骤游标, 保证下次是"重新建连"。
 *   把复位集中在一个函数里, 是裸机 C 代码里避免"散落各处的赋值漏改"的常用手法。
 * ---------------------------------------------------------------- */
static void GotoState(MqttState_t s)
{
    g_State     = s;
    g_StateTick = Timer_GetTick();
    g_CmdFirst  = 1;
    g_SubStep   = 0;
}

/* 进入等待重试状态, 延时后回到目标状态 */
/* ----------------------------------------------------------------
 * EnterRetryWait - 失败退避: 延时 waitMs 后自动回到 target 状态
 * 参数: waitMs  退避时长(毫秒), 传 RETRY_INTERVAL_MS = 5000;
 *       target  退避结束后要回到的状态(重试的"安全起点")
 * 返回: 无
 *
 * 原理与设计思路:
 *   失败时**不能原地立刻重发**: WiFi 没连上时立刻重试只会刷屏 AT 命令,
 *   既没意义又会把 AT 通道堵死; 因此引入一个独立的 RETRY_WAIT 状态做统一退避。
 *   与 GotoState 的关键差别 —— 计时基准的方向相反:
 *     GotoState 写的是"现在"(用于算已过去多久),
 *     EnterRetryWait 写的是"未来时刻 g_StateTick = now + waitMs",
 *     于是 RETRY_WAIT 分支只需判断 (now - g_StateTick) >= 0 即到期。
 *   这种"把到期时刻当基准"的写法配合 32 位无符号自然回绕, 不会因为
 *   Timer_GetTick 溢出而出错(前提是全部用差值比较, 不要直接比大小)。
 *   g_CmdFirst = 0 是刻意的: 退避期间不发命令, 该状态本身不产生任何 AT 流量。
 *
 *   选择 target 的经验法则: 退到"刚刚失败的那一步或它前一步"。
 *   例如 WiFi 连不上就退到 WIFI_JOIN(不需要重跑 AT/ATE0),
 *   MQTT 认证失败就退到 MQTT_CONN, 建连失败退到 MQTT_CONN, 订阅失败退到 MQTT_SUB。
 * ---------------------------------------------------------------- */
static void EnterRetryWait(uint32_t waitMs, MqttState_t target)
{
    g_RetryState = target;
    g_StateTick  = Timer_GetTick() + waitMs;
    g_State      = MQTT_STATE_RETRY_WAIT;
    g_CmdFirst   = 0;
}

/* Build clientId, username, password for Huawei Cloud.
   Signature type 0: use the exact fixed clientId + password from the console,
   because the clientId timestamp and password are generated together and must match. */
/* 生成 MQTT clientId/用户名/密码 (控制台固定值, 必须配套) */
/* ----------------------------------------------------------------
 * BuildMqttCred - 生成华为云 MQTT 三元组(clientId / username / password)
 * 参数: cid,cidLen      输出: clientId 缓冲及其容量
 *       user,userLen    输出: username 缓冲及其容量
 *       pass,passLen    输出: password 缓冲及其容量
 * 返回: 无(结果通过出参返回)
 *
 * 原理与设计思路:
 *   华为云的 MQTT 认证(password 签名类型 0)要求三元组**互相配套**:
 *     clientId = {设备ID}_{时间戳} 形态的固定串, 与 password 由控制台"一次性成对生成",
 *     时间戳参与过 HMAC 计算 —— 一旦自己拼一个 clientId 或改一位密码,
 *     服务端算出的摘要就对不上, 直接 CONNACK 拒绝。
 *   因此这里**不做任何动态计算/时间戳拼接**, 只是把 mqtt_huawei.h 里配置好的
 *   宏原样拷进缓冲区: 设备端不持有密钥、不跑 HMAC, 密码是"提前算好的口令"。
 *   这样做的好处是嵌入式端零密码学依赖, 代价是换设备/换密钥必须改头文件重烧。
 *   统一用 snprintf("%s") 而不是 strcpy, 是为了让"容量不够就静默截断"成为最后一道
 *   防线 —— 截断后认证必然失败, 但至少不会越界写坏内存。
 * ---------------------------------------------------------------- */
static void BuildMqttCred(char *cid, uint32_t cidLen,
                          char *user, uint32_t userLen,
                          char *pass, uint32_t passLen)
{
    /* clientId: exact fixed value from console */
    snprintf(cid, cidLen, "%s", HUAWEI_CLIENT_ID);

    /* username: {device_id} */
    snprintf(user, userLen, "%s", HUAWEI_DEVICE_ID);

    /* password: exact fixed value from console */
    snprintf(pass, passLen, "%s", HUAWEI_STATIC_PWD);
}

/* 发送当前状态对应的 AT 命令并注册期望应答 */
/* ----------------------------------------------------------------
 * SendStateCmd - 按状态发出该状态对应的那一条 AT 命令
 * 参数: s  当前状态(由 MQTT_Process 传入 g_State)
 * 返回: 无
 *
 * 原理与设计思路:
 *   把"哪个状态发什么命令、期待什么应答、给多长超时"集中成一张**表**(switch),
 *   MQTT_Process 里就只剩"发一次 + 轮询"的骨架, 新增状态时只改这一处。
 *   每个 case 的三要素:
 *     命令内容 / expect(期望应答子串) / 超时(ms, 底层据此判 ESP_CmdResult()==2)。
 *   ESP_CmdSend 的返回值这里**有意忽略**: 返回 0 只表示 AT 通道当时忙,
 *   而 g_CmdFirst 已被置 0, 于是本状态会停在"轮询"分支直到通道空出来 ——
 *   由 MQTT_Process 的超时逻辑兜底重来。这就是 SendStateCmd 不含任何阻塞等待的原因。
 *
 *   各状态的命令/应答/超时一览(超时值取"正常应答时间 x 3~5 倍"的经验值):
 *     AT_INIT     "AT"                          expect "OK"             1000ms
 *     ATE0        "ATE0"                        expect "OK"             1000ms
 *     WIFI_MODE   "AT+CWMODE=1"                 expect "OK"             1000ms
 *     WIFI_JOIN   "AT+CWJAP=ssid,pwd"           expect "WIFI GOT IP"   15000ms
 *                 (关联 AP 要扫描+四次握手+DHCP, 是最慢的一步, 故给 15s)
 *     NTP_CFG     "AT+CIPSNTPCFG=1,8,ntp.aliyun.com" expect "OK"         3000ms
 *                 (第 1 个参数 1=使能 SNTP; 第 2 个参数 8=时区 UTC+8,
 *                  所以模组回报的 +CIPSNTPTIME 已经是北京时间, MqttOnNtp 不再加 8 小时)
 *     NTP_WAIT    "AT+CIPSNTPTIME?"             expect "+CIPSNTPTIME"   5000ms
 *                 (查询当前 SNTP 时间, 真正的解析在 MqttOnNtp 回调里做)
 *     MQTT_CLEAN  "AT+MQTTCLEAN=0"              expect "OK"             3000ms
 *                 (第 1 个参数 0=LinkID; 上电残留的 MQTT 会话可能让后续 CONN 直接 ERROR,
 *                  故先清一次。已干净时模组回 ERROR, 本状态同样放行, 见 MQTT_Process)
 *     MQTT_CONN(旧) AT+MQTTUSERCFG=...          expect "OK"             3000ms
 *                 ? 注意: AT+MQTTCONN(真正连 broker)不在这个 case 里,
 *                   而是放在 MQTT_SUB 的 g_SubStep==0 步 —— 见 MQTT_SUB 的注释。
 *     MQTT_SUB    "AT+MQTTSUB=0,<命令订阅主题>,0" expect "OK"            5000ms
 *                 ? 同上: 这一步的命令在本状态内会被 g_SubStep 覆盖,
 *                   此处 case 只在 g_CmdFirst 分支之外被走到, 属历史遗留分支。
 *                 订阅 QoS 固定 0(最多一次): 命令/属性设置报文小, 且云端会重发,
 *                 不值得为 QoS1 引入 PUBACK 往返与本地存储开销。
 * ---------------------------------------------------------------- */
static void SendStateCmd(MqttState_t s)
{
    char cmd[320];   /* 拼命令用的临时缓冲。320 是最大者 AT+MQTTUSERCFG 的需要:
                        固定语法约 40 字符 + clientId(约 60) + user(设备ID约 40) + pass(约 40),
                        留一倍余量。放栈上没问题 —— 本函数只被状态机顺序调用, 无递归。 */

    switch (s)
    {
    case MQTT_STATE_AT_INIT:
        ESP_CmdSend("AT", "OK", 1000u);           /* 最朴素的探活: 模组在不在、波特率对不对 */
        break;

    case MQTT_STATE_ATE0:
        ESP_CmdSend("ATE0", "OK", 1000u);         /* 关回显。不关的话模组会把发出去的命令原样吐回来,
                                                     底层逐行匹配时容易把"命令回显"误当成"应答" */
        break;

    case MQTT_STATE_WIFI_MODE:
        ESP_CmdSend("AT+CWMODE=1", "OK", 1000u);  /* 1=Station 模式(只做客户端连路由器), 不建热点 */
        break;

    case MQTT_STATE_WIFI_JOIN:
        sprintf(cmd, "AT+CWJAP=\"%s\",\"%s\"", WIFI_SSID, WIFI_PASSWORD);
        ESP_CmdSend(cmd, "WIFI GOT IP", 15000u);  /* 等 "WIFI GOT IP" 而不是 "OK": 前者才代表
                                                     DHCP 拿到地址(真正可发数据), 后者只代表命令被接收 */
        break;

    case MQTT_STATE_NTP_CFG:
        ESP_CmdSend("AT+CIPSNTPCFG=1,8,\"ntp.aliyun.com\"", "OK", 3000u); /* 使能 SNTP, 时区 +8, 阿里 NTP。
                                                     用国内 NTP 源而非 pool.ntp.org: 跨网延迟小、成功率高 */
        break;

    case MQTT_STATE_NTP_WAIT:
        ESP_CmdSend("AT+CIPSNTPTIME?", "+CIPSNTPTIME", 5000u); /* 查询。应答行里带时间字符串,
                                                     由 esp01s_at.c 交给 MqttOnNtp 解析 */
        break;

    case MQTT_STATE_MQTT_CLEAN:
        /* close any leftover MQTT connection; ERROR means already clean */
        ESP_CmdSend("AT+MQTTCLEAN=0", "OK", 3000u); /* 0=LinkID。清掉上一次的 MQTT 会话上下文
                                                     (clientId/订阅/遗嘱), 避免新 CONN 被旧状态干扰 */
        break;

    case MQTT_STATE_MQTT_CONN:
    {
        char cid[96], user[64], pass[66];  /* 局部三元组缓冲: 只在拼命令的瞬间需要, 用完即弃,
                                              不占全局内存。容量按控制台串长度 + 余量给 */
        BuildMqttCred(cid, sizeof(cid), user, sizeof(user), pass, sizeof(pass));
        /* AT+MQTTUSERCFG=<LinkID>,<scheme>,<"client_id">,<"username">,<"password">,<cert_key_ID>,<CA_ID>,<"path"> */
        sprintf(cmd, "AT+MQTTUSERCFG=0,%u,\"%s\",\"%s\",\"%s\",0,0,\"\"",
                (unsigned)HUAWEI_MQTT_SCHEME, cid, user, pass);
        /* scheme=1 表示 MQTT over TCP(明文 1883); 后面三个 0/0/"" 是 TLS 证书相关参数,
           本方案不用 TLS —— 证书链需要上百 KB 内存和 NTP 时间, F103 裸机带不动。
           代价是明文传输, 属于教学/演示项目的取舍。 */
        ESP_CmdSend(cmd, "OK", 3000u);
        break;
    }

    case MQTT_STATE_MQTT_SUB:
        sprintf(cmd, "AT+MQTTSUB=0,\"%s\",0", g_TopicCmdSub); /* 订阅命令主题(通配 '#'); 0=QoS0 */
        ESP_CmdSend(cmd, "OK", 5000u);
        break;

    default:
        break;   /* IDLE / READY / RETRY_WAIT 不发命令, 属正常情况而非错误 */
    }
}

/* ==================== event callbacks ==================== */
/* 以下三个回调都由 esp01s_at.c 在解析到对应 URC 时**从中断外的轮询上下文**调用
   (非 ISR, 所以可以放心 printf/改全局变量), 调用时机不在本文件的掌控之内 ——
   这也是它们必须自己判断 g_State 是否允许该动作的原因。 */

/* ----------------------------------------------------------------
 * MqttOnDisconnect - 底层报告 MQTT 断开(收到 +MQTTDISCONNECTED)时的回调
 * 参数: 无
 * 返回: 无
 *
 * 原理与设计思路:
 *   断线可能发生在任何时候(路由器重启、云端踢连接、WiFi 信号差),
 *   回调里**不能做重连动作**(会和在跑的 AT 命令打架), 只能"改状态":
 *   把状态机推入 RETRY_WAIT 退避, 退避结束后从 WIFI_JOIN 重新爬一遍链路。
 *   为什么退到 WIFI_JOIN 而不是 MQTT_CONN: 断开往往是 WiFi 侧的问题,
 *   直接重连 MQTT 大概率还是失败, 从头走一遍代价不大(几秒)但成功率高得多。
 *
 *   两个例外**必须忽略**, 否则会打断正在进行的启动流程:
 *     - g_State == IDLE: 还没开始连, 断线事件是上电残留/误报;
 *     - g_State == AT_INIT: 刚上电探活阶段, 此时 MQTT_CLEAN 还没跑,
 *       模组常会报一条 +MQTTDISCONNECTED, 若据此退避会把启动拖慢 5 秒。
 * ---------------------------------------------------------------- */
static void MqttOnDisconnect(void)
{
    if (g_State != MQTT_STATE_IDLE && g_State != MQTT_STATE_AT_INIT)
    {
        printf("[MQTT] link lost, reconnect...\r\n");
        EnterRetryWait(RETRY_INTERVAL_MS, MQTT_STATE_WIFI_JOIN);
    }
}

/* NTP 时间回调: 解析时间并写入 RTC (北京时间) */
/* ----------------------------------------------------------------
 * MqttOnNtp - 解析模组回报的 SNTP 时间并写入 RTC
 * 参数: s  "+CIPSNTPTIME" 应答后面的时间字符串(可能带前导空格)
 * 返回: 无
 *
 * 原理与设计思路:
 *   为什么需要校时: 本项目的时间戳(RTC 存 Unix 秒)是服药记录和定时判断的依据,
 *   而裸机上电时没有任何可信时间源; 不校时的话, 上报给云端的 chufa_time 会是
 *   1970 年起算的垃圾, 手机 App 看到的时间完全错乱。
 *
 *   为什么要写两种解析格式: ESP-01S 的 AT 固件因版本而异, +CIPSNTPTIME 的
 *   时间串存在两种风格 ——
 *     风格A(ISO, 较新固件): "2024-05-20T13:45:30"
 *     风格B(ctime, 较老固件): "Mon May 20 13:45:30 2024"
 *   先用 sscanf 试 ISO, 失败(返回值不是 6 个字段)再回落到 ctime 风格。
 *   ctime 风格里月份是英文缩写, 需要查 mons[] 表换算成 1~12;
 *   格式串 "%*s %3s %d %d:%d:%d %d" 里的 %*s 就是跳过开头的星期缩写。
 *
 *   时区处理(这里有一处注释与实现的矛盾, 详见交付报告的"问题清单"):
 *   代码上方的英文注释说"NTP 返回 UTC, 要加 8 小时", 紧跟着的中文注释又说
 *   "模组返回的已是北京时间, 直接存", 而**实际代码一行都没加 8 小时**。
 *   代码行为与后者一致, 原因是 NTP_CFG 里把时区参数设成了 8(UTC+8),
 *   模组自己已经做过偏移 —— 所以"不加"是对的, 前面那句英文注释是过时的。
 *
 *   合法性校验 y/mo/d 的边界检查是必要的: NTP 失败时模组可能回全 0 或残缺串,
 *   不校验就会把 RTC 写成 2000 年以前, 定时逻辑随之错乱。
 *   g_NtpDone 置 1 供 NTP_WAIT 状态判定"时间是否真的拿到了"(命令成功 ≠ 时间有效)。
 * ---------------------------------------------------------------- */
static void MqttOnNtp(const char *s)
{
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;

    while (*s == ' ') s++;   /* 应答行里冒号后常带空格, 先跳过, 否则 sscanf 直接失配 */

    if (sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) != 6)
    {
        /* 风格B: ctime —— 月份是英文缩写, 需要查表转成数字 */
        static const char *mons[12] =
        { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
        char mon[4] = { 0 };   /* 4 字节: 3 字符缩写 + '\0', 正好够 strncmp 用 */
        int dd = 0, hh = 0, mm = 0, ss = 0, yy = 0, i;

        if (sscanf(s, "%*s %3s %d %d:%d:%d %d", mon, &dd, &hh, &mm, &ss, &yy) >= 6)
        {
            for (i = 0; i < 12; i++)
            {
                /* 只比 3 个字符: 模组输出的缩写一定是 3 字母, 而 mons[] 也是 3 字母,
                   用 strncmp(...,3) 可避免 mon 里混入意外字符导致比较失败 */
                if (strncmp(mon, mons[i], 3) == 0) { mo = i + 1; break; }
            }
            y = yy; d = dd; h = hh; mi = mm; sec = ss;
        }
    }

    if (y >= 2000 && y <= 2099 && mo >= 1 && mo <= 12 && d >= 1 && d <= 31)
    {
        /* 直接存当前值, 不做时区换算 —— 上方 NTP_CFG 已把模组时区设为 +8,
           +CIPSNTPTIME 返回的本来就是北京时间。
           (历史注记: 早期这里曾错误地再加 8 小时, 导致时间快 8 小时; 已修正。) */
        /* 合法性通过才落库: RTC_SetEpoch 内部只做日历换算, 不校验输入,
           所以"y/mo/d 的范围检查"是唯一的守门人。 */
        RTC_SetEpoch(RTC_ToEpoch((uint16_t)y, (uint8_t)mo, (uint8_t)d,
                                 (uint8_t)h, (uint8_t)mi, (uint8_t)sec));
        g_NtpDone = 1;
        printf("[NTP] sync ok: %04d-%02d-%02d %02d:%02d:%02d (local)\r\n",
               y, mo, d, h, mi, sec);
    }
}

/* Find integer value for key in simple JSON: {"key":123} or {"key":"123"} */
/* ----------------------------------------------------------------
 * JsonGetInt - 从简单 JSON 文本里按 key 取一个整数值
 * 参数: json  待搜索的 JSON 文本(必须已 '\0' 结尾)
 *       key   键名(不含引号)
 *       val   输出: 取到的整数
 * 返回: 1=取到, 0=没找到该键或值不是数字
 *
 * 原理与设计思路(务必理解, 这不是 JSON 解析器):
 *   实现就是"拼出 \"key\" 再 strstr 子串查找", 找到后跳过空格/冒号/制表符,
 *   用 atoi 取数。它只认三种形态: {"key":123} / {"key": "123"} / {"key":-45}。
 *
 *   为什么不用真正的 JSON 解析器: F103 裸机上引入 cJSON 类库要几 KB RAM
 *   外加动态内存分配, 而本项目下行报文的键名、层级都是自己产品模型定死的,
 *   子串查找够用且零分配、零递归。这是典型的"用约束换资源"的取舍。
 *
 *   局限(改动报文结构前必须对照检查):
 *     1) 不处理嵌套: 键名若同时出现在外层和内层, 取到的是**文本上第一个**出现的,
 *        不一定是语义上该层的那个;
 *     2) 不处理转义: 值里的 \" 会被当成字符串边界;
 *     3) 键名可能被误匹配: 搜索的是 "\"key\"", 由于带引号, 一般不会命中
 *        另一个键的值中间, 但若某个**字符串值**里恰好包含 "\"key\":123" 这样的
 *        文本, 就会被误当成字段(本项目报文里不存在这种数据, 属已知接受风险);
 *     4) 未做 key 长度检查: pat[24] 装不下超长 key 会截断(调用方全部用短键名)。
 *   拼 pat 用 sprintf 而非 snprintf: key 是本文件里的字面量, 长度已知可控。
 * ---------------------------------------------------------------- */
static int JsonGetInt(const char *json, const char *key, int *val)
{
    char pat[24];   /* 形如 "target_box", 含两个引号和结尾 '\0' → 最长约 22 字符 */
    const char *p;

    sprintf(pat, "\"%s\"", key);   /* 带引号搜索, 避免命中值里的裸字词 */
    p = strstr(json, pat);
    if (!p) return 0;

    p += strlen(pat);   /* 越过键名 */
    /* 跳过键名与值之间的分隔: 华为云报文可能是 {"key":1} 也可能是 {"key": 1},
       甚至键值间夹制表符, 统一吃掉 */
    while (*p && (*p == ' ' || *p == ':' || *p == '\t')) p++;
    if (*p == '-') { *val = atoi(p); return 1; }              /* 负数(如 hour=-1 之类) */
    if (*p >= '0' && *p <= '9') { *val = atoi(p); return 1; }/* 裸数字 —— 正常路径 */
    if (*p == '"')
    {
        p++;   /* 数字被引号包成字符串(华为云某些字段会这样), 再试一次 */
        if (sscanf(p, "%d", val) == 1) return 1;
    }
    return 0;   /* 值是 null/对象/数组/非数字字符串 → 交给调用者当"没取到" */
}

/* Find string value for key in simple JSON: {"key":"value"} */
/* ----------------------------------------------------------------
 * JsonGetString - 从简单 JSON 文本里按 key 取一个字符串值
 * 参数: json     待搜索的 JSON 文本
 *       key      键名(不含引号)
 *       out      输出缓冲
 *       outSize  输出缓冲容量(含结尾 '\0')
 * 返回: 1=取到, 0=没找到或值不是字符串
 *
 * 原理与设计思路:
 *   与 JsonGetInt 同一套子串查找原理(见其局限说明), 差别是它要求值必须被
 *   双引号包起来 —— 这一条恰好是它的安全网: 只有当键名后面真的跟着一个
 *   字符串字面量时才返回 1, 因此"键名出现在别的值里"这类误匹配大多会被挡掉。
 *   本函数在本文件里唯一的重度使用者是属性设置分支: 它要取出
 *   schedule_1..schedule_7 这七个**超长字符串值**(每盒 56 个数字, 最长约 200 字符),
 *   所以 outSize 必须给足(调用方用 300 字节)。
 *   截断策略是"宁短不乱": n < outSize-1 才继续写, 最后补 '\0',
 *   容量不足时返回的仍是一个合法字符串(只是被截短), 由调用方的
 *   ScheduleParse 靠"数字个数必须正好 56"来判定失败, 不会把半截数据写进 Flash。
 * ---------------------------------------------------------------- */
static int JsonGetString(const char *json, const char *key, char *out, int outSize)
{
    char pat[24];
    const char *p;
    int n = 0;

    sprintf(pat, "\"%s\"", key);
    p = strstr(json, pat);
    if (!p) return 0;

    p += strlen(pat);
    while (*p && (*p == ' ' || *p == ':' || *p == '\t')) p++;  /* 与 JsonGetInt 相同的跳分隔逻辑 */
    if (*p != '"') return 0;   /* 不是字符串值(数字/对象/null) → 明确失败, 不做隐式转换 */
    p++;                       /* 越过起始引号 */
    while (*p && *p != '"' && n < (outSize - 1))
    {
        out[n++] = *p++;       /* 逐字节拷贝, 遇到结束引号或缓冲满即停 */
    }
    out[n] = 0;
    return 1;
}

/* Extract request_id after "request_id=" in the downlink command topic.
   Topic format: $oc/devices/{id}/sys/commands/request_id={request_id} */
/* 从命令主题提取 request_id: .../request_id={rid} */
/* ----------------------------------------------------------------
 * ParseRequestId - 从下行主题里抠出 request_id
 * 参数: topic   下行报文的完整主题
 *       out,outLen  输出缓冲及容量(调用方给 40 字节, 足够 UUID 形态的 rid)
 * 返回: 无。取不到时 out[0]=0(空串), 调用方据此判"没有 rid, 别回响应"
 *
 * 原理与设计思路:
 *   华为云下发的真实主题形如:
 *     $oc/devices/{deviceId}/sys/commands/request_id=5f2b0e1a-1234-4bcd-9abc-0123456789ab
 *   (属性设置则是 .../sys/properties/set/request_id=xxx)
 *   订阅时用的是 '#', 收报文时才知道 rid —— 而发响应时**必须把这个 rid 原样拼回
 *   响应主题**, 所以每收到一条下行报文都得现抠一次。
 *
 *   实现要点:
 *     - 用 "request_id=" 定位, 而不是找第一个 '=', 因为主题前缀里没有 '=' 但
 *       这样做更不容易被将来主题结构变化影响;
 *     - 硬编码 +11 越过 "request_id=" 这 11 个字符(它和上面 strstr 的字面量
 *       必须同步修改, 属易错点);
 *     - 容忍 rid 两侧带引号(某些固件会把 topic 里的值加引号上报), 起始引号跳过;
 *     - 遇到 '/' 或 '"' 或缓冲满即停 —— '/' 是防御性的: 若主题后面还有别的段,
 *       不会被一并吃进来;
 *     - 结尾一定补 '\0', 保证 out 永远是合法 C 字符串。
 *   与"坑3"的关系: 本函数写入 g_CmdRequestId 后, 调用方会立刻快照到
 *   g_ReplyRequestId; 两个缓冲都是文件级静态数组(在 .bss 而非栈上),
 *   从布局上远离那类会溢出的接收缓冲。
 * ---------------------------------------------------------------- */
static void ParseRequestId(const char *topic, char *out, uint32_t outLen)
{
    const char *p = strstr(topic, "request_id=");

    if (!p)
    {
        out[0] = 0;   /* 明确置空而不是留着上次的旧值 —— 否则会把上一条命令的
                         rid 当成本条的用, 云端收到"张冠李戴"的响应照样判超时 */
        return;
    }
    p += 11;                        /* skip "request_id=" */
    if (*p == '"') p++;             /* skip leading quote if present */
    {
        uint32_t n = 0;
        while (*p && *p != '/' && *p != '"' && n < (outLen - 1))
        {
            out[n++] = *p++;
        }
        out[n] = 0;
    }
}

/* Independent snapshot of request_id used only by the reply,
   declared here so it is not adjacent to g_CmdRequestId / g_TopicCmdSub. */
/* 独立的 request_id 快照, 只给"发响应"用。
   为什么要第二份缓冲(而不是复用 g_CmdRequestId)—— 这是本文件最值得记住的设计:
     1) "坑3"的教训: 历史上 1KB 栈缓冲溢出踩坏过 request_id, 导致响应主题带垃圾;
        把真正要发出去的那份放到独立的静态区, 并**故意声明在离 g_CmdRequestId /
        g_TopicCmdSub 较远的位置**(见上方英文注释), 是为了不让一次越界写同时毁掉
        "当前 rid"和"主题"这两样东西 —— 用布局换取故障隔离;
     2) 语义上的必须: 响应的发送是"可延迟"的(g_PendingReply 重试),
        而 g_CmdRequestId 会被下一条命令立刻覆写。若响应直接引用 g_CmdRequestId,
        就会把"上一条命令的响应"发到"这一条命令的 rid"上。
        所以收到命令的那一刻就 strncpy 出快照, 之后只认快照。 */
static char g_ReplyRequestId[40];

/* completion callback for command reply publish */
/* ----------------------------------------------------------------
 * ReplyDone - 命令响应发布完成回调(由 esp01s_at.c 在 PUBRAW 结束后调用)
 * 参数: result  1=发布成功, 0=失败(如固件忙/超时)
 * 返回: 无
 *
 * 原理与设计思路:
 *   ESP_PubRaw 返回 1 只代表"报文已受理并开始发送", 真正的成败要等回调。
 *   失败时不能认栽: 华为云在等这条响应, 超时就会记一条 IOTDA.014111。
 *   所以这里把 g_PendingReply 重新置 1, 由 READY 状态的每个 tick 调
 *   RetryPendingReply() 再试 —— 组成一个"直到成功为止"的小重试环。
 *   注意 g_ReplyRequestId 是快照, 重试时仍然拼得出正确的主题, 这正是"坑3"
 *   里双缓冲设计的收益点。
 * ---------------------------------------------------------------- */
static void ReplyDone(uint8_t result)
{
    printf("[MQTT] cmd reply publish %s\r\n", result ? "OK" : "FAILED");
    if (!result)
    {
        /* failed (e.g. firmware busy), requeue for retry */
        g_PendingReply = 1;
    }
}

/* Reply to Huawei Cloud command; if the pub channel is busy, queue for retry */
/* 回复华为云命令(区分 remote_open_box / set_schedule 响应体) */
/* ----------------------------------------------------------------
 * ReplyCommand - 向云端回应一条命令(同步命令的响应报文)
 * 参数: 无(主题与响应体取自 g_ReplyRequestId / g_ReplyType 两个静态状态)
 * 返回: 无
 *
 * 原理与设计思路:
 *   这是"坑2"(同步命令必须回响应)的执行点。响应体按命令类型二选一,
 *   两份 JSON 都做成 static const 放在 Flash(.rodata)里 —— 响应固定不变,
 *   没必要每次在栈上拼字符串, 也省 RAM:
 *     remote_open_box -> {"result_code":0,"response_name":"remote_open_box",
 *                         "paras":{"open_result":"success"}}
 *     set_schedule    -> {"result_code":0,"response_name":"set_schedule",
 *                         "paras":{"status":"success"}}
 *   result_code=0 是华为云约定的"成功"; response_name 必须与云端下发的命令名一致,
 *   paras 里的字段名要和产品模型里配置的响应参数对得上, 否则平台会判"响应不合法"。
 *
 *   为什么要给"药盒号越界/解析失败"也回 result_code=0: 从平台视角,
 *   "命令已收到并处理完毕"比"业务上是否真的开了那个盒子"更重要 —— 不回响应会被
 *   记为超时故障并触发告警/重发, 而业务结果另有属性上报(服药记录)体现。
 *
 *   两个前置守卫:
 *     - g_State != READY: 连接已断/正在重连时发不出去, 直接返回
 *       (报文没必要排队 —— 重连后云端会重发命令);
 *     - g_ReplyRequestId[0] == 0: 没有 rid 就拼不出响应主题, 发了也是错的主题。
 *
 *   背压处理: ESP_PubRaw 返回 0 表示发布通道忙(常见于定时整表上报还在飞),
 *   此时置 g_PendingReply=1 交给 RetryPendingReply() 稍后再发, **不丢任务**。
 * ---------------------------------------------------------------- */
static void ReplyCommand(void)
{
    char topic[200];   /* 响应主题: 前缀 + 设备ID + "/sys/commands/response/request_id=" + rid,
                          实测约 120 字符, 200 留足余量。放栈上可以 —— 
                          本函数不接收任何下行数据, 不与"坑3"那类大缓冲共存 */
    /* paras matches product model response params */
    static const char respOpen[] =
        "{\"result_code\":0,\"response_name\":\"remote_open_box\",\"paras\":{\"open_result\":\"success\"}}";
    static const char respSet[] =
        "{\"result_code\":0,\"response_name\":\"set_schedule\",\"paras\":{\"status\":\"success\"}}";
    const char *resp = (g_ReplyType == 1u) ? respSet : respOpen;  /* 默认走 open_box 分支,
                                                                     只有显式标了 1 才用 set 响应体 */

    if (g_State != MQTT_STATE_READY)
    {
        return;
    }
    if (g_ReplyRequestId[0] == 0)
    {
        return;
    }
    sprintf(topic, "$oc/devices/%s/sys/commands/response/request_id=%s",
            HUAWEI_DEVICE_ID, g_ReplyRequestId);
    if (ESP_PubRaw(topic, (const uint8_t *)resp, (uint16_t)strlen(resp), 0u, ReplyDone))
    {
        g_PendingReply = 0;   /* 已受理 → 清重试标志; 真失败时 ReplyDone 会再置回 1 */
    }
    else
    {
        g_PendingReply = 1;   /* busy (e.g. attribute report in flight), retry in loop */
    }
}


/* Try to send a queued command reply (called from MQTT_Process when idle) */
/* ----------------------------------------------------------------
 * RetryPendingReply - 重试一条尚未发出的命令响应(READY 状态下每 tick 调用)
 * 参数: 无
 * 返回: 无
 *
 * 原理与设计思路:
 *   裸机没有消息队列, "发不出去就先记着"全靠标志 + 轮询。本函数是那个轮询点,
 *   它要过四道闸才真正发:
 *     1) g_PendingReply 为 0 → 没有待办, 立刻返回(绝大多数 tick 都走这条, 开销极小);
 *     2) 状态不是 READY → 连接不可用, 等重连后云端重发命令, 不必自己攒着;
 *     3) ESP_CmdBusy() → AT 通道正忙(比如定时整表某一条正在发), 这一 tick 让路;
 *     4) 距上次尝试不足 REPLY_RETRY_GAP_MS(300ms) → 再等等。
 *   第 4 条不是可有可无的: ESP-01S 固件在一条 AT+MQTTPUBRAW 刚结束的瞬间
 *   会拒绝紧随其后的下一条, 表现为莫名 FAILED。拉开 300ms 间隔能显著提高成功率,
 *   这也是"重试代价换稳定性"的一个小例子。
 *
 *   lastReplyTick 用 static 局部变量保存: 它的生命周期等于本函数的调用历史,
 *   但**只有本函数看得见它**, 比放文件级变量更能收敛影响范围。
 *
 *   时间比较写成 (int32_t)(now - last) < (int32_t)GAP 而不是 now-last < GAP:
 *   强转成有符号后, 即使 Timer_GetTick() 在第 49.7 天回绕, 差值语义仍然正确
 *   —— 这是无 RTOS 环境里做时间比较的标准写法, 本文件多处使用。
 * ---------------------------------------------------------------- */
static void RetryPendingReply(void)
{
    static uint32_t lastReplyTick = 0;   /* 上一次真正调用 ReplyCommand 的时刻(ms) */

    if (!g_PendingReply)
    {
        return;
    }
    if (g_State != MQTT_STATE_READY)
    {
        return;
    }
    if (ESP_CmdBusy())
    {
        return;
    }
    /* wait a gap after the previous publish completed */
    if ((int32_t)(Timer_GetTick() - lastReplyTick) < (int32_t)REPLY_RETRY_GAP_MS)
    {
        return;
    }
    lastReplyTick = Timer_GetTick();
    ReplyCommand();   /* 不检查返回: 成功会清 g_PendingReply, 失败会重新置 1 并下轮再来 */
}

/* MQTT data callback: handle subscribed command */
/* forward decls (definitions later in this file) */
static int ScheduleParse(uint8_t box, const char *s, TimerCfg_t *cfg);
/* 回复属性设置(properties/set)请求: {"result_code":0} */
static void ReplyPropSet(const char *rid);

/* MQTT 数据回调: 解析收到的命令/属性设置消息 */
/* ----------------------------------------------------------------
 * MqttOnData - 下行报文总入口(esp01s_at.c 收到 +MQTTSUBRECV 并拆好后回调)
 * 参数: topic  报文主题(用来区分是命令还是属性设置, 也是 rid 的来源)
 *       data   报文体(JSON 文本), len  data 的有效字节数(不含结尾 '\0')
 * 返回: 无
 *
 * 原理与设计思路:
 *   先做一件"防御性"的事: 把 data 拷进本地大缓冲并**强制截断到 2048 字节**。
 *   为什么必须拷贝而不能直接解析 data: ESP 驱动的行缓冲是不保证 '\0' 结尾的
 *   裸字节流, 而后面所有 JsonGet* 都依赖 C 字符串语义; 拷贝同时给了长度上限,
 *   这正是"坑3"(1KB 缓冲溢出踩坏 request_id)的修复手段之一 ——
 *   buf[2050] 对应 ESP_DATA_MAX=2048, 多出的 2 字节留给 '\0' 和余量。
 *
 *   三条分支的判定顺序与依据(全部用 strstr 子串匹配, 见 JsonGetInt 的局限说明):
 *     分支1: 正文含 "command_name" 且含 "remote_open_box" → 远程开盒命令
 *            - 立刻抠 rid 并快照;
 *            - target_box 必须在 1..7, 越界只打印不动作(但仍要回响应);
 *            - App_OpenBox(box-1, TRIG_REMOTE): 云端 1..7 → 内部下标 0..6;
 *              TRIG_REMOTE 让记录里能区分"定时触发"和"远程手动触发"。
 *     分支2: 正文含 "command_name" 且含 "set_schedule" → 远程改定时命令
 *            - day 用云端约定 1=周一..7=周日, 存内部数组要 -1;
 *            - hour 允许 0..24(24 是"删除该时间点"的哨兵值, 见 app.c 约定);
 *            - 成功后置 g_PendingSchedule, 让新规则立刻上报回云端影子,
 *              否则手机 App 看到的还是旧规则。
 *     分支3: 主题含 "properties/set/request_id=" → 属性设置(下发整表定时)
 *            - 与分支1/2 判定依据不同: 这里**看主题**而不是正文, 因为属性设置
 *              没有 command_name 字段, 只能靠主题路径区分;
 *            - 循环取 schedule_1..schedule_7 七个键, 逐个解析;
 *            - 关键安全点: 先 tmp = g_Cfg 拷一份副本, **只覆盖云端下发的那几盒**,
 *              解析全部成功才整体提交并落盘。若直接改 g_Cfg, 一旦某盒解析失败
 *              或报文被 512 字节截断, 未提及的盒会被清成垃圾(见第 411 行注释);
 *            - 最后无论成功失败都要回 {"result_code":0}(坑2)。
 *   三条分支是互斥的 else-if: 一条下行报文只可能是其中一类。
 * ---------------------------------------------------------------- */
static void MqttOnData(const char *topic, const char *data, uint16_t len)
{
    char buf[2050];   /* 下行正文工作缓冲。2048(截断上限) + 结尾 '\0' + 1 字节余量。
                         ? 这是本文件最大的栈对象, 也是"坑3"的案发现场:
                         早期版本这里更小(约 1KB)且未做长度裁剪, 一旦云端报文超长
                         就会溢出覆盖相邻局部/全局, 把 request_id 踩成乱码。
                         现在的双重防护 = 大缓冲 + 显式 len 裁剪。 */
    int  box;

    if (len > 2048) len = 2048;      /* 硬性裁剪: 宁可丢尾部, 绝不越界写 */
    memcpy(buf, data, len);          /* 按字节拷贝(报文是 UTF-8 裸字节, 不能当字符串处理) */
    buf[len] = 0;                    /* 补字符串结束符, 让 JsonGet* 可以安全用 strstr/atoi */

    if (strstr(buf, "\"command_name\"") && strstr(buf, "\"remote_open_box\""))
    {
        /* capture request_id from topic for command response */
        ParseRequestId(topic, g_CmdRequestId, sizeof(g_CmdRequestId));
        /* snapshot rid into a separate buffer used only for the reply,
           because g_CmdRequestId may be corrupted by the attribute report */
        /* 立刻快照(坑3的核心对策): 响应可能因通道忙被推迟到几个 tick 之后才发,
           那时 g_CmdRequestId 可能已被下一条命令覆写。这里用 strncpy 并
           显式补 '\0' —— strncpy 在源串超长时不会补结束符, 手工补上才安全。 */
        strncpy(g_ReplyRequestId, g_CmdRequestId, sizeof(g_ReplyRequestId) - 1);
        g_ReplyRequestId[sizeof(g_ReplyRequestId) - 1] = 0;

        if (JsonGetInt(buf, "target_box", &box) && box >= 1 && box <= 7)
        {
            printf("[MQTT] remote open box %d\r\n", box);
            App_OpenBox((uint8_t)(box - 1), TRIG_REMOTE);   /* 云端 1..7 → 内部 0..6 */
        }
        /* 注意这里没有 else: target_box 缺失/越界时**什么都不做**, 但下面的
           响应照发 —— 平台要的是"命令已处理"的回执, 不是业务结果 */

        /* always reply to the command (success or unknown box) */
        g_ReplyType = 0;   /* 先定响应体类型, 再调用 —— 顺序不能反, ReplyCommand 靠它选 JSON */
        ReplyCommand();
    }
    else if (strstr(buf, "\"command_name\"") && strstr(buf, "\"set_schedule\""))
    {
        int day = 0, hour = 0;

        ParseRequestId(topic, g_CmdRequestId, sizeof(g_CmdRequestId));
        strncpy(g_ReplyRequestId, g_CmdRequestId, sizeof(g_ReplyRequestId) - 1);
        g_ReplyRequestId[sizeof(g_ReplyRequestId) - 1] = 0;   /* 同分支1: 先用后改, 必须快照 */

        /* API day convention: 1=Monday..7=Sunday; store as array index 0..6 */
        /* hour 的合法区间是 0..24 而非 0..23: 24 是业务约定的"删除该时间点"哨兵,
           由 App_RemoteSetTimer 负责翻译。四个条件全部满足才执行,
           任一不满足则只回响应不改配置 —— 把"脏数据不进 Flash"这条守在设计里。 */
        if (JsonGetInt(buf, "box", &box) && JsonGetInt(buf, "day", &day) &&
            JsonGetInt(buf, "hour", &hour) &&
            box >= 1 && box <= 7 && day >= 1 && day <= 7 && hour >= 0 && hour <= 24)
        {
            printf("[MQTT] set schedule box=%d day=%d hour=%d\r\n", box, day, hour);
            App_RemoteSetTimer((uint8_t)(box - 1), (uint8_t)(day - 1), (uint8_t)hour);
            g_PendingSchedule = 1;   /* report new schedule after reply */
                                     /* 改完必须回读上报: 云端的属性影子还存着旧表,
                                        不回读的话手机 App 界面会一直显示旧规则 */
        }
        g_ReplyType = 1;   /* 用 set_schedule 的响应体 */
        ReplyCommand();
    }
    else if (strstr(topic, "properties/set/request_id="))
    {
        char rid[40];
        TimerCfg_t tmp;   /* 配置副本。TimerCfg_t 约 400 字节, 放栈上而非全局:
                             它是"本函数私有的草稿", 用栈能天然避免与 g_Cfg 混淆,
                             而且本回调由主循环调用, 栈深度可控 */

        uint8_t b;
        int got = 0;      /* 是否至少成功解析了一盒 —— 决定要不要提交副本 */

        /* 先拷贝当前配置, 只覆盖下发的那一盒 (其他盒保持原值, 否则会变垃圾) */
        tmp = g_Cfg;   /* 结构体整体赋值 = memcpy 400 字节, 一次搞定。
                          这是"写前先复制"策略的关键一步: 云端通常只下发被改动的那盒,
                          没下发的盒必须原样保留, 因此绝不能在 g_Cfg 上原地解析 */

        for (b = 0; b < BOX_NUM; b++)   /* BOX_NUM = 7 */
        {
            char key[16], sched[300];   /* key: "schedule_7" 最长 10 字符;
                                           sched: 一盒 56 个数字(每数最多 3 位 + 逗号,
                                           约 200 字符), 300 有余量。
                                           这正是"坑1"(512 字节上限)在代码里的另一半体现:
                                           一盒一份, 分批上报/解析 */

            sprintf(key, "schedule_%u", (unsigned)b + 1u);   /* 属性名从 1 开始, 数组从 0 开始 */
            if (JsonGetString(buf, key, sched, sizeof(sched)))
            {
                printf("[MQTT] got %s len=%u\r\n", key, (unsigned)strlen(sched));
                if (ScheduleParse(b, sched, &tmp) == 1)
                {
                    got = 1;
                }
                else
                {
                    /* 解析失败只打印前 20 字符: 串口日志带宽有限, 打全了反而淹没关键信息,
                       而前 20 字符足够看出"是不是被截断/格式不对" */
                    printf("[MQTT] parse fail %s first=%.20s\r\n", key, sched);
                }
            }
        }
        if (got)
        {
            g_Cfg = tmp;   /* 一次性提交: 全部解析完才动全局配置, 中途失败不影响现网规则 */
            if (Flash_SaveConfig(&g_Cfg)) printf("[MQTT] schedule set ok\r\n");
            g_PendingSchedule = 1;   /* report new schedule to shadow */
                                     /* 落盘后立刻回读上报, 让云端影子与设备一致 */
        }
        else
        {
            printf("[MQTT] no schedule_N key, buf len=%u\r\n", (unsigned)len);
        }
        /* always reply result_code (even on parse failure) */
        /* 坑2 在这里的体现: 解析失败也必须回响应, 否则平台记 IOTDA.014111 超时。
           注意 rid 用的是本轮从主题现抠的局部串(而非 g_ReplyRequestId):
           属性设置响应是**立即**发生的, 不需要为延迟重试保快照 */
        ParseRequestId(topic, rid, sizeof(rid));
        if (rid[0]) ReplyPropSet(rid);   /* 抠不到 rid 就不发: 拼不出正确主题, 发了也是错 */
    }
}

/* 序列化【单个药盒】的定时规则: 输出 56 个逗号分隔的数字
   = 7 天 x 4 时间点 x (时,分), 顺序周一->周日, 每点先时后分; 未设置的小时写 255。
   注意不是整表: 函数只处理入参 box 这一盒, 调用方按盒循环并逐盒上报
   (schedule_1..schedule_7 七个云属性, 为绕开 ESP-01S 512 字节报文上限)。
   历史注记: 注释曾写 "49 values / box-major / 0=未设置", 与实现不符, 已更正。 */
/* ----------------------------------------------------------------
 * ScheduleSerialize - 把一盒的定时规则序列化成 56 个逗号分隔的数字
 * 参数: box      药盒下标 0..6
 *       out      输出缓冲
 *       outSize  缓冲容量
 * 返回: 写入的字符数(与 snprintf 语义一致的累计长度, 供调用方接着往后拼)
 *
 * 原理与设计思路(坑1 的正面实现):
 *   格式: 7 天 x 4 时间点 x (时,分) = 56 个数字, 顺序为周一->周日、
 *         每天 4 个时间点, 每个时间点先小时后分钟。例:
 *         "7,30,255,0,255,0,255,0, 8,0,..." 第一个时间点=07:30,
 *         后面三组 "255,0" 表示该槽未设置。
 *   为什么是裸数字串而不是嵌套 JSON 对象: 512 字节的报文上限下, 嵌套 JSON 光是
 *   键名和括号就要多花几倍字节, 而裸数字串一盒约 200 字符, 刚好装得下 —— 
 *   这是"用最省字节的编码换取能一条报文发完一盒"的取舍。
 *
 *   ★ 255 的语义(全文件最容易踩的坑):
 *     hour[d][t] == 0xFF 表示该槽**未设置**, 序列化成数字 255 发给云端;
 *     **绝不能用 0 表示未设置**, 因为 0 是合法时刻"0 点(午夜)"。
 *     一旦用 0 当空值, 用户在 0 点设的提醒会静默失效, 而所有空槽会被当成
 *     "每天 0 点提醒"集体误触发。云端的规则引擎与手机 App 都按 255=空 来理解。
 *   注意未设置时输出的分钟被强制写成 0(而不是 min[] 里的垃圾值):
 *   未用槽的 min 字段是无效数据(见 flash.h 的 BoxRule_t 说明), 直接发出去
 *   会让云端看到 "255,37" 这种自相矛盾的组合, 故显式归零。
 *
 *   逗号的处理: 第一个数字前不加逗号, 其余都加 —— 实现方式是判断
 *   (d==0 && t==0) 这个"是不是第一个"的条件, 比在循环外单独打印首元素更紧凑。
 *   注意返回值是**累计长度**而不是"是否溢出": snprintf 在截断时返回的是
 *   "本应写入的长度", 所以 pos 有可能超过 outSize, 调用方必须保证缓冲足够
 *   (仓库里的调用点给了 300 字节, 实测一盒约 200 字符, 安全)。
 * ---------------------------------------------------------------- */
static int ScheduleSerialize(uint8_t box, char *out, int outSize)
{
    int pos = 0;      /* 已写入的字符数, 同时作为下一次写入的偏移 */
    uint8_t d, t;     /* d = 星期下标 0..6(0=周一); t = 当天第几个时间点 0..3 */

    for (d = 0; d < 7u; d++)
    {
        for (t = 0; t < BOX_MAX_TIMES; t++)   /* BOX_MAX_TIMES = 4 */
        {
            const BoxRule_t *r = &g_Cfg.rules[box];   /* 每轮重取指针: 编译器可省掉重复寻址;
                                                         放循环体内不影响正确性(g_Cfg 不会被本函数改) */
            /* 未设置的槽: 小时发 255, 分钟发 0 —— 见上方 255 语义说明。
               这两个三元表达式是"255=空"约定唯一的落地位置, 改动前请三思 */
            int h = (r->hour[d][t] == 0xFFu) ? 255 : (int)r->hour[d][t];
            int m = (r->hour[d][t] == 0xFFu) ? 0   : (int)r->min[d][t];

            pos += snprintf(out + pos, outSize - pos, "%s%d,%d",
                            (d == 0u && t == 0u) ? "" : ",", h, m);
        }
    }
    return pos;
}

/* Parse one box (56 nums) into cfg->rules[box]; return 1 on success */
static int ScheduleParse(uint8_t box, const char *s, TimerCfg_t *cfg)
{
    const char *p = s;
    int idx = 0;

    memset(cfg->rules[box].hour, 0xFF, sizeof(cfg->rules[box].hour));
    memset(cfg->rules[box].min, 0, sizeof(cfg->rules[box].min));
    while (*p)
    {
        int val;
        uint8_t d, t;

        if (*p < '0' || *p > '9') { p++; continue; }   /* skip separators */
        val = atoi(p);
        while (*p >= '0' && *p <= '9') p++;
        if (idx >= 56) return 0;

        d = (uint8_t)(idx / 8);
        t = (uint8_t)((idx % 8) / 2);
        if ((idx % 2) == 0u)   /* hour */
        {
            cfg->rules[box].hour[d][t] = (uint8_t)((val > 255) ? 255 : val);
        }
        else                     /* min */
        {
            cfg->rules[box].min[d][t] = (uint8_t)val;
        }
        idx++;
    }
    return (idx == 56) ? 1 : 0;
}

/* Parse the schedule wire format into cfg->rules; return 1 on success */
/* Reply to a properties/set request with result_code */
static void ReplyPropSet(const char *rid)
{
    char topic[200];
    static const char resp[] = "{\"result_code\":0}";

    if (g_State != MQTT_STATE_READY) return;
    sprintf(topic, "$oc/devices/%s/sys/properties/set/response/request_id=%s",
            HUAWEI_DEVICE_ID, rid);
    ESP_PubRaw(topic, (const uint8_t *)resp, (uint16_t)strlen(resp), 0u, NULL);
}

/* Report the full timer schedule as a property; Huawei shadow stores it. */
/* Report all boxes as separate properties schedule_1..schedule_7.
   Each box ~150-200 chars < ESP-01S 512-byte message limit. */
/* 逐盒上报定时规则 schedule_1~7 (每盒一条, 规避512字节限制) */
void MQTT_PublishSchedule(void)
{
    uint8_t sent = 0;

    if (g_State != MQTT_STATE_READY) return;

    /* send one box per call; driven from READY loop until all 7 sent */
    while (g_SchedIdx < BOX_NUM)
    {
        char payload[300];
        int pos;

        pos = sprintf(payload,
                "{\"services\":[{\"service_id\":\"%s\",\"properties\":{\"schedule_%u\":\"",
                HUAWEI_SERVICE_ID, (unsigned)g_SchedIdx + 1u);
        pos += ScheduleSerialize(g_SchedIdx, payload + pos, (int)sizeof(payload) - pos);
        pos += sprintf(payload + pos, "\"}}]}");

        if (ESP_PubRaw(g_TopicProp, (const uint8_t *)payload,
                       (uint16_t)strlen(payload), 0u, NULL))
        {
            printf("[MQTT] schedule_%u report\r\n", (unsigned)g_SchedIdx + 1u);
            g_SchedIdx++;          /* this box sent, next call sends next */
            sent = 1;
            break;                 /* wait for next loop tick */
        }
        break;                     /* busy: retry same box next tick */
    }
    if (g_SchedIdx >= BOX_NUM)
    {
        g_SchedIdx = 0;
        g_PendingSchedule = 0;
        printf("[MQTT] schedule report all ok\r\n");
    }
    else if (sent)
    {
        g_PendingSchedule = 1;   /* keep driving */
    }
}

/* Report that all boxes are closed. box_num=0 signals "closed" to the cloud
   (shadow updates), so the phone app can reset its open-state display. */
/* 上报"全部药盒已关闭"(box_num=0), 供手机App同步显示 */
void MQTT_PublishClose(void)
{
    char payload[160];
    RtcTime_t t;

    if (g_State != MQTT_STATE_READY)
    {
        return;
    }

    RTC_GetTime(&t);
    sprintf(payload,
            "{\"services\":[{\"service_id\":\"%s\",\"properties\":{\"box_num\":0,"
            "\"chufa_time\":\"%04u-%02u-%02u %02u:%02u:%02u\"}}]}",
            HUAWEI_SERVICE_ID,
            (unsigned)t.year, (unsigned)t.month, (unsigned)t.day,
            (unsigned)t.hour, (unsigned)t.minute, (unsigned)t.second);

    if (ESP_PubRaw(g_TopicProp, (const uint8_t *)payload,
                   (uint16_t)strlen(payload), 0u, NULL))
    {
        printf("[MQTT] boxes closed report ok\r\n");
    }
}

/* ==================== public interface ==================== */

void MQTT_Init(void)
{
    ESP_Init();
    ESP_RegDataCb(MqttOnData);
    ESP_RegDisconnectCb(MqttOnDisconnect);
    ESP_RegNtpCb(MqttOnNtp);

    /* property report topic */
    sprintf(g_TopicProp,  "$oc/devices/%s/sys/properties/report", HUAWEI_DEVICE_ID);
    /* command subscribe topic (wildcard replaces request_id={request_id}) */
    sprintf(g_TopicCmdSub, "$oc/devices/%s/sys/commands/#", HUAWEI_DEVICE_ID);
    sprintf(g_TopicPropSet, "$oc/devices/%s/sys/properties/set/#", HUAWEI_DEVICE_ID);

    printf("[MQTT] init, device=%s\r\n", HUAWEI_DEVICE_ID);
    g_State     = MQTT_STATE_IDLE;
    g_StateTick = Timer_GetTick();
    g_CmdFirst  = 0;
}

/* 主循环驱动: 处理AT接收 + 连接状态机 */
void MQTT_Process(void)
{
    ESP_Process();

    switch (g_State)
    {
    case MQTT_STATE_IDLE:
        if ((int32_t)(Timer_GetTick() - g_StateTick) >= 200)
        {
            printf("[MQTT] start connect...\r\n");
            GotoState(MQTT_STATE_AT_INIT);
        }
        break;

    case MQTT_STATE_AT_INIT:
        if (g_CmdFirst) { SendStateCmd(g_State); g_CmdFirst = 0; break; }
        if (!ESP_CmdBusy())
        {
            if (ESP_CmdResult() == 1u) GotoState(MQTT_STATE_ATE0);
            else                       GotoState(MQTT_STATE_AT_INIT);
        }
        break;

    case MQTT_STATE_ATE0:
        if (g_CmdFirst) { SendStateCmd(g_State); g_CmdFirst = 0; break; }
        if (!ESP_CmdBusy())
        {
            if (ESP_CmdResult() == 1u) GotoState(MQTT_STATE_WIFI_MODE);
            else                       GotoState(MQTT_STATE_ATE0);
        }
        break;

    case MQTT_STATE_WIFI_MODE:
        if (g_CmdFirst) { SendStateCmd(g_State); g_CmdFirst = 0; break; }
        if (!ESP_CmdBusy())
        {
            if (ESP_CmdResult() == 1u) GotoState(MQTT_STATE_WIFI_JOIN);
            else                       GotoState(MQTT_STATE_WIFI_MODE);
        }
        break;

    case MQTT_STATE_WIFI_JOIN:
        if (g_CmdFirst) { SendStateCmd(g_State); g_CmdFirst = 0; break; }
        if (!ESP_CmdBusy())
        {
            if (ESP_CmdResult() == 1u)
            {
                printf("[MQTT] WiFi connected\r\n");
                g_NtpDone = 0;
                GotoState(MQTT_STATE_NTP_CFG);
            }
            else
            {
                printf("[MQTT] WiFi fail, retry in 5s\r\n");
                EnterRetryWait(RETRY_INTERVAL_MS, MQTT_STATE_WIFI_JOIN);
            }
        }
        break;

    case MQTT_STATE_NTP_CFG:
        if (g_CmdFirst) { SendStateCmd(g_State); g_CmdFirst = 0; break; }
        if (!ESP_CmdBusy())
        {
            if (ESP_CmdResult() == 1u) GotoState(MQTT_STATE_NTP_WAIT);
            else                       GotoState(MQTT_STATE_MQTT_CLEAN);
        }
        break;

    case MQTT_STATE_NTP_WAIT:
        if (g_CmdFirst) { SendStateCmd(g_State); g_CmdFirst = 0; break; }
        if (!ESP_CmdBusy())
        {
            if (ESP_CmdResult() == 1u && g_NtpDone)
            {
                printf("[MQTT] NTP time ready, proceed\r\n");
            }
            GotoState(MQTT_STATE_MQTT_CLEAN);
        }
        break;

    case MQTT_STATE_MQTT_CLEAN:
        if (g_CmdFirst) { SendStateCmd(g_State); g_CmdFirst = 0; break; }
        if (!ESP_CmdBusy())
        {
            /* ERROR is also OK (MQTT already clean/uninitialized) */
            GotoState(MQTT_STATE_MQTT_CONN);
        }
        break;

    case MQTT_STATE_MQTT_CONN:
        if (g_CmdFirst) { SendStateCmd(g_State); g_CmdFirst = 0; break; }
        if (!ESP_CmdBusy())
        {
            if (ESP_CmdResult() == 1u)
            {
                printf("[MQTT] auth cfg ok, connecting broker...\r\n");
                GotoState(MQTT_STATE_MQTT_SUB);
            }
            else
            {
                printf("[MQTT] auth cfg fail, retry in 5s\r\n");
                EnterRetryWait(RETRY_INTERVAL_MS, MQTT_STATE_MQTT_CONN);
            }
        }
        break;

    case MQTT_STATE_MQTT_SUB:
        if (g_CmdFirst)
        {
            char cmd[160];
            /* step 0: connect to broker */
            if (g_SubStep == 0u)
            {
                sprintf(cmd, "AT+MQTTCONN=0,\"%s\",%u,0", HUAWEI_HOST, (unsigned)HUAWEI_PORT);
                ESP_CmdSend(cmd, "+MQTTCONNECTED", 10000u);
            }
            /* step 1: subscribe commands */
            else if (g_SubStep == 1u)
            {
                sprintf(cmd, "AT+MQTTSUB=0,\"%s\",0", g_TopicCmdSub);
                ESP_CmdSend(cmd, "OK", 5000u);
            }
            /* step 2: subscribe properties/set */
            else if (g_SubStep == 2u)
            {
                sprintf(cmd, "AT+MQTTSUB=0,\"%s\",0", g_TopicPropSet);
                ESP_CmdSend(cmd, "OK", 5000u);
            }
            g_SubStep++;
            g_CmdFirst = 0;
            break;
        }
        if (!ESP_CmdBusy())
        {
            if (ESP_CmdResult() == 1u)
            {
                if (g_SubStep <= 2u)
                {
                    /* send next subscribe step now */
                    char cmd[160];
                    sprintf(cmd, "AT+MQTTSUB=0,\"%s\",0",
                            (g_SubStep == 2u) ? g_TopicPropSet : g_TopicCmdSub);
                    ESP_CmdSend(cmd, "OK", 5000u);
                    g_SubStep++;
                    g_CmdFirst = 0;
                }
                else
                {
                    printf("[MQTT] subscribed, ready\r\n");
                    GotoState(MQTT_STATE_READY);
                }
            }
            else
            {
                if (g_SubStep == 1u)
                {
                    printf("[MQTT] broker conn fail, retry in 5s\r\n");
                    EnterRetryWait(RETRY_INTERVAL_MS, MQTT_STATE_MQTT_CONN);
                }
                else
                {
                    printf("[MQTT] subscribe fail, retry in 5s\r\n");
                    EnterRetryWait(RETRY_INTERVAL_MS, MQTT_STATE_MQTT_SUB);
                }
            }
        }
        break;
    case MQTT_STATE_READY:
        /* report schedule once on entering ready, retry if pub was busy */
        if (g_CmdFirst)
        {
            g_CmdFirst = 0;
            MQTT_PublishSchedule();
        }
        else if (g_PendingSchedule)
        {
            MQTT_PublishSchedule();
        }
        /* retry queued command reply if pub was busy */
        RetryPendingReply();
        break;

    case MQTT_STATE_RETRY_WAIT:
        if ((int32_t)(Timer_GetTick() - g_StateTick) >= 0)
        {
            GotoState(g_RetryState);
        }
        break;

    default:
        break;
    }
}

uint8_t MQTT_IsWifiConnected(void)
{
    return (g_State == MQTT_STATE_NTP_CFG || g_State == MQTT_STATE_NTP_WAIT ||
            g_State == MQTT_STATE_MQTT_CLEAN || g_State == MQTT_STATE_MQTT_CONN ||
            g_State == MQTT_STATE_MQTT_SUB || g_State == MQTT_STATE_READY);
}

uint8_t MQTT_IsReady(void)
{
    return (g_State == MQTT_STATE_READY);
}

LinkState_t MQTT_GetLinkState(void)
{
    if (g_State == MQTT_STATE_READY)            return LNK_READY;
    if (g_State == MQTT_STATE_NTP_CFG ||
        g_State == MQTT_STATE_NTP_WAIT ||
        g_State == MQTT_STATE_MQTT_CLEAN ||
        g_State == MQTT_STATE_MQTT_CONN ||
        g_State == MQTT_STATE_MQTT_SUB)         return LNK_MQTT;
    if (g_State == MQTT_STATE_RETRY_WAIT)
    {
        return (g_RetryState == MQTT_STATE_WIFI_JOIN) ? LNK_WIFI : LNK_MQTT;
    }
    if (g_State == MQTT_STATE_IDLE)             return LNK_NONE;
    return LNK_WIFI;
}

/* Report medicine-taking record via property report (PUBRAW, JSON payload).
   box: 0~6, mode: 0=timer trigger 1=remote trigger */
/* 上报服药记录(属性): box_num/chufa_text/chufa_time */
void MQTT_PublishRecord(uint8_t box, uint8_t mode)
{
    static const uint8_t textTimer[] =
        { 0xE5,0xAE,0x9A,0xE6,0x97,0xB6,0xE8,0xA7,0xA6,0xE5,0x8F,0x91 }; /* "ding shi chu fa" (UTF-8) */
    static const uint8_t textRemote[] =
        { 0xE8,0xBF,0x9C,0xE7,0xA8,0x8B,0xE6,0x89,0x8B,0xE5,0x8A,0xA8,0xE8,0xA7,0xA6,0xE5,0x8F,0x91 }; /* "yuan cheng shou dong" (UTF-8) */

    char payload[200];
    RtcTime_t t;
    const uint8_t *text;
    int textLen;

    if (box >= BOX_NUM) return;

    RTC_GetTime(&t);
    if (mode == TRIG_TIMER)
    {
        text = textTimer; textLen = (int)sizeof(textTimer);
    }
    else
    {
        text = textRemote; textLen = (int)sizeof(textRemote);
    }

    /* build JSON with escaped quotes for PUBRAW raw bytes */
    /* {"services":[{"service_id":"medicine_box_service","properties":{"box_num":N,"chufa_text":"..","chufa_time":"YYYY-MM-DD HH:MM:SS"}}]} */
    sprintf(payload,
            "{\"services\":[{\"service_id\":\"%s\",\"properties\":{\"box_num\":%u,"
            "\"chufa_text\":\"",
            HUAWEI_SERVICE_ID, (unsigned)box + 1);
    {
        int pos = (int)strlen(payload);
        int i;
        for (i = 0; i < textLen && (pos + 1) < (int)sizeof(payload); i++)
        {
            payload[pos++] = (char)text[i];
        }
        payload[pos] = 0;
    }
    {
        char tail[80];
        sprintf(tail, "\",\"chufa_time\":\"%04u-%02u-%02u %02u:%02u:%02u\"}}]}",
                (unsigned)t.year, (unsigned)t.month, (unsigned)t.day,
                (unsigned)t.hour, (unsigned)t.minute, (unsigned)t.second);
        strncat(payload, tail, sizeof(payload) - strlen(payload) - 1);
    }

    if (g_State == MQTT_STATE_READY && !ESP_CmdBusy())
    {
        if (ESP_PubRaw(g_TopicProp, (const uint8_t *)payload, (uint16_t)strlen(payload), 0u, NULL))
        {
            printf("[MQTT] report: %s\r\n", payload);
        }
        else
        {
            printf("[MQTT] report busy, drop: %s\r\n", payload);
        }
    }
    else if (g_State != MQTT_STATE_READY)
    {
        printf("[MQTT] not ready, record dropped: %s\r\n", payload);
    }
    else
    {
        printf("[MQTT] report busy, drop: %s\r\n", payload);
    }
}
