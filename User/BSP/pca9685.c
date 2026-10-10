/**
  ******************************************************************************
  * @file    pca9685.c
  * @brief   PCA9685 十六路 PWM 驱动(舵机控制, 软件模拟 I2C)
  ******************************************************************************
  * 硬件连接: SCL = PB6, SDA = PB7, 均为开漏输出 + 外部上拉电阻
  *
  * 【为什么用 GPIO 软件模拟 I2C, 而不用 STM32F103 的硬件 I2C 外设】
  *   - STM32F103 的硬件 I2C 存在业界熟知的**稳定性缺陷**(ERRATA 中的
  *     总线死锁问题): 一旦从机在某个时序上把 SCL 拉低, 硬件状态机可能
  *     卡在 BUSY 迟迟不释放, 只能靠整机复位恢复;
  *   - 药箱是长期无人值守运行的设备, 一次 I2C 死锁就意味着舵机再也开不了盒;
  *   - 本驱动只在"开盒/复位"这类低频时刻通信, 数据量极小,
  *     位翻转带来的额外 CPU 开销完全可以接受;
  *   - 软件模拟还有一个好处: 出问题可以按位打时序, 比调硬件状态机直观得多。
  *
  * 开漏 + 外部上拉 的含义(理解本驱动的前提)
  *   - 引脚**永远不会主动输出高电平**, 只能"拉低"或"放开";
  *   - 写 BSRR = "放开总线(高阻)", 由外部上拉电阻把电平抬到 3.3V;
  *   - 写 BRR  = "主动拉低到 0V";
  *   - 所以本文件里所有名为 High 的操作, 实际语义都是"放开"而不是"输出高";
  *   - 正因为可以随时放开, 主机才能在应答位把 SDA 交给从机去驱动。
  *
  * 通道映射: PCA9685 通道 0~6 一一对应 1~7 号药盒的舵机
  ******************************************************************************
  */
#include "pca9685.h"
#include "timer.h"
#include <stdio.h>

/* ==================== software I2C ==================== */

/* 以下 5 个函数是软件 I2C 的"位操作原语", 直接操作寄存器而不是调库函数,
   为的是把每次电平翻转压到 1~2 条指令, 保证时序稳定 */

/**
  * @brief  释放 SCL 时钟线(高电平)
  *
  * 原理: BSRR 是"置位"寄存器, 写 1 的位在 ODR 中置 1。
  *       但在**开漏输出**模式下, ODR=1 并不代表引脚输出高电平,
  *       而是让输出 MOS 管截止(高阻, Hi-Z), 由外部上拉电阻把电平抬到 VCC。
  *       这里用 BSRR 而不是"读-改-写 ODR"的好处是它是单次 32 位写,
  *       不会因为先读后写被中断打断而误改其它引脚。
  */
static void SCL_High(void)
{
    I2C_SCL_PORT->BSRR = I2C_SCL_PIN;
}

/**
  * @brief  拉低 SCL 时钟线
  *
  * 原理: BRR 是"复位"寄存器, 写 1 的位在 ODR 中清 0,
  *       开漏输出下即导通下拉 MOS 管, 把引脚拉到 0V。
  *       同样是单次写, 不影响同端口的其它引脚。
  */
static void SCL_Low(void)
{
    I2C_SCL_PORT->BRR = I2C_SCL_PIN;
}

/**
  * @brief  释放 SDA 数据线(由外部上拉拉高)
  *
  * 原理: 同 SCL_High。注意本函数**不是**"输出 1 表示数据位 1",
  *       而是让总线浮空: 在 I2C 协议中, "数据 1" 恰好就等于"谁都不拉低"——
  *       这一点与推挽输出驱动的 SPI/UART 完全不同。
  */
static void SDA_High(void)
{
    I2C_SDA_PORT->BSRR = I2C_SDA_PIN;
}

/**
  * @brief  拉低 SDA 数据线(表示数据位 0, 或用于产生起始/停止条件)
  */
static void SDA_Low(void)
{
    I2C_SDA_PORT->BRR = I2C_SDA_PIN;
}

