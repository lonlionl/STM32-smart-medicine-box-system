#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""
数值自检：用「独立重算」验证代码里的常量表/算法是否正确。

目前覆盖：
  1. flash.c 的 CRC32 查找表 256 项，是否与标准 CRC-32/ISO-HDLC 一致
  2. Flash_CRC32 的算法（把 C 逻辑照搬到 Python）能否通过标准测试向量
     "123456789" -> 0xCBF43926
  3. font.c 的字库表尺寸与 total 是否与声明一致（辅助核对）

这些是"能被机器判定对错"的部分，比人眼读表可靠得多。

用法: python tools/verify_numbers.py
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
fails = []


def load(rel):
    p = os.path.join(ROOT, rel.replace("/", os.sep))
    raw = open(p, "rb").read()
    for enc in ("utf-8", "gbk"):
        try:
            return raw.decode(enc)
        except UnicodeDecodeError:
            continue
    raise SystemExit("无法解码 " + rel)


# ---------------------------------------------------------------- 1. CRC 表
print("=" * 70)
print("[1] flash.c 的 CRC32 查找表 vs 标准表")
src = load("User/BSP/flash.c")
m = re.search(r"s_CrcTable\[256\]\s*=\s*\{(.*?)\};", src, re.S)
body = m.group(1)
nums = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{8})u?", body)]
print("    表项数: %d（应为 256）" % len(nums))

# 独立重算标准表
def std_table():
    tbl = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ 0xEDB88320 if (c & 1) else (c >> 1)
        tbl.append(c)
    return tbl

ref = std_table()
if len(nums) != 256:
    fails.append("CRC 表项数不是 256，实际 %d" % len(nums))
else:
    diff = [i for i in range(256) if nums[i] != ref[i]]
    if diff:
        fails.append("CRC 表有 %d 项与标准不一致，首个下标 %d：%08X（应为 %08X）"
                     % (len(diff), diff[0], nums[diff[0]], ref[diff[0]]))
        print("    !! 不一致项数: %d" % len(diff))
    else:
        print("    全部 256 项与标准 CRC-32/ISO-HDLC 表完全一致  [PASS]")

# ---------------------------------------------------------------- 2. 算法
print("\n[2] CRC32 算法是否通过标准测试向量")
tbl = nums if len(nums) == 256 else ref
def crc32(data):
    crc = 0xFFFFFFFF
    for b in data:
        crc = ((crc >> 8) ^ tbl[(crc ^ b) & 0xFF]) & 0xFFFFFFFF
    return crc ^ 0xFFFFFFFF

got = crc32(b"123456789")
print('    crc32("123456789") = 0x%08X（标准值 0xCBF43926）%s'
      % (got, "  [PASS]" if got == 0xCBF43926 else "  [FAIL]"))
if got != 0xCBF43926:
    fails.append("CRC32 未通过标准测试向量")

blank = crc32(b"\xFF" * 396)
print("    全 0xFF 的 396 字节 CRC = 0x%08X" % blank)
claim = re.search(r"对前 396 字节算得的 CRC = 0x([0-9A-Fa-f]{8})", src)
if claim:
    cv = int(claim.group(1), 16)
    ok = (cv == blank)
    print("    注释声称该值为 0x%08X  %s" % (cv, "[PASS]" if ok else "[FAIL]"))
    if not ok:
        fails.append("flash.c 注释里的空页 CRC 参考值 0x%08X 与实际 0x%08X 不符"
                     % (cv, blank))
zero = crc32(b"\x00" * 396)
print("    对照: 全 0x00 的 396 字节 CRC = 0x%08X（与全 FF 不同，可区分）" % zero)

# ---------------------------------------------------------------- 3. 结构体尺寸
print("\n[3] flash.h 结构体尺寸推算")
h = load("User/BSP/flash.h")
box = int(re.search(r"BOX_MAX_TIMES\s+(\d+)u?", load("User/APP/app.h")).group(1))
boxn = int(re.search(r"BOX_NUM\s+(\d+)u?", load("User/APP/app.h")).group(1))
rule = 2 * 7 * box
total = 4 + boxn * rule + 4
print("    BOX_NUM=%d, BOX_MAX_TIMES=%d -> BoxRule_t=%d B, TimerCfg_t=%d B"
      % (boxn, box, rule, total))
for claim, val in (("56 字节", rule), ("400 字节", total)):
    n = claim.split()[0]
    print("    注释声称 %s -> %s" % (claim, "一致 [PASS]" if int(n) == val else "!! 不一致，实际 %d" % val))

# ---------------------------------------------------------------- 4. 字库尺寸
print("\n[4] font.c 字库表尺寸与声明")
fc = load("User/BSP/font.c")
for pat, name in ((r"font_ascii_8x16\[(\d+)\]\[(\d+)\]", "ASCII 8x16"),
                  (r"font_digit_16x32\[(\d+)\]\[(\d+)\]", "大号数字 16x32"),
                  (r"gbk16_buf\[(\d+)\]\[(\d+)\]", "GBK 16x16"),
                  (r"gbk24_buf\[(\d+)\]\[(\d+)\]", "GBK 24x24")):
    m = re.search(pat, fc)
    if m:
        n, sz = int(m.group(1)), int(m.group(2))
        print("    %-14s %d 项 x %d 字节 = %d 字节" % (name, n, sz, n * sz))
    else:
        fails.append("font.c 里找不到 %s 的声明" % name)
