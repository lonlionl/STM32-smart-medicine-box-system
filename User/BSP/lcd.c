/**
  ******************************************************************************
  * @file    lcd.c
  * @brief   TFTLCD 显示驱动（ILI9488 控制器 + FSMC 16 位并口）
  ******************************************************************************
  * @note    本文件只包含显示底层的读写与绘图原语，不含任何界面业务逻辑；
  *          菜单/主界面等上层界面代码在 APP/menu.c 里。
  ******************************************************************************
  *
  * 一、硬件连接（ALIENTEK 精英版 STM32F103ZET6，屏为 3.5 寸 ILI9488，320x480）
  * ---------------------------------------------------------------------------
  *   LCD_CS   <- FSMC_NE4 (PG12)   片选，低有效；选中即把整片 LCD 挂进 FSMC 的 Bank1
  *   LCD_RS   <- FSMC_A10 (PG0)    命令/数据选择（0=命令，1=数据），见下面第三节
  *   LCD_WR   <- FSMC_NWE (PD5)    写选通，低有效，每个写周期自动产生
  *   LCD_RD   <- FSMC_NOE (PD4)    读选通，低有效，仅在读 ID / 读 GRAM 时用到
  *   LCD_RST  <- PD3               硬复位，低有效（普通 GPIO，不归 FSMC 管）
  *   LCD_BL   <- PB0               背光使能（普通 GPIO，推挽输出高电平点亮）
  *   数据总线 <- FSMC_D0~D15：
  *                PD14=D0, PD15=D1, PD0=D2, PD1=D3, PD8=D4, PD9=D5, PD10=D6,
  *                PE7=D7, PE8=D8, PE9=D9, PE10=D10, PE11=D11, PE12=D12,
  *                PE13=D13, PE14=D14, PE15=D15
  *   （注意：这些引脚号是 STM32F103 的 FSMC 硬件复用固定分配，不能随便换端口；
  *     本文件里按端口分组写 GPIO 初始化，就是为了迁就这个"物理分组不连续"的分配。）
  *
  ******************************************************************************
  *
  * 二、为什么能用"访问内存"的方式操作 LCD（FSMC 地址映射原理，重点）
  * ---------------------------------------------------------------------------
  *  1) STM32 的 FSMC 把外部 NOR/SRAM 映射进 CPU 的线性地址空间：
  *        Bank1 的 4 个子区各占 64MB，NE1/NE2/NE3/NE4 对应：
  *          0x60000000 (NE1)  0x64000000 (NE2)  0x68000000 (NE3)  0x6C000000 (NE4)
  *     本工程 LCD 接在 NE4 上，所以 LCD 的"基地址"就是 0x6C000000。
  *  2) CPU 总线上的地址线 A[x] 会依次接到 FSMC 的地址引脚 FSMC_A[x]，
  *     而 FSMC_A[x] 又接到外部器件的某个引脚上。本板把 **FSMC_A10 接到了 LCD 的 RS 脚**。
  *  3) 于是"访问哪个地址"就等价于"RS 是什么电平"：
  *        写 (uint16_t*)0x6C000000  ->  A10 = 0  ->  RS = 0  ->  LCD 认为是**命令**
  *        写 (uint16_t*)0x6C000800  ->  A10 = 1  ->  RS = 1  ->  LCD 认为是**数据**
  *     0x800 = 1 << 11，而 A10 在 16 位宽的 FSMC 上对应 HADDR[11]（A10<<1 = 11），
  *     所以宏里写成 (1UL << (LCD_RS_BIT + 1))，见 lcd.h 的 LCD_REG_ADDR / LCD_RAM_ADDR。
  *  4) 好处：区分命令/数据不再需要额外的 GPIO 去翻转 RS 电平，也不用手动拉片选、
  *     拉读写选通——一次普通的 16 位指针赋值，FSMC 硬件就自动产生完整的
  *     "片选有效 + 地址 + 写选通"时序。这正是本驱动全部读写都只用
  *     LCD_WR_REG() / LCD_WR_DATA() 两个宏的原因，代码因此极短且执行很快。
  *
  ******************************************************************************
  *
  * 三、FSMC 时序参数（HCLK = 72MHz，1 个 HCLK ≈ 13.89ns）
  * ---------------------------------------------------------------------------
  *     读时序：ADDSET=3、ADDHOLD=1、DATAST=6
  *     写时序：ADDSET=3、ADDHOLD=1、DATAST=3
  *   ILI9488 是并口屏，读操作比写操作慢得多（读 GRAM 尤其慢），
  *   所以这里给的读时序比写时序宽松一倍左右，属于"够用且留余量"的取值。
  *   详见 LCD_FSMC_Init() 里的逐字段注释。
  *
  ******************************************************************************
  *
  * 四、初始化流程总览（LCD_Init）
  * ---------------------------------------------------------------------------
  *     GPIO 上电复用 -> FSMC 配置并使能 NE4 -> 硬复位拉低再拉高
  *     -> 写 ILI9488 寄存器序列（软复位 / 方向 / 像素格式 / 电源 / VCOM /
  *        伽马 / 反显 / 退出睡眠 / 开显示）-> 清屏成白底。
  *
  ******************************************************************************
  *
  * 五、字库（点阵数据在 font.c，由 tools/gen_font.py 自动生成，勿手改）
  * ---------------------------------------------------------------------------
  *     font_ascii_8x16[95][16]     : ASCII 8x16，下标 = 字符码 - 0x20
  *     font_digit_16x32[11][64]    : 大号时钟数字 16x32，下标 = 数字 0~9，下标 10 是 ':'
  *     font_gbk16[] + font_gbk16_num : GBK 汉字 16x16，**按 GBK 码升序排列**
  *     font_gbk24[] + font_gbk24_num : GBK 汉字 24x24，同样升序
  *   因为两张 GBK 表都是升序的，查找可以用**二分查找**（O(log n)），
  *   不必线性扫描——这在只有几十个字形时差别不大，但把界面文案改多以后
  *   能明显减少每帧的查找开销。注意：字库是"按需生成"的，
  *   只收录了 tools/gen_font.py 里 UI_STRINGS / TITLE_STRINGS 列出的字，
  *   所以界面上新增文案后必须重跑生成脚本，否则查不到字形会画不出字。
  *   取模方式：**按行取模，高位在左**，每行字节数 = (宽 + 7) / 8，
  *   取某像素就是 data[行 * 每行字节数 + 列/8]，再取第 (7 - 列%8) 位。
  *
  ******************************************************************************
  *
  * 六、绘制路径（所有绘图最终都收敛到同一条路）
  * ---------------------------------------------------------------------------
  *     SetWindow(0x2A 设列范围 / 0x2B 设页范围 / 0x2C 准备写 GRAM)
  *       -> 连续往 LCD_WR_DATA 灌像素，控制器自动按窗口自增地址
  *     所以 FillRect / Clear / DrawGlyph 都是"设一次窗口 + 循环灌色"，
  *     只有 SetPixel 是每次重设一个 1x1 窗口（最慢，画线时才用）。
  ******************************************************************************
  */

#include "lcd.h"
#include "timer.h"
#include <stdio.h>
#include <string.h>

/* ==================== 内部函数声明 ==================== */
static void LCD_GPIO_Init(void);
static void LCD_FSMC_Init(void);

/* ==================== GPIO 与 FSMC 底层配置 ==================== */

/**
  * @brief  初始化 LCD 用到的全部 GPIO
  * @note   本函数只做"引脚功能/方向/速度"配置，不产生任何 LCD 通信时序；
  *         真正的总线时序由紧接着调用的 LCD_FSMC_Init() 决定。
  * @note   按端口分组初始化（GPIOD / GPIOE / GPIOG / GPIOB），
  *         因为 FSMC 的复用引脚在芯片上是按端口固定分配的，无法自由挑选。
  */