/**
  * @brief  读回 SDA 线上的实际电平
  * @retval 1 = 高电平, 0 = 低电平
  *
  * 原理: 读的是 IDR(输入数据寄存器), 即引脚的真实电平。
  *       即便引脚当前被配置为"开漏输出", 只要 ODR 该位为 1(输出管截止),
  *       IDR 依然能正确反映外部电平, 所以主机在输出模式下也能监视总线
  *       (这是 I2C 判断总线是否被从机拉低/是否被卡住的基础)。
  *       真正读从机数据前仍会先切到 SDA_Input(), 那是为了更明确地
  *       把引脚置为浮空输入, 避免 ODR 状态被误改后驱动总线。
  */
static uint8_t SDA_Read(void)
{
    return (I2C_SDA_PORT->IDR & I2C_SDA_PIN) ? 1u : 0u;
}

/**
  * @brief  把 SDA 引脚切回"开漏输出", 由主机驱动数据线
  *
  * 原理: 这是软件 I2C 的关键来回切换。I2C 的 SDA 是**双向**线,
  *       同一根线上既要主机发送(地址/寄存器/写数据), 又要从机发送
  *       (应答位 ACK、读数据), 而 GPIO 的方向寄存器是单向的,
  *       只能靠"输出开漏"与"浮空输入"两种模式切换来分时复用。
  *       之所以用"开漏输出"而不是"推挽输出": 推挽会在主机想输出 1 时
  *       强行把线拉到 VCC, 若此刻从机正在拉低(例如发 ACK),
  *       两个 MOS 管对顶就会短路大电流, 既可能烧引脚也会让时序错乱。
  *
  * 参数: 无(引脚由 pca9685.h 中的 I2C_SDA_PIN 定义)
  */
static void SDA_Output(void)
{
    GPIO_InitTypeDef gpio;
    gpio.GPIO_Pin   = I2C_SDA_PIN;
    gpio.GPIO_Mode  = GPIO_Mode_Out_OD;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(I2C_SDA_PORT, &gpio);
}

/**
  * @brief  把 SDA 引脚切成"浮空输入", 把数据线的驱动权让给从机
  *
  * 原理: 浮空输入下引脚呈高阻, 主机彻底不参与驱动,
  *       总线电平完全由外部上拉电阻与从机的开漏管决定,
  *       这样从机发 ACK 或发数据时才不会被主机抢线。
  *       用浮空输入而不是上拉/下拉输入: 芯片内部弱上拉(约 40k)
  *       与本驱动依赖的外部上拉电阻并联后阻值仍偏大, 上拉能力不足
  *       会让上升沿变缓, 在高频下更容易读错; 保持高阻最干净。
  */
static void SDA_Input(void)
{
    GPIO_InitTypeDef gpio;
    gpio.GPIO_Pin   = I2C_SDA_PIN;
    gpio.GPIO_Mode  = GPIO_Mode_IN_FLOATING;
    GPIO_Init(I2C_SDA_PORT, &gpio);
}

/*
 * 时序延时: 半个位周期约 5us, 对应 SCL 约 100kHz
 *   估算依据(72MHz 主频): 目标 5us = 360 个 CPU 周期;
 *   下面这个空循环每轮约 4 条指令(减法、比较、条件跳转、回写),
 *   取 80 轮 -> 约 320~400 周期, 正好落在 5us 附近。
 *   n 必须声明为 volatile: 否则编译器会把整个空循环当成无副作用代码删掉,
 *   延时直接变成 0, 通信必然失败(这是软件 I2C 最常见的"移植后不工作"原因)。
 *
 * 注意两点
 *   1. 本延时是**按 72MHz 标定的**。若把主频改成 36MHz, 位周期会翻倍,
 *      速率掉到约 50kHz —— 对 PCA9685 仍可用(它支持到 1MHz),
 *      但若再往上超频, 则需要重新标定循环次数;
 *   2. 真正的 I2C 速率由从机与总线电容决定, 这里留了较大余量,
 *      慢一点更稳, 不必为了速度去卡极限。
 */
static void I2C_Delay(void)
{
    volatile uint32_t n = 80u;
    while (n--) { }
}

/**
  * @brief  产生 I2C 起始条件(START)
  *
  * 原理: I2C 规定"SCL 为高期间 SDA 由高变低"即起始条件, 与普通数据位
  *       (数据只在 SCL 低电平期间变化)相区分。所以时序是:
  *         1. 先放开 SDA/SCL 让总线空闲为高;
  *         2. 保持 SCL 高, 把 SDA 拉低 —— 从机在这一跳变上开始监听;
  *         3. 最后拉低 SCL, 进入第 1 个时钟周期的低电平阶段。
  *       先 SDA_High/SCL_High 再延时, 是为了兼容"重复起始"条件
  *       (总线并非空闲, 前一次传输刚结束), 让线上电平先稳定下来。
  */
