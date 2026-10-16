#!/usr/bin/env python3
"""从 wokwi-cli 的输出里还原墨水屏仿真芯片每次刷新后的画面。

wokwi-cli 截不了自定义芯片的 framebuffer，所以 uc8179 仿真芯片每次刷新后把
「玻璃上的画面」以 RLE 十六进制打到控制台（[uc8179] FRAME n BEGIN / F ... / END）。

用法：python3 tools/wokwi_frames.py <wokwi-cli 输出文件> <输出目录>
"""
import re
import sys
from pathlib import Path

from PIL import Image

W, H = 800, 480


def main():
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    frames, cur, n = [], None, None
    for line in src.read_text(errors="replace").splitlines():
        m = re.search(r"\[uc8179\] FRAME (\d+) BEGIN", line)
        if m:
            n, cur = int(m.group(1)), []
            continue
        if cur is not None and "[uc8179] F " in line:
            cur.append(line.split("[uc8179] F ", 1)[1].strip())
            continue
        if cur is not None and "END" in line and "FRAME" in line:
            frames.append((n, "".join(cur)))
            cur = None
    for i, (n, hexs) in enumerate(frames, 1):
        raw = bytearray()
        data = bytes.fromhex(hexs)
        for k in range(0, len(data), 2):
            raw += bytes([data[k + 1]]) * data[k]
        if len(raw) != W * H // 8:
            print(f"frame {i}: 长度不对 {len(raw)}，跳过")
            continue
        img = Image.frombytes("1", (W, H), bytes(raw))
        path = out / f"wokwi_{i:02d}.png"
        img.save(path)
        print(path)


if __name__ == "__main__":
    main()
