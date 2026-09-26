// Тесты чистой геометрии ресайза (см. resize_geometry.h). Запуск:
// win/run-geometry-tests.cmd. Каждый кейс — утверждение «что видит
// владелец»: тянуть за угол до упора → окно стоит на месте.
#include <windows.h>

#include <cstdio>

#include "resize_geometry.h"

using resize::ApplySizeConstraints;
using resize::Edge;
using resize::EdgeFromHt;
using resize::Limits;

static int g_failed = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
      ++g_failed;                                                      \
    }                                                                  \
  } while (0)

static const double kAspect = 560.0 / 670.0;
static Limits Lim() {
  return {448, 784, 536, 938};
}

static bool SameRect(const RECT& a, const RECT& b) {
  return a.left == b.left && a.top == b.top && a.right == b.right &&
         a.bottom == b.bottom;
}

static LONG W(const RECT& r) { return r.right - r.left; }
static LONG H(const RECT& r) { return r.bottom - r.top; }

// Базовый прямоугольник 560×670 в (100,100).
static RECT Base() {
  return {100, 100, 660, 770};
}

// Тяга за угол вниз-вправо в пределах лимитов: левый-верхний угол стоит.
static void CornerBottomRight_Free() {
  RECT rc = Base();
  rc.right += 40;   // 600 шириной
  rc.bottom += 48;
  resize::ApplySizeConstraints(&rc, resize::kBottomRight, kAspect, Lim());
  CHECK(rc.left == 100);
  CHECK(rc.top == 100);
  CHECK(H(rc) == W(rc) * 670 / 560 || H(rc) == W(rc) * 670 / 560 + 1);
}

// Тяга за угол НИЖЕ-ПРАВО за максимальный лимит: левый-верхний стоит,
// размер зажат максимумом, повторное применение ничего не меняет
// («замерание» на лимите).
static void CornerBottomRight_MaxFrozen() {
  RECT rc = Base();
  rc.right += 900;    // далеко за максимум
  rc.bottom += 1100;
  resize::ApplySizeConstraints(&rc, resize::kBottomRight, kAspect, Lim());
  CHECK(rc.left == 100);
  CHECK(rc.top == 100);
  CHECK(W(rc) <= 784 && H(rc) <= 938);
  RECT before = rc;
  RECT rc2 = rc;
  rc2.right += 50;   // мышь ушла ещё дальше за лимит
  rc2.bottom += 60;
  resize::ApplySizeConstraints(&rc2, resize::kBottomRight, kAspect, Lim());
  CHECK(SameRect(rc, rc2)) ;
  (void)before;
}

// Тяга за угол влево-вверх за минимальный лимит: правый-нижний стоит,
// размер зажат минимумом, повтор — без изменений.
static void CornerTopLeft_MinFrozen() {
  RECT rc = Base();
  rc.left -= 900;
  rc.top -= 1100;
  resize::ApplySizeConstraints(&rc, resize::kTopLeft, kAspect, Lim());
  CHECK(rc.right == 660);
  CHECK(rc.bottom == 770);
  CHECK(W(rc) >= 448 && H(rc) >= 536);
  RECT rc2 = rc;
  rc2.left -= 40;
  rc2.top -= 50;
  resize::ApplySizeConstraints(&rc2, resize::kTopLeft, kAspect, Lim());
  CHECK(SameRect(rc, rc2));
}

// Левый край: правая сторона — якорь, на лимите правый край не двигается.
static void LeftEdge_MaxFrozen() {
  RECT rc = Base();
  rc.left -= 900;
  resize::ApplySizeConstraints(&rc, resize::kLeft, kAspect, Lim());
  CHECK(rc.right == 660);
  CHECK(W(rc) <= 784);
  RECT rc2 = rc;
  rc2.left -= 60;
  resize::ApplySizeConstraints(&rc2, resize::kLeft, kAspect, Lim());
  CHECK(SameRect(rc, rc2));
}

// Правый край: левая сторона — якорь.
static void RightEdge_MinFrozen() {
  RECT rc = Base();
  rc.right -= 900;
  resize::ApplySizeConstraints(&rc, resize::kRight, kAspect, Lim());
  CHECK(rc.left == 100);
  CHECK(W(rc) >= 448);
  RECT rc2 = rc;
  rc2.right -= 40;
  resize::ApplySizeConstraints(&rc2, resize::kRight, kAspect, Lim());
  CHECK(SameRect(rc, rc2));
}

