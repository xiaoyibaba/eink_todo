// Wokwi 自定义芯片：7.5" 800x480 黑白墨水屏（UC8179 控制器，对应 GDEY075T7 + DESPI-C02）。
//
// 只实现 GxEPD2_750_GDEY075T7 实际发出的命令，并按控制器语义模拟会导致「画面不对」的部分：
//   0x10/0x13  写 old / new RAM（0x91 窗口模式下只写窗口）
//   0x90       设置窗口；0x91/0x92 进入/退出窗口模式
//   0x12       刷新。差分模式（GxEPD2 用 0xE5=0x6E 强制温度触发 OTP 快速局刷 LUT，或 0x00=0x3F 寄存器 LUT）
//              只驱动 old != new 的像素；否则整屏按 new 驱动。刷新后 new 拷到 old（N2OCP）
//   0x07 0xA5  深睡：两块 RAM 内容丢失（填随机数），硬复位前忽略所有命令
//   BUSY       低电平 = 忙，按 GxEPD2 源码里的耗时模拟
// 显示的是「玻璃上实际的样子」，漏写 old RAM 之类的错误会以残留像素的形式直接看到。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wokwi-api.h"

#define W 800
#define H 480
#define BW (W / 8)
#define FB_BYTES (BW * H)

typedef struct {
  pin_t cs, dc, rst, busy;
  spi_dev_t spi;
  uint8_t buf[1024];
  bool dataPhase;  // 本次 CS 期间 DC 为高（数据）
  uint8_t cmd;
  uint32_t argIdx;
  uint8_t args[16];
  uint8_t old[FB_BYTES], neu[FB_BYTES], phys[FB_BYTES];
  bool asleep, ptl, tsfix;
  uint8_t panel, forcedTemp;
  uint16_t wx0, wx1, wy0, wy1;
  uint32_t wptr;
  buffer_t fb;
  timer_t busyTimer;
  uint32_t fulls, partials, ignored;
  uint32_t seed;
  uint32_t cmdLog, csEdges;
} chip_t;

static void scramble(chip_t* c) {
  for (int i = 0; i < FB_BYTES; ++i) {
    c->seed = c->seed * 1103515245u + 12345u;
    c->old[i] = (uint8_t)(c->seed >> 16);
    c->seed = c->seed * 1103515245u + 12345u;
    c->neu[i] = (uint8_t)(c->seed >> 16);
  }
}

static void paint(chip_t* c) {
  static uint8_t row[W * 4];
  for (int y = 0; y < H; ++y) {
    for (int x = 0; x < W; ++x) {
      bool white = c->phys[y * BW + x / 8] & (0x80 >> (x & 7));
      uint8_t* p = row + x * 4;
      p[0] = white ? 242 : 28;
      p[1] = white ? 240 : 28;
      p[2] = white ? 234 : 28;
      p[3] = 255;
    }
    buffer_write(c->fb, y * W * 4, row, sizeof row);
  }
}

static void busyFor(chip_t* c, uint32_t ms) {
  pin_write(c->busy, LOW);
  timer_start(c->busyTimer, ms * 1000, false);
}

static void busyDone(void* user) {
  chip_t* c = (chip_t*)user;
  pin_write(c->busy, HIGH);
}

// 把玻璃上的画面以 RLE + 十六进制打到控制台（wokwi-cli 截不了自定义芯片的图，用 tools/wokwi_frames.py 还原成 PNG）
static void dumpFrame(chip_t* c) {
  static char line[1100];
  int n = 0, frameNo = c->fulls + c->partials;
  printf("[uc8179] FRAME %d BEGIN\n", frameNo);
  for (int i = 0; i < FB_BYTES;) {
    uint8_t v = c->phys[i];
    int run = 1;
    while (i + run < FB_BYTES && c->phys[i + run] == v && run < 255) run++;
    n += sprintf(line + n, "%02x%02x", run, v);
    i += run;
    if (n > 1000 || i >= FB_BYTES) {
      printf("[uc8179] F %s\n", line);
      n = 0;
    }
  }
  printf("[uc8179] FRAME %d END\n", frameNo);
}

static void refresh(chip_t* c) {
  const bool diff = c->panel == 0x3F || (c->tsfix && c->forcedTemp == 0x6E);
  int x0 = 0, x1 = W - 1, y0 = 0, y1 = H - 1;
  if (c->ptl) { x0 = c->wx0; x1 = c->wx1; y0 = c->wy0; y1 = c->wy1; }
  for (int y = y0; y <= y1 && y < H; ++y)
    for (int xb = x0 / 8; xb <= x1 / 8 && xb < BW; ++xb) {
      int i = y * BW + xb;
      if (diff) {
        uint8_t m = c->old[i] ^ c->neu[i];
        c->phys[i] = (c->phys[i] & ~m) | (c->neu[i] & m);
      } else {
        c->phys[i] = c->neu[i];
      }
    }
  memcpy(c->old, c->neu, FB_BYTES);  // N2OCP
  if (diff) c->partials++; else c->fulls++;
  printf("[uc8179] %s refresh #%u (window %s)\n", diff ? "partial" : "full", diff ? c->partials : c->fulls,
         c->ptl ? "on" : "off");
  paint(c);
  dumpFrame(c);
  busyFor(c, diff ? 450 : 1200);
}

