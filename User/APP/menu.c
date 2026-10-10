/**
  ******************************************************************************
  * @file    menu.c
  * @brief   分级菜单与全部界面绘制实现(人机交互层)
  ******************************************************************************
  * 硬件平台: STM32F103ZET6 + ILI9488 320x480 竖屏(FSMC 并口驱动, 见 lcd.c)
  * 运行方式: 纯裸机, 无 RTOS。1ms SysTick 提供时基(Timer_GetTick),
  *           主循环轮流调用 Menu_Tick()(周期刷新)与 Menu_OnKey()(按键事件),
  *           本模块内部不使用任何阻塞延时, 所有"等待"都靠时间戳比较实现。
  * 上下游:   按键事件由 key.c 扫描 PF0~PF4 后打包成位掩码交给 Menu_OnKey();
  *           时间来自 rtc.c; 定时规则存放于 g_Cfg(flash.h), 改完写回 Flash;
  *           连接状态取自 mqtt_huawei.c; 蜂鸣剩余秒数取自 beep.c。
  ******************************************************************************
  * 【一、四级界面状态机】
  *   全部界面由一个变量 g_Scr(ScrId_t) 表示, 任何跳转都必须走 EnterScreen(),
  *   由它统一"改状态 + 清 Toast + 重画该界面整屏", 避免出现半张新半张旧的画面。
  *
  *        ┌──────────────────────── 返回(逐级回退) ────────────────────────┐
  *        v                                                              │
  *   SCR_MAIN ──[设置]──> SCR_BOX_SEL ──[确认]──> SCR_DAY_SEL ──[确认]──> SCR_TIME_SET
  *   主界面            药盒选择(7 项)          星期选择(7 项)          时间设置(4 槽)
  *     ^                                                              │
  *     └──────────────────── 药盒选择界面按[返回]回到主界面 ────────────┘
  *
  *   - SCR_MAIN     : 只读信息页。标题栏、WiFi/云连接状态、日期、大时钟、
  *                    蜂鸣倒计时/系统提示、7 个药盒状态列表、底部操作提示。
  *                    本界面唯一的按键动作是[设置] -> 进入药盒选择。
  *   - SCR_BOX_SEL  : 列表选 1~7 号药盒, 每行右侧显示该盒"距现在最近的一次定时"。
  *   - SCR_DAY_SEL  : 列表选周一~周日, 每行右侧显示该盒该天"最早的一个时间点"。
  *                    标题栏右侧额外用小字标出当前药盒号, 防止用户忘了在改哪一盒。
  *   - SCR_TIME_SET : 4 个时间槽的三段式编辑(选槽 -> 调小时 -> 调分钟)。
  *
  *   列表光标位置的回退记忆: 从深层界面返回上层时先置 g_KeepItemSel = 1 并把
  *   g_ItemSel 预置成"刚进入过的那个父项", 上层绘制的第一件事就是读这个标志,
  *   于是光标停在用户上次离开的位置, 而不是跳回第 0 项。标志用一次即清零,
  *   否则用户主动从主界面重新进入时也会被"锁"在旧位置。
  ******************************************************************************
  * 【二、主界面的按需重绘(本模块最关键的优化)】
  *   主界面每秒都要更新时钟, 但绝不能每秒 LCD_Clear 一次整屏。原因是这块屏走
  *   FSMC 并口, 刷一次 320x480 要写 15 万多个像素点, 期间 CPU 几乎全被总线占满;
  *   而 1ms SysTick 中断里的调度(定时到点开盒、蜂鸣计数)全部依赖 CPU 及时响应,
  *   整屏重画会把它们推迟, 表现为"到点了却慢半拍"; 同时人眼能看出明显的闪白。
  *
  *   因此采用"脏标志 + 局部重绘": 每个可能变化的信息都配一个影子变量记录
  *   "上一次画到屏上的值", 每次 Menu_Tick() 把当前真实值和影子比对, 相等就
  *   一个像素都不写, 不等才只重画那一小块矩形并更新影子。影子变量的初值都取
  *   一个正常情况不可能出现的值(如 0xFF), 保证上电第一次比较必然"不等",
  *   从而把该区域画出来, 不需要额外的"首次强制刷新"分支。
  *
  *     g_LastSec      : 秒值, 每秒才重画时钟 + 蜂鸣倒计时(1Hz 的信息)
  *     g_LastDateSig  : 年月日压缩成的签名, 只在跨零点时重画日期行(1 次/天)
  *     g_LastLink     : MQTT/云连接状态枚举, 只在状态跳变时重画状态栏(事件驱动)
  *     g_BoxDirty     : 药盒开合状态的脏标志, 由 app.c 开盒时调
  *                      Menu_NotifyBoxChange() 置位, 属于"被动通知"而非轮询比对
  *
  *   收益: 稳态下主循环每秒只写约 2 块小矩形, FSMC 带宽占用下降两个数量级,
  *   画面无闪烁, 且 SysTick 调度抖动极小。代价是每块区域必须严格知道自己的
  *   屏幕坐标范围, 画的时候要连同背景一起填充(局部重画没有"整屏底色"可依靠),
  *   所以本模块所有绘制函数都成对出现"先 LCD_FillRect 填底 -> 再写文字"。
  ******************************************************************************
  * 【三、时间槽的特殊值约定】
  *   g_Cfg.rules[盒].hour[天][槽] == 0xFF 表示"该槽未设置"。绝不能用 0 当空值,
  *   因为 0 点是合法时刻 —— 用 0 当空会出现"空槽显示成 00:00 并被当成凌晨提醒"。
  *   本文件里所有遍历时间槽的地方都必须先判断 0xFF 再取 hour/min, 详见 flash.h。
  ******************************************************************************
  */
#include "menu.h"
#include "lcd.h"
#include "key.h"
#include "rtc.h"
#include "flash.h"
#include "beep.h"
#include "mqtt_huawei.h"
#include "timer.h"
#include <stdio.h>
#include <string.h>


/* ==================== GBK UI strings ==================== */
#define STR_TITLE     "\xD6\xC7\xC4\xDC\xD2\xA9\xCF\xE4\xCF\xB5\xCD\xB3"   /* 智能药箱系统：主界面顶部 40 像素高标题栏的正中大字(24 号字体)，只在 Main_DrawTitle() 里用到 */
#define STR_BOX       "\xD2\xA9\xBA\xD0"                                    /* 药盒：作为名词前缀反复复用 —— 主界面各行 "药盒1"~"药盒7"、药盒选择列表项文本、星期/时间设置页标题栏右侧的 "<药盒>N 星期X" 上下文提示 */
#define STR_OPENED    "\xD2\xD1\xB4\xF2\xBF\xAA"                            /* 已打开：主界面药盒行最右侧的状态文字，表示该盒已到点并弹出；同时用绿色 GREEN 显示，左侧方形状态灯也同步变绿 */
#define STR_PENDING   "\xB4\xFD\xB4\xF2\xBF\xAA"                            /* 待打开：主界面药盒行最右侧的状态文字，表示该盒尚未触发；用深灰 DGRAY 显示，与"已打开"的绿色形成对比，状态灯保持浅灰 LGRAY */
#define STR_WIFI      "WiFi"
#define STR_CLOUD     "\xD4\xC6"                                            /* 云：主界面连接状态栏里 "WiFi:" 后面紧跟的那个字，用来引出云平台(MQTT)的连接状态文字 */
#define STR_CONN_OK   "\xD2\xD1\xC1\xAC\xBD\xD3"                            /* 已连接：连接状态栏显示，对应 LNK_READY(两段都绿)或 LNK_MQTT(WiFi 段绿)，告诉用户链路正常 */
#define STR_CONN_ING  "\xC1\xAC\xBD\xD3\xD6\xD0"                            /* 连接中：连接状态栏显示，对应 LNK_MQTT 的云段(橙)或 LNK_WIFI 的 WiFi 段(橙)，表示正在握手/重连 */
#define STR_CONN_NO   "\xCE\xB4\xC1\xAC\xBD\xD3"                            /* 未连接：连接状态栏显示，用红色 RED 报警，表示 WiFi 未接入(云段也为红)或模块完全离线 */
#define STR_RUNNING   "\xCF\xB5\xCD\xB3\xD4\xCB\xD0\xD0\xD6\xD0"            /* 系统运行中：主界面蜂鸣提示行的"空闲"文案 —— 既没到点蜂鸣、也没有临时消息(g_MainMsg)可显示时，就显示这行深蓝字，表示一切正常待命 */
#define STR_BEEPING   "\xB7\xE4\xC3\xF9\xD6\xD0\x20\xCA\xA3\xD3\xE0"        /* 蜂鸣中 剩余：与 STR_SEC 拼接成 "蜂鸣中 剩余N秒"，在开盒提醒期间以红字显示在主界面蜂鸣行，N 为 Beep_GetRemain() 的剩余秒数 */
#define STR_SEC       "\xC3\xEB"                                            /* 秒：蜂鸣倒计时文案的单位后缀，紧随 STR_BEEPING 后面的数字；单独拿出来是为了将来换单位/加图标时只改一处 */
#define STR_HINT_MAIN "\xB0\xB4\x5B\xC9\xE8\xD6\xC3\x5D\xBD\xF8\xC8\xEB\xC9\xE8\xD6\xC3"   /* 按[设置]进入设置：主界面底部两行操作提示的上面一行，方括号里的"[设置]"即 PF0 键，与 key.c 的键位定义对应 */
#define STR_HINT_TIME "\xB0\xB4\xCA\xB1\xB3\xD4\xD2\xA9\xD7\xD4\xB6\xAF\xCD\xA3\xD6\xB9"   /* 按时吃药自动停止：主界面底部两行操作提示的下面一行，说明"到点蜂鸣若无人干预会自动停"，是给用户的安全/行为说明而非按键提示 */
#define STR_BOXSEL    "\xD2\xA9\xBA\xD0\xD1\xA1\xD4\xF1"                    /* 药盒选择：SCR_BOX_SEL 界面顶部标题栏文字，由 DrawBoxSel() 传给 DrawTitleBar() 居中显示 */
#define STR_DAYSEL    "\xD1\xA1\xD4\xF1\xD0\xC7\xC6\xDA"                    /* 选择星期：SCR_DAY_SEL 界面顶部标题栏文字，由 DrawDaySel() 传给 DrawTitleBar() 居中显示 */
#define STR_TIMESET   "\xCA\xB1\xBC\xE4\xC9\xE8\xD6\xC3"                    /* 时间设置：SCR_TIME_SET 界面顶部标题栏文字，由 DrawRuleEdit() 传给 DrawTitleBar() 居中显示 */
#define STR_HINT_SEL  "\xBC\xD3\xBC\xF5\xD1\xA1\xD4\xF1\x20\xC8\xB7\xC8\xCF\xBD\xF8\xC8\xEB\x20\xB7\xB5\xBB\xD8\xB7\xB5\xBB\xD8"  /* 加减选择 确认进入 返回返回：药盒选择/星期选择两个"纯列表"界面的底部提示，三段分别对应 PF2/PF3、PF1、PF4；末段是代码里就存在的重复"返回"两字，不要"顺手修正" */
#define STR_HINT_SET  "\xBC\xD3\xBC\xF5\xB5\xF7\xD5\xFB\x20\xC8\xB7\xC8\xCF\xB1\xA3\xB4\xE6\x20\xB7\xB5\xBB\xD8\xB7\xB5\xBB\xD8"  /* 加减调整 确认保存 返回返回：时间设置界面在"列表阶段"(g_EditPhase==0)的底部提示；调小时/调分钟两阶段另用 STR_HINT_HOUR / STR_HINT_MIN */
#define STR_HINT_HOUR "加减=调小时 确认=进入分钟 返回=返回"
#define STR_HINT_MIN "加减=调分钟 确认=保存 返回=返回"
#define STR_SAVED     "\xB1\xA3\xB4\xE6\xB3\xC9\xB9\xA6"                    /* 保存成功：Flash_SaveConfig() 返回 1 时经 Menu_ShowToast() 弹出的短提示(1 秒)，覆盖在底部提示栏上；成功后还会顺带调 MQTT_PublishSchedule() 把新定时同步到云端 */
#define STR_SAVEFAIL  "\xB1\xA3\xB4\xE6\xCE\xDE\xD0\xA7"                    /* 保存无效：Flash_SaveConfig() 返回 0(擦写失败/校验不符)时的失败提示，仅在时间设置界面确认保存这一条路径上弹出；此时 g_Cfg 内存值已改而 Flash 未落盘，掉电会丢 */
#define STR_NOSET     "\xCE\xB4\xC9\xE8\xD6\xC3"                            /* 未设置：所有"该处没有有效时间"的统一文案 —— 时间设置界面里 hour==0xFF 的槽、以及 RuleDaySummary()/RuleSummary() 在整盒/整天都没定时时的输出 */

