import 'dart:convert';
import 'dart:io';

/// Обновление приложения через GitHub Releases (публичный репозиторий,
/// без ключей и входа).
///
/// Проверка — один маленький запрос при запуске, не чаще раза в сутки;
/// дата последней проверки хранится рядом с данными приложения. Любая
/// ошибка (нет сети, GitHub недоступен) глотается молча: человек не просил
/// проверять. Скачивание — только по нажатию «Обновить», во временную
/// папку системы (не в Загрузки).
class UpdateInfo {
  const UpdateInfo({
    required this.version,
    required this.assetName,
    required this.assetUrl,
  });

  final String version;   // версия релиза, например '1.1.0'
  final String assetName; // имя установщика, например 'K-LOAD-V1.1-setup.pkg'
  final String assetUrl;  // browser_download_url ассета
}

class UpdateService {
  UpdateService({String? repo, String? apiBase, this.stampDir})
      : repo = repo ??
            Platform.environment['K_LOAD_UPDATE_REPO'] ??
            'americanboy1855/k-load',
        apiBase = apiBase ??
            Platform.environment['K_LOAD_UPDATE_API'] ??
            'https://api.github.com';

  /// Владелец/репозиторий. Env-переопределение — тестовый шов и запас
  /// на случай переезда релизов (прецедент: K_LOAD_TOOLS, K_LOAD_DYLIB).
  final String repo;
  final String apiBase;   // база API (в тестах — локальный сервер)
  final String? stampDir; // куда класть метку проверки (в тестах — tmp)

  HttpClient? _client;

  /// Есть ли релиз новее [currentVersion] («1.0.0+1» из pubspec).
  /// null — обновления нет, проверяться рано или сеть недоступна.
  Future<UpdateInfo?> check(String currentVersion) async {
    if (!_dueToday) return null;
    // Метку ставим ДО сети: сбой не должен долбить API при каждом запуске.
    _markChecked();
    try {
      final body = await _get('$apiBase/repos/$repo/releases/latest');
      final release = jsonDecode(body) as Map<String, dynamic>;
      final latest = parseVersion((release['tag_name'] as String?) ?? '');
      if (latest == null || !isNewer(latest, parseVersion(currentVersion))) {
        return null;
      }
      final asset = pickAsset(
        (release['assets'] as List?) ?? const [],
        platform: Platform.operatingSystem,
      );
      if (asset == null) return null;
      return UpdateInfo(
        version: latest.join('.'),
        assetName: asset.name,
        assetUrl: asset.url,
      );
    } on Object {
      return null; // нет сети / GitHub молчит — и ладно
    }
  }

  /// Скачать установщик во временную папку. [onProgress] — доля 0..1.
  Future<File> download(UpdateInfo info,
      {void Function(double)? onProgress}) async {
    final request =
        await (_client ??= HttpClient()).getUrl(Uri.parse(info.assetUrl));
    request.headers.set(HttpHeaders.userAgentHeader, 'K LOAD');
    final response = await request.close();
    if (response.statusCode != 200) {
      throw HttpException('HTTP ${response.statusCode}',
          uri: Uri.parse(info.assetUrl));
    }
    final file = File('${Directory.systemTemp.path}/${info.assetName}');
    final sink = file.openWrite();
    final total = response.contentLength;
    var got = 0;
    try {
      await for (final chunk in response) {
        got += chunk.length;
        sink.add(chunk);
        if (total > 0) onProgress?.call(got / total);
      }
      await sink.flush();
    } finally {
      await sink.close();
    }
    onProgress?.call(1);
    return file;
  }

  /// Запуск установщика; приложение закрывается — установщик обновит его.
  static Future<void> install(File installer) async {
    if (Platform.isMacOS) {
      await Process.run('open', [installer.path]);
    } else {
      await Process.start(installer.path, const [],
          mode: ProcessStartMode.detached);
    }
    // Дать установщику подняться, затем освободить файлы приложения.
    await Future<void>.delayed(const Duration(milliseconds: 800));
    exit(0);
  }

  // ---- чистые функции (покрываются тестами) ----

  /// 'v1.2.0' / '1.2' / '1.2.0+3' → [1,2,0]; цифр нет — null.
  static List<int>? parseVersion(String raw) {
    var s = raw.trim();
    if (s.startsWith('v') || s.startsWith('V')) s = s.substring(1);
    final plus = s.indexOf('+');
    if (plus >= 0) s = s.substring(0, plus);
    final out = <int>[];
    for (final part in s.split('.')) {
      final n = int.tryParse(part.trim());
      if (n == null) break;
      out.add(n);
    }
    return out.isEmpty ? null : out;
  }

  /// [candidate] строго новее [current]? Свою версию не распознали —
  /// считаем, что обновления нет (молчим, не пристаём).
  static bool isNewer(List<int> candidate, List<int>? current) {
    if (current == null) return false;
    final len =
        candidate.length > current.length ? candidate.length : current.length;
    for (var i = 0; i < len; i++) {
      final c = i < candidate.length ? candidate[i] : 0;
      final k = i < current.length ? current[i] : 0;
      if (c != k) return c > k;
    }
    return false;
  }

  /// Установщик платформы из ассетов релиза: .pkg для macOS, .exe для
  /// Windows; на других платформах обновления не предлагаем.
  static ({String name, String url})? pickAsset(List<dynamic> assets,
      {required String platform}) {
    final suffix = switch (platform) {
      'macos' => '.pkg',
      'windows' => '.exe',
      _ => null,
    };
    if (suffix == null) return null;
    for (final a in assets) {
      if (a is! Map) continue;
      final name = (a['name'] as String?) ?? '';
      final url = (a['browser_download_url'] as String?) ?? '';
      if (name.toLowerCase().endsWith(suffix) && url.isNotEmpty) {
        return (name: name, url: url);
      }
    }
    return null;
  }

  // ---- метка последней проверки ----

  /// GET с UA (GitHub API без него отказывает); тело ответа строкой.
  Future<String> _get(String url) async {
    final request = await (_client ??= HttpClient()).getUrl(Uri.parse(url));
    request.headers.set(HttpHeaders.userAgentHeader, 'K LOAD');
    request.headers.set(HttpHeaders.acceptHeader, 'application/vnd.github+json');
    final response = await request.close();
    if (response.statusCode != 200) {
      throw HttpException('HTTP ${response.statusCode}',
          uri: Uri.parse(url));
    }
    return response.transform(utf8.decoder).join();
  }

  File get _stamp {
    final dir = stampDir ??
        (Platform.isMacOS
            ? '${Platform.environment['HOME']}/Library/Application Support/K LOAD'
            : '${Platform.environment['APPDATA'] ?? Directory.systemTemp.path}/K LOAD');
    return File('$dir/update_check');
  }

  bool get _dueToday {
    try {
      final stamp = _stamp;
      if (!stamp.existsSync()) return true;
      final last = DateTime.parse(stamp.readAsStringSync().trim());
      return DateTime.now().difference(last) >= const Duration(hours: 24);
    } on Object {
      return true;
    }
  }

  void _markChecked() {
    try {
      final stamp = _stamp;
      stamp.parent.createSync(recursive: true);
      stamp.writeAsStringSync(DateTime.now().toIso8601String());
    } on Object {
      // Записать не смогли — просто будем проверять чаще. Не страшно.
    }
  }
}
