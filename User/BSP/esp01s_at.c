/**
  ******************************************************************************
  * @file    esp01s_at.c
  * @brief   ESP-01S AT command driver implementation
  ******************************************************************************
  * Non-blocking, polled in main loop via ESP_Process().
  * Supports normal command send (ESP_CmdSend) and raw-payload publish
  * (ESP_PubRaw, two-step AT+MQTTPUBRAW: send command, wait '>', send bytes).
  ******************************************************************************
  */

/* ============================================================================
 * 模块说明(本项目视角, 2024 年补注)
 * ----------------------------------------------------------------------------
 * 项目    : 智能药箱系统
 * 芯片    : STM32F103ZET6 (Cortex-M3, 72MHz), 纯裸机无 RTOS
 * 本文件  : esp01s_at.c —— ESP-01S 的 AT 指令驱动(整个上云链路的“物理层之上”)
 *           本次只增加中文注释, 代码一字未改。
 *
 * ============================== 一、它在链路中的位置 ==============================
 *   STM32(本文件) --USART2, PA2=TX / PA3=RX, 115200 8N1--> ESP-01S(乐鑫 esp-at 固件)
 *        ESP-01S --WiFi(TCP 1883)--> 华为云 IoTDA
 *   上层是 BSP\mqtt_huawei.c: 它负责“业务语义”(连 WiFi、连 MQTT、订阅、上报属性、
 *   接收云端命令), 本文件负责“把一句话发出去并认出对方的回答”。二者用两个回调
 *   加两个查询函数对接: ESP_CmdSend / ESP_CmdResult / ESP_CmdBusy / ESP_Process,
 *   以及 ESP_RegDataCb / ESP_RegNtpCb / ESP_RegDisconnectCb(MQTT_Init 里注册)。
 *
 * ============================== 二、为什么是全非阻塞的 ==============================
 *   本文件里没有一个“等回应”的 while 循环, 所有等待都靠状态机 + 轮询完成:
 *   上层发完一条指令立刻返回, 主循环每次调用 ESP_Process() 时:
 *     1) 把 USART2 环形缓冲里收到的字节逐个取出来(收数由 USART2 中断完成,
 *        见 usart.c / Usart2_RxIsr 与 stm32f10x_it.c);
 *     2) 按行组装(g_Line), 遇到 '\n' 就把整行交给 HandleLine() 分派;
 *     3) 检查两个状态机是否超时。
 *   好处: 等待 ESP 回应(通常几十毫秒~几秒)期间, 主循环仍然在跑按键、刷 LCD、
 *   走定时器, 界面不会“卡住”。这是本项目能同时做界面和联网的关键。
 *
 * ============================== 三、两个互斥的状态机 ==============================
 *   g_Cmd —— “命令态”: 发出一条 AT 命令, 记住期望看到的回应串(expect), 然后
 *            在 HandleLine() 里用 strstr 逐行匹配; 匹配上=成功(1)、出现
 *            ERROR/FAIL=失败(2)、超时=失败(2)。上层用 ESP_CmdResult() 查询。
 *   g_Pub —— “原始发布态”: AT+MQTTPUBRAW 的两步法(见下), 它是唯一会“先收提示
 *            符、再吐裸数据”的特例, 普通命令态表达不了, 所以单独一个状态机。
 *   两者互斥: ESP_CmdSend() 与 ESP_PubRaw() 在入口都检查
 *   “g_Cmd.active || g_Pub.active”, 只要有一个在跑就返回 0 拒绝新请求。
 *   ESP_CmdBusy() 把两者“或”起来给上层当背压(back-pressure)信号: mqtt_huawei.c
 *   必须先问它“忙不忙”, 不忙才敢发下一条命令, 否则命令会互相穿插、回应错配。
 *
 * ============================== 四、AT+MQTTPUBRAW 两步法(本文件最绕的地方) ==============================
 *   为什么不能用普通的 AT+MQTTPUB: 该 esp-at 固件在 MQTT 发布时不支持 JSON 里的
 *   引号转义, 报文里带 \" 会被直接拒绝并回 ERROR。所以必须用“原始(Raw)发布”,
 *   由我们自己在第二步把 JSON 原样吐出去。
 *   完整时序(以 qos=0 为例):
 *     步骤1  STM32 发:  AT+MQTTPUBRAW=0,"topic",len,qos,0\r\n
 *     步骤2  固件回:    OK\r\n>        <-- 注意: 提示符是 '>' 之后一个空格,
 *                                              '>' 后面并不保证有换行符
 *     步骤3  STM32 收:  '>' 出现即视为“可以发数据了”, 于是流式吐出 len 个原始
 *                       字节; 不转义、结尾不加 CRLF(长度已经由 len 声明, 固件
 *                       只认这 len 个字节)
 *     步骤4  固件回:    +MQTTPUB:OK   <-- 发布成功的标志(失败则回 ERROR 等)
 *   关键点/坑: 触发点必须是单独的 '>' 字节, 不能写成“匹配一整行 OK\r\n>”。
 *   因为 '>' 之后没有换行, “按行”的解析器永远等不到这一行的结束, 会一直死等。
 *   所以 ESP_Process() 在 g_Pub.waitPrompt 分支里先单独判断 b == '>', 与
 *   “按行组装”的逻辑并列存在, 互不干扰。
 *
 * ============================== 五、发布期间为什么还在解析行 ==============================
 *   等待 '>' 的那段时间里, 固件仍可能主动上报 URC(比如 +MQTTSUBRECV 云端下发的
 *   数据)。如果这期间“只盯着 '>'、不看别的”, 这些行就会被丢掉、数据就丢了。
 *   所以 g_Pub.waitPrompt 分支里, 除了判断 '>' 之外, 仍然继续按行累积, 遇到
 *   '\n' 就照常调用 HandleLine() —— 相当于“一边等提示符, 一边正常收信”。
 *   价格是这段逻辑要自己复制一份按行组装(见下面 g_Pub.waitPrompt 分支)。
 *
 * ============================== 六、缓冲区大小与来历 ==============================
 *   ESP_LINE_MAX 2048 —— 行缓冲 g_Line 的大小(定义在 esp01s_at.h)。
 *   ESP_DATA_MAX 2048 —— 下行数据缓冲 g_Data 与发布缓冲 g_PubBuf 的大小。
 *   历史沿革: 这两个值早期是 128, 后来 256, 最后改到 2048。原因是固件上报一条
 *   +MQTTSUBRECV 时“数据是内联在长度后面同一行”的, 一条云端下发的属性设置
 *   报文有 130 字节左右, 用 128/256 会把行截断, 表现为“收到半条 JSON、解析失败”。
 *   放大到 2048 后, 常见报文(最长约 1700 字符)都能整行装下。
 *
 * ============================== 七、URC 分发优先级(HandleLine) ==============================
 *   一行收完后的处理顺序(自上而下, 越靠前越先处理):
 *     1) 待处理命令的期望回应串(g_Cmd.expect) —— 有命令在等就先让命令判断成败;
 *     2) 发布完成串 "+MQTTPUB:OK" / ERROR 系列 —— 结束 g_Pub;
 *     3) URC 事件: +MQTTSUBRECV(下行数据) / +MQTTDISCONNECTED(断连) /
 *        +CIPSNTPTIME(NTP 时间)。
 *   注意顺序 1 有副作用: 如果一行同时包含期望串和 ERROR, 代码会先判成功、并把
 *   g_Cmd.active 清零, 于是后面的 ERROR 分支进不去(见本文件末尾“已发现的疑点”)。
 *
 * ============================== 八、回调注册 ==============================
 *   三个回调由 MQTT_Init() 注册(见 mqtt_huawei.c):
 *     ESP_RegDataCb       收到订阅消息 -> MQTT 模块解析 JSON 命令
 *     ESP_RegNtpCb        收到 NTP 时间 -> 校正 RTC
 *     ESP_RegDisconnectCb 收到断连上报 -> 上层进入重连流程
 *   回调可能为 NULL(未注册), 所有调用点都做了判空。
 * ========================================================================== */