static void LCD_GPIO_Init(void)
{
    GPIO_InitTypeDef gpio;

    /* 使能需要用到的端口时钟。FSMC 用到的引脚分布在 D/E/G 三个端口；
       另外把 GPIOF 的时钟也一起打开——本驱动并没有使用任何 PF 引脚，
       属于从参考代码沿袭下来的冗余使能，多耗的电可以忽略，故保留不改。
       背光在 PB0，所以还要单独带上 LCD_BL_CLK（GPIOB）。 */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOD | RCC_APB2Periph_GPIOE |
                           RCC_APB2Periph_GPIOF | RCC_APB2Periph_GPIOG | LCD_BL_CLK, ENABLE);

    /* PD0 PD1 PD4 PD5 PD8 PD9 PD10 PD11 PD14 PD15: FSMC 数据/控制线
       （精英版：战舰版上 PD7 是 NE1，本板片选改到 PG12 = NE4，
         所以这里不再出现 PD7）
       逐个对应：PD14/PD15 = FSMC_D0/D1，PD0/PD1 = D2/D3，PD8~PD11 = D4~D7，
                 PD4 = FSMC_NOE（读选通），PD5 = FSMC_NWE（写选通）
       配成 GPIO_Mode_AF_PP（复用推挽）：引脚交给 FSMC 外设驱动，
       绝不能配成普通输出，否则 FSMC 抢不到引脚、总线时序全废。
       50MHz 档是 FSMC 高速翻转的最低要求档位。 */
    gpio.GPIO_Pin   = GPIO_Pin_0  | GPIO_Pin_1  | GPIO_Pin_4 | GPIO_Pin_5 |
                      GPIO_Pin_8  | GPIO_Pin_9  | GPIO_Pin_10 |
                      GPIO_Pin_11 | GPIO_Pin_14 | GPIO_Pin_15;
    gpio.GPIO_Mode  = GPIO_Mode_AF_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOD, &gpio);

    /* PD3: LCD 复位脚，低电平有效。
       它不归 FSMC 管，是普通 GPIO，所以配成推挽输出；
       初始化为高电平（不复位），真正的复位脉冲在 LCD_Init 里产生。
       注意此处与上面一组分开配置，正是因为它和 FSMC 复用脚的模式不同。 */
    gpio.GPIO_Pin   = GPIO_Pin_3;
    gpio.GPIO_Mode  = GPIO_Mode_Out_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOD, &gpio);
    GPIO_SetBits(GPIOD, GPIO_Pin_3);

    /* PE7~PE15: FSMC 数据线 D4~D12（实际是 D7~D15，见下一行的对应说明）
       准确对应关系：PE7=D7, PE8=D8, PE9=D9, PE10=D10, PE11=D11,
                     PE12=D12, PE13=D13, PE14=D14, PE15=D15
       这 9 根线配合 PD 上的 7 根（D0~D6）凑齐 16 位并行数据总线，
       正好对应 FSMC_MemoryDataWidth_16b；同样必须配成复用推挽。 */
    gpio.GPIO_Pin   = GPIO_Pin_7  | GPIO_Pin_8  | GPIO_Pin_9 | GPIO_Pin_10 |
                      GPIO_Pin_11 | GPIO_Pin_12 | GPIO_Pin_13 | GPIO_Pin_14 |
                      GPIO_Pin_15;
    gpio.GPIO_Mode  = GPIO_Mode_AF_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOE, &gpio);

    /* PG0: FSMC_A10，本板把它当 LCD_RS 用（精英版接法）
       这一根线是整套"用地址区分命令/数据"技巧的物理基础：
       往 0x6C000000 写时 A10=0（命令），往 0x6C000800 写时 A10=1（数据）。
       所以它必须交给 FSMC 自动控制，配成复用推挽，绝不能当普通 GPIO 手动翻转。 */
    gpio.GPIO_Pin   = GPIO_Pin_0;
    gpio.GPIO_Mode  = GPIO_Mode_AF_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOG, &gpio);

    /* PG12: FSMC_NE4，LCD 片选（精英版接法）
       NE4 有效时才响应 0x6C000000 起始的那 64MB 空间；
       拉高时 LCD 不理会总线，等于把屏从总线上"摘下来"。
       同理必须复用推挽，由 FSMC 硬件在访问对应地址时自动拉低。 */
    gpio.GPIO_Pin   = GPIO_Pin_12;
    gpio.GPIO_Mode  = GPIO_Mode_AF_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOG, &gpio);

    /* 背光：PB0 普通推挽输出，这里直接置高点亮。
       不影响任何 LCD 通信，因此可以在初始化 GPIO 阶段就打开，
       好处是后面整段寄存器初始化过程用户能看见"白屏逐渐刷出来"，
       便于判断是屏没亮还是初始化序列没生效。
       如果以后要做 PWM 调光，只需把这里改成复用推挽并接定时器通道。 */
    gpio.GPIO_Pin   = LCD_BL_PIN;
    gpio.GPIO_Mode  = GPIO_Mode_Out_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(LCD_BL_PORT, &gpio);
    GPIO_SetBits(LCD_BL_PORT, LCD_BL_PIN);      /* 背光点亮 */
}

/**
  * @brief  把 FSMC 的 Bank1 NE4 配置成 16 位 SRAM 模式，用来驱动 LCD
  * @note   为什么用"SRAM 模式"而不是"NOR 模式"：LCD 只需要"给地址、读写数据"
  *         这种最朴素的异步并行时序，SRAM 模式的模式 A（AccessMode_A）恰好
  *         就是"地址建立 -> 数据保持"的读写波形，不需要 NOR 特有的
  *         等待信号、突发、时钟分频等复杂机制，配置最简单、时序最可控。
  * @note   本函数必须在 LCD_GPIO_Init() 之后调用：引脚没配成复用，
  *         FSMC 即使使能了也驱动不了总线。
  */
