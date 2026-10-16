# eink-todo

给孩子用的墨水屏 todolist：墨水屏 + ESP32 开发板，桌面摆件形态。个人项目。

## 目录
- `docs/` — 需求（`需求.md`）、硬件选型与接线（`hardware.md`）、决策记录
- `firmware/` — 设备固件（计划用 PlatformIO；硬件定了再 `pio project init`）
- `hardware/` — 外壳 3D 模型、接线图、实物照片
- `tools/` — 辅助脚本（中文字库取模、图片转墨水屏位图等）

## 约定
- 硬件型号以 `docs/hardware.md` 为准，写代码前先看；没定的别假设。
- 需求里标「待定」的先问琼哥，不要自己拍板。
- WiFi 密码等写在 `firmware/include/secrets.h`（已 gitignore），仓里只放 `secrets.example.h`。