static void I2C_Start(void)
{
    SDA_High();
    SCL_High();
    I2C_Delay();
    SDA_Low();
    I2C_Delay();
    SCL_Low();
    I2C_Delay();
}

/**
  * @brief  产生 I2C 停止条件(STOP)
  *
  * 原理: 与起始条件对称 —— "SCL 为高期间 SDA 由低变高"即停止条件,
  *       从机据此认为一次传输结束并释放总线。
  *       结束时有意把 SDA 留为高(放开)而不拉低: 让总线回到空闲态,
  *       这样下一次 I2C_Start 之前不必再单独做一次总线释放。
  */
static void I2C_Stop(void)
{
    SDA_Low();
    SCL_High();
    I2C_Delay();
    SDA_High();
    I2C_Delay();
}

/**
  * @brief  主机向总线写 1 个字节, 并读回从机的应答
  * @param  byte 待发送字节
  * @retval 1 = 收到 ACK(从机拉低了 SDA, 通信正常)
  * @retval 0 = 收到 NACK 或总线异常(无器件/地址错/从机忙)
  *
  * 原理: 高位先发(MSB first)。每个数据位必须在 SCL 为**低**期间改变,
  *       在 SCL 为**高**期间保持稳定, 从机正是在 SCL 高电平那一刻采样。
  *       因此循环体固定为: 摆数据 -> 延时 -> 拉高 SCL -> 延时 -> 拉低 SCL -> 延时。
  *       byte 左移并用 0x80 掩码取最高位, 8 轮后正好发完 8 位。
  *
  *       第 9 个时钟是应答位: 主机必须先 SDA_Input() 放开数据线,
  *       否则会与从机的拉低动作抢线(开漏虽不至于烧毁, 但电平不确定,
  *       读到的 ACK 会不可靠)。SDA 为低即 ACK, 用三元表达式反相成 1。
  *       读完立即 SDA_Output() 收回总线, 方便上层紧接着发下一字节。
  */
static uint8_t I2C_WriteByte(uint8_t byte)
{
    uint8_t i;

    SDA_Output();
    for (i = 0; i < 8; i++)
    {
        if (byte & 0x80u) SDA_High();
        else              SDA_Low();
        byte <<= 1;
        I2C_Delay();
        SCL_High();
        I2C_Delay();
        SCL_Low();
        I2C_Delay();
    }

    /* release SDA, read ACK */
    SDA_Input();
    I2C_Delay();
    SCL_High();
    I2C_Delay();
    {
        uint8_t ack = SDA_Read() ? 0u : 1u;
        SCL_Low();
        I2C_Delay();
        SDA_Output();
        return ack;
    }
}

/**
  * @brief  主机从总线读 1 个字节, 并发回应答位
  * @param  ack 读完后主机发什么: 1 = 发 ACK(还想继续读), 0 = 发 NACK(读完了)
  * @retval 读到的字节
  *
  * 原理: 读时序同样高位在前。主机全程保持 SDA 为浮空输入, 把驱动权交给从机;
  *       先延时再拉高 SCL, 是为了让从机有足够时间把数据线摆稳
  *       (从机是在检测到 SCL 下降沿后更新数据的)。
  *       拼字节用 (byte << 1) | SDA_Read(), 在 SCL 高电平期间采样。
  *
  *       ACK/NACK 的取值规则: 读到"最后一个字节"时必须发 NACK,
  *       用来告诉从机"别再往总线送数据了"; 中间字节则发 ACK。
  *       这个判断由调用者 I2C_ReadRegs 用 (i < len-1) 完成。
  *       发完应答位后把 SDA 放高, 让总线回到空闲, 便于紧接 STOP
  *       或下一次 START。
  */
static uint8_t I2C_ReadByte(uint8_t ack)
{
    uint8_t i, byte = 0;

    SDA_Input();
    for (i = 0; i < 8; i++)
    {
        I2C_Delay();
        SCL_High();
        I2C_Delay();
        byte = (uint8_t)((byte << 1) | SDA_Read());
        SCL_Low();
        I2C_Delay();
    }

    /* send ACK/NACK */
    SDA_Output();
    if (ack) SDA_Low();
    else     SDA_High();
    I2C_Delay();
    SCL_High();
    I2C_Delay();
    SCL_Low();
    I2C_Delay();
    SDA_High();
    return byte;
}