static void LCD_FSMC_Init(void)
{
    FSMC_NORSRAMInitTypeDef  fsmc;
    FSMC_NORSRAMTimingInitTypeDef readTiming;   /* 只配一套: 读写共用(见下方说明) */

    /* FSMC 挂在 AHB 总线上，用 AHB 时钟使能（不是 APB2！）。
       不使能这一位，后面所有 FSMC 寄存器写入都是无效的。 */
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_FSMC, ENABLE);

    /* ---- 读时序（仅在 LCD_ReadID / 将来读 GRAM 时生效）----
       全部字段单位都是 HCLK 周期，此处 HCLK = 72MHz，1 周期 ≈ 13.89ns。 */

    /* FSMC_AddressSetupTime = 3：地址建立时间 3 周期 ≈ 41.7ns。
       这是从"地址(含 RS/CS)有效"到"读选通有效"之间的等待，
       给 LCD 足够时间识别到片选并准备输出数据。 */
    readTiming.FSMC_AddressSetupTime      = 3;
    /* FSMC_AddressHoldTime = 1：地址保持时间 1 周期 ≈ 13.9ns。
       读选通结束后地址还需多保持一会儿。ILI 系列控制器对读操作的
       地址保持要求不高，给 1 个周期即可。 */
    readTiming.FSMC_AddressHoldTime       = 1;
    /* FSMC_DataSetupTime = 6：数据建立时间 6 周期 ≈ 83.3ns。
       这是读周期中最关键的一项：读选通拉低后必须等这么久才采样数据。
       ILI9488 读操作（尤其读 GRAM）远比写慢，给 3 周期（写时序的取值）
       常常读回全 0 或乱码，所以这里刻意放宽到 6 周期。 */
    readTiming.FSMC_DataSetupTime         = 6;
    /* 以下 4 项都只对同步（突发/时钟）模式有意义，本工程用异步 SRAM 模式，
       全部置 0 表示不使用，留着只是为了把结构体填满。 */
    readTiming.FSMC_BusTurnAroundDuration = 0;   /* 总线周转时间，异步模式不用 */
    readTiming.FSMC_CLKDivision           = 0;   /* 同步时钟分频，不用 */
    readTiming.FSMC_DataLatency           = 0;   /* 同步模式数据延迟，不用 */
    /* 访问模式 A：SRAM/PSRAM 的标准异步时序，也是 LCD 驱动的常规选择。
       模式 A 下上面的 ADDSET / DATAST 才按"读/写各一套"生效。 */
    readTiming.FSMC_AccessMode            = FSMC_AccessMode_A;

        /* ---- 关于写时序（重要，与早期版本的描述不同）----

       注意：**本工程没有启用 FSMC 扩展模式**（见下面 FSMC_ExtendedMode），
       而 ST 标准库 stm32f10x_fsmc.c 里的逻辑是：

           if (FSMC_ExtendedMode == Enable)  用写时序结构体的内容写 BWTR
           else                              BWTR = 0x0FFFFFFF（各字段全 1，即最长）

       也就是说：扩展模式关闭时，**读和写都用 FSMC_ReadWriteTimingStruct
       那一套时序**（ADDSET=3 / DATAST=6）。写周期因此是 10 个 HCLK(约 139ns)，
       对 ILI9488 远在安全范围内，真机显示稳定。

       所以这里**不再保留一份不生效的写时序配置** —— 留着只会让后来人以为
       改它能改时序（本文件早期版本就是如此，注释还写着"模式 A 下这两个指针
       都会被用到"，那是错的）。

       如果将来确实要把写周期缩短（例如刷新整屏太慢），正确做法是：
           1) 把下面的 FSMC_ExtendedMode 改成 FSMC_ExtendedMode_Enable；
           2) 增加一个写时序结构体并赋给 FSMC_WriteTimingStruct，
              其中 DATAST 可取 3（写周期约 7 HCLK ≈ 97ns）；
           3) 改完必须实测 —— 写太快在杜邦线较长时会花屏/颜色错位，且很难查。
    */
    fsmc.FSMC_Bank           = FSMC_Bank1_NORSRAM4;   /* 精英版：NE4 */
    /* 数据/地址复用关闭：LCD 的数据线和地址线是分开的物理引脚，
       不存在"同一组线分时传地址和数据"的情况。 */
    fsmc.FSMC_DataAddressMux = FSMC_DataAddressMux_Disable;
    /* 存储器类型选 SRAM：决定 FSMC 使用模式 A 的异步读写波形 */
    fsmc.FSMC_MemoryType     = FSMC_MemoryType_SRAM;
    /* 数据宽度 16 位：与 LCD 的 16 位并口（RGB565 一次传一个像素）匹配。
       这一项还间接决定了地址线偏移——16 位宽时 HADDR[11] 才对应 FSMC_A10，
       也就是 lcd.h 里 LCD_RAM_ADDR 要用 1<<(10+1) 的原因。 */
    fsmc.FSMC_MemoryDataWidth= FSMC_MemoryDataWidth_16b;
    /* 突发访问关闭：LCD 不是支持突发读写的同步存储器，异步逐个传输即可 */
    fsmc.FSMC_BurstAccessMode= FSMC_BurstAccessMode_Disable;
    /* 异步等待关闭：不使用 LCD 输出的等待信号（本板也没接这根线） */
    fsmc.FSMC_AsynchronousWait = FSMC_AsynchronousWait_Disable;
    /* 以下 4 项都服务于"等待信号 NWait"机制，本工程完全不用等待信号，
       因此取值对实际时序没有影响；保持参考代码的默认值，
       避免以后真要接等待信号时又要回头改。 */
    fsmc.FSMC_WaitSignalPolarity = FSMC_WaitSignalPolarity_Low;   /* 等待信号低有效（未用） */
    fsmc.FSMC_WrapMode       = FSMC_WrapMode_Disable;             /* 环绕模式关闭（未用） */
    fsmc.FSMC_WaitSignalActive = FSMC_WaitSignalActive_BeforeWaitState; /* 等待信号在等待周期前采样（未用） */
    /* 写操作使能：LCD 绝大多数时间在接收数据（命令也是写），必须打开，
       关掉的话 FSMC 只允许读，屏上什么都不会出现。 */
    fsmc.FSMC_WriteOperation = FSMC_WriteOperation_Enable;
    fsmc.FSMC_WaitSignal     = FSMC_WaitSignal_Disable;           /* 不使用等待信号 */
    /* 扩展模式关闭。扩展模式用于"读写时序差别很大、需要独立配置"的场景：
       开启后写操作会用 FSMC_WriteTimingStruct 单独配一套时序；
       关闭则**读写共用 FSMC_ReadWriteTimingStruct**（原因见上面"关于写时序"）。
       本工程读写共用一套时序已实测稳定，所以保持关闭。 */
    fsmc.FSMC_ExtendedMode   = FSMC_ExtendedMode_Disable;
    fsmc.FSMC_WriteBurst     = FSMC_WriteBurst_Disable;           /* 写突发关闭 */
    fsmc.FSMC_ReadWriteTimingStruct = &readTiming;   /* 唯一的时序指针: 读写共用 */

    /* 先按上述参数写寄存器，再使能该子区。
       顺序不能反：先使能会让 FSMC 用未配置的默认时序去访问总线。 */
    FSMC_NORSRAMInit(&fsmc);
    FSMC_NORSRAMCmd(FSMC_Bank1_NORSRAM4, ENABLE);
}

/* ==================== 底层读写与字库查找 ==================== */

/**
  * @brief  在 GBK 16x16 字库表里二分查找一个汉字字形
  * @param  hi  汉字 GBK 编码的高字节（首字节，范围 0x81~0xFE）
  * @param  lo  汉字 GBK 编码的低字节（次字节，范围 0x40~0xFE）
  * @retval 指向字形结构体的指针；字库里没有这个字时返回 NULL
  * @note   原理：font_gbk16[] 由 tools/gen_font.py 按 GBK 码升序生成，
  *         所以可以直接二分。把两个字节拼成一个 uint16_t（高字节在前）
  *         当作比较键，比逐字节比较更直观也更快。
  *         查找区间用 [loIdx, hiIdx) 左闭右开写法：
  *         loIdx==hiIdx 时区间空、循环结束，天然处理"找不到"的情况。
  * @note   一个易踩的坑：字库只收录了界面真正用到的汉字，
  *         所以这里的"找不到"是正常返回值，调用方必须处理（本文件的做法
  *         是干脆跳过不画，但游标仍然右移，保证后面的字位置不乱）。
  */
static const FontGbk16_t *FontGbk_Find16(uint8_t hi, uint8_t lo)
{
    uint16_t loIdx = 0, hiIdx = font_gbk16_num;
    /* 拼成 16 位比较键：GBK 首字节在高位，正好与升序表的排列一致 */
    uint16_t key = (uint16_t)(((uint16_t)hi << 8) | lo);

    while (loIdx < hiIdx)
    {
        /* 取中点。这里用无符号右移 1 位代替除以 2，编译器会直接生成移位指令 */
        uint16_t mid = (uint16_t)((loIdx + hiIdx) >> 1);
        uint16_t val = (uint16_t)(((uint16_t)font_gbk16[mid].gbk[0] << 8) |
                                   font_gbk16[mid].gbk[1]);
        if (val == key) return &font_gbk16[mid];
        /* 表中值偏小 -> 目标在右半区，注意是 mid+1，避免 mid 被反复取到而死循环 */
        if (val < key)  loIdx = (uint16_t)(mid + 1);
        /* 表中值偏大 -> 目标在左半区，hiIdx = mid（开区间，不含 mid） */
        else            hiIdx = mid;
    }
    return NULL;    /* 字库里没有这个字 */
}

