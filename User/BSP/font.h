/**
  ******************************************************************************
  * @file    font.h
  * @brief   LCD 点阵字库声明（自动生成，请勿手工修改）
  ******************************************************************************
  * 本文件由 tools/gen_font.py 生成。
  * 要改字库（增删汉字、换字体、改字号），请改脚本后重新运行，不要直接改本文件。
  *
  * 点阵排布约定（4 张表统一遵守）:
  *   - 按行取模，每行占 (宽 + 7) / 8 个字节，高位在左（MSB 在屏幕左侧）；
  *   - 某位为 1 表示该像素点亮（前景色），为 0 表示背景色；
  *   - 字节数换算: 8x16 = 16 字节, 16x16 = 32 字节, 16x32 = 64 字节, 24x24 = 72 字节。
  *
  * 为什么中文表要单独建索引:
  *   GBK 汉字是双字节，无法像 ASCII 那样直接用字符码当下标；
  *   故用 { GBK 双字节码, 点阵指针 } 的索引表，并**按 GBK 码升序排列**，
  *   这样查字时可以做**二分查找**（见 lcd.c 的 FontGbk_Find16/FontGbk_Find24）。
  ******************************************************************************
  */
#ifndef __FONT_H__
#define __FONT_H__
#include "stm32f10x.h"

/* ASCII 8x16 点阵表: 每字符 16 字节, 下标 = 字符码 - 0x20 (覆盖 0x20~0x7E 共 95 个) */
extern const uint8_t font_ascii_8x16[95][16];

/* 大号时钟数字 16x32: 每字符 64 字节, 下标 = 字符 - 0x30; 冒号 ':' 固定放在下标 10 */
extern const uint8_t font_digit_16x32[11][64];

/* ==================== 中文 GBK 16x16（界面正文用） ==================== */
/* 索引项: gbk = 该汉字的 GBK 双字节码（高字节在前）, data = 指向 32 字节点阵 */
typedef struct { const uint8_t gbk[2]; const uint8_t *data; } FontGbk16_t;
extern const FontGbk16_t font_gbk16[];   /* 按 gbk 升序，供二分查找 */
extern const uint16_t font_gbk16_num;     /* 表项个数（即收录的汉字数） */

/* ==================== 中文 GBK 24x24（标题/大字用） ==================== */
/* 结构同上；只收录 TITLE_STRINGS 用到的字，以压缩 Flash 占用 */
typedef struct { const uint8_t gbk[2]; const uint8_t *data; } FontGbk24_t;
extern const FontGbk24_t font_gbk24[];
extern const uint16_t font_gbk24_num;

#endif /* __FONT_H__ */
