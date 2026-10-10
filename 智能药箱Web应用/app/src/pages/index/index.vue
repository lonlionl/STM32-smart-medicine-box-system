<template>
  <view class="page">
    <!-- 顶部标题 -->
    <view class="header">
      <view class="header-title">智能药箱</view>
      <view class="header-sub">远程智能控制</view>
    </view>

    <!-- 设备状态 -->
    <view class="status-card">
      <view class="status-row">
        <text class="status-label">设备状态</text>
        <text class="status-dot" :class="online ? 'dot-on' : 'dot-off'"></text>
        <text class="status-text" :style="{ color: online ? '#52C41A' : '#FF4D4F' }">
          {{ online ? '在线' : '离线' }}
        </text>
      </view>
      <view class="status-row" v-if="nextAlarm">
        <text class="status-label">下次服药</text>
        <text class="status-value" style="color:#4a90d9;font-weight:bold">{{ nextAlarm }}</text>
      </view>
      <view class="status-row" v-else>
        <text class="status-label">下次服药</text>
        <text class="status-value">未设置</text>
      </view>
      <view class="status-row" v-if="lastTime">
        <text class="status-label">最近上报</text>
        <text class="status-value">{{ lastTime }}</text>
      </view>
    </view>

    <!-- 药盒网格 -->
    <view class="box-grid">
      <view
        v-for="b in boxes"
        :key="b.num"
        class="box-card"
        :class="{ 'box-opened': b.opened }"
        @click="openBox(b.num)"
      >
        <view class="box-top">
          <text class="box-num">{{ b.num }}</text>
          <text class="box-state" :class="b.opened ? 'state-open' : 'state-close'">
            {{ b.opened ? '已开' : '待开' }}
          </text>
        </view>
        <text class="box-time">{{ b.time }}</text>
        <view class="open-btn" :class="b.opened ? 'btn-disabled' : ''">
          {{ b.opened ? '已开启' : '点击开箱' }}
        </view>
      </view>
    </view>

    <!-- 定时设置入口 -->
    <view class="schedule-entry" @click="goSchedule">
      <text class="entry-label">定时设置</text>
      <text class="entry-sub">修改每个药盒的吃药时间</text>
      <text class="entry-arrow">›</text>
    </view>

    <!-- 底部提示 -->
    <view class="footer">
      <text>点击药盒可远程开箱 · 数据来自华为云</text>
    </view>

    <!-- 加载/提示 -->
    <view class="toast" v-if="toast">{{ toast }}</view>
  </view>
</template>

<script>
const API_BASE = 'http://localhost:3000'; // TODO: 改成你的后端地址

