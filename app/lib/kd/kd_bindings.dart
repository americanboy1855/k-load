import 'dart:async';
import 'dart:convert';
import 'dart:ffi';
import 'dart:io';
import 'dart:isolate';

import 'package:ffi/ffi.dart';

// Прямая обвязка C-API ядра kdcore (include/kd_capi.h). Все строки — UTF-8,
// сложные ответы — JSON. События (queue_changed / probe / vpn) приходят на
// Dart-порт через Dart_PostCObject.

typedef _AttachDartNative = IntPtr Function(Pointer<Void>);
typedef _AttachDart = int Function(Pointer<Void>);

typedef _CreateNative = Pointer<Void> Function(Pointer<Utf8>);
typedef _Create = Pointer<Void> Function(Pointer<Utf8>);

typedef _VoidEngineNative = Void Function(Pointer<Void>);
typedef _VoidEngineDart = void Function(Pointer<Void>);

typedef _SetPortNative = Int32 Function(Pointer<Void>, Int64);
typedef _SetPort = int Function(Pointer<Void>, int);

typedef _StringEngineNative = Pointer<Utf8> Function(Pointer<Void>);
typedef _StringEngine = Pointer<Utf8> Function(Pointer<Void>);

typedef _StringStringNative = Pointer<Utf8> Function(Pointer<Utf8>);
typedef _StringString = Pointer<Utf8> Function(Pointer<Utf8>);

typedef _EnqueueNative = Int32 Function(Pointer<Void>, Pointer<Utf8>, Pointer<Utf8>);
typedef _Enqueue = int Function(Pointer<Void>, Pointer<Utf8>, Pointer<Utf8>);

typedef _IdNative = Void Function(Pointer<Void>, Int32);
typedef _Id = void Function(Pointer<Void>, int);

typedef _IntEngineNative = Int32 Function(Pointer<Void>);
typedef _IntEngine = int Function(Pointer<Void>);

typedef _VoidEngineStringNative = Void Function(Pointer<Void>, Pointer<Utf8>);
typedef _VoidEngineStringDart = void Function(Pointer<Void>, Pointer<Utf8>);
typedef _VoidPtrNative = Void Function(Pointer<Utf8>);
typedef _VoidPtrDart = void Function(Pointer<Utf8>);
typedef _CStringNative = Pointer<Utf8> Function();
typedef _CStringDart = Pointer<Utf8> Function();

typedef _StringEngineStringNative = Pointer<Utf8> Function(Pointer<Void>, Pointer<Utf8>);
typedef _StringEngineString = Pointer<Utf8> Function(Pointer<Void>, Pointer<Utf8>);

typedef _ProbeSourceNative = Void Function(Pointer<Void>, Pointer<Utf8>, Pointer<Utf8>);
typedef _ProbeSourceDart = void Function(Pointer<Void>, Pointer<Utf8>, Pointer<Utf8>);

typedef _ThumbPathNative = Pointer<Utf8> Function(Pointer<Void>, Pointer<Utf8>);
typedef _ThumbPathDart = Pointer<Utf8> Function(Pointer<Void>, Pointer<Utf8>);

typedef _PredictNative = Pointer<Utf8> Function(Pointer<Void>, Pointer<Utf8>);
typedef _PredictDart = Pointer<Utf8> Function(Pointer<Void>, Pointer<Utf8>);