#include "esp01s_at.h"
#include "usart.h"
#include "timer.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>


/* 命令状态结构体: 描述“一条正在等待回应的 AT 命令” */
typedef struct
{
    const char *expect;         /* 期望在回应里看到的子串(strstr 匹配); 只存指针不拷贝,
                                   所以调用方传进来的串必须是长寿命的(通常传字面量) */
    uint32_t    startTick;      /* 本条命令发出时的系统毫秒数(Timer_GetTick), 单位 ms;
                                   只用于算差值, 回绕安全 */
    uint32_t    timeoutMs;      /* 本条命令允许的等待时长, 单位 ms(由调用方按命令特性指定) */
    uint8_t     active;         /* 是否有一条命令正在等待: 0=空闲 1=等待中; 它同时也是一把
                                   “互斥锁”, 防止两条命令的回应互相串线 */
    uint8_t     result;         /* 结果码: 0=仍在等待(pending) 1=成功 2=失败/超时 */
} EspCmd_t;

/* 原始发布状态结构体(AT+MQTTPUBRAW 两步法专用) */
typedef struct
{
    uint8_t     active;         /* PUBRAW 流程是否在进行中: 0=空闲 1=进行中 */
    uint16_t    remain;         /* 本步要发送的载荷字节数。第二步(收到 '>' 之后)它就是
                                   实际要吐出的长度, 上限 ESP_DATA_MAX=2048;
                                   字段名容易误会成“剩余”, 其实它是“总长度”(见文末疑点 4) */
    uint8_t     *buf;           /* 载荷数据指针, 指向 g_PubBuf(内部拷贝, 不引用调用方的缓冲) */
    EspPubDoneCb_t done;        /* 完成回调: 成功传 1, 失败/超时传 0; 可为 NULL */
    uint32_t    startTick;      /* 本流程开始的系统毫秒数, 单位 ms; 用于算总超时 */
    uint32_t    timeoutMs;      /* 整个两步流程的总超时, 单位 ms(本文件固定 5000) */
    uint8_t     waitPrompt;     /* 是否还在等 '>' 提示符: 1=等提示符(载荷尚未发出)
                                   0=提示符已到、载荷已吐出(在等 +MQTTPUB:OK) */
} EspPub_t;

static EspCmd_t g_Cmd;
static uint8_t  g_Line[ESP_LINE_MAX];
static uint16_t g_LineLen = 0;

static uint8_t  g_Data[ESP_DATA_MAX];
static uint16_t g_DataLen = 0;
static uint16_t g_DataRemain = 0;
static char     g_DataTopic[200];
static uint16_t g_DataTopicLen = 0;

static EspDataCb_t      g_DataCb;
static EspNtpCb_t       g_NtpCb;
static EspDisconnectCb_t g_DisconnectCb;

static EspPub_t g_Pub;

/* Internal payload buffer for raw publish (kept static) */
static uint8_t g_PubBuf[ESP_DATA_MAX];

/* ==================== internal helpers ==================== */

/* Send a line to ESP-01S followed by CRLF */
/* --------------------------------------------------------------------------
 * 作用  : 向 ESP-01S 发送一条 AT 命令文本, 并自动补上结尾的 CRLF。
 * 参数  : s —— 要发送的 ASCII 字符串('\0' 结尾), 不含结尾的 "\r\n"。
 * 返回值: 无。
 * 原理  : AT 指令的“一条命令”是以 CRLF 为边界的, 固件只有看到 "\r\n" 才会开始
 *         解析这条命令。所以命令文本和结尾的 CRLF 分两次调 Usart2_SendString
 *         发出即可(Usart2_SendString 内部逐字节查询 TC 标志发送, 见 usart.c)。
 * 注意  : 它是“发完就返回”, 不等任何回应 —— 回应由 ESP_Process 的状态机异步收。
 * -------------------------------------------------------------------------- */
