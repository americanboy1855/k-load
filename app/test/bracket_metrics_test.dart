// Разовый замер метрик глифа «]» (Press Start 2P 9px, ls 0.1): advance и
// видимый (чернильный) правый край — для выравнивания «[ОЧИСТИТЬ]» по
// кнопке-корзине. Можно удалить после использования.
import 'dart:io';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  test('measure closing bracket ink edge', () async {
    final bytes =
        await File('assets/fonts/PressStart2P-Regular.ttf').readAsBytes();
    final loader = FontLoader('Press Start 2P')
      ..addFont(Future.value(ByteData.view(bytes.buffer)));
    await loader.load();

    const style = TextStyle(
        fontFamily: 'Press Start 2P', fontSize: 9, letterSpacing: 0.1);

    // Advance глифа с хвостовым letter-spacing.
    final tp = TextPainter(
      text: const TextSpan(text: ']', style: style),
      textDirection: TextDirection.ltr,
    )..layout();
    final advance = tp.width;
    debugPrint('ADVANCE «]»: $advance');

    // Рендер «]» в картинку и поиск правого чернильного пикселя.
    const w = 40, h = 32;
    final recorder = ui.PictureRecorder();
    final canvas = Canvas(recorder);
    tp.paint(canvas, Offset.zero);
    final pic = recorder.endRecording();
    final img = pic.toImageSync(w, h);
    final data = (await img.toByteData(format: ui.ImageByteFormat.rawRgba))!
        .buffer
        .asUint8List();
    var inkRight = 0.0;
    for (var y = 0; y < h; y++) {
      for (var x = w - 1; x >= 0; x--) {
        final a = data[(y * w + x) * 4 + 3];
        if (a > 8) {
          if (x + 1 > inkRight) inkRight = (x + 1).toDouble();
          break;
        }
      }
    }
    debugPrint('INK RIGHT (от начала блока): $inkRight');
    debugPrint('ХВОСТ (advance - inkRight): ${advance - inkRight}');
    tp.dispose();
    img.dispose();
  });
}
