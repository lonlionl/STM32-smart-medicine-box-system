/**
 * 华为云 IoTDA 应用侧配置模板
 * ============================
 * 用法：把本文件复制为 server.local.js，填入你自己的值。
 *       server.local.js 已写进 .gitignore，不会被提交。
 *
 *   cp server.example.js server.local.js
 *
 * 也可以不用本文件，改为设置环境变量（同名的大写下划线形式）：
 *   HW_AK / HW_SK / HW_PROJECT_ID / HW_INSTANCE_ID / HW_DEVICE_ID / HW_ENDPOINT / HW_REGION
 *
 * 各项在哪里找：
 *   ak / sk      → 华为云控制台 → 右上角账号 → 我的凭证 → 访问密钥
 *                  （IAM 子用户必须用「编程访问」方式创建，且代码里已启用派生签名）
 *   projectId    → 我的凭证 → 项目列表
 *   instanceId   → IoTDA 控制台 → 总览 → 实例 ID
 *                  （基础版可留空；标准版/企业版必填，否则请求缺少 Instance-Id 头）
 *   deviceId     → IoTDA 控制台 → 设备 → 点进设备 → 设备 ID
 *                  （形如 <产品ID>_<设备标识>，例如 65f0a1b2c3d4e5f6a7b8c9d0_medicinebox）
 *   endpoint     → IoTDA 控制台 → 总览 → 应用侧接入地址
 *                  （形如 https://xxxxxxxxxx.st1.iotda-app.cn-east-3.myhuaweicloud.com）
 *   region       → 形如 cn-east-3
 */
module.exports = {
  ak: '<YOUR_HUAWEI_AK>',
  sk: '<YOUR_HUAWEI_SK>',
  projectId: '<YOUR_PROJECT_ID>',
  instanceId: '<YOUR_IOTDA_INSTANCE_ID>',
  deviceId: '<YOUR_DEVICE_ID>',
  endpoint: 'https://<YOUR_IOTDA_APP_ENDPOINT>',
  region: 'cn-east-3',

  // 产品模型里定义的，通常不用改
  serviceId: 'medicine_box_service',
  commandName: 'remote_open_box',
};
