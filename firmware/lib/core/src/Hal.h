// 硬件抽象层：core 里的代码只依赖这里的接口。
// 实现有两份：src/esp32/（真机和 Wokwi）、src/sim/（电脑上的 SDL 模拟器）。
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>

namespace et {

constexpr int W = 800, H = 480;
constexpr size_t FB_BYTES = W * H / 8;

struct Rect {
  int x = 0, y = 0, w = 0, h = 0;
  bool empty() const { return w <= 0 || h <= 0; }
};

class File {
 public:
  virtual ~File() = default;
  virtual size_t size() = 0;
  virtual size_t readAt(uint32_t off, void* buf, size_t n) = 0;
};

class Fs {
 public:
  virtual ~Fs() = default;
  virtual std::unique_ptr<File> open(const std::string& path) = 0;  // 不存在返回 nullptr
  virtual bool readAll(const std::string& path, std::string& out) = 0;
  // 先写临时文件再改名，掉电时不会留下写了一半的文件
  virtual bool writeAtomic(const std::string& path, const std::string& data) = 0;
  virtual bool remove(const std::string& path) = 0;
};

// 只暴露 GxEPD2 里我们用到的几个调用。模拟器按 UC8179 的 RAM 语义模拟：
// 控制器有 old/new 两块 RAM，局刷只驱动 old != new 的像素，深睡后两块 RAM 都丢。
// 帧缓冲约定同 GxEPD2：1 bit/像素，bit7 在左，1 = 白。
class Epd {
 public:
  virtual ~Epd() = default;
  virtual void begin(bool coldBoot) = 0;  // coldBoot：上电冷启动，屏上内容未知
  virtual void writePrevious(const uint8_t* fb) = 0;  // 整帧写入 old RAM
  virtual void writeNew(const uint8_t* fb) = 0;       // 整帧写入 new RAM
  virtual void refreshFull() = 0;
  virtual void refreshPartial(Rect r) = 0;
  virtual void hibernate() = 0;
};

enum class Key : uint8_t { None, Up, Down, Ok };
enum class WakeReason : uint8_t { PowerOn, Key, Timer };

struct HttpRequest {
  std::string method, path, body;
  std::map<std::string, std::string> query;
};
struct HttpResponse {
  int status = 200;
  std::string type = "application/json";
  std::string body;
};
using HttpHandler = std::function<HttpResponse(const HttpRequest&)>;

// 深睡期间保留的少量数据（ESP32 上放 RTC 内存）
struct RtcData {
  uint32_t magic = 0;
  uint16_t partialCount = 0;
  int8_t battery = -1;
};
constexpr uint32_t RTC_MAGIC = 0x45544F31;  // "ETO1"

enum class NetMode : uint8_t { Off, Station, SetupAp };

class Platform {
 public:
  virtual ~Platform() = default;
  virtual uint32_t millis() = 0;
  virtual int64_t nowUtc() = 0;  // 未校时返回 0
  virtual bool syncTime() = 0;   // 联网校时，有超时，会阻塞几秒
  virtual bool keyDown(Key k) = 0;
  virtual int batteryPercent() = 0;  // 未知返回 -1
  // 联网供家长编辑：连上家里 WiFi 返回 Station；没配置或连不上则开热点返回 SetupAp
  virtual NetMode startNetwork(std::string& url) = 0;
  virtual void stopNetwork() = 0;
  virtual void pollNetwork() = 0;
  virtual void setHttpHandler(HttpHandler h) = 0;
  virtual bool saveWifi(const std::string& ssid, const std::string& pass) = 0;
  // 深睡到 wakeAtUtc 或按键。ESP32 上不会返回；模拟器上返回，由外层重建 App 模拟掉电。
  virtual void deepSleep(int64_t wakeAtUtc) = 0;
  virtual RtcData& rtc() = 0;
  virtual Fs& fs() = 0;
  virtual Epd& epd() = 0;
  virtual void log(const char* msg) = 0;
};

}  // namespace et
