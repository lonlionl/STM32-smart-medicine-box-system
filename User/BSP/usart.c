/**
  ******************************************************************************
  * @file    usart.c
  * @brief   串口模块实现：USART1 调试口(printf 重定向) + USART2 接 ESP-01S
  ******************************************************************************
  * 【本文件提供什么】
  *   - USART_InitAll()     一次把 USART1/USART2 的时钟、GPIO、参数、中断全部配好
  *   - Usart1_SendByte()   调试口阻塞发送 1 字节（printf 的最终出口）
  *   - Usart2_SendByte()/Usart2_SendString()  向 ESP-01S 阻塞下发 AT 指令
  *   - Usart2_RxIsr()      USART2 接收中断实体，只把字节搬进环形缓冲
  *   - Usart2_RxCount()/Usart2_RxRead()/Usart2_RxClear()  主循环侧取数据
  *
  * 【与谁交互】
  *   - 上层：stm32f10x_it.c 的 USART2_IRQHandler() 转调 Usart2_RxIsr()；
  *           esp01s_at.c 的 ESP_SendCmd()/ESP_Process() 用发送与取数接口跑 AT 状态机；
  *           mqtt_huawei.c 的 MQTT_Process() 间接触发 AT 收发；各处 printf 用调试口。
  *   - 下层：GPIOA(PA2/PA3/PA9/PA10)、GPIOB(时钟由本函数统一打开，供 LCD 等复用)、
  *           AFIO、USART1(APB2)、USART2(APB1)、NVIC。
  *   - 依赖：timer.c 的 Timer_GetTick() 提供 1ms 时基（SysTick）。
  *
  * 【环形缓冲的数据流与并发模型（本文件最需要看懂的部分）】
  *
  *      USART2 硬件 ──RXNE 中断──? Usart2_RxIsr() ──写──? g_EspRxBuf[512]
  *                                                          │  head(中断独写)
  *                                                          │  tail(主循环独写)
  *      主循环 ESP_Process() ──Usart2_RxCount/RxRead──? 取出字节送 AT 解析
  *
  *   1) 单生产者单消费者(SPSC)：head 只有中断在改，tail 只有主循环在改，
  *      两者各自单向推进，因此【全程不需要关中断/临界区】，最多读到"偏旧"的值。
  *   2) head/tail 必须用 volatile 修饰：否则编译器会把它们缓存进寄存器
  *      （对中断里被改写的变量做"值不变量"假设），导致主循环永远看到旧值。
  *   3) 判满用"牺牲一个空位"法：next == tail 即认为满，因此 512 字节实际可存 511 字节。
  *      满时【丢弃新字节】而不是覆盖最旧的字节——覆盖会把一帧 AT 应答撕成
  *      前后不搭的两段，解析出的错包比丢包更难排查；丢新字节则整帧仍在，只是少尾巴。
  *   4) 中断里必须读一次 DR（数据寄存器）来清 ORE 溢出标志。若不清：ORE 一旦置位，
  *      RXNE 中断会持续挂起（表现为中断反复进出或彻底卡死），接收链路等同于瘫痪。
  *      （本函数保留原有结构：RXNE 置位时读 DR 清 RXNE，ORE 置位时再补一次读 DR。）
  *   5) 中断只搬字节、不做解析，整条 ISR 只有几十条指令：115200 下两字节间隔约 87us，
  *      ISR 必须远小于这个时间才不会漏字节；解析放在主循环 ESP_Process() 里做，
  *      既让中断极短，也让解析可以放心调用 sprintf 等慢操作。
  *   6) USART2 中断优先级(抢占 1)高于 SysTick(0x0F)：1ms 滴答只是普通时基，
  *      被串口中断延后几十微秒无影响；反过来若串口中断被滴答或主循环拖住，
  *      ESP 连续发来的数据就会溢出丢失。
  *
  * 【典型调用顺序】
  *   NVIC_PriorityGroupConfig() → Timer_Init() → USART_InitAll() → printf(...)
  *   → ESP/MQTT 依次调用 Usart2_* 接口；主循环每轮调用 ESP_Process() 排空缓冲。
  ******************************************************************************
  */
