# 智能药箱 Web 端

远程控制智能药箱的网页端（H5 / 可安装的 Web 应用 PWA）。整体结构是前端（uni-app，构建为 H5/PWA）调用后端（Node.js），后端调用华为云 IoTDA，华为云再和家里的药箱设备通信。

设备端（STM32 固件）的接线、编译烧录、云平台配置与菜单操作见项目根目录的 [README.md](../README.md)。

## 目录结构

```
智能药箱Web应用/
├── backend/                  Node.js 后端（Express，所有华为云调用都在这）
│   ├── server.js             主服务，接口定义与华为云调用（凭证从 server.local.js / 环境变量读取）
│   ├── server.example.js     凭证模板（复制为 server.local.js 后填写）
│   └── package.json
└── app/                      uni-app 前端（Vue3 + Vite）
    ├── index.html            入口页面，带 PWA manifest 链接
    ├── package.json          uni-app CLI 依赖
    ├── vite.config.js
    └── src/
        ├── main.js           程序入口
        ├── manifest.json     uni-app 配置（含 PWA）
        ├── pages.json        页面注册
        ├── static/           PWA 图标和 manifest.webmanifest
        └── pages/
            ├── index/index.vue       主界面：设备状态、各药盒最近定时、远程开箱
            └── schedule/schedule.vue 定时设置：药盒、星期、每天最多 4 个时间点
```

## 一、准备华为云

1. 获取 AK/SK：华为云控制台，点右上角头像，进入我的凭证，访问密钥，新建访问密钥，记下 AK 和 SK
2. 获取项目 ID：我的凭证，项目，记下项目 ID 和区域（例如 cn-east-3）
3. 获取实例 ID：设备接入 IoTDA 控制台，总览页，实例信息里有（标准版和 4 元版需要）
4. 获取应用侧接入地址：IoTDA 控制台总览页，例如 https://<YOUR_IOTDA_APP_ENDPOINT>

说明：如果用 IAM 子用户的 AK/SK，必须用派生签名，代码里已经配好了（withDerivedPredicate 加 withRegionId）。如果用的是主账号的 AK/SK，可以把这两行去掉。

### 设备注册和产品模型

在 IoTDA 控制台注册设备（直连设备，MQTT 协议），得到设备 ID 和设备密钥。

产品模型（medicine_box_service 服务）需要配这些：

属性（访问权限都设成可读可写）：

| 属性名 | 类型 | 说明 |
|---|---|---|
| box_num | int | 最近触发的药盒号，0 表示全部关闭 |
| chufa_text | string | 触发方式文本 |
| chufa_time | string | 触发时间 |
| schedule_1 到 schedule_7 | string，长度 500 | 各药盒定时规则，7 个都要建 |

（这三个属性设备端只上报、不读取；云端修改它们不会影响设备。真正双向的是 schedule_1~7。）

命令：

| 命令名 | 下发参数 | 响应参数 |
|---|---|---|
| remote_open_box | target_box（int 1 到 7） | open_result（string） |
| set_schedule（可选，旧版兼容） | box、day（1 到 7 周一等于 1）、hour（0 到 24） | status（string） |

schedule_N 的线格式：每盒 56 个逗号分隔数字，等于 7 天乘 4 个时间点乘（时,分），顺序周一到周日，每点先时后分。未设置的时间点小时填 255。例如周一 8:30、其余 3 个时间点未设置，写作 8,30,255,0,255,0,255,0（一天 8 个数字）；整盒 = 周一 8 个 + 周二 8 个 + … + 周日 8 个 = 56 个数字。

为什么要分 7 个属性：ESP-01S 固件单条消息最长 512 字节，整表数据会被截断，分盒后每盒 150 到 200 字符能安全传输。设备收到哪盒就只更新哪盒。

## 二、启动后端

先装依赖再启动：

```bash
cd backend
npm install
npm start
```

后端运行在 http://localhost:3000，监听所有网卡。

### 配置凭证（必做，否则启动即退出）

后端不再把凭证写在源码里。二选一：

1. 复制模板再填写：cp server.example.js server.local.js，然后填入 AK/SK/项目ID/实例ID/设备ID/接入地址；
2. 或设置环境变量（同名大写）：HW_AK、HW_SK、HW_PROJECT_ID、HW_INSTANCE_ID、HW_DEVICE_ID、HW_ENDPOINT、HW_REGION、HW_SERVICE_ID、HW_COMMAND_NAME。

缺少必填项时后端会打印 [配置缺失] 并以退出码 1 结束 —— 这是预期行为，不是后端坏了。

server.local.js 已在 .gitignore 中，不会被提交。

代码里已经处理好的几个坑（一般不用动）：