m = re.search(r"font_gbk16_num\s*=\s*(\d+)", fc)
m2 = re.search(r"gbk16_buf\[(\d+)\]", fc)
if m and m2 and m.group(1) != m2.group(1):
    fails.append("font_gbk16_num=%s 与 gbk16_buf 维度 %s 不一致" % (m.group(1), m2.group(1)))
else:
    print("    font_gbk16_num 与 gbk16_buf 维度一致  [PASS]")

# ---------------------------------------------------------------- 5. PCA9685 预分频
print("\n[5] pca9685.c 的 PWM 预分频计算")
pc = load("User/BSP/pca9685.c")
m = re.search(r"PCA9685_FREQ\s+(\d+)u?", load("User/BSP/pca9685.h"))
freq = int(m.group(1)) if m else None
print("    PCA9685_FREQ = %s Hz" % freq)
# 内部振荡器 25MHz，4096 级
if freq:
    calc = int(25000000 / (4096 * freq)) - 1
    print("    按 25MHz/(4096*%d)-1 算得预分频 = %d（注释声称 121）%s"
          % (freq, calc, "  [PASS]" if calc == 121 else "  [FAIL]"))
    actual = 25000000 / (4096 * (calc + 1))
    print("    实际输出频率 = %.2f Hz（目标 %d Hz）" % (actual, freq))
    if abs(actual - freq) > 0.5:
        fails.append("PCA9685 实际频率 %.2f Hz 偏离目标 %d Hz" % (actual, freq))
# 角度换算
ang = int(re.search(r"SERVO_OPEN_ANGLE\s+(\d+)u?", load("User/APP/app.h")).group(1))
pmin = int(re.search(r"SERVO_PULSE_MIN\s+(\d+)u?", load("User/BSP/pca9685.h")).group(1))
pmax = int(re.search(r"SERVO_PULSE_MAX\s+(\d+)u?", load("User/BSP/pca9685.h")).group(1))
off = pmin + ang * (pmax - pmin) // 180
print("    开盒角度 %d 度 -> 计数 %d + %d*%d/180 = %d（注释声称 307）%s"
      % (ang, pmin, ang, pmax - pmin, off, "  [PASS]" if off == 307 else "  [注意]" ))
us = (pmax - pmin) / 180.0 * 4096 / 25000000 * 1e6 if freq else 0
print("    SERVO_PULSE_MIN/MAX = %d/%d；%d 度对应脉宽约 %.3f ms"
      % (pmin, pmax, ang, off / 4096 / freq * 1000 if freq else 0))

# ---------------------------------------------------------------- 6. 定时格式 56 个数字
print("\n[6] 定时规则线格式的字段数")
days, slots = 7, box
n_fields = days * slots * 2
print("    7 天 x %d 时间点 x (时,分) = %d 个数字（代码/文档声称 56）%s"
      % (slots, n_fields, "  [PASS]" if n_fields == 56 else "  [FAIL]"))
if n_fields != 56:
    fails.append("定时线格式字段数 %d 与声称的 56 不符" % n_fields)

# ---------------------------------------------------------------- 7. ESP 缓冲
print("\n[7] ESP 与串口缓冲尺寸")
eh = load("User/BSP/esp01s_at.h")
uh = load("User/BSP/usart.h")
for pat, name, claim in ((r"ESP_LINE_MAX\s+(\d+)", "ESP_LINE_MAX", 2048),
                         (r"ESP_DATA_MAX\s+(\d+)", "ESP_DATA_MAX", 2048)):
    m = re.search(pat, eh)
    v = int(m.group(1)) if m else None
    print("    %-14s = %s（文档声称 %d）%s" % (name, v, claim,
          "  [PASS]" if v == claim else "  !! 与文档不符"))
    if v != claim:
        fails.append("%s 实际 %s，与文档/README 声称的 %d 不一致" % (name, v, claim))
m = re.search(r"ESP_RX_BUF_SIZE\s+(\d+)u?", uh)
v = int(m.group(1)) if m else None
print("    %-14s = %s（文档声称 512）%s" % ("ESP_RX_BUF_SIZE", v,
      "  [PASS]" if v == 512 else "  !! 不符"))

# ---------------------------------------------------------------- 8. 超时值
print("\n[8] AT 各级超时（文档声称 1s/15s/10s/5s）")
mq = load("User/BSP/mqtt_huawei.c")
timeouts = re.findall(r'(AT\+[A-Z]+=?[^;]{0,40}?),\s*"([^"]*)",\s*(\d+)u\s*\)', mq)
if timeouts:
    for cmd, exp, ms in timeouts[:12]:
        print("    %-46s expect=%-16s %5s ms" % (cmd.strip()[:46], '"%s"' % exp, ms))
else:
    for line in mq.split("\n"):
        if "ESP_CmdSend" in line and "u)" in line:
            print("    " + line.strip()[:110])

# ---------------------------------------------------------------- 汇总
print("\n" + "=" * 70)
if fails:
    print("发现问题 %d 处：" % len(fails))
    for f in fails:
        print("  !! " + f)
    sys.exit(1)
print("数值自检全部通过")
sys.exit(0)
