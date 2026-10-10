/**
  ******************************************************************************
  * @file    menu.h
  * @brief   分级菜单与界面绘制模块
  ******************************************************************************
  * 界面层级:
  *   主界面 -> 药盒选择 -> 周几选择 -> 时间设置(整点小时)
  *   【返回】逐级回退; 设置保存后写入 Flash 备份
  ******************************************************************************
  */
#ifndef __MENU_H__
#define __MENU_H__

#include "stm32f10x.h"
/* ==================== 界面布局常量 (320x480 屏) ==================== */
#define TITLE_BAR_H     40u      /* 顶部标题栏高度 */
#define ITEM_X0         20u      /* 列表项左边界 */
#define ITEM_X1         300u     /* 列表项右边界 */
#define ITEM_ROW_H      40u      /* 列表项行高 */
#define ITEM_Y0         50u      /* 列表起始 y */
#define MAIN_BOX_Y0     190u     /* 主界面药盒列表起始 y */
#define BOX_ROW_H       30u      /* 主界面药盒行高 */
#define MAIN_MSG_TIME_MS   3000u /* 主界面消息显示时长(毫秒) */
#define TOAST_TIME_MS      1000u /* Toast 提示显示时长(毫秒) */
#include "app.h"

/* 界面编号 */
typedef enum
{
    SCR_MAIN = 0,       /* 主界面 */
    SCR_BOX_SEL,        /* 药盒选择 */
    SCR_DAY_SEL,        /* 周几选择 */
    SCR_TIME_SET        /* 时间设置 */
} ScrId_t;

/* 初始化菜单与主界面 */
void Menu_Init(void);

/* 按键处理 */
void Menu_OnKey(uint16_t keys);

/* 主循环周期调用: 定时刷新 */
void Menu_Tick(void);

/* 强制刷新主界面(返回主界面/药盒复位时调用) */
void Menu_RefreshMain(void);

/* 标记药盒区需重绘 */
void Menu_NotifyBoxChange(void);

/* 在主界面显示一条提示消息(3秒) */
void Menu_ShowMainMsg(const char *str);

/* 在设置页底部显示提示信息(1秒) */
void Menu_ShowToast(const char *str);

/* 当前是否处于主界面 */
uint8_t Menu_IsInMain(void);

#endif /* __MENU_H__ */
