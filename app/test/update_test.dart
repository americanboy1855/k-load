import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

import 'package:kload/update/update_service.dart';

void main() {
  group('чистые функции: версии и ассеты', () {
    test('версии режутся и сравниваются', () {
      expect(UpdateService.parseVersion('v1.2.0'), [1, 2, 0]);
      expect(UpdateService.parseVersion('1.2'), [1, 2]);
      expect(UpdateService.parseVersion('1.0.0+1'), [1, 0, 0]);
      expect(UpdateService.parseVersion('abc'), isNull);
      expect(UpdateService.isNewer([1, 2], [1, 2]), isFalse);
      expect(UpdateService.isNewer([1, 2, 0], [1, 1, 9]), isTrue);
      expect(UpdateService.isNewer([1, 10], [1, 9, 5]), isTrue);
      expect(UpdateService.isNewer([0, 9], [1, 0]), isFalse);
      // свою версию не распознали — молчим (не пристаём с обновлениями)
      expect(UpdateService.isNewer([2, 0], null), isFalse);
    });

    test('ассет установщика по платформе', () {
      final assets = [
        {'name': 'K-LOAD-Instrukciya.pdf', 'browser_download_url': 'http://x/1.pdf'},
        {'name': 'K-LOAD-V1.1-setup.pkg', 'browser_download_url': 'http://x/1.pkg'},
        {'name': 'K-LOAD-V1.1-Setup.exe', 'browser_download_url': 'http://x/1.exe'},
      ];
      expect(UpdateService.pickAsset(assets, platform: 'macos')?.url,
          'http://x/1.pkg');
      expect(UpdateService.pickAsset(assets, platform: 'windows')?.url,
          'http://x/1.exe');
      expect(UpdateService.pickAsset(assets, platform: 'linux'), isNull);
    });
  });

  group('check/download через локальный сервер', () {
    late HttpServer server;
    late Directory stamps;

    setUp(() async {
      stamps = await Directory.systemTemp.createTemp('kload-stamps');
      server = await HttpServer.bind(InternetAddress.loopbackIPv4, 0);
      server.listen((req) async {
        final path = req.uri.path;
        if (path == '/repos/test/k-load/releases/latest') {
          final payload = jsonEncode({
            'tag_name': 'v1.2.0',
            'assets': [
              {
                'name': 'K-LOAD-V1.2-setup.pkg',
                'browser_download_url':
                    'http://127.0.0.1:${server.port}/fake.pkg',
              },
              {
                'name': 'K-LOAD-V1.2-Setup.exe',
                'browser_download_url':
                    'http://127.0.0.1:${server.port}/fake.exe',
              },
            ],
          });
          req.response.headers.contentType = ContentType.json;
          req.response.add(utf8.encode(payload));
          await req.response.close();
        } else if (path == '/fake.pkg' || path == '/fake.exe') {
          req.response.headers.set('Content-Length', '1000');
          req.response.add(List.filled(1000, 0x4B));
          await req.response.close();
        } else {
          req.response.statusCode = 404;
          await req.response.close();
        }
      });
    });

    tearDown(() async {
      await server.close(force: true);
      stamps.deleteSync(recursive: true);
    });

    UpdateService service() => UpdateService(
        repo: 'test/k-load',
        apiBase: 'http://127.0.0.1:${server.port}',
        stampDir: stamps.path);

    test('новый релиз → UpdateInfo с установщиком этой платформы', () async {
      final info = await service().check('1.0.0+1');
      expect(info, isNotNull);
      expect(info!.version, '1.2.0');
      // На macOS CI ждём .pkg, на Windows CI — .exe.
      final suffix = Platform.isMacOS ? '.pkg' : '.exe';
      expect(info.assetName, endsWith(suffix));
      expect(info.assetUrl, endsWith('/fake$suffix'));
    });

    test('та же версия → null, тихо', () async {
      expect(await service().check('1.2.0'), isNull);
    });

    test('старый релиз → null', () async {
      expect(await service().check('1.2.0+7'), isNull);
    });

    test('дважды в сутки не проверяем: метка уже стоит', () async {
      final s = service();
      expect(await s.check('1.0.0'), isNotNull);
      // второй вызов (как при перезапуске в тот же день) — сети не трогаем
      expect(await s.check('1.0.0'), isNull);
      expect(stamps.listSync().length, 1); // метка записана
    });

    test('недоступный API → null без исключений', () async {
      final s = UpdateService(
          repo: 'test/k-load',
          apiBase: 'http://127.0.0.1:1', // никто не слушает
          stampDir: stamps.path);
      expect(await s.check('1.0.0'), isNull);
    });

    test('скачивание — во временную папку, прогресс до 1', () async {
      final s = service();
      final info = (await s.check('1.0.0'))!;
      final seen = <double>[];
      final file = await s.download(info, onProgress: seen.add);
      expect(file.path.startsWith(Directory.systemTemp.path), isTrue);
      expect(file.lengthSync(), 1000);
      expect(seen.last, 1.0);
      file.deleteSync();
    });
  });
}
