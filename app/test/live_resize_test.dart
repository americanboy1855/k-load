import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:kload/ui/live_resize.dart';

void main() {
  group('alignmentForAnchor', () {
    test('якорь — противоположный тянущемуся краю угол', () {
      expect(alignmentForAnchor(0), Alignment.topLeft);
      expect(alignmentForAnchor(1), Alignment.topRight);
      expect(alignmentForAnchor(2), Alignment.bottomLeft);
      expect(alignmentForAnchor(3), Alignment.bottomRight);
    });

    test('край (4) — центр', () {
      expect(alignmentForAnchor(4), Alignment.center);
    });

    test('вне диапазона — зажим в допустимый диапазон (без исключений)', () {
      expect(alignmentForAnchor(-5), Alignment.topLeft);
      expect(alignmentForAnchor(99), Alignment.center);
    });
  });

  group('zoneWidth', () {
    test('обычная строка — зона без правых кнопок (84px)', () {
      expect(zoneWidth(524), 440);
    });

    test('узкая строка — зоны нет', () {
      expect(zoneWidth(100), isNull);
      expect(zoneWidth(124), isNull); // ровно минимум 40 не достигнут
    });

    test('граничная ширина — минимальная зона допустима', () {
      expect(zoneWidth(125), 41);
    });
  });

  group('pointInZone', () {
    const zx = 13.0, zy = 400.0, zw = 440.0, zh = 34.0;
    test('внутри', () {
      expect(pointInZone(200, 417, zx, zy, zw, zh), isTrue);
    });
    test('на границе — внутри', () {
      expect(pointInZone(13, 400, zx, zy, zw, zh), isTrue);
      expect(pointInZone(453, 434, zx, zy, zw, zh), isTrue);
    });
    test('снаружи', () {
      expect(pointInZone(12.9, 417, zx, zy, zw, zh), isFalse);
      expect(pointInZone(200, 435, zx, zy, zw, zh), isFalse);
    });
  });
}