#include "usart.h"
#include <stdio.h>

/* USART2 RX ring buffer (interrupt fill, poll drain) */
/* 接收环形缓冲本体。512B 静态数组直接占 RAM，不用 malloc（本项目堆已被链接器移除）。
   加 volatile 是因为它同时被中断和主循环访问，且中断写入的值必须真的落到内存，
   不能被编译器"优化"成寄存器副本。 */
static volatile uint8_t  g_EspRxBuf[ESP_RX_BUF_SIZE];
/* 写索引（head）：指向"下一个待写入"的位置。只有 Usart2_RxIsr() 会改它，
   所以主循环读它不需要保护；volatile 保证每次读取都真的去内存取最新值。 */
static volatile uint16_t g_EspRxHead = 0;   /* write index */
/* 读索引（tail）：指向"下一个待取出"的位置。只有主循环侧函数会改它，
   中断读它时同样不需要保护。head == tail 表示缓冲空。 */
static volatile uint16_t g_EspRxTail = 0;   /* read index */

/**
  * @brief  Init USART1 (debug) and USART2 (ESP-01S)
  *         USART2: PA2=TX(AF push-pull), PA3=RX(floating input)
  * @note   作用：初始化 USART1(调试口，printf 出口) 与 USART2(ESP-01S 口)，
  *         包含时钟、GPIO 复用、帧格式、波特率、接收中断与 NVIC 优先级。
  *         参数：无。
  *         返回：无。
  *         原理：见函数内各行注释；核心是"先时钟、再 GPIO、后外设、最后中断"。
  *         调用时机：必须在 NVIC_PriorityGroupConfig() 之后、任何 printf 与
  *         ESP 驱动动作之前调用一次（重复调用会复位 USART 并丢掉未发完的数据）。
  */