/**
  * @brief  在 GBK 24x24 字库表里二分查找一个汉字字形
  * @param  hi  汉字 GBK 编码的高字节
  * @param  lo  汉字 GBK 编码的低字节
  * @retval 指向字形结构体的指针；找不到返回 NULL
  * @note   与 FontGbk_Find16 完全同构，只是查的表不同（font_gbk24[]）。
  *         分开写两份而不是做成一个带参数的通用函数，是为了避免
  *         运行时多一层"表基址/元素大小"的间接计算——本函数在刷屏路径上
  *         会被频繁调用，这一层开销在 72MHz 的 M3 上并不便宜。
  */
static const FontGbk24_t *FontGbk_Find24(uint8_t hi, uint8_t lo)
{
    uint16_t loIdx = 0, hiIdx = font_gbk24_num;
    uint16_t key = (uint16_t)(((uint16_t)hi << 8) | lo);

    while (loIdx < hiIdx)
    {
        uint16_t mid = (uint16_t)((loIdx + hiIdx) >> 1);
        uint16_t val = (uint16_t)(((uint16_t)font_gbk24[mid].gbk[0] << 8) |
                                   font_gbk24[mid].gbk[1]);
        if (val == key) return &font_gbk24[mid];
        if (val < key)  loIdx = (uint16_t)(mid + 1);
        else            hiIdx = mid;
    }
    return NULL;
}

/**
  * @brief  把一个点阵字形画进 w x h 的矩形区域（逐像素写前景/背景色）
  * @param  x,y   矩形左上角坐标（屏幕像素坐标，左上角为原点）
  * @param  w,h   字形宽高：ASCII 8x16 传 (8,16)、GBK 16x16 传 (16,16)、
  *               大数字传 (16,32) 或 (24,24)
  * @param  data  点阵数据首地址，**按行取模、高位在左**
  * @param  fg    字形笔画（值为 1 的位）使用的颜色，RGB565
  * @param  bg    字形空白（值为 0 的位）使用的颜色，RGB565
  * @note   原理：一次性把整个字形区域设为 GRAM 窗口，之后连续灌 w*h 个像素。
  *         因为所有字形都是定宽的，每行像素数恒定，所以**不需要换行重设窗口**，
  *         控制器会自己按窗口宽度折行，这也是本函数只调一次 LCD_SetWindow 的原因。
  * @note   bg 一定要真的写进去（而不是留空不画）：上层界面是靠反复重画
  *         "同位置、同底色"的字来实现局部刷新的，如果背景不写，
  *         新老数字/文字会叠影。
  * @note   每像素一次 LCD_WR_DATA，没有做 8 像素打包优化，
  *         属于"代码简单优先"的写法；实测刷一屏中文（约 300 字）耗时在
  *         几十毫秒量级，对本项目 1 秒级的界面刷新率足够。
  */
static void LCD_DrawGlyph(uint16_t x, uint16_t y, uint8_t w, uint8_t h,
                          const uint8_t *data, uint16_t fg, uint16_t bg)
{
    /* 每行占几个字节：宽度不是 8 的倍数时向上取整。
       16 宽 -> 2 字节；24 宽 -> 3 字节；8 宽 -> 1 字节。 */
    uint8_t nbyte = (uint8_t)((w + 7) / 8);
    uint8_t row, col;

    /* 只设一次窗口：横跨整个字形，控制器写满一行会自动折到下一行 */
    LCD_SetWindow(x, y, (uint16_t)(x + w - 1), (uint16_t)(y + h - 1));
    for (row = 0; row < h; row++)
    {
        for (col = 0; col < w; col++)
        {
            /* 定位到第 row 行的第 (col>>3) 个字节（>>3 即除以 8） */
            uint8_t byte = data[(uint16_t)row * nbyte + (col >> 3)];
            /* 取该字节里代表 col 列的那一位：取模是高位在左，
               所以左边第 0 列对应 bit7，故用 (7 - (col & 7)) */
            uint8_t bit  = (byte >> (7u - (col & 7))) & 1u;
            /* 1 画前景、0 画背景，一个像素都不能省 */
            LCD_WR_DATA(bit ? fg : bg);
        }
    }
}

/* ==================== 初始化 ==================== */

/**
  * @brief  LCD 总初始化：GPIO -> FSMC -> 硬复位 -> ILI9488 寄存器序列 -> 清屏
  * @note   调用前无需任何前提；调用后屏幕为纯白，可以开始画界面。
  * @note   整个函数里所有延时都是**必须的**，不是在"保险起见等一等"：
  *         复位后控制器内部要跑上电时序，软复位后要重载出厂参数，
  *         退出睡眠后要等内部电荷泵把电压建立起来。延时不够的典型症状是
  *         屏幕全白或花屏，而且换一块屏就好了/坏了，极难定位。
  */
