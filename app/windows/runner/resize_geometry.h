#pragma once
// Чистая геометрия ресайза K LOAD: пропорция 560:670, лимиты 0.8×–1.4×,
// якорь — противоположная тянущемуся краю сторона, которая ЗАМИРАЕТ
// НАМЕРТВО при упоре в лимит (репорт «окно уезжает при достижении
// мин/макс»). Заголовок самодостаточный: подключается раннером и
// тестами (win/run-geometry-tests.cmd), не тянет ни MFC, ни Dart.
#include <windows.h>

namespace resize {

enum Edge {
  kLeft = 0,
  kRight,
  kTop,
  kBottom,
  kTopLeft,
  kTopRight,
  kBottomLeft,
  kBottomRight,
};

inline Edge EdgeFromHt(UINT ht) {
  switch (ht) {
    case HTLEFT: return kLeft;
    case HTRIGHT: return kRight;
    case HTTOP: return kTop;
    case HTBOTTOM: return kBottom;
    case HTTOPLEFT: return kTopLeft;
    case HTTOPRIGHT: return kTopRight;
    case HTBOTTOMLEFT: return kBottomLeft;
    default: return kBottomRight;
  }
}

// Якорь для Dart (0 topLeft, 1 topRight, 2 bottomLeft, 3 bottomRight) —
// противоположный тянущемуся краю угол: контент прижимается к нему и не
// «прыгает» при ресайзе слева/сверху.
inline int AnchorForEdge(Edge edge) {
  switch (edge) {
    case kLeft: return 1;        // верх-право неподвижен
    case kTop: return 2;         // низ-лево
    case kTopLeft: return 3;     // низ-право
    case kTopRight: return 2;    // низ-лево
    case kBottomLeft: return 1;  // верх-право
    default: return 0;           // topLeft
  }
}

struct Limits {
  LONG min_w;
  LONG max_w;
  LONG min_h;
  LONG max_h;
};

inline LONG RoundHalf(double v) {
  return static_cast<LONG>(v + 0.5);
}

// Пропорция aspect = ширина/высота (560/670). Сторона, ПРОТИВОПОЛОЖНАЯ
// тянущемуся краю, неподвижна всегда — в том числе при упоре в лимит:
// применив ограничения к прямоугольнику, который уже на лимите, получим
// тот же прямоугольник (окно не сдвигается ни на пиксель).
inline void ApplySizeConstraints(RECT* rc, Edge edge, double aspect,
                                 const Limits& lim) {
  LONG w = rc->right - rc->left;
  LONG h = rc->bottom - rc->top;
  if (edge == kTop || edge == kBottom) {
    w = RoundHalf(static_cast<double>(h) * aspect);
  } else {
    h = RoundHalf(static_cast<double>(w) / aspect);
  }
  if (h < lim.min_h) {
    h = lim.min_h;
    w = RoundHalf(static_cast<double>(h) * aspect);
  } else if (h > lim.max_h) {
    h = lim.max_h;
    w = RoundHalf(static_cast<double>(h) * aspect);
  }
  // Якорная сторона = противоположная тянущейся: она остаётся на месте.
  // Тяга левой стороны (kLeft/kTopLeft/kBottomLeft) → правая сторона якорь,
  // иначе — левая. Тяга верхней (kTop/kTopLeft/kTopRight) → низ якорь,
  // иначе — верх. Совпадает с AnchorForEdge для Dart.
  const bool anchor_left = edge != kLeft && edge != kTopLeft &&
                           edge != kBottomLeft;
  const bool anchor_top = edge != kTop && edge != kTopLeft &&
                          edge != kTopRight;
  if (anchor_left) {
    rc->right = rc->left + w;
  } else {
    rc->left = rc->right - w;
  }
  if (anchor_top) {
    rc->bottom = rc->top + h;
  } else {
    rc->top = rc->bottom - h;
  }
}

}  // namespace resize
