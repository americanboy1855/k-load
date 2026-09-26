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

// Базовый (якорный) прямоугольник 560×670 в (100,100). Центры: cx=380,
// cy=435.
static RECT Base() {
  return {100, 100, 660, 770};
}

// Тяга за угол вниз-вправо в пределах лимитов: левый-верхний угол стоит.
static void CornerBottomRight_Free() {
  RECT anchor = Base();
  RECT rc = anchor;
  rc.right += 40;
  rc.bottom += 48;
  ApplySizeConstraints(&rc, anchor, resize::kBottomRight, kAspect, Lim());
  CHECK(rc.left == 100);
  CHECK(rc.top == 100);
  CHECK(W(rc) == 600);
  CHECK(H(rc) == 718 || H(rc) == 719);
}

// Тяга за угол вниз-вправо ЗА максимальный лимит: якорь (левый-верх)
// стоит, размер зажат максимумом, повторное применение не меняет ничего
// (курсор летит дальше — окно замерло).
static void CornerBottomRight_MaxFrozen() {
  RECT anchor = Base();
  RECT rc = anchor;
  rc.right += 900;
  rc.bottom += 1100;
  ApplySizeConstraints(&rc, anchor, resize::kBottomRight, kAspect, Lim());
  CHECK(rc.left == 100);
  CHECK(rc.top == 100);
  CHECK(W(rc) == 784 && H(rc) == 938);
  RECT rc2 = rc;
  rc2.right += 50;
  rc2.bottom += 60;
  ApplySizeConstraints(&rc2, anchor, resize::kBottomRight, kAspect, Lim());
  CHECK(SameRect(rc, rc2));
}

// Тяга за угол влево-вверх ЗА максимальный лимит: якорь (правый-низ)
// стоит, размер зажат максимумом, замерание (курсор летит — окно стоит).
static void CornerTopLeft_MaxFrozen() {
  RECT anchor = Base();
  RECT rc = anchor;
  rc.left -= 900;
  rc.top -= 1100;
  ApplySizeConstraints(&rc, anchor, resize::kTopLeft, kAspect, Lim());
  CHECK(rc.right == 660);
  CHECK(rc.bottom == 770);
  CHECK(W(rc) == 784 && H(rc) == 938);
  RECT rc2 = rc;
  rc2.left -= 40;
  rc2.top -= 50;
  ApplySizeConstraints(&rc2, anchor, resize::kTopLeft, kAspect, Lim());
  CHECK(SameRect(rc, rc2));
}

// Тяга за угол влево-внутрь за минимальный лимит: якорь (правый-низ)
// стоит, замерание на минимуме.
static void CornerTopLeft_MinFrozen() {
  RECT anchor = Base();
  RECT rc = anchor;
  rc.left += 900;
  rc.top += 1100;
  ApplySizeConstraints(&rc, anchor, resize::kTopLeft, kAspect, Lim());
  CHECK(rc.right == 660);
  CHECK(rc.bottom == 770);
  CHECK(W(rc) == 448 && H(rc) == 536);
  RECT rc2 = rc;
  rc2.left += 40;
  rc2.top += 50;
  ApplySizeConstraints(&rc2, anchor, resize::kTopLeft, kAspect, Lim());
  CHECK(SameRect(rc, rc2));
}

// Левый край за максимум: правая сторона — якорь, перпендикуляр от центра
// якоря, замерание.
static void LeftEdge_MaxFrozen() {
  RECT anchor = Base();
  RECT rc = anchor;
  rc.left -= 900;
  ApplySizeConstraints(&rc, anchor, resize::kLeft, kAspect, Lim());
  CHECK(rc.right == 660);
  CHECK(rc.top == 435 - 938 / 2);
  CHECK(rc.bottom == 435 + 938 / 2);
  CHECK(W(rc) == 784);
  RECT rc2 = rc;
  rc2.left -= 60;
  ApplySizeConstraints(&rc2, anchor, resize::kLeft, kAspect, Lim());
  CHECK(SameRect(rc, rc2));
}