void LCD_Init(void)
{
    LCD_GPIO_Init();    /* 先把引脚交给 FSMC（含背光点亮） */
    LCD_FSMC_Init();    /* 再配好总线时序并使能 NE4，此时才能读写 LCD */

    /* 硬件复位：RST(PD3) 拉低 -> 保持 10ms -> 拉高 -> 等 50ms。
       ILI9488 要求复位低电平至少持续 10us，这里给到 10ms 是远超要求的余量，
       目的是兼容排线上可能存在的 RC 滤波，确保复位脉冲真的够"实"。
       拉高后的 50ms 是留给控制器完成内部上电初始化的。 */
    GPIO_ResetBits(GPIOD, GPIO_Pin_3);
    Timer_DelayMs(10);
    GPIO_SetBits(GPIOD, GPIO_Pin_3);
    Timer_DelayMs(50);

    /* ---------- ILI9488 初始化序列（320x480，16 位色）----------
       下面统一用 LCD_WR_REG(reg) 写命令、LCD_WR_DATA(dat) 写参数。
       这两个宏之所以只是普通指针赋值就能产生完整总线时序，
       原理见文件头的第二节（FSMC 地址线 A10 当 RS 用）。 */

    /* SWRESET(0x01) 软复位：让控制器把除"睡眠/显示开关"以外的大部分寄存器
       恢复出厂值。这里做的是"硬复位 + 软复位"双重复位，
       好处是初始化序列有一个确定的起点，不受上次运行残留状态影响。
       软复位后必须等 120ms（手册要求 5ms 以上，这里给足余量）。 */
    LCD_WR_REG(0x01);                                   /* 软复位 SWRESET */
    Timer_DelayMs(120);

    /* MADCTL(0x36) 内存访问控制 = 0x48。
       这一条是本工程最"有故事"的寄存器：早期画面上内容是镜像的，
       靠把 MADCTL 改成 0x48 才修正过来（见 Doc/开发历程整理.docx）。
       0x48 = 0100 1000b = 0x40 | 0x08，按位拆开是：
         bit7 MY=0 ：行地址增长方向（保持自上而下）
         bit6 MX=1 ：**列地址增长方向反向** —— 这一位正是"修镜像"的关键：
                     屏的默认列扫描方向与驱动预期相反，画面才会左右颠倒，
                     把 MX 置 1 让列地址反向增长，画面就正过来了。
                     （早期版本的注释把这一位写成 MX=0，与 0100 1000b 自相矛盾）
         bit5 MV=0 ：行列交换关闭（不交换，所以仍是 320 宽 x 480 高，竖屏）
         bit3 BGR=1：像素字节内 R/B 通道交换 —— 因为屏的物理子像素排列是 BGR，
                     而程序里的颜色常量按 RGB565 写（如 RED=0xF800），
                     置这一位后，写 0xF800 出来才是红色；不置则红蓝互换。
       MY/MX/MV 三位（bit7/6/5）合起来共 8 种组合，正是"屏幕旋转 90 度"
       那套参数的来源；本工程只用这一组固定竖屏参数，不做旋转。 */
    LCD_WR_REG(0x36); LCD_WR_DATA(0x48);                /* MADCTL：修正镜像 + BGR + 竖屏 320x480 */
    /* PIXFMT(0x3A) 像素格式 = 0x55，即每个像素 16 位（RGB565）。
       0x55 = 0101 0101b 是 ILI9488 手册规定的 16bpp 专用值
       （不是把 0x55 当"位数"读，它只是编码）。
       之所以选 16bpp：一个像素正好一次 16 位总线写，配合 FSMC 的
       16 位数据宽度效率最高；再高的 18bpp/24bpp 需要拆成两次总线写，
       颜色好看一点但刷屏速度直接掉一半，对 3.5 寸屏不划算。 */
    LCD_WR_REG(0x3A); LCD_WR_DATA(0x55);                /* PIXFMT：16bpp RGB565 */

    /* PWCTRL1(0xC0) 电源控制 1 = 0x19。
       该寄存器给内部升压电路定"倍率"：低 4 位定 VGH/VGL 的倍率档，
       高 2 位定 AVDD 的倍率档。0x19 这一取值来自 ILI9488 的
       推荐初始化序列，作用是让面板获得合适的栅极驱动电压。
       值改小了屏会偏暗/对比度低，改大了功耗与发热上升。 */
    LCD_WR_REG(0xC0); LCD_WR_DATA(0x19);                /* 电源控制 1（升压倍率） */
    /* PWCTRL2(0xC1) 电源控制 2 = 0x12，设定 VGH / VGL / VCI 等各路电压的
       细化档位，同样照搬手册推荐值。它与 0xC0 一起决定面板的实际驱动能力，
       两者必须成对使用参考序列的值，不能只改其中一个。 */
    LCD_WR_REG(0xC1); LCD_WR_DATA(0x12);                /* 电源控制 2 */
    /* VCOM(0xC5) VCOM 控制 = 0x5B。
       这一条直接决定液晶的公共极电压（即"底色偏压"），
       是消除闪烁与残影的关键：值不合适时画面会明显闪烁或有拖影。
       0x5B 为该面板的推荐值；注意它和伽马校正是一组，
       调了 VCOM 通常需要重新确认伽马，反之亦然。 */
    LCD_WR_REG(0xC5); LCD_WR_DATA(0x5B);                /* VCOM 控制 */
    /* DFUNCTR(0xB6) 显示功能控制，本工程写 3 个参数：
         byte1 = 0x02：设置 RGB 视频模式的同步方式（本工程用并口写 GRAM，
                        这一项其实不起作用，照抄参考序列）
         byte2 = 0x02：设置"扫描方向/低功耗"等显示行为位，
                        保持与 MADCTL 的竖屏方向一致
         byte3 = 0x3B：后廊（back porch）时间设置，影响扫描的
                        场消隐时间；值偏小可能在刷新时看到撕裂，
                        0x3B 为手册推荐值
       这 3 个字节**必须一次写完**，中间不能被别的命令打断，
       所以这里用一行连续三次 LCD_WR_DATA。 */
    LCD_WR_REG(0xB6); LCD_WR_DATA(0x02); LCD_WR_DATA(0x02); LCD_WR_DATA(0x3B); /* 显示功能控制 DFUNCTR */

    /* GMCTRP1(0xE0) 正极性伽马校正，15 个参数对应 15 个灰阶拐点。
       伽马校正决定"灰阶亮度曲线"，人眼对低灰阶特别敏感，
       所以拐点都集中在前半段精确调整。这 15 个值取自 ILI9488
       推荐序列，是本面板看起来"颜色正常"的基础。
       调错的表现不是花屏，而是画面发白/发暗/有色偏——很像硬件故障，
       所以没有明确理由时**不要改这组值**。 */
    LCD_WR_REG(0xE0);
    LCD_WR_DATA(0x00); LCD_WR_DATA(0x03); LCD_WR_DATA(0x09); LCD_WR_DATA(0x08);
    LCD_WR_DATA(0x16); LCD_WR_DATA(0x0A); LCD_WR_DATA(0x3F); LCD_WR_DATA(0x78);
    LCD_WR_DATA(0x4C); LCD_WR_DATA(0x09); LCD_WR_DATA(0x0A); LCD_WR_DATA(0x08);
    LCD_WR_DATA(0x16); LCD_WR_DATA(0x1A); LCD_WR_DATA(0x0F);

    /* GMCTRN1(0xE1) 负极性伽马校正，同样 15 个参数。
       液晶必须正负极性交替驱动（防直流残留把液晶"电解"坏），
       正负两套伽马必须配对调整，保证两种极性下的亮度一致，
       否则会看到整体闪烁。因此 0xE0/0xE1 这两组值总是成对出现。 */
    LCD_WR_REG(0xE1);
    LCD_WR_DATA(0x00); LCD_WR_DATA(0x16); LCD_WR_DATA(0x19); LCD_WR_DATA(0x03);
    LCD_WR_DATA(0x0F); LCD_WR_DATA(0x05); LCD_WR_DATA(0x32); LCD_WR_DATA(0x45);
    LCD_WR_DATA(0x46); LCD_WR_DATA(0x04); LCD_WR_DATA(0x0E); LCD_WR_DATA(0x0D);
    LCD_WR_DATA(0x35); LCD_WR_DATA(0x37); LCD_WR_DATA(0x0F);

    /* INVON(0x21) 打开显示反显。
       反显会把"写进去的颜色值"整体取反再输出。本面板是常黑（normally black）
       类型，配合上面的 BGR 设置，打开反显后颜色才正常；
       如果关掉这一条，整个画面会变成"负片"效果（白变黑、红变青），
       是很典型的初始化漏写症状。 */
    LCD_WR_REG(0x21);                                   /* 反显开 INVON */
    /* SLPOUT(0x11) 退出睡眠。
       睡眠态下控制器关掉了内部升压与振荡器以省电，此命令重新启动它们。
       手册要求退出睡眠后等 120ms 才能发下一条命令（内部电压建立需要时间），
       这里的延时是**必须**的，省掉会出现随机的初始化失败。 */
    LCD_WR_REG(0x11);                                   /* 退出睡眠 SLPOUT */
    Timer_DelayMs(120);
    /* DISPON(0x29) 开显示。放在最后：前面所有参数都已就绪，
       一旦打开显示立刻就是最终画面，避免用户看到中间态的乱码。
       到此为止初始化序列结束（本工程不读 ID 做型号判断）。 */
    LCD_WR_REG(0x29);                                   /* 开显示 DISPON */

    /* 清成白底：把 GRAM 全部写成白色，给上层界面一个干净的起始状态。
       放在初始化末尾而不是交给上层，是为了保证"LCD_Init 返回后屏幕状态确定"。 */
    LCD_Clear(WHITE);
}

/**
  * @brief  读 LCD 控制器的 ID
  * @retval 本屏 ILI9488 实际返回 0x9488（低 8 位可能因读时序略有差异）
  * @note   原理：先写 RDID4 命令 0xD3，然后连续读回若干字节，
  *         其中真正的 ID 高低字节在最后。ILI9488 返回的序列是
  *         00 00 94 88，所以要先丢弃前面几个无效字节。
  * @note   这里刻意先做两次"哑读"再取第三次的值：因为 LCD 的读时序里，
  *         命令写入后控制器需要时间准备数据，头几次读操作常常读到
  *         上一次总线上残留的值或全 0。多读两次相当于把这段不确定期
  *         跨过去，代价是两次多余的总线读，换来的是稳定的 ID。
  * @note   返回值主要用于 main.c 开机打印（确认屏型号/排查接触不良），
  *         本驱动**不依赖**这个返回值来决定初始化参数，
  *         所以即使读回的值因排线过长而不准，也不影响显示。
  */