/* ==================== internal state ==================== */
/* ---------- 界面状态机的"当前位置" ---------- */
static ScrId_t g_Scr = SCR_MAIN;   /* 当前界面编号。全模块唯一的界面真源:
                                      绘制函数靠它决定"这一行右侧该显示哪种定时",
                                      Menu_Tick() 靠它决定"要不要做主界面按需刷新"。
                                      只允许 EnterScreen() 修改它, 见文件头说明。 */

/* ---------- 列表类界面的通用渲染缓存 ---------- */
static uint8_t g_ItemSel = 0;               /* 列表高亮光标所在行的下标(0 起) */
static uint8_t g_ItemCount = 0;             /* 当前界面列表总行数: 药盒选择=BOX_NUM(7), 星期选择=7。
                                               上/下键的环绕判断只看它, 所以两个界面可以共用同一段按键代码 */
static char   g_Items[BOX_NUM][16];         /* 列表每行左侧的文本(如"药盒3"、"星期三")。
                                               每次进入列表界面时由 DrawBoxSel()/DrawDaySel() 用
                                               sprintf 预生成, DrawItemRow() 直接取用 —— 好处是
                                               换行高亮时不必重新拼字符串, 只需重画那一行 */

/* ---------- 用户当前"钻"到哪一级 ---------- */
static uint8_t g_SelBox = 0;        /* 当前正在编辑的药盒下标 0~6, 对应屏上 1~7 号盒 */
static uint8_t g_KeepItemSel = 0;  /* 返回时保留列表光标位置 */
static uint8_t g_SelDay = 0;    /* 当前星期 (0=周一) */
static uint8_t g_TiSel  = 0;    /* 选中时间点索引 0~3 */
static uint8_t g_EditPhase = 0; /* 0=列表 1=调小时 2=调分钟 */
static uint8_t g_EditH = 8;     /* 编辑中小时 */
static uint8_t g_EditM = 0;     /* 编辑中分钟 */

/* ---------- 主界面按需重绘的影子变量(详见文件头"二、按需重绘") ----------
   共同约定: 只保存"上一次真正写到屏上的值", 由 Menu_Tick() 负责比对与刷新。
   异常初值(0xFF / 1)是刻意为之 —— 保证上电后第一次比较必然判定"已变化",
   于是首帧一定会画出来, 不需要单独写"首次强制刷新"的分支。 */
static uint32_t g_LastDateSig = 0;               /* 日期签名 = 年*10000+月*100+日。
                                                    不用三个变量分别比较, 是因为压缩成一个数后
                                                    Menu_Tick() 里只需一次 != 判断, 省去多次分支 */
static uint8_t  g_LastSec     = 0xFF;            /* 上次画到屏上的秒值。正常值域 0~59, 故 0xFF 必然不等 */
static LinkState_t g_LastLink = (LinkState_t)0xFF; /* 上次画到屏上的连接状态枚举。
                                                     LinkState_t 里没有 0xFF 这个取值, 借它当"未初始化"哨兵 */
static uint8_t  g_BoxDirty    = 1;               /* 药盒区脏标志。与上面三个不同: 它不靠轮询比对,
                                                    而是由 app.c 开盒/复位时调用 Menu_NotifyBoxChange() 置位,
                                                    属于"谁改谁举手"的通知式刷新, 省掉了每秒遍历 7 个盒状态的开销 */

/* ---------- 主界面蜂鸣行的临时消息(优先级高于蜂鸣倒计时) ---------- */
static char    g_MainMsg[32];        /* 消息缓冲, 最多 31 字符 + 结尾 '\0' */
static uint8_t g_MainMsgActive = 0;  /* 1=消息有效 */
static uint32_t g_MainMsgEnd = 0;    /* 到期时刻(Timer_GetTick 绝对时间戳, 单位 ms)。
                                        注意比较写法是 (int32_t)(now - End) < 0,
                                        用"有符号差值"判到期, 这样 32 位 tick 回绕时依然正确 */

/* ---------- 设置页底部的 Toast 短提示(保存成功/保存无效) ---------- */
static char    g_Toast[24];          /* 缓冲 23 字符 + '\0'; 比 g_MainMsg 短, 因为底部提示栏还要留白居中 */
static uint8_t g_ToastActive = 0;    /* 1=正在显示, 此时 DrawHintBar() 会用 Toast 覆盖掉常规按键提示 */
static uint32_t g_ToastEnd = 0;      /* 到期时刻(ms 时间戳, 同样用有符号差值判到期) */

/* 星期名查找表: 索引 = 内部星期下标(0=周一 ... 6=周日)。
   【索引约定】本文件所有"星期"数组一律 0~6 且 0 代表周一, 与 BoxRule_t 的
   hour[7][4] 第一维一致; 而 RTC 给出的 t.weekday 是 1~7(1=周一), 所以凡是要用
   RTC 的 weekday 去索引本表或 BoxRule_t 的地方都必须先减 1(见 Main_DrawDate、RuleNextMin)。
   把这条约定写在这里, 是因为下面每一个 hour[day][slot] 的下标都依赖它。 */
static const char *g_WeekName[7] =
{ "\xD0\xC7\xC6\xDA\xD2\xBB", "\xD0\xC7\xC6\xDA\xB6\xFE", "\xD0\xC7\xC6\xDA\xC8\xFD",
  "\xD0\xC7\xC6\xDA\xCB\xC4", "\xD0\xC7\xC6\xDA\xCE\xE5", "\xD0\xC7\xC6\xDA\xC1\xF9",
  "\xD0\xC7\xC6\xDA\xC8\xD5" };  /* Monday..Sunday */
/* Sort timepoints of one box+day ascending (unused slots last) */
/* Minutes from now until the next occurrence of (day,h:m) */
/**
  * @brief  计算"某个星期几的 h:m"下一次触发的绝对分钟数(供按最近触发排序使用)
  * @param  day 目标星期, 0=周一 ... 6=周日(内部约定, 不是 RTC 的 1~7)
  * @param  h   小时 0~23
  * @param  m   分钟 0~59
  * @return 触发时刻的绝对分钟数 = 目标日序号 * 1440 + h*60 + m, 日序号以
  *         RTC_GetEpoch() 的原点为基准, 因此返回值可与 nowMin 直接比大小。
  *         注意它**不是**"距现在还有多少分钟"的差值: 调用方只需要排序,
  *         返回绝对值可以少做一次减法, 也就少一次出错的机会。
  *
  * 【原理: 为什么必须有"跨周回绕"这一步】
  *   用户的定时是"每星期循环"的语义 —— "周一 08:00"指的是以后每个周一 08:00,
  *   而不是某一个具体的周一。要把"星期几 + 时分"落到一条时间轴上, 就得先确定
  *   "离现在最近的那个该星期几"是哪一天, 算法分三步:
  *     1) dayOff = (目标星期 - 今天星期 + 7) % 7
  *        - 先减后加 7 再取模, 是为了把 C 语言里可能出现的负余数掰回 0~6;
  *        - t.weekday 是 1~7, 必须先减 1 才能与内部 0~6 相减, 否则整体错位一天;
  *        - dayOff 的含义: 0=就是今天, 1=明天 ... 6=六天后。
  *     2) trig = (今天序号 + dayOff) * 1440 + h*60 + m
  *        把"哪一天"和"当天的哪一刻"组合成一个绝对分钟数。
  *     3) 若 trig <= nowMin, 则 trig += 7*1440(= 10080, 一周的分钟数)
  *        这是跨周回绕的关键: 目标时刻落在今天但已经过去(或正好是当前这一分钟)时,
  *        同一个星期几的下一次出现必然在 7 天之后, 于是整体后移一周。
  *        写成 <= 而不是 < 是刻意的: 恰好等于当前分钟时视为"刚刚发生过", 推到下周,
  *        免得排序把"正在触发的这一刻"当成最近项, 导致列表在触发瞬间来回跳动。
  *
  *   举例(今天周三, 内部下标 2, 假设现在 10:00):
  *     "周一 08:00" -> dayOff=(0-2+7)%7=5 -> 落在 5 天后, 即下周一 08:00
  *     "周三 23:00" -> dayOff=0, 尚未过, 落在今天 23:00(不回绕)
  *     "周三 09:00" -> dayOff=0 但早于现在, 回绕成下周三 09:00
  *
  * 【用途】只被 RuleSortDay() 的插入排序调用, 用来给 4 个时间槽定序。
  *         主界面和两个选择界面显示"最近一次定时"用的是 RuleSummary(),
  *         两者算法同源, 但 RuleSummary 要跨 7 天找全局最近项, 所以没有复用本函数。
  */
static uint32_t RuleNextMin(uint8_t day, uint8_t h, uint8_t m)
{
    RtcTime_t t;
    uint32_t nowMin, curDay, dayOff, trig;

    RTC_GetTime(&t);
    nowMin = RTC_GetEpoch() / 60u;   /* 当前绝对分钟数, 与返回值同坐标系 */
    curDay = nowMin / 1440u;         /* 今天是第几天(一天 1440 分钟) */
    dayOff = (uint32_t)((day - (int)(t.weekday - 1u) + 7) % 7);  /* RTC 的 1~7 先减 1 换成 0~6 再求日差 */
    trig = (curDay + dayOff) * 1440u + (uint32_t)h * 60u + m;
    if (trig <= nowMin) trig += 7u * 1440u;   /* 本周期已过 -> 跨周回绕到下周同一时刻 */
    return trig;
}

/* Sort timepoints of one box+day by next-trigger time (unused last) */
/**
  * @brief  把某药盒某星期的 4 个时间槽按"下一次触发时刻"由近到远重排, 未设置的槽排到最后
  * @param  box 药盒下标 0~6
  * @param  day 星期下标 0~6(0=周一)
  * @return 无。原地改写 g_Cfg.rules[box].hour[day][*] 与 min[day][*]
  *
  * 【原理: 为什么排"按最近触发"而不是排"按时间数值 00:00 -> 23:59"】
  *   用户看这张列表是想知道"下一个到点的是第几号槽"。若按数值排, 一台晚上 20:00
  *   开着的设备会把已经过去的"08:00"顶在列表第一行, 用户一眼看到的是过期信息。
  *   按 RuleNextMin 给出的绝对触发坐标排序后, 列表首项恒等于"从现在起的下一次提醒",
  *   第一行永远是最该关心的那条。排序键由 RuleNextMin 提供, 它已处理跨周回绕,
  *   所以"今天已过的时刻"会自动带上 +7 天偏移, 自然沉到后面。
  *
  * 【实现: 三步】
  *   第一步 紧凑化: 遍历 4 个槽, 把 hour != 0xFF 的有效槽依次拷进局部数组 nh/nm,
  *          用 cnt 记有效个数。这样做是把"空洞"摘掉, 排序只需处理连续的前 cnt 个元素;
  *          同时也把 0xFF 这个哨兵与真实数据彻底隔离 —— 排序过程中绝不可能把
  *          0xFF 当成小时拿去参与运算(0xFF=255, 若混进去排序键会飞出天际)。
  *   第二步 插入排序: 对 4 个元素来说这是最省事的选择 —— 无递归、无额外内存、
  *          几乎不占栈, 很适合裸机; 元素个数固定为 4, 性能差异可忽略。
  *          内层 while 是"整体后移"而不是"两两交换", 每轮比较都重新调 RuleNextMin;
  *          因为 RTC 只精确到秒, 一次排序内 nowMin 不会变, 排序键稳定、结果确定。
  *          条件写 > 而不是 >=, 相等时不动, 保证相同键的元素保持原有相对顺序(稳定排序)。
  *   第三步 回填: 排好序的写回前 cnt 个槽, 第 cnt~3 个槽统一恢复成 hour=0xFF / min=0。
  *
  * 【重要约束】本函数会改写槽的顺序, 而槽序就是用户在屏上看到的行序。
  *   因此调用时机被严格限制在"用户在时间设置界面按下确认、保存完一个槽之后"
  *   (见 Menu_OnKey 的 SCR_TIME_SET 第三阶段分支), 绝不能在按键过程中调用 ——
  *   否则用户正在编辑的那一行会因为重排突然跳到别的行号上, 光标跟着乱跳。
  */