static void EspTxLine(const char *s)
{
    Usart2_SendString(s);
    Usart2_SendString("\r\n");
}

/* Parse "+MQTTSUBRECV:" header, enter data reception mode.
   Format: +MQTTSUBRECV:0,"topic",<length>,<data>  (data inline after len) */
/* --------------------------------------------------------------------------
 * 作用  : 解析固件主动上报的下行数据行 "+MQTTSUBRECV:...", 把 topic 与载荷
 *         取出来, 然后回调给上层(g_DataCb)。
 * 参数  : line —— 一整行文本(已去掉结尾的 CR/LF, 且以 '\0' 结尾)。
 * 返回值: 无。
 * 原理  : 报文实际形状是
 *             +MQTTSUBRECV:<linkid>,"<topic>",<len>,<data>
 *         —— 关键是 <data> 内联在同一行的 <len> 后面(不是另起一行), 所以本函数
 *         用“跳过冒号 -> 跳过 linkid 到第一个逗号 -> 截取 topic 到下一个逗号 ->
 *         atoi 取长度 -> 再跳一个逗号 -> 剩下的全部当数据”这一条链路解析。
 *         数据长度以固件给的 <len> 为准(不是以本行剩余字符数为准), 这样即使数据
 *         里含有逗号、引号、'{' '}' 也不会算错 —— JSON 报文里逗号遍地都是, 必须
 *         靠长度而不是靠分隔符来界定。
 *         解析成功后直接调用 g_DataCb(g_DataTopic, g_Data, g_DataLen) 把数据交给
 *         上层(通常是 mqtt_huawei.c 里解析华为云命令的处理函数)。
 *         同时把 g_DataRemain 置 0, 表示“这一条已经在同行内消化完了”。
 * 说明  : 本函数只处理“数据与长度同行内联”这一种形状 —— 这正是本固件的实际行为。
 *         ESP_Process() 里还有另一条“逐字节续传”的路径(g_DataRemain > 0 分支),
 *         当数据不在同行时才用得上; 就本固件而言, g_DataRemain 在本函数里一定会
 *         被清零, 所以那条路径实际不会触发(见文末疑点 3)。
 * 防御  : - 长度超过 ESP_DATA_MAX 时按 ESP_DATA_MAX 截断, 避免缓冲区溢出;
 *         - topic 最长只取 sizeof(g_DataTopic)-1 = 199 字节, 超出部分被丢弃;
 *         - 任何一步没找到预期的分隔逗号就 return, 不产生回调(宁可丢一条消息,
 *           也不能用不完整的数据去驱动业务)。
 * -------------------------------------------------------------------------- */
static void ParseMqttSubRecv(const char *line)
{
    const char *p = strchr(line, ':');

    if (!p) return;
    p++;
    while (*p == ' ') p++;

    /* skip linkid */
    while (*p && *p != ',') p++;
    if (*p != ',') return;
    p++;

    /* capture topic (between comma and next comma) */
    g_DataTopicLen = 0;
    while (*p && *p != ',' && g_DataTopicLen < (sizeof(g_DataTopic) - 1))
    {
        g_DataTopic[g_DataTopicLen++] = *p++;
    }
    g_DataTopic[g_DataTopicLen] = 0;
    if (*p != ',') return;
    p++;

    /* get length */
    g_DataRemain = (uint16_t)atoi(p);
    if (g_DataRemain > (ESP_DATA_MAX - 1))
    {
        g_DataRemain = (ESP_DATA_MAX - 1);
    }
    g_DataLen = 0;

    /* skip len digits, check if data follows inline: "...len,<data>" */
    while (*p && *p != ',') p++;
    if (*p == ',')
    {
        p++;
        /* copy inline data (bounded by g_DataRemain) */
        while (g_DataLen < g_DataRemain && *p)
        {
            g_Data[g_DataLen++] = (uint8_t)*p++;
        }
        g_DataRemain = 0;   /* consumed inline */
        if (g_DataCb) g_DataCb(g_DataTopic, (const char *)g_Data, g_DataLen);
    }
}

/* Finish a raw publish: notify callback and clear state */
/* --------------------------------------------------------------------------
 * 作用  : 结束一次原始发布流程, 清状态并通知完成回调。
 * 参数  : result —— 结果: 1=成功(收到 +MQTTPUB:OK), 0=失败(ERROR/FAIL/超时)。
 * 返回值: 无。
 * 原理  : 先把 g_Pub.active 清零再回调, 顺序很重要 —— 回调里上层很可能立刻发起
 *         下一条命令; 若此时 active 还等于 1, ESP_CmdSend/ESP_PubRaw 会以“忙”
 *         为由拒绝, 上层就得再等一轮。先把锁放开、再通知, 才能让上层在回调里
 *         顺畅地串起下一步(例如上报完属性马上再发一次发布)。
 *         先把 g_Pub.done 取到局部变量 cb 再清空字段, 也是同样的道理: 清空之后
 *         仍然能安全使用 cb。
 * -------------------------------------------------------------------------- */
static void PubFinish(uint8_t result)
{
    EspPubDoneCb_t cb = g_Pub.done;
    g_Pub.active = 0;
    g_Pub.waitPrompt = 0;
    g_Pub.buf = NULL;
    g_Pub.done = NULL;
    if (cb) cb(result);
}

