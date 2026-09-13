// E2E «как пользователь» на реальной Windows (CI windows-latest).
// Запуск: flutter test integration_test -d windows
//
// Сценарии: приложение поднимается с ядром и инструментами, ссылка на
// YouTube разбирается и скачивается файл на диск. Остальной чек-лист
// приёмки (8 сервисов, ХРОН, плейлисты, drag-out, переустановка) —
// ручной прогон по чек-листу релиза.

import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';

import 'package:kload/main.dart' as app;

void main() {
  final binding = IntegrationTestWidgetsFlutterBinding.ensureInitialized();
  binding.framePolicy = LiveTestWidgetsFlutterBindingFramePolicy.fullyLive;

  final downloads =
      Directory('${Platform.environment['USERPROFILE']}/Downloads/K LOAD');

  testWidgets('boot: ядро поднялось, инструменты найдены',
      (tester) async {
    app.main();
    // В интерфейсе живут бесконечные анимации (LED, плазма, вуаль) —
    // pumpAndSettle не оседает, качаем фиксированно.
    for (var i = 0; i < 8; i++) {
      await tester.pump(const Duration(seconds: 1));
    }

    // Диспетчер (низ экрана) и поле ввода — признак живого экрана.
    expect(find.textContaining('K LOAD'), findsWidgets);
  });

  testWidgets('e2e: YouTube — ссылка разбирается и скачивается',
      (tester) async {
    if (downloads.existsSync()) {
      downloads.deleteSync(recursive: true);
    }

    app.main();
    for (var i = 0; i < 8; i++) {
      await tester.pump(const Duration(seconds: 1));
    }

    // Вставка ссылки (знаменитый «Me at the zoo», 19 секунд, ~2 МБ).
    await tester.enterText(
        find.byType(TextField).first, 'https://www.youtube.com/watch?v=jNQXAC9IVRw');
    await tester.pump(const Duration(seconds: 2));

    // Debounce 600 мс + разбор ссылки → карточка с кнопкой СКАЧАТЬ.
    final goDeadline = DateTime.now().add(const Duration(seconds: 30));
    var appeared = false;
    while (DateTime.now().isBefore(goDeadline)) {
      await tester.pump(const Duration(seconds: 1));
      if (find.textContaining('СКАЧАТЬ').evaluate().isNotEmpty) {
        appeared = true;
        break;
      }
    }
    expect(appeared, true, reason: 'карточка разбора не появилась за 30 секунд');
    await tester.tap(find.textContaining('СКАЧАТЬ').last);

    // Скачивание короткого ролика: до 90 секунд.
    final deadline = DateTime.now().add(const Duration(seconds: 90));
    var downloaded = false;
    while (DateTime.now().isBefore(deadline)) {
      await tester.pump(const Duration(seconds: 2));
      if (downloads.existsSync()) {
        final files = downloads
            .listSync(recursive: true)
            .whereType<File>()
            .where((f) => !f.path.endsWith('.part'))
            .toList();
        if (files.isNotEmpty) {
          downloaded = true;
          break;
        }
      }
    }
    expect(downloaded, true,
        reason: 'файл не появился в ${downloads.path} за 90 секунд');
  });
}
