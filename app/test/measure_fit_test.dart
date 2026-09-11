// Разовое измерение: влезает ли «ВЕРНУТЬСЯ К РЕЗУЛЬТАТАМ» в строку
// источника при разных бейджах. Не влияет на приложение — можно удалить.
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  test('measure source row fit', () async {
    Future<void> load(String family, String file) async {
      final bytes = await File('assets/fonts/$file').readAsBytes();
      final loader = FontLoader(family)
        ..addFont(Future.value(ByteData.view(bytes.buffer)));
      await loader.load();
    }

    await load('Press Start 2P', 'PressStart2P-Regular.ttf');
    await load('Departure Mono', 'DepartureMono-Regular.otf');

    double measure(String text, String family, double size, double ls) {
      final tp = TextPainter(
        text: TextSpan(
          text: text,
          style: TextStyle(
              fontFamily: family, fontSize: size, letterSpacing: ls),
        ),
        textDirection: TextDirection.ltr,
      )..layout();
      final w = tp.width;
      tp.dispose();
      return w;
    }

    // Правый край строки: ширина карточки = 560 - 2*20 (поля UI) -
    // 118 (обложка) - 15 (зазор) = 387.
    const available = 387.0;
    const buttonPadding = 16.0; // 8+8 по горизонтали у _GhostButton
    const linkGap = 9.0;        // SizedBox между бейджем и [ССЫЛКА]

    final button =
        measure('ВЕРНУТЬСЯ К РЕЗУЛЬТАТАМ', 'Press Start 2P', 7, .08) +
            buttonPadding;
    final link = measure('[ССЫЛКА]', 'Departure Mono', 10, .04);
    debugPrint('КНОПКА: $button  [ССЫЛКА]: $link');

    for (final badgeText in [
      'YOUTUBE',
      'SOUNDCLOUD',
      'ПЛЕЙЛИСТ · YOUTUBE',
      'ПЛЕЙЛИСТ · SOUNDCLOUD',
      'YT MUSIC',
      'ПЛЕЙЛИСТ · YT MUSIC',
    ]) {
      final badge = measure(badgeText, 'Press Start 2P', 8, .04) + 18;
      final total = badge + linkGap + link + button;
      debugPrint(
          '$badgeText → строка: ${total.toStringAsFixed(1)} / $available '
          '${total <= available ? "OK" : "НЕ ВЛЕЗАЕТ"}');
    }
  });
}