class KdBindings {
  KdBindings._(this.lib) {
    _attach = lib
        .lookup<NativeFunction<_AttachDartNative>>('kd_attach_dart')
        .asFunction();
    _create = lib
        .lookup<NativeFunction<_CreateNative>>('kd_engine_create')
        .asFunction();
    _destroy = lib
        .lookup<NativeFunction<_VoidEngineNative>>('kd_engine_destroy')
        .asFunction();
    _setPort = lib
        .lookup<NativeFunction<_SetPortNative>>('kd_set_event_port')
        .asFunction();
    _snapshot = lib
        .lookup<NativeFunction<_StringEngineNative>>('kd_snapshot')
        .asFunction();
    _splitLinks = lib
        .lookup<NativeFunction<_StringStringNative>>('kd_split_links')
        .asFunction();
    _enqueueBatch = lib
        .lookup<NativeFunction<_EnqueueNative>>('kd_enqueue_batch')
        .asFunction();
    _enqueuePhoto = lib
        .lookup<NativeFunction<_EnqueueNative>>('kd_enqueue_photo')
        .asFunction();
    _cancel = lib
        .lookup<NativeFunction<_IdNative>>('kd_cancel')
        .asFunction();
    _remove = lib
        .lookup<NativeFunction<_IdNative>>('kd_remove')
        .asFunction();
    _clearFinished = lib
        .lookup<NativeFunction<_VoidEngineNative>>('kd_clear_finished')
        .asFunction();
    _setPaused = lib
        .lookup<NativeFunction<_IdNative>>('kd_set_paused')
        .asFunction();
    _isPaused = lib
        .lookup<NativeFunction<_IntEngineNative>>('kd_is_paused')
        .asFunction();
    _retryNetworkFailed = lib
        .lookup<NativeFunction<_IntEngineNative>>('kd_retry_network_failed')
        .asFunction();
    _probeBlocking = lib
        .lookup<NativeFunction<_StringEngineStringNative>>('kd_probe_blocking')
        .asFunction();
    _probeAsync = lib
        .lookup<NativeFunction<_VoidEngineStringNative>>('kd_probe_async')
        .asFunction();
    _probeAsyncSource = lib
        .lookup<NativeFunction<_ProbeSourceNative>>('kd_probe_async_source')
        .asFunction();
    _thumbPath = lib
        .lookup<NativeFunction<_ThumbPathNative>>('kd_thumb_path')
        .asFunction();
    _predictFiles = lib
        .lookup<NativeFunction<_PredictNative>>('kd_predict_files')
        .asFunction();
    _vpnState = lib
        .lookup<NativeFunction<_IntEngineNative>>('kd_vpn_state')
        .asFunction();
    _defaultDest = lib
        .lookup<NativeFunction<_StringEngineNative>>('kd_default_dest')
        .asFunction();
    _toolsStatus = lib
        .lookup<NativeFunction<_StringEngineNative>>('kd_tools_status')
        .asFunction();
    _stringFree = lib
        .lookup<NativeFunction<_VoidPtrNative>>('kd_string_free')
        .asFunction();
    _version = lib
        .lookup<NativeFunction<_CStringNative>>('kd_version')
        .asFunction();
  }


  static KdBindings? _instance;

  /// Открывает libkdcore: сначала явный путь из K_LOAD_DYLIB, затем рядом
  /// с бандлом, затем сборка ядра в дереве проекта (разработка).
  static KdBindings open() {
    if (_instance != null) return _instance!;
    // Проект может лежать на iCloud-синхронизируемом рабочем столе, поэтому
    // пути считаем от бинаря приложения, а не от текущей директории.
    final exeDir = File(Platform.resolvedExecutable).parent.path;
    final root9 = List.filled(9, '..').join('/');
    final candidates = <String>[
      if (Platform.environment['K_LOAD_DYLIB'] != null)
        Platform.environment['K_LOAD_DYLIB']!,
      '$exeDir/libkdcore.dylib',
      '$exeDir/$root9/kdcore/build/libkdcore.dylib',
      'kdcore/build/libkdcore.dylib',
    ];
    Object? lastError;
    for (final path in candidates) {
      try {
        final dylib = DynamicLibrary.open(path);
        _instance = KdBindings._(dylib);
        return _instance!;
      } on Object catch (e) {
        lastError = e;
      }
    }
    throw StateError(
        'libkdcore.dylib не найдена (пути: $candidates): $lastError\n'
        'Собери ядро: cd kdcore && cmake -B build && cmake --build build');
  }

  final DynamicLibrary lib;

