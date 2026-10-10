/**
  ******************************************************************************
  * @file    beep.h
  * @brief   蜂鸣器模块: 5V有源蜂鸣器, 支持定时鸣叫
  ******************************************************************************
  * 引脚定义:
  *   - 蜂鸣器 : PB8  高电平触发
  * 修改引脚请修改本头文件 BEEP_PORT / BEEP_PIN 宏.
  ******************************************************************************
  */
#ifndef __BEEP_H__
#define __BEEP_H__

#include "stm32f10x.h"

/* 蜂鸣器引脚 */
#define BEEP_PORT   GPIOB
#define BEEP_PIN    GPIO_Pin_8
#define BEEP_CLK    RCC_APB2Periph_GPIOB

/* 初始化蜂鸣器 GPIO (推挽输出) */
void Beep_Init(void);

/* 直接开关 */
void Beep_On(void);
void Beep_Off(void);

/* 启动鸣叫, 持续 seconds 秒后自动关闭 */
void Beep_Start(uint32_t seconds);

/* 立即停止鸣叫 */
void Beep_Stop(void);

/* 主循环周期调用: 检查计时结束自动关闭 */
void Beep_Process(void);

/* 当前是否正在鸣叫 */
uint8_t Beep_IsActive(void);

/* 剩余鸣叫秒数 */
uint32_t Beep_GetRemain(void);

#endif /* __BEEP_H__ */
