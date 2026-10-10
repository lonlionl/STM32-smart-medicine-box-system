/**
  ******************************************************************************
  * @file    usart.h
  * @brief   串口模块（USART1 调试口 + USART2 接 ESP-01S 云链路）—— 接口声明
  ******************************************************************************
  * 模块职责：
  *   - USART1：调试/日志口，PA9(TX) PA10(RX)，115200-8-N-1。
  *     printf 通过 usart.c 里的 fputc 重定向到这里，是裸机上唯一的"控制台"。
  *   - USART2：ESP-01S（WiFi 模块）专用口，PA2(TX) PA3(RX)，115200-8-N-1。
  *     发送走阻塞式 Usart2_SendByte/Usart2_SendString 下发 AT 指令；
  *     接收走"中断逐字节入环形缓冲 + 主循环轮询取出"的非阻塞模型。
  *
  * 数据流（谁调用谁）：
  *   ESP-01S --UART--> USART2 硬件 --RXNE 中断--> Usart2_RxIsr()
  *            （只搬字节，不做任何解析）--> g_EspRxBuf[512] 环形缓冲
  *   主循环 --MQTT_Process()--> esp 驱动 --Usart2_RxCount()/Usart2_RxRead()--> 缓冲
  *   主循环 --AT 指令--> Usart2_SendString() --> USART2 --> ESP-01S
  *
  * 典型调用顺序（main.c）：
  *   NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2) 先定好分组，
  *   Timer_Init() 给 1ms 滴答，然后 USART_InitAll()，
  *   之后才允许 printf 与 ESP 驱动（MQTT_Init/MQTT_Process）使用本模块。
  *
  * 关键约束（改动前务必先读懂）：
  *   1) ESP_RX_BUF_SIZE 用 "% 尺寸" 取模而非 "& (尺寸-1)"，因此不要求 2 的幂；
  *      512 = 2^9 只是"足够放下一次完整 AT 应答的一行"的工程取值。
  *   2) 环形缓冲是单生产者单消费者（SPSC）：中断只写 head、主循环只读 tail，
  *      两个 index 各自只被一方改写，故本模块内部无需任何临界区/关中断保护。
  *   3) 中断优先级数值(1)必须小于 SysTick 的抢占优先级数值，
  *      这样 ESP 的连续数据流才不会被 1ms 滴答或主循环拖慢而丢包。
  ******************************************************************************
  * Pin mapping:
  *   - USART1: PA9(TX)  PA10(RX)   debug port, system log print
  *   - USART2: PA2(TX)  PA3(RX)    to ESP-01S
  * Modify baud rates in the macro definitions below.
  ******************************************************************************
  */
#ifndef __USART_H__
#define __USART_H__

#include "stm32f10x.h"
#include <stdio.h>          /* fputc 原型用到 FILE 类型，必须包含 */

/* Baud rates */
/* 调试口波特率。选 115200 是因为：与 ESP 口一致，便于把 ESP 的原始 AT 交互
   用同一套串口工具观察；且 1 字节约 87us，printf 单行不会明显拖慢启动。 */
#define DEBUG_BAUDRATE   115200u
/* ESP-01S 波特率。出厂固件 AT 默认 115200，改这里时 ESP 端也要同步改，
   否则 AT 指令无响应（表现为 ESP 驱动一直等超时）。 */
#define ESP_BAUDRATE     115200u

/* USART2 RX ring buffer size (bytes) */
/* USART2 接收环形缓冲字节数。容量 512 的用途：一次 MQTT 下行报文/一条 AT 应答
   可能上百字节，512 足以容纳"主循环还没轮到读"时的整包数据，避免丢弃。
   内部用取模回绕，所以这里不要求是 2 的幂。占用 512B 静态 RAM，无动态分配。 */
#define ESP_RX_BUF_SIZE  512u

/* Init USART1 + USART2 */
/* 作用：一次性初始化 USART1(调试) 与 USART2(ESP-01S)，含 GPIO、时钟、
   中断使能和 NVIC 优先级配置。
   参数：无。
   返回：无。
   原理：USART1 挂在 APB2(72MHz)、USART2 挂在 APB1(36MHz)，库函数按所挂总线的
   频率自动算 BRR 分频，所以必须先把两条总线的外设时钟都打开再配置。
   GPIO 侧 PA9/PA2 配成复用推挽(AF_PP) 承担 TX，PA10/PA3 配成浮空输入
   (IN_FLOATING) 承担 RX —— 接收引脚不能配上拉，否则空闲电平会和对方驱动冲突。
   最后使能 USART2 的 RXNE 中断并把它放进 NVIC（抢占优先级 1），
   让"来一个字节就立刻搬进缓冲"成为可能，主循环从此不必再等待串口。
   调用时机：必须在任何 printf / ESP 驱动动作之前，且要在
   NVIC_PriorityGroupConfig() 之后调用，否则优先级分组未定、中断配置无意义。 */