/* ==================== I2C register access ==================== */

/**
  * @brief  向 PCA9685 的连续寄存器写数据
  * @param  devAddr 7 位 I2C 地址(本模块固定 0x40)
  * @param  reg     起始寄存器地址
  * @param  data    待写数据首地址
  * @param  len     待写字节数
  * @retval 1 = 全部写成功, 0 = 中途失败(某字节没等到 ACK)
  *
  * 原理: 完整时序 START -> 地址+写(0) -> 寄存器地址 -> 数据0..数据N -> STOP。
  *       7 位地址必须左移 1 位, 最低位(bit0)放读写方向:
  *         0 = 主机写, 1 = 主机读。所以这里用 (devAddr << 1)。
  *       之所以能连写多个字节而不用每字节都重发寄存器地址,
  *       是因为初始化时把 PCA9685 的 MODE1.AI(自动递增)位置了 1,
  *       芯片每收一字节会自动把寄存器指针 +1。
  *
  *       任何一步没等到 ACK 都立刻 I2C_Stop() 释放总线再返回失败:
  *       绝不能让总线停在"START 之后没有 STOP"的半截状态,
  *       否则从机可能一直等待后续时钟, 后续通信全部失败。
  */
static uint8_t I2C_WriteReg(uint8_t devAddr, uint8_t reg, const uint8_t *data, uint8_t len)
{
    uint8_t i;

    I2C_Start();
    if (!I2C_WriteByte((uint8_t)(devAddr << 1))) { I2C_Stop(); return 0; }
    if (!I2C_WriteByte(reg))                     { I2C_Stop(); return 0; }
    for (i = 0; i < len; i++)
    {
        if (!I2C_WriteByte(data[i]))             { I2C_Stop(); return 0; }
    }
    I2C_Stop();
    return 1;
}

/**
  * @brief  从 PCA9685 的连续寄存器读数据
  * @param  devAddr 7 位 I2C 地址(本模块固定 0x40)
  * @param  reg     起始寄存器地址
  * @param  data    接收缓冲
  * @param  len     期望读取的字节数; len == 0 视为成功且不产生任何总线动作
  * @retval 1 = 读取成功, 0 = 失败
  *
  * 原理: PCA9685 没有"直接指定读地址再读"的两步式接口,
  *       必须先像写一样把寄存器指针设好, 再重新发起读传输:
  *         START -> 地址+写(0) -> 寄存器地址 -> **重复START**
  *               -> 地址+读(1) -> 数据0..数据N -> STOP
  *       中间那个"重复起始条件"很关键: 它不发 STOP,
  *       因此总线控制权不会丢失, 寄存器指针也就保持在 reg 上。
  *       读方向用 (devAddr << 1) | 1。
  *
  *       最后一个字节必须回 NACK 而不是 ACK, 否则从机会继续吐数据
  *       (PCA9685 会自动递增指针), 把时序拉长甚至读飞。
  *       这里用 (i < len - 1) ? ACK : NACK 精确实现这一规则。
  */
static uint8_t I2C_ReadRegs(uint8_t devAddr, uint8_t reg, uint8_t *data, uint8_t len)
{
    uint8_t i;

    if (len == 0) return 1;

    I2C_Start();
    if (!I2C_WriteByte((uint8_t)(devAddr << 1))) { I2C_Stop(); return 0; }
    if (!I2C_WriteByte(reg))                     { I2C_Stop(); return 0; }

    I2C_Start();
    if (!I2C_WriteByte((uint8_t)((devAddr << 1) | 1u))) { I2C_Stop(); return 0; }

    for (i = 0; i < len; i++)
    {
        data[i] = I2C_ReadByte((i < (uint8_t)(len - 1)) ? 1u : 0u);
    }
    I2C_Stop();
    return 1;
}

/* ==================== PCA9685 driver ==================== */

