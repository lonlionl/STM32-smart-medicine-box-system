/**
  ******************************************************************************
  * @file    mqtt_huawei.h
  * @brief   Huawei Cloud IoTDA MQTT client (via ESP-01S AT)
  ******************************************************************************
  * ============ CONFIGURE BEFORE USE ============
  * 1. Set WIFI_SSID / WIFI_PASSWORD for your AP.
  * 2. Set HUAWEI_DEVICE_ID / HUAWEI_DEVICE_SECRET from Huawei Cloud console.
  *    - device_id and secret can be found on the device details page.
  *    - Also in DEVICES-KEY-*.txt in the project root.
  * 3. Set HUAWEI_HOST to your region endpoint.
  * 4. Product model (service: medicine_box_service):
  *      properties: box_num(int), chufa_text(string), chufa_time(string)
  *      command:    remote_open_box(para: target_box)
  * ==============================================
  */
#ifndef __MQTT_HUAWEI_H__
#define __MQTT_HUAWEI_H__

#include "stm32f10x.h"

/* ==================== user configuration ==================== */
#define WIFI_SSID            "<YOUR_WIFI_SSID>"
#define WIFI_PASSWORD        "<YOUR_WIFI_PASSWORD>"

/* Huawei Cloud IoTDA device info */
#define HUAWEI_DEVICE_ID     "<YOUR_DEVICE_ID>"
#define HUAWEI_DEVICE_SECRET "<YOUR_DEVICE_SECRET>"
#define HUAWEI_HOST          "<YOUR_IOTDA_DEVICE_ENDPOINT>"
#define HUAWEI_PORT          1883u

/* MQTT scheme: 1 = MQTT over TCP */
#define HUAWEI_MQTT_SCHEME   1u
/* Fixed clientId and password from Huawei Cloud console (must match each other).
   clientId timestamp and password are generated together by the console. */
#define HUAWEI_CLIENT_ID     "<YOUR_CLIENT_ID>"
#define HUAWEI_STATIC_PWD    "<YOUR_MQTT_PASSWORD>"

/* 连接失败后的重试间隔(毫秒) */
#define RETRY_INTERVAL_MS  5000u
/* 两次 PUBRAW 命令之间的最小间隔(避免固件拒绝紧接的命令) */
#define REPLY_RETRY_GAP_MS  300u

/* Service ID of the product model */
#define HUAWEI_SERVICE_ID    "medicine_box_service"
/* Command ID for remote open box */
#define HUAWEI_CMD_OPEN      "remote_open_box"

/* ============================================================ */

/* Link state */
typedef enum
{
    LNK_NONE = 0,       /* not connected */
    LNK_WIFI,           /* WiFi connected / connecting */
    LNK_MQTT,           /* MQTT connected */
    LNK_READY           /* MQTT connected and subscribed */
} LinkState_t;

/* Init MQTT client (call after peripherals) */
void MQTT_Init(void);

/* Poll loop: drive state machine and data reception */
void MQTT_Process(void);

/* WiFi connected? */
uint8_t MQTT_IsWifiConnected(void);

/* MQTT ready (report/subscribe allowed) */
uint8_t MQTT_IsReady(void);

/* Get link state (for LCD display) */
LinkState_t MQTT_GetLinkState(void);

/* Report medicine-taking record via property report.
   box: 0~6, mode: 0=timer trigger 1=remote trigger */
void MQTT_PublishRecord(uint8_t box, uint8_t mode);

/* Report full timer schedule as a property (for the phone app to read) */
void MQTT_PublishSchedule(void);

/* Report that all boxes are closed (box_num=0), for the phone app UI */
void MQTT_PublishClose(void);

#endif /* __MQTT_HUAWEI_H__ */