  late final _AttachDart _attach;
  late final _Create _create;
  late final _VoidEngineDart _destroy;
  late final _SetPort _setPort;
  late final _StringEngine _snapshot;
  late final _StringString _splitLinks;
  late final _Enqueue _enqueueBatch;
  late final _Enqueue _enqueuePhoto;
  late final _Id _cancel;
  late final _Id _remove;
  late final _VoidEngineDart _clearFinished;
  late final _Id _setPaused;
  late final _IntEngine _isPaused;
  late final _IntEngine _retryNetworkFailed;
  late final _StringEngineString _probeBlocking;
  late final _VoidEngineStringDart _probeAsync;
  late final _ProbeSourceDart _probeAsyncSource;
  late final _ThumbPathDart _thumbPath;
  late final _PredictDart _predictFiles;
  late final _IntEngine _vpnState;
  late final _StringEngine _defaultDest;
  late final _StringEngine _toolsStatus;
  late final _VoidPtrDart _stringFree;
  late final _CStringDart _version;

  int attachDart() => _attach(NativeApi.initializeApiDLData);

  String version() => _version().toDartString();

  String _take(Pointer<Utf8> p) {
    final s = p.toDartString();
    _stringFree(p);
    return s;
  }
}

/// События ядра: очередь изменилась / пришёл разбор ссылки / сменился VPN.
sealed class KdEvent {
  KdEvent();
  factory KdEvent.fromJson(Map<String, dynamic> j) {
    switch (j['type']) {
      case 'probe':
        return KdProbeEvent(j);
      case 'vpn':
        return KdVpnEvent(j['state'] == 'on');
      default:
        return KdQueueChanged();
    }
  }
}

class KdQueueChanged extends KdEvent {}

class KdVpnEvent extends KdEvent {
  KdVpnEvent(this.on);
  final bool on;
}

class KdProbeEvent extends KdEvent {
  KdProbeEvent(this.json);
  final Map<String, dynamic> json;

  bool get ok => json['ok'] == true;
  String get title => json['title'] ?? '';
  String get serviceTitle => json['serviceTitle'] ?? '';
  String get error => json['error'] ?? '';
  String get link => json['link'] ?? '';
  String get resolved => json['resolved'] ?? '';
  String get thumbnail => json['thumbnail'] ?? '';
  String get uploader => json['uploader'] ?? '';
  int get duration => (json['duration'] ?? 0) as int;
  int get count => (json['count'] ?? 1) as int;
  bool get isPlaylist => json['isPlaylist'] == true;
  bool get hasPlaylist => json['hasPlaylist'] == true;
  bool get isPhoto => json['isPhoto'] == true;
  bool get isSearch => json['isSearch'] == true;
  List<int> get heights => [
    for (final h in (json['heights'] ?? []) as List) (h as num).toInt(),
  ];
  bool get drm => json['drm'] == true;
  bool get shortVideo => json['shortVideo'] == true;
  List<Map<String, dynamic>> get entries => [
    for (final e in (json['entries'] ?? []) as List)
      Map<String, dynamic>.from(e as Map),
  ];
  int get service => (json['service'] ?? 0) as int;
}

/// Подпись источника в строке результатов поиска.
String serviceLabel(int code) {
  switch (code) {
    case 0: return 'YOUTUBE';
    case 1: return 'YT MUSIC';
    case 2: return 'INSTAGRAM';
    case 3: return 'TIKTOK';
    case 4: return 'PINTEREST';
    case 5: return 'ВКОНТАКТЕ';
    case 6: return 'SPOTIFY';
    case 7: return 'APPLE MUSIC';
    case 8: return 'ВК МУЗЫКА';
    case 9: return 'SOUNDCLOUD';
    default: return 'САЙТ';
  }
}

/// Снимок задания очереди из kd_snapshot.
class KdItem {
  KdItem(this.json);
  final Map<String, dynamic> json;