static void RuleSortDay(uint8_t box, uint8_t day)
{
    uint8_t i, j, nh[BOX_MAX_TIMES], nm[BOX_MAX_TIMES], cnt = 0;   /* nh/nm = 紧凑后的有效槽副本 */

    /* --- 第一步: 把有效槽紧凑到 nh/nm 的前 cnt 个位置, 顺带滤掉 0xFF --- */
    for (i = 0; i < BOX_MAX_TIMES; i++)
    {
        if (g_Cfg.rules[box].hour[day][i] == 0xFFu) continue;   /* 0xFF = 该槽未设置, 不参与排序 */
        nh[cnt] = g_Cfg.rules[box].hour[day][i];
        nm[cnt] = g_Cfg.rules[box].min[day][i];
        cnt++;
    }
    /* insertion sort by next-trigger minute */
    /* --- 第二步: 插入排序, 排序键 = RuleNextMin() 的绝对触发分钟数 --- */
    for (i = 1; i < cnt; i++)
    {
        uint8_t vh = nh[i], vm = nm[i];              /* 本轮待插入元素先取出, 后面后移会覆盖它 */
        uint32_t vt = RuleNextMin(day, vh, vm);      /* 待插入元素的键, 循环外只算一次 */
        j = i;
        while (j > 0 && RuleNextMin(day, nh[j-1], nm[j-1]) > vt)  /* 用 > 不用 >=, 相等不交换 = 稳定排序 */
        {
            nh[j] = nh[j-1]; nm[j] = nm[j-1];        /* 元素整体后移让位, 比反复交换少一半写入 */
            j--;
        }
        nh[j] = vh; nm[j] = vm;                      /* 找到插入点后一次性落座 */
    }
    /* --- 第三步: 回填 4 个槽; 前 cnt 个是排好序的有效值, 其余恢复成"未设置" --- */
    for (i = 0; i < BOX_MAX_TIMES; i++)
    {
        if (i < cnt)
        {
            g_Cfg.rules[box].hour[day][i] = nh[i];
            g_Cfg.rules[box].min[day][i]  = nm[i];
        }
        else
        {
            g_Cfg.rules[box].hour[day][i] = 0xFFu;   /* 必须写回 0xFF, 不能写 0:
                                                        0 是合法的"0 点", 写 0 会让空槽在屏上显示成 00:00 并被当成凌晨提醒 */
            g_Cfg.rules[box].min[day][i]  = 0u;      /* 未用槽的 min 无意义, 清零只为让 Flash 内容确定、便于比对 */
        }
    }
}


/* Summary: the box rule closest to now (next trigger time) */

/* Auto-select first unused time slot for a box+day */
/**
  * @brief  自动把时间设置界面的光标 g_TiSel 定位到该盒该天的第一个"未设置"槽
  * @param  box 药盒下标 0~6
  * @param  day 星期下标 0~6(0=周一)
  * @return 无。副作用是写全局光标 g_TiSel(0~3)
  *
  * 【原理: 为什么要有这个"自动落位"】
  *   用户从星期选择界面按确认进入时间设置, 十有八九是想**新增**一个提醒, 而不是
  *   修改已有的。4 个槽是按"下一次触发时刻"排过序的, 所以"第一个空槽"在屏上总是
  *   排在最后一个有效时间之后 —— 光标自动落在那里, 用户一进来就能直接按加减调时间,
  *   省掉"先按几下加号把光标挪到空槽"的无谓操作。
  *
  *   反向的细节: 若 4 个槽全都已设置(没有空槽), 则回退到选中 0 号槽,
  *   让用户至少能改掉第一条, 而不是把光标留在不确定的位置上。
  *
  * 【调用时机】共两处, 都在 Menu_OnKey() 里:
  *   1) 星期选择界面按确认、即将进入时间设置时(保证一进去就落在空槽);
  *   2) 时间设置界面第三阶段保存成功后(该槽从空变满, 需要重新找下一个空槽;
  *      同时 RuleSortDay 刚刚重排过槽序, g_TiSel 的旧值已不再指向原来那一行)。
  *   注意本函数只改内存光标, **不碰 g_Cfg**, 也不写 Flash。
  */
static void RuleAutoSelect(uint8_t box, uint8_t day)
{
    uint8_t ti;

    for (ti = 0; ti < BOX_MAX_TIMES; ti++)   /* 从小到大扫, 取遇到的第一个空槽 */
    {
        if (g_Cfg.rules[box].hour[day][ti] == 0xFFu) { g_TiSel = ti; return; }   /* 找到即刻返回, 不动后面的槽 */
    }
    g_TiSel = 0;   /* 4 个槽全满: 退化为选中 0 号槽, 让用户至少能改第一条 */
}
/* First (earliest) set time of one box+day; "unset" if none */
/**
  * @brief  求某盒某天**数值最小**(最早)的那个已设置时间, 供星期选择界面每行右侧显示
  * @param  box 药盒下标 0~6
  * @param  day 星期下标 0~6(0=周一)
  * @param  out 输出缓冲, 至少 16 字节(调用方约定, 见 DrawItemRow 的 tbuf[16]),
  *             内容为 "HH:MM", 或该天一个时间都没设置时的 STR_NOSET("未设置")
  * @return 无, 结果通过 out 带回
  *
  * 【原理: 比较的是"钟表数值"而不是"距现在的距离"】
  *   这里刻意与 RuleSortDay 不同 —— 用的是 v = h*60 + m 这个纯时刻数值来比大小,
  *   与当前时间无关。因为这一行是给"用户正在挑哪一个星期几"做横向对比用的:
  *   七个星期几的摘要必须处在同一个评价尺度上, "周一最早 08:00 / 周二最早 09:30"
  *   才有可比性。如果掺进"距现在多少分钟", 同一天的不同行会因为当前时刻而扭曲,
  *   用户就没法一眼横向比较了。
  *
  * 【写法说明】found 标志配合 !found || v < best:
  *   第一轮没有"上一个值"可比, 直接用 !found 短路吸收首元素, 避免把 best 初值设成
  *   一个假想的大数(比如 65535)。同时 v 用 uint16_t 存放, 最大 23*60+59 = 1439,
  *   远在范围内, 不会溢出。
  *
  * 【易错点】0xFF 判定必须在取 h/m 之前 —— 未设置槽里的 min 是垃圾值,
  *   先取后判断会把它读出来参与计算(详见 flash.h 的时间槽特殊值约定)。
  */
static void RuleDaySummary(uint8_t box, uint8_t day, char *out)
{
    uint8_t ti;
    uint16_t best = 0;
    uint8_t bH = 0, bM = 0, found = 0;

    for (ti = 0; ti < BOX_MAX_TIMES; ti++)
    {
        uint8_t h, m;
        uint16_t v;

        if (g_Cfg.rules[box].hour[day][ti] == 0xFFu) continue;
        h = g_Cfg.rules[box].hour[day][ti];
        m = g_Cfg.rules[box].min[day][ti];
        v = (uint16_t)h * 60u + m;
        if (!found || v < best) { best = v; bH = h; bM = m; found = 1; }
    }
    if (!found)
    {
        strcpy(out, STR_NOSET);
        return;
    }
    sprintf(out, "%02u:%02u", (unsigned)bH, (unsigned)bM);
}

/**
  * @brief  求某药盒**距当前最近的未来触发时刻**(跨全部 7 天、4 个槽一起找),
  *         供主界面药盒行与药盒选择界面每行右侧显示
  * @param  box 药盒下标 0~6
  * @param  out 输出缓冲, 至少 16 字节(调用方约定, 见 Main_DrawBoxes 的 tbuf[16]),
  *             内容为 "星期三 08:00"(带星期, 说明是后面哪一天),
  *             或整盒一条有效定时都没有时的 STR_NOSET("未设置")
  * @return 无, 结果通过 out 带回
  *
  * 【原理: 与 RuleNextMin 同源, 但多了一层"跨天取最小"】
  *   RuleNextMin 只回答"某个指定星期几的下一次是何时"; 本函数要回答的是
  *   "这一盒不管星期几, 下一次到底是何时"。做法是把 7 天 x 4 槽全部展开,
  *   对每个有效槽算出一个绝对触发分钟数 trig(算法与 RuleNextMin 完全一致:
  *   先求日差 dayOff, 组合成绝对分钟数, 已过的 +7*1440 回绕到下周),
  *   再在所有这些 trig 里取最小值 best, 同时记下取得最小值的那个星期 bDay 和时分 bH/bM。
  *
  *   为什么要带上星期一起输出: 因为最近的一次可能不在今天。只显示 "08:00"
  *   会让用户误以为"今天 8 点会响", 而实际可能是下周一 8 点。带上"星期X"后
  *   这一行的语义就无歧义了。
  *
  * 【与 RuleSortDay 的关键差别 —— 别把两者混为一谈】
  *   - RuleSortDay 是**写**操作: 会重排 g_Cfg 里的槽序, 排序范围仅限"同一天";
  *   - 本函数是**只读**操作: 一个字节都不改 g_Cfg(否则每次刷新主界面都会写配置),
  *     而且查找范围是"7 天 x 4 槽"的全集。
  *   两者共享同一套回绕公式, 但没有任何调用关系。
  *
  * 【性能与调用频率的说明】本函数是 7*4=28 次循环, 每次主界面刷新药盒区会被
  *   连续调用 7 次(每盒一次), 合计约 196 次内层迭代。它只在 g_BoxDirty 置位时
  *   才被执行(即药盒状态真的变了), 不是每秒都跑, 所以这点开销可以接受;
  *   这也是为什么本函数没有为"排序/查找"做任何缓存或剪枝。
  *
  * 【易错点】同样必须先判 0xFF 再取 h/m; best 的初值靠 found 标志吸收首元素,
  *   不能图省事写成 0 —— 触发分钟数是一个从 1970 起算的大数, 用 0 当"无穷大"
  *   会让任何一次比较都失败, 结果永远是"未设置"。
  */
static void RuleSummary(uint8_t box, char *out)
{
    RtcTime_t t;
    uint32_t nowMin, curDay, best = 0;
    int d, ti, found = 0;
    uint32_t bDay = 0, bH = 0, bM = 0;

    RTC_GetTime(&t);
    nowMin = RTC_GetEpoch() / 60u;
    curDay = nowMin / 1440u;
    for (d = 0; d < 7; d++)
    {
        for (ti = 0; ti < BOX_MAX_TIMES; ti++)
        {
            uint8_t h, m;
            uint32_t dayOff, trig;

            if (g_Cfg.rules[box].hour[d][ti] == 0xFFu) continue;
            h = g_Cfg.rules[box].hour[d][ti];
            m = g_Cfg.rules[box].min[d][ti];
            dayOff = (uint32_t)((d - (int)(t.weekday - 1u) + 7) % 7);
            trig = (curDay + dayOff) * 1440u + (uint32_t)h * 60u + m;
            if (trig <= nowMin) trig += 7u * 1440u;
            if (!found || trig < best)
            {
                best = trig; bDay = d; bH = h; bM = m; found = 1;
            }
        }
    }
    if (!found)
    {
        strcpy(out, STR_NOSET);
        return;
    }
    sprintf(out, "%s %02lu:%02lu", g_WeekName[bDay], (unsigned long)bH, (unsigned long)bM);
}

/* ==================== forward decls ==================== */
/* 这些函数在文件中后置定义, 前置声明让 EnterScreen() 等调用点无需关心定义顺序。
   本模块的内部绘制函数全部是 static, 不对外暴露, 对外接口只有 menu.h 里那几个。 */
