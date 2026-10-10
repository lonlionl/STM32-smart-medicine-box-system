/**
  ******************************************************************************
  * @file    Project/STM32F10x_StdPeriph_Template/stm32f10x_conf.h 
  * @author  MCD Application Team
  * @version V3.5.0
  * @date    08-April-2011
  * @brief   Library configuration file.
  ******************************************************************************
  * @attention
  *
  * THE PRESENT FIRMWARE WHICH IS FOR GUIDANCE ONLY AIMS AT PROVIDING CUSTOMERS
  * WITH CODING INFORMATION REGARDING THEIR PRODUCTS IN ORDER FOR THEM TO SAVE
  * TIME. AS A RESULT, STMICROELECTRONICS SHALL NOT BE HELD LIABLE FOR ANY
  * DIRECT, INDIRECT OR CONSEQUENTIAL DAMAGES WITH RESPECT TO ANY CLAIMS ARISING
  * FROM THE CONTENT OF SUCH FIRMWARE AND/OR THE USE MADE BY CUSTOMERS OF THE
  * CODING INFORMATION CONTAINED HEREIN IN CONNECTION WITH THEIR PRODUCTS.
  *
  * <h2><center>&copy; COPYRIGHT 2011 STMicroelectronics</center></h2>
  ******************************************************************************
  */

/* ============================================================================
 * 模块说明(本项目视角, 补注)
 * ----------------------------------------------------------------------------
 * 项目    : 智能药箱系统
 * 芯片    : STM32F103ZET6 (Cortex-M3, 72MHz), ST 标准外设库 V3.5.0
 * 本文件  : stm32f10x_conf.h —— ST 标准外设库的"配置文件"(库的裁剪开关)。
 *           它属于 ST 官方模板, 本次只补中文注释, 代码一行未改。
 *
 * 【本文件到底决定什么】
 *   它是整个标准库的"总开关 + 外设裁剪表", 只做两件事:
 *     1) 决定哪些外设驱动头文件参与本次编译(下面的 #include 列表);
 *     2) 决定参数检查宏 assert_param 是否起作用(下面的 USE_FULL_ASSERT)。
 *
 * 【核心机制: 注释掉一个 #include, 就等于不编译该外设驱动, 从而节省 Flash】
 *   stm32f10x_conf.h 本身是一个头文件, 而"头文件里的 #include"只影响"声明",
 *   不产生任何机器码。真正的取舍逻辑是这样的:
 *     - 每个驱动源码文件(如 stm32f10x_adc.c)开头都包含 stm32f10x.h;
 *     - stm32f10x.h 的第 8296 行是:
 *           #ifdef USE_STDPERIPH_DRIVER
 *             #include "stm32f10x_conf.h"
 *           #endif
 *       也就是说, 只有定义了 USE_STDPERIPH_DRIVER, 这个 conf.h 才会被包含;
 *     - 因此: 把某个外设的 #include 注释掉, 该驱动的函数原型、寄存器位定义就
 *       不再参与编译 ——
 *         (a) 若该项目里根本没有把 stm32f10x_xxx.c 加入工程, 就等于该驱动彻底
 *             不编译, 直接省下它的全部代码空间(Flash);
 *         (b) 若那个 .c 仍在工程里被编译, 它自己会因找不到头文件而编译报错,
 *             于是你必须同步把它从工程里移除 —— 结果同样是省 Flash;
 *         (c) 反之, 保留多余的 include 只是多花一点点预处理/编译时间, 对最终
 *             Flash 通常没有影响: ARM 链接器只会把"被真正引用到的"函数和常量
 *             从 .c 里摘进来, 没人调用的驱动不会被链接进最终镜像。
 *   [本项目实测] Project\yehuoF103.uvprojx 里 23 个驱动 .c 全部加入了工程,
 *   而 stm32f10x_conf.h 中 23 个 include 也全部处于打开状态, 两边是一致的。
 *   若想真正给 Flash 减负, 正确做法是"工程里移除 .c" 与 "这里注释掉 include"
 *   两步一起做, 少任何一步都会编译报错或没效果。
 *
 * 【与 Keil 工程的关系】
 *   Project\yehuoF103.uvprojx 的 C/C++ 选项里已经预定义了这两个宏:
 *       USE_STDPERIPH_DRIVER, STM32F10X_HD
 *   所以"启用标准库驱动"和"选择大容量器件"是由工程设置决定的, 本文件里那行
 *   #define USE_STDPERIPH_DRIVER 只是库自带的"自带说明书式写法", 必须与工程设置
 *   保持一致的语义。若两边不一致(例如工程里有、这里注释掉)不会有问题, 但反过来
 *   把工程里的宏删掉而只靠本文件, 库的裁剪就失效了。
 * ========================================================================== */

