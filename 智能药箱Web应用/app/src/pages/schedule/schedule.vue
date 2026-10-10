<template>
  <view class="page">
    <!-- 药盒选择 -->
    <view class="box-tabs">
      <view
        v-for="b in 7"
        :key="b"
        class="box-tab"
        :class="{ active: b === curBox }"
        @click="selectBox(b)"
      >
        药盒{{ b }}
      </view>
    </view>

    <!-- 星期列表 -->
    <view class="week-list">
      <view
        v-for="(d, di) in weekDays"
        :key="di"
        class="week-row"
        @click="openDay(di)"
      >
        <text class="week-name">{{ d }}</text>
        <text class="week-times" :class="{ empty: dayTimes(di).length === 0 }">
          {{ dayTimes(di).length ? dayTimesText(di) : '未设置' }}
        </text>
        <text class="week-arrow">›</text>
      </view>
    </view>

    <!-- 单日编辑面板 -->
    <view class="day-panel" v-if="editingDay >= 0">
      <view class="panel-title">{{ weekDays[editingDay] }} 定时 (最多4个)</view>
      <view class="time-list">
        <view v-for="(tp, i) in editTimes" :key="i" class="time-row">
          <picker mode="time" :value="fmt(tp)" @change="onTime($event, i)">
            <view class="time-val">{{ fmt(tp) }}</view>
          </picker>
          <view class="time-del" @click="removeTime(i)">✕</view>
        </view>
        <view v-if="editTimes.length < 4" class="time-add" @click="addTime">
          + 添加时间点
        </view>
        <view v-if="editTimes.length === 0" class="time-empty">未设置时间点</view>
      </view>
      <view class="panel-btns">
        <view class="btn-cancel" @click="cancelEdit">取消</view>
        <view class="btn-ok" @click="applyDay">保存这一天</view>
      </view>
    </view>

    <view class="hint">
      点击星期设置该天的吃药时间，每天最多 4 个时间点。保存后立即下发到药箱。
    </view>

    <view class="save-btn" @click="save">保存本盒全部定时</view>

    <view class="toast" v-if="toast">{{ toast }}</view>
  </view>
</template>

<script>
const API_BASE = 'http://localhost:3000'; // TODO: 改成你的后端地址

