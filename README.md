# 智能药箱系统 | STM32 Smart Medicine Box

> 基于 **STM32F103ZET6** 的七路智能药箱：本地菜单 + 云端远程双通道定时管理，到点舵机自动开盒并蜂鸣提醒，服药状态实时上云，配套 Web/PWA 前端远程操控。
>
> 纯裸机开发（**无 RTOS**）；MCU 侧 11 个外设驱动、AT 驱动层与 MQTT 业务协议层均自行实现，未引入第三方中间件
> （网络侧 TCP/IP 与 MQTT 协议栈由 ESP-01S 模组的 AT 固件承担，本工程实现的是模组之上的驱动与业务层）
>
> 固件 9,643 行（30 个 `.c/.h`，含自动生成字库 `font.c` 749 行，手写部分 8,894 行）/ Flash 31.0 KB / RAM 20.6 KB · Web 端约 1,150 行（server.js 306 + 两个页面 435/367 + 配置与静态资源）
>
> （统计口径：含注释与空行的物理行数；本轮补充中文注释后行数较早期文档有大幅增加）

- **MCU**：STM32F103ZET6（ALIENTEK 精英板 V2.6）· **工具链**：Keil MDK 5 / ARMCC V5.06（-O2 + MicroLIB）
- **云端**：华为云 IoTDA（MQTT 1883）· **前端**：uni-app + Vue 3 + Vite（H5/PWA）+ Node.js/Express
- **许可证**：[MIT](LICENSE)

---

## 目录