export default {
  data() {
    return {
      online: false,
      lastTime: '',
      nextAlarm: '',
      toast: '',
      boxes: Array.from({ length: 7 }, (_, i) => ({
        num: i + 1,
        opened: false,
        time: '未设置',
      })),
      timer: null,
    };
  },

  onLoad() {
    this.refresh();
    // 每 5 秒刷新状态
    this.timer = setInterval(() => this.refresh(), 5000);
  },

  onUnload() {
    if (this.timer) clearInterval(this.timer);
  },

  methods: {
    showToast(msg) {
      this.toast = msg;
      setTimeout(() => (this.toast = ''), 2000);
    },

    async api(path, options = {}) {
      try {
        const res = await uni.request({
          url: API_BASE + path,
          method: options.method || 'GET',
          data: options.data || {},
          timeout: 10000,
        });
        return res.data;
      } catch (e) {
        this.showToast('网络错误，请检查后端');
        return null;
      }
    },

    async refresh() {
      // 查设备状态
      const dev = await this.api('/api/device');
      if (dev && dev.success) {
        this.online = !!dev.online;
      }

      // 查影子（药盒状态）
      const shadow = await this.api('/api/shadow');
      if (shadow && shadow.success) {
        this.parseShadow(shadow.data);
      }

      // 查定时配置, 更新每个药盒的最近定时 + 全局下次服药
      const sched = await this.api('/api/schedule');
      if (sched && sched.success && sched.boxes) {
        this.updateBoxTimes(sched.boxes);
      }
    },

    // 计算某个药盒距当前最近的未来触发时刻 (分钟数); 无定时返回 null
    boxNextTrig(box, todayIdx, nowMin) {
      let best = null;
      for (let d = 0; d < 7; d++) {
        const day = box.days[d] || { times: [] };
        for (const tp of day.times) {
          const hm = tp.h * 60 + tp.m;
          let dayOff = (d - todayIdx + 7) % 7;
          let trig = dayOff * 1440 + hm;
          if (trig <= nowMin) trig += 7 * 1440;
          if (!best || trig < best) best = trig;
        }
      }
      return best;
    },

    // 按最近触发时刻找对应 (星期, 时, 分)
    boxNextAlarm(box, todayIdx, nowMin, bestTrig) {
      if (bestTrig === null) return null;
      for (let d = 0; d < 7; d++) {
        const day = box.days[d] || { times: [] };
        for (const tp of day.times) {
          const hm = tp.h * 60 + tp.m;
          let dayOff = (d - todayIdx + 7) % 7;
          let trig = dayOff * 1440 + hm;
          if (trig <= nowMin) trig += 7 * 1440;
          if (trig === bestTrig) return { day: d, h: tp.h, m: tp.m };
        }
      }
      return null;
    },

    updateBoxTimes(boxes) {
      const now = new Date();
      const nowMin = now.getHours() * 60 + now.getMinutes();
      const todayIdx = (now.getDay() + 6) % 7; // 周一=0
      const weekNames = ['周一', '周二', '周三', '周四', '周五', '周六', '周日'];
      const p = (n) => (n < 10 ? '0' + n : '' + n);
      let best = null;

      this.boxes = this.boxes.map((b, i) => {
        const box = boxes[i] || { days: [] };
        const trig = this.boxNextTrig(box, todayIdx, nowMin);
        let time = '未设置';
        if (trig !== null) {
          const a = this.boxNextAlarm(box, todayIdx, nowMin, trig);
          if (a) time = `${weekNames[a.day]} ${p(a.h)}:${p(a.m)}`;
          if (!best || trig < best) best = trig;
        }
        return { ...b, time };
      });

      // 全局下次服药: 各盒最近定时里最早的那个
      this.nextAlarm = null;
      for (let i = 0; i < 7; i++) {
        const trig = this.boxNextTrig(boxes[i] || { days: [] }, todayIdx, nowMin);
        if (trig !== null && (!best || trig < best)) best = trig;
      }
      if (best !== null) {
        for (let i = 0; i < 7; i++) {
          const a = this.boxNextAlarm(boxes[i] || { days: [] }, todayIdx, nowMin, best);
          if (a) {
            this.nextAlarm = `药盒${i + 1} ${weekNames[a.day]} ${p(a.h)}:${p(a.m)}`;
            break;
          }
        }
      }
    },

    parseShadow(data) {
      // 解析设备影子里的属性 (box_num 等)
      // 影子可能含多个 service (如残留的 s1)，必须取 medicine_box_service
      try {
        const entry =
          data.shadow &&
          data.shadow.find((s) => s && s.service_id === 'medicine_box_service');
        const reported = entry && entry.reported;
        if (reported && reported.properties) {
          const props = reported.properties;
          // 更新最近上报时间
          if (props.chufa_time) {
            this.lastTime = props.chufa_time;
          }
          // box_num: 0 = 设备全部关闭(复位为待开), 1~7 = 设备触发了该药盒
          if (props.box_num !== undefined) {
            const n = parseInt(props.box_num, 10);
            if (n === 0) {
              this.boxes = this.boxes.map((b) => ({ ...b, opened: false }));
            } else if (n >= 1 && n <= 7) {
              this.boxes = this.boxes.map((b) => ({
                ...b,
                opened: b.opened || b.num === n,
              }));
            }
          }
        }
      } catch (e) {
        console.log('解析影子失败', e);
      }
    },

    async openBox(num) {
      if (this.boxes[num - 1].opened) {
        this.showToast(`药盒${num}已开启`);
        return;
      }
      this.showToast(`正在开启药盒${num}...`);
      const res = await this.api('/api/open-box', {
        method: 'POST',
        data: { box: num },
      });
      if (res && res.success) {
        // 命令已送达设备 (同步等待设备响应) — 本轮会话标记为已开
        this.boxes = this.boxes.map((b) => ({
          ...b,
          opened: b.num === num ? true : b.opened,
        }));
        this.showToast(`药盒${num}已开启`);
      } else if (res) {
        this.showToast('开箱失败: ' + (res.error || ''));
      }
    },

    goSchedule() {
      uni.navigateTo({ url: '/pages/schedule/schedule' });
    },
  },
};
</script>