uint16_t LCD_ReadID(void)
{
    uint16_t id;

    LCD_WR_REG(0xD3);                   /* 读 ID 命令 RDID4 */
    (void)*LCD_RAM_ADDR;                /* 第 1 次哑读，丢弃（总线未稳定/残留值） */
    (void)*LCD_RAM_ADDR;                /* 第 2 次哑读，丢弃（该字节固定为 0x00） */
    id  = (uint16_t)(*LCD_RAM_ADDR << 8);   /* 第 3 次 = ID 高字节（0x94） */
    id |= (uint16_t)(*LCD_RAM_ADDR & 0x00FFu);  /* 第 4 次 = ID 低字节（0x88） */
    /* 必须把两次读拼起来才是 0x9488：RDID4 回读的是**字节流** 00 00 94 88，
       不是一次 16 位读就能拿到完整 ID —— 这正是本函数早期版本的问题，
       当时只读一次就返回，于是打印出来恒为 0x0094，与注释承诺的 0x9488 不符。
       低字节在不同读时序下可能是 0x88 也可能是 0x93，只做参考、驱动不依赖它。 */
    return id;
}

/* ==================== 绘图原语 ==================== */

/**
  * @brief  设置 GRAM 写入窗口（列范围 + 页范围），并把写指针定位到窗口左上角
  * @param  x0,y0  窗口左上角坐标
  * @param  x1,y1  窗口右下角坐标（**含**端点，不是长度）
  * @note   原理：这是 ILI9488 的"区域自增写入"机制。
  *         0x2A 设列地址范围（横坐标），0x2B 设页地址范围（纵坐标），
  *         之后每写一个 0x2C 数据，内部写指针自动右移，写到列末尾
  *         自动折到下一行开头，写满整个窗口后又回到窗口左上角。
  *         所以只要设一次窗口，就能一次性灌完整个矩形，这是所有填充类
  *         函数能跑得很快的根本原因。
  * @note   坐标都是 16 位的，所以每个值要拆成"高字节 + 低字节"两次写
  *         （控制器是 8 位寄存器，而总线是 16 位）。
  * @note   最后写 0x2C（写 GRAM）只是"进入写数据状态"，
  *         并不包含任何像素，因而是 0 个数据参数的裸命令；
  *         调用方紧接着连续 LCD_WR_DATA 就是在填这个窗口。
  * @note   本函数**不做边界检查**：调用方必须保证 x1 >= x0、y1 >= y0
  *         且都在 0~319 / 0~479 之内，否则写出的窗口是无意义的，
  *         表现为图像错位或局部不刷新。
  */
void LCD_SetWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    LCD_WR_REG(0x2A);                   /* 设列地址范围（X 方向） */
    LCD_WR_DATA(x0 >> 8); LCD_WR_DATA(x0);    /* 起始列 SC，先高字节后低字节 */
    LCD_WR_DATA(x1 >> 8); LCD_WR_DATA(x1);    /* 结束列 EC */
    LCD_WR_REG(0x2B);                   /* 设页地址范围（Y 方向） */
    LCD_WR_DATA(y0 >> 8); LCD_WR_DATA(y0);    /* 起始页 SP */
    LCD_WR_DATA(y1 >> 8); LCD_WR_DATA(y1);    /* 结束页 EP */
    LCD_WR_REG(0x2C);                   /* 写 GRAM：之后的数据都进这个窗口 */
}

/**
  * @brief  用单色清整屏
  * @param  color  填充色（RGB565）
  * @note   实现就是"全屏窗口 + 灌 LCD_W*LCD_H 个像素"，
  *         所以它和 LCD_FillRect 是同一条路径，没有额外优化空间。
  * @note   全屏 320x480 = 153600 个像素，每个像素一次 16 位总线写
  *         （写时序约 7 个 HCLK ≈ 97ns），理论耗时约 15ms，
  *         实际含循环开销在 20~30ms 量级，肉眼能看到"刷"的一下，
  *         但不构成界面卡顿。
  */
void LCD_Clear(uint16_t color)
{
    uint32_t i;

    LCD_SetWindow(0, 0, LCD_W - 1, LCD_H - 1);   /* 窗口覆盖整屏 */
    /* 循环变量用 uint32_t：320*480 = 153600 已经超出 uint16_t 上限，
       用 16 位会溢出成死循环，这里是把乘积先提升为 32 位再比较。 */
    for (i = 0; i < ((uint32_t)LCD_W * LCD_H); i++)
    {
        LCD_WR_DATA(color);
    }
}

/**
  * @brief  填充一个实心矩形
  * @param  x0,y0  左上角坐标
  * @param  x1,y1  右下角坐标（**含**端点）
  * @param  color  填充色（RGB565）
  * @note   宽高都用 +1 计算，因为坐标是"含端点"的：x0==x1 时
  *         宽应当是 1 而不是 0。这里先把 (x1-x0+1) 提升成 uint32_t
  *         再相乘，避免 w*h 在 16 位下溢出（大矩形很容易超过 65535）。
  * @note   上层界面大量使用"用底色铺一块再写字"的方式做局部刷新，
  *         所以本函数是界面刷新里调用最频繁的原语之一。
  */
void LCD_FillRect(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color)
{
    uint32_t w = (uint32_t)(x1 - x0 + 1);
    uint32_t h = (uint32_t)(y1 - y0 + 1);
    uint32_t i, total = w * h;

    LCD_SetWindow(x0, y0, x1, y1);
    for (i = 0; i < total; i++)
    {
        LCD_WR_DATA(color);
    }
}

/**
  * @brief  画单个像素
  * @param  x,y    像素坐标
  * @param  color  颜色（RGB565）
  * @note   这是最"重"的绘图方式：每个像素都要重新下发 0x2A/0x2B/0x2C
  *         三条命令共 9 次总线写，再写 1 次数据，开销是 FillRect 的十几倍。
  *         所以它只用在"点数量少且位置不连续"的场合——本文件里只有
  *         Bresenham 画线会调用它。
  * @note   之所以不在这里做边界判断：调用方（画线/画矩形）的坐标都来自
  *         上层算好的界面布局，加了判断反而拖慢最热的内层循环。
  */
void LCD_SetPixel(uint16_t x, uint16_t y, uint16_t color)
{
    LCD_SetWindow(x, y, x, y);  /* 1x1 的窗口 */
    LCD_WR_DATA(color);
}

/**
  * @brief  用 Bresenham 整数算法画一条直线
  * @param  x0,y0  起点坐标
  * @param  x1,y1  终点坐标
  * @param  color  线条颜色（RGB565）
  * @note   原理（为什么要用 Bresenham）：直线方程需要浮点乘除，
  *         在 M3 上既慢又有舍入误差，画长线会出现"该连的像素断开"。
  *         Bresenham 用整数误差累积代替除法：err 表示"当前点距离理想
  *         直线的偏差"，每一步按偏差符号决定是否同时移动 y，
  *         全程只有加减法和乘 2（编译成移位），既快又绝对均匀。
  * @note   dx/dy 先取绝对值、sx/sy 只取符号（+1/-1），
  *         这样八种方向的直线共用同一段循环，无须分情况讨论。
  * @note   结束时用"坐标相等"判断而不是递减步数，是因为
  *         水平/垂直/斜线三种情况步数不同，用坐标判断最省心。
  */
void LCD_DrawLine(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color)
{
    int16_t dx, dy, sx, sy, err, e2;

    /* 有符号 16 位足够：坐标范围是 0~479，差值绝不会溢出 int16_t */
    dx = (int16_t)((x1 > x0) ? (x1 - x0) : (x0 - x1));   /* |Δx| */
    dy = (int16_t)((y1 > y0) ? (y1 - y0) : (y0 - y1));   /* |Δy| */
    sx = (x0 < x1) ? 1 : -1;                             /* x 步进方向 */
    sy = (y0 < y1) ? 1 : -1;                             /* y 步进方向 */
    err = dx - dy;                                       /* 初始误差 */

    for (;;)
    {
        LCD_SetPixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;    /* 到终点即收工 */
        e2 = (int16_t)(2 * err);            /* 误差乘 2，避免下面出现除法 */
        /* 水平方向该走：走完把 y 的"欠账"从误差里扣掉 */
        if (e2 > -dy) { err -= dy; x0 = (uint16_t)(x0 + sx); }
        /* 垂直方向该走：走完把 x 的"欠账"加进误差 */
        if (e2 <  dx) { err += dx; y0 = (uint16_t)(y0 + sy); }
    }
}