/* Handle one complete line received from ESP-01S */
/* --------------------------------------------------------------------------
 * 作用  : 一行收完后(遇到 '\n')的总分派入口 —— 决定这一行是“命令的回应”、
 *         “发布完成的通知”, 还是固件主动上报的 URC。
 * 参数  : line —— 完整一行(已去掉 CR/LF, '\0' 结尾); len —— 该行长度(不含 '\0')。
 * 返回值: 无。
 * 原理  : 按下面的固定优先级依次判断(注意三个 elif 只用于 URC 分支, 上面两个 if
 *         是独立的, 可能在同一行里都执行到):
 *         1) 若 g_Cmd.active: 用 strstr(line, g_Cmd.expect) 找期望子串;
 *              找到 -> result=1(成功); 否则若出现 "ERROR"/"FAIL"/"ERR CODE"
 *              -> result=2(失败)。两种情况都会把 active 清零, 表示“这条命令
 *              已经有结论了”。用“包含”而不是“整行相等”, 是因为固件常常在
 *              OK 前后带别的字符(如 "\r\nOK\r\n"、"+MQTTSUB:OK")。
 *         2) 若 g_Pub.active: 收到 "+MQTTPUB:OK" 即发布成功 -> PubFinish(1);
 *              收到 ERROR/FAIL/ERR CODE -> PubFinish(0)。这一步解决的是
 *              “第二步把裸数据吐出去之后, 固件到底收没收下”的问题。
 *         3) URC(固件主动上报, 与请求无关):
 *              +MQTTSUBRECV      -> 云端下发的订阅消息, 交给 ParseMqttSubRecv
 *              +MQTTDISCONNECTED -> MQTT 断连(如被云端踢下线、网络中断),
 *                                   通知上层进入重连流程
 *              +CIPSNTPTIME      -> NTP 对时结果, 把冒号后的时间串原样交给上层
 *                                   (line + 12 跳过 "+CIPSNTPTIME" 这 12 个字符)
 *         判断 URC 用 strncmp 比前缀而不是 strstr 全文查找: URC 只会出现在行首,
 *         用前缀比较能避免“数据里恰好含 +MQTTSUBRECV 字样”被误判成 URC。
 *         短行保护: len == 0 直接返回(空行没有信息量)。
 * -------------------------------------------------------------------------- */
static void HandleLine(char *line, uint16_t len)
{
    if (len == 0)
    {
        return;
    }

    /* ---- 1. normal command response match ---- */
    if (g_Cmd.active)
    {
        if (g_Cmd.expect && strstr(line, g_Cmd.expect))
        {
            g_Cmd.result = 1;
            g_Cmd.active = 0;
        }
        else if (strstr(line, "ERROR") || strstr(line, "FAIL") || strstr(line, "ERR CODE"))
        {
            g_Cmd.result = 2;
            g_Cmd.active = 0;
        }
    }

    /* ---- 2. raw publish response match ---- */
    if (g_Pub.active)
    {
        if (strstr(line, "+MQTTPUB:OK"))
        {
            PubFinish(1);
        }
        else if (strstr(line, "ERROR") || strstr(line, "FAIL") || strstr(line, "ERR CODE"))
        {
            PubFinish(0);
        }
    }

    /* ---- 3. URC events ---- */
    if (strncmp(line, "+MQTTSUBRECV", 12) == 0)
    {
        ParseMqttSubRecv(line);
    }
    else if (strncmp(line, "+MQTTDISCONNECTED", 17) == 0)
    {
        if (g_DisconnectCb) g_DisconnectCb();
    }
    else if (strncmp(line, "+CIPSNTPTIME", 12) == 0)
    {
        if (g_NtpCb) g_NtpCb(line + 12);
    }
}

/* ==================== public interface ==================== */

/* Init AT driver */
/* --------------------------------------------------------------------------
 * 作用  : AT 驱动初始化 —— 把所有状态机、缓冲下标、回调指针复位到“干净”状态。
 * 参数  : 无。
 * 返回值: 无。
 * 原理  : 必须在 USART_InitAll() 之后调用(否则串口还没配好, 清缓冲没有意义;
 *         而且本函数会调用 Usart2_RxClear 操作串口环形缓冲)。
 *         清掉的东西及原因:
 *           g_Cmd.active/result —— 防止残留的“待回应命令”让第一条真命令被拒;
 *           g_LineLen / g_DataLen / g_DataRemain / g_DataTopicLen —— 丢弃半截
 *             的残行/残数据(上电瞬间 ESP 可能正在吐上电日志);
 *           g_DataCb/g_NtpCb/g_DisconnectCb —— 置空, 强制上层重新注册, 避免
 *             用到野指针;
 *           memset(&g_Pub, 0, sizeof) —— 一次性把发布状态机整个清零(含 done 回调),
 *             比逐字段赋值更不容易漏;
 *           Usart2_RxClear —— 把 USART2 接收环形缓冲的头尾指针复位, 丢掉上电
 *             噪声/ESP 启动信息。
 * 注意  : 这里不发送任何 AT 指令, 也不复位 ESP 硬件(硬件复位靠 ESP 自己的
 *         上电时序); 后续的连接流程由 mqtt_huawei.c 的命令序列驱动。
 * -------------------------------------------------------------------------- */
void ESP_Init(void)
{
    g_Cmd.active = 0;
    g_Cmd.result = 0;
    g_LineLen = 0;
    g_DataRemain = 0;
    g_DataLen = 0;
    g_DataTopicLen = 0;
    g_DataCb = NULL;
    g_NtpCb = NULL;
    g_DisconnectCb = NULL;
    memset(&g_Pub, 0, sizeof(g_Pub));
    Usart2_RxClear();
}

