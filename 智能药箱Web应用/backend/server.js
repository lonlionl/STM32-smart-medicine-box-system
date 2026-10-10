/**
 * 智能药箱 - 后端代理服务
 * 用华为云 IoTDA 应用侧 API 下发命令、查询设备影子
 *
 * 部署: Node.js 服务器或华为云 FunctionGraph
 * 依赖: npm install express @huaweicloud/huaweicloud-sdk-iotda cors uuid
 *
 * 配置: 复制 server.example.js 为 server.local.js 并填入你自己的华为云凭证，
 *       或直接设置环境变量 HW_AK / HW_SK / HW_PROJECT_ID / HW_INSTANCE_ID /
 *       HW_DEVICE_ID / HW_ENDPOINT / HW_REGION。
 *       首次使用请把产品模型、设备、实例都换成你自己的，否则启动即报错。
 */
// 华为云域名有时优先解析到 IPv6，而网络环境 IPv6 不通，强制用 IPv4
const dns = require('dns');
dns.setDefaultResultOrder('ipv4first');

// 网络到华为云偶发超时，包装一个重试 (最多 3 次, 间隔 2s)
async function withRetry(fn, tries = 3) {
  let lastErr;
  for (let i = 0; i < tries; i++) {
    try {
      return await fn();
    } catch (e) {
      lastErr = e;
      if (i < tries - 1) await new Promise((r) => setTimeout(r, 2000));
    }
  }
  throw lastErr;
}

const express = require('express');
const cors = require('cors');
const {
  IoTDAClient,
  IoTDARegion,
  ShowDeviceRequest,
  ShowDeviceShadowRequest,
  UpdatePropertiesRequest,
  CreateCommandRequest,
} = require('@huaweicloud/huaweicloud-sdk-iotda');
const { Region, BasicCredentials } = require('@huaweicloud/huaweicloud-sdk-core');

// ==================== 华为云配置 ====================
// 优先读 server.local.js（已 gitignore），没有就用环境变量。
// 仓库里不保存任何真实凭证，缺失时直接退出，避免误用示例值连到别人的账号。
let local = {};
try {
  local = require('./server.local.js');
} catch (e) {
  /* 只在"server.local.js 这个文件本身不存在"时静默跳过。
   * 不能只判 e.code === 'MODULE_NOT_FOUND' —— 如果文件存在、但它内部 require
   * 的某个模块写错了名字, 同样会抛 MODULE_NOT_FOUND, 那样就会被错误地吞掉,
   * 然后报成"HW_AK 未设置", 把人引向完全无关的方向。
   * 因此额外要求错误信息里出现 server.local.js。 */
  const notFound = e.code === 'MODULE_NOT_FOUND' &&
    /server\.local\.js/.test(String(e.message || ''));
  if (!notFound) throw e;
}

/* 兜底: 任何未处理的 Promise 拒绝都不要让它结束进程。
 * Express 4 不会接管 async 路由处理函数抛出的错误, 默认行为是进程退出,
 * 那样一个畸形请求就能把服务打崩(远程开箱功能随之全线下线)。
 * 这里至少保证进程活着, 并留下可排查的日志。 */
process.on('unhandledRejection', (reason) => {
  console.error('[未处理的 Promise 拒绝]', reason);
});

function required(name, fallback) {
  const v = fallback !== undefined && fallback !== null && fallback !== '' ? fallback : undefined;
  /* 同时拦截"照抄了模板但没改"的情况: server.example.js 里的占位符形如 <YOUR_HUAWEI_AK>。
   * 只判空的话, 直接 cp 出来的 server.local.js 会让服务"看起来启动成功",
   * 然后每次请求都收到华为云的签名/鉴权错误 —— 根因很难定位。 */
  const isPlaceholder = typeof v === 'string' && /^<.*>$/.test(v.trim());
  if (!v || isPlaceholder) {
    console.error(
      `[配置缺失或未填写] ${name} = ${v === undefined ? '(空)' : JSON.stringify(v)}。` +
        `请复制 server.example.js 为 server.local.js 并填入真实值，或设置同名环境变量 ${name}。`
    );
    process.exit(1);
  }
  return v;
}