/**
  * @brief  画一个空心矩形（四条边，内部不填充）
  * @param  x0,y0  左上角坐标
  * @param  x1,y1  右下角坐标（含端点）
  * @param  color  边框颜色（RGB565）
  * @note   直接复用四次 LCD_DrawLine 画四条边，不做任何"角点去重"优化：
  *         四个角会被画两次，代价是 4 个多余像素，
  *         换来的是代码极短且不会漏画边——这种取舍在本项目里是合适的。
  * @note   界面上的列表项外框、分隔框都用它。
  */
void LCD_DrawRect(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color)
{
    LCD_DrawLine(x0, y0, x1, y0, color);    /* 上边 */
    LCD_DrawLine(x0, y1, x1, y1, color);    /* 下边 */
    LCD_DrawLine(x0, y0, x0, y1, color);    /* 左边 */
    LCD_DrawLine(x1, y0, x1, y1, color);    /* 右边 */
}

/* ==================== 文本与字库 ==================== */

/**
  * @brief  画一串文本（ASCII 用 8x16、汉字用 16x16）
  * @param  x,y   文本左上角坐标
  * @param  str   **GBK 编码**的字符串（源文件存的就是 GBK 字面量，可直接传）
  * @param  fg    文字颜色
  * @param  bg    文字底色（必须给，用于擦除上一次的残留内容）
  * @note   原理：逐字节判断字符类型——
  *           首字节 < 0x80  => ASCII 单字节，宽 8
  *           首字节 >= 0x80 => GBK 双字节汉字，宽 16
  *         所以同一行里中英文可以混排，游标 xx 按各自宽度右移。
  * @note   **被截断的半个汉字**必须处理：如果字符串最后一个字节是汉字的
  *         首字节（例如上层用 snprintf 按长度截断了文案），此时 str[1] 是
  *         字符串结束符 0。若不判断就会把 0x00 当成第二个字节去查表，
  *         轻则查表失败，重则越界读。这里直接跳过这半个字并结束循环。
  * @note   查不到字形时**仍然把游标右移**：宁可留一个空白格，
  *         也不能让后面所有字整体左移——否则一行文字的排版会崩掉，
  *         反而更难看出是字库缺字。
  * @note   本函数不处理换行、不做右边界裁剪，调用方需自行保证
  *         文本不超出屏幕（本项目的界面都在 320 宽内排版）。
  */
void LCD_ShowText16(uint16_t x, uint16_t y, const char *str, uint16_t fg, uint16_t bg)
{
    uint16_t xx = x;    /* 当前字符的绘制横坐标，逐字符右移 */

    while (*str)
    {
        uint8_t c = (uint8_t)*str;
        if (c < 0x80)                       /* ASCII：单字节 */
        {
            /* 可打印 ASCII 是 0x20~0x7E，两端都要判：
               低于 0x20 是控制字符（\r \n \t），高于 0x7E 是 0x7F(DEL)。
               font_ascii_8x16 只有 95 项（下标 0~94，对应 0x20~0x7E），
               只判下界的话 0x7F 会取到下标 95 —— 越界读到相邻表的头部，
               画出一块错乱图案，而且不报错、不 HardFault（Flash 越界读），
               属于极难排查的显示异常。 */
            if (c >= 0x20 && c <= 0x7E)
            {
                /* 字库下标 = 字符码 - 0x20（表里第 0 项是空格） */
                LCD_DrawGlyph(xx, y, 8, 16, font_ascii_8x16[c - 0x20], fg, bg);
            }
            /* 即使没画（控制字符）也要推进游标，保持排版与预期一致 */
            xx += 8;
            str++;
        }
        else                                /* 汉字：GBK 双字节 */
        {
            /* 半个汉字（首字节后就是 '\0'）：跳过它，别去读越界的数据 */
            if (str[1] == 0) { str++; continue; }
            {
                const FontGbk16_t *e = FontGbk_Find16(c, (uint8_t)str[1]);
                if (e)      /* 查不到就留空，但位置照留 */
                {
                    LCD_DrawGlyph(xx, y, 16, 16, e->data, fg, bg);
                }
            }
            xx += 16;       /* 汉字固定占 16 像素宽（16x16 等宽点阵） */
            str += 2;       /* 一次消费两个字节 */
        }
    }
}

/**
  * @brief  画一串 24x24 的汉字（仅用于标题等大字场合）
  * @param  x,y   文本左上角坐标
  * @param  str   GBK 编码字符串
  * @param  fg    文字颜色
  * @param  bg    文字底色
  * @note   **ASCII 会被跳过（不画也不占位）**：24x24 字库里没有 ASCII 字形
  *         （只收录了 TITLE_STRINGS 里的汉字，目的是压缩 Flash 占用），
  *         所以本函数假定传入的都是纯中文标题。
  *         正因如此，调用方排版时必须按"汉字个数 x 24"算宽度，
  *         而不是用 LCD_TextWidth16（那是 16 号字宽，两者不通用）。
  * @note   同样处理了半个汉字：str[1] == 0 时走 else 分支当作跳过处理。
  */
void LCD_ShowText24(uint16_t x, uint16_t y, const char *str, uint16_t fg, uint16_t bg)
{
    uint16_t xx = x;

    while (*str)
    {
        uint8_t c = (uint8_t)*str;
        /* 只有"双字节汉字的首字节 + 后面确实还有字节"才进入 24x24 绘制 */
        if (c >= 0x80 && str[1] != 0)
        {
            const FontGbk24_t *e = FontGbk_Find24(c, (uint8_t)str[1]);
            if (e)
            {
                LCD_DrawGlyph(xx, y, 24, 24, e->data, fg, bg);
            }
            xx += 24;       /* 24x24 汉字每个占 24 像素宽 */
            str += 2;
        }
        else
        {
            str++;                          /* ASCII 字节：直接跳过，不占宽度 */
        }
    }
}

/**
  * @brief  按固定宽度右对齐显示一个十进制整数（16 号字）
  * @param  x,y   文本左上角坐标
  * @param  num   要显示的整数（有符号，内部转成 int）
  * @param  len   **最小**显示宽度（不足时左侧补空格，超过时按实际位数显示）
  * @param  fg    文字颜色
  * @param  bg    文字底色
  * @note   原理：借用标准库 sprintf 的 "%*d"——星号表示宽度由参数给出，
  *         这样"补前导空格"这件事交给库函数，不用手写补位逻辑。
  *         注意补的是**空格**而不是 '0'；如果要补零，格式串应写成 "%0*d"。
  * @note   输出到屏幕上的空格同样会被 LCD_ShowText16 用 bg 色画出来，
  *         所以"补空格"同时起到了"擦掉这一格上原有内容"的作用，
  *         这正是这个函数存在的意义：让变化的数字不会留下残影。
  * @note   缓冲区 buf[16] 是按 32 位整数最大 11 位（含负号）留的余量。
  *         "%*d" 里的宽度是**最小**宽度，len 越大写出的字符越多，
  *         所以 len 必须先钳位，否则 sprintf 会写越界踩栈。
  *         这里把 len 钳到 15（与同文件 LCD_ShowBigNum 的做法一致）。
  * @note   本函数当前未被界面调用（menu.c 里都是自己拼好字符串再调
  *         LCD_ShowText16），保留作为通用工具函数。
  */
void LCD_ShowNum16(uint16_t x, uint16_t y, int32_t num, uint8_t len, uint16_t fg, uint16_t bg)
{
    char buf[16];

    /* 钳位：留 1 字节给结束符，最多 15 个字符 */
    if (len > 15u) len = 15u;

    sprintf(buf, "%*d", len, (int)num);     /* 宽度不足左侧补空格 */
    LCD_ShowText16(x, y, buf, fg, bg);
}