/* Define to prevent recursive inclusion -------------------------------------*/
/* 防卫式宏(include guard): 防止本文件被重复包含。
   为什么需要: 本文件是被 stm32f10x.h 间接包含的, 而 stm32f10x.h 会被几乎每个
   源文件包含。加了这个宏, 第二次包含时内容直接跳过, 避免宏重复定义报警告。
   注意: 即使 #define USE_STDPERIPH_DRIVER 被注释掉, 这个防卫宏名字也仍然照
   头文件惯例保留(TEMPLATE 里没有条件包含它, 所以它不是开关)。 */
#ifndef __STM32F10x_CONF_H
#define __STM32F10x_CONF_H

/* Includes ------------------------------------------------------------------*/
/* Uncomment/Comment the line below to enable/disable peripheral header file inclusion */
/* 上面这行英文原意: "注释/取消注释下面的行, 以启用/禁用外设头文件的包含"。
   下面 23 行的取舍逐条注明如下(判定依据: 23 个驱动 .c 都在工程里编译, 而真实
   是否被调用由业务代码决定; "未使用"的驱动虽然不影响功能, 但注释掉可以省一点
   编译时间, 前提是同时把它从工程的 StdPeriph_Driver 组里移除)。 */
#include "stm32f10x_adc.h"
/* [本项目未使用] ADC 模数转换。本项目没有接任何模拟量传感器(电池电压检测也
   未做), 因此未使用。保留 include 不影响功能, 去掉可略省编译时间。 */
#include "stm32f10x_bkp.h"
/* [本项目使用] BKP 备份寄存器(掉电由 VBAT/纽扣电池保持)。
   用在哪: BSP\rtc.c 里 BKP_ReadBackupRegister/BKP_WriteBackupRegister 配合一个
   魔数(RTC_BKP_MAGIC)判断"RTC 是否已初始化过", 避免每次上电都重新配置 RTC
   (重新配置会清掉当前时间); 时钟源选择结果也借它记住。 */
#include "stm32f10x_can.h"
/* [本项目未使用] CAN 总线。本项目联网走 ESP-01S 的 WiFi(USART2), 无 CAN 节点。
   保留 include 不影响功能, 去掉可略省编译时间。 */
#include "stm32f10x_cec.h"
/* [本项目未使用] CEC(HDMI 消费电子控制)。本板没有 HDMI 接口, 完全无关。
   保留 include 不影响功能, 去掉可略省编译时间。 */
#include "stm32f10x_crc.h"
/* [本项目未使用] 硬件 CRC 计算单元。本项目没有用到硬件 CRC(Flash 配置区靠魔数
   和长度校验, 通信靠 AT/MQTT 自带校验), 因此未使用。
   保留 include 不影响功能, 去掉可略省编译时间。 */
#include "stm32f10x_dac.h"
/* [本项目未使用] DAC 数模转换。蜂鸣器是 GPIO 直接驱动(beep.c), 不需要模拟输出。
   保留 include 不影响功能, 去掉可略省编译时间。 */
#include "stm32f10x_dbgmcu.h"
/* [本项目未使用] DBGMCU 调试支持(冻结看门狗/低功耗时的调试行为)。只在特定调试
   场景用, 本项目未使用。保留 include 不影响功能, 去掉可略省编译时间。 */
#include "stm32f10x_dma.h"
/* [本项目未使用] DMA 直接存储器访问。本项目数据搬运量很小(串口每毫秒几十字节、
   LCD 刷屏由 FSMC 直接写), 用不着 DMA。保留 include 不影响功能, 去掉可略省编译时间。 */
#include "stm32f10x_exti.h"
/* [本项目未使用] EXTI 外部中断。5 个按键(KEY0~KEY4)全部采用"主循环定时扫描 +
   软件消抖"(key.c), 没有用外部中断触发; 也没有用到 EXTI 相关的唤醒功能。
   保留 include 不影响功能, 去掉可略省编译时间。 */
#include "stm32f10x_flash.h"
/* [本项目使用] FLASH 编程接口(注意: 这里的 FLASH 指芯片内部 Flash 的擦写,
   不是外接存储芯片)。用在哪: BSP\flash.c 用 FLASH_Unlock/FLASH_ErasePage/
   FLASH_ProgramHalfWord 把配置(定时提醒、开关门时间等)写进内部 Flash 指定页,
   实现"掉电保存"——这是本项目唯一的持久化手段(没有外接 EEPROM/SD 卡)。 */
#include "stm32f10x_fsmc.h"
/* [本项目使用] FSMC 灵活静态存储控制器。用在哪: BSP\lcd.c 用 FSMC_NORSRAMInit/
   FSMC_NORSRAMCmd 把 LCD(并口 8080 时序)挂在 Bank1 NORSRAM4 上, 由 FSMC 自动
   产生读写时序, CPU 只需像访问内存一样读写地址即可 —— 这是刷屏能跑快的关键。 */
