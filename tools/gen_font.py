# -*- coding: utf-8 -*-
"""
生成 LCD 显示用字库: font.h(声明) 与 font.c(定义)

产出内容:
  - 中文 GBK 16x16 点阵（界面正文用）
  - 中文 GBK 24x24 点阵（标题/大字用，只收录 TITLE_STRINGS 里的字以压缩体积）
  - ASCII 8x16 点阵（95 个可打印字符）
  - 大号时钟数字 16x32 点阵（0-9 与冒号）

两个关键设计:
  1) **字库按需生成**：只把界面真正用到的汉字打进字库。所以**改动界面文案后必须重跑本脚本**，
     否则屏上会出现空白/乱码（找不到对应字形）。
  2) **输出为 GBK 编码**：生成的 .c/.h 里的中文注释按 GBK 写入，与工程其它文件保持一致；
     Keil(armcc V5, C99 关闭) 在中文 Windows 下按 936 代码页读源码，GBK 才能正确显示中文。
     （点阵数据本身是 ASCII 十六进制，不受编码影响。）

用法: python tools/gen_font.py
"""
import io, os
from PIL import Image, ImageDraw, ImageFont

HERE    = os.path.dirname(os.path.abspath(__file__))
OUT_H   = os.path.normpath(os.path.join(HERE, "..", "User", "BSP", "font.h"))
OUT_C   = os.path.normpath(os.path.join(HERE, "..", "User", "BSP", "font.c"))
CJK_F   = r"C:\Windows\Fonts\simhei.ttf"    # 中文用黑体
ASCII_F = r"C:\Windows\Fonts\consola.ttf"   # 英文/数字用 Consolas

# 生成文件的输出编码。必须与工程其它源文件一致，否则 Keil 里中文注释会乱码。
OUT_ENC = "gbk"

NL = "\n"

# ==================== 所有界面用到的中文字符串 ====================
# ⚠️ 改界面上任何中文文案后，必须把新文案补进这里并重跑本脚本，
#    否则字库里没有对应字形，屏上那个字会**变成空白**（不报错、不崩溃，
#    只是字不见了 —— 极难排查）。可以用 tools/check_font_coverage.py 自动查漏。
UI_STRINGS = [
    u"智能药箱系统",          # 主界面标题
    u"WiFi:已连接", u"WiFi:未连接", u"WiFi:连接中", u"云:已连接", u"云:未连接",
    u"年", u"月", u"日", u"星期", u"一二三四五六日",
    u"蜂鸣中 剩余", u"秒", u"系统运行中",
    u"药盒", u"待打开", u"已打开",
    u"按[设置]进入设置", u"按时吃药自动停止",
    u"保存成功", u"保存无效",
    u"药盒选择", u"选择星期", u"时间设置", u"时",
    u"确认", u"返回", u"加减选择 确认进入 返回返回", u"加减调整 确认保存 返回返回",
    u"未设置",
    # ↓ 下面两条是"调小时/调分钟"两个阶段的底部提示（menu.c 的 STR_HINT_HOUR / STR_HINT_MIN）。
    #   它们是在字库生成之后才加进 menu.c 的，当时漏了同步本清单，
    #   导致「小」「分」「钟」三个字没有字形，在屏上显示为空白。
    u"加减=调小时 确认=进入分钟 返回=返回",
    u"加减=调分钟 确认=保存 返回=返回",
    # ↓ 主界面状态行（app.c 经 Menu_ShowMainMsg 画到屏上，属 16 号字）
    u"定时触发:药盒", u"远程开盒:药盒", u"远程设置成功",
    # ↓ 开机画面提示（main.c 的 STR_INITING，用 16 号字画在标题下面）。
    #   注意：main.c 的开机大标题 STR_BOOT_TITLE("智能药箱系统") 是 24 号字，
    #   只在 TITLE_STRINGS 里收录；而同一屏下方那行"正在初始化"走的是 16 号字，
    #   必须在 UI_STRINGS 里 —— 这两处用的是**不同的字库表**，容易只记得一处。
    u"正在初始化",
]

# 24x24 大字仅用于标题/大字提示, 只需以下字符串用到的汉字(压缩字库体积)
TITLE_STRINGS = [
    u"智能药箱系统", u"药盒选择", u"选择星期", u"时间设置", u"时",
]