const CONFIG = {
  // 华为云账号 AK/SK (控制台 -> 我的凭证 -> 访问密钥)
  ak: required('HW_AK', process.env.HW_AK || local.ak),
  sk: required('HW_SK', process.env.HW_SK || local.sk),
  // 项目 ID (控制台 -> 我的凭证 -> 项目)
  projectId: required('HW_PROJECT_ID', process.env.HW_PROJECT_ID || local.projectId),
  // 区域 (如 cn-east-3)
  region: process.env.HW_REGION || local.region || 'cn-east-3',
  // IoTDA 实例 ID (标准版/企业版需要；基础版可留空)
  instanceId: process.env.HW_INSTANCE_ID || local.instanceId || '',
  // 设备 ID (控制台 -> 设备 -> 设备详情)
  deviceId: required('HW_DEVICE_ID', process.env.HW_DEVICE_ID || local.deviceId),
  // 服务 ID 和命令名 (产品模型里定义的)
  serviceId: process.env.HW_SERVICE_ID || local.serviceId || 'medicine_box_service',
  commandName: process.env.HW_COMMAND_NAME || local.commandName || 'remote_open_box',
  // 标准版/企业版的应用侧接入地址 (IoTDA控制台 -> 总览 -> 应用侧接入地址)
  endpoint: required('HW_ENDPOINT', process.env.HW_ENDPOINT || local.endpoint),
};

// ==================== 初始化客户端 ====================
const ENDPOINT = CONFIG.endpoint;
const region = new Region(CONFIG.region, ENDPOINT);

// IAM 子用户 + 自定义 endpoint 需要派生签名 (DerivedCredentials)
const credentials = new BasicCredentials({
  ak: CONFIG.ak,
  sk: CONFIG.sk,
  projectId: CONFIG.projectId,
})
  .withDerivedPredicate(() => true)
  .withRegionId(CONFIG.region);

// 强制 IPv4: 华为云域名解析到 IPv6 时连接不通, 用 https.Agent family:4 只走 IPv4
const https = require('https');
const ipv4Agent = new https.Agent({ family: 4, keepAlive: true });

const client = IoTDAClient.newBuilder()
  .withCredential(credentials)
  .withRegion(region)
  .withOptions({ axiosRequestConfig: { httpsAgent: ipv4Agent } })
  .build();

// 标准版/企业版: 给 Request 加实例 ID 头
function withInstanceId(request) {
  if (CONFIG.instanceId) {
    request.withInstanceId(CONFIG.instanceId);
  }
  return request;
}

// ==================== Express 服务 ====================
const app = express();
app.use(cors());
app.use(express.json());

/**
 * 下发远程开箱命令
 * POST /api/open-box  { "box": 1 }
 */
app.post('/api/open-box', async (req, res) => {
  const box = parseInt(req.body.box, 10);
  if (isNaN(box) || box < 1 || box > 7) {
    return res.status(400).json({ error: 'box 必须在 1~7 之间' });
  }

  try {
    const request = withInstanceId(new CreateCommandRequest(CONFIG.deviceId)).withBody({
      service_id: CONFIG.serviceId,
      command_name: CONFIG.commandName,
      paras: { target_box: box },
    });
    const result = await withRetry(() => client.createCommand(request));
    res.json({ success: true, command_id: result.command_id, data: result });
  } catch (e) {
    res.status(500).json({ error: e.message || '下发命令失败' });
  }
});

/**
 * 解析单盒 schedule 线格式 (56 数字: 7天 × 4点 × (h,m); 未用点 h=255)
 * 返回 { days: [ { times: [{h,m}] } ×7 ] }
 */
function parseOneBox(raw) {
  if (typeof raw !== 'string') return null;
  const vals = raw.split(',').map(Number);
  if (vals.length !== 56 || vals.some((v) => isNaN(v))) return null;
  // 整盒全 0 = 未设置 (旧 bug 写入的脏数据), 视为空
  if (vals.every((v) => v === 0)) return null;
  const days = [];
  for (let d = 0; d < 7; d++) {
    const times = [];
    for (let t = 0; t < 4; t++) {
      const off = (d * 8) + (t * 2);
      const h = vals[off];
      if (h !== 255) times.push({ h, m: vals[off + 1] });
    }
    days.push({ times });
  }
  return days;
}

/** 把单盒 days 序列化回 56 数字线格式 */
function serializeOneBox(days) {
  const out = [];
  for (let d = 0; d < 7; d++) {
    const day = days[d] || { times: [] };
    for (let t = 0; t < 4; t++) {
      const tp = day.times[t];
      const h = tp && tp.h !== undefined && tp.h !== 255 ? tp.h : 255;
      const m = tp && tp.h !== undefined && tp.h !== 255 ? tp.m : 0;
      out.push(h, m);
    }
  }
  return out.join(',');
}

/**
 * 查询定时规则 (读影子 schedule_1..schedule_7)
 * GET /api/schedule
 * 返回 { boxes: [7盒 × 7天 × 时间点列表] }
 */