void USART_InitAll(void)
{
    GPIO_InitTypeDef  gpio;
    USART_InitTypeDef usart;
    NVIC_InitTypeDef  nvic;

    /* Enable clocks: GPIOA(USART1,USART2) GPIOB(I2C1) AFIO USART1 USART2 */
    /* 为什么先开时钟：STM32 外设寄存器在时钟关闭时写入无效（读回全 0），
       必须先使能对应总线的外设时钟，后续配置才真正生效。
       - APB2 挂 GPIOA/GPIOB/AFIO/USART1（本芯片 APB2=72MHz）
       - APB1 挂 USART2（本芯片 APB1=36MHz）
       库函数会按所挂总线频率自动计算 BRR 分频，所以两个时钟都要开对。
       GPIOB 在这里被一并打开，是因为 LCD(FSMC) 的控制脚等也用 GPIOB，
       集中打开可避免别处漏开时钟；AFIO 用于复用功能管理。 */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB |
                           RCC_APB2Periph_AFIO, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART2, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1, ENABLE);

    /* ---------- USART1: PA9=TX(AF push-pull) PA10=RX(floating input) ---------- */
    /* PA9 作 TX：必须配成"复用推挽输出"(AF_PP)，由外设而非 GPIO 数据寄存器
       驱动引脚电平；Speed=50MHz 是正点原子例程的通用取值（推挽输出无速率含义，
       但库要求给一个合法值）。 */
    gpio.GPIO_Pin   = GPIO_Pin_9;
    gpio.GPIO_Mode  = GPIO_Mode_AF_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &gpio);

    /* PA10 作 RX：配成"浮空输入"(IN_FLOATING)——接收脚不能开内部上拉/下拉，
       否则会和对方发送端的驱动电平"打架"，空闲电平由对方 TX 决定。 */
    gpio.GPIO_Pin   = GPIO_Pin_10;
    gpio.GPIO_Mode  = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOA, &gpio);

    /* 帧格式：8 位数据 + 无校验 + 1 位停止位（即 8-N-1），与 ESP-01S 出厂
       AT 固件的默认帧格式一致；波特率 115200 见 DEBUG_BAUDRATE。 */
    usart.USART_BaudRate            = DEBUG_BAUDRATE;
    usart.USART_WordLength          = USART_WordLength_8b;
    usart.USART_StopBits            = USART_StopBits_1;
    usart.USART_Parity              = USART_Parity_No;
    usart.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    /* 调试口也开接收：方便把 PC 端串口助手敲进来的字符接进板子做临时调试。 */
    usart.USART_Mode                = USART_Mode_Rx | USART_Mode_Tx;
    USART_Init(USART1, &usart);
    /* 使能 USART1。这一步必须在 printf 之前完成，否则首帧日志会丢。 */
    USART_Cmd(USART1, ENABLE);

    /* ---------- USART2: PA2=TX(AF push-pull) PA3=RX(floating input) ---------- */
    /* PA2/PA3 的配置理由与 PA9/PA10 完全相同：TX 复用推挽、RX 浮空输入。 */
    gpio.GPIO_Pin   = GPIO_Pin_2;
    gpio.GPIO_Mode  = GPIO_Mode_AF_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &gpio);

    gpio.GPIO_Pin   = GPIO_Pin_3;
    gpio.GPIO_Mode  = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOA, &gpio);

    /* 只需改波特率：usart 结构体其余字段（8-N-1/无流控/收发双向）与 USART1
       相同，直接复用上一次的赋值，避免重复代码。 */
    usart.USART_BaudRate            = ESP_BAUDRATE;
    USART_Init(USART2, &usart);

    /* Enable USART2 RX interrupt */
    /* 只开 RXNE（接收数据寄存器非空）中断：来一个字节就进一次中断搬走。
       不开 TXE 中断，因为发送走阻塞式 Usart2_SendByte，不需要异步通知；
       少一个中断源就少一处并发状态。 */
    USART_ITConfig(USART2, USART_IT_RXNE, ENABLE);
    /* 使能 USART2。注意：必须先配好中断再使能外设，避免使能瞬间就收到字节
       却因中断未配置而丢掉第一个字节。 */
    USART_Cmd(USART2, ENABLE);

    /* Configure USART2 interrupt priority */
    /* 抢占优先级 = 1（数值越小越优先）。timer.c 中 SysTick 用 NVIC_SetPriority(0x0F)
       配置为最低优先级，因此串口接收中断可以打断 SysTick 与主循环，
       保证 ESP 连续来数据时每字节都能被及时搬走而不溢出。 */
    nvic.NVIC_IRQChannel                   = USART2_IRQn;
    nvic.NVIC_IRQChannelPreemptionPriority = 1;
    /* 子优先级 = 0：同抢占级下若还有别的中断（如 EXTI），数值小的先响应。
       本系统 USART2 是唯一需要频繁响应的中断源，取 0 即可。 */
    nvic.NVIC_IRQChannelSubPriority        = 0;
    nvic.NVIC_IRQChannelCmd                = ENABLE;
    NVIC_Init(&nvic);
}

/**
  * @brief  USART1 send one byte (blocking)
  * @note   作用：向 USART1(调试口) 发送 1 字节，printf 的最终落点。
  *         参数：ch —— 待发送字节，取低 8 位。
  *         返回：无。
  *         原理：先自旋等待 TC(Transmission Complete，发送完成) 标志置位，
  *         再写 DR。等 TC 而非 TXE，意味着函数返回时该字节已完整移出移位寄存器，
  *         对调试输出这种"偶发、量小"的场景最直白可靠（代价是每字节要等满一帧）。
  *         注意：属阻塞发送，115200 下每字节约 87us，不要在实时路径里密集调用。
  */
void Usart1_SendByte(uint8_t ch)
{
    while (USART_GetFlagStatus(USART1, USART_FLAG_TC) == RESET);
    USART_SendData(USART1, ch);
}

/**
  * @brief  USART2 send one byte
  * @note   作用：向 ESP-01S 发送 1 字节。
  *         参数：ch —— 待发送字节。
  *         返回：无。
  *         原理：与 Usart1_SendByte 完全一致（等 TC 后写 DR），只是换了外设。
  *         为什么阻塞也不影响系统：AT 指令由主循环状态机分步下发，单条指令
  *         几十字节、耗时几毫秒，期间舵机由 PCA9685 硬件自行走完、界面只是
  *         晚一帧刷新，都不会出错；但绝不能放在中断里调用。
  */