/* 切换到指定界面并重绘 */
static void EnterScreen(ScrId_t scr);
/* 绘制界面顶部标题栏(蓝底白字) */
static void DrawTitleBar(const char *title);
/* 绘制界面底部提示栏(含 Toast 覆盖) */
static void DrawHintBar(const char *hint);
/* 在指定 y 坐标居中绘制 16 号字文本 */
static void DrawTextCentered16(uint16_t y, const char *s, uint16_t fg, uint16_t bg);
/* 主界面标题 */
static void Main_DrawTitle(void);
/* 主界面 WiFi/云连接状态(绿/橙/红) */
static void Main_DrawLinkStatus(void);
/* 主界面日期+星期 */
static void Main_DrawDate(void);
/* 主界面大时钟(时:分:秒) */
static void Main_DrawClock(void);
/* 主界面蜂鸣倒计时提示 */
static void Main_DrawBeep(void);
/* 主界面 7 个药盒状态列表(状态灯+最近定时+开/待开) */
static void Main_DrawBoxes(void);
/* 主界面底部操作提示 */
static void Main_DrawHint(void);
/* 主界面整屏重绘 */
static void Main_RedrawAll(void);
/* 绘制药盒选择界面: 7 个药盒, 右侧显示各盒最近定时 */
static void DrawBoxSel(void);
/* 绘制全部列表项 */
static void DrawItemList(void);
/* 移动列表高亮光标 */
static void UpdateItemSel(uint8_t newSel);
/**
  * @brief  绘制时间设置界面(SCR_TIME_SET): 上下文行 + 4 个时间槽(高亮当前槽) + 底部按键提示
  * @param  无(全部状态取自全局: g_SelBox / g_SelDay / g_TiSel / g_EditPhase / g_EditH / g_EditM)
  * @return 无
  *
  * 屏幕布局(320x480, y 单位像素):
  *   0   ~ 39  : 标题栏, 显示 STR_TIMESET("时间设置"), 由 DrawTitleBar 画
  *   46        : 上下文行, 居中显示 "药盒N 星期X" —— 进入本界面后标题栏不再随层级变化,
  *               用户只能靠这一行确认自己正在改哪一盒哪一天
  *   86 起, 每 36 一行, 共 4 行: 4 个时间槽, 每行高 30、x 从 40 到 280, 行内三段文字:
  *               x=56  : 槽序号 1~4
  *               x=120 : 该槽已保存的时间 "HH:MM", 或未设置时的 STR_NOSET("未设置")
  *               x=216 : 编辑中的值(仅当前槽且 g_EditPhase != 0 时出现)
  *   440 ~ 479 : 底部提示栏, 由 DrawHintBar 画
  *
  * 【三段式编辑(g_EditPhase)在屏上的表现】
  *   0(列表态)  : 只有 4 行槽, 光标所在行白字紫底反显; 底部提示"加减调整 确认保存 返回返回"
  *   1(调小时)  : 光标行右端多出一段 "HH:--" —— 用 "--" 占位表示分钟这一位还没轮到编辑,
  *                这种"半成品显示"能让用户马上明白确认键还要再按一次才轮到分钟
  *   2(调分钟)  : 同一位置显示完整的 "HH:MM"
  *   提示栏也随阶段切换(STR_HINT_HOUR / STR_HINT_MIN), 因为不同阶段加减键改的东西不同。
  *
  * 【为什么每次调用都整屏 LCD_Clear + 重画全部 4 行, 而不做局部刷新】
  *   本界面只由按键事件驱动重绘(不像主界面有 1Hz 的时钟), 两次重绘之间按键必然已经
  *   抬起, 无人观看刷新过程, 所以不会看到闪烁; 而槽序会因为 RuleSortDay 重排,
  *   "哪一行变了"其实不好判定, 整屏重画反而更简单可靠。省带宽的优化只用在主界面
  *   那种每秒都要刷新的高频路径上(见文件头"二、按需重绘")。
  *
  * 【注意】本函数是**只读**绘制的: 不修改 g_Cfg。编辑中的值只存在于 g_EditH/g_EditM,
  *   只有用户在第三阶段按确认时才写回 g_Cfg 并落盘 Flash —— 于是"返回键"天然等于
  *   "放弃本次修改", 不需要额外的备份/还原逻辑。
  */
static void DrawRuleEdit(void)
{
    char ctx[24], line[16];
    uint8_t ti;
    const BoxRule_t *r = &g_Cfg.rules[g_SelBox];   /* 当前盒的规则基址, 下面两个指针是"该盒该天"这一维的切片 */
    const uint8_t *hrs = r->hour[g_SelDay];        /* 指向 4 个槽的小时数组, 用 hrs[ti] 比写 r->hour[g_SelDay][ti] 清爽 */
    const uint8_t *mns = r->min[g_SelDay];         /* 对应的分钟数组 */

    LCD_Clear(WHITE);              /* 清整屏: 上一界面可能留有药盒/星期列表, 必须擦干净 */
    DrawTitleBar(STR_TIMESET);

    /* 上下文行: 把"药盒几 + 星期几"合成一句居中显示, 防止用户改错对象 */
    sprintf(ctx, "%s%d %s", STR_BOX, (int)g_SelBox + 1, g_WeekName[g_SelDay]);   /* 下标 0~6 显示成 1~7 */
    DrawTextCentered16(46u, ctx, BLACK, WHITE);

    for (ti = 0; ti < BOX_MAX_TIMES; ti++)
    {
        uint16_t y = (uint16_t)(86u + ti * 36u);   /* 行距 36 > 行高 30, 留 6 像素间隙分隔各行 */
        uint8_t  sel = (ti == g_TiSel) ? 1u : 0u;
        uint16_t fg = sel ? WHITE : BLACK;         /* 选中行反显: 白字 + 紫底(HILITE_BG) */
        uint16_t bg = sel ? HILITE_BG : WHITE;

        LCD_FillRect(40u, y, 280u, (uint16_t)(y + 30u), bg);   /* 先铺底色, 局部重画时背景不会残留旧字 */
        sprintf(line, "%d", (int)ti + 1);                      /* 槽序号按人习惯从 1 开始显示 */
        LCD_ShowText16(56u, (uint16_t)(y + 8u), line, fg, bg);
        if (hrs[ti] == 0xFFu)
        {
            strcpy(line, STR_NOSET);   /* 0xFF = 未设置, 显示"未设置"而不是 00:00(见 flash.h 的约定) */
        }
        else
        {
            sprintf(line, "%02u:%02u", (unsigned)hrs[ti], (unsigned)mns[ti]);   /* 补零对齐, 4 行竖着看整齐 */
        }
        LCD_ShowText16(120u, (uint16_t)(y + 8u), line, fg, bg);
        if (g_EditPhase != 0u && ti == g_TiSel)   /* 只有"正在编辑的那一行"才追加编辑值 */
        {
            char edit[8];

            if (g_EditPhase == 1u) sprintf(edit, "%02u:--", (unsigned)g_EditH);   /* 调小时阶段: 分钟位尚未编辑, 用 -- 占位 */
            else                   sprintf(edit, "%02u:%02u", (unsigned)g_EditH, (unsigned)g_EditM);
            LCD_ShowText16(216u, (uint16_t)(y + 8u), edit, fg, bg);
        }
    }

    /* 底部提示随编辑阶段切换: 三个阶段加减键改的东西不同, 不能用同一句提示 */
    if (g_EditPhase == 0u)      DrawHintBar(STR_HINT_SET);
    else if (g_EditPhase == 1u) DrawHintBar(STR_HINT_HOUR);
    else                        DrawHintBar(STR_HINT_MIN);
}

/**
  * @brief  绘制星期选择界面(SCR_DAY_SEL): 7 行"星期一~星期日", 每行右侧显示该盒该天最早的时间点
  * @param  无(药盒取自全局 g_SelBox, 光标取自 g_ItemSel / g_KeepItemSel)
  * @return 无
  *
  * 屏幕布局: 标题栏(y 0~39)显示 STR_DAYSEL("选择星期"); 标题栏左上角 (4,4) 另叠一小段
  *   "药盒N" 白字 —— 这是本界面独有的做法, 用来在列表本身只显示星期几的情况下,
  *   补上"正在给哪一盒选星期"这个上下文; 列表区从 ITEM_Y0 起, 7 行每行 ITEM_ROW_H;
  *   底部提示栏显示 STR_HINT_SEL("加减选择 确认进入 返回返回")。
  *
  * 【回退光标的实现: g_KeepItemSel 一次性标志】
  *   从时间设置界面按返回键回到本界面时, Menu_OnKey 会先置 g_KeepItemSel = 1 并把
  *   g_ItemSel 预置成 g_SelDay, 再调 EnterScreen()。于是本函数开头的
  *   "if (!g_KeepItemSel) g_ItemSel = 0;" 就不会执行, 光标得以停在用户上次选的星期上,
  *   而不是跳回"星期一"。
  *   关键在紧随其后的 g_KeepItemSel = 0: 标志是**一次性的**, 用完立刻清零。
  *   否则用户之后从主界面一路重新进来时也会带着这个标志, 光标被"锁"在旧位置,
  *   看起来就像按键失灵。
  *
  * 【为什么本界面只有 7 行, 而 DrawBoxSel 是 BOX_NUM 行, 却能共用按键代码】
  *   两者都只用 g_ItemCount 做环绕判断、都只走 DrawItemList()/UpdateItemSel(),
  *   区别仅在于本函数把 g_ItemCount 设成 7、把 g_Items[] 填成星期名。
  *   这种"同一套渲染管线换数据源"的写法使得上下键逻辑只需维护一份。
  */
static void DrawDaySel(void)
{
    uint8_t i;

    g_ItemCount = 7u;
    if (!g_KeepItemSel) g_ItemSel = 0;
    g_KeepItemSel = 0;
    for (i = 0; i < 7u; i++)
    {
        strcpy(g_Items[i], g_WeekName[i]);
    }
    LCD_Clear(WHITE);
    DrawTitleBar(STR_DAYSEL);
    {
        char ctx[16];
        sprintf(ctx, "%s%d", STR_BOX, (int)g_SelBox + 1);
        LCD_ShowText16(4u, 4u, ctx, WHITE, TITLE_BG);
    }
    DrawItemList();
    DrawHintBar(STR_HINT_SEL);
}

/**
  * @brief  绘制界面顶部的标题栏: 铺满全宽、高 TITLE_BAR_H 像素的蓝底白字大标题
  * @param  title 标题字符串。约定必须是 **GBK 编码的中文**(如"药盒选择"),
  *               因为它同时参与宽度计算, 详见下面的注意事项
  * @return 无
  *
  * 屏幕区域: x 0 ~ LCD_W-1(320), y 0 ~ TITLE_BAR_H-1(39), 即屏幕最上方 40 像素的一条横带。
  *   本模块四个界面都调用它, 因此四个界面的标题栏高度、配色完全一致, 视觉上统一。
  *
  * 【宽度计算 w = strlen(title) / 2 * 24 的原理与陷阱】
  *   字库里的 24 号汉字是 24x24 点阵, 而字符串按 GBK 存储时一个汉字占 2 字节,
  *   所以 "字节数 / 2" 就是汉字个数, 再乘 24 得到实际像素宽度。用像素宽度而不是
  *   字符个数来居中, 是因为 (LCD_W - w) / 2 必须得到像素坐标。
  *   **陷阱**: 这个公式只对纯中文标题成立。若传入 "WiFi" 这类 ASCII(1 字节 1 字符),
  *   算出来的 w 只有真实宽度的一半, 标题会明显右偏。本文件所有传给本函数的都是
  *   中文常量(STR_BOXSEL / STR_DAYSEL / STR_TIMESET), 是安全的;
  *   而主界面标题因为要显示 6 个汉字、宽度固定, 干脆没有走本函数, 见 Main_DrawTitle。
  *
  * 【为什么 y 从 2 而不是 0 开始画字】24 号字实际占 24 像素, 而条带高 40 像素,
  *   字高在 40 里居中本该 y=8; 但本屏字库的点阵顶部通常留有空行, 取 2 是配合字库
  *   实测的视觉居中值 —— 属于经验参数, 改动前请上屏确认, 不要按公式"纠正"。
  */
static void DrawTitleBar(const char *title)
{
    uint16_t w = (uint16_t)(strlen(title) / 2u * 24u);   /* GBK: 字节数/2 = 汉字数; 24 号字每字宽 24 像素 */

    LCD_FillRect(0, 0, LCD_W - 1, TITLE_BAR_H - 1, TITLE_BG);   /* 先铺满蓝底, 顺便盖掉上一界面的残留 */
    LCD_ShowText24((LCD_W - w) / 2u, 2u, title, WHITE, TITLE_BG);
}

/**
  * @brief  绘制界面底部的提示栏, 在 Toast 有效期内用 Toast 文案覆盖常规按键提示
  * @param  hint 常规按键提示文案(如 STR_HINT_SEL); **允许传 NULL**, 此时按空串处理,
  *              专门给 Menu_ShowToast() 用 —— 它只想显示 Toast, 手里没有合适的常规提示
  * @return 无
  *
  * 屏幕区域: x 0 ~ LCD_W-1, y 440 ~ LCD_H-1(479), 即屏幕最下方 40 像素。
  *   文字在 y=446 处水平居中。注意主界面的提示区(见 Main_DrawHint)是画在
  *   410~475 的, 两者虽然都叫"底部提示"但坐标不同: 主界面用的是白底灰字的
  *   操作说明两行, 而本函数画的是灰底黑字的单行按键栏, 只在三个设置界面使用。
  *
  * 【优先级设计: g_ToastActive 一旦有效就无条件盖掉 hint】
  *   本模块有三个界面共用这一条提示栏, 显示内容由 g_Scr 决定(见 Menu_Tick 里
  *   三元表达式那段)。"保存成功/保存无效"这类反馈必须在用户刚按完确认键时
  *   立刻可见, 否则用户会怀疑到底存没存进去, 于是设计成 Toast 优先。
  *   覆盖是**绘制层面的**, 不改变 g_Toast/保存状态本身; Toast 到期后由
  *   Menu_Tick() 复位 g_ToastActive 并重新调本函数, 常规提示就自动"回来"了,
  *   不需要在 Toast 里备份原提示文本。
  *
  * 【为什么用 (int32_t)(now - end) < 0 而不是 now < end】
  *   Timer_GetTick() 是 32 位毫秒计数, 约 49.7 天回绕一次。直接比大小会在回绕点
  *   出错; 改成"有符号差值是否仍为负"后, 只要到期时间没跨过半个计数周期,
  *   判断在回绕前后都成立。全模块所有超时判断都统一用这个写法。
  */