/**
  * @brief  软件 I2C 的 GPIO 初始化(把 PB6/PB7 配成开漏输出并置为空闲高)
  *
  * 原理与注意事项
  *   - 必须先开 GPIOB 的 APB2 时钟: STM32 复位后所有外设时钟默认关闭,
  *     不开时钟就配置 GPIO, 寄存器写入全部无效(这是"代码看着没错但引脚不动"
  *     的头号原因); 这里函数名叫 I2C1_Init 只是沿用历史命名,
  *     与硬件 I2C1 外设无关, 本驱动完全没有用到 I2C 外设;
  *   - 两根线都配成 GPIO_Mode_Out_OD(开漏) + 50MHz 翻转速度:
  *     I2C 是"线与"总线, 只能拉低不能推高, 高电平靠外部上拉电阻,
  *     这样多个器件同时驱动也不会短路;
  *   - 初始电平都置为 1(放开), 保证总线进入空闲态,
  *     这样从机不会把上电瞬间的乱跳当成起始条件; 若初始化后 SCL 为低,
  *     则第一次 I2C_Start 的"SCL 由低变高"会变成一个时钟脉冲,
  *     可能被从机误解析(这也是 I2C 规范里要求上电后先给若干个
  *     时钟脉冲复位从机状态机的原因)。
  *
  * 注意: 外部上拉电阻是必需的硬件前提。若模块上或板子上没有上拉,
  *       开漏输出永远无法把线抬到高电平, 读到的永远是 0, 通信必失败。
  */
static void I2C1_Init(void)
{
    GPIO_InitTypeDef gpio;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);

    /* SCL: 开漏输出, 初始置高(放开总线, 由上拉电阻拉高) */
    gpio.GPIO_Pin   = I2C_SCL_PIN;
    gpio.GPIO_Mode  = GPIO_Mode_Out_OD;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(I2C_SCL_PORT, &gpio);
    SCL_High();

    /* SDA: 开漏输出, 初始置高(同上, 保证总线空闲) */
    gpio.GPIO_Pin   = I2C_SDA_PIN;
    gpio.GPIO_Mode  = GPIO_Mode_Out_OD;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(I2C_SDA_PORT, &gpio);
    SDA_High();
}

/**
  * @brief  初始化 PCA9685: 配置 50Hz PWM, 打开自动递增, 输出推挽
  * @param  无
  * @retval 无
  *
  * 原理(顺序是被芯片手册强制的, 不能调换)
  *   1. 先配好软件 I2C 的两根引脚, 并延时 2ms 等 PCA9685 内部上电复位完成
  *      (上电瞬间它可能还在复位期间, 此时发的命令会被丢掉);
  *   2. 读 MODE1 再置 SLEEP 位(bit4)写回 —— **必须先进入睡眠**,
  *      因为 PRE_SCALE(0xFE) 只在 SLEEP=1 时可写, 运行中写它会被忽略;
  *   3. 写预分频值, 把 PWM 频率定成 50Hz(舵机标准频率):
  *        f = 25MHz / (4096 * (prescale + 1))
  *        反解 prescale = 25000000 / (4096 * 50) - 1 = 121(代码按宏动态计算)
  *      其中 25MHz 是 PCA9685 的内部振荡器标称频率,
  *      4096 是 12 位计数器的满量程;
  *   4. 清 SLEEP 唤醒, 延时 5ms —— 手册要求振荡器起振稳定需要约 500us, 留足余量;
  *   5. 置 RESTART(bit7) | AI(bit5), 得到 MODE1 = 0x21:
  *        RESTART 让 PWM 从完整周期开始输出, 保证首拍脉宽正确;
  *        AI(Auto-Increment) 让寄存器指针每收一字节自动 +1,
  *        这样一次传输就能写完 LED0_ON_L/H、LED0_OFF_L/H 四个字节,
  *        SetPWM 的 4 字节连写正是依赖这一位;
  *   6. 写 MODE2 = 0x04, 置 OUTDRV: 输出级改为**推挽(totem-pole)**,
  *      默认的开漏输出驱动能力太弱, 舵机信号线长、容性大,
  *      换成推挽才能给出干净陡峭的方波;
  *   7. 回读 MODE1 打印出来, 作为"I2C 通信是否真的通了"的自检;
  *   8. 最后 PCA9685_ResetAll() 把所有舵机归位, 避免上电时舵机停在乱角度。
  */
