# firmware

同一份业务代码（`lib/core/`）跑在三个地方：电脑模拟器、单元测试、ESP32 真机 / Wokwi 仿真。

```
lib/core/        业务逻辑、界面绘制、网页 API、刷新管理（不依赖硬件）
lib/qrcodegen/   二维码（nayuki，MIT）
src/sim/         电脑模拟器：SDL 窗口 + HTTP 服务
src/esp32/       真机：GxEPD2 驱动、LittleFS、WiFi、深睡
data/            打进 LittleFS 的素材：字库、图标、家长网页（字库和图标由 tools/build_assets.py 生成）
test/            单元测试（test_core）和模拟器脚本（sim/full_week.txt）
wokwi/           Wokwi 仿真：接线图、UC8179 屏幕仿真芯片、自动化场景
```

以下命令都在 `firmware/` 下执行，`pio` = `~/.platformio/penv/bin/pio`。

## 电脑模拟器（主力）

完整步骤（第一次准备、操作说明、手机访问、常见问题）见 [docs/模拟器使用.md](../docs/模拟器使用.md)。

```sh
pio run -e sim
.pio/build/sim/program            # 打开窗口；加 --reset 清空状态从头开始
```

- ↑ ↓ 选择，回车 / 空格 = 确认，按住 3 秒 = 长按进家长设置
- T 时间 +1 小时，N 跳到换日前 10 秒，B 电量 -10%，W 切换有没有 WiFi，P 断电重启
- 家长网页：进入编辑模式后，手机连同一个 WiFi 扫屏幕上的二维码；或电脑打开 http://localhost:8090
- 屏幕按 GxEPD2 源码里的耗时模拟刷新（全刷 1.2s，局刷 0.45s），刚局刷过的区域会短暂画红框
- 状态存在 `.sim/state/`，相当于设备的 flash

自动化脚本（无窗口、虚拟时间，几秒跑完一周）：

```sh
.pio/build/sim/program --script test/sim/full_week.txt --out .sim/shots --start "2026-10-16 17:30"
```

每一步截图存到 `.sim/shots/`；任何 `expect` 失败或控制器报错都会让程序返回非 0。

## 单元测试

```sh
pio test -e test
```

## ESP32 真机

```sh
pio run -e xiao -t upload        # 固件
pio run -e xiao -t uploadfs      # 字库、图标、网页（改了 data/ 才需要）
pio device monitor
```

必须是带 8MB PSRAM 的 XIAO ESP32S3：整套字库加载进 PSRAM，没有 PSRAM 内存不够。

## Wokwi 仿真（真固件 + GxEPD2 真驱动）

```sh
wokwi/build.sh                   # 生成合并镜像和屏幕仿真芯片
```

- VS Code：打开 `wokwi/diagram.json`，F1 →「Wokwi: Start Simulator」（第一次要按提示申请免费个人 license）。
  键盘 ↑ ↓ 空格对应三个按键；电位器模拟电池电压；进入编辑模式后电脑浏览器开 http://localhost:8180
- 命令行（需要 https://wokwi.com/dashboard/ci 的 token）：
  `WOKWI_CLI_TOKEN=... ~/.wokwi/bin/wokwi-cli wokwi --scenario wokwi/scenario.yaml --timeout 180000`，截图在 `wokwi/shots/`

## 改了界面文字之后

48px 大字库只含界面里用到的字（自动从 `lib/core/src` 的字符串里收集），改了文案要重新生成：

```sh
python3 ../tools/build_assets.py
```