void Usart2_SendByte(uint8_t ch)
{
    while (USART_GetFlagStatus(USART2, USART_FLAG_TC) == RESET);
    USART_SendData(USART2, ch);
}

/**
  * @brief  USART2 send a string
  * @note   作用：把以 '\0' 结尾的字符串逐字节发给 ESP-01S（不含结尾的 '\0'）。
  *         参数：s —— C 字符串指针，调用者保证非 NULL 且生命周期覆盖本次发送。
  *         返回：无。
  *         原理：while(*s) 边判边发，整串发完前一直阻塞。
  *         注意：本函数不自动补 "\r\n"，AT 指令的结束符由调用者提供
  *         （esp01s_at.c 中惯例是 Usart2_SendString(s) 之后再发 "\r\n"）。
  */
void Usart2_SendString(const char *s)
{
    while (*s)
    {
        Usart2_SendByte((uint8_t)*s++);
    }
}

/**
  * @brief  USART2 RX interrupt service (called from stm32f10x_it.c)
  * @note   作用：USART2 接收中断实体——把硬件收到的字节搬进环形缓冲，并清溢出标志。
  *         参数：无。返回：无。
  *         原理：中断里只做两件"必须在中断里做"的事：把 DR 里的字节抢走（不在
  *         中断里取，下一个字节到来就会覆盖 DR 造成溢出），以及清 ORE。所有
  *         解析工作都留给主循环，ISR 只有几十条指令，远小于 115200 下 87us 的
  *         字节间隔，因此不会漏字节。
  *         并发说明：本函数只写 g_EspRxHead，主循环只写 g_EspRxTail，
  *         形成单生产者单消费者模型，全程无需关中断。
  */
void Usart2_RxIsr(void)
{
    /* 判 RXNE：① 正常收到一个字节；② 发生溢出(ORE)时 RXNE 同样会被置位，
       两种情况都需要下面这次读 DR 来处理，所以判断条件是合适的。 */
    if (USART_GetITStatus(USART2, USART_IT_RXNE) != RESET)
    {
        /* 读 DR 有两个副作用：① 取出刚收到的字节；② 硬件自动清 RXNE 标志
           （若处于溢出状态，这次读 DR 也会把 ORE 一并清掉）。
           所以必须"先读进变量"，绝不能因为判断缓冲满就跳过这次读——
           跳过就等于 RXNE 永不清、中断反复触发。 */
        uint8_t ch = (uint8_t)USART_ReceiveData(USART2);
        /* 先算出写入后的新位置，再判断"满不满"：next == tail 说明再写一个字节
           就会把 tail 追上，此时缓冲已满（牺牲一个空位的经典判满法）。 */
        uint16_t next = (uint16_t)((g_EspRxHead + 1) % ESP_RX_BUF_SIZE);
        if (next != g_EspRxTail)     /* buffer not full, store */
        {
            g_EspRxBuf[g_EspRxHead] = ch;
            /* 先写数据、后更新 head：head 是主循环判断"有没有数据"的依据，
               顺序反过来会让主循环读到尚未写入的槽位（脏数据）。 */
            g_EspRxHead = next;
        }
        /* 缓冲满：本字节已从 DR 取走（避免中断风暴），但直接丢弃。
           丢弃而非覆盖的理由见文件头第 3 条。 */
    }
    /* clear overrun flag (prevent frequent interrupt) */
    /* 溢出(ORE)兜底：若上一轮 RXNE 分支未执行（例如 RXNE 已清但 ORE 仍置位），
       这里再读一次 DR 把 ORE 清掉。ORE 不清的后果是接收通道持续报错，
       ESP 数据再也进不来（现象就是"WiFi 突然哑了"），所以这步不能省。
       本分支读到的是无效数据，故意不存入缓冲以免污染 AT 应答。 */
    if (USART_GetITStatus(USART2, USART_IT_ORE) != RESET)
    {
        USART_ReceiveData(USART2);
    }
}