// Верхний край: низ и лево — якорь; на минимуме замерает (ширина при
// тяге верх/низ подгоняется под пропорцию с фиксацией левого края).
static void TopEdge_MinFrozen() {
  RECT rc = Base();
  rc.top -= 900;
  resize::ApplySizeConstraints(&rc, resize::kTop, kAspect, Lim());
  CHECK(rc.bottom == 770);
  CHECK(rc.left == 100);
  CHECK(W(rc) == 784 && H(rc) == 938);
  RECT rc2 = rc;
  rc2.top -= 40;
  resize::ApplySizeConstraints(&rc2, resize::kTop, kAspect, Lim());
  CHECK(SameRect(rc, rc2));
}

// Нижний край: верх и лево — якорь; на максимуме замерает.
static void BottomEdge_MaxFrozen() {
  RECT rc = Base();
  rc.bottom += 900;
  resize::ApplySizeConstraints(&rc, resize::kBottom, kAspect, Lim());
  CHECK(rc.top == 100);
  CHECK(rc.left == 100);
  CHECK(W(rc) == 784 && H(rc) == 938);
  RECT rc2 = rc;
  rc2.bottom += 60;
  resize::ApplySizeConstraints(&rc2, resize::kBottom, kAspect, Lim());
  CHECK(SameRect(rc, rc2));
}

// Правый-верхний угол: левый и нижний края неподвижны.
static void CornerTopRight_Anchor() {
  RECT rc = Base();
  rc.right += 300;
  rc.top -= 360;
  resize::ApplySizeConstraints(&rc, resize::kTopRight, kAspect, Lim());
  CHECK(rc.left == 100);
  CHECK(rc.bottom == 770);
}

// Якоря для Dart: противоположный тянущемуся угол.
static void AnchorMapping() {
  CHECK(resize::AnchorForEdge(resize::kLeft) == 1);
  CHECK(resize::AnchorForEdge(resize::kRight) == 0);
  CHECK(resize::AnchorForEdge(resize::kTop) == 2);
  CHECK(resize::AnchorForEdge(resize::kBottom) == 0);
  CHECK(resize::AnchorForEdge(resize::kTopLeft) == 3);
  CHECK(resize::AnchorForEdge(resize::kTopRight) == 2);
  CHECK(resize::AnchorForEdge(resize::kBottomLeft) == 1);
  CHECK(resize::AnchorForEdge(resize::kBottomRight) == 0);
  CHECK(EdgeFromHt(HTLEFT) == resize::kLeft);
  CHECK(EdgeFromHt(HTBOTTOMRIGHT) == resize::kBottomRight);
}

// Соответствие лимитам: итоговые размеры всегда в диапазоне.
static void AlwaysWithinLimits() {
  RECT rc = Base();
  for (int step = -60; step <= 60; step += 7) {
    for (int e = 0; e < 8; ++e) {
      RECT r = Base();
      const LONG d = step * 20;
      const Edge edge = static_cast<Edge>(e);
      switch (edge) {
        case resize::kLeft: r.left += d; break;
        case resize::kRight: r.right += d; break;
        case resize::kTop: r.top += d; break;
        case resize::kBottom: r.bottom += d; break;
        case resize::kTopLeft: r.left += d; r.top += d; break;
        case resize::kTopRight: r.right += d; r.top += d; break;
        case resize::kBottomLeft: r.left += d; r.bottom += d; break;
        case resize::kBottomRight: r.right += d; r.bottom += d; break;
      }
      resize::ApplySizeConstraints(&r, edge, kAspect, Lim());
      const LONG w = W(r);
      const LONG h = H(r);
      CHECK(w >= 448 && w <= 784);
      CHECK(h >= 536 && h <= 938);
      // пропорция в допуске ±1 px на округление
      CHECK(h >= static_cast<LONG>(w * 670 / 560) - 1);
      CHECK(h <= static_cast<LONG>(w * 670 / 560) + 1);
    }
  }
}

int main() {
  CornerBottomRight_Free();
  CornerBottomRight_MaxFrozen();
  CornerTopLeft_MinFrozen();
  LeftEdge_MaxFrozen();
  RightEdge_MinFrozen();
  TopEdge_MinFrozen();
  BottomEdge_MaxFrozen();
  CornerTopRight_Anchor();
  AnchorMapping();
  AlwaysWithinLimits();
  if (g_failed == 0) {
    std::printf("geometry: все тесты пройдены\n");
    return 0;
  }
  std::printf("geometry: провалено проверок: %d\n", g_failed);
  return 1;
}
