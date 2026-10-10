/**
  ******************************************************************************
  * @file    beep.c
  * @brief   蜂鸣器模块实现
  ******************************************************************************
  */
#include "beep.h"
#include "timer.h"

/* 鸣叫结束的绝对毫秒时刻(用于比较) */
static volatile uint32_t g_BeepEndTick = 0;
static volatile uint8_t  g_BeepActive  = 0;

/**
  * @brief  初始化蜂鸣器 GPIO
  */
void Beep_Init(void)
{
    GPIO_InitTypeDef gpio;

    RCC_APB2PeriphClockCmd(BEEP_CLK, ENABLE);

    gpio.GPIO_Pin   = BEEP_PIN;
    gpio.GPIO_Mode  = GPIO_Mode_Out_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(BEEP_PORT, &gpio);

    Beep_Off();
}

/**
  * @brief  蜂鸣器响
  */
void Beep_On(void)
{
    GPIO_SetBits(BEEP_PORT, BEEP_PIN);
}

/**
  * @brief  蜂鸣器停
  */
void Beep_Off(void)
{
    GPIO_ResetBits(BEEP_PORT, BEEP_PIN);
}

/**
  * @brief  启动鸣叫
  * @param  seconds: 持续秒数
  */
void Beep_Start(uint32_t seconds)
{
    g_BeepEndTick = Timer_GetTick() + seconds * 1000u;
    g_BeepActive  = 1;
    Beep_On();
}

/**
  * @brief  立即停止鸣叫
  */
void Beep_Stop(void)
{
    g_BeepActive = 0;
    Beep_Off();
}

/**
  * @brief  周期调用: 计时结束自动关闭
  */
void Beep_Process(void)
{
    if (g_BeepActive)
    {
        /* 带符号差值判断是否到达结束时刻(避免32位回绕问题) */
        if ((int32_t)(Timer_GetTick() - g_BeepEndTick) >= 0)
        {
            g_BeepActive = 0;
            Beep_Off();
        }
    }
}

/**
  * @brief  当前是否正在鸣叫
  */
uint8_t Beep_IsActive(void)
{
    return g_BeepActive;
}

/**
  * @brief  剩余鸣叫秒数(向上取整)
  */
uint32_t Beep_GetRemain(void)
{
    int32_t diff;

    if (!g_BeepActive)
    {
        return 0;
    }
    diff = (int32_t)(g_BeepEndTick - Timer_GetTick());
    if (diff <= 0)
    {
        return 0;
    }
    return (uint32_t)(((uint32_t)diff + 999u) / 1000u);
}