- [一、项目简介](#一项目简介)
- [二、技术亮点](#二技术亮点)
- [三、系统架构](#三系统架构)
- [四、硬件接线](#四硬件接线)
- [五、编译与烧录](#五编译与烧录)
- [六、华为云 IoTDA 配置](#六华为云-iotda-配置)
- [七、ESP-01S 固件](#七esp-01s-固件)
- [八、Web 端（前端 + 后端）](#八web-端前端--后端)
- [九、定时规则线协议](#九定时规则线协议)
- [十、开发过程中解决的 14 个核心技术问题](#十开发过程中解决的-14-个核心技术问题)
- [十一、目录结构](#十一目录结构)
- [十二、已知限制与改进方向](#十二已知限制与改进方向)

---

## 一、项目简介

面向 **7 天 × 7 药盒 × 每天 4 个时间点** 的服药提醒场景。针对**定时规则（`schedule_N`）**，系统提供两条完全对等的配置通道——开发板按键菜单和手机/浏览器远程界面——任意一端修改都会自动同步到另一端，配置持久化在片内 Flash，断电不丢失。

| 功能 | 说明 |
|---|---|
| **本地定时** | 5 个按键四级菜单：药盒选择 → 星期选择 → 时间设置（4 槽位），支持逐盒/逐天/逐点清除 |
| **远程开盒** | 云端下发 `remote_open_box` 命令，同步响应带 `open_result`，App 端即时反馈 |
| **远程定时** | 云端 `properties/set` 下发 `schedule_1`..`schedule_7`，设备解析后写 Flash 并回上报 |
| **自动开盒** | RTC 到点触发，PCA9685 驱动 SG90 舵机开盒 + 蜂鸣器提醒 60 秒，按任意键停止并复位全部药盒 |
| **状态上报** | 触发方式（定时/远程）、触发时间、药盒号实时上报，App 5 秒轮询同步 |
| **NTP 校时** | 联网后自动校时，RTC 掉电靠纽扣电池走时 |
| **断电保存** | 配置存 Flash 末页，**CRC32 校验 + 写后回读比对**，损坏自动回落默认值 |

---

## 二、技术亮点

- **全程非阻塞**：11 个 BSP 驱动、12 状态云连接状态机、AT 命令与原始发布双状态机，全部基于 1 ms 系统滴答轮询，**主循环内零 `delay`**。
- **中断只搬字节**：`USART2_IRQHandler` 仅做单字节入环形缓冲（512 B SPSC），解析、Flash、LCD、`printf` 一律回主循环执行。
- **两条独立并发链路**：业务逻辑（RTC/按键/舵机/蜂鸣）与云端链路（WiFi/MQTT）互不阻塞；云端断连 5 秒退避重连，业务侧照常工作。
- **背压而非阻塞**：AT 通道忙时 `ESP_PubRaw` 返回 0，上层用 `g_PendingReply` / `g_PendingSchedule` + 逐盒下标 `g_SchedIdx` 暂存，下个 tick 重试——**命令响应与定时下发不丢任务**（高频状态上报的取舍见[十二、已知限制](#十二已知限制与改进方向)）。
- **绕过硬性报文上限**：ESP-01S 固件把 MQTT 单条消息钉死在 512 字节，整表约 1600 字符必然被截断 → 拆成 7 个云属性、每盒 56 个数字独立传输，并据此设计增量下发协议。
- **通过内存布局分析定位栈溢出**：读 Keil `.map` 发现全局 `request_id` 缓冲区紧邻 1 KB 栈区，深调用链约 1.4 KB 向下踩踏全局变量 → 栈扩到 12 KB 根治。
- **端到端自主实现**：从 ILI9488 显示驱动与字库生成脚本、软件 I2C 驱动 PCA9685，到 MQTT 报文组包、JSON 取字段、云端 SDK 派生签名，MCU 侧与云端调用侧均未引入第三方中间件。

---

## 三、系统架构

```
                    ┌──────────────────────────────────────────┐
                    │        华为云 IoTDA 设备接入              │
                    │  属性上报 / 命令下发 / 属性设置 / 命令响应  │
                    └───────┬──────────────────────────┬───────┘
                MQTT/TCP 1883                    HTTPS 应用侧 API
                            │                          │
              ┌─────────────▼───────────┐   ┌──────────▼───────────────┐
              │  ESP-01S (ESP8285 1MB)  │   │ Node.js 代理后端         │
              │  esp-at v2.2.2.0        │   │ Express + IoTDA SDK      │
              │  AT+MQTTPUBRAW 两步发布 │   │ 派生签名 / Instance-Id   │
              └─────────────▲───────────┘   │ 强制 IPv4 / 3 次重试     │
                            │ USART2        └──────────▲───────────────┘
                            │ PA2/PA3, 115200           │ REST
              ┌─────────────┴───────────────────────────┴───────────────┐
              │                  STM32F103ZET6 @72MHz                   │
              │  ┌────────────────────────────────────────────────────┐  │
              │  │ main.c 超级循环（5 步）                            │  │
              │  │  ① Key_Scan  ② Menu_Tick  ③ App_Process           │  │
              │  │  ④ Beep_Process  ⑤ MQTT_Process                   │  │
              │  └────────────────────────────────────────────────────┘  │
              │  APP: app.c 药盒状态机/定时调度   menu.c 四级菜单 UI     │
              │  BSP: esp01s_at · mqtt_huawei · lcd · font · key ·      │
              │       beep · rtc · flash · pca9685 · usart · timer       │
              └───┬──────────┬───────────┬──────────┬───────────────────┘
                  │ FSMC     │ 软件I2C   │ GPIO     │ RTC
              ┌───▼───┐  ┌───▼────┐  ┌───▼───┐  ┌───▼─────┐
              │ILI9488│  │PCA9685 │  │蜂鸣器 │  │32.768kHz│
              │320x480│  │16 路PWM│  │ PB8   │  │ 晶振    │
              └───────┘  └───┬────┘  └───────┘  └─────────┘
                             │ ch0..ch6
                        ┌────▼─────────────────┐
                        │ SG90 舵机 ×7（药盒盖）│
                        └──────────────────────┘
```

### 关键状态机

**① 云端连接状态机（12 态，`mqtt_huawei.c`）**

```
IDLE →(200ms)→ AT_INIT → ATE0 → WIFI_MODE → WIFI_JOIN → NTP_CFG → NTP_WAIT
     → MQTT_CLEAN → MQTT_CONN → MQTT_SUB(3 子步) → READY
                                     ↑
                              RETRY_WAIT(5s) ◄── 任意步骤失败 / +MQTTDISCONNECTED
```

超时按步骤分级：`AT`/`ATE0`/`CWMODE` 1 s，`CWJAP` 15 s，`MQTTCONN` 10 s，`MQTTSUB` 5 s，`PUBRAW` 5 s（AT/ATE0/CWMODE 1000ms，CWJAP 15000ms，CIPSNTPCFG 3000ms，CIPSNTPTIME 5000ms，MQTTCLEAN 3000ms，MQTTUSERCFG 3000ms，MQTTCONN 10000ms，MQTTSUB 5000ms，PUBRAW 5000ms）。

**② AT 驱动双状态机（`esp01s_at.c`）** —— 命令态 `g_Cmd` 与原始发布态 `g_Pub` 互斥，`ESP_CmdBusy()` 作为背压信号。`AT+MQTTPUBRAW` 分两步：先发 `AT+MQTTPUBRAW=0,"topic",len,qos,0`，**以 `'>'` 单字节为触发点**（固件实际回 `"OK\r\n> "`，不一定有换行）再流式吐原始 JSON，不转义、结尾不加 CRLF。

**③ 菜单状态机（4 界面，`menu.c`）**

```
SCR_MAIN ──SET──► SCR_BOX_SEL ──OK──► SCR_DAY_SEL ──OK──► SCR_TIME_SET
    ▲                  │                   │                    │
    └────BACK──────────┘◄──────BACK────────┘◄───────BACK────────┘
                 （返回时用 g_KeepItemSel 记住光标位置）
TIME_SET 内确认键推进 g_EditPhase：0 列表 → 1 调时 → 2 调分 → 保存
```

---

## 四、硬件接线

**主控**：正点原子精英板 V2.6（STM32F103ZET6，512 KB Flash / 64 KB RAM）

| 外设 | 接口 | 引脚 |
|---|---|---|
| 调试串口 | USART1 | PA9 (TX) / PA10 (RX)，115200 |
| ESP-01S | USART2 | PA2 (TX) / PA3 (RX)，115200，CH_PD 接 3.3V |
| PCA9685 | 软件 I2C（PB6/PB7 开漏 + 外部上拉） | PB6 (SCL) / PB7 (SDA) |
| 蜂鸣器 | GPIO | PB8，高电平有效 |
| 按键 ×5 | GPIO 上拉 | PF0 设置 / PF1 确认 / PF2 加 / PF3 减 / PF4 返回，低电平有效 |
| TFTLCD | FSMC Bank1 NE4 | PG12 (CS) / PG0 (RS=A10) / PD5 (WR) / PD4 (RD) / PD3 (RST) / PB0 (BL) |
| RTC | LSE | PC14 / PC15（32.768 kHz + 纽扣电池） |

**LCD 地址解码**：FSMC 基址 `0x6C000000`，地址线 A10 作 RS → 数据地址 `0x6C000800`，普通指针写入即可区命令/数据。

**PCA9685 通道映射**：ch0–ch6 ↔ 1–7 号药盒，开盒角度 `SERVO_OPEN_ANGLE = 90°`（脉宽 307/4096 ≈ 1.50 ms）。

> **【注意】** **舵机供电**：PCA9685 的 `VCC` 接 3.3V（芯片供电），`V+` 必须接 **5V 且独立稳压**，不要直接吃板载 3.3V，否则舵机无力或抖动。
>
> **【注意】** **调试注意**：USB 转 TTL 与 ESP-01S 共用 USART2 引脚时，插着串口线会干扰模组通信（表现为 WiFi 状态反复横跳）。调试 ESP 时请拔掉串口线。

### 屏幕菜单操作

主界面显示实时时间、WiFi/云连接状态与 7 行药盒状态。**蜂鸣器响期间按任意键**即停止蜂鸣并关闭全部药盒——这是"确认服药"的动作。

按键固定为：`设置`(PF0) / `确认`(PF1) / `加`(PF2) / `减`(PF3) / `返回`(PF4)，四级菜单逐级进入：

| 界面 | 加/减 | 确认 | 设置 | 返回 |
|---|---|---|---|---|
| **药盒选择** | 移动光标选 1–7 号盒，右侧显示该盒最近一次定时 | 进入星期选择 | 清除该盒全部定时 | 回主界面 |
| **星期选择** | 移动光标选周一–周日，右侧显示该天最早定时 | 进入时间设置 | 清除该星期全部定时 | 回药盒选择（光标停在原盒） |
| **时间设置** | 在"已设置的槽"与"第一个未设置槽"之间移动（未设置槽之间不能互移） | 见下 | 清除当前时间点 | 逐级退回 |

时间设置界面列出 **4 个时间槽**，按"距当前最近的触发时刻"排序、未设置的排在最后；进入时自动锁定第一个未设置槽，直接按确认即可开始设置。确认键的行为是**三段式**：

```
列表阶段 --确认--> 调小时 --确认--> 调分钟 --确认--> 保存并上报云端
```

保存成功后配置写入片内 Flash，并自动上报 `schedule_N` 到华为云，手机端同步可见。

---

## 五、编译与烧录

### 固件

1. 用 **Keil MDK5（ARM Compiler V5）** 打开 `Project/yehuoF103.uvprojx`
2. 芯片选 `STM32F103ZE`；宏定义 `USE_STDPERIPH_DRIVER`、`STM32F10X_HD`
3. Target 选项勾选 **Use MicroLIB**（`printf` 重定向需要）
4. **优化级别必须 ≥ -O2**：Keil 免费评估版限制代码 ≤ 32 KB，不开优化会链接失败（`L6047U`）
5. 编译生成 hex，用 ST-Link / J-Link 下载

当前构建结果：**0 error / 0 warning**，`Code 23,212 + RO-data 8,428 + RW-data 376 = 32,016 B`；链接器实测 `Total ROM 31,764 B`，距 32 KB 上限约 0.7~1.0 KB 余量。

> **【注意】** **余量已经很小**：本工程卡在 Keil 评估版 32 KB 上限附近（占用约 97%）。
> 字库是按需生成的，多收一个汉字约 36 字节 —— 以后加功能或改界面文案前，
> 先看这个数，并记住改文案后要重跑 `tools/gen_font.py`（否则字显示成空白）。

### 烧录前必须知道的三个坑

- 首次上电 RTC 无有效时间时，系统会自动写入默认配置（1 号盒 周一 08:00），联网后 NTP 自动校时。
- 配置存于 Flash 末页 `0x0807F800`（2 KB）。**改了配置结构体必须同步更新 `flash.h` 里的 `FLASH_CFG_MAGIC`**，否则旧格式脏数据仍能通过 magic 校验被误读。
- 未设置的时间点一律初始化为 `0xFF`，**绝不能用 0**——0 是合法小时，会被显示成 `00:00` 并破坏"最近定时"排序。

### 设备端必填配置（`User/BSP/mqtt_huawei.h`）

```c
#define WIFI_SSID            "<YOUR_WIFI_SSID>"
#define WIFI_PASSWORD        "<YOUR_WIFI_PASSWORD>"
#define HUAWEI_DEVICE_ID     "<YOUR_DEVICE_ID>"
#define HUAWEI_HOST          "<YOUR_IOTDA_DEVICE_ENDPOINT>"
#define HUAWEI_CLIENT_ID     "<YOUR_CLIENT_ID>"
#define HUAWEI_STATIC_PWD    "<YOUR_MQTT_PASSWORD>"
```

> **重要**：**本仓库所有凭证均为占位符。** `clientId` 里的时间戳与 `HUAWEI_STATIC_PWD` 必须**配套**（在控制台设备详情页一次性生成），只改其一会认证失败（`auth cfg fail`）。

---

## 六、华为云 IoTDA 配置

### 产品模型（服务 ID：`medicine_box_service`）

**属性**（访问权限均为**可读可写**）

| 属性名 | 类型 | 说明 |
|---|---|---|
| `box_num` | int | 最近触发的药盒号，0 表示全部关闭 |
| `chufa_text` | string | 触发方式（定时触发 / 远程手动触发） |
| `chufa_time` | string | 触发时间 `YYYY-MM-DD HH:MM:SS` |
| `schedule_1` … `schedule_7` | string(500) | 各药盒定时规则，线格式见第九节 |

> **说明**：`box_num` / `chufa_text` / `chufa_time` 这三个属性：设备端只上报这三个属性，不会读取云端对它们的修改；真正双向同步的只有 `schedule_1`~`schedule_7`。

**命令**

| 命令 | 下发参数 | 响应参数 |
|---|---|---|
| `remote_open_box` | `target_box` (int 1–7) | `open_result` (string) |
| `set_schedule`（旧版兼容，可选） | `box`(1–7)、`day`(1–7，周一=1)、`hour`(0–24，0=清除) | `status` (string) |

### 平台主题

| 用途 | 主题 |
|---|---|
| 上报属性 | `$oc/devices/{设备ID}/sys/properties/report` |
| 属性设置（订阅） | `$oc/devices/{设备ID}/sys/properties/set/#` |
| 命令下发（订阅） | `$oc/devices/{设备ID}/sys/commands/#` |
| 命令响应 | `$oc/devices/{设备ID}/sys/commands/response/request_id={rid}` |
| 属性设置响应 | `$oc/devices/{设备ID}/sys/properties/set/response/request_id={rid}` |

订阅用 `#` 通配而非固定 `request_id`，因为订阅时还不知道具体请求 ID。

> **【注意】** **同步命令必须定义响应参数**，且设备回复的 `paras` 必须与产品模型完全一致，否则平台判超时（`IOTDA.014111`）。这是本项目耗时最久的排查点之一。

---

## 七、ESP-01S 固件

使用乐鑫官方 **esp-at v2.2.2.0 的 1 MB 版本（esp8285-1MB-at）**，必须支持 MQTT AT 指令。

1. **本仓库只收录了改过引脚的那一个文件**：`ESP8266固件烧录资料/esp8285-1MB-at/customized_partitions/factory_param_ESP01S.bin`。其余固件（`esp-at.bin`、bootloader、分区表、证书分区、地址表等）是乐鑫官方原版二进制、总量约 14 MB，不属于本项目成果，**未纳入仓库**。
2. **请从乐鑫 esp-at 的 `release/v2.2.0.0_esp8266` 分支下载 `esp8285-1MB-at` 完整包**（CI Artifacts），解压后把本仓库那个 `factory_param_ESP01S.bin` 覆盖进它的 `customized_partitions/`。
3. **AT 引脚必须改**：官方固件默认 AT 引脚是 GPIO15/GPIO13，而 ESP-01S 的 UART 是 GPIO1(TX)/GPIO3(RX)，直接用会「烧录显示成功但发 AT 无响应、芯片发烫」。本仓库的 `factory_param_ESP01S.bin` 已把引脚改成 GPIO1/GPIO3（与官方原始 `factory_param.bin` 仅差 5 字节，偏移 0x10~0x14：`0F 0D 03 01 05` → `01 03 FF FF FF`）。
4. **【注意】** **必须把地址表里 0x19000 那一行换成改过引脚的版本**。官方原包的 `download.config` / `flasher_args.json` 里 0x19000 指向的是**未改引脚**的 `factory_param.bin`，照原表烧会白烧一次。请把该行改为：
   ```
   0x19000 customized_partitions/factory_param_ESP01S.bin
   ```
5. 刷写：GPIO0 接地进下载模式 → 用 Flash Download Tools 按（已改好的）地址表烧录 → 恢复 GPIO0 重启 → 发 `AT` 应返回 `OK`，`AT+GMR` 应显示 `AT version:2.2.2.0 / Bin version:2.2.2(ESP8266_1MB)`

**为什么上报必须用 `AT+MQTTPUBRAW`**：`AT+MQTTPUB` 不支持 JSON 里的引号转义，带 `\"` 的报文会被固件直接拒绝（返回 `ERROR`），必须走两步法发原始字节。

---

## 八、Web 端（前端 + 后端）

目录 `智能药箱Web应用/`，分前后端两部分。

### 后端（`backend/`，Node.js + Express，5 个接口）

| 方法 | 路径 | 说明 | 调用的华为云 API |
|---|---|---|---|
| POST | `/api/open-box` | 远程开盒 | `createCommand` |
| GET | `/api/schedule` | 读全部定时（读影子后解析 7 个属性） | `showDeviceShadow` |
| POST | `/api/schedule` | 写单个药盒定时（**只下发改动的那一盒**） | `updateProperties` |
| GET | `/api/shadow` | 查询设备影子 | `showDeviceShadow` |
| GET | `/api/device` | 查询设备在线状态 | `showDevice` |

```bash
cd 智能药箱Web应用/backend
cp server.example.js server.local.js   # 填入自己的华为云凭证
npm install
npm start                                # → http://localhost:3000
```

`server.local.js` 已在 `.gitignore` 中；未配置时进程**直接退出并提示**，不会静默使用示例值。

### 前端（`app/`，uni-app + Vue 3 + Vite，H5/PWA）

```bash
cd 智能药箱Web应用/app
npm install
npm run dev:h5        # → http://localhost:5173（Vite 默认端口）
npm run build:h5      # 构建产物 dist/build/h5
```

- **页面**：主界面（在线状态 / 下次服药 / 7 个药盒状态与远程开箱）、定时设置（药盒页签 → 星期列表 → 单日内联编辑面板）
- **轮询**：主界面 5 秒一轮，请求超时 10 秒
- **PWA**：`manifest.webmanifest` + 192/512 图标，`display: standalone`，手机浏览器「添加到主屏幕」后可全屏运行（**无 Service Worker，不具备离线能力**）

> 后端需与前端同网段可访问；前端 `API_BASE` 硬编码为 `http://localhost:3000`，跨机访问请自行改为局域网 IP。

---

## 九、定时规则线协议

每盒定时被序列化为 **恰好 56 个逗号分隔的十进制数字**：

```
7 天 × 4 个时间点 × (时, 分) = 56
顺序：周一到周日（索引 0..6），天内按时间点 0..3，每点先「时」后「分」
下标公式：off = 天 * 8 + 时间点 * 2   （off = 时，off+1 = 分）
```

**哨兵值**：小时 = `255` 表示该时间点**未设置**（分钟固定写 0）。

示例：仅设置「周一 08:30」

```
8,30,255,0,255,0,255,0,255,0,255,0,255,0,255,0,255,0,255,0,255,0,255,0,
255,0,255,0,255,0,255,0,255,0,255,0,255,0,255,0,255,0,255,0,255,0,255,0,
255,0,255,0,255,0,255,0
```

**为什么拆成 7 个属性**：ESP-01S 固件把单条 MQTT 消息长度钉死在 512 字节（含主题）。整表 392 个数字约 1600 字符，必然被截断（日志特征 `buf len=512`）。拆成 `schedule_1`..`schedule_7` 后每盒约 150–200 字符，安全传输。

**增量写**：后端只下发被改动的那一盒；设备端解析前先 `tmp = g_Cfg` 拷贝当前配置再覆盖目标盒，避免未下发的盒被垃圾值冲掉。

---

## 十、开发过程中解决的 14 个核心技术问题

这一节是项目最有价值的部分——每个问题都记录了**现象、根因、定位手段、修复方案**。

<details open>
<summary><b>1. ESP-01S 烧录后发烫、AT 无响应</b></summary>

- **现象**：按官方地址表烧录显示 finish，但芯片严重发烫，发 `AT` 完全无反应；刷回原固件又正常。
- **根因**：官方 1 MB 固件（esp8285 系列）默认 AT 引脚是 **GPIO15/GPIO13**，而 ESP-01S 的 UART 是 **GPIO1(TX)/GPIO3(RX)**。引脚错配，固件起来了但串口不通，时钟/引脚配置异常导致发烫。
- **解决**：改固件包的 `customized_partitions/factory_param_ESP01S.bin`，把 AT 引脚配成 GPIO1/GPIO3，随固件一起烧录。验证：`AT+GMR` 返回 `AT version:2.2.2.0 / Bin version:2.2.2(ESP8266_1MB)`。
</details>

<details>
<summary><b>2. <code>AT+MQTTPUB</code> 上报属性恒返回 ERROR</b></summary>

- **现象**：WiFi 连接、订阅下发主题都成功，但 `AT+MQTTPUB` 上报带 `\"` 转义的 JSON 恒返回 `ERROR`；同参数的 MQTTx 图形工具却能成功。
- **根因**：该版本固件的 `AT+MQTTPUB` **不支持 JSON 引号转义**；MQTTx 内部自行处理了转义。
- **解决**：改用 `AT+MQTTPUBRAW` 两步法——先发 `AT+MQTTPUBRAW=0,"topic",len,0,0`，等 `>` 提示符后发原始 JSON 字节（不转义、不加换行），收到 `+MQTTPUB:OK` 即成功。封装为 `ESP_PubRaw()`，成为全部上报的唯一通道。
</details>

<details>
<summary><b>3. <code>+MQTTSUBRECV</code> 数据被截断 / 解析不到</b></summary>

- **现象**：下行数据 `len=0`；且数据取不到。
- **根因**：① 完整命令行有 130 字节，而 `ESP_LINE_MAX` 只有 128，被截断；② 固件把数据**内联在长度后面同一行**返回（`+MQTTSUBRECV:0,"topic",len,{data}`），而代码在等下一行。
- **解决**：`ESP_LINE_MAX` 128 → 256 → 2048；重写 `ParseMqttSubRecv` 支持同行内联数据，并保留逐字节回退路径。
</details>

<details>
<summary><b>4. 订阅应答永远收不到</b></summary>

- **现象**：`AT+MQTTSUB` 一直等不到应答，超时。
- **根因**：该固件文档里订阅应答返回的是 **`OK`**，不是 `+MQTTSUBACK`。
- **解决**：预期字符串从 `+MQTTSUBACK` 改为 `OK`。
</details>

<details>
<summary><b>5. 时钟错到 1970 年、NTP 后又多 8 小时</b></summary>

- **根因**：① RTC 首次写入时未等同步完成就调用，写入不生效，时间停在初值；② `+CIPSNTPTIME` 返回的**已经是北京时间**，代码又加了 8 小时。
- **解决**：`RTC_SetEpoch()` 内先 `RTC_WaitForSynchro()` 再 `RTC_SetCounter()` 再 `RTC_WaitForLastTask()`；NTP 回调取消 +8 小时偏移。
</details>

<details>
<summary><b>6. 云平台报命令超时 IOTDA.014111（响应参数缺失）</b></summary>

- **现象**：下发 `remote_open_box`，蜂鸣器响、界面显示已开盒，但平台报命令超时。
- **根因**：产品模型里该命令**没有定义响应参数**。华为云同步命令必须收到设备响应，且响应 `paras` 必须与模型定义一致。
- **解决**：模型侧新增响应参数（`result` → 后调整为 `open_result`），设备端回复 `{"result_code":0,"response_name":"remote_open_box","paras":{"open_result":"success"}}`。
</details>

<details>
<summary><b>7. 【最难的 bug】响应发布了但平台仍超时——1KB 栈溢出踩踏全局变量</b></summary>

- **现象**：日志显示命令响应已发布成功，平台仍报超时（`costTime 23779ms`）。加诊断打印发现响应 topic 里的 `request_id` 只剩 1 个字符：`request_id=?`。
- **定位过程**：
  1. `ParseRequestId` 刚提取完时 `rid` 是完整的 36 字符；
  2. 经过 `App_OpenBox`（内部会做属性上报）后，`rid` 被破坏成 1–2 个字符；
  3. 读 Keil 编译生成的 **`.map`** 看内存布局：

     > **【注意】** 下面是修复前的**示意片段**（手工简化，非某次真实构建的完整输出），仅用于说明"栈紧邻全局变量"这一结论。当前真实内存布局请看 Project/Listings/yehuoF103.map。

     ```
     g_TopicProp      0x20000758  120  mqtt_huawei.o(.bss)
     g_TopicCmdSub    0x200007d0  120  mqtt_huawei.o(.bss)
     g_CmdRequestId   0x20000848   40  mqtt_huawei.o(.bss)
     g_ReplyRequestId 0x20000870   40  mqtt_huawei.o(.bss)
     STACK            0x20000960 1024  startup_stm32f10x_hd.o(STACK)
     ```

  4. 栈区只有 1 KB 且**紧跟在 rid 缓冲区后面、向下增长**；而最深调用链 `MqttOnData(buf[2050])` → `App_OpenBox` → `MQTT_PublishRecord(payload[200]+tail[80])` → `ESP_PubRaw(cmd[180])`，加上响应路径 `ReplyCommand(topic[200])` → `ESP_PubRaw(cmd[180])`，累计栈用量约 **1.4–1.5 KB**，向下溢出正好踩掉紧邻的两个 `request_id` 缓冲区。
  5. 这同时解释了两个诡异现象：为什么"昨天能跑今天不行"（前一天把 payload 从 160 调到 200，栈用量恰好越过 1 KB 临界点）；为什么 `rid` 只剩 1 个字符。
- **解决**：栈 `Stack_Size` 由 `0x400`(1KB) 提升到 **`0x3000`(12KB)**；同步加大 `ESP_LINE_MAX` 防长命令行截断；并让 PUBRAW 等待提示符期间收到的 URC 也走 `HandleLine`，避免丢命令。
- **方法论沉淀**：全局变量被莫名破坏时，读 `.map` 确认内存布局，检查栈区是否紧邻该变量、深调用链用量是否超限。
</details>

<details>
<summary><b>8. 定时下发后整表被截断（ESP-01S 512 字节上限）</b></summary>

- **现象**：前端保存定时后，串口日志出现 `sched str len=316`、`schedule parse fail`、`buf len=512`。
- **根因**：ESP-01S 固件编译时定死 `MQTT_BUFFER_SIZE_BYTE=512`，单条消息（含主题）最长 512 字节。整表 392 个数字约 1600 字符，远超上限被固件截断。
- **解决**：把 `schedule` 拆成 **`schedule_1` … `schedule_7`** 七个属性，每盒 56 个数字（约 150–200 字符）；设备端序列化/解析改成单盒版本；后端 GET 读七个属性拼装、POST 只下发改动的那一盒（增量写）。
</details>

<details>
<summary><b>9. 设备端改定时后只上报了 1 个药盒</b></summary>

- **现象**：设备端改完定时，影子里只有 `schedule_1`，其余 6 个属性缺失，日志却打印 all ok。
- **根因**：`MQTT_PublishSchedule` 用 `for` 循环，`ESP_PubRaw` 在上一条未发完时返回 0（busy），原代码 busy 时**跳过该盒继续下一盒**，结果 7 盒全被跳过、下标直接走到头。
- **解决**：改为 `while` + 状态机，**成功才递增下标，busy 就 `break` 等下一个 tick 重试同一盒**，保证不丢盒。
</details>

<details>
<summary><b>10. 设置一个药盒定时，其他药盒全变成 00:00</b></summary>

- **现象**：前端设置好某盒定时，其余所有药盒定时被刷成 `00:00`，设备端也被覆盖。
- **根因**：设备端处理 `properties/set` 时，局部变量 `TimerCfg_t tmp` **未初始化**就直接写入下发的那一盒，其他盒是栈上垃圾值；最后 `g_Cfg = tmp` 把垃圾值覆盖到整份配置。
- **解决**：改成 `tmp = g_Cfg;` **先拷贝当前配置再覆盖目标盒**。
</details>

<details>
<summary><b>11. 未设置的定时显示 00:00，排序全乱</b></summary>

- **根因**：`App_Init` 用 `memset 0` 初始化配置，`hour == 0` 被当成凌晨 0 点；且旧 Flash 里的全 0 数据 magic 匹配，被当作有效配置读入。由于 00:00 最小，"最近定时"永远优先显示它。
- **解决**：所有时间点初始化为 **`0xFF`（未设置哨兵）**；更换 `FLASH_CFG_MAGIC` 让旧全 0 数据失效；后端 `parseOneBox` 加兜底——整盒全 0 视为未设置，兼容旧脏数据。
</details>

<details>
<summary><b>12. 前端保存成功但设备端不同步（用错了云 API）</b></summary>

- **根因**：后端最初调用 `UpdateDeviceShadowDesiredData`，该接口**只修改影子里的 desired，不会下发给设备**。
- **解决**：改用 **修改设备属性** `UpdatePropertiesRequest` + `client.updateProperties()`，华为云会通过 `properties/set` 主题主动推给设备。附带修正一处 SDK 类名笔误（`UpdateDeviceShadowRequest` 并不存在）。
</details>

<details>
<summary><b>13. IAM 子用户 AK/SK 调用恒 401</b></summary>

- **根因**：IAM 子用户（编程访问）+ 自定义 endpoint 时，SDK 默认的普通签名验签不通过，**必须使用派生签名**（V11-HMAC-SHA256 derived）；且标准版所有请求必须带 `Instance-Id` 请求头。
- **解决**：`new BasicCredentials({...}).withDerivedPredicate(() => true).withRegionId(region)`；封装 `withInstanceId(request)` 统一为 5 个请求类注入实例 ID。
</details>

<details>
<summary><b>14. 后端连华为云 ETIMEDOUT</b></summary>

- **根因**：华为云域名优先解析到 IPv6（`2407:...`），本机 IPv6 不通；`dns.setDefaultResultOrder('ipv4first')` 对 SDK 内部 axios 实例不生效。
- **解决**：给 SDK 传 `https.Agent({ family: 4, keepAlive: true })`，通过 `withOptions({ axiosRequestConfig: { httpsAgent } })` 强制走 IPv4；所有云调用再包一层 `withRetry`（最多 3 次、间隔 2 秒）抗网络抖动。
</details>

---

## 十一、目录结构

```
智能药箱系统/
├── User/                          # 设备端源码（本项目核心，共 30 文件 9,643 行，含自动生成字库 font.c 749 行）
│   ├── main.c                     #   主循环：初始化 + 5 步超级循环
│   ├── stm32f10x_it.c             #   中断服务（SysTick / USART2）
│   ├── APP/
│   │   ├── app.c/.h               #   药盒状态机、定时触发调度、开盒关盒
│   │   └── menu.c/.h              #   四级菜单与 LCD 界面（1,517 行）
│   └── BSP/                       #   11 个驱动模块
│       ├── mqtt_huawei.c/.h       #   华为云 MQTT 层（1,432 行，12 状态机）
│       ├── esp01s_at.c/.h         #   ESP-01S AT 驱动（非阻塞双状态机）
│       ├── pca9685.c/.h           #   PCA9685 舵机驱动（软件 I2C）
│       ├── flash.c/.h             #   Flash 配置存储（CRC32 + 写后回读）
│       ├── lcd.c/.h  font.c/.h    #   ILI9488 FSMC 驱动 + 字库
│       ├── rtc.c/.h  key.c/.h     #   实时时钟 / 按键扫描
│       ├── beep.c/.h timer.c/.h   #   蜂鸣器 / 1ms 系统滴答
│       └── usart.c/.h             #   双串口 + 512B 环形缓冲
├── Libraries/                     # ST 标准外设库 + CMSIS（含 12KB 栈的 startup）
├── Project/                       # Keil MDK5 工程（yehuoF103.uvprojx）
├── 智能药箱Web应用/
│   ├── app/                       # 前端：uni-app + Vue3 + Vite（H5/PWA）
│   ├── backend/                   # 后端：Express + 华为云 IoTDA SDK
│   │   └── server.example.js      #   凭证模板（复制为 server.local.js）
│   └── README.md                  # Web 端详细说明
├── ESP8266固件烧录资料/
│   └── esp8285-1MB-at/            # ESP-01S 可用的 1MB AT 固件 + 烧录地址表
├── 开发历程问题整理.docx            # 开发过程中遇到的问题：现象 / 原因 / 解决办法（47 条 + 附录 10 条）
├── tools/                         # 工程自用的辅助脚本与编码约定
│   ├── README.md                  # 脚本用途说明 + 源码编码约定（GBK 的原因与注意事项）
│   ├── gen_font.py                # 字库生成：从 simhei/consola 渲染点阵，输出 font.c/font.h
│   │                              #   （196 字形：95 ASCII + 11 数字 + 75 GBK16 + 15 GBK24）
│   │                              #   改界面中文文案后需把新文案加入本脚本的 UI_STRINGS 并重跑，
│   │                              #   否则该字在屏上显示为空白
│   ├── show.py                    # 带行号打印 GBK 源码，供阅读与检索
│   └── verify_numbers.py          # 复核常量：CRC 表与算法、结构体尺寸、PWM 预分频、角度换算
└── (第三方原理图与引脚表未收录，见下方说明)
```

> **行数统计口径**：以上行数均为**含注释与空行的物理行数**，统计范围是 `User/` 下 30 个入库的 `.c/.h`（不含本地专用的 `mqtt_huawei.local.h`）；本轮补充中文注释后行数较早期文档大幅增加，设备端共 9,643 行（剔除自动生成字库 `font.c` 749 行后，手写部分为 8,894 行）。

> **未纳入仓库的第三方参考文件**：`原子f103zet6总原理图.pdf`、
> `精英板V2 IO引脚分配表.xlsx` 为正点原子（ALIENTEK）官方资料，版权归其所有，
> 故不收录。这两个文件仍保留在本地工作区，需要时请从正点原子官方资料包获取。

---

## 十二、已知限制与改进方向

诚实记录当前不足，也是后续迭代的方向：

| 限制 | 影响 | 改进方向 |
|---|---|---|
| **后端无鉴权、CORS 全开** | 任何能访问 3000 端口的人都能远程开盒、改定时 | 加登录态 + Token 校验，限制 CORS 白名单 |
| **HTTPS 未启用** | 局域网访问时明文传输 | 上 TLS，前端改用 `import.meta.env` 注入地址 |
| **无看门狗** | 程序跑飞后无法自恢复 | 启用 IWDG，主循环喂狗 |
| **故障处理仅 `while(1)`** | HardFault 无现场信息、无复位 | 记录故障寄存器到 Flash 后触发软复位 |
| **上报为"尽力而为"，无发送队列** | 通道忙时按"不阻塞主循环"优先，宁可少报一条也不拖慢定时与界面 | 加环形缓冲队列，错峰补发（命令响应与定时下发已有背压重试，不受此影响） |
| **前端固定 5 秒轮询** | 实时性一般 | 改 MQTT over WebSocket 或 SSE 推送 |
| **`JsonGet*` 非严格 JSON 解析** | 键名出现在其他值里可能误匹配 | 引入轻量状态机 JSON 解析器 |
| **无 Service Worker** | PWA 无离线能力 | 加 SW 缓存静态资源 |
| **软件 I2C 无超时/总线恢复** | 总线卡死需重启 | 加 SCL 脉冲恢复 + 超时返回 |

---

## 许可证

本项目代码以 **MIT License** 发布，可自由学习与二次开发。

第三方组件版权归原作者所有：ST STM32F10x 标准外设库与 CMSIS（STMicroelectronics）、esp-at 固件与 ESP8266 NONOS SDK（Espressif Systems）、Flash Download Tools（Espressif）、安信可串口调试助手（Ai-Thinker）。

> **关于文件编码**：`User/` 下 30 个 `.c/.h` 为 **GBK** 编码（中文注释），请勿用会强制转 UTF-8 的编辑器直接保存，否则中文会损坏成乱码替换符；`README.md` 及 Web 端文件为 UTF-8。
>
> **关于换行符**：仓库内换行符**不统一（CRLF/LF 混合），不影响编译，未做归一化**。