/**
  * @brief  Get number of unread bytes in USART2 RX buffer
  * @note   作用：查询缓冲中还有多少字节没被主循环取走。
  *         参数：无。
  *         返回：未读字节数，范围 0 ~ ESP_RX_BUF_SIZE-1；0 表示当前没有数据。
  *         原理：(head + SIZE - tail) % SIZE："加 SIZE 再取模"是为了避免无符号
  *         减法下溢（head < tail 时直接相减会得到一个巨大的数）。
  *         为什么不需要临界区：head 只被中断写、tail 只被本函数所在的主循环写，
  *         两个变量各自单向变化；即便在读取过程中被打断，最坏结果是返回一个
  *         "略偏旧"的字节数，环形结构本身不会被破坏。调用方（ESP_Process）用
  *         while(count>0) 循环排空，因此这种瞬时偏差不影响正确性。
  */
uint16_t Usart2_RxCount(void)
{
    return (uint16_t)((g_EspRxHead + ESP_RX_BUF_SIZE - g_EspRxTail) % ESP_RX_BUF_SIZE);
}

/**
  * @brief  Read one byte from USART2 RX buffer (non-blocking)
  * @note   作用：从缓冲取走 1 字节，并把 tail 前移一格。
  *         参数：无。
  *         返回：取出的字节（缓冲非空时才是刚收到的新数据）。
  *         原理：只读不判空、不等待，立刻返回，因此是纯非阻塞实现。
  *         使用约束：调用前应先用 Usart2_RxCount() 确认有数据，
  *         否则在空缓冲上调用会取到 tail 处的陈旧字节。
  *         正确用法示例（esp01s_at.c）：
  *             while (Usart2_RxCount() > 0) { uint8_t b = Usart2_RxRead(); ... }
  *         为什么不用临界区：本函数只改 tail，中断只改 head，互不干扰。
  */
uint8_t Usart2_RxRead(void)
{
    uint8_t ch = g_EspRxBuf[g_EspRxTail];
    /* 同样用"加 SIZE 再取模"实现回绕，保证 tail 永远落在 0..SIZE-1。 */
    g_EspRxTail = (uint16_t)((g_EspRxTail + 1) % ESP_RX_BUF_SIZE);
    return ch;
}

/**
  * @brief  Clear USART2 RX buffer
  * @note   作用：丢弃缓冲内所有未读数据，把 head/tail 都归零，使缓冲回到"空"状态。
  *         参数：无。返回：无。
  *         原理：直接把两个索引都写 0；因为数据区是按索引访问的，索引归零
  *         等价于"全部作废"，不需要真的擦写 512 字节 RAM。
  *         用途：AT 交互超时后重新同步、ESP 重启后丢弃残留半包，
  *         避免上一条指令的尾巴被当成下一条指令的应答（协议错位的经典来源）。
  *         注意：本函数不关中断；若此刻中断正好在写 head，可能残留 1 字节，
  *         所以只在"确认 ESP 已静默"的时机调用（如发送新指令之前）。
  */
void Usart2_RxClear(void)
{
    g_EspRxHead = 0;
    g_EspRxTail = 0;
}

/**
  * @brief  printf redirected to USART1 (requires Keil MicroLIB)
  * @note   作用：标准库 printf 的字符输出出口，把每个字符转到调试串口。
  *         参数：ch —— 待输出字符；f —— 标准库传入的流指针，本实现不用。
  *         返回：ch（按 C 库约定，返回实际写出的字符；返回 EOF 表示失败）。
  *         原理：ARM C 库在"无 OS"环境下最终都通过弱符号 fputc 输出字符，
  *         重定义它即可让 printf/sprintf 之外的输出走 USART1，无需改任何调用点。
  *         注意：必须勾选 Keil 的 Use MicroLIB，否则会链入完整 C 库；
  *         另外这是一次一字节的阻塞输出，printf 里带 %f 或长字符串会明显变慢。
  */
int fputc(int ch, FILE *f)
{
    /* 显式吃掉未使用的参数，避免编译器报 unused parameter 警告。 */
    (void)f;
    Usart1_SendByte((uint8_t)ch);
    return ch;
}