#include "stm32f10x_gpio.h"
/* [本项目使用] GPIO 通用输入输出。用在哪: 几乎所有 BSP 模块 ——
   LCD 的背光/片选/复位脚(lcd.c)、5 个按键(lcd.c+key.c)、蜂鸣器(beep.c)、
   双串口引脚(usart.c)、舵机 I2C 的 SCL/SDA(pca9685.c)等。
   本项目引脚资源占用较多, 属于"最基础、必用"的驱动。 */
#include "stm32f10x_i2c.h"
/* [本项目未使用] 硬件 I2C 外设。注意: 舵机驱动板 PCA9685 走的是"软件模拟 I2C"
   (BSP\pca9685.c 用 GPIO 推拉/开漏时序自己模拟 SCL/SDA), 全程没有调用
   I2C_Init/I2C_GenerateSTART 等硬件 I2C 函数 —— 这是为了规避 STM32F1 硬件 I2C
   众所周知的"死锁"问题。保留 include 不影响功能, 去掉可略省编译时间。 */
#include "stm32f10x_iwdg.h"
/* [本项目未使用] 独立看门狗 IWDG。本项目没有使能任何看门狗(见 stm32f10x_it.c
   里对各个 Fault 处理函数的说明: 目前死循环且无看门狗兜底, 无法自恢复)。
   这是后续可靠性演进的重点方向之一, 目前保留 include 不影响功能。 */
#include "stm32f10x_pwr.h"
/* [本项目使用] PWR 电源控制。用在哪: BSP\rtc.c 里开启 PWR/BKP 时钟
   (RCC_APB1PeriphClockCmd(RCC_APB1Periph_PWR...))并通过 PWR_BackupAccessCmd()
   解锁备份域, 才能访问 RTC 与 BKP 寄存器 —— 它属于"RTC 相关"的配套驱动。 */
#include "stm32f10x_rcc.h"
/* [本项目使用] RCC 复位与时钟控制。用在哪: 每个 BSP 模块初始化时都要先开外设
   时钟(RCC_APB2PeriphClockCmd/RCC_APB1PeriphClockCmd/RCC_AHBPeriphClockCmd);
   BSP\rtc.c 还用 RCC_LSEConfig/RCC_RTCCLKConfig 选 RTC 时钟源(LSE 32768Hz 晶振,
   失败则退回 LSI), 是整个系统的"心跳来源"。缺了它所有外设都不工作。 */
#include "stm32f10x_rtc.h"
/* [本项目使用] RTC 实时时钟。用在哪: BSP\rtc.c 用 RTC_SetCounter/RTC_GetCounter
   做"秒计数"走时(RTC_GetEpoch/RTC_SetEpoch), BSP\menu.c 与 mqtt_huawei.c 用它
   取当前时间显示与上报。药箱的核心业务是"按时提醒吃药", 时间必须由 RTC 独立
   维持(掉电靠纽扣电池), 因此这是关键驱动。 */
#include "stm32f10x_sdio.h"
/* [本项目未使用] SDIO(接 SD/TF 卡)。本项目没有接存储卡, 配置保存用的是内部
   Flash。保留 include 不影响功能, 去掉可略省编译时间。 */
#include "stm32f10x_spi.h"
/* [本项目未使用] SPI 外设。LCD 用的是 FSMC 并口模式而不是 SPI 屏, 其它外设也
   没有走 SPI。保留 include 不影响功能, 去掉可略省编译时间。 */
#include "stm32f10x_tim.h"
/* [本项目使用] TIM 定时器。用在哪: BSP\timer.c 里 SysTick 由 CMSIS 的
   SysTick_Config 配置(严格说 SysTick 属于内核, 不归 TIM 驱动), 但本工程仍保留
   tim 驱动用于通用定时器/延时相关能力, 属于"计时/延时"这一类需求。 */
#include "stm32f10x_usart.h"
/* [本项目使用] USART 串口。用在哪: BSP\usart.c ——
   USART1(PA9/PA10)作为调试口, 重定向 printf 打印日志;
   USART2(PA2/PA3, 115200)接 ESP-01S WiFi 模块收发 AT 指令与 MQTT 数据。
   两个串口都用到, 是"联网 + 调试"两条命脉。 */
#include "stm32f10x_wwdg.h"
/* [本项目未使用] 窗口看门狗 WWDG。与 IWDG 同理, 本项目没有看门狗。
   保留 include 不影响功能, 去掉可略省编译时间。 */