static void DrawHintBar(const char *hint)
{
    const char *txt = hint;

    if (g_ToastActive && (int32_t)(Timer_GetTick() - g_ToastEnd) < 0)   /* 未到期的有符号差值为负 */
    {
        txt = g_Toast;   /* Toast 优先: 保存成功/失败反馈必须立刻可见 */
    }
    if (txt == NULL)
    {
        txt = "";        /* Menu_ShowToast 会传 NULL 进来, 兜底成空串以免 LCD_TextWidth16 解引用空指针 */
    }
    LCD_FillRect(0, 440u, LCD_W - 1, LCD_H - 1, HINT_BG);
    LCD_ShowText16((LCD_W - LCD_TextWidth16(txt)) / 2u, 446u, txt, BLACK, HINT_BG);
    /* 用 LCD_TextWidth16 实测像素宽度来居中, 而不是按字数估算 ——
       中文 16 号字宽 16、ASCII 只有 8, 混排时按字数算必然偏。 */
}

/**
  * @brief  在指定 y 坐标处, 把 16 号字体的一行文本沿屏幕水平方向居中
  * @param  y  文字的顶边 y 坐标(像素)。只决定纵向位置, 横向由本函数算
  * @param  s  要显示的文本(GBK 编码)
  * @param  fg 前景色(字色)
  * @param  bg 背景色; 必须与调用方预先 LCD_FillRect 铺好的底色一致,
  *            否则字模的"背景像素"会与周围底色不同, 看起来像给每个字加了色块
  * @return 无
  *
  * 【原理】居中就是 (屏宽 - 文本实测宽度) / 2。宽度必须用 LCD_TextWidth16()
  *   实测而不能按 strlen 推算, 因为中英文混排时 ASCII 是半宽, 按字节数算会偏。
  *   结果可能算出奇数, 整数除法向下取整, 最多偏 1 像素, 肉眼不可辨。
  *
  * 【用途】本模块里所有"整行居中"的短文本都走它, 统一了居中口径:
  *   主界面日期行/蜂鸣行/两行操作提示, 以及时间设置界面的上下文行。
  *   注意主界面标题不走它, 因为那是 24 号字且居中口径不同(见 Main_DrawTitle)。
  */
static void DrawTextCentered16(uint16_t y, const char *s, uint16_t fg, uint16_t bg)
{
    LCD_ShowText16((LCD_W - LCD_TextWidth16(s)) / 2u, y, s, fg, bg);
}

/* ==================== main screen ==================== */
/*
 * 主界面的屏幕分区总览(320x480, y 为像素):
 *     0 ~  39  标题栏         Main_DrawTitle      "智能药箱系统"
 *    46 ~  61  连接状态栏     Main_DrawLinkStatus WiFi/云 两段独立着色的状态指示
 *    68 ~  83  日期行         Main_DrawDate       "2024年05月20日 星期一"
 *    90 ~ 121  大时钟         Main_DrawClock      HH:MM:SS, 8 字符宽
 *   128 ~ 143  蜂鸣/消息行    Main_DrawBeep       三选一, 见该函数
 *   190 ~ 405  药盒列表(7行)  Main_DrawBoxes      每行 BOX_ROW_H=30, 行末一条分隔线
 *   408 ~ 475  操作提示       Main_DrawHint       两行灰字 + 上边一条分隔线
 *   476 ~ 479  底部留白       (无)
 * 各绘制函数只负责自己那一条矩形, 且都先 LCD_FillRect 填白底再写字 ——
 * 这是"按需局部重绘"能成立的前提: 局部重画时没有整屏底色可依靠, 必须自带背景。
 */

/**
  * @brief  绘制主界面顶部标题栏
  * @param  无
  * @return 无
  *
  * 屏幕区域: x 0~319, y 0~TITLE_BAR_H-1(39)。
  *
  * 【为什么不用 DrawTitleBar()】
  *   两者画的是同一条蓝带, 但 DrawTitleBar 靠 strlen(title)/2*24 反推宽度,
  *   只对长度不定的中文标题合适。主界面标题是全屏最显眼的一行, 这里把
  *   宽度 24u * 6u 直接写死(6 个汉字 = 144 像素), 好处有二:
  *     1) 居中位置 (320-144)/2 = 88 在编译期就确定了, 不依赖运行时 strlen;
  *     2) 万一有人改了 STR_TITLE 的字数, 这里会立刻"歪掉"从而被肉眼发现,
  *        而不是被 strlen 悄悄算出一个"看着还算居中"的假象。
  *   代价是标题字数与这里的 6 必须保持一致, 改动 STR_TITLE 时要同步改这里。
  */
static void Main_DrawTitle(void)
{
    LCD_FillRect(0, 0, LCD_W - 1, TITLE_BAR_H - 1, TITLE_BG);
    LCD_ShowText24((LCD_W - 24u * 6u) / 2u, 2u, STR_TITLE, WHITE, TITLE_BG);   /* 24u*6u = 6 个 24 号汉字的宽度, 写死以便居中 */
}

/**
  * @brief  绘制连接状态栏: 一行里并排显示 "WiFi:<状态> 云<状态>", 两段状态各自独立着色
  * @param  无(状态实时取自 MQTT_GetLinkState())
  * @return 无
  *
  * 屏幕区域: x 4~316, y 46~61(高 16 像素, 正好一行 16 号字)。
  * 显示形态举例(四种组合之一):
  *     WiFi:已连接 云已连接     (两段都绿, 完全正常)
  *     WiFi:已连接 云连接中     (WiFi 通、MQTT 还在握手)
  *     WiFi:连接中 云未连接     (正在连 WiFi, 云当然谈不上)
  *     WiFi:未连接 云未连接     (全部红, 离线)
  *
  * 【原理: 为什么用 switch 先把"颜色 + 文案"一次算完, 再统一绘制】
  *   链路有四个状态, 每个状态要决定两个颜色和两段文字, 共 4 个变量。
  *   如果一边判断一边画, 这段代码会变成四层嵌套 if, 且"文字宽度"会影响后面文字的
  *   x 坐标, 很难改。现在把"逻辑判断"与"上屏"彻底分开: switch 只填变量,
  *   后面的 4 次 LCD_ShowText16 是纯线性代码, 想调间距只需动那几行。
  *
  * 【状态到颜色的映射逻辑(体现"故障定位"的设计意图)】
  *   LNK_READY: 两段绿 —— WiFi 与 MQTT 都就绪
  *   LNK_MQTT : WiFi 绿 / 云橙 —— 说明卡在云平台这一层, 网络本身没问题
  *   LNK_WIFI : WiFi 橙 / 云红 —— 橙表示"正在连", 云段直接红: 网络都没通, 云必然不通
  *   默认(断线): 两段都红
  *   即: 绿色=好, 橙色=正在努力, 红色=坏。用户扫一眼颜色就知道该查网络还是查云端。
  *
  * 【xx 变量: 手写的"排版游标"】
  *   LCD 库没有"流式排版"能力, 每段文字的 x 都得自己算。xx 从左边距 8 开始,
  *   每画完一段就加上该段的实测宽度再补 8 像素间隙, 于是四段自动排开。
  *   必须用 LCD_TextWidth16() 实测: 中文 16 号字宽 16、ASCII 只有 8,
  *   "WiFi:" 和 "已连接" 的字节数相同但像素宽度不同, 按字节数累加会重叠。
  *
  * 【注意】xx 在画 "WiFi:" 段时只加了宽度没加间隙(见该行), 因为冒号本身就是
  *   视觉分隔符, 后面的状态文字紧跟冒号才读得通。这是刻意的不一致, 不要"统一"成 +8。
  */
static void Main_DrawLinkStatus(void)
{
    uint16_t fgWifi, fgCloud;   /* 两段状态文字各自的颜色 */
    const char *sw, *sc;        /* 两段状态文字: WiFi 段 / 云段 */
    uint16_t xx = 8u;           /* 排版游标: 当前这段文字该从哪个 x 开始画 */

    /* 先把"颜色 + 文案"一次算完, 下面就是纯线性上屏代码, 见函数注释 */
    switch (MQTT_GetLinkState())
    {
    case LNK_READY: fgWifi = GREEN; fgCloud = GREEN; sw = STR_CONN_OK; sc = STR_CONN_OK; break;
    case LNK_MQTT:  fgWifi = GREEN; fgCloud = ORANGE; sw = STR_CONN_OK; sc = STR_CONN_ING; break;
    case LNK_WIFI:  fgWifi = ORANGE; fgCloud = RED;   sw = STR_CONN_ING; sc = STR_CONN_NO; break;
    default:        fgWifi = RED;   fgCloud = RED;    sw = STR_CONN_NO; sc = STR_CONN_NO; break;
    }

    LCD_FillRect(4u, 46u, 316u, 61u, WHITE);   /* 本行整条填白: 状态从绿变红时不会残留旧颜色像素 */
    LCD_ShowText16(xx, 46u, "WiFi:", BLACK, WHITE);   xx += LCD_TextWidth16("WiFi:");   /* 紧跟冒号, 故意不留间隙 */
    LCD_ShowText16(xx, 46u, sw, fgWifi, WHITE);       xx += (uint16_t)(LCD_TextWidth16(sw) + 8u);   /* 段间留 8 像素 */
    LCD_ShowText16(xx, 46u, STR_CLOUD, BLACK, WHITE); xx += LCD_TextWidth16(STR_CLOUD) + 8u;        /* "云"字用黑色, 只作标签 */
    LCD_ShowText16(xx, 46u, sc, fgCloud, WHITE);      /* 状态文字用颜色区分, 不靠文字长短辨认 */
}

/**
  * @brief  绘制主界面日期行: "YYYY年MM月DD日 星期X"
  * @param  无(时间实时取自 RTC_GetTime())
  * @return 无
  *
  * 屏幕区域: x 4~316, y 68~83。整行水平居中。
  *
  * 【格式串里的字节转义】
  *   "%04d\xC4\xEA%02d\xD4\xC2%02d\xC8\xD5\x20%s" 中的 \xC4\xEA 是 GBK 的"年"、
  *   \xD4\xC2 是"月"、\xC8\xD5 是"日"、\x20 是空格。之所以不直接写汉字,
  *   是为了让这个文件在任意编码的编辑器里打开展示都一致(本文件整体是 GBK,
  *   但字符串改用字节转义后就不依赖源码编码了)。改格式时不要动这些字节。
  *
  * 【下标 t.weekday - 1u 是必须的】
  *   RTC 给出的 weekday 是 1~7(1=周一), 而 g_WeekName[] 是 0~6(0=周一),
  *   不减 1 会整体错位一天 —— 这是本模块最容易犯的错, 所有用到 weekday 的地方
  *   (本函数、RuleNextMin、RuleSummary)都做了同样的减 1。
  *
  * 【%02d 而不是 %d】月和日补零后整行长度固定, 每月 1 号/10 号切换时
  *   居中的位置不会左右跳动。
  */
static void Main_DrawDate(void)
{
    char buf[32];
    RtcTime_t t;

    RTC_GetTime(&t);
    LCD_FillRect(4u, 68u, 316u, 83u, WHITE);   /* 先清本行: 跨零点后日期位数变化, 必须擦掉旧内容 */
    sprintf(buf, "%04d\xC4\xEA%02d\xD4\xC2%02d\xC8\xD5\x20%s",
            t.year, t.month, t.day, g_WeekName[t.weekday - 1u]);   /* RTC 的 1~7 -> 表的 0~6 */
    DrawTextCentered16(68u, buf, BLACK, WHITE);
}