void USART_InitAll(void);

/* USART1 send one byte (blocking) */
/* 作用：向调试口 USART1 发送 1 字节。
   参数：ch —— 待发送字节（取低 8 位）。
   返回：无。
   原理：自旋等待 TC(发送完成) 标志后写 DR。属于阻塞发送，单字节约 87us@115200，
   只适合日志量很小的裸机调试场景；不要在需要实时响应的路径上循环调用。 */
void Usart1_SendByte(uint8_t ch);

/* USART2 send */
/* 作用：向 ESP-01S 发送 1 字节（阻塞）。
   参数：ch —— 待发送字节。
   返回：无。
   原理：与 Usart1_SendByte 相同，等 TC 后写 DR；调用者是 ESP 驱动，
   下发 "AT+..." 指令时逐字节调用。 */
void Usart2_SendByte(uint8_t ch);
/* 作用：向 ESP-01S 发送以 '\0' 结尾的 AT 指令字符串（不含结尾的 '\0'）。
   参数：s —— C 字符串指针，不可为 NULL。
   返回：无。
   原理：逐字节调用 Usart2_SendByte，因此整串发完前一直阻塞。
   注意：AT 指令必须以 "\r\n" 结尾，本函数不会自动补，需调用者自带。 */
void Usart2_SendString(const char *s);

/* USART2 RX ring buffer interface (used by ESP driver) */
/* 作用：查询 USART2 接收缓冲中"已收到但尚未取走"的字节数。
   参数：无。
   返回：未读字节数，0 表示当前无数据。
   原理：(head + SIZE - tail) % SIZE 得到头尾间距；不用临界区，
   原因是 head 只被中断改写、tail 只被主循环改写，两个 index 各自单向变化，
   最长临界区只有一条 16 位赋值，读到的值最多"偏旧一点"，不会破坏环形结构。
   典型用法：ESP 驱动先判断 Usart2_RxCount() > 0 再去取，避免读到陈旧数据。 */
uint16_t Usart2_RxCount(void);        /* number of unread bytes */
/* 作用：从接收缓冲取走 1 字节并让 tail 前移。
   参数：无。
   返回：取出的字节（由调用者保证此时缓冲非空）。
   原理：先读 g_EspRxBuf[tail] 再更新 tail。实现上不加"是否为空"的判断，
   也不阻塞等待，所以调用前应先用 Usart2_RxCount() 确认有数据。 */
uint8_t  Usart2_RxRead(void);         /* read one byte (blocking) */
/* 作用：清空接收缓冲，把 head/tail 归零，丢弃全部未读数据。
   参数：无。返回：无。
   原理：同时写 head 与 tail，等价于把缓冲"重新认作空"。
   用途：AT 交互超时后重同步、模块重启后丢弃残留半包，避免旧数据污染新应答。
   注意：它不关中断，若此刻中断正在写 head，极端情况下可能残留 1 字节，
   因此只在"确认 ESP 已静默"的时机调用。 */
void     Usart2_RxClear(void);        /* clear ring buffer */

/* USART2 interrupt service (called from stm32f10x_it.c) */
/* 作用：USART2 接收中断的实体，把收到的字节搬进环形缓冲。
   参数：无。
   返回：无。
   原理：中断只做"搬运"这一件事，不做任何解析/判断字符串，保证中断时间极短。
   缓冲满时丢弃新字节（宁丢不覆盖，避免把一帧数据撕成两段更难处理的错包）；
   另外必须读一次 DR 清除 ORE 溢出标志，否则溢出后 RXNE 会一直挂着，接收卡死。
   注意：stm32f10x_it.c 中的 USART2_IRQHandler() 必须转调本函数，
   中断向量表里才有真正的入口。 */
void Usart2_RxIsr(void);

/* printf redirected to USART1 (requires Keil MicroLIB) */
/* 作用：把标准库 printf 的字符输出重定向到 USART1。
   参数：ch —— 待输出字符；f —— 标准库传入的流指针，本实现不使用。
   返回：ch（按 C 库约定返回写出的字符）。
   原理：微库 MICROLIB / 标准库在"无 OS 的裸机"上都会调用弱符号 fputc 完成
   字符输出，重定义它即可让 printf 走串口；需要在 Keil 中勾选 Use MicroLIB，
   否则会链入完整 C 库（只提供 fputc 时仍可工作，但 Flash 占用明显变大）。 */
int fputc(int ch, FILE *f);

#endif /* __USART_H__ */
