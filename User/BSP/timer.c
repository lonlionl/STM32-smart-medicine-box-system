/**
  ******************************************************************************
  * @file    timer.c
  * @brief   系统时基模块实现
  ******************************************************************************
  */
#include "timer.h"

/* 系统运行毫秒计数 (volatile: 中断中修改) */
static volatile uint32_t g_TickMs = 0;

/**
  * @brief  初始化 SysTick, 配置为 1ms 中断
  * @note   SystemCoreClock 由 system_stm32f10x.c 提供(72MHz)
  */
void Timer_Init(void)
{
    /* SysTick 使用 HCLK/8 时钟源(默认), 1ms = SystemCoreClock/8/1000 个周期 */
    if (SysTick_Config(SystemCoreClock / 1000))
    {
        /* 配置失败(返回值非0), 死循环等待 */
        while (1);
    }
    /* 将 SysTick 中断优先级设为最低, 避免影响外设中断实时性 */
    NVIC_SetPriority(SysTick_IRQn, 0x0F);
}

/**
  * @brief  SysTick 中断服务函数(由 stm32f10x_it.c 的 SysTick_Handler 调用)
  */
void Timer_Tick(void)
{
    g_TickMs++;
}

/**
  * @brief  获取当前系统毫秒数
  * @retval 32位毫秒计数, 可直接做差值运算(处理回绕)
  */
uint32_t Timer_GetTick(void)
{
    return g_TickMs;
}

/**
  * @brief  阻塞延时
  * @param  ms: 延时毫秒数(最大约49天)
  * @note   仅用于初始化/上电时序等场合, 主循环内禁止使用
  */
void Timer_DelayMs(uint32_t ms)
{
    uint32_t start = Timer_GetTick();
    while ((uint32_t)(Timer_GetTick() - start) < ms)
    {
    }
}