/**
  * @brief  绘制主界面大时钟: 超大字号显示 HH:MM:SS
  * @param  无(时间实时取自 RTC_GetTime())
  * @return 无
  *
  * 屏幕区域: x 4~316, y 90~121(高 32 像素, 是本屏最大的一行字), 水平居中。
  *
  * 【宽度估算 w = 8u * 16u 的含义与局限】
  *   时钟一共 8 个字符("HH:MM:SS"), 大号字库里数字与冒号都是半宽, 每个约 16 像素,
  *   故总宽约 128 像素。这里直接写死估算值而不是用 LCD_TextWidth, 是因为
  *   LCD_ShowBigClock() 是按"时/分/秒"三个数值 + 分隔符自行排版绘制的,
  *   并没有一个现成的字符串能拿去量宽度。
  *   因此这个 8u*16u 是经验值: 若将来更换大号字库或改成 "HH:MM" 少显示秒,
  *   **必须同步改这个数**, 否则时钟会偏左/偏右。选中它居中也是同一个原因。
  *
  * 【为什么本行每次刷新都整条填白】秒每跳一次都要把旧的 "23:59:59" 抹掉,
  *   大号字笔画粗, 不清底会叠影。本函数正是 Menu_Tick() 里 g_LastSec 变化时才调的
  *   (1Hz), 不是每毫秒都调, 所以这点带宽开销是可控的。
  */
static void Main_DrawClock(void)
{
    RtcTime_t t;
    uint16_t w = 8u * 16u;   /* "HH:MM:SS" 共 8 个半宽字符, 每字符约 16 像素(经验值, 换字库要同步改) */

    RTC_GetTime(&t);
    LCD_FillRect(4u, 90u, 316u, 121u, WHITE);   /* 大号笔画粗, 必须先清底, 否则秒跳时叠影 */
    LCD_ShowBigClock((LCD_W - w) / 2u, 90u, t.hour, t.minute, t.second, BLACK, WHITE);
}

/**
  * @brief  绘制主界面"蜂鸣/消息"行: 三选一显示(临时消息 > 蜂鸣倒计时 > 系统运行中)
  * @param  无
  * @return 无
  *
  * 屏幕区域: x 4~316, y 128~143, 整行水平居中。
  *
  * 【三级优先级, 从上到下依次判定】
  *   1) g_MainMsgActive 且未到期  -> 显示 g_MainMsg(由 Menu_ShowMainMsg() 设置的临时消息,
  *      通常来自远程开盒通知之类的外部事件), 红色。给 3 秒(MAIN_MSG_TIME_MS)。
  *   2) Beep_IsActive()           -> 显示 "蜂鸣中 剩余N秒"(STR_BEEPING + 剩余秒 + STR_SEC), 红色。
  *   3) 都没有                    -> 显示 STR_RUNNING("系统运行中"), 深蓝色。
  *   消息排在最前面是有意的: 外部事件必须能立刻顶掉倒计时, 否则用户会以为设备没反应。
  *
  * 【颜色即语义】红色 = "正在发生一件需要你注意的事"(消息/蜂鸣);
  *   深蓝 = 平静待命。用户不必读文字, 扫一眼颜色就知道设备有没有在动作。
  *
  * 【为什么用 strncpy(..., 31) 再手动补 buf[31] = 0】
  *   strncpy 在源串长度 >= n 时**不会**写结尾的 '\0'。这里 n=31、缓冲 32 字节,
  *   手动补第 32 个字节才能保证 buf 一定是以 '\0' 结尾的合法字符串,
  *   否则 g_MainMsg 恰好填满时会把后面的栈内容当字符串继续打印出去。
  *
  * 【已知限制(不是 bug, 但改动须知)】本行只有 16 像素高, 只容得下一行 16 号字;
  *   而 g_MainMsg 缓冲允许 31 个字符, 中文字符串超过约 19 个汉字就会超出屏幕宽度,
  *   超出的部分不会被裁剪, 会直接画到屏外(不影响内存安全, 因为已保证 '\0' 结尾)。
  *   传入长消息时请自行控制长度, 或改成滚动/截断显示。
  *
  * 【调用频率】本函数由 Menu_Tick() 在"秒变化"时调用(与时钟同一个 1Hz 分支),
  *   所以 g_MainMsg 到期后最迟 1 秒内会恢复成蜂鸣或"系统运行中"。
  */
static void Main_DrawBeep(void)
{
    char buf[32];               /* 缓冲足够容纳 31 字符 + '\0' */
    uint16_t fg = RED;          /* 默认红色: 消息与蜂鸣都用红色, 只有"系统运行中"才改成深蓝 */

    LCD_FillRect(4u, 128u, 316u, 143u, WHITE);   /* 每次先清本行, 三种状态互相切换时不留残影 */

    if (g_MainMsgActive && (int32_t)(Timer_GetTick() - g_MainMsgEnd) < 0)   /* 有符号差值判到期, 抗 tick 回绕 */
    {
        strncpy(buf, g_MainMsg, 31);
        buf[31] = 0;            /* strncpy 不保证补 '\0', 必须手动补, 见函数注释 */
    }
    else if (Beep_IsActive())
    {
        sprintf(buf, "%s%u%s", STR_BEEPING, (unsigned)Beep_GetRemain(), STR_SEC);   /* "蜂鸣中 剩余N秒" */
    }
    else
    {
        strcpy(buf, STR_RUNNING);
        fg = DARKBLUE;          /* 平静待命用深蓝, 与"需要关注"的红色形成区分 */
    }
    DrawTextCentered16(128u, buf, fg, WHITE);
}

/**
  * @brief  绘制主界面的 7 行药盒状态列表(状态灯 + 药盒号 + 最近定时 + 开/待开), 末尾补一条分隔线
  * @param  无(数据取自 g_BoxState[] 与 g_Cfg)
  * @return 无
  *
  * 屏幕区域: y 从 MAIN_BOX_Y0(190) 起, 每行 BOX_ROW_H(30) 像素, 共 BOX_NUM(7) 行,
  *   即 190 ~ 399; 最后在 y = MAIN_BOX_Y0 + 7*BOX_ROW_H - 2 = 398 处画一条浅灰横线
  *   (x 4~316)作为列表与下方操作提示之间的收尾分隔。
  *   每一行的 x 布局:
  *     10 ~ 25  方形状态灯(16x18): 绿=已打开, 浅灰=待打开
  *     30~       "药盒" 二字; 紧接着是编号 1~7
  *     90 ~      RuleSummary() 给出的"最近一次定时", 如 "星期三 08:00" 或 "未设置"
  *     右对齐    状态文字"已打开"(绿) / "待打开"(深灰)
  *
  * 【状态灯与状态文字用两套颜色, 是有意的冗余设计】
  *   状态灯用 GREEN / LGRAY 这一对高对比色, 是为了让人在两三米外、不读字的情况下
  *   扫一眼就知道哪几个盒子已经弹开了; 右侧文字则用 GREEN / DGRAY 这一对更"文雅"的
  *   颜色, 因为文字需要保证在白色背景上有足够可读性。同一状态在两处的灰阶不同,
  *   不是漏改, 而是分别针对"远看"和"近读"两种使用场景调过的。
  *
  * 【右对齐的实现】LCD 库只有左对齐绘制, 所以先量出文字像素宽度 sw,
  *   再从右边界 316 减去 sw 得到起点 x。必须实测宽度: "已打开"和"待打开"都是 3 个汉字,
  *   碰巧等宽, 但一旦文案改成中英混排, 按字数推算就会错位。
  *
  * 【xx 排版游标】与 Main_DrawLinkStatus 同一手法: xx 从 30 开始, 画完"药盒"后
  *   +32(而不是 +LCD_TextWidth16("药盒")) —— 这里用固定步进是为了让 1~7 号盒的
  *   编号永远从同一列开始, 即使将来"药盒"二字的字库宽度变了, 编号列也不会左右参差。
  *
  * 【刷新时机: 由 g_BoxDirty 驱动, 不是每秒都画】
  *   Menu_Tick() 只在 g_BoxDirty 为 1 时调用本函数, 而该标志仅由
  *   Menu_NotifyBoxChange()(app.c 开关盒时)置位。所以稳态下这一大块区域
  *   一个像素都不会被重写 —— 这正是省 FSMC 带宽的关键所在。
  */
static void Main_DrawBoxes(void)
{
    uint16_t y = MAIN_BOX_Y0;   /* 行首 y, 每画完一行自增 BOX_ROW_H */
    uint8_t  b;

    for (b = 0; b < BOX_NUM; b++)
    {
        char num[4];
        char tbuf[16];                                        /* RuleSummary 的输出缓冲, 约定 >= 16 */
        uint8_t opened = (g_BoxState[b] == BOX_OPENED);
        uint16_t xx = 30u;                                    /* 行内排版游标 */
        uint16_t sw;

        LCD_FillRect(4u, y, 316u, (uint16_t)(y + BOX_ROW_H - 1u), WHITE);   /* 整行清底: 状态/定时变化时不留残影 */
        LCD_FillRect(10u, (uint16_t)(y + 6u), 25u, (uint16_t)(y + 23u),
                     opened ? GREEN : LGRAY);                 /* 16x18 状态灯: 远看用, 绿=已弹开 */
        LCD_ShowText16(xx, (uint16_t)(y + 7u), STR_BOX, BLACK, WHITE);
        xx += 32u;                                            /* 固定步进, 保证 7 行的编号列严格对齐 */
        sprintf(num, "%d", (int)b + 1);                       /* 内部下标 0~6 显示成 1~7 号盒 */
        LCD_ShowText16(xx, (uint16_t)(y + 7u), num, BLACK, WHITE);

        /* time: first configured time point of this box */
        /* 该盒最近一次定时(跨 7 天找最小触发时刻); 整盒无定时时输出 "未设置" */
        RuleSummary(b, tbuf);
        LCD_ShowText16(90u, (uint16_t)(y + 7u), tbuf, BLACK, WHITE);

        /* status text: right-aligned */
        sw = LCD_TextWidth16(opened ? STR_OPENED : STR_PENDING);   /* 实测像素宽, 不能用字节数推算 */
        LCD_ShowText16((uint16_t)(316u - sw), (uint16_t)(y + 7u),
                       opened ? STR_OPENED : STR_PENDING,
                       opened ? GREEN : DGRAY, WHITE);        /* 近读用: 绿/深灰, 与状态灯的高对比配色不同 */
        y += BOX_ROW_H;
    }
    /* 列表收尾分隔线: 位置由布局常量推出(=398), 与下方操作提示区分开 */
    LCD_DrawLine(4u, (uint16_t)(MAIN_BOX_Y0 + 7u * BOX_ROW_H - 2u),
                 316u, (uint16_t)(MAIN_BOX_Y0 + 7u * BOX_ROW_H - 2u), LGRAY);
}

/**
  * @brief  绘制主界面底部的两行操作说明
  * @param  无
  * @return 无
  *
  * 屏幕区域: 先清 x 4~316, y 410~475 的矩形, 再在 y=408 处画一条浅灰分隔线,
  *   然后在 y=416 与 y=442 处居中输出两行深灰文字:
  *     第一行 STR_HINT_MAIN("按[设置]进入设置")  —— 告诉用户唯一能按的键
  *     第二行 STR_HINT_TIME("按时吃药自动停止")  —— 对设备行为的安全说明
  *   两行都是 DGRAY 而不是纯黑: 这是"说明文字"而非"数据", 视觉上主动弱化,
  *   不跟上面的时钟和药盒列表抢注意力。
  *
  * 【为什么主界面的提示不放在 DrawHintBar() 那条 440~479 的灰带里】
  *   主界面要同时显示两条提示(按键说明 + 行为说明), 而 DrawHintBar 只容得下一行;
  *   而且主界面是常驻信息页, 不该用"设置界面"那种灰底按键栏的视觉风格。
  *   因此主界面自己画白底灰字的两行, 三个设置界面才用 DrawHintBar。
  *
  * 【分隔线画在 y=408, 清底从 410 开始, 顺序不能颠倒】
  *   代码先 FillRect(410 起)再 DrawLine(408): 两者 y 范围不重叠, 所以顺序其实无关;
  *   但**清底范围必须从 410 开始而不是 408**, 否则会把刚画好的分隔线擦掉。
  *   改动坐标时请保持"清底区不覆盖分隔线"这一约束。
  */
static void Main_DrawHint(void)
{
    LCD_FillRect(4u, 410u, 316u, 475u, WHITE);          /* 清底区从 410 起, 不能盖到 408 的分隔线 */
    LCD_DrawLine(4u, 408u, 316u, 408u, LGRAY);
    DrawTextCentered16(416u, STR_HINT_MAIN, DGRAY, WHITE);
    DrawTextCentered16(442u, STR_HINT_TIME, DGRAY, WHITE);
}