// Правый край за минимум: левая сторона — якорь, замерание.
static void RightEdge_MinFrozen() {
  RECT anchor = Base();
  RECT rc = anchor;
  rc.right -= 900;
  ApplySizeConstraints(&rc, anchor, resize::kRight, kAspect, Lim());
  CHECK(rc.left == 100);
  CHECK(rc.top == 435 - 536 / 2);
  CHECK(rc.bottom == 435 + 536 / 2);
  CHECK(W(rc) == 448);
  RECT rc2 = rc;
  rc2.right -= 40;
  ApplySizeConstraints(&rc2, anchor, resize::kRight, kAspect, Lim());
  CHECK(SameRect(rc, rc2));
}

// Верхний край за максимум: низ — якорь, горизонталь от центра якоря,
// замерание.
static void TopEdge_MaxFrozen() {
  RECT anchor = Base();
  RECT rc = anchor;
  rc.top -= 900;
  ApplySizeConstraints(&rc, anchor, resize::kTop, kAspect, Lim());
  CHECK(rc.bottom == 770);
  CHECK(rc.left == 380 - 784 / 2);
  CHECK(rc.right == 380 + 784 / 2);
  CHECK(H(rc) == 938);
  RECT rc2 = rc;
  rc2.top -= 40;
  ApplySizeConstraints(&rc2, anchor, resize::kTop, kAspect, Lim());
  CHECK(SameRect(rc, rc2));
}

// Нижний край за максимум: верх — якорь, горизонталь от центра якоря,
// замерание.
static void BottomEdge_MaxFrozen() {
  RECT anchor = Base();
  RECT rc = anchor;
  rc.bottom += 900;
  ApplySizeConstraints(&rc, anchor, resize::kBottom, kAspect, Lim());
  CHECK(rc.top == 100);
  CHECK(rc.left == 380 - 784 / 2);
  CHECK(rc.right == 380 + 784 / 2);
  CHECK(H(rc) == 938);
  RECT rc2 = rc;
  rc2.bottom += 60;
  ApplySizeConstraints(&rc2, anchor, resize::kBottom, kAspect, Lim());
  CHECK(SameRect(rc, rc2));
}

// Правый-верхний угол: левый и нижний края (якорь) неподвижны.
static void CornerTopRight_Anchor() {
  RECT anchor = Base();
  RECT rc = anchor;
  rc.right += 300;
  rc.top -= 360;
  ApplySizeConstraints(&rc, anchor, resize::kTopRight, kAspect, Lim());
  CHECK(rc.left == 100);
  CHECK(rc.bottom == 770);
}

// Якоря для Dart: края — центр (4), углы — противоположный угол.
static void AnchorMapping() {
  CHECK(resize::AnchorForEdge(resize::kLeft) == 4);
  CHECK(resize::AnchorForEdge(resize::kRight) == 4);
  CHECK(resize::AnchorForEdge(resize::kTop) == 4);
  CHECK(resize::AnchorForEdge(resize::kBottom) == 4);
  CHECK(resize::AnchorForEdge(resize::kTopLeft) == 3);
  CHECK(resize::AnchorForEdge(resize::kTopRight) == 2);
  CHECK(resize::AnchorForEdge(resize::kBottomLeft) == 1);
  CHECK(resize::AnchorForEdge(resize::kBottomRight) == 0);
  CHECK(EdgeFromHt(HTLEFT) == resize::kLeft);
  CHECK(EdgeFromHt(HTBOTTOMRIGHT) == resize::kBottomRight);
}

// Всегда в лимитах и в пропорции (±1 px округления) на любом шаге.
static void AlwaysWithinLimits() {
  RECT anchor = Base();
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
      ApplySizeConstraints(&r, anchor, edge, kAspect, Lim());
      const LONG w = W(r);
      const LONG h = H(r);
      CHECK(w >= 448 && w <= 784);
      CHECK(h >= 536 && h <= 938);
      CHECK(h >= static_cast<LONG>(w * 670 / 560) - 1);
      CHECK(h <= static_cast<LONG>(w * 670 / 560) + 1);
    }
  }
}

int main() {
  CornerBottomRight_Free();
  CornerBottomRight_MaxFrozen();
  CornerTopLeft_MaxFrozen();
  CornerTopLeft_MinFrozen();
  LeftEdge_MaxFrozen();
  RightEdge_MinFrozen();
  TopEdge_MaxFrozen();
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