<style scoped>
.page {
  min-height: 100vh;
  background: linear-gradient(180deg, #4a90d9 0%, #f5f7fa 280rpx);
  padding: 20rpx;
}

.header {
  padding: 40rpx 20rpx 30rpx;
  color: #fff;
}
.header-title {
  font-size: 48rpx;
  font-weight: bold;
}
.header-sub {
  font-size: 24rpx;
  opacity: 0.8;
  margin-top: 8rpx;
}

.status-card {
  background: #fff;
  border-radius: 20rpx;
  padding: 24rpx;
  margin-bottom: 24rpx;
  box-shadow: 0 4rpx 12rpx rgba(0, 0, 0, 0.06);
}
.status-row {
  display: flex;
  align-items: center;
  margin-bottom: 8rpx;
}
.status-label {
  width: 160rpx;
  color: #999;
  font-size: 26rpx;
}
.status-text {
  font-size: 28rpx;
  font-weight: bold;
}
.status-value {
  font-size: 26rpx;
  color: #333;
}
.status-dot {
  width: 16rpx;
  height: 16rpx;
  border-radius: 50%;
  margin-right: 12rpx;
}
.dot-on {
  background: #52c41a;
}
.dot-off {
  background: #ff4d4f;
}

.box-grid {
  display: flex;
  flex-wrap: wrap;
  justify-content: space-between;
}
.box-card {
  width: 30%;
  background: #fff;
  border-radius: 16rpx;
  padding: 20rpx;
  margin-bottom: 20rpx;
  text-align: center;
  box-shadow: 0 4rpx 12rpx rgba(0, 0, 0, 0.06);
  transition: all 0.3s;
}
.box-card.box-opened {
  background: #e6f7ff;
  border: 2rpx solid #4a90d9;
}
.box-top {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 10rpx;
}
.box-num {
  font-size: 40rpx;
  font-weight: bold;
  color: #4a90d9;
}
.box-state {
  font-size: 22rpx;
  padding: 4rpx 12rpx;
  border-radius: 20rpx;
}
.state-open {
  background: #4a90d9;
  color: #fff;
}
.state-close {
  background: #f0f0f0;
  color: #999;
}
.box-time {
  display: block;
  font-size: 22rpx;
  color: #666;
  margin-bottom: 16rpx;
}
.open-btn {
  background: #4a90d9;
  color: #fff;
  font-size: 24rpx;
  padding: 12rpx 0;
  border-radius: 12rpx;
}
.btn-disabled {
  background: #ccc;
}

.schedule-entry {
  background: #fff;
  border-radius: 20rpx;
  padding: 24rpx;
  margin-bottom: 24rpx;
  display: flex;
  align-items: center;
  box-shadow: 0 4rpx 12rpx rgba(0, 0, 0, 0.06);
}
.entry-label {
  font-size: 30rpx;
  color: #333;
  font-weight: bold;
  margin-right: 20rpx;
}
.entry-sub {
  flex: 1;
  font-size: 24rpx;
  color: #999;
}
.entry-arrow {
  color: #ccc;
  font-size: 34rpx;
}

.footer {
  text-align: center;
  color: #999;
  font-size: 22rpx;
  padding: 20rpx 0 40rpx;
}

.toast {
  position: fixed;
  top: 50%;
  left: 50%;
  transform: translate(-50%, -50%);
  background: rgba(0, 0, 0, 0.7);
  color: #fff;
  padding: 20rpx 40rpx;
  border-radius: 16rpx;
  font-size: 28rpx;
  z-index: 999;
}
</style>
