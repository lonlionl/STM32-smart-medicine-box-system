/**
  ******************************************************************************
  * @file    key.c
  * @brief   按键模块实现：5 键轮询扫描 + 20ms 软件消抖 + 长按连发
  ******************************************************************************
  * 【本文件提供什么】
  *   - Key_Init()  把 PF0~PF4 配成内部上拉输入（按下=低电平）
  *   - Key_Scan()  每次调用扫描全部 5 键，返回"本轮事件位掩码"
  *   文件私有的 KeyDev_t 结构体保存每个键的消抖状态与计时戳。
  *
  * 【与谁交互】
  *   - 上层：main.c 主循环每轮调用 Key_Scan()，把结果交给 menu.c 的 Menu_OnKey()；
  *           menu.c 用按位与逐位取事件。本模块不碰界面、不碰业务，只报"哪个键有事件"。
  *   - 下层：GPIOF(PF0~PF4) 读电平；timer.c 的 Timer_GetTick() 提供 1ms 时间戳。
  *
  * 【算法原理：两层电平状态 + 时间戳比较（全程非阻塞）】
  *   每个键维护两个电平：
  *     lastRaw —— 上一次读到的原始电平（1=按下），只要和本次不同就更新它并记时刻；
  *     stable  —— 已确认稳定的电平，只有"原始电平保持不变且已持续 KEY_DEBOUNCE_MS"
  *                时才跟随更新，并在"变成按下"的这一刻产生一次事件。
  *   于是抖动（几十微秒到几毫秒的反复跳变）会把 changeTick 不断刷新，
  *   stable 永远不会被带着跳，从而实现消抖。
  *
  *   连发（只对加/减键）：stable 已经是按下，且"从按下那一刻起已经超过
  *   KEY_LONG_PRESS_MS(500ms)"，则每 KEY_REPEAT_MS(100ms) 再产生一次事件；
  *   松开时 stable 变 0，长按状态自然结束，不需要额外的状态机。
  *
  * 【为什么不用延时消抖（如 delay_ms(20) 后再读一次）】
  *   本系统是单主循环裸机：舵机、LCD 刷新、ESP/MQTT 状态机全靠主循环轮转推进，
  *   任何 delay 都会让另外三件事一起卡住（例如 MQTT 心跳/AT 超时判断被拖偏）。
  *   本模块把"时间"交给 1ms 滴答做比较，函数本身微秒级返回，因此可以每轮都调用。
  *
  * 【时间比较的写法】统一用 (uint32_t)(now - tick) >= 阈值，而不是 now >= tick + 阈值：
  *   前者天然处理 Timer_GetTick() 的 49.7 天回绕（无符号减法回绕后差值依然正确），
  *   后者在回绕点会漏判或误判，属于长期运行的隐性故障。
  *
  * 【返回值语义（与 key.h 一致）】
  *   Key_Scan() 返回位掩码：bit0=SET bit1=OK bit2=BACK bit3=UP bit4=DOWN，
  *   0 表示本轮无事件；多键同时按下会同时置多个位；
  *   长按连发期间同一按键位会每 100ms 重复出现。
  *
  * 【典型调用顺序】
  *   Timer_Init() → Key_Init() → 主循环 { uint16_t keys = Key_Scan(); if(keys) Menu_OnKey(keys); }
  ******************************************************************************
  */
#include "key.h"
#include "timer.h"

/* Button state structure */
/* 单个按键的运行时状态。一个键一份，共 5 份（静态数组 g_Keys），
   全部是普通变量，无动态分配——本项目堆已在链接脚本/启动文件里被移除。 */
typedef struct
{
    GPIO_TypeDef *port;      /* GPIO port */
    uint16_t      pin;       /* pin */
    uint8_t       lastRaw;   /* 上一次读到的原始电平：1=按下(引脚为低) 0=松开 */
    uint8_t       stable;    /* 消抖后确认的电平：1=稳定按下 0=稳定松开 */
    uint32_t      changeTick;/* 原始电平最后一次变化的时刻(ms)，消抖窗口的起点 */
    uint32_t      pressTick; /* 本次稳定按下的时刻(ms)；0 表示当前没有按住 */
    uint32_t      lastRepeatTick; /* 最近一次连发事件的时刻(ms)，用于控制连发节奏 */
} KeyDev_t;

