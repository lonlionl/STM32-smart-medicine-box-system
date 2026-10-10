/**
  ******************************************************************************
  * @file    flash.h
  * @brief   内部 Flash 配置备份模块: 定时配置的掉电保存(含魔数与 CRC32 校验)
  ******************************************************************************
  * 说明:
  *   - 定时配置(7 药盒 x 7 天 x 每天 4 个时间点)存于片内 Flash 最后一页, 掉电不丢失
  *   - 结构含魔数与 CRC32 校验, 防止读到"半截写入"的脏数据
  *   - STM32F103ZE 为大容量产品, 每页 2KB, 末页地址 0x0807F800
  *
  * 本模块的使用约定(上层 app.c 必须遵守)
  *   1. 上电时先 Flash_ReadConfig(&g_Cfg), 返回 0 就用默认配置填满 g_Cfg;
  *   2. 修改 g_Cfg(或它的副本)后调用 Flash_SaveConfig(), 由它负责重算 CRC;
  *   3. 不要在业务代码里直接访问 0x0807F800 这个地址。
  *
  * 修改结构体时的强制要求
  *   - 只要动了 TimerCfg_t / BoxRule_t 的字段、顺序或数组维度,
  *     **必须同时修改 FLASH_CFG_MAGIC**。原因: 旧固件按旧布局写下的数据,
  *     仅仅靠 magic 是分辨不出"格式变了"的, 会把旧字节按新布局解释;
  *     换了 magic 之后旧数据直接判无效, 平稳回落到默认配置。
  ******************************************************************************
  */
#ifndef __FLASH_H__
#define __FLASH_H__

#include "stm32f10x.h"
#include "app.h"

/* Flash 配置区地址(最后一页) */
#define FLASH_CFG_ADDR    0x0807F800u   /* 512KB 片内 Flash 的最后一页首地址:
                                           0x08000000 + 512K - 2K = 0x0807F800。
                                           必须与擦除粒度(整页 2KB)对齐,
                                           否则 FLASH_ErasePage 会连带擦掉别的页 */
#define FLASH_CFG_MAGIC   0xA5A5C7C7u   /* 配置有效标志。选这个值只为"不像
                                           随机数据/全 0/全 F", 无算法含义。
                                           改动结构体布局时必须同步改这里 */

/* 单个药盒的定时规则: 按星期编辑(天维度), 每天最多 BOX_MAX_TIMES 个时间点(槽维度)
 *
 * 维度含义: 第一维 = 星期(0=周一 ... 6=周日), 第二维 = 当天第几个提醒
 * 大小: 2 x 7 x 4 = 56 字节, 全部是 uint8_t, 无对齐填充, 可安全 memcpy
 *
 * 【时间槽的特殊值约定 —— 最容易踩的坑】
 *   hour[天][槽] == 0xFF 表示该槽**未设置**, 必须跳过;
 *   **绝不能用 0 表示未设置**, 因为 0 是合法时刻"0 点(午夜)" ——
 *   一旦用 0 当"空", 用户在 0 点设的提醒会静默失效,
 *   而所有空槽又会被当成"每天 0 点提醒"而误触发。
 *   min 字段只在对应 hour != 0xFF 时才有意义, 未用槽里的 min 是垃圾值。
 */
typedef struct
{
    uint8_t  hour[7][BOX_MAX_TIMES];  /* hour[天(0=周一)][时间点] = 0~23(小时), 0xFF=该槽未用 */
    uint8_t  min[7][BOX_MAX_TIMES];   /* min[天][时间点] = 0~59(分钟); 仅当对应 hour != 0xFF 时有效 */
} BoxRule_t;

/* 定时配置结构体: 一次 Flash 写入的完整单元
 *
 * 大小 = 4 + 7*56 + 4 = 400 字节(无填充: uint32_t 在前, 结构体内部自然 4 字节对齐,
 *        BoxRule_t 是纯 uint8_t 数组, 只要求 1 字节对齐, 因此没有空洞)
 *
 * 字段顺序即 Flash 中的字节顺序, 也是 CRC 的覆盖范围:
 *   magic(4) -> rules[0..6](392) -> crc(4)
 *
 * crc 只校验它**之前**的 396 字节, 不含 crc 自身 ——
 * 写入时先算 CRC 再填进去, 读取时用同一范围重算, 两边才能对上。
 */
typedef struct
{
    uint32_t  magic;                 /* 有效标志, 必须等于 FLASH_CFG_MAGIC = 0xA5A5C7C7 */
    BoxRule_t rules[BOX_NUM];        /* 每盒一条规则; 下标 0~6 对应 1~7 号药盒 */
    uint32_t  crc;                   /* CRC32 校验和, 覆盖范围为前 offsetof(crc)=396 字节 */
} TimerCfg_t;



/* 全局定时配置: 唯一真源。上电由 Flash_ReadConfig 填充,
   界面改时间后写回本变量再调 Flash_SaveConfig 落盘 */
extern TimerCfg_t g_Cfg;

/* 读取配置并校验; 返回 1=数据有效已填入 cfg, 0=无效(首次上电/已损坏, 勿使用 cfg 内容) */
uint8_t Flash_ReadConfig(TimerCfg_t *cfg);

/* 写入配置(拷副本 -> 重算 CRC -> 解锁 -> 擦整页 -> 半字编程 -> 上锁 -> 回读比对), 成功返回 1
   注意: 执行期间因 Flash 忙 CPU 取指暂停, 属于阻塞操作, 切勿放入主循环 */
uint8_t Flash_SaveConfig(const TimerCfg_t *cfg);

/* 计算标准 CRC-32/ISO-HDLC(查表法, 多项式 0xEDB88320): buf 起始的 len 字节 */
uint32_t Flash_CRC32(const uint8_t *buf, uint32_t len);

#endif /* __FLASH_H__ */