1. 强制 IPv4：给 SDK 传了 https.Agent 设置 family 为 4。华为云域名有时解析到 IPv6，本机 IPv6 不通就会连接超时
2. 自动重试：所有华为云调用包了重试逻辑（最多 3 次，间隔 2 秒），网络抖动时不容易失败
3. 定时下发用修改设备属性接口 updateProperties，华为云通过 properties/set 主题推给设备。注意不要用 UpdateDeviceShadowDesiredData，那个接口只改影子里的期望值，不会下发给设备，用了就会前端显示成功但设备没反应

验证：浏览器打开 http://localhost:3000/api/device，返回 success true 和在线状态就说明后端正常。

## 三、运行前端

方式一：CLI 方式（推荐，只需要 npm）

```bash
cd app
npm install
npm run dev:h5
```

浏览器打开 http://localhost:5173。源码在 app/src/ 下，改完保存自动刷新。

方式二：HBuilderX

用 HBuilderX 打开 app/ 文件夹，运行到浏览器或手机。注意源码在 src/ 子目录。如果 HBuilderX 装不上 uni-app 编译器插件（国内网络经常卡在下载 DCloud CDN 依赖），就用方式一。

手机访问（和电脑同一个 WiFi）：

1. 手机和电脑连同一个 WiFi
2. 把 app/src/pages/index/index.vue 和 app/src/pages/schedule/schedule.vue 里的 API_BASE 改成 http://电脑局域网IP:3000
3. 手机浏览器打开 http://电脑局域网IP:5173

## 四、做成可安装的 Web 应用（PWA 添加到主屏幕）

项目已经配好了 PWA（manifest 和图标），手机浏览器打开后添加到主屏幕就能像原生应用一样全屏运行，不用打包。

1. 电脑启动后端：cd backend 然后 npm start
2. 电脑启动前端：cd app 然后 npm run dev:h5，记下电脑的局域网 IP
3. 把两处 API_BASE 改成 http://电脑局域网IP:3000
4. 手机连同一个 WiFi，浏览器打开 http://电脑局域网IP:5173
5. 添加到主屏幕：安卓的 Chrome 浏览器菜单里有添加到主屏幕，苹果的 Safari 点分享按钮选添加到主屏幕
6. 主屏幕出现智能药箱图标，点开就是全屏应用

注意：这个方案后端要在电脑上运行，手机通过局域网访问。要彻底脱离电脑，把后端部署到云服务器，前端构建产物放到静态服务器，见第六节。

## 五、接口说明

| 接口 | 方法 | 功能 |
|---|---|---|
| /api/device | GET | 查设备在线状态 |
| /api/shadow | GET | 查设备影子（药盒状态） |
| /api/open-box | POST，参数 box | 下发远程开箱命令，同步等设备响应 |
| /api/schedule | GET | 查定时规则（7 盒乘 7 天乘时间点列表） |
| /api/schedule | POST，参数 box 和 days | 设置整盒定时，只下发改动的那盒 |

## 六、部署到服务器（可选）

后端：把 backend/ 目录放到云服务器或轻量服务器，npm install 后 npm start，建议用 pm2 守护进程。也可以改成华为云 FunctionGraph 云函数。

前端：在 app/ 目录执行 npm run build:h5，把 dist/build/h5/ 里的文件放到 Nginx 或对象存储静态托管。

部署后手机随时随地都能访问，不用电脑开机，也不用同一个 WiFi。

## 七、常见问题排查

| 现象 | 原因 | 解决办法 |
|---|---|---|
| 后端启动报 EADDRINUSE | 3000 端口被旧进程占用 | 任务管理器结束 node 进程，或 netstat -ano 找 PID 结束 |
| 前端打不开 | 5173 被占用，vite 自动换端口 | 看启动日志里的 Local 地址，或结束占用进程 |
| 接口报 connect ETIMEDOUT | 网络 IPv6 不通 | 代码已强制 IPv4，重启后端；还不行检查网络 |
| 保存定时报 IOTDA.013002 | 产品模型缺 schedule_N 属性或权限不是可读可写 | 控制台建好 7 个属性，权限改可读可写，等一两分钟再试 |
| 保存定时报 IOTDA.014111 | 设备不在线，或设备固件没有按盒解析 | 设备上电联网，重新编译烧录最新固件 |
| 前端设置成功但设备没变化 | 后端用了 UpdateDeviceShadowDesiredData | 确认 server.js 用的是 client.updateProperties |
| 设备端改定时前端没变化 | 前端轮询间隔，或设备没上报 | 看设备串口有没有 schedule_N report，稍等几秒 |
| 前端显示 00:00 而不是未设置 | 影子里有旧的全 0 脏数据 | 后端已有兜底处理，设备端重新设置一次覆盖 |
| 设备端显示星期星期五 | 旧固件的显示 bug | 已修复，重新烧录 |
