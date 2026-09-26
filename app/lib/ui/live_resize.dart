import 'package:flutter/material.dart';

/// Чистая логика живого ресайза окна (Windows) и зон drag-out.
/// Вынесена из screen.dart, чтобы покрывалась юнит-тестами.

/// Якорь — противоположный тянущемуся краю угол: контент прижимается к
/// НЕподвижному краю окна и не «прыгает» при ресайзе слева/сверху.
/// Коды совпадают с AnchorForEdge в win32_window.cpp.
Alignment alignmentForAnchor(int anchor) {
  const anchors = [
    Alignment.topLeft,
    Alignment.topRight,
    Alignment.bottomLeft,
    Alignment.bottomRight,
  ];
  return anchors[anchor.clamp(0, 3)];
}

/// Ширина зоны drag-out готовой строки: вся строка минус правые кнопки
/// (папка/корзина остаются кликабельными). null — зона невозможна.
double? zoneWidth(double rowWidth) {
  final w = rowWidth - 84;
  if (w <= 40) return null;
  return w;
}

/// Попадание точки в зону (координаты канваса 560×670).
bool pointInZone(double px, double py, double zx, double zy, double zw,
    double zh) {
  return px >= zx && py >= zy && px <= zx + zw && py <= zy + zh;
}
