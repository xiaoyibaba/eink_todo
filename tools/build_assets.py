#!/usr/bin/env python3
"""生成固件用的点阵字库和图标（写入 firmware/data/，打进 LittleFS）。

用法：python3 tools/build_assets.py
依赖：Pillow（需带 FreeType）。字体源文件首次运行自动下载到 tools/.cache/。

输出：
  firmware/data/fonts/sans24.bin  GB2312 全集 + ASCII，24px
  firmware/data/fonts/sans32.bin  GB2312 全集 + ASCII，32px
  firmware/data/fonts/sans48.bin  只含界面固定文案用到的字 + ASCII，48px
  firmware/data/icons.bin         任务图标 40px + 星星等界面图标
  firmware/data/web/icons/*.png   给家长网页用的同一套图标
  firmware/lib/core/src/IconList.inc  图标名与中文名（固件和网页共用一份）

字库格式（小端）：
  header 16B: 'EFNT' | u8 ver=1 | u8 size | u8 emAscent | u8 lineH | u32 count | u32 reserved
  index  count*16B（按码点升序）: u32 cp | u32 off | u8 w | u8 h | i8 xoff | i8 yoff | u8 adv | 3B pad
  glyph  每行 ceil(w/8) 字节，bit7 在最左，1 = 黑
  yoff 为字形顶部相对基线的偏移（向上为负）；emAscent 为方块字顶部到基线的距离。
"""
import hashlib
import os
import re
import struct
import sys
import urllib.request
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont, features

ROOT = Path(__file__).resolve().parent.parent
CACHE = ROOT / "tools" / ".cache"
DATA = ROOT / "firmware" / "data"
CORE_SRC = ROOT / "firmware" / "lib" / "core" / "src"

FONT_URL = "https://github.com/google/fonts/raw/main/ofl/notosanssc/NotoSansSC%5Bwght%5D.ttf"
ICON_VER = "3.19.0"
ICON_TTF_URL = f"https://cdn.jsdelivr.net/npm/@tabler/icons-webfont@{ICON_VER}/dist/fonts/tabler-icons.ttf"

# (固件里的名字, 网页上显示的中文名, tabler 图标码点)
TASK_ICONS = [
    ("kousuan", "口算", 0xEB80),     # calculator
    ("shuxue", "数学", 0xF554),      # 123
    ("yuwen", "语文", 0xEBBE),       # language
    ("yingyu", "英语", 0xF567),      # abc
    ("yuedu", "阅读", 0xEA39),       # book
    ("tingdu", "听读", 0xEABD),      # headphones
    ("beisong", "背诵", 0xEAF0),     # microphone
    ("tingxie", "听写", 0xEBCE),     # ear
    ("lianzi", "练字", 0xEB04),      # pencil
    ("xiezuo", "写作", 0xEF08),      # writing
    ("yuxi", "预习", 0xEA51),        # bulb
    ("dingzheng", "订正", 0xFBFF),   # pencil-check
    ("jiancha", "检查", 0xF074),     # checklist
    ("lianqin", "练琴", 0xFAD3),     # piano
    ("huahua", "画画", 0xEB01),      # palette
    ("tiaosheng", "跳绳", 0xED8F),   # jump-rope
    ("yundong", "运动", 0xEC82),     # run
    ("shubao", "书包", 0xEF47),      # backpack
    ("kexue", "科学", 0xEF64),       # microscope
    ("shougong", "手工", 0xEB1B),    # scissors
    ("star", "通用", 0xEB2E),        # star
]
TASK_ICON_PX = 40

# GB2312 里没有、但界面要用的符号
EXTRA_CHARS = "·▼"

# 界面用的固定图标：(名字, 码点, 像素)
UI_ICONS = [
    ("star_fill_44", 0xF6A6, 44),
    ("star_fill_26", 0xF6A6, 26),
    ("star_line_26", 0xEB2E, 26),
    ("star_fill_150", 0xF6A6, 150),
]


def fetch(url: str, dest: Path) -> Path:
    if not dest.exists():
        dest.parent.mkdir(parents=True, exist_ok=True)
        print(f"下载 {url}")
        tmp = dest.with_suffix(".part")
        urllib.request.urlretrieve(url, tmp)
        tmp.rename(dest)
    return dest


def gb2312_chars() -> list[str]:
    out = []
    for hi in range(0xA1, 0xF8):
        for lo in range(0xA1, 0xFF):
            try:
                out.append(bytes([hi, lo]).decode("gb2312"))
            except UnicodeDecodeError:
                pass
    return out


def ui_literal_chars() -> set[str]:
    """扫描 core 源码里字符串字面量中的非 ASCII 字符，作为 48px 子集。"""
    chars = set()
    for p in CORE_SRC.rglob("*"):
        if p.suffix not in (".h", ".cpp", ".inc"):
            continue
        for lit in re.findall(r'"((?:[^"\\\n]|\\.)*)"', p.read_text(encoding="utf-8")):
            chars.update(c for c in lit if ord(c) > 0x7F)
    return chars


def pack_rows(img: Image.Image) -> bytes:
    w, h = img.size
    px = img.load()
    out = bytearray()
    for y in range(h):
        byte, bit = 0, 7
        for x in range(w):
            if px[x, y]:
                byte |= 1 << bit
            bit -= 1
            if bit < 0:
                out.append(byte)
                byte, bit = 0, 7
        if bit != 7:
            out.append(byte)
    return bytes(out)