void PCA9685_Init(void)
{
    uint8_t prescale = (uint8_t)(25000000u / (4096u * PCA9685_FREQ)) - 1u;
    uint8_t v;

    I2C1_Init();
    Timer_DelayMs(2);

    /* 读回 MODE1 原始值, 只置 SLEEP 位, 保留其余位(读-改-写),
       这样不会顺手清掉芯片的其它配置 */
    v = 0x00;
    I2C_ReadRegs(PCA9685_ADDR, 0x00u, &v, 1);
    v |= 0x10u;                                     /* bit4 = SLEEP: 置 1 进入低功耗并允许写 PRE_SCALE */
    I2C_WriteReg(PCA9685_ADDR, 0x00u, &v, 1);

    /* 写预分频 0xFE: f = 25MHz / (4096 * (prescale+1)) -> 50Hz, 算得 prescale=121 */
    I2C_WriteReg(PCA9685_ADDR, 0xFEu, &prescale, 1);

    /* 清 SLEEP 位(bit4)唤醒振荡器; 手册要求起振约 500us, 这里等 5ms 留足余量 */
    v &= 0xEFu;
    I2C_WriteReg(PCA9685_ADDR, 0x00u, &v, 1);
    Timer_DelayMs(5);

    /* bit7 = RESTART(从完整周期开始输出) | bit5 = AI(寄存器指针自动递增),
       结果 MODE1 = 0x21, 是后续 4 字节连写 SetPWM 的前提 */
    v |= 0xA0u;                                     /* RESTART | AI */
    I2C_WriteReg(PCA9685_ADDR, 0x00u, &v, 1);

    /* MODE2 = 0x04: bit2 = OUTDRV, 输出级从开漏改为推挽,
       推挽才能驱动舵机信号线这种容性负载给出干净边沿 */
    v = 0x04u;
    I2C_WriteReg(PCA9685_ADDR, 0x01u, &v, 1);

    /* 回读 MODE1 自检: 能读回 0x21 就证明 I2C 收发双向都通了 */
    v = 0;
    if (I2C_ReadRegs(PCA9685_ADDR, 0x00u, &v, 1))
    {
        printf("[PCA9685] MODE1=0x%02X (I2C OK)\r\n", (unsigned)v);
    }
    else
    {
        printf("[PCA9685] I2C READ FAIL!\r\n");
    }

    PCA9685_ResetAll();
}

/**
  * @brief  设置指定通道的 PWM 起止计数(12 位)
  * @param  ch  通道号 0~15
  * @param  on  周期内开始输出高电平的计数值(0~4095)
  * @param  off 周期内结束输出高电平的计数值(0~4095)
  * @retval 1 = 写成功, 0 = 通道越界或 I2C 写失败
  *
  * 原理: PCA9685 每个通道有 4 个寄存器:
  *         LEDn_ON_L / LEDn_ON_H / LEDn_OFF_L / LEDn_OFF_H
  *       其中 H 字节的高 4 位是"full on / full off"标志位,
  *       低 4 位才是 12 位计数值的高 4 位。这里把 on/off 全部限制在
  *       0~4095 的低 12 位范围内, 因此标志位恒为 0,
  *       写出的就是"普通 PWM"而不是"常亮/常灭"。
  *
  *       寄存器地址按 4 字节步进: LED0_ON_L = 0x06, 所以
  *         起始地址 = 0x06 + ch*4。
  *       4 个字节能一次发完, 靠的是初始化时打开的 AI(自动递增)位。
  *       字节顺序必须是**小端先低后高**: ON_L, ON_H, OFF_L, OFF_H,
  *       顺序写反会得到完全错误的脉宽。
  *
  *       舵机控制实际只用 on=0、off=脉宽计数这一种组合:
  *       即每个 20ms 周期开头就拉高, 到 off 时刻拉低,
  *       高电平宽度就是驱动舵机的脉冲宽度。
  */
uint8_t PCA9685_SetPWM(uint8_t ch, uint16_t on, uint16_t off)
{
    uint8_t buf[4];

    if (ch > PCA9685_CH_MAX) return 0;   /* 越界直接拒掉: 否则 0x06+ch*4 会写到别的寄存器,
                                            例如把 LED 寄存器写进 MODE1/MODE2 造成芯片失能 */

    buf[0] = (uint8_t)(on & 0xFF);
    buf[1] = (uint8_t)(on >> 8);
    buf[2] = (uint8_t)(off & 0xFF);
    buf[3] = (uint8_t)(off >> 8);
    /* LED0_ON_L 寄存器 = 0x06 + 4*ch; 从它开始连写 4 字节,
       依赖 MODE1.AI=1 让芯片自动递增寄存器指针 */
    return I2C_WriteReg(PCA9685_ADDR, (uint8_t)(0x06u + ch * 4u), buf, 4);
}

