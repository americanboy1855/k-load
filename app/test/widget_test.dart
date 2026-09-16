// Регрессия формата ошибок (аудит): единый вид «ЗАГОЛОВОК — [подсказка]»,
// без точки в конце, длинное тире. Заменяет пустышку «каркас объявлен».
import 'package:flutter_test/flutter_test.dart';

import 'package:kload/ui/screen.dart';

void main() {
  test('ошибки сети и VPN — точный текст без точки', () {
    final m = mapUserMessage('сеть пропала');
    expect(m.title, 'НЕТ СЕТИ ИЛИ VPN');
    expect(m.hint, 'Проверьте интернет и включите VPN');
    expect(userMessageText(m), 'НЕТ СЕТИ ИЛИ VPN — [Проверьте интернет и включите VPN]');
  });

  test('DRM распознаётся по стему «защищ»', () {
    final m = mapUserMessage('Запись защищена от скачивания');
    expect(m.title, 'ЗАПИСЬ ЗАЩИЩЕНА DRM');
    expect(userMessageText(m),
        'ЗАПИСЬ ЗАЩИЩЕНА DRM — [Скачивание защищённых записей невозможно]');
  });

  test('неизвестная ошибка уходит в ЧТО-ТО ПОШЛО НЕ ТАК без точки', () {
    final m = mapUserMessage('что-то совсем странное');
    expect(m.title, 'ЧТО-ТО ПОШЛО НЕ ТАК');
    expect(m.hint.endsWith('.'), isFalse,
        reason: 'подсказка не должна заканчиваться точкой');
  });

  test('все 14 позиций каталога — в формате «TITLE — [подсказка]»', () {
    const samples = <String, String>{
      'vpn отвалился': 'НЕТ СЕТИ ИЛИ VPN',
      '404': 'КОНТЕНТ НЕДОСТУПЕН',
      'drm protected': 'ЗАПИСЬ ЗАЩИЩЕНА DRM',
      'unsupported url': 'ССЫЛКА НЕ РАСПОЗНАНА',
      'pinterest': 'ИСТОЧНИК НЕ ПОДДЕРЖИВАЕТ ПОИСК',
      'отрезок': 'НЕВЕРНЫЙ ДИАПАЗОН',
      'requested format': 'ФОРМАТ НЕДОСТУПЕН',
      'аудио': 'АУДИОДОРОЖКИ НЕТ',
      'permission': 'НЕ УДАЛОСЬ СОХРАНИТЬ ФАЙЛ',
      'отменено': 'ЗАГРУЗКА ОТМЕНЕНА',
      'загрузчик не найден': 'ИНСТРУМЕНТЫ НЕ НАЙДЕНЫ',
      'загрузчик не запущен системой': 'ЗАГРУЗЧИК ЗАБЛОКИРОВАН',
      'обработать': 'НЕ УДАЛОСЬ ПОДГОТОВИТЬ ФАЙЛ',
    };
    samples.forEach((raw, title) {
      final text = userMessageText(mapUserMessage(raw));
      expect(text.startsWith('$title — ['), isTrue, reason: raw);
      expect(text.endsWith(']'), isTrue, reason: raw);
      expect(text.endsWith('.]'), isFalse, reason: raw);
    });
    final internal = userMessageText(mapUserMessage('совсем неизвестное'));
    expect(internal.startsWith('ЧТО-ТО ПОШЛО НЕ ТАК — ['), isTrue);
    expect(internal.endsWith(']'), isTrue);
  });

  test('проверка порядка: DRM раньше «недоступно»', () {
    // «защищена» и «удалена» в одной строке — DRM частный случай.
    final m = mapUserMessage('запись защищена и удалена');
    expect(m.title, 'ЗАПИСЬ ЗАЩИЩЕНА DRM');
  });
}
