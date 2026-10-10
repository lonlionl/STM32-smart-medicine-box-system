/**
  ******************************************************************************
  * @file    esp01s_at.h
  * @brief   ESP-01S AT command driver (for MQTT-enabled AT firmware)
  ******************************************************************************
  * Communicate with ESP-01S via USART2 (PA2/PA3).
  * Provides command send / response match, URC handling (MQTT data, link,
  * NTP time), and raw payload publish for AT+MQTTPUBRAW.
  *
  * Supported MQTT AT commands:
  *   AT+MQTTCONN / AT+MQTTSUB / AT+MQTTPUB / AT+MQTTPUBRAW / AT+MQTTDISCONN
  *   URC: +MQTTCONNECTED / +MQTTSUBACK / +MQTTSUBRECV / +MQTTDISCONNECTED
  ******************************************************************************
  */
#ifndef __ESP01S_AT_H__
#define __ESP01S_AT_H__

#include "stm32f10x.h"
/* 接收行缓冲长度(需容纳完整 +MQTTSUBRECV 行, 含长 topic+数据) */
#define ESP_LINE_MAX     2048u
/* MQTT 接收数据缓冲长度(属性设置消息最长约 1700 字符) */
#define ESP_DATA_MAX     2048u

/* Publish-complete callback: result=1 success, 0 failure */
typedef void (*EspPubDoneCb_t)(uint8_t result);

/* MQTT data callback: called when a subscribed message is received.
   topic: the topic of the received message, data/len: payload. */
typedef void (*EspDataCb_t)(const char *topic, const char *data, uint16_t len);

/* NTP time callback: called when +CIPSNTPTIME returns the time */
typedef void (*EspNtpCb_t)(const char *timeStr);

/* Disconnect callback: called when +MQTTDISCONNECTED is received */
typedef void (*EspDisconnectCb_t)(void);

/* Init AT driver (call after USART_InitAll) */
void ESP_Init(void);

/* Poll loop: parse RX data and process command state machine */
void ESP_Process(void);

/* Send AT command and register expected response; return 1 if accepted */
uint8_t ESP_CmdSend(const char *cmd, const char *expect, uint32_t timeoutMs);

/* Publish raw payload via AT+MQTTPUBRAW (two-step: send cmd, wait '>', send data)
   topic: MQTT topic, data/len: payload bytes, done: completion callback (may be NULL) */
uint8_t ESP_PubRaw(const char *topic, const uint8_t *data, uint16_t len,
                   uint8_t qos, EspPubDoneCb_t done);

/* Whether a command is currently pending */
uint8_t ESP_CmdBusy(void);

/* Command result: 0=pending 1=success 2=fail/timeout */
uint8_t ESP_CmdResult(void);

/* Register callbacks */
void ESP_RegDataCb(EspDataCb_t cb);
void ESP_RegNtpCb(EspNtpCb_t cb);
void ESP_RegDisconnectCb(EspDisconnectCb_t cb);

#endif /* __ESP01S_AT_H__ */
