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

// Якорь для Dart: контент прижимается к НЕподвижной части окна, чтобы не
// «прыгать» при ресайзе. Края (растут с центровкой перпендикулярной оси) —
// якорь ЦЕНТР (4). Углы — противоположный угол (0 topLeft, 1 topRight,
// 2 bottomLeft, 3 bottomRight).
inline int AnchorForEdge(Edge edge) {
  switch (edge) {
    case kTopLeft: return 3;
    case kTopRight: return 2;
    case kBottomLeft: return 1;
    case kBottomRight: return 0;
    default: return 4;  // края — центр
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

// Пропорция aspect = ширина/высота (560/670). rc — желаемый прямоугольник
// (с мышинными дельтами), anchor — прямоугольник НАЧАЛА жеста (якорь).
// КРАЙ: ось тяги — противоположная сторона неподвижна (от якоря),
// перпендикулярная ось — симметрично от центра якоря. УГОЛ: противоположный
// угол якоря неподвижен. На лимите — «замерание»: повторное применение к
// прямоугольнику на лимите не меняет ни пикселя (окно не уезжает, когда
// курсор продолжает лететь за лимитом).
inline void ApplySizeConstraints(RECT* rc, const RECT& anchor, Edge edge,
                                 double aspect, const Limits& lim) {
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
  const bool corner = edge == kTopLeft || edge == kTopRight ||
                      edge == kBottomLeft || edge == kBottomRight;
  if (!corner) {
    // Край: ось тяги — противоположная сторона от якоря.
    switch (edge) {
      case kLeft: rc->left = anchor.right - w; break;
      case kRight: rc->right = anchor.left + w; break;
      case kTop: rc->top = anchor.bottom - h; break;
      default: rc->bottom = anchor.top + h; break;
    }
    // Перпендикулярная ось — симметрично от центра якоря.
    if (edge == kLeft || edge == kRight) {
      const LONG cy = (anchor.top + anchor.bottom) / 2;
      rc->top = cy - h / 2;
      rc->bottom = rc->top + h;
    } else {
      const LONG cx = (anchor.left + anchor.right) / 2;
      rc->left = cx - w / 2;
      rc->right = rc->left + w;
    }
    return;
  }
  // Угол: противоположный угол якоря неподвижен (его координаты берём
  // из anchor напрямую), тянущиеся стороны вычисляются от зажатого размера.
  switch (edge) {
    case kTopLeft:  // тянем верх-лево → якорь прав-низ
      rc->right = anchor.right;
      rc->left = rc->right - w;
      rc->bottom = anchor.bottom;
      rc->top = rc->bottom - h;
      break;
    case kTopRight:  // тянем верх-право → якорь лев-низ
      rc->left = anchor.left;
      rc->right = rc->left + w;
      rc->bottom = anchor.bottom;
      rc->top = rc->bottom - h;
      break;
    case kBottomLeft:  // тянем низ-лево → якорь прав-верх
      rc->right = anchor.right;
      rc->left = rc->right - w;
      rc->top = anchor.top;
      rc->bottom = rc->top + h;
      break;
    default:  // kBottomRight: тянем низ-право → якорь лев-верх
      rc->left = anchor.left;
      rc->top = anchor.top;
      rc->right = rc->left + w;
      rc->bottom = rc->top + h;
      break;
  }
}

}  // namespace resize
