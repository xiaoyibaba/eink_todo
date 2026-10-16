// 各个画面的绘制。布局对应 docs/mockup.html。
#pragma once
#include <string>

#include "Canvas.h"
#include "Font.h"
#include "Icons.h"
#include "Model.h"

namespace et {

struct Assets {
  Font f24, f32, f48;
  Icons icons;
  bool load(Fs& fs);
};

enum class View : uint8_t { Main, Celebrate, Edit, Setup, NoTime };

struct UiState {
  View view = View::Main;
  bool showCursor = false;
  int cursor = 0;
  int battery = -1;
  int64_t localSecs = 0;  // 用来显示日期
  std::string url;        // 编辑模式：网页地址
  std::string apSsid;     // 配网模式：热点名
};

constexpr int LIST_ROWS = 6;  // 一屏最多显示几条

class Ui {
 public:
  explicit Ui(const Assets& a) : a_(a) {}
  void render(Canvas& c, const Model& m, const UiState& s) const;

 private:
  void header(Canvas& c, const UiState& s) const;
  void mainView(Canvas& c, const Model& m, const UiState& s) const;
  void restBody(Canvas& c, const Model& m) const;
  void sidePanel(Canvas& c, const Model& m) const;
  void celebrate(Canvas& c, const Model& m) const;
  void editView(Canvas& c, const UiState& s) const;
  void setupView(Canvas& c, const UiState& s) const;
  void noTimeView(Canvas& c) const;
  void qr(Canvas& c, const std::string& text, int x, int y, int box) const;
  void checkbox(Canvas& c, int x, int y, bool done, Color col) const;

  const Assets& a_;
};

}  // namespace et
