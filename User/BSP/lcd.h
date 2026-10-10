/**
  ******************************************************************************
  * @file    lcd.h
  * @brief   TFTLCD 驱动接口声明：ILI9488 控制器、FSMC 并口、320x480 竖屏
  ******************************************************************************
  * 一、硬件连接（ALIENTEK 精英版，16 位并口接 FSMC）
  * ---------------------------------------------------------------------------
  *   LCD_CS  <- FSMC_NE4 (PG12)     片选，选中 0x6C000000 起的 64MB 空间
  *   LCD_RS  <- FSMC_A10 (PG0)      命令/数据选择，是本驱动"用地址区分命令与数据"的基础
  *   LCD_WR  <- FSMC_NWE (PD5)      写选通，由 FSMC 硬件自动产生
  *   LCD_RD  <- FSMC_NOE (PD4)      读选通，仅读 ID 时用到
  *   LCD_RST <- PD3                 硬复位，普通 GPIO，低有效
  *   LCD_BL  <- PB0                 背光，普通 GPIO（精英版），高电平点亮
  *   数据线  <- FSMC_D0~D15（PD0,PD1,PD8~PD15,PE7~PE15）
  ******************************************************************************
  * 二、命令/数据地址是怎么来的（本驱动最核心的一处设计）
  * ---------------------------------------------------------------------------
  *   FSMC 把外设映射进 CPU 线性地址空间，本屏挂在 Bank1 的 NE4 子区，
  *   基址就是 0x6C000000。板子上又把 **FSMC_A10 接到了屏的 RS 脚**，
  *   于是访问哪个地址就等于给 RS 什么电平：
  *       访问 0x6C000000（A10 = 0）-> RS = 0 -> 写**命令**（LCD_REG_ADDR）
  *       访问 0x6C000800（A10 = 1）-> RS = 1 -> 写**数据**（LCD_RAM_ADDR）
  *   这样区分命令/数据不需要任何 GPIO 翻转，一句指针赋值就能完成一次
  *   完整的"片选 + 地址 + 写选通"时序，所以下面两个宏看起来只是普通内存写。
  * @note 0x800 是 1<<11；因为数据宽度是 16 位，FSMC 的 A10 对应 CPU 的
  *       HADDR[11]，所以偏移量要写成 1 << (LCD_RS_BIT + 1)，而不是 1 << 10。
  ******************************************************************************
  */
#ifndef __LCD_H__
#define __LCD_H__

#include "stm32f10x.h"
#include "font.h"

/* ==================== 屏幕尺寸 ==================== */
/* 3.5 寸 ILI9488 竖屏：宽 320、高 480（由初始化时 MADCTL=0x48 的 MV 位决定）。
   这两个常量是对外契约：上层界面布局、居中计算、清屏范围全都基于它们，
   改屏型（比如换 480x320 横屏）时必须连同 MADCTL 一起改。 */
#define LCD_W   320u                     /* 宽 320（ILI9488 竖屏） */
#define LCD_H   480u                     /* 高 480（ILI9488 竖屏） */

/* ==================== 颜色（RGB565） ==================== */
/* 16 位色格式：R 占高 5 位、G 占中间 6 位、B 占低 5 位。
   G 多一位是因为人眼对绿色最敏感，这样分配能把 16 位的表现力用在刀刃上。
   这些常量按 **RGB** 顺序书写，能正确显示是靠初始化时 MADCTL 的 BGR 位
   （0x48 里的 bit3）把面板的 BGR 子像素顺序换过来；
   若哪天把 MADCTL 改成 0x40，屏上所有颜色都会红蓝互换。 */
#define WHITE       0xFFFF
#define BLACK       0x0000
#define BLUE        0x001F
#define DARKBLUE    0x0010
#define RED         0xF800
#define GREEN       0x07E0
#define CYAN        0x07FF
#define YELLOW      0xFFE0
#define MAGENTA     0xF81F
#define ORANGE      0xFD20
#define GRAY        0x8410
#define LGRAY       0xC618
#define DGRAY       0x4208
/* 以下三个是界面配色常量（不是纯色，而是"某个区域该用什么底色"的约定），
   由 menu.c 直接引用，改这里就等于统一换肤。
   注意 HILITE_BG 与 TITLE_BG 取值相同：当前界面里选中项与标题栏用同一底色，
   这不是笔误，是保持配色一致的有意选择。 */
#define TITLE_BG    0x001F
#define HILITE_BG   0x001F
#define HINT_BG     0xC618