/* Buttons mapped by KeyId:
   KEY_SET=PF0, KEY_OK=PF1, KEY_BACK=PF4, KEY_UP=PF2, KEY_DOWN=PF3 */
/* 按键表：数组下标 == KeyId_t 的枚举值（下标 0=KEY_SET … 4=KEY_DOWN），
   所以 Key_Scan() 里可以直接用下标 i 去生成事件位 KEY_EVT_BIT(i)。
   注意这种"下标与枚举隐式一致"的约定是脆弱的：若调整键号顺序，
   必须同时调整本表顺序，否则事件位会与实际引脚错位。
   为什么 BACK 排在数组第 3 项却是 PF4：把 SET/OK/BACK 三个"主操作键"的键号
   连续编号（0/1/2）便于 menu.c 中批量判断，硬件引脚顺序无关紧要，
   引脚由本表的 port/pin 字段决定，与键号解耦。
   结构体后 5 个字段初值全 0：开机时"未按下、无历史时刻"，语义正确。 */
static KeyDev_t g_Keys[KEY_NUM] =
{
    { GPIOF, GPIO_Pin_0, 0, 0, 0, 0, 0 },   /* KEY_SET  */
    { GPIOF, GPIO_Pin_1, 0, 0, 0, 0, 0 },   /* KEY_OK   */
    { GPIOF, GPIO_Pin_4, 0, 0, 0, 0, 0 },   /* KEY_BACK */
    { GPIOF, GPIO_Pin_2, 0, 0, 0, 0, 0 },   /* KEY_UP   */
    { GPIOF, GPIO_Pin_3, 0, 0, 0, 0, 0 }    /* KEY_DOWN */
};

/**
  * @brief  Init button GPIO: PF0~PF4 as input pull-up
  * @note   作用：把 5 个按键引脚一次性配成"内部上拉输入"。
  *         参数：无。返回：无。
  *         原理：内部上拉(IPU)让引脚在按键松开时被内部电阻拉到高电平，按下时
  *         被按键直接短接到 GND 变成低电平 —— 即"低电平有效"，无需外部上拉电阻。
  *         5 个引脚用同一次 GPIO_Init 批量配置，因为它们的模式完全相同。
  *         调用时机：Timer_Init() 之后、进入主循环之前调用一次即可（幂等，重复调用无害）。
  *         注意：GPIOF 挂在 APB2，必须先开时钟；本模块不使用任何中断，
  *         按键完全由主循环轮询，少一个中断源就少一处并发风险。
  */
void Key_Init(void)
{
    GPIO_InitTypeDef gpio;

    /* 先开 GPIOF 时钟：时钟不开时对 GPIOF 寄存器的写入不会生效。 */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOF, ENABLE);

    /* 5 个引脚一次配完；IPU=Input Pull-Up（内部上拉输入）。 */
    gpio.GPIO_Pin  = GPIO_Pin_0 | GPIO_Pin_1 | GPIO_Pin_2 | GPIO_Pin_3 | GPIO_Pin_4;
    gpio.GPIO_Mode = GPIO_Mode_IPU;      /* pull-up, low active when pressed */
    /* 输入模式下 GPIO_Speed 对电气特性无实际影响，仅为满足库结构体赋值要求
       （库函数会检查该字段，给一个合法值即可）。 */
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOF, &gpio);
}

/**
  * @brief  Scan buttons, return event bit mask; one event per press
  * @note   作用：扫描 5 个按键，完成消抖与长按连发判定，返回本轮事件位掩码。
  *         参数：无。
  *         返回：uint16_t 位掩码，bit i 对应第 i 号键（KeyId_t）；
  *               0 = 本轮无任何事件。UP/DOWN 长按期间会周期性重复置位。
  *         原理：见文件头"算法原理"。要点是"两级电平 + 时间戳比较"：
  *               原始电平变化只记录时刻，只有保持足够久才被认定为稳定电平，
  *               在"稳定电平变成按下"的那一刻产生一次事件。
  *         调用要求：必须周期性调用且周期远小于 KEY_DEBOUNCE_MS(20ms)。
  *         并发说明：本函数只读写主循环自己的数据，唯一的外部干扰是
  *               Timer_GetTick() 读的 SysTick 计数（单次 32 位读，天然原子），
  *               因此全程不需要关中断/临界区。
  */