static void onCommand(chip_t* c, uint8_t cmd) {
  if (c->cmdLog < 40) {  // 只记前 40 条，够看初始化序列
    printf("[uc8179] cmd 0x%02X\n", cmd);
    c->cmdLog++;
  }
  c->cmd = cmd;
  c->argIdx = 0;
  switch (cmd) {
    case 0x10:
    case 0x13: c->wptr = 0; break;
    case 0x91: c->ptl = true; break;
    case 0x92: c->ptl = false; break;
    case 0x04: busyFor(c, 140); break;  // power on
    case 0x02: busyFor(c, 42); break;   // power off
    case 0x12: refresh(c); break;
    default: break;
  }
}

static void onData(chip_t* c, uint8_t b) {
  if (c->cmd == 0x10 || c->cmd == 0x13) {
    uint8_t* ram = c->cmd == 0x10 ? c->old : c->neu;
    uint32_t bx0 = 0, bx1 = BW - 1, ry0 = 0;
    if (c->ptl) { bx0 = c->wx0 / 8; bx1 = c->wx1 / 8; ry0 = c->wy0; }
    uint32_t per = bx1 - bx0 + 1;
    uint32_t row = ry0 + c->wptr / per, col = bx0 + c->wptr % per;
    if (row < H && col < BW) ram[row * BW + col] = b;
    c->wptr++;
    return;
  }
  if (c->argIdx < sizeof c->args) c->args[c->argIdx] = b;
  c->argIdx++;
  switch (c->cmd) {
    case 0x00: if (c->argIdx == 1) c->panel = b; break;
    case 0xE0: if (c->argIdx == 1) c->tsfix = b & 0x02; break;
    case 0xE5: if (c->argIdx == 1) c->forcedTemp = b; break;
    case 0x07:
      if (c->argIdx == 1 && b == 0xA5) {
        c->asleep = true;
        scramble(c);  // 深睡后 RAM 不保留
        printf("[uc8179] deep sleep\n");
      }
      break;
    case 0x90:
      if (c->argIdx == 9) {
        uint8_t* a = c->args;
        c->wx0 = ((a[0] << 8) | a[1]) & ~7;
        c->wx1 = ((a[2] << 8) | a[3]) | 7;
        c->wy0 = (a[4] << 8) | a[5];
        c->wy1 = (a[6] << 8) | a[7];
      }
      break;
    default: break;
  }
}

static void spiDone(void* user, uint8_t* buffer, uint32_t count) {
  chip_t* c = (chip_t*)user;
  if (c->csEdges < 8) printf("[uc8179] spi done count=%u first=0x%02X\n", count, count ? buffer[0] : 0);
  for (uint32_t i = 0; i < count; ++i) {
    if (c->asleep) {
      if (c->ignored++ == 0) printf("[uc8179] ERROR: SPI write while in deep sleep (missing reset)\n");
      continue;
    }
    if (c->dataPhase) onData(c, buffer[i]);
    else onCommand(c, buffer[i]);
  }
  if (pin_read(c->cs) == LOW) spi_start(c->spi, c->buf, sizeof c->buf);
}

static void onCs(void* user, pin_t pin, uint32_t value) {
  chip_t* c = (chip_t*)user;
  if (c->csEdges++ < 6) printf("[uc8179] CS -> %u, DC=%u\n", value, pin_read(c->dc));
  if (value == LOW) {
    c->dataPhase = pin_read(c->dc) == HIGH;
    spi_start(c->spi, c->buf, sizeof c->buf);
  } else {
    spi_stop(c->spi);
  }
}

static void onRst(void* user, pin_t pin, uint32_t value) {
  chip_t* c = (chip_t*)user;
  if (value == LOW) {
    c->asleep = false;  // 硬复位把控制器叫醒；RAM 内容不变（深睡时已经丢了）
    c->ptl = false;
    pin_write(c->busy, HIGH);
  }
}

void chip_init(void) {
  chip_t* c = (chip_t*)calloc(1, sizeof(chip_t));
  c->seed = 12345;
  c->cs = pin_init("CS", INPUT_PULLUP);
  c->dc = pin_init("DC", INPUT);
  c->rst = pin_init("RST", INPUT_PULLUP);
  c->busy = pin_init("BUSY", OUTPUT_HIGH);
  const spi_config_t spi = {
      .user_data = c,
      .sck = pin_init("SCK", INPUT),
      .mosi = pin_init("SDI", INPUT),
      .miso = NO_PIN,
      .mode = 0,
      .done = spiDone,
  };
  c->spi = spi_init(&spi);
  const timer_config_t t = {.user_data = c, .callback = busyDone};
  c->busyTimer = timer_init(&t);
  const pin_watch_config_t csw = {.user_data = c, .edge = BOTH, .pin_change = onCs};
  pin_watch(c->cs, &csw);
  const pin_watch_config_t rw = {.user_data = c, .edge = FALLING, .pin_change = onRst};
  pin_watch(c->rst, &rw);
  uint32_t w, h;
  c->fb = framebuffer_init(&w, &h);
  memset(c->phys, 0xFF, FB_BYTES);  // 新屏出厂是白的
  scramble(c);
  c->asleep = false;
  paint(c);
  printf("[uc8179] ready %ux%u\n", w, h);
}