/**
  * @brief  主界面整屏重绘: 清屏后按分区顺序重画全部内容, 并复位所有影子变量
  * @param  无
  * @return 无
  *
  * 【影子变量复位的必要性 —— 本函数最容易看漏的一段】
  *   重画之前先把 g_LastLink / g_LastDateSig / g_LastSec 置成"不可能的值"、
  *   把 g_BoxDirty 置 1。如果不做这一步, 就会出现这种尴尬情况:
  *   本函数已经无条件把状态栏/日期/时钟/药盒区都画到屏上了, 但影子变量里记的
  *   还是"重画之前"的旧值; 等 Menu_Tick() 下次比对时, 真实值和影子不等,
  *   于是又把同样的内容原样重画一遍 —— 白费一次 FSMC 带宽。
  *   置成异常值后, 影子与"刚画上去的内容"在逻辑上重新同步, 后续比对才准确。
  *   (另一种写法是在每个 Main_DrawXxx 里更新自己的影子, 但那样会让
  *    "绘制函数"同时承担"状态管理"职责, 这里选择了职责更清晰的集中复位。)
  *
  * 【调用时机】共三处: Menu_Init() 上电首帧、EnterScreen(SCR_MAIN) 从设置界面返回主界面、
  *   Menu_RefreshMain()(menu.h 公开接口, 供 app.c 在药盒复位等场合强制刷新)。
  *
  * 【绘制顺序】就是从上到下的屏幕顺序: 标题 -> 状态 -> 日期 -> 时钟 -> 蜂鸣行 ->
  *   药盒列表 -> 底部提示。各分区矩形互不重叠, 因此顺序对结果无影响,
  *   但保持"自上而下"便于和屏幕布局对照阅读。
  */
static void Main_RedrawAll(void)
{
    LCD_Clear(WHITE);        /* 唯一一处整屏清屏: 只在进入主界面时做一次, 不在 1Hz 刷新路径上 */
    Main_DrawTitle();
    /* 影子变量复位, 与下面刚画上去的内容重新同步, 避免 Menu_Tick 再重复画一遍 */
    g_LastLink    = (LinkState_t)0xFF;
    g_LastDateSig = 0;
    g_LastSec     = 0xFF;
    g_BoxDirty    = 1;
    Main_DrawLinkStatus();
    Main_DrawDate();
    Main_DrawClock();
    Main_DrawBeep();
    Main_DrawBoxes();
    Main_DrawHint();
}

/* ==================== sub screens ==================== */
/*
 * 三个设置界面(药盒选择 / 星期选择 / 时间设置)中, 前两个共用下面这套"列表渲染管线":
 *   g_ItemCount / g_Items[]  由各自的 DrawXxxSel() 填好数据
 *   DrawItemList()           首帧把全部行画一遍
 *   UpdateItemSel()          之后每次上下键只重画"旧行 + 新行"两行
 * 两个界面的区别只在数据源和行数, 渲染代码完全复用, 所以上下键逻辑也只需一份。
 */

/**
  * @brief  绘制列表中的一行: 左端是行文本, 右端(仅选择界面)是该项对应的定时摘要
  * @param  idx      行下标 0 起; 同时也是 g_Items[] 与 RuleSummary/RuleDaySummary 的索引
  * @param  selected 0=普通行(黑字白底), 1=当前高亮行(白字紫底 HILITE_BG, 即反显)
  * @return 无
  *
  * 屏幕区域(横向由 menu.h 的布局常量控制, 纵向由行号推出):
  *   x: ITEM_X0(20) ~ ITEM_X1(300), 文字从 ITEM_X0+14=34 开始(左内边距 14)
  *   y: ITEM_Y0(50) + idx * ITEM_ROW_H(40), 行矩形高 ITEM_ROW_H-3=37(留 3 像素行间隙)
  *   右侧摘要文字右对齐到 ITEM_X1=300
  *
  * 【为什么右侧摘要要按 g_Scr 分支取不同的数据源】
  *   同一行矩形在两个界面里语义不同:
  *     SCR_BOX_SEL: 行内容是"药盒N", 右侧要显示**这一盒**最近一次定时 ->
  *                  用 RuleSummary(idx, ...), 跨 7 天找最近;
  *     SCR_DAY_SEL: 行内容是"星期X", 右侧要显示**当前这盒在这一天**最早的时间 ->
  *                  用 RuleDaySummary(g_SelBox, idx, ...), 只看这一天。
  *   两者参数顺序恰好相反(一个把 idx 当盒号, 一个把 idx 当天号), 读代码时别弄混。
  *
  * 【idx < BOX_NUM 这个额外条件的由来】
  *   SCR_BOX_SEL 恰好是 BOX_NUM(7) 行、SCR_DAY_SEL 恰好是 7 行, 两个数相等,
  *   所以这个判断在目前的行数下永远成立。它存在的意义是**防御性**的:
  *   一旦将来把星期选择的行数改得比 BOX_NUM 多(比如加上"工作日/周末"快捷项),
  *   越界的 idx 就不会再去索引 rules[] 数组, 而是安静地跳过摘要绘制,
  *   不至于读出一片随机数据当时间显示。
  *
  * 【先填底色再写字】局部重画(UpdateItemSel)时没有整屏底色可依靠,
  *   所以每次都必须先把这一行的矩形连同间隙一起填成目标底色, 再写文字,
  *   否则从高亮变成普通行时会残留上一帧的紫色像素。
  */
static void DrawItemRow(uint8_t idx, uint8_t selected)
{
    uint16_t y  = (uint16_t)(ITEM_Y0 + idx * ITEM_ROW_H);   /* 行顶边 y, 由布局常量推出 */
    uint16_t fg = selected ? WHITE : BLACK;                 /* 高亮 = 反显 */
    uint16_t bg = selected ? HILITE_BG : WHITE;

    LCD_FillRect(ITEM_X0, y, ITEM_X1, (uint16_t)(y + ITEM_ROW_H - 3u), bg);
    LCD_ShowText16((uint16_t)(ITEM_X0 + 14u), (uint16_t)(y + 7u), g_Items[idx], fg, bg);

    /* box-sel / day-sel: show configured time on the right */
    /* 右侧定时摘要: 药盒选择看"该盒最近", 星期选择看"该盒该天最早" */
    if (idx < BOX_NUM && (g_Scr == SCR_BOX_SEL || g_Scr == SCR_DAY_SEL))
    {
        char tbuf[16];
        uint16_t sw;

        if (g_Scr == SCR_BOX_SEL) RuleSummary((uint8_t)idx, tbuf);                 /* idx 当盒号用 */
        else                     RuleDaySummary(g_SelBox, (uint8_t)idx, tbuf);     /* idx 当天号用 */
        sw = LCD_TextWidth16(tbuf);                                                /* 实测宽度以便右对齐 */
        LCD_ShowText16((uint16_t)(ITEM_X1 - sw), (uint16_t)(y + 7u), tbuf, fg, bg);
    }
}

/**
  * @brief  把当前界面的全部列表行画一遍(进入列表界面时的首帧全量绘制)
  * @param  无(g_ItemCount 由调用方的 DrawBoxSel/DrawDaySel 预先设好)
  * @return 无
  *
  * 【与 UpdateItemSel 的分工】本函数是 O(全部行) 的全量绘制, 只在"进入界面"
  *   或"数据被清空后(按设置键清除定时)需要整体刷新"时调用; 用户上下移动光标
  *   走的是 UpdateItemSel 的 O(2 行) 局部重绘。这一分工是列表界面不闪烁、
  *   也不浪费带宽的原因。
  *
  * 【g_ItemCount 的来源必须由调用方保证】本函数自己不知道列表有几行 ——
  *   药盒选择是 BOX_NUM(7), 星期选择是 7。这是刻意的解耦: 渲染管线不关心数据语义。
  *   若调用方忘记设置, g_ItemCount 会保留上一个界面的值, 画出错误行数。
  */
static void DrawItemList(void)
{
    uint8_t i;

    for (i = 0; i < g_ItemCount; i++)
    {
        DrawItemRow(i, (i == g_ItemSel) ? 1u : 0u);   /* 只有光标所在行反显 */
    }
}

/**
  * @brief  列表光标移动: 只重画"失去高亮的旧行"和"获得高亮的新行"两行
  * @param  newSel 新的光标行下标; 调用方(Menu_OnKey 的上下键分支)已保证它落在
  *                0 ~ g_ItemCount-1 内并完成了首尾环绕
  * @return 无。副作用是更新 g_ItemSel
  *
  * 【原理: 局部重绘把一次移动的代价从 7 行降到 2 行】
  *   列表项的位置和文本在进入界面时(见 DrawBoxSel/DrawDaySel)就已经由 sprintf 生成,
  *   移动光标**不改变任何行内容**, 只改变哪一行反显。所以只需把旧行按普通样式重画、
  *   把新行按高亮样式重画, 中间那 5 行完全不用碰。在 FSMC 并口屏上,
  *   省下的这几行的像素写入量直接体现为按键响应更跟手。
  *
  * 【重画顺序: 先旧行后新行】
  *   顺序本身不影响正确性(两行矩形不重叠), 但保持"先撤销旧高亮、再打上新高亮"
  *   在阅读上更符合直觉。注意旧行被画了两次(第二次是 newSel 恰好等于旧值的情形),
  *   多一次矩形填充没有副作用, 仅为代码简洁而保留, 不做特例判断。
  *
  * 【g_ItemSel 的更新放在最后】先按"旧值"重画完旧行, 再改 g_ItemSel；
  *   若先改 g_ItemSel 再画旧行, DrawItemRow(g_ItemSel, 0) 就不是"旧行"了。
  *   这类"先读旧值后写新值"的顺序在状态机代码里很容易写反, 改动时请留意。
  *
  * 【已知限制(不改)】本函数不刷新右侧时间摘要, 所以静止不动时摘要数字不会自己更新;
  *   但上下键一移出再移回该行, 摘要就会顺带重算 —— 对本项目"看有没有设好时间"
  *   的使用场景足够, 且省掉了每次移动都重算 7 行 RuleSummary 的开销(约 196 次迭代)。
  */
static void UpdateItemSel(uint8_t newSel)
{
    DrawItemRow(g_ItemSel, 0);   /* 撤销旧行的高亮(此处 g_ItemSel 还是旧值) */
    DrawItemRow(newSel, 1);      /* 给新行打上高亮 */
    g_ItemSel = newSel;          /* 最后更新状态, 供下次移动时当"旧值"使用 */
}

/**
  * @brief  绘制药盒选择界面(SCR_BOX_SEL): 7 行"药盒1~药盒7", 每行右侧显示该盒最近一次定时
  * @param  无(光标状态取自 g_ItemSel / g_KeepItemSel)
  * @return 无
  *
  * 屏幕布局: 标题栏(y 0~39)显示 STR_BOXSEL("药盒选择"); 列表区从 ITEM_Y0(50) 起,
  *   7 行 x 每行 ITEM_ROW_H(40); 底部提示栏显示 STR_HINT_SEL("加减选择 确认进入 返回返回")。
  *
  * 【执行顺序不能乱 —— 本函数是"填数据 -> 清屏 -> 画标题 -> 画列表 -> 画提示"的模板】
  *   1) 先把 g_ItemCount 和 g_Items[] 填好。必须在 DrawItemList() 之前,
  *      否则渲染管线会拿着上一个界面的残留数据(甚至越界)去画。
  *      这里用 sprintf 把"药盒N"预生成一次存进 g_Items, 之后每次移动光标
  *      DrawItemRow 直接取用, 不必反复拼字符串 —— 这也是局部重绘能便宜的前提。
  *   2) LCD_Clear 清整屏: 从主界面(或从星期选择返回)进来时, 屏上还留着上一界面的内容。
  *   3) 最后才画列表和提示栏 —— 它们都依赖前两步的结果。
  *
  * 【注意本界面没有像 DrawDaySel 那样在标题栏左上角叠"上下文"文字】
  *   因为本界面的每一行自己就是药盒名, 上下文已经一目了然;
  *   星期选择界面才需要额外标出"正在给哪一盒选星期"。
  *
  * 【回退光标】与 DrawDaySel 同一机制: g_KeepItemSel 为 1 时保留 g_ItemSel
  *   (从星期选择界面按返回会走到这里, 把光标停在刚选过的药盒上), 用后立即清零。
  *   见 DrawDaySel 的详细说明。
  */
static void DrawBoxSel(void)
{
    uint8_t i;

    /* --- 1) 准备数据: 行数 + 每行文本 --- */
    g_ItemCount = BOX_NUM;
    if (!g_KeepItemSel) g_ItemSel = 0;   /* 非"返回"路径进入时, 光标从头开始 */
    g_KeepItemSel = 0;                   /* 一次性标志, 用后立即清零, 否则下次进入会被锁在旧位置 */
    for (i = 0; i < BOX_NUM; i++)
    {
        sprintf(g_Items[i], "%s%d", STR_BOX, (int)i + 1);   /* 内部 0~6 -> 显示 1~7 */
    }
    /* --- 2) 清屏 --- */
    LCD_Clear(WHITE);
    /* --- 3) 上屏 --- */
    DrawTitleBar(STR_BOXSEL);
    DrawItemList();
    DrawHintBar(STR_HINT_SEL);
}