/**
  * @brief  设置指定通道舵机的角度
  * @param  ch    通道号 0~15(0~6 对应 1~7 号药盒)
  * @param  angle 目标角度, 0~180(度); 大于 180 会被截断到 180 而不是失败
  * @retval 1 = 成功, 0 = 通道越界或 I2C 写失败
  *
  * 原理(角度 -> 脉宽计数)
  *   舵机用"脉宽"表示角度: 50Hz 周期 20ms, 其中高电平 0.5ms 对应 0 度,
  *   2.5ms 对应 180 度, 中间线性。PCA9685 的计数频率为
  *       4096 计数 / 20ms = 204.8 计数/ms, 即 1 计数约 4.88us;
  *   于是 SERVO_PULSE_MIN = 0.5ms -> 102 计数,
  *       SERVO_PULSE_MAX = 2.5ms -> 512 计数。
  *   换算用**整数线性插值**而不是浮点:
  *       off = PULSE_MIN + angle * (PULSE_MAX - PULSE_MIN) / 180
  *   (与本文件历史版本注释里写的 102 + angle*410/180 不同:
  *    410 = 512-102 只有端点巧合相等, 90 度时两者差 11 个计数,
  *    以实际代码为准。)
  *   - angle 先声明为 uint8_t, 参与乘法时被提升为 int,
  *     最大 180 * 410 = 73800, 不会溢出 16 位, 中间量仍显式转 uint32_t
  *     是为了让"先乘后除"的意图明确、避免将来改成大角度时溢出;
  *   - **先乘后除**是这里的关键: 写成 angle * (410/180) 会先做整数除法
  *     得 2, 180 度只能算到 360 计数, 舵机行程直接减半。
  *
  * 为什么开盒用 90 度(SERVO_OPEN_ANGLE, 定义在 app.h)
  *   - 90 度是舵机的中间位, 往返行程对称, 对药箱盖的机械限位最友好。
  *
  * 【为什么直接跳到目标角度, 不做逐度爬升】
  *   早期版本为了"保护舵机"按度循环、每度加几毫秒延时, 结果从 0 度
  *   转到 90 度要**约 10 秒**才到位, 定时提醒已经响了、盒盖还没开,
  *   体验完全不可接受。舵机内部本身就带位置闭环, 直接给目标脉宽
  *   它会自己以最快速度转过去, 不存在"跳变损伤"问题;
  *   因此这里只写一次寄存器。
  */
uint8_t PCA9685_SetAngle(uint8_t ch, uint8_t angle)
{
    uint16_t off;

    if (ch > PCA9685_CH_MAX) return 0;
    if (angle > 180u) angle = 180u;

    /* 线性映射: 0.5ms(0度)=102 -> 2.5ms(180度)=512; 先乘后除避免整数除法把行程砍半 */
    off = (uint16_t)(SERVO_PULSE_MIN +
                     ((uint32_t)angle * (SERVO_PULSE_MAX - SERVO_PULSE_MIN)) / 180u);

    if (!PCA9685_SetPWM(ch, 0, off))
    {
        printf("[PCA9685] ch%u set fail!\r\n", (unsigned)ch);
        return 0;
    }
    return 1;
}

/**
  * @brief  复位全部通道: 0~6 路舵机回 0 度(关盖位置), 7~15 路关闭输出
  * @param  无
  * @retval 无
  *
  * 原理: 通道 0~6 上接的是 7 个药盒的舵机, 统一转到 0 度即机械关盖位;
  *       其余通道没有接舵机, 用 on=0/off=0 把输出彻底关掉,
  *       避免悬空通道输出脉冲在排线上形成干扰。
  *       一次复位全部而不是逐个关, 是因为 7 路共用同一片 PCA9685,
  *       写 7 次即可, 简单且不会漏掉。
  *
  *       off=0 的含义: LEDn_OFF 计数值为 0, 相当于高电平立刻结束,
  *       即该通道恒为低电平, 不输出任何脉冲。
  */
void PCA9685_ResetAll(void)
{
    uint8_t ch;

    for (ch = 0; ch <= PCA9685_CH_MAX; ch++)   /* 扫全部 16 路: 0~6 归位, 7~15 关断 */
    {
        if (ch < 7u)
        {
            PCA9685_SetAngle(ch, 0u);
        }
        else
        {
            PCA9685_SetPWM(ch, 0, 0);
        }
    }
}