#include "misc.h" /* High level functions for NVIC and SysTick (add-on to CMSIS functions) */
/* [本项目使用] misc 是标准库的"附加件"(严格说不属于 stdperiph 的 22 个外设),
   提供 NVIC 与 SysTick 的高级封装。用在哪: main.c 里
   NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2) 设定"2 位抢占 + 2 位子优先级";
   BSP\usart.c 里 NVIC_Init() 给 USART2 配中断优先级; BSP\timer.c 里
   NVIC_SetPriority(SysTick_IRQn, 0x0F) 把滴答压到最低优先级。
   没有它就无法配置中断优先级, 必用。 */

/* Exported types ------------------------------------------------------------*/
/* 本文件不导出任何类型。下面这几行是 ST 模板保留的分类注释:
   Exported types / Exported constants / Exported macro / Exported functions,
   用来标识头文件各段落的用途, 便于阅读和自动生成文档。 */
/* Exported constants --------------------------------------------------------*/
/* Uncomment the line below to expanse the "assert_param" macro in the 
   Standard Peripheral Library drivers code */
/* 上面英文原意: 取消下面这行的注释, 即可在标准外设库的驱动代码中"展开"
   assert_param 宏 —— 也就是打开库自带的参数检查。
   【为什么不打开(目前是注释状态)】
   标准库的绝大多数函数开头都有 assert_param(IS_XXX(...)) 之类的断言, 用来检查
   传入的参数(引脚号、通道号、模式)是否合法。打开后一旦参数非法, 就会调用
   assert_failed(), 报出"哪个文件第几行"——这在调试期极其有用(能立刻抓出写错的
   宏), 但它会:
     - 让每个库函数多出若干次判断, 代码变大(Flash 增加)、执行变慢;
     - 依赖 assert_failed 的实现(见文件末尾), 若没人实现就会链接失败。
   所以工程惯例是: 调试期打开、量产关闭。本工程当前为关闭状态, 宏展开成空操作,
   库函数不做任何参数检查。 */
/* #define USE_FULL_ASSERT    1 */

/* Exported macro ------------------------------------------------------------*/
/* 这一整段是条件编译: 只有在定义了 USE_FULL_ASSERT 时, assert_param 才是"真的
   检查"; 否则它展开成 (void)0, 也就是什么都不做。这解释了为什么关掉断言不会
   报错 —— 因为下面 #else 分支给了它一个空定义。 */
#ifdef  USE_FULL_ASSERT

/**
  * @brief  The assert_param macro is used for function's parameters check.
  * @param  expr: If expr is false, it calls assert_failed function which reports 
  *         the name of the source file and the source line number of the call 
  *         that failed. If expr is true, it returns no value.
  * @retval None
  */
/* 上面是 ST 官方英文说明, 中文含义:
   assert_param 宏用于函数参数检查。参数 expr 为真时什么都不做; 为假时调用
   assert_failed(), 把"出错的文件名(__FILE__)和行号(__LINE__)"报出来。
   写法解读:
     ((expr) ? (void)0 : assert_failed((uint8_t *)__FILE__, __LINE__))
     - 三元运算符: 条件成立走 (void)0(空语句, 不产生代码);
     - 条件不成立才调用 assert_failed, 并把两个编译期常量作为参数传入;
     - 结果强制转成 void, 避免"表达式结果未使用"的编译警告;
     - __FILE__/__LINE__ 是编译器内置宏, 展开成字符串字面量和当前行号,
       这也是它只在调试期有用的原因: 量产固件里既占空间又暴露源码路径。 */
  #define assert_param(expr) ((expr) ? (void)0 : assert_failed((uint8_t *)__FILE__, __LINE__))
/* Exported functions ------------------------------------------------------- */
/* 断言失败时被调用的回调(实现在 stm32f10x_it.c 或 main.c 里, 由用户自己写)。
   参数含义: file —— 断言失败所在的源文件名(__FILE__);
             line —— 断言失败所在的行号(__LINE__)。
   典型实现是"printf 打出文件名行号然后死循环", 方便直接定位问题;
   注意: 量产固件应关闭 USE_FULL_ASSERT, 因此这个函数也通常只在调试版里存在。
   本工程当前未定义 USE_FULL_ASSERT, 所以下面的声明不参与编译, 也不需要实现。 */
  void assert_failed(uint8_t* file, uint32_t line);
#else
/* 未定义 USE_FULL_ASSERT(本工程当前情况): 把 assert_param 定义成空操作。
   (void)0 的写法是为了让 "assert_param(...);" 这样的语句在语法上仍然成立
   (分号前必须有个表达式), 同时编译器不会为它生成任何机器码 —— 这正是"关掉
   参数检查不花任何代价"的原因。 */
  #define assert_param(expr) ((void)0)
#endif /* USE_FULL_ASSERT */

#endif /* __STM32F10x_CONF_H */

/******************* (C) COPYRIGHT 2011 STMicroelectronics *****END OF FILE****/