export default {
  data() {
    return {
      curBox: 1,
      weekDays: ['周一', '周二', '周三', '周四', '周五', '周六', '周日'],
      boxes: Array.from({ length: 7 }, () => ({
        days: Array.from({ length: 7 }, () => ({ times: [] })),
      })),
      editingDay: -1,
      editTimes: [],
      toast: '',
    };
  },

  onLoad() {
    this.fetchSchedule();
  },

  methods: {
    showToast(msg) {
      this.toast = msg;
      setTimeout(() => (this.toast = ''), 2000);
    },

    fmt(tp) {
      const p = (n) => (n < 10 ? '0' + n : '' + n);
      return `${p(tp.h)}:${p(tp.m)}`;
    },

    dayTimes(di) {
      return (this.boxes[this.curBox - 1].days[di] || {}).times || [];
    },

    dayTimesText(di) {
      return this.dayTimes(di)
        .slice(0, 2)
        .map((t) => this.fmt(t))
        .join('  ') + (this.dayTimes(di).length > 2 ? ' …' : '');
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

    async fetchSchedule() {
      const res = await this.api('/api/schedule');
      if (res && res.success && res.boxes) {
        this.boxes = res.boxes.map((box) => ({
          days: box.days.map((day) => ({
            times: (day.times || []).map((t) => ({ h: t.h, m: t.m })),
          })),
        }));
      } else if (res) {
        this.showToast('获取定时失败: ' + (res.error || ''));
      }
    },

    selectBox(b) {
      this.curBox = b;
      this.editingDay = -1;
    },

    openDay(di) {
      this.editingDay = di;
      this.editTimes = this.dayTimes(di).map((t) => ({ h: t.h, m: t.m }));
    },

    addTime() {
      if (this.editTimes.length < 4) {
        this.editTimes.push({ h: 8, m: 0 });
      }
    },

    removeTime(i) {
      this.editTimes.splice(i, 1);
    },

    onTime(e, i) {
      const [h, m] = e.detail.value.split(':').map(Number);
      this.editTimes[i] = { h, m };
    },

    applyDay() {
      if (this.editTimes.length === 0) {
        this.showToast('请至少添加一个时间点');
        return;
      }
      const day = this.editingDay;
      // 按时间排序
      const times = this.editTimes.map((t) => ({ h: t.h, m: t.m }));
      times.sort((a, b) => a.h * 60 + a.m - (b.h * 60 + b.m));
      this.boxes[this.curBox - 1].days[day].times = times;
      this.editingDay = -1;
      this.showToast(`已设置 ${this.weekDays[day]} ${times.length} 个时间点`);
    },

    cancelEdit() {
      this.editingDay = -1;
    },

    async save() {
      this.showToast(`正在保存药盒${this.curBox}...`);
      const res = await this.api('/api/schedule', {
        method: 'POST',
        data: { box: this.curBox, days: this.boxes[this.curBox - 1].days },
      });
      if (res && res.success) {
        this.showToast(`药盒${this.curBox}定时已保存`);
        // 等设备上报后刷新 (设备收到 desired 解析保存并上报, 通常 1~3 秒)
        setTimeout(() => this.fetchSchedule(), 3000);
      } else if (res) {
        this.showToast('保存失败: ' + (res.error || ''));
      }
    },
  },
};
</script>

<style scoped>
.page {
  min-height: 100vh;
  background: #f5f7fa;
  padding: 20rpx;
}

.box-tabs {
  display: flex;
  justify-content: space-between;
  margin-bottom: 24rpx;
}
.box-tab {
  flex: 1;
  margin: 0 6rpx;
  background: #fff;
  text-align: center;
  padding: 16rpx 0;
  border-radius: 12rpx;
  font-size: 24rpx;
  color: #666;
  border: 2rpx solid transparent;
}
.box-tab.active {
  background: #4a90d9;
  color: #fff;
  font-weight: bold;
}

.week-list {
  background: #fff;
  border-radius: 20rpx;
  overflow: hidden;
  box-shadow: 0 4rpx 12rpx rgba(0, 0, 0, 0.06);
}
.week-row {
  display: flex;
  align-items: center;
  padding: 28rpx 24rpx;
  border-bottom: 1rpx solid #f0f0f0;
}
.week-row:last-child {
  border-bottom: none;
}
.week-name {
  width: 120rpx;
  font-size: 30rpx;
  color: #333;
}
.week-times {
  flex: 1;
  text-align: right;
  font-size: 28rpx;
  color: #4a90d9;
}
.week-times.empty {
  color: #bbb;
}
.week-arrow {
  margin-left: 16rpx;
  color: #ccc;
  font-size: 34rpx;
}

.day-panel {
  margin-top: 24rpx;
  background: #fff;
  border-radius: 20rpx;
  padding: 24rpx;
  box-shadow: 0 4rpx 12rpx rgba(0, 0, 0, 0.06);
}
.panel-title {
  font-size: 28rpx;
  color: #333;
  font-weight: bold;
  margin-bottom: 16rpx;
}
.time-list {
  border-top: 1rpx solid #f0f0f0;
  padding-top: 12rpx;
}
.time-row {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 20rpx 8rpx;
  border-bottom: 1rpx solid #f5f5f5;
}
.time-val {
  font-size: 34rpx;
  color: #333;
  font-weight: bold;
  padding: 8rpx 24rpx;
  background: #f9f9f9;
  border-radius: 10rpx;
}
.time-del {
  color: #ff4d4f;
  font-size: 30rpx;
  padding: 8rpx 20rpx;
}
.time-add {
  padding: 20rpx 8rpx;
  color: #4a90d9;
  font-size: 28rpx;
  text-align: center;
}
.time-empty {
  padding: 24rpx 8rpx;
  color: #bbb;
  font-size: 26rpx;
  text-align: center;
}
.panel-btns {
  display: flex;
  margin-top: 20rpx;
  gap: 20rpx;
}
.btn-cancel {
  flex: 1;
  background: #f0f0f0;
  color: #666;
  text-align: center;
  padding: 20rpx 0;
  border-radius: 12rpx;
  font-size: 28rpx;
}
.btn-ok {
  flex: 1;
  background: #4a90d9;
  color: #fff;
  text-align: center;
  padding: 20rpx 0;
  border-radius: 12rpx;
  font-size: 28rpx;
  font-weight: bold;
}

.hint {
  margin-top: 24rpx;
  padding: 20rpx;
  background: #fffbe6;
  border-radius: 12rpx;
  color: #b08c00;
  font-size: 24rpx;
  line-height: 1.6;
}

.save-btn {
  margin-top: 24rpx;
  background: #4a90d9;
  color: #fff;
  text-align: center;
  padding: 26rpx 0;
  border-radius: 14rpx;
  font-size: 30rpx;
  font-weight: bold;
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
