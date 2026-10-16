#include "Ui.h"

#include <algorithm>
#include <cstdio>

#include "DateUtil.h"
#include "qrcodegen.h"

namespace et {

namespace {
const char* WEEK_CN[7] = {"一", "二", "三", "四", "五", "六", "日"};

constexpr int HDR_H = 64, LINE = 3;
constexpr int FTR_Y = 437;
constexpr int LIST_W = 528, LIST_TOP = 75, ROW_H = 60;
constexpr int SIDE_X = 553;
}  // namespace

bool Assets::load(Fs& fs) {
  return f24.load(fs, "/fonts/sans24.bin") && f32.load(fs, "/fonts/sans32.bin") &&
         f48.load(fs, "/fonts/sans48.bin") && icons.load(fs, "/icons.bin");
}

void Ui::render(Canvas& c, const Model& m, const UiState& s) const {
  c.clear(WHITE);
  switch (s.view) {
    case View::Main: mainView(c, m, s); break;
    case View::Celebrate: celebrate(c, m); break;
    case View::Edit: editView(c, s); break;
    case View::Setup: setupView(c, s); break;
    case View::NoTime: noTimeView(c); break;
  }
}

void Ui::header(Canvas& c, const UiState& s) const {
  c.fillRect(0, HDR_H, W, LINE, BLACK);
  if (s.battery >= 0) {
    const int bx = 732, by = 21;
    c.rect(bx, by, 44, 22, 3, BLACK);
    c.fillRect(bx + 44, by + 6, 4, 10, BLACK);
    int fill = (38 - 4) * std::min(100, s.battery) / 100;
    c.fillRect(bx + 5, by + 5, fill, 12, BLACK);
    if (s.battery < 15) a_.f24.draw(c, bx - 84, 19, "电量低", BLACK);
  }
}

void Ui::checkbox(Canvas& c, int x, int y, bool done, Color col) const {
  c.rect(x, y, 34, 34, 4, col);
  if (done) {
    c.thickLine(x + 7, y + 17, x + 14, y + 25, 5, col);
    c.thickLine(x + 14, y + 25, x + 28, y + 6, 5, col);
  }
}

void Ui::mainView(Canvas& c, const Model& m, const UiState& s) const {
  header(c, s);
  if (m.today.day >= 0) {
    Ymd d = civilFromDays(m.today.day);
    char buf[64];
    snprintf(buf, sizeof buf, "%d月%d日 星期%s", d.m, d.d, WEEK_CN[weekdayMon0(m.today.day)]);
    a_.f32.draw(c, 24, 16, buf, BLACK);
  }
  c.fillRect(LIST_W, HDR_H + LINE, LINE, FTR_Y - HDR_H - LINE, BLACK);
  c.fillRect(0, FTR_Y, W, LINE, BLACK);

  const auto& items = m.today.items;
  if (!m.counts()) {
    restBody(c, m);
    a_.f24.drawCentered(c, W / 2, FTR_Y + 8, "长按 ● 3 秒：家长设置", BLACK);
  } else {
    const int n = static_cast<int>(items.size());
    const int first = n <= LIST_ROWS ? 0 : std::clamp(s.cursor - (LIST_ROWS - 1), 0, n - LIST_ROWS);
    for (int r = 0; r < LIST_ROWS && first + r < n; ++r) {
      const Item& it = items[first + r];
      const int y = LIST_TOP + r * ROW_H;
      checkbox(c, 20, y + 13, it.done, BLACK);
      a_.icons.draw(c, it.icon, 68, y + 10, BLACK);
      int tw = a_.f32.draw(c, 122, y + 14, it.text, BLACK, 370);
      // 已完成的图标和文字变灰。不用删除线：会把「口」划成「日」，孩子正在认字
      if (it.done) c.halftone(68, y + 6, 122 - 68 + tw, ROW_H - 12);
      if (s.showCursor && first + r == s.cursor) c.invertRect(0, y, LIST_W, ROW_H);
    }
    if (first > 0) a_.f24.draw(c, 498, LIST_TOP - 6, "▲", BLACK);
    if (first + LIST_ROWS < n) a_.f24.draw(c, 498, FTR_Y - 30, "▼", BLACK);
    a_.f24.drawCentered(c, W / 2, FTR_Y + 8, "▲▼ 选择     ● 打勾     长按 ● 家长设置", BLACK);
  }
  sidePanel(c, m);
}

void Ui::restBody(Canvas& c, const Model& m) const {
  const int cx = LIST_W / 2;
  if (m.today.rest) {
    a_.f48.drawCentered(c, cx, 170, "今天休息", BLACK);
    a_.f32.drawCentered(c, cx, 250, "好好玩吧！", BLACK);
  } else {
    a_.f48.drawCentered(c, cx, 170, "今天没有任务", BLACK);
    a_.f24.drawCentered(c, cx, 250, "请家长在手机上添加", BLACK);
  }
}

void Ui::sidePanel(Canvas& c, const Model& m) const {
  char buf[64];
  if (m.counts()) {
    const int total = static_cast<int>(m.today.items.size());
    const int done = static_cast<int>(m.doneCount());
    snprintf(buf, sizeof buf, "今天完成 %d / %d", done, total);
    a_.f24.draw(c, SIDE_X, 84, buf, BLACK);
    const int bx = SIDE_X, by = 122, bw = 225, bh = 26;
    c.rect(bx, by, bw, bh, LINE, BLACK);
    const int inner = bw - 2 * LINE;
    for (int i = 0; i < total; ++i) {
      int x0 = bx + LINE + inner * i / total, x1 = bx + LINE + inner * (i + 1) / total;
      if (i < done) c.fillRect(x0, by, x1 - x0, bh, BLACK);
      if (i > 0) c.fillRect(x0 - 1, by, LINE, bh, BLACK);
    }
  } else {
    a_.f24.draw(c, SIDE_X, 84, m.today.rest ? "休息日" : "没有任务", BLACK);
  }
  a_.icons.draw(c, "star_fill_44", SIDE_X, 166, BLACK);
  snprintf(buf, sizeof buf, "%d", m.displayStars());
  a_.f48.draw(c, SIDE_X + 56, 164, buf, BLACK);
  snprintf(buf, sizeof buf, "连续 %d 天", m.displayStreak());
  a_.f24.draw(c, SIDE_X, 232, buf, BLACK);

  a_.f24.draw(c, SIDE_X, 284, "本周", BLACK);
  auto week = m.weekView();
  for (int i = 0; i < 7; ++i) {
    const int x = SIDE_X + i * 32;
    a_.f24.draw(c, x + 4, 320, WEEK_CN[i], BLACK);
    switch (week[i]) {
      case 1: a_.icons.draw(c, "star_fill_26", x + 3, 356, BLACK); break;
      case 2: c.fillRect(x + 13, 366, 6, 6, BLACK); break;
      case 3: a_.f24.draw(c, x + 4, 357, "休", BLACK); break;
      case 4: a_.icons.draw(c, "star_line_26", x + 3, 356, BLACK); break;
      default: break;
    }
  }
}

void Ui::celebrate(Canvas& c, const Model& m) const {
  a_.icons.draw(c, "star_fill_150", (W - 150) / 2, 48, BLACK);
  a_.f48.drawCentered(c, W / 2, 232, "今天的任务全部完成！", BLACK);
  char buf[80];
  snprintf(buf, sizeof buf, "连续 %d 天 · 一共 %d 颗星", m.displayStreak(), m.displayStars());
  a_.f32.drawCentered(c, W / 2, 312, buf, BLACK);
}

void Ui::qr(Canvas& c, const std::string& text, int x, int y, int box) const {
  static uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
  static uint8_t code[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
  if (!qrcodegen_encodeText(text.c_str(), tmp, code, qrcodegen_Ecc_MEDIUM, 1, 10, qrcodegen_Mask_AUTO, true)) return;
  const int n = qrcodegen_getSize(code);
  const int scale = box / (n + 8);  // 四周各留 4 格空白
  const int off = (box - n * scale) / 2;
  c.rect(x, y, box, box, LINE, BLACK);
  for (int r = 0; r < n; ++r)
    for (int col = 0; col < n; ++col)
      if (qrcodegen_getModule(code, col, r)) c.fillRect(x + off + col * scale, y + off + r * scale, scale, scale, BLACK);
}

void Ui::editView(Canvas& c, const UiState& s) const {
  qr(c, s.url, 48, 110, 264);
  const int x = 352;
  a_.f48.draw(c, x, 96, "家长设置", BLACK);
  a_.f24.draw(c, x, 172, "手机连家里同一个 WiFi，", BLACK);
  a_.f24.draw(c, x, 204, "扫码或在浏览器打开：", BLACK);
  a_.f32.draw(c, x, 244, s.url, BLACK, W - x - 16);
  a_.f24.draw(c, x, 288, "或 http://todo.local", BLACK);
  a_.f24.draw(c, x, 352, "10 分钟没有操作会自动退出", BLACK);
  a_.f24.draw(c, x, 386, "按 ● 键也可以退出", BLACK);
}

void Ui::setupView(Canvas& c, const UiState& s) const {
  qr(c, "WIFI:T:nopass;S:" + s.apSsid + ";;", 48, 110, 264);
  const int x = 352;
  a_.f32.draw(c, x, 96, "第一次使用：连接 WiFi", BLACK, W - x - 16);
  a_.f24.draw(c, x, 160, "1. 手机扫码，连上热点", BLACK);
  a_.f24.draw(c, x + 26, 194, s.apSsid, BLACK);
  a_.f24.draw(c, x, 240, "2. 浏览器打开", BLACK);
  a_.f24.draw(c, x + 26, 274, s.url, BLACK);
  a_.f24.draw(c, x, 320, "3. 选家里的 WiFi，填密码", BLACK);
}

void Ui::noTimeView(Canvas& c) const {
  a_.f48.drawCentered(c, W / 2, 170, "时间还没同步", BLACK);
  a_.f24.drawCentered(c, W / 2, 260, "需要连上 WiFi 才能知道今天是几号", BLACK);
  a_.f24.drawCentered(c, W / 2, 300, "长按 ● 键 3 秒，进入设置", BLACK);
}

}  // namespace et