/* Send AT command and register expected response */
/* --------------------------------------------------------------------------
 * 作用  : 非阻塞地发出一条 AT 命令, 并登记“期望看到什么回应”。
 * 参数  : cmd       —— 命令文本(不含结尾 CRLF, 本函数会补), 如 "AT+MQTTCONN?"。
 *         expect    —— 期望在回应行里出现的子串, 如 "OK"、"+MQTTSUB:OK"。
 *                      只保存指针不拷贝, 所以必须是长寿命字符串(通常直接传字面量)。
 *         timeoutMs —— 等待回应的超时时间, 单位 ms。
 * 返回值: 1 = 命令已受理(已发出, 正在等回应);
 *         0 = 被拒绝(当前有命令或发布在进行中)。
 * 原理  : 这是“命令态”状态机的唯一入口, 做三件事:
 *           1) 互斥检查 —— g_Cmd.active 或 g_Pub.active 只要有一个非 0 就返回 0。
 *              因为回应的匹配是“全局的”: 若允许并发两条命令, 第一条的 OK 会被
 *              第二条抢走, 两条都错。宁可让上层稍后重试。
 *           2) 真正把命令发出去(EspTxLine 补 CRLF)。
 *           3) 登记状态: expect/startTick/timeoutMs 三个字段 + active=1、result=0。
 *              注意必须“先发命令、后置 active”: 反过来(先置 active 再发)在单线程
 *              轮询下没问题, 但稳妥的写法是让 active 与“命令已在线上”保持一致。
 *         之后本函数立即返回, 成败由 ESP_Process() 在后续轮询里判定:
 *         匹配到 expect -> result=1; 出现 ERROR/FAIL -> result=2;
 *         超过 timeoutMs -> result=2(超时也算失败, 由 ESP_Process 判)。
 *         上层用法: ESP_CmdSend(...) 返回 1 后, 轮询 ESP_CmdResult(),
 *         得到 1/2 再做下一步。
 * 注意  : 超时时间要按命令特性给: 纯本地命令(AT、AT+CWMODE)给几百毫秒即可;
 *         联网类命令(AT+CWJAP 连 WiFi、AT+MQTTCONN 连云)要给到 5~20 秒,
 *         否则会在真正连上之前就判超时。
 * -------------------------------------------------------------------------- */
uint8_t ESP_CmdSend(const char *cmd, const char *expect, uint32_t timeoutMs)
{
    if (g_Cmd.active || g_Pub.active)
    {
        return 0;
    }
    EspTxLine(cmd);
    g_Cmd.expect    = expect;
    g_Cmd.startTick = Timer_GetTick();
    g_Cmd.timeoutMs = timeoutMs;
    g_Cmd.active    = 1;
    g_Cmd.result    = 0;
    return 1;
}

/* Publish raw payload via AT+MQTTPUBRAW:
   step1 send "AT+MQTTPUBRAW=0,\"topic\",len,qos,0"
   step2 wait for '>' prompt, then send raw payload bytes (no CRLF) */
/* --------------------------------------------------------------------------
 * 作用  : 用 AT+MQTTPUBRAW 的“两步法”发布一段原始载荷(本项目里就是 JSON 属性
 *         报文), 载荷按字节原样发出, 不做任何转义。
 * 参数  : topic —— MQTT 主题(如华为云属性上报主题), 不能为 NULL;
 *         data  —— 载荷首地址, 不能为 NULL;
 *         len   —— 载荷字节数, 取值 1~ESP_DATA_MAX(2048);
 *         qos   —— 服务质量等级 0 或 1(本项目实际用 0, 即“最多一次”);
 *         done  —— 完成回调, 收到 +MQTTPUB:OK 时以 1 调用, 失败/超时以 0 调用;
 *                  可以为 NULL(不需要通知时)。
 * 返回值: 1 = 已受理(命令已发出, 正在等 '>' 提示符);
 *         0 = 被拒绝(忙、参数非法、或长度超限)。
 * 原理  : 两步法(详细时序见文件头“四”):
 *           第一步(本函数): 组命令 AT+MQTTPUBRAW=0,"topic",len,qos,0 并发出去,
 *             同时把状态机 g_Pub 置为“等提示符”(waitPrompt=1);
 *           第二步(ESP_Process): 等到 '>' 之后, 把 g_PubBuf 里的 len 个字节
 *             原样吐出去(不转义、不加 CRLF), 然后等 +MQTTPUB:OK。
 *         为什么必须把载荷先 memcpy 到 g_PubBuf:
 *           本函数返回后调用方的缓冲可能马上被改写(比如上层复用了报文缓冲区),
 *           而真正发送要等到若干轮询之后(等 '>' 到达)。做一份内部拷贝, 才能保证
 *           那时吐出去的内容和此刻提交的一致。代价是 2048 字节的静态 RAM 和一次
 *           最多 2KB 的拷贝(72MHz 下约十几微秒, 可接受)。
 *         为什么固定 5000ms 超时:
 *           它覆盖“第一步命令没被受理、'<' 等不到 '>'、第二步数据发出后等价回应”
 *           这几种可能的卡死。5 秒对一条几十~几百字节的属性上报足够宽裕, 又不至于
 *           让上层重连逻辑等太久。
 *         校验顺序: 先查忙(与命令态互斥), 再查参数(len 范围、两个指针非空),
 *           任何一项不满足都返回 0 而不改变状态机 —— 保证“返回 1 就一定已经接管
 *           了状态机”, 上层不必区分“半途失败”。
 * 风险  : cmd[] 只有 180 字节, 命令行 = 22(固定部分) + strlen(topic) + 数字位;
 *         topic 很长(超过约 130 字节)时会写越界。本工程用的华为云主题都远低于
 *         这个长度, 所以没暴露出来(见文末疑点 1)。
 * -------------------------------------------------------------------------- */
uint8_t ESP_PubRaw(const char *topic, const uint8_t *data, uint16_t len,
                   uint8_t qos, EspPubDoneCb_t done)
{
    char cmd[180];

    if (g_Cmd.active || g_Pub.active)
    {
        return 0;
    }
    if (len > ESP_DATA_MAX || len == 0 || topic == NULL || data == NULL)
    {
        return 0;
    }

    /* save payload copy for later transmission */
    memcpy(g_PubBuf, data, len);

    g_Pub.remain    = len;
    g_Pub.buf       = g_PubBuf;
    g_Pub.done      = done;
    g_Pub.active    = 1;
    g_Pub.waitPrompt = 1;
    g_Pub.startTick = Timer_GetTick();
    g_Pub.timeoutMs = 5000u;

    /* send the PUBRAW command (topic length bounded by format string) */
    sprintf(cmd, "AT+MQTTPUBRAW=0,\"%s\",%u,%u,0", topic, (unsigned)len, (unsigned)qos);
    EspTxLine(cmd);
    return 1;
}