/* ==================== screen switch ==================== */

/**
  * @brief  界面跳转的唯一入口: 切换 g_Scr 并整屏重画目标界面
  * @param  scr 目标界面编号(SCR_MAIN / SCR_BOX_SEL / SCR_DAY_SEL / SCR_TIME_SET)
  * @return 无
  *
  * 【为什么所有跳转都必须走这里, 而不许直接写 g_Scr = xxx】
  *   一次界面切换包含三件必须同时发生的事:
  *     1) 更新界面编号 g_Scr;
  *     2) 清掉可能还挂着的 Toast 提示 —— 否则从上一个界面带过来的"保存成功"
  *        会盖在新界面的底部提示上, 用户看到的提示与实际界面不符;
  *     3) 把新界面整屏画出来。
  *   如果允许散落地直接改 g_Scr, 迟早会漏掉后两步(尤其是第 2 步),
  *   出现"状态是 B 界面、屏幕还是 A 界面"的不一致。集中成一个函数后,
  *   这两个副作用就成了跳转的原子组成部分, 不可能漏。
  *
  * 【状态转移表(与 Menu_OnKey 中的调用点对应)】
  *   SCR_MAIN     -> SCR_BOX_SEL   : 主界面按[设置]
  *   SCR_BOX_SEL  -> SCR_MAIN      : 按[返回]
  *   SCR_BOX_SEL  -> SCR_DAY_SEL   : 按[确认](此时 g_SelBox = g_ItemSel)
  *   SCR_DAY_SEL  -> SCR_BOX_SEL   : 按[返回](带 g_KeepItemSel = 1)
  *   SCR_DAY_SEL  -> SCR_TIME_SET  : 按[确认](此时 g_SelDay = g_ItemSel, 并 RuleAutoSelect)
  *   SCR_TIME_SET -> SCR_DAY_SEL   : 在列表阶段(g_EditPhase==0)按[返回](带 g_KeepItemSel = 1)
  *   注意: 时间设置界面的调小时/调分钟两个阶段, 按返回键**不经过本函数** ——
  *   它只在同一界面内回退 g_EditPhase 并重画(见 Menu_OnKey 的返回键分支),
  *   因为此时并没有发生界面跳转。
  *
  * 【default 分支的处理】scr 若为非法值, 什么都不画, 但 g_Scr 已被改成该非法值 ——
  *   这会让 Menu_Tick 的 `if (g_Scr == SCR_MAIN)` 判断为假, 主界面自动刷新停止,
  *   屏幕保持不动。SC 枚举只有 4 个合法值, 实际不会走到这里。
  */
static void EnterScreen(ScrId_t scr)
{
    g_Scr = scr;            /* 先改状态: 后面调用的绘制函数会读 g_Scr 决定右侧摘要取哪种数据 */
    g_ToastActive = 0;      /* 跨界面不允许残留 Toast, 见函数注释第 2 条 */

    switch (scr)
    {
    case SCR_MAIN:     Main_RedrawAll(); break;
    case SCR_BOX_SEL:  DrawBoxSel();     break;
    case SCR_DAY_SEL:  DrawDaySel();       break;
    case SCR_TIME_SET: DrawRuleEdit();       break;
    default: break;         /* 非法界面号: 只改状态不绘制, 刷新随之停摆 */
    }
}

/* ==================== public interface ==================== */

void Menu_Init(void)
{
    g_Scr         = SCR_MAIN;
    g_ItemSel     = 0;
    g_SelBox      = 0;
    g_SelDay      = 0;
    g_TiSel       = 0;
    g_EditPhase   = 0;
    g_EditH       = 8;
    g_EditM       = 0;
    g_LastDateSig = 0;
    g_LastSec     = 0xFF;
    g_LastLink    = (LinkState_t)0xFF;
    g_BoxDirty    = 1;
    g_MainMsgActive = 0;
    g_ToastActive   = 0;
    Main_RedrawAll();
}

void Menu_OnKey(uint16_t keys)
{
    uint8_t up   = (keys & KEY_EVT_BIT(KEY_UP))   ? 1u : 0u;
    uint8_t down = (keys & KEY_EVT_BIT(KEY_DOWN)) ? 1u : 0u;

    /* ---- main ---- */
    if (g_Scr == SCR_MAIN)
    {
        if (keys & KEY_EVT_BIT(KEY_SET)) EnterScreen(SCR_BOX_SEL);
        return;
    }


    /* ---- set: clear current item ---- */
    if (keys & KEY_EVT_BIT(KEY_SET))
    {
        switch (g_Scr)
        {
        case SCR_BOX_SEL:
            memset(g_Cfg.rules[g_ItemSel].hour, 0xFF, sizeof(g_Cfg.rules[g_ItemSel].hour));
            memset(g_Cfg.rules[g_ItemSel].min, 0, sizeof(g_Cfg.rules[g_ItemSel].min));
            if (Flash_SaveConfig(&g_Cfg)) { Menu_ShowToast(STR_SAVED); MQTT_PublishSchedule(); }
            DrawItemList();
            break;

        case SCR_DAY_SEL:
            memset(g_Cfg.rules[g_SelBox].hour[g_ItemSel], 0xFF, BOX_MAX_TIMES);
            memset(g_Cfg.rules[g_SelBox].min[g_ItemSel], 0, BOX_MAX_TIMES);
            if (Flash_SaveConfig(&g_Cfg)) { Menu_ShowToast(STR_SAVED); MQTT_PublishSchedule(); }
            DrawItemList();
            break;

        case SCR_TIME_SET:
            if (g_EditPhase == 0u)
            {
                g_Cfg.rules[g_SelBox].hour[g_SelDay][g_TiSel] = 0xFFu;
                g_Cfg.rules[g_SelBox].min[g_SelDay][g_TiSel]  = 0u;
                if (Flash_SaveConfig(&g_Cfg)) { Menu_ShowToast(STR_SAVED); MQTT_PublishSchedule(); }
                DrawRuleEdit();
            }
            break;

        default: break;
        }
        return;
    }

    /* ---- back ---- */
    if (keys & KEY_EVT_BIT(KEY_BACK))
    {
        switch (g_Scr)
        {
        case SCR_BOX_SEL:  EnterScreen(SCR_MAIN);    break;
        case SCR_DAY_SEL:  g_KeepItemSel = 1; g_ItemSel = g_SelBox; EnterScreen(SCR_BOX_SEL); break;
        case SCR_TIME_SET:
            if (g_EditPhase == 2u) { g_EditPhase = 1u; DrawRuleEdit(); }
            else if (g_EditPhase == 1u) { g_EditPhase = 0u; DrawRuleEdit(); }
            else { g_KeepItemSel = 1; g_ItemSel = g_SelDay; EnterScreen(SCR_DAY_SEL); }
            break;
        default: break;
        }
        return;
    }

    /* ---- confirm ---- */
    if (keys & KEY_EVT_BIT(KEY_OK))
    {
        switch (g_Scr)
        {
        case SCR_BOX_SEL:
            g_SelBox = g_ItemSel;
            g_TiSel = 0; g_EditPhase = 0;
            EnterScreen(SCR_DAY_SEL);
            break;

        case SCR_DAY_SEL:
            g_SelDay = g_ItemSel;
            RuleAutoSelect(g_SelBox, g_SelDay);
            g_EditPhase = 0;
            EnterScreen(SCR_TIME_SET);
            break;

        case SCR_TIME_SET:
            if (g_EditPhase == 0u)
            {
                /* enter hour edit for the selected slot */
                const BoxRule_t *r = &g_Cfg.rules[g_SelBox];
                g_EditH = (r->hour[g_SelDay][g_TiSel] == 0xFFu) ? 8u : r->hour[g_SelDay][g_TiSel];
                g_EditM = (r->hour[g_SelDay][g_TiSel] == 0xFFu) ? 0u : r->min[g_SelDay][g_TiSel];
                g_EditPhase = 1u;
            }
            else if (g_EditPhase == 1u)
            {
                g_EditPhase = 2u;      /* hour ok -> minute */
            }
            else
            {
                /* minute ok -> save slot */
                g_Cfg.rules[g_SelBox].hour[g_SelDay][g_TiSel] = g_EditH;
                g_Cfg.rules[g_SelBox].min[g_SelDay][g_TiSel]  = g_EditM;
                RuleSortDay(g_SelBox, g_SelDay);
                if (Flash_SaveConfig(&g_Cfg)) { Menu_ShowToast(STR_SAVED); MQTT_PublishSchedule(); }
                else                          Menu_ShowToast(STR_SAVEFAIL);
                g_EditPhase = 0u;
                RuleAutoSelect(g_SelBox, g_SelDay);
            }
            DrawRuleEdit();
            break;

        default: break;
        }
        return;
    }

    /* ---- up/down ---- */
    if (up || down)
    {
        int8_t dir = up ? 1 : -1;

        switch (g_Scr)
        {
        case SCR_BOX_SEL:
        case SCR_DAY_SEL:
        {
            int16_t n = (int16_t)g_ItemSel + dir;
            if (n < 0)               n = (int16_t)g_ItemCount - 1;
            if (n >= g_ItemCount)    n = 0;
            UpdateItemSel((uint8_t)n);
            break;
        }

        case SCR_TIME_SET:
            if (g_EditPhase == 0u)
            {
                /* move among SET slots + first unused slot */
                uint8_t next = g_TiSel, step, firstUnused = 0xFF;
                const uint8_t *hrs = g_Cfg.rules[g_SelBox].hour[g_SelDay];
                {
                    uint8_t t2;
                    for (t2 = 0; t2 < BOX_MAX_TIMES; t2++)
                        if (hrs[t2] == 0xFFu) { firstUnused = t2; break; }
                }
                for (step = 1; step <= BOX_MAX_TIMES; step++)
                {
                    next = (uint8_t)((next + dir + BOX_MAX_TIMES) % BOX_MAX_TIMES);
                    if (hrs[next] != 0xFFu || next == firstUnused) { g_TiSel = next; break; }
                }
            }
            else if (g_EditPhase == 1u)
            {
                g_EditH = (uint8_t)(((int)g_EditH + dir + 24) % 24);
            }
            else
            {
                g_EditM = (uint8_t)(((int)g_EditM + dir + 60) % 60);
            }
            DrawRuleEdit();
            break;

        default: break;
        }
    }
}

void Menu_Tick(void)
{
    uint32_t now = Timer_GetTick();

    if (g_ToastActive && (int32_t)(now - g_ToastEnd) >= 0)
    {
        g_ToastActive = 0;
        if (g_Scr != SCR_MAIN)
        {
            DrawHintBar(g_Scr == SCR_BOX_SEL || g_Scr == SCR_DAY_SEL ?
                        STR_HINT_SEL : STR_HINT_SET);
        }
    }

    if (g_Scr == SCR_MAIN)
    {
        RtcTime_t t;
        uint32_t dateSig;
        LinkState_t link;

        RTC_GetTime(&t);
        dateSig = (uint32_t)t.year * 10000u + (uint32_t)t.month * 100u + (uint32_t)t.day;
        link    = MQTT_GetLinkState();

        if (dateSig != g_LastDateSig) { g_LastDateSig = dateSig; Main_DrawDate(); }
        if (t.second != g_LastSec)
        {
            g_LastSec = t.second;
            Main_DrawClock();
            Main_DrawBeep();
        }
        if (link != g_LastLink) { g_LastLink = link; Main_DrawLinkStatus(); }
        if (g_BoxDirty)         { g_BoxDirty = 0;   Main_DrawBoxes(); }
    }
}

void Menu_RefreshMain(void)
{
    if (g_Scr == SCR_MAIN)
    {
        Main_RedrawAll();
    }
}

void Menu_NotifyBoxChange(void)
{
    g_BoxDirty = 1;
}

void Menu_ShowMainMsg(const char *str)
{
    strncpy(g_MainMsg, str, 31);
    g_MainMsg[31] = 0;
    g_MainMsgActive = 1;
    g_MainMsgEnd = Timer_GetTick() + MAIN_MSG_TIME_MS;
}

uint8_t Menu_IsInMain(void)
{
    return (g_Scr == SCR_MAIN);
}

void Menu_ShowToast(const char *str)
{
    strncpy(g_Toast, str, 23);
    g_Toast[23] = 0;
    g_ToastActive = 1;
    g_ToastEnd = Timer_GetTick() + TOAST_TIME_MS;
    if (g_Scr != SCR_MAIN)
    {
        DrawHintBar(NULL);
    }
}
