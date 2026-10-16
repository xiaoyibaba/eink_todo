#!/bin/sh
# 构建 Wokwi 仿真需要的东西：合并固件镜像（引导 + 分区表 + 程序 + LittleFS）和墨水屏仿真芯片
set -e
cd "$(dirname "$0")/.."
PIO="$HOME/.platformio/penv/bin/pio"
PY="$HOME/.platformio/penv/bin/python"
"$PIO" run -e wokwi
"$PIO" run -e wokwi -t buildfs
B=.pio/build/wokwi
BOOT_APP0=$(find "$HOME/.platformio/packages/framework-arduinoespressif32/tools/partitions" -name boot_app0.bin | head -1)
"$PY" "$HOME/.platformio/packages/tool-esptoolpy/esptool.py" --chip esp32s3 merge_bin -o $B/merged.bin \
  --flash_mode keep --flash_size 8MB \
  0x0 $B/bootloader.bin 0x8000 $B/partitions.bin 0xe000 "$BOOT_APP0" 0x10000 $B/firmware.bin 0x310000 $B/littlefs.bin
"$HOME/.wokwi/bin/wokwi-cli" chip compile wokwi/chip/uc8179.chip.c -o wokwi/chip/uc8179.chip.wasm -q
echo "OK: $B/merged.bin"