/* Whether a command is currently pending */
/* --------------------------------------------------------------------------
 * 作用  : 查询驱动是否“忙”(有命令在等回应, 或有发布流程在进行)。
 * 参数  : 无。
 * 返回值: 1 = 忙(此时调 ESP_CmdSend/ESP_PubRaw 一定会被拒); 0 = 空闲。
 * 原理  : 把两个状态机的 active 位“或”起来, 作为给上层的背压信号。
 *         上层(mqtt_huawei.c)的主循环每次都先问它, 只有返回 0 才继续下一步,
 *         这样一个状态机跑完之前不会塞进第二条命令, 从根上避免回应错配。
 *         这是“非阻塞驱动 + 轮询”框架里最常用的握手方式。
 * -------------------------------------------------------------------------- */
uint8_t ESP_CmdBusy(void)
{
    return (g_Cmd.active || g_Pub.active);
}

/* Command result: 0=pending 1=success 2=fail/timeout */
/* --------------------------------------------------------------------------
 * 作用  : 查询最近一条命令的结果码。
 * 参数  : 无。
 * 返回值: 0 = 还在等待(pending); 1 = 成功(匹配到期望串); 2 = 失败(ERROR/FAIL 或超时)。
 * 原理  : 只读 g_Cmd.result。它由三处写入: ESP_CmdSend 置 0、
 *         HandleLine 匹配成功置 1 / 出错置 2、ESP_Process 超时置 2。
 *         除非又发了新命令(会把 result 重置为 0), 否则读到的就是那条命令的结论。
 * -------------------------------------------------------------------------- */
uint8_t ESP_CmdResult(void)
{
    return g_Cmd.result;
}

/* Poll loop: parse RX data and drive state machines */
/* --------------------------------------------------------------------------
 * 作用  : 驱动的“心跳” —— 主循环必须反复调用它。它做三件事:
 *           1) 把 USART2 环形缓冲里的字节全部取出来并解析(组装成行 -> HandleLine),
 *              解析过程中顺带处理“下行数据续传”和“等 '>' 提示符”两个特例;
 *           2) 检查命令态是否超时;
 *           3) 检查发布态是否超时。
 * 参数  : 无。
 * 返回值: 无。
 * 原理  : 三段式, 逐段说明。
 *
 *         【A. 收数循环】while (Usart2_RxCount() > 0) 逐字节取。
 *             收数本身是在中断里做的(每来一个字节就进一次 USART2_IRQHandler,
 *             写进 512 字节环形缓冲); 这里只是“消费”。这样中断极短, 解析这种
 *             重活全在主循环做, 界面不会被拖慢。
 *             循环条件每次重新查询 Count, 所以一次调用就能把攒下的数据全部吃完。
 *
 *         【B. 三种字节去向】取出一个字节 b 后, 按优先级判断:
 *             (1) 若 g_DataRemain > 0 —— 说明正处在“接收下行数据的续传模式”:
 *                 每来一个字节就存进 g_Data, 计数递减; 减到 0 表示这一条数据
 *                 收齐了, 立刻回调 g_DataCb 交给上层, 然后 continue 取下一个字节。
 *                 注意: 数据里含 '\r'/'\n'/'>' 也不会被当成控制字符 —— 在这条
 *                 路径上它们只是普通数据, 这正是“按长度收”的好处。
 *             (2) 若 g_Pub.active && g_Pub.waitPrompt —— 正等 PUBRAW 的 '>' 提示符:
 *                 - b == '>' : 提示符到了! 把 waitPrompt 清零, 然后 for 循环把
 *                   g_Pub.remain 个字节逐个 Usart2_SendByte 吐出去。
 *                   不转义、结尾不加 CRLF(长度已声明)。因为 Usart2_SendByte 是
 *                   “等 TC 标志”的阻塞发送, 这两步之间不会有别的事情插入,
 *                   不会把命令和数据搅在一起。
 *                 - 顺带还做了“按行累积 + 遇 '\n' 调 HandleLine”, 目的就是文件头
 *                   第五节说的: 等提示符期间照常收 URC, 别丢云端下发的数据。
 *                   其中 ERROR/FAIL 那几行会先 printf 出来便于现场排错, 然后
 *                   HandleLine 内部会走“发布态”分支把 g_Pub 收尾(失败)。
 *                 - 注意这里 '>' 不会被写进 g_Line(它是提示符不是正文), 而
 *                   '\r' 被忽略、其余字符累积 —— 与 (3) 的按行规则一致。
 *             (3) 其余情况就是普通“按行分帧”: '\n' 结束一行(末尾补 '\0' 后调
 *                 HandleLine, 再把 g_LineLen 清零), '\r' 直接忽略, 其它字符
 *                 累积进 g_Line。累积时用 g_LineLen < (ESP_LINE_MAX - 1) 限位,
 *                 保证总能留一个字节放 '\0'。超出部分被静默丢弃(而不是阻塞或
 *                 复位), 宁可截断一行也不能让主循环卡住。
 *
 *         【C. 两个超时检查】都在收数循环之后做, 所以“数据刚收完就超时”这种
 *             边界情况会先被解析处理, 不会误判。
 *             判断形式统一为 (int32_t)(Timer_GetTick() - startTick) >= (int32_t)timeoutMs,
 *             为什么要用“带符号差值”: Timer_GetTick() 是 32 位无符号毫秒计数,
 *             约 49.7 天回绕一次。写成 now >= start + timeout 在回绕时会算错
 *             (now 突然变小, 于是永远等不到); 而“无符号相减再转有符号比较”保证
 *             在间隔小于 2^31 ms(约 24.8 天)时结果永远正确 —— 本项目所有超时都
 *             是秒级, 所以回绕完全无感。
 *             命令超时: result=2(失败)、active=0(放锁), 由上层自行重试。
 *             发布超时: 先 printf 打出 waitPrompt/remain 便于定位“是没等到 '>',
 *             还是数据发完没等到 +MQTTPUB:OK”, 再 PubFinish(0) 通知上层失败。
 * 注意  : 本函数必须在主循环里被高频调用(建议每轮都调), 调用间隔决定了:
 *         - 能容忍多大的数据突发(环形缓冲 512 字节); 
 *         - 状态机的判定延迟(超时最坏会晚一个调用周期被发现)。
 * -------------------------------------------------------------------------- */