def collect_chars(strings):
    chars = set()
    for s in strings:
        for ch in s:
            if u'\u4e00' <= ch <= u'\u9fff':
                chars.add(ch)
    return sorted(chars)

# ==================== 位图渲染 ====================
def render_bbox_bits(font, text, cell_w, cell_h, scale):
    """在 cell_w x cell_h 单元内渲染字符, 返回行位(每行一个整数, 高字节在前)."""
    S = scale
    W, H = cell_w * S, cell_h * S
    img = Image.new('L', (W, H), 0)
    d = ImageDraw.Draw(img)
    f = ImageFont.truetype(font, S * (cell_h - 2))
    d.text((0, 0), text, font=f, fill=255)
    bbox = img.getbbox()
    if not bbox:
        return [0] * cell_h
    x0, y0, x1, y1 = bbox
    bw, bh = x1 - x0, y1 - y0
    crop = img.crop((x0, y0, x1, y1))
    hh = cell_h - 2
    nw = max(1, int(round(bw * hh / bh)))
    nw = min(nw, cell_w)
    small = crop.resize((nw, hh), Image.BOX)
    canvas = Image.new('L', (cell_w, cell_h), 0)
    canvas.paste(small, ((cell_w - nw) // 2, 1))
    pix = canvas.load()
    rows = []
    for y in range(cell_h):
        v = 0
        for x in range(cell_w):
            v = (v << 1) | (1 if pix[x, y] >= 128 else 0)
        rows.append(v)
    return rows

def render_cjk_bits(font_path, ch, size):
    """中文字符: 整字居中, 满格 size x size."""
    scale = 4
    S = size * scale
    img = Image.new('L', (S, S), 0)
    d = ImageDraw.Draw(img)
    f = ImageFont.truetype(font_path, S)
    d.text((0, 0), ch, font=f, fill=255)
    bbox = img.getbbox()
    if not bbox:
        return [0] * size
    x0, y0, x1, y1 = bbox
    w, h = x1 - x0, y1 - y0
    side = max(w, h) + int(max(w, h) * 0.06)
    cx, cy = (x0 + x1) // 2, (y0 + y1) // 2
    ax0, ay0 = cx - side // 2, cy - side // 2
    ax1, ay1 = ax0 + side, ay0 + side
    if ax0 < 0: ax0, ax1 = 0, side
    if ay0 < 0: ay0, ay1 = 0, side
    if ax1 > S: ax0, ax1 = S - side, S
    if ay1 > S: ay0, ay1 = S - side, S
    crop = img.crop((ax0, ay0, ax1, ay1))
    small = crop.resize((size, size), Image.BOX)
    pix = small.load()
    rows = []
    for y in range(size):
        v = 0
        for x in range(size):
            v = (v << 1) | (1 if pix[x, y] >= 128 else 0)
        rows.append(v)
    return rows

def rows_to_bytes(rows, cell_w):
    out = []
    nbytes = (cell_w + 7) // 8
    for r in rows:
        for b in range(nbytes - 1, -1, -1):
            out.append((r >> (8 * b)) & 0xFF)
    return out

def c_byte_list(data, per_line=16):
    return (",\n    ").join(
        ", ".join("0x%02X" % b for b in data[i:i+per_line])
        for i in range(0, len(data), per_line))

# ==================== 生成 ====================
chars      = collect_chars(UI_STRINGS)     # 16x16 用字
title_chars = collect_chars(TITLE_STRINGS) # 24x24 仅标题用字

# ---- 位图数据 ----
ascii_data = []
for c in range(0x20, 0x7F):
    ascii_data.append(rows_to_bytes(render_bbox_bits(ASCII_F, chr(c), 8, 16, 2), 8))

# 大号时钟数字: 仅 0-9 与 ':' (索引 = ch-'0', ':' = 10)
digit_data = [None] * 11
for c in range(0x30, 0x3A):
    digit_data[c - 0x30] = rows_to_bytes(render_bbox_bits(ASCII_F, chr(c), 16, 32, 2), 16)
digit_data[10] = rows_to_bytes(render_bbox_bits(ASCII_F, ':', 16, 32, 2), 16)

gbk16, gbk24 = {}, {}
for ch in chars:
    gb = ch.encode('gbk')
    gbk16[gb] = rows_to_bytes(render_cjk_bits(CJK_F, ch, 16), 16)
for ch in title_chars:
    gb = ch.encode('gbk')
    gbk24[gb] = rows_to_bytes(render_cjk_bits(CJK_F, ch, 24), 24)

# ---- font.h (仅声明) ----
hl = []
hl.append("/**")
hl.append("  ******************************************************************************")
hl.append("  * @file    font.h")
hl.append("  * @brief   LCD 点阵字库声明（自动生成，请勿手工修改）")
hl.append("  ******************************************************************************")
hl.append("  * 本文件由 tools/gen_font.py 生成。")
hl.append("  * 要改字库（增删汉字、换字体、改字号），请改脚本后重新运行，不要直接改本文件。")
hl.append("  *")
hl.append("  * 点阵排布约定（4 张表统一遵守）:")
hl.append("  *   - 按行取模，每行占 (宽 + 7) / 8 个字节，高位在左（MSB 在屏幕左侧）；")
hl.append("  *   - 某位为 1 表示该像素点亮（前景色），为 0 表示背景色；")
hl.append("  *   - 字节数换算: 8x16 = 16 字节, 16x16 = 32 字节, 16x32 = 64 字节, 24x24 = 72 字节。")
hl.append("  *")
hl.append("  * 为什么中文表要单独建索引:")
hl.append("  *   GBK 汉字是双字节，无法像 ASCII 那样直接用字符码当下标；")
hl.append("  *   故用 { GBK 双字节码, 点阵指针 } 的索引表，并**按 GBK 码升序排列**，")
hl.append("  *   这样查字时可以做**二分查找**（见 lcd.c 的 FontGbk_Find16/FontGbk_Find24）。")
hl.append("  ******************************************************************************")
hl.append("  */")
hl.append("#ifndef __FONT_H__")
hl.append("#define __FONT_H__")
hl.append("#include \"stm32f10x.h\"")
hl.append("")
hl.append("/* ASCII 8x16 点阵表: 每字符 16 字节, 下标 = 字符码 - 0x20 (覆盖 0x20~0x7E 共 95 个) */")
hl.append("extern const uint8_t font_ascii_8x16[95][16];")
hl.append("")
hl.append("/* 大号时钟数字 16x32: 每字符 64 字节, 下标 = 字符 - 0x30; 冒号 ':' 固定放在下标 10 */")
hl.append("extern const uint8_t font_digit_16x32[11][64];")
hl.append("")
hl.append("/* ==================== 中文 GBK 16x16（界面正文用） ==================== */")
hl.append("/* 索引项: gbk = 该汉字的 GBK 双字节码（高字节在前）, data = 指向 32 字节点阵 */")
hl.append("typedef struct { const uint8_t gbk[2]; const uint8_t *data; } FontGbk16_t;")
hl.append("extern const FontGbk16_t font_gbk16[];   /* 按 gbk 升序，供二分查找 */")
hl.append("extern const uint16_t font_gbk16_num;     /* 表项个数（即收录的汉字数） */")
hl.append("")
hl.append("/* ==================== 中文 GBK 24x24（标题/大字用） ==================== */")
hl.append("/* 结构同上；只收录 TITLE_STRINGS 用到的字，以压缩 Flash 占用 */")
hl.append("typedef struct { const uint8_t gbk[2]; const uint8_t *data; } FontGbk24_t;")
hl.append("extern const FontGbk24_t font_gbk24[];")
hl.append("extern const uint16_t font_gbk24_num;")
hl.append("")
hl.append("#endif /* __FONT_H__ */")
hl.append("")

# ---- font.c (定义) ----
cl = []
cl.append("/**")
cl.append("  ******************************************************************************")
cl.append("  * @file    font.c")
cl.append("  * @brief   LCD 点阵字库数据（自动生成，请勿手工修改）")
cl.append("  ******************************************************************************")
cl.append("  * 本文件由 tools/gen_font.py 生成，请勿手工编辑——下次生成会覆盖。")
cl.append("  * 修改方式见 font.h 与 tools/gen_font.py 顶部说明。")
cl.append("  * 每行末尾的注释给出该字形的 GBK 码与对应汉字，方便对照排查显示问题。")
cl.append("  ******************************************************************************")
cl.append("  */")
cl.append("#include \"font.h\"")
cl.append("")

cl.append("/* ==================== ASCII 8x16（下标 = 字符码 - 0x20） ==================== */")
cl.append("/* 每行 1 字节，共 16 行/字符；注释标出该字符的 ASCII 码与字符本身 */")
cl.append("const uint8_t font_ascii_8x16[95][16] = {")
for i, b in enumerate(ascii_data):
    cl.append("  { %s },%s" % (c_byte_list(b, 16), " /* 0x%02X '%s' */" % (0x20 + i, chr(0x20 + i)) if i % 8 == 0 else ""))
cl.append("};")
cl.append("")

cl.append("/* ==================== 大号时钟数字 16x32（下标 = 字符 - 0x30，':' 在 10） ==================== */")
cl.append("/* 每行 2 字节（16 位宽），共 32 行/字符 = 64 字节 */")
cl.append("const uint8_t font_digit_16x32[11][64] = {")
for c in range(0x30, 0x3A):
    b = digit_data[c - 0x30]
    cl.append("  { %s }, /* 数字 '%c' */" % (c_byte_list(b, 8), chr(c)))
cl.append("  { %s }, /* 冒号 ':' */" % c_byte_list(digit_data[10], 8))
cl.append("};")
cl.append("")

gbk_items = sorted(gbk16.items())
cl.append("/* ==================== 中文 GBK 16x16 点阵数据（界面正文用） ==================== */")
cl.append("/* 每行 2 字节，共 16 行 = 32 字节/字；顺序与下面的索引表一一对应 */")
cl.append("static const uint8_t gbk16_buf[%d][32] = {" % len(gbk_items))
for gb, data in gbk_items:
    try:
        ch = bytes(gb).decode("gbk")
    except UnicodeDecodeError:
        ch = "?"
    cl.append("  { %s }, /* 0x%02X 0x%02X = %s */" % (c_byte_list(data, 8), gb[0], gb[1], ch))
cl.append("};")
cl.append("/* 索引表: 按 GBK 码升序排列，lcd.c 用二分查找快速定位字形 */")
cl.append("const FontGbk16_t font_gbk16[] = {")
for idx, (gb, data) in enumerate(gbk_items):
    try:
        ch = bytes(gb).decode("gbk")
    except UnicodeDecodeError:
        ch = "?"
    cl.append("  { {0x%02X, 0x%02X}, gbk16_buf[%d] }, /* %s */" % (gb[0], gb[1], idx, ch))
cl.append("};")
cl.append("/* 收录的汉字个数（查表时作为二分查找的上界） */")
cl.append("const uint16_t font_gbk16_num = %d;" % len(gbk_items))
cl.append("")

gbk24_items = sorted(gbk24.items())
cl.append("/* ==================== 中文 GBK 24x24 点阵数据（标题/大字用） ==================== */")
cl.append("/* 每行 3 字节（24 位宽），共 24 行 = 72 字节/字；只收录 TITLE_STRINGS 用到的字 */")
cl.append("static const uint8_t gbk24_buf[%d][72] = {" % len(gbk24_items))
for gb, data in gbk24_items:
    try:
        ch = bytes(gb).decode("gbk")
    except UnicodeDecodeError:
        ch = "?"
    cl.append("  { %s }, /* 0x%02X 0x%02X = %s */" % (c_byte_list(data, 8), gb[0], gb[1], ch))
cl.append("};")
cl.append("/* 索引表: 同样按 GBK 码升序，供二分查找 */")
cl.append("const FontGbk24_t font_gbk24[] = {")
for idx, (gb, data) in enumerate(gbk24_items):
    try:
        ch = bytes(gb).decode("gbk")
    except UnicodeDecodeError:
        ch = "?"
    cl.append("  { {0x%02X, 0x%02X}, gbk24_buf[%d] }, /* %s */" % (gb[0], gb[1], idx, ch))
cl.append("};")
cl.append("const uint16_t font_gbk24_num = %d;" % len(gbk24_items))
cl.append("")

os.makedirs(os.path.dirname(OUT_H), exist_ok=True)
# 以 GBK 写出（与工程其它源文件一致）；点阵数据本身是 ASCII，不受编码影响
with io.open(OUT_H, "w", encoding=OUT_ENC, newline=NL) as f:
    f.write(NL.join(hl))
with io.open(OUT_C, "w", encoding=OUT_ENC, newline=NL) as f:
    f.write(NL.join(cl))

print("字库生成完成: 16x16 汉字 %d 个, 24x24 汉字 %d 个, ASCII 95 个, 大号数字 11 个"
      % (len(chars), len(gbk24)))
print("输出编码 %s -> %s + %s" % (OUT_ENC, OUT_H, OUT_C))