app.get('/api/schedule', async (req, res) => {
  try {
    const result = await withRetry(() => client.showDeviceShadow(withInstanceId(new ShowDeviceShadowRequest(CONFIG.deviceId))));
    const entry =
      result.shadow &&
      result.shadow.find((s) => s && s.service_id === CONFIG.serviceId);
    const props = entry && entry.reported && entry.reported.properties;
    const boxes = [];
    for (let b = 0; b < 7; b++) {
      const raw = props && props[`schedule_${b + 1}`];
      boxes.push({ days: parseOneBox(raw) || Array.from({ length: 7 }, () => ({ times: [] })) });
    }
    res.json({ success: true, boxes });
  } catch (e) {
    res.status(500).json({ error: e.message || '查询定时失败' });
  }
});

/**
 * 设置整盒定时 (通过影子 desired 下发, 设备收到后保存并上报)
 * POST /api/schedule  { box: 1~7, days: [ { times:[{h,m}..] } ×7 ] }
 *   每盒 7 天, 每天最多 4 个时间点
 */
app.post('/api/schedule', async (req, res) => {
  try {
    /* 注意: 全部入参校验都必须放在 try 里面。
     * Express 4 不会接管 async handler 抛出的错误, 一旦在 try 之外抛异常,
     * 就会变成 unhandledRejection —— Node 默认直接结束进程,
     * 表现为"发一个畸形请求就能把整个后端打崩"。
     * 这里同时把校验写严: req.body 可能是 undefined, days 的元素也可能是 null。 */
    const body = req.body || {};
    const box = parseInt(body.box, 10);
    const days = body.days;
    if (isNaN(box) || box < 1 || box > 7) {
      return res.status(400).json({ error: 'box 必须在 1~7 之间' });
    }
    if (!Array.isArray(days) || days.length !== 7) {
      return res.status(400).json({ error: 'days 必须包含 7 天' });
    }
    for (const day of days) {
      /* day 可能是 null; 用可选链避免 TypeError */
      const rawTimes = day && Array.isArray(day.times) ? day.times.slice(0, 4) : [];
      for (const tp of rawTimes) {
        /* 必须用 Number.isInteger 而不是 isNaN: 后者会放过 null 与 ''
         * (isNaN(null) === false), 那样会序列化出空字段, 设备解析成 0 点。 */
        if (!tp || !Number.isInteger(tp.h) || !Number.isInteger(tp.m) ||
            tp.h < 0 || tp.h > 23 || tp.m < 0 || tp.m > 59) {
          return res.status(400).json({ error: 'times 时间点无效（h/m 必须是 0~23/0~59 的整数）' });
        }
      }
    }

    // 只下发改动的那一盒 (schedule_N), 每盒 56 数字 < ESP-01S 512 限制
    const schedule = serializeOneBox(days.map((day) => ({
      times: (day && Array.isArray(day.times) ? day.times.slice(0, 4) : [])
        .map((tp) => ({ h: tp.h, m: tp.m })),
    })));

    // 用"修改设备属性"接口下发, 华为云通过 properties/set 推给设备
    const request = withInstanceId(new UpdatePropertiesRequest(CONFIG.deviceId)).withBody({
      services: [{ service_id: CONFIG.serviceId, properties: { [`schedule_${box}`]: schedule } }],
    });
    const result = await withRetry(() => client.updateProperties(request));
    res.json({ success: true, data: result });
  } catch (e) {
    res.status(500).json({ error: e.message || '设置定时失败' });
  }
});

/**
 * 查询设备影子 (获取药盒状态)
 * GET /api/shadow
 */
app.get('/api/shadow', async (req, res) => {
  try {
    const result = await withRetry(() => client.showDeviceShadow(withInstanceId(new ShowDeviceShadowRequest(CONFIG.deviceId))));
    res.json({ success: true, data: result });
  } catch (e) {
    res.status(500).json({ error: e.message || '查询影子失败' });
  }
});

/**
 * 查询设备在线状态
 * GET /api/device
 */
app.get('/api/device', async (req, res) => {
  try {
    const result = await withRetry(() => client.showDevice(withInstanceId(new ShowDeviceRequest(CONFIG.deviceId))));
    res.json({ success: true, online: result.status === 'ONLINE', data: result });
  } catch (e) {
    res.status(500).json({ error: e.message || '查询设备失败' });
  }
});

// 启动
const PORT = process.env.PORT || 3000;
app.listen(PORT, '0.0.0.0', () => {
  console.log(`智能药箱后端已启动: http://0.0.0.0:${PORT}`);
  console.log(`设备: ${CONFIG.deviceId}`);
});
