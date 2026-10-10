/**
  ******************************************************************************
  * @file    timer.h
  * @brief   系统时基模块: 基于 SysTick 的 1ms 软件定时器
  ******************************************************************************
  * 说明:
  *   - 提供 1ms 系统滴答, 供按键消抖、蜂鸣计时、界面刷新等使用
  *   - Timer_DelayMs 用于初始化阶段阻塞延时(如LCD复位、PCA9685上电)
  ******************************************************************************
  */
#ifndef __TIMER_H__
#define __TIMER_H__

#include "stm32f10x.h"

/* 初始化 SysTick, 产生 1ms 中断 (需在 SysTick_Handler 中调用 Timer_Tick) */
void Timer_Init(void);

/* SysTick 中断服务, 由 stm32f10x_it.c 调用 */
void Timer_Tick(void);

/* 获取当前系统运行毫秒数 (32位, 约49.7天回绕, 差值计算不受影响) */
uint32_t Timer_GetTick(void);

/* 阻塞延时毫秒 (仅在初始化等对实时性要求不高的场合使用) */
void Timer_DelayMs(uint32_t ms);

#endif /* __TIMER_H__ */