  int get id => json['id'] as int;
  String get title => (json['title'] ?? '') as String;
  String get link => (json['link'] ?? '') as String;
  String get stage => (json['stage'] ?? '') as String;
  String get state => (json['state'] ?? '') as String;
  String get serviceTitle => (json['serviceTitle'] ?? '') as String;
  double get progress => (json['progress'] as num?)?.toDouble() ?? 0;
  bool get isAudio => json['isAudio'] == true;
  bool get isPhoto => json['isPhoto'] == true;
  String get audioFormat => (json['audioFormat'] ?? 'mp3') as String;
  String get container => (json['container'] ?? 'mp4') as String;
  String get sections => (json['sections'] ?? '') as String;
  int get maxHeight => (json['maxHeight'] ?? 0) as int;
  int get itemIndex => (json['itemIndex'] ?? 0) as int;
  int get itemTotal => (json['itemTotal'] ?? 1) as int;
  int get batchIndex => (json['batchIndex'] ?? 0) as int;
  int get batchTotal => (json['batchTotal'] ?? 0) as int;
  List<String> get files =>
      ((json['files'] ?? []) as List).cast<String>();
  String get dest => (json['dest'] ?? '') as String;
}

/// Один движок K LOAD: события — в Stream, состояние — снапшотами.
class KdCore {
  KdCore._(this._b, this._engine) {
    _recv = RawReceivePort(_onEvent);
    _b._setPort(_engine, _recv.sendPort.nativePort);
  }

  static KdCore? _instance;

  /// Открывает ядро и поднимает поток событий.
  static KdCore start() {
    if (_instance != null) return _instance!;
    final b = KdBindings.open();
    if (b.attachDart() != 0) {
      throw StateError('Dart API DL не инициализировалась');
    }
    _instance = KdCore._(b, b._create(nullptr));
    return _instance!;
  }

  final KdBindings _b;
  final Pointer<Void> _engine;
  late final RawReceivePort _recv;

  final StreamController<KdEvent> _events = StreamController<KdEvent>.broadcast();

  /// Поток событий ядра: queue_changed / probe / vpn.
  Stream<KdEvent> get events => _events.stream;

  String get version => _b.version();

  void _onEvent(dynamic message) {
    if (message is String) {
      try {
        _events.add(KdEvent.fromJson(jsonDecode(message) as Map<String, dynamic>));
      } on FormatException {
        _events.add(KdQueueChanged());
      }
    }
  }

  List<KdItem> snapshot() {
    final s = _b._take(_b._snapshot(_engine));
    final arr = jsonDecode(s) as List;
    return [for (final e in arr) KdItem(e as Map<String, dynamic>)];
  }

  List<String> splitLinks(String text) {
    final t = text.toNativeUtf8();
    final out = jsonDecode(_b._take(_b._splitLinks(t))) as List;
    calloc.free(t);
    return out.cast<String>();
  }

  int enqueueBatch(List<String> links,
      {String? dest, bool audio = false, String quality = 'best',
       String audioFormat = 'mp3', int wholePlaylist = -1, String? nameOverride,
       String? sections, int playlistLimit = 0, String container = 'mp4',
       String imageFormat = 'jpg', int durationHint = 0,
       bool forceOverwrite = false}) {
    final linksJson = jsonEncode(links).toNativeUtf8();
    final opts = jsonEncode({
      if (dest != null) 'dest': dest,
      'mode': audio ? 'audio' : 'video',
      'quality': quality,
      'audioFormat': audioFormat,
      'wholePlaylist': wholePlaylist,
      if (nameOverride != null) 'nameOverride': nameOverride,
      if (sections != null && sections.isNotEmpty) 'sections': sections,
      if (playlistLimit > 0) 'playlistLimit': playlistLimit,
      'container': container,
      'imageFormat': imageFormat,
      if (durationHint > 0) 'durationHint': durationHint,
      if (forceOverwrite) 'forceOverwrite': true,
    }).toNativeUtf8();
    final n = _b._enqueueBatch(_engine, linksJson, opts);
    calloc
      ..free(linksJson)
      ..free(opts);
    return n;
  }