void ESP_Process(void)
{
    /* ---- drain RX buffer ---- */
    while (Usart2_RxCount() > 0)
    {
        uint8_t b = Usart2_RxRead();

        /* MQTT received-data mode */
        if (g_DataRemain > 0)
        {
            /* 边界用 ESP_DATA_MAX - 1: g_Data 恰好 ESP_DATA_MAX 字节,
               合法下标 0..ESP_DATA_MAX-1; 留出最后一位的余量,
               避免出现 g_DataLen == ESP_DATA_MAX 这种"长度等于数组尺寸"的状态。
               (与分析行缓冲 g_Line 用 (ESP_LINE_MAX - 1) 限位保持一致) */
            if (g_DataLen < (ESP_DATA_MAX - 1))
            {
                g_Data[g_DataLen++] = b;
            }
            g_DataRemain--;
            if (g_DataRemain == 0)
            {
                if (g_DataCb) g_DataCb(g_DataTopic, (const char *)g_Data, g_DataLen);
            }
            continue;
        }

        /* raw-publish prompt handling: wait for '>' then send payload.
           The AT firmware emits "OK\r\n> " (a '>' with trailing space, no LF
           necessarily before data). So trigger on the '>' byte itself. */
        if (g_Pub.active && g_Pub.waitPrompt)
        {
            if (b == '>')
            {
                uint16_t i;
                g_Pub.waitPrompt = 0;
                /* send payload now, no trailing CRLF */
                for (i = 0; i < g_Pub.remain; i++)
                {
                    Usart2_SendByte(g_Pub.buf[i]);
                }
            }
            /* accumulate a line; on LF run normal line handling too,
               so URCs (e.g. +MQTTSUBRECV) arriving during PUBRAW are
               not lost. HandleLine also finishes PUBRAW on ERROR. */
            if (b == '\n')
            {
                g_Line[g_LineLen] = 0;
                if (g_Pub.active &&
                    (strstr((char *)g_Line, "ERROR") || strstr((char *)g_Line, "FAIL")))
                {
                    printf("[PUB] cmd error: %s\r\n", (char *)g_Line);
                }
                HandleLine((char *)g_Line, g_LineLen);
                g_LineLen = 0;
            }
            else if (b != '\r')
            {
                if (g_LineLen < (ESP_LINE_MAX - 1))
                {
                    g_Line[g_LineLen++] = b;
                }
            }
            continue;
        }

        /* line framing: '\r' ignored, '\n' completes a line */
        if (b == '\n')
        {
            g_Line[g_LineLen] = 0;
            HandleLine((char *)g_Line, g_LineLen);
            g_LineLen = 0;
        }
        else if (b == '\r')
        {
            /* ignore */
        }
        else
        {
            if (g_LineLen < (ESP_LINE_MAX - 1))
            {
                g_Line[g_LineLen++] = b;
            }
        }
    }

    /* ---- command timeout check ---- */
    if (g_Cmd.active)
    {
        if ((int32_t)(Timer_GetTick() - g_Cmd.startTick) >= (int32_t)g_Cmd.timeoutMs)
        {
            g_Cmd.result = 2;
            g_Cmd.active = 0;
        }
    }

    /* ---- raw publish timeout check ---- */
    if (g_Pub.active)
    {
        if ((int32_t)(Timer_GetTick() - g_Pub.startTick) >= (int32_t)g_Pub.timeoutMs)
        {
            printf("[PUB] timeout! waitPrompt=%u remain=%u\r\n",
                   (unsigned)g_Pub.waitPrompt, (unsigned)g_Pub.remain);
            PubFinish(0);
        }
    }
}

/* Register MQTT data callback */
/* --------------------------------------------------------------------------
 * 作用  : 注册“收到下行数据”的回调。
 * 参数  : cb —— 回调函数指针, 签名 void (*)(const char *topic, const char *data, uint16_t len);
 *               传 NULL 表示注销。
 * 返回值: 无。
 * 原理  : 只是把指针存进静态变量 g_DataCb。之后每收齐一条 +MQTTSUBRECV 数据,
 *         驱动就调用它一次(ParseMqttSubRecv 或 ESP_Process 的续传分支里调用)。
 *         谁注册谁负责实现: 本工程由 MQTT_Init() 注册到 mqtt_huawei.c 的处理函数,
 *         那里再解析华为云下发的 JSON 命令(如远程开药盒)。
 * 注意  : 回调是在 ESP_Process() 的上下文(即主循环)里被调用的, 不是中断里,
 *         所以回调内部可以安全地做较重的处理, 但不要阻塞太久 —— 阻塞期间收不到
 *         新的串口数据(有 512 字节环形缓冲兜着, 短暂处理没问题)。
 * -------------------------------------------------------------------------- */
void ESP_RegDataCb(EspDataCb_t cb)
{
    g_DataCb = cb;
}