uint16_t Key_Scan(void)
{
    uint16_t evt = 0;
    /* 本轮统一取一次时间戳：5 个键共用同一时刻，保证同一轮内的判定基准一致；
       整个扫描只有几十条指令（微秒级），这点时间差对 20ms/500ms 的判定无影响。 */
    uint32_t now = Timer_GetTick();
    uint8_t  i;

    for (i = 0; i < KEY_NUM; i++)
    {
        KeyDev_t *k = &g_Keys[i];
        /* 读引脚并转成"1=按下"的直观语义：按键低电平有效，所以读到 Bit_RESET(低)
           记为按下。集中在这一行做电平取反，后面所有逻辑都按"1=按下"思考，避免混乱。 */
        uint8_t raw = (GPIO_ReadInputDataBit(k->port, k->pin) == Bit_RESET) ? 1u : 0u;

        /* debounce: level change must be stable for KEY_DEBOUNCE_MS */
        /* 分支一：原始电平与上次不同 —— 视为"可能刚开始抖动/刚被按下"，
           只记录新电平和时刻，绝不立即改变 stable，消抖窗口从这一刻重新计时。 */
        if (raw != k->lastRaw)
        {
            k->lastRaw    = raw;
            k->changeTick = now;
        }
        /* 分支二：原始电平已连续保持（raw == lastRaw），但还没被确认为稳定电平。
           这就是"抖动已经过去"的候选状态，需要用时间戳再确认一次。 */
        else if (raw != k->stable)
        {
            /* 保持时间达到 KEY_DEBOUNCE_MS 才承认电平真的稳定了。
               用 (now - changeTick) 差值比较而非绝对时刻比较，可安全跨过
               Timer_GetTick() 的 49.7 天回绕。 */
            if ((uint32_t)(now - k->changeTick) >= KEY_DEBOUNCE_MS)
            {
                k->stable = raw;
                if (raw)
                {
                    /* 稳定按下：产生 1 次事件（短按事件就在这里）。
                       同时记下按下时刻与"上次连发时刻"，两者都从按下这一瞬间起算，
                       这样长按后第一次连发要等满 KEY_LONG_PRESS_MS 而不是立刻连发。 */
                    evt |= KEY_EVT_BIT(i);
                    k->pressTick      = now;
                    k->lastRepeatTick = now;
                }
                else
                {
                    /* 稳定松开：清 pressTick 表示"没有键被按住了"，
                       连发逻辑因此不会在松手后继续补事件。
                       注意松开本身不产生事件（本模块只有"按下类"事件）。 */
                    k->pressTick = 0;
                }
            }
        }

        /* auto-repeat for up/down on long press */
        /* 只有加/减键支持连发：这两个键用于连续调节时间/数值，
           长按连发能大幅减少按键次数；SET/OK/BACK 是"一次性动作"，
           连发会导致误操作（例如长按 OK 反复确认），所以故意不开放。 */
        if ((i == KEY_UP || i == KEY_DOWN) && k->stable)
        {
            /* 判长按：从稳定按下那一刻起算已超过 500ms 才进入连发状态。
               这里依赖 i（数组下标）与枚举值 KEY_UP/KEY_DOWN 数值一致。 */
            if ((uint32_t)(now - k->pressTick) >= KEY_LONG_PRESS_MS)
            {
                /* 连发节奏控制：距上次事件满 100ms 才再产生一次，
                   因此长按期间事件频率恒为 10 次/秒，与主循环执行速度无关。 */
                if ((uint32_t)(now - k->lastRepeatTick) >= KEY_REPEAT_MS)
                {
                    /* 刷新连发时刻：不刷新会变成"每到 100ms 的整数倍就补一发"，
                       在扫描周期不均匀时会连续补发多个事件。 */
                    k->lastRepeatTick = now;
                    evt |= KEY_EVT_BIT(i);
                }
            }
        }
        /* 补充说明：pressTick 为 0 时上面的减法会得到一个很大的数，看似会误判长按；
           但实际上 pressTick==0 只可能出现在"从未按下过"或"已稳定松开"两种情况，
           而这两种情况下 k->stable 都是 0，外层 if 已经把它挡掉了，所以不会误触发。 */
    }
    /* evt 每轮从 0 开始：返回的是"本轮新发生的事件"，而不是"当前按住的键"。
       因此调用者必须每轮都调用并及时处理，漏掉一轮就等于丢掉一次事件。 */
    return evt;
}