/* ==================== FSMC 地址与 RS 的对应关系 ==================== */
/* 精英版：LCD_RS 接在 FSMC_A10 上（对应 PG0） */
#define LCD_RS_BIT  10

/* 精英版：LCD 片选 FSMC_NE4 (PG12)，基址 0x6C000000 */

/* 命令寄存器地址：A10 = 0，即 RS = 0。
   写成 volatile 指针是必须的——否则编译器会认为"往同一个地址反复写同样的值"
   是无效操作而优化掉，那些重复下发的命令就会消失。 */
#define LCD_REG_ADDR ((volatile uint16_t *)(0x6C000000UL))
/* 数据寄存器地址：把 A10 置 1 得到的偏移 0x800（= 1<<11，见文件头说明），
   其余地址位保持为 0，所以和 REG_ADDR 落在同一个 NE4 子区里。 */
#define LCD_RAM_ADDR ((volatile uint16_t *)(0x6C000000UL | (1UL << (LCD_RS_BIT + 1))))

/* 两个唯一的底层写入口：全文件所有 LCD 操作最终都归结到这两条指针赋值。
   宏里统一强转 uint16_t，防止调用方传进 32 位值导致 FSMC 按错误宽度访问。 */
#define LCD_WR_REG(reg)   (*LCD_REG_ADDR = (uint16_t)(reg))
#define LCD_WR_DATA(dat)  (*LCD_RAM_ADDR = (uint16_t)(dat))

/* 背光（精英版：PB0）
   集中定义在这里而不是直接在 lcd.c 里写死，是为了换板子时只改这三行；
   若以后要 PWM 调光，改这三项即可（并把模式从推挽改成复用）。 */
#define LCD_BL_PORT GPIOB
#define LCD_BL_PIN  GPIO_Pin_0
#define LCD_BL_CLK  RCC_APB2Periph_GPIOB

/* ==================== 对外接口 ==================== */
/* 初始化：GPIO + FSMC + ILI9488 寄存器序列，返回时屏幕已被清成白色 */
void LCD_Init(void);
/* 读控制器 ID，用于开机自检打印（ILI9488 应读回 0x9488） */
uint16_t LCD_ReadID(void);
/* 绘图原语：坐标一律"含端点"，即 (0,0)-(319,479) 是整屏 */
void LCD_Clear(uint16_t color);                                              /* 单色清整屏 */
void LCD_FillRect(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color);  /* 实心矩形 */
void LCD_SetPixel(uint16_t x, uint16_t y, uint16_t color);                   /* 单像素（最慢，慎用） */
void LCD_DrawLine(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color);  /* 直线，Bresenham */
void LCD_DrawRect(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color);  /* 空心矩形 */
/* 设置 GRAM 写入窗口，通常不必直接调用（各绘图函数内部已调） */
void LCD_SetWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);
/* 文本：str 必须是 GBK 编码；fg/bg 都要给，bg 用于擦掉该位置原有内容 */
void LCD_ShowText16(uint16_t x, uint16_t y, const char *str, uint16_t fg, uint16_t bg);  /* 混排 8x16/16x16 */
void LCD_ShowText24(uint16_t x, uint16_t y, const char *str, uint16_t fg, uint16_t bg);  /* 纯中文 24x24，ASCII 跳过 */
void LCD_ShowNum16(uint16_t x, uint16_t y, int32_t num, uint8_t len, uint16_t fg, uint16_t bg);  /* 定宽整数 */
/* 大号时钟与数字（16x32 字模），用于主界面的时间显示 */
void LCD_ShowBigClock(uint16_t x, uint16_t y, uint8_t h, uint8_t m, uint8_t s,
                      uint16_t fg, uint16_t bg);
/* 大数字版本的无符号整数显示。
   注意参数名 width 是"显示位数"（会被夹到 1~10），并非横向拉伸倍数；
   由于函数体里没有任何缩放逻辑，本函数只用于同宽场景。
   该函数当前未被界面调用，保留作为通用工具。 */
void LCD_ShowBigNum(uint16_t x, uint16_t y, uint32_t num, uint8_t width,
                    uint16_t fg, uint16_t bg);
/* 量出字符串按 16 号字显示时的像素宽度（ASCII 算 8、汉字算 16），
   界面里所有水平居中都用它：(LCD_W - LCD_TextWidth16(s)) / 2。
   其推进规则必须与 LCD_ShowText16 完全一致，否则居中会偏。 */
uint16_t LCD_TextWidth16(const char *str);

#endif /* __LCD_H__ */
