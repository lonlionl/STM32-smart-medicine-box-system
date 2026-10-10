/**
  ******************************************************************************
  * @file    key.h
  * @brief   按键模块（5 键，消抖 + 长按连发）—— 接口声明
  ******************************************************************************
  * 模块职责：
  *   扫描板上 5 个独立按键，做 20ms 软件消抖，识别"按下事件"和"长按连发事件"，
  *   把结果打包成一个位掩码返回给调用者；模块本身不关心按键代表什么业务含义，
  *   也不直接操作界面——业务解释全部交给 menu.c。
  *
  * 与谁交互：
  *   - 上层：main.c 主循环每轮调用 Key_Scan()，把返回值原样交给 Menu_OnKey()；
  *           menu.c 用 (keys & KEY_EVT_BIT(KEY_XXX)) 逐位判断该做什么。
  *   - 下层：GPIOF 的 PF0~PF4（内部上拉输入）+ timer.c 的 Timer_GetTick() 1ms 时基。
  *
  * 典型调用顺序：
  *   Timer_Init()（必须有 1ms 滴答，否则消抖/长按时序全错）
  *   → Key_Init()（配 GPIO）
  *   → 主循环 while(1) { uint16_t keys = Key_Scan(); if (keys) Menu_OnKey(keys); }
  *   调用周期要求：不超过半个消抖周期（10ms 左右）且不得含有阻塞延时，
  *   否则短按可能被跨过而漏检。本项目主循环无任何 delay，天然满足。
  *
  * 返回值形式（很重要，容易误解）：
  *   Key_Scan() 返回的是【位掩码】而不是"键值"：
  *     位 i 为 1 表示"第 i 号键本轮有事件"，bit0=KEY_SET … bit4=KEY_DOWN，
  *   可以同时有多个位为 1（例如双手同时按 SET 和 OK 会返回两位置位），
  *   但同一个键同一轮只会有一位。调用者必须用按位与判断，不能拿返回值做 == 比较。
  *
  * 事件产生规则（详见 key.c）：
  *   ① 短按：电平稳定 KEY_DEBOUNCE_MS(20ms) 后，在"按下沿"产生 1 次事件；
  *   ② 连发：KEY_UP / KEY_DOWN 按住超过 KEY_LONG_PRESS_MS(500ms) 后，
  *      每 KEY_REPEAT_MS(100ms) 追加 1 次事件（用于连续调节数值）；
  *   ③ 松开不产生事件（本模块没有"抬起/长按结束"事件）。
  ******************************************************************************
  * Pin mapping (low-level active, internal pull-up):
  *   - KEY_SET  : PF0  setting
  *   - KEY_OK   : PF1  confirm
  *   - KEY_BACK : PF4  back (added at the end)
  *   - KEY_UP   : PF2  plus
  *   - KEY_DOWN : PF3  minus
  * Modify KEY_*_PORT / KEY_*_PIN below to change mapping.
  ******************************************************************************
  */
#ifndef __KEY_H__
#define __KEY_H__

#include "stm32f10x.h"

/* Debounce and auto-repeat timing (ms) */
/* 消抖时间(ms)：机械触点抖动通常持续 5~15ms，取 20ms 可覆盖绝大多数轻触开关，
   同时短于人的连续两次有意按键间隔(>100ms)，不会把两次真按键合并成一次。
   该值同时是"扫描周期上限"的约束来源：主循环必须比它快得多。 */
#define KEY_DEBOUNCE_MS    20u      /* debounce time */
/* 长按判定阈值(ms)：按住达到 500ms 才认为用户想"连续调节"，
   取值兼顾两端——太短会把普通短按误判成长按，太长则用户会觉得连发"来得很慢"。 */
#define KEY_LONG_PRESS_MS  500u     /* long-press threshold */
/* 连发间隔(ms)：进入长按状态后每 100ms 追加一次事件，
   对应约 10 次/秒的调节速度，既能快速跨越数值范围又不会跳得看不清。 */
#define KEY_REPEAT_MS      100u     /* auto-repeat interval */

/* Key IDs (physical mapping via g_Keys array in key.c) */
/* 逻辑键号：同时充当"位掩码中的位序号"和 g_Keys[] 数组下标。
   因此顺序一旦调整，位含义与硬件映射会一起变，改动时必须同步检查 key.c 的
   g_Keys[] 初始化顺序和 menu.c 里各 KEY_EVT_BIT(KEY_XXX) 的判断。 */
typedef enum
{
    KEY_SET  = 0,   /* setting */
    KEY_OK   = 1,   /* confirm */
    KEY_BACK = 2,   /* back */
    KEY_UP   = 3,   /* plus */
    KEY_DOWN = 4,   /* minus */
    KEY_NUM  = 5    /* 键数量：用作数组长度与扫描循环上界，不是有效键号 */
} KeyId_t;

/* Key event bit mask */
/* 作用：把逻辑键号 id 换算成事件位掩码中的那一位。
   原理：1u << id，id 取值 0~4，结果落在 uint16_t 低 5 位。
   典型用法：if (keys & KEY_EVT_BIT(KEY_OK)) { ...确认... } */
#define KEY_EVT_BIT(id) ((uint16_t)(1u << (id)))
/* plus/minus mask for simultaneous auto-repeat */
/* 加/减两键的位掩码合集（KEY_UP|KEY_DOWN = 0x18）。
   设计意图：供"同时按住加减键做特殊操作"之类的判断使用；
   注意 key.c 目前的连发逻辑是逐键判断的，并未使用这个掩码，
   即本项目中它是预留常量，没有任何调用点。 */
#define KEY_MASK_UP_DOWN (KEY_EVT_BIT(KEY_UP) | KEY_EVT_BIT(KEY_DOWN))

/* Init button GPIO (internal pull-up) */
/* 作用：初始化 PF0~PF4 为带上拉的输入，使按键"按下=低电平"。
   参数：无。返回：无。
   原理：内部上拉(IPU)在按键松开时把引脚拉到高电平，按下时被按键短接到 GND
   成为低电平，因此"低电平有效"，无需外部上拉电阻。
   注意：只开 GPIOF 时钟并配置引脚，不打开任何中断——按键完全靠主循环轮询。
   调用时机：Timer_Init() 之后、主循环之前调用一次。 */
void Key_Init(void);

/* Scan buttons, return event bit mask; one event per press */
/* 作用：扫描 5 个按键，完成消抖与长按连发判定。
   参数：无。
   返回：事件位掩码（uint16_t）。bit0=SET, bit1=OK, bit2=BACK, bit3=UP, bit4=DOWN；
         0 表示本轮无任何事件；多键同时按下时会有多位同时为 1。
         注意：长按连发期间 UP/DOWN 对应的位会每 100ms 重复置位，
         所以"一次按下"可能产生多次事件，调用者要按"可重复"来设计处理逻辑。
   原理：见 key.c 中 Key_Scan() 的逐行注释（两级状态：原始电平 lastRaw +
         消抖后电平 stable，用 1ms 滴答做时间比较，全部为非阻塞比较运算）。
   调用要求：必须周期调用（本项目由主循环无延时地每轮调用），
         且调用周期要明显小于 KEY_DEBOUNCE_MS，否则可能整个按下过程都没被采到。 */
uint16_t Key_Scan(void);

#endif /* __KEY_H__ */