def render_glyph(font: ImageFont.FreeTypeFont, ch: str):
    l, t, r, b = font.getbbox(ch, anchor="ls")
    adv = round(font.getlength(ch))
    w, h = max(r - l, 0), max(b - t, 0)
    if w == 0 or h == 0:
        return dict(w=0, h=0, xoff=0, yoff=0, adv=adv, data=b"")
    img = Image.new("L", (w, h), 0)
    ImageDraw.Draw(img).text((-l, -t), ch, font=font, fill=255, anchor="ls")
    bw = img.point(lambda v: 1 if v >= 110 else 0, mode="1")
    return dict(w=w, h=h, xoff=l, yoff=t, adv=adv, data=pack_rows(bw))


def build_font(ttf: Path, size: int, chars: list[str], dest: Path, weight: str = "Bold"):
    font = ImageFont.truetype(str(ttf), size)
    font.set_variation_by_name(weight)
    cmap_ok = []
    for ch in sorted(set(chars), key=ord):
        # 跳过字体里没有的字（会渲染成 .notdef 方块）
        if ch != " " and font.getmask(ch).getbbox() is None:
            continue
        cmap_ok.append(ch)
    glyphs = [(ord(c), render_glyph(font, c)) for c in cmap_ok]
    em_ascent = round(size * 0.88)
    line_h = round(size * 1.25)
    header = b"EFNT" + struct.pack("<BBBBII", 1, size, em_ascent, line_h, len(glyphs), 0)
    index, blob = bytearray(), bytearray()
    base = len(header) + 16 * len(glyphs)
    for cp, g in glyphs:
        assert g["w"] < 256 and g["h"] < 256 and -128 <= g["xoff"] < 128 and -128 <= g["yoff"] < 128
        index += struct.pack("<IIBBbbB3x", cp, base + len(blob), g["w"], g["h"], g["xoff"], g["yoff"], g["adv"])
        blob += g["data"]
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(header + index + blob)
    print(f"{dest.relative_to(ROOT)}: {len(glyphs)} 字, {dest.stat().st_size / 1024:.0f} KB")


def render_icon(font_path: Path, cp: int, px: int) -> Image.Image:
    # 先 4 倍渲染、裁掉 em 框留白，再缩到目标尺寸，线条才能撑满图标
    ch = chr(cp)
    big = Image.new("L", (px * 4, px * 4), 0)
    font4 = ImageFont.truetype(str(font_path), px * 4)
    ImageDraw.Draw(big).text((0, 0), ch, font=font4, fill=255)
    bbox = big.getbbox()
    glyph = big.crop(bbox)
    gw, gh = glyph.size
    scale = px / max(gw, gh)
    glyph = glyph.resize((max(1, round(gw * scale)), max(1, round(gh * scale))), Image.LANCZOS)
    canvas = Image.new("L", (px, px), 0)
    canvas.paste(glyph, ((px - glyph.width) // 2, (px - glyph.height) // 2))
    return canvas.point(lambda v: 1 if v >= 100 else 0, mode="1")


def build_icons(ttf: Path):
    entries = []  # (name, img)
    web_dir = DATA / "web" / "icons"
    web_dir.mkdir(parents=True, exist_ok=True)
    for name, _label, cp in TASK_ICONS:
        img = render_icon(ttf, cp, TASK_ICON_PX)
        entries.append((name, img))
        # 网页用：黑线、透明底、放大 2 倍，保持和屏幕上一样的像素感
        alpha = img.convert("L").point(lambda v: 255 if v else 0)
        web = Image.merge("LA", (Image.new("L", img.size, 0), alpha))
        web.resize((img.width * 2, img.height * 2), Image.NEAREST).save(web_dir / f"{name}.png")
    for name, cp, px in UI_ICONS:
        entries.append((name, render_icon(ttf, cp, px)))

    header = b"EICN" + struct.pack("<HH", len(entries), 0)
    table, blob = bytearray(), bytearray()
    base = len(header) + 28 * len(entries)
    for name, img in entries:
        nb = name.encode()
        assert len(nb) < 20
        table += nb.ljust(20, b"\0") + struct.pack("<HHI", img.width, img.height, base + len(blob))
        blob += pack_rows(img)
    out = DATA / "icons.bin"
    out.write_bytes(header + table + blob)
    print(f"{out.relative_to(ROOT)}: {len(entries)} 个图标, {out.stat().st_size / 1024:.1f} KB")

    inc = CORE_SRC / "IconList.inc"
    lines = ["// 由 tools/build_assets.py 生成，别手改。X(名字, 中文名)"]
    lines += [f'X("{n}", "{lab}")' for n, lab, _ in TASK_ICONS]
    inc.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main():
    if not features.check("freetype2"):
        sys.exit("Pillow 没有 FreeType 支持")
    font = fetch(FONT_URL, CACHE / "NotoSansSC-wght.ttf")
    icons = fetch(ICON_TTF_URL, CACHE / f"tabler-icons-{ICON_VER}.ttf")
    print("NotoSansSC sha256:", hashlib.sha256(font.read_bytes()).hexdigest()[:16])

    build_icons(icons)  # 先生成 IconList.inc，它的中文也要进 48px 子集

    ascii_chars = [chr(c) for c in range(0x20, 0x7F)]
    full = ascii_chars + gb2312_chars() + list(EXTRA_CHARS)
    build_font(font, 24, full, DATA / "fonts" / "sans24.bin")
    build_font(font, 32, full, DATA / "fonts" / "sans32.bin")
    build_font(font, 48, ascii_chars + list(EXTRA_CHARS) + sorted(ui_literal_chars()), DATA / "fonts" / "sans48.bin")


if __name__ == "__main__":
    main()