/* Register NTP time callback */
/* --------------------------------------------------------------------------
 * 作用  : 注册“收到 NTP 对时结果”的回调。
 * 参数  : cb —— 回调函数指针, 签名 void (*)(const char *timeStr);
 *               参数是 +CIPSNTPTIME 后面的时间串原文, 传 NULL 表示注销。
 * 返回值: 无。
 * 原理  : 存进 g_NtpCb。HandleLine() 识别到行首是 "+CIPSNTPTIME" 时,
 *         用 line + 12 跳过这 12 个字符, 把剩余的时间串(形如
 *         "Mon Nov 25 10:20:30 2024" 或数字时间戳, 取决于固件配置)原样交给回调。
 *         本工程用它给 RTC 校准走时(RTC 本身没有联网能力)。
 * -------------------------------------------------------------------------- */
void ESP_RegNtpCb(EspNtpCb_t cb)
{
    g_NtpCb = cb;
}

/* Register disconnect callback */
/* --------------------------------------------------------------------------
 * 作用  : 注册“MQTT 断连”的回调。
 * 参数  : cb —— 回调函数指针, 签名 void (*)(void), 无参数; 传 NULL 表示注销。
 * 返回值: 无。
 * 原理  : 存进 g_DisconnectCb。固件检测到与云端的 MQTT 连接断开时会主动上报
 *         "+MQTTDISCONNECTED", HandleLine() 用前缀匹配识别到它, 就调用本回调,
 *         让上层立刻把链路状态降级并进入重连流程(mqtt_huawei.c 里有
 *         RETRY_INTERVAL_MS = 5000ms 的重试间隔)。
 *         这在“心跳超时被云端踢下线”这类场景里非常关键: 不处理的话上层还以为
 *         连着, 会一直往一个已经断掉的连接上报数据。
 * -------------------------------------------------------------------------- */
void ESP_RegDisconnectCb(EspDisconnectCb_t cb)
{
    g_DisconnectCb = cb;
}

/* ============================================================================
 * 代码审阅记录(本次只加注释, 一行代码都没有改动; 以下仅为报告, 未做修改)
 * ----------------------------------------------------------------------------
 * 疑点 1(潜在缓冲区溢出, 建议修复): ESP_PubRaw 里的 char cmd[180] 用 sprintf
 *   拼命令, 不检查长度。命令行固定部分("AT+MQTTPUBRAW=0,\"\",len,qos,0" 加 CRLF)
 *   约占 30 字节, 再算上 len/qos 的数字, 因此 topic 超过约 140 字节就会写越界,
 *   破坏栈上的相邻数据。本工程用的华为云主题都远短于此, 所以没有暴露出来。
 *   建议: 改成 snprintf 并判断返回值, 或显式 if (strlen(topic) > 130) return 0;
 *
 * 疑点 2(静默截断 topic): ParseMqttSubRecv 里 g_DataTopic 只有 200 字节, topic
 *   超过 199 字节的部分被丢弃, 但函数仍会照常回调, 上层拿到的是被截断的主题 ——
 *   基于主题字符串做匹配的话会莫名其妙匹配失败。建议加一个“超长则丢弃整条消息”
 *   的处理。
 *
 * 疑点 3(两条数据路径实际只走一条): 文件头第六节说固件把数据内联在长度后面同一行,
 *   ParseMqttSubRecv 也确实是按内联处理的(取完内联数据就把 g_DataRemain 清 0)。
 *   于是 ESP_Process 里 g_DataRemain > 0 的“逐字节续传”分支在本固件上永远不会触发。
 *   如果哪天真换成“数据另起一行”的固件版本, 这行代码也会失效: ParseMqttSubRecv 里
 *   “while (*p && *p != ',') p++; if (*p == ',')”要求长度后面必须紧跟逗号, 而那种
 *   固件给的是 "\r\n", 条件不成立 -> 不进内联分支 -> g_DataRemain 被留成正数 ->
 *   下一条命令的回应字节会被当成数据吃掉, 症状是“网络功能随机失灵”。换固件时
 *   必须重测这里。
 *
 * 疑点 4(字段名有歧义): EspPub_t.remain 的语义其实是“本次要发送的载荷总长度”,
 *   不是“剩余待发长度”。第二步是一次 for 循环把全部数据发完, 过程中并不递减它。
 *   名字容易让后来的维护者误以为可以在发送中途被打断续传。
 *
 * 疑点 5(URC 与命令回应相互干扰): HandleLine 的分派顺序是“先判命令成败, 再判
 *   URC, 最后才解析 URC 内容”, 且三者用的是同一行的文本。若某条 URC 里恰好包含
 *   等待中的 expect 子串(例如等 "OK", 而云端下发的数据里含 "OK"), 这条命令会被
 *   误判为成功。当前业务里下发的 JSON 不包含 "OK"/"ERROR" 之类字样, 所以没出问题;
 *   彻底的做法是利用 +MQTTSUBRECV 自带长度字段做字节级帧同步, 而不是按行猜。
 *
 * 疑点 6(一行同时含 expect 与 ERROR 时判成功): 因为分支是 if/else if, expect
 *   先命中就会把 active 清零, 后面的 ERROR 分支进不去了。可能出现“命令实际失败
 *   却上报成功”。出现这种行需要固件把两种标记混在同一行, 概率低但存在。
 *
 * 非缺陷的说明(容易误读, 提前澄清):
 *   - g_Cmd.expect 只存指针不拷贝: 调用方必须传长寿命字符串(本项目全部传字面量),
 *     传局部数组会导致悬空指针。
 *   - 行缓冲溢出后静默丢字节而不是复位: 是有意为之 —— 主循环不能被串口噪声卡死,
 *     丢一行最多导致一条命令超时重试。
 *   - 下行数据回调可能被“提前”触发: 如果上一行的内联数据没走完就来了新行,
 *     会出现 g_DataRemain 清零后又立即被 ESP_Process 回调一次的情形; 上层回调
 *     若假定“一次消息一次回调”需要留意(本项目回调只解析 JSON, 重复调用无害)。
 * ========================================================================== */

/* P4MARK-ESP-END */