/**
  * @brief  用 16x32 大号数字画 "HH:MM:SS" 时钟
  * @param  x,y   时钟左上角坐标
  * @param  h,m,s 时、分、秒（各 1 字节）
  * @param  fg    数字颜色
  * @param  bg    数字底色
  * @note   原理：先用 sprintf 把三个数拼成固定 8 字符的 "HH:MM:SS"
  *         （补零由 %02d 完成），再逐个字符取 16x32 点阵去画。
  *         因为格式固定，8 个字符的宽度恒定，所以**字符位置在屏幕上完全不动**，
  *         每秒整帧重画也不会看到位置跳动。
  * @note   数字与冒号共用一张 font_digit_16x32 表：
  *         下标 0~9 是数字，下标 10 是冒号（冒号也做成 16x32 满格，
  *         这样不需要为它单独留空位，排版宽度自然对齐）。
  * @note   continue 的隐患：非数字非冒号的分支只 continue 而**不推进 xx**，
  *         若真出现这种字符，后面的字会画在同一位置重叠。
  *         当前 sprintf 的输出只可能是数字和冒号，所以实际不会触发。
  * @note   本函数不做居中，居中由调用方（menu.c 用 (LCD_W - w)/2）负责。
  */
void LCD_ShowBigClock(uint16_t x, uint16_t y, uint8_t h, uint8_t m, uint8_t s,
                      uint16_t fg, uint16_t bg)
{
    char    buf[9];     /* 8 个字符 + 结束符 */
    uint16_t xx = x;
    int     i;

    /* 先把入参钳到 0~99：%02d 只是"最少两位"，不会截断更长的数字，
       若 h/m/s 超过 99（形参是 uint8_t，最大 255）会写出 12 字节，
       撑破 buf[9] 踩栈。时分秒的合法范围本就在 0~99 内，
       钳一下既不影响正常调用，也堵住了这个栈溢出。 */
    if (h > 99u) h = 99u;
    if (m > 99u) m = 99u;
    if (s > 99u) s = 99u;

    /* %02d 既保证补零，也保证每段恒为两位，从而总宽度恒为 8 字符 */
    sprintf(buf, "%02d:%02d:%02d", (int)h, (int)m, (int)s);
    for (i = 0; i < 8; i++)
    {
        char ch = buf[i];
        const uint8_t *data;
        if (ch >= '0' && ch <= '9')
        {
            data = font_digit_16x32[ch - '0'];      /* 数字：下标 0~9 */
        }
        else if (ch == ':')
        {
            data = font_digit_16x32[10];            /* 冒号：下标 10 */
        }
        else
        {
            continue;                               /* 理论不可达 */
        }
        LCD_DrawGlyph(xx, y, 16, 32, data, fg, bg);
        xx += 16;                                   /* 每个字符固定 16 像素宽 */
    }
}

/**
  * @brief  右对齐显示一个无符号整数的"大数字"版本（16x32 字模）
  * @param  x,y    左上角坐标
  * @param  num    要显示的数值
  * @param  width  显示位数（会被夹到 1~10 之间）
  * @param  fg     数字颜色
  * @param  bg     数字底色
  * @note   原理：把 num 从低位往高位逐位取模拆成字符，填进 buf 的**尾部**，
  *         于是 buf 天然就是右对齐的；高位不足的部分由循环起点决定，
  *         那些位置根本不会被写入 buf，也就不会被画出来（而不是补空格），
  *         所以本函数是"最少位数显示"而不是"定宽显示"。
  * @note   **刻意全程使用整数运算（% 10 与 / 10）而不用浮点**：
  *         若用浮点做等比缩放（例如按 width 缩放字形的横向比例），
  *         每次乘除都会引入舍入误差，误差逐字累积后会让相邻字符
  *         落在偏离 16 像素整数倍的位置上，表现为字距忽宽忽窄、
  *         数字像"散开"了一样；而整数取模/整除的结果是精确的，
  *         字符位置始终是 16 的整数倍，画面绝对整齐。
  *         （这也是为什么本函数与 LCD_ShowBigClock 共用同一套 16x32 字模，
  *           不另做缩放字库。）
  * @note   夹取 width 的理由：buf[12] 只够 10 位数字 + 结束符，
  *         超过 10 位会写越界；width 为 0 时循环一次都不执行（buf 空），
  *         所以下限强制为 1。
  * @note   参数名 width 容易误解为"横向拉伸倍数"，但函数体里并没有任何
  *         拉伸逻辑，它实际是"位数"。当前界面未调用本函数。
  */
void LCD_ShowBigNum(uint16_t x, uint16_t y, uint32_t num, uint8_t width,
                    uint16_t fg, uint16_t bg)
{
    char buf[12];
    int  i;

    if (width > 10u) width = 10u;   /* 上限：防止写越界 */
    if (width == 0)  width = 1;     /* 下限：至少显示一位 */

    /* 有意采用整数运算：浮点缩放的舍入误差会累积，
       导致相邻字形之间的间距错位（详见函数头注释） */
    for (i = (int)width - 1; i >= 0; i--)
    {
        buf[i] = (char)('0' + (num % 10u));     /* 取最低位 */
        num /= 10u;                             /* 去掉最低位，准备下一次 */
    }
    buf[width] = 0;                 /* 手动补结束符（没走 sprintf，得自己加） */

    for (i = 0; i < (int)width; i++)
    {
        char ch = buf[i];
        const uint8_t *data;
        if (ch >= '0' && ch <= '9')
        {
            data = font_digit_16x32[ch - '0'];
        }
        else if (ch == ':')
        {
            data = font_digit_16x32[10];    /* 保留冒号支持，便于复用同一张字模 */
        }
        else
        {
            continue;                       /* 理论不可达 */
        }
        LCD_DrawGlyph(x, y, 16, 32, data, fg, bg);
        x += 16;                            /* 逐字符右移，间距恒为 16 像素 */
    }
}

/**
  * @brief  计算一串 GBK 文本按 16 号字显示时占多少像素宽
  * @param  str  GBK 编码字符串
  * @retval 像素宽度（不含任何边距）
  * @note   必须与 LCD_ShowText16 的推进规则**完全一致**，否则算出来的
  *         居中位置会偏。这里逐条对齐：
  *           ASCII（<0x80）        -> 8 像素，消费 1 字节
  *           汉字（>=0x80 且后有字节）-> 16 像素，消费 2 字节
  *           半个汉字（后面是 '\0'）-> **按 0 像素算**，消费 1 字节
  * @note   最后那条最容易写错，也曾经真的写错过：LCD_ShowText16 遇到
  *         "汉字首字节之后就是 '\0'"时是 `str++` 然后 continue —— 既没画，
  *         也**没有推进游标**，即实际占 0 像素。本函数原先按 8 像素算，
  *         于是量出来的宽度比实际多 8 像素，居中的文字会整体偏左 4 像素、
  *         右对齐的会偏左 8 像素。现已与 LCD_ShowText16 严格对齐为 0 像素。
  *         保持两处规则一致是这段代码正确性的关键（本文件的设计契约）。
  * @note   界面里所有"水平居中"（(LCD_W - w) / 2）都依赖本函数，
  *         所以它虽小，却决定了整个界面的对齐观感。
  */
uint16_t LCD_TextWidth16(const char *str)
{
    uint16_t w = 0;

    while (*str)
    {
        if ((uint8_t)*str < 0x80) { w += 8; str++; }        /* ASCII 半角 8 像素 */
        else if (str[1] != 0)      { w += 16; str += 2; }   /* 完整汉字 16 像素 */
        else                       { str++; }              /* 半个汉字：不画也不推进 → 0 像素
                                                                   （必须与 LCD_ShowText16 一致，
                                                                    否则居中/右对齐会偏移） */
    }
    return w;
}