  int enqueuePhoto(String link, {String? dest, String imageFormat = 'jpg'}) {
    final l = link.toNativeUtf8();
    final o = jsonEncode({
      if (dest != null) 'dest': dest,
      'imageFormat': imageFormat,
    }).toNativeUtf8();
    final n = _b._enqueuePhoto(_engine, l, o);
    calloc
      ..free(l)
      ..free(o);
    return n;
  }

  void probeAsync(String text, {String? source}) {
    final t = text.toNativeUtf8();
    if (source == null || source.isEmpty || source == 'auto') {
      _b._probeAsync(_engine, t);
    } else {
      final src = source.toNativeUtf8();
      _b._probeAsyncSource(_engine, t, src);
      calloc.free(src);
    }
    calloc.free(t);
  }

  /// Адрес движка: для вызовов ядра из фонового изолята (превью).
  int get address => _engine.address;

  /// Локальный файл превью из кэша ядра; '' — превью нет. Блокирующий:
  /// первый вызов скачивает, поэтому гонять в Isolate.run.
  static String thumbPath(int engineAddr, String url) {
    if (url.isEmpty) return '';
    final b = KdBindings.open(); // библиотека уже загружена процессом
    final u = url.toNativeUtf8();
    final r = b._thumbPath(Pointer<Void>.fromAddress(engineAddr), u);
    final out = r.toDartString();
    calloc.free(u);
    b._stringFree(r);
    return out;
  }

  /// То же в фоновом изоляте. Метод статический нарочно: замыкание,
  /// созданное в контексте State, тянет за собой `this` с деревом виджетов
  /// и отказывается быть «sendable».
  static Future<String> thumbPathAsync(int engineAddr, String url) async {
    return Isolate.run(() => thumbPath(engineAddr, url));
  }

  Map<String, dynamic> probeBlocking(String text) {
    final t = text.toNativeUtf8();
    final r = jsonDecode(_b._take(_b._probeBlocking(_engine, t)))
        as Map<String, dynamic>;
    calloc.free(t);
    return r;
  }

  /// Предсказание итоговых файлов по папке назначения: заявки описываются
  /// теми же значениями, что уйдут в очередь (см. kd_capi.h). Отвечает
  /// списком {"i":…,"path":…,"name":…,"exists":…} — источник правды для
  /// окна «этот файл уже скачан». Без сети: заголовки уже у карточки.
  List<Map<String, dynamic>> predictFiles(List<Map<String, dynamic>> files) {
    final req = jsonEncode({'files': files}).toNativeUtf8();
    final raw = _b._take(_b._predictFiles(_engine, req));
    calloc.free(req);
    final out = jsonDecode(raw) as Map<String, dynamic>;
    return [for (final r in (out['results'] ?? []) as List)
      Map<String, dynamic>.from(r as Map)];
  }

  void cancel(int id) => _b._cancel(_engine, id);
  void remove(int id) => _b._remove(_engine, id);
  void clearFinished() => _b._clearFinished(_engine);


  /// Глобальная пауза очереди: текущие загрузки останавливаются (.part
  /// сохраняется), следующие задания не подаются до setPaused(false).
  void setPaused(bool paused) => _b._setPaused(_engine, paused ? 1 : 0);
  bool isPaused() => _b._isPaused(_engine) != 0;

  /// Повтор сетевых сбоев очереди после восстановления VPN.
  int retryNetworkFailed() => _b._retryNetworkFailed(_engine);
  int vpnState() => _b._vpnState(_engine);
  Map<String, dynamic> defaultDest() =>
      jsonDecode(_b._take(_b._defaultDest(_engine))) as Map<String, dynamic>;

  Map<String, dynamic> toolsStatus() =>
      jsonDecode(_b._take(_b._toolsStatus(_engine))) as Map<String, dynamic>;
}
