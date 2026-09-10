import 'dart:async';
import 'dart:io';
import 'dart:ui' as ui;
import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_svg/flutter_svg.dart';

import '../kd/kd_bindings.dart';
import 'theme.dart';
import 'widgets.dart';

const tvW = 560.0, tvH = 670.0;

// Классификатор сервисов — зеркально design/main-screen.html.
class ServiceInfo {
  const ServiceInfo(this.id, this.name, {this.music = false, this.playlist = false});
  final String id;
  final String name;
  final bool music;
  final bool playlist;
}

ServiceInfo serviceOf(String link) {
  final l = link.toLowerCase();
  if (RegExp(r'music\.youtube\.com').hasMatch(l)) {
    return const ServiceInfo('YTX', 'YT MUSIC', music: true);
  }
  if (RegExp(r'youtube\.com|youtu\.be').hasMatch(l)) {
    return RegExp(r'list=|/playlist').hasMatch(l)
        ? const ServiceInfo('YTP', 'YOUTUBE', playlist: true)
        : const ServiceInfo('YT', 'YOUTUBE');
  }
  if (RegExp(r'instagram\.com').hasMatch(l)) return const ServiceInfo('IG', 'INSTAGRAM');
  if (RegExp(r'tiktok\.com').hasMatch(l)) return const ServiceInfo('TT', 'TIKTOK');
  if (RegExp(r'pinterest\.|pin\.it').hasMatch(l)) return const ServiceInfo('PIN', 'PINTEREST');
  if (RegExp(r'vk\.com|vkvideo\.ru').hasMatch(l)) return const ServiceInfo('VK', 'ВКОНТАКТЕ');
  if (RegExp(r'open\.spotify\.com').hasMatch(l)) return const ServiceInfo('SP', 'SPOTIFY', music: true);
  if (RegExp(r'music\.apple\.com').hasMatch(l)) return const ServiceInfo('AM', 'APPLE MUSIC', music: true);
  if (RegExp(r'music\.yandex\.ru').hasMatch(l)) return const ServiceInfo('YM', 'ЯНДЕКС МУЗЫКА', music: true);
  if (RegExp(r'soundcloud\.com').hasMatch(l)) return const ServiceInfo('SC', 'SOUNDCLOUD', music: true);
  return const ServiceInfo('WEB', 'САЙТ');
}

String fmtDur(int s) {
  if (s <= 0) return '';
  final m = s ~/ 60, ss = (s % 60).toString().padLeft(2, '0');
  final h = m ~/ 60;
  return h > 0 ? '$h:${(m % 60).toString().padLeft(2, '0')}:$ss' : '$m:$ss';
}

/// «10», «1:07», «1:02:03» → нормальный таймкод; мусор → ''.
String normTC(String v) {
  v = v.trim();
  if (v.isEmpty || !RegExp(r'^[\d:]+$').hasMatch(v)) return '';
  var total = 0;
  for (final part in v.split(':')) {
    final n = int.tryParse(part);
    if (n == null) return '';
    total = total * 60 + n;
  }
  final h = total ~/ 3600, m = (total % 3600) ~/ 60, ss = total % 60;
  final mm = m.toString().padLeft(2, '0'), s2 = ss.toString().padLeft(2, '0');
  return h > 0 ? '$h:$mm:$s2' : '$m:$s2';
}

enum Phase { idle, seeking, found }

class KLoadScreen extends StatefulWidget {
  const KLoadScreen({super.key});

  @override
  State<KLoadScreen> createState() => _KLoadScreenState();
}

class _KLoadScreenState extends State<KLoadScreen> with TickerProviderStateMixin {
  KdCore? core;
  StreamSubscription<KdEvent>? sub;

  // телевизор
  bool booted = false;
  bool beamStarted = false;
  late final AnimationController beamC =
      AnimationController(vsync: this, duration: const Duration(milliseconds: 1500));
  late final AnimationController trackC =
      AnimationController(vsync: this, duration: const Duration(milliseconds: 3200));
  bool tracking = false;
  Timer? trackTimer;

  // vpn
  int vpnState = 0; // 0 неизвестно, 1 вкл, 2 выкл
  bool vpnDismissed = false;
  int vpnEpoch = 0; // каждое новое «выключился» — новый glitch на плашке

  // поиск
  final query = TextEditingController();
  final searchFocus = FocusNode();
  bool searchFocused = false;
  bool searchHover = false;
  Phase phase = Phase.idle;
  Timer? debounce;
  Timer? seekAnim;
  int seekPct = 0;

  // карточка
  KdProbeEvent? probe;
  bool isBatch = false;
  int batchCount = 0;
  List<String> batchLinks = const [];
  bool mediaMode = false; // в пачке чип ВИДЕО превращается в МЕДИА
  bool musicOnly = false; // музыкальный сервис: видео оттуда не скачать
  String mode = 'video'; // video | music
  bool chronOn = false;   // режим активен — отрезок уйдёт в загрузку
  bool chronOpen = false; // панель ОТ/ДО раскрыта (не влияет на режим)
  bool chronLocked = false;
  final chronFrom = TextEditingController(text: '0:00');
  final chronTo = TextEditingController(text: '0:10');

  // результаты текстового поиска и выбранный (полный разбор по ссылке)
  List<Map<String, dynamic>> searchResults = const [];
  String? selectedResultUrl;
  bool showResultList = false;
  String? resultError; // причина, почему результат недоступен
  // одноразовый догруз хронометража для текущей ссылки
  bool durationRetried = false;
  // текстовый запрос в поиске: неопределённая шкала «ИЩЕМ...» вместо %
  bool searchingNow = false;
  // сторожевой таймер разбора: зависший запрос → честная ошибка
  Timer? _probeWatchdog;
  bool clearHover = false;
  // качество видео: реальные высоты (дефолт 1080P); МАКСИМУМ убран
  String quality = '1080';
  bool qualityOpen = false;
  // формат контейнера видео и аудио/фото-формата
  String videoContainer = 'mp4';
  String audioFormat = 'mp3';
  String imageFormat = 'jpg';
  // плейлист: сколько первых роликов качать; 0 — весь
  int playlistLimit = 0;
  final countCtrl = TextEditingController();

  // раскрытая панель: '' | chron | count | quality | vformat | aformat
  String openPanel = '';
  final chipKeys = {
    'chron': GlobalKey(),
    'count': GlobalKey(),
    'quality': GlobalKey(),
  };
  double panelX = 0;

  final _queueKey = GlobalKey(); // верх диспетчера — граница панели результатов

  // пачка: последовательный сбор метаданных (общий хронометраж)
  bool batchProbing = false;
  int batchIdx = 0;
  int batchProcessed = 0;
  int batchDuration = 0;

  // очередь
  List<KdItem> items = [];
  String destFolder = '';
  bool toolsFound = true;

  // тост папки
  String? toastText;
  Timer? toastTimer;

  // системный выбор папки + drag-out скачанных файлов
  static const _native = MethodChannel('kload/native');
  final _rowKeys = <int, GlobalKey>{}; // строки очереди для drag-зон
  final _canvasKey = GlobalKey();      // канвас 560×670 — начало координат зон
  String _lastZonesKey = '';           // подпись последних отосланных зон

  // позиция чипа, под которым раскрыта панель
  final _uiColumnKey = GlobalKey();
  double _chronX = 0;

  String get rawText => query.text.trim();
  bool get vpnVisible => booted && vpnState == 2;
  bool get cardVisible => phase == Phase.found;
  bool get resultsOpen => showResultList && searchResults.isNotEmpty;
  /// Текстовый поиск закончился пусто/ошибкой — показываем панель с
  /// причиной и ПОВТОРИТЬ (повторяет именно поиск).
  bool get searchFailed =>
      phase == Phase.idle &&
      !resultsOpen &&
      probe == null &&
      rawText.isNotEmpty &&
      resultError != null;

  /// Похож ли текст на ссылку (зеркало Detector::looksLikeLink).
  bool _looksLikeLink(String s) {
    final t = s.trim();
    return t.contains('.') && t.contains('/');
  }

  @override
  void initState() {
    super.initState();
    _boot();
    // reduced-motion: включение мгновенное, декоративные слои не запускаем.
    final reduce = WidgetsBinding
        .instance.platformDispatcher.accessibilityFeatures.disableAnimations;
    if (reduce) {
      booted = true;
    } else {
      // Телевизор включается сам: без надписей, ровно один луч кинескопа.
      beamStarted = true;
      beamC.addStatusListener((s) {
        if (s == AnimationStatus.completed) _finishBoot();
      });
      Future.delayed(const Duration(milliseconds: 350), () {
        if (mounted) beamC.forward(from: 0);
      });
      _scheduleTracking();
    }
  }

  Future<void> _boot() async {
    try {
      final c = KdCore.start();
      sub = c.events.listen(_onEvent);
      setState(() {
        core = c;
        items = c.snapshot();
        toolsFound = c.toolsStatus()['found'] == true;
        vpnState = c.vpnState();
        final dest = c.defaultDest();
        destFolder = (dest['folder'] ?? '') as String;
      });
    } on Object catch (e) {
      debugPrint('KD_BOOT ошибка: $e');
    }
  }

  void _onEvent(KdEvent e) {
    final c = core;
    if (c == null) return;
    if (e is KdVpnEvent) {
      final wasOff = vpnState == 2;
      setState(() {
        vpnState = e.on ? 1 : 2;
        if (e.on) vpnDismissed = false;
        // Новое появление плашки при каждом переходе вкл -> выкл.
        if (wasOff && !e.on) vpnEpoch += 1;
      });
      // VPN вернулся: безопасно повторяем прерванный разбор. Поле,
      // источник, результаты и очередь загрузок не трогаем.
      if (e.on && wasOff && booted) _recoverAfterVpn();
    } else if (e is KdProbeEvent) {
      if (batchProbing) {
        // Пачка: копим хронометраж и переходим к следующей ссылке.
        var dur = 0;
        if (e.ok && !e.isPlaylist) dur = e.duration;
        setState(() {
          batchProcessed += 1;
          batchDuration += dur;
          batchIdx += 1;
        });
        _probeNextBatchLink();
        return;
      }
      _onProbe(e);
    } else {
      setState(() => items = c.snapshot());
      _scheduleDragZones();
    }
  }

  /// VPN снова работает: повторяем разбор, который мог оборваться.
  /// Безопасно: новое поколение разбора отменяет старое, состояние экрана
  /// (запрос, источник, очередь) остаётся как было.
  void _recoverAfterVpn() {
    // Очередь: задания, упавшие по сети, встают обратно (лимит автоповторов).
    core?.retryNetworkFailed();
    setState(() => items = core?.snapshot() ?? items);
    _scheduleDragZones();
    if (!mounted || rawText.isEmpty) return;
    _probeWatchdog?.cancel();
    if (selectedResultUrl != null) {
      // Дозапрашиваем разбор выбранного результата.
      _armProbeWatchdog();
      core?.probeAsync(selectedResultUrl!);
      return;
    }
    if (phase == Phase.seeking || phase == Phase.idle || probe == null || !probe!.ok) {
      _startSeek();
    }
  }

  /// Готовые файлы: сообщаем нативному слою прямоугольники строк, чтобы
  /// зажатием на строке можно было перетащить сам файл в Finder/DAW.
  void _scheduleDragZones() {
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!mounted) return;
      // Зоны в координатах канваса 560×670 — того самого, что масштабирует
      // окно. Нативный слой сам учтёт cover-масштаб и поля телевизора.
      final canvasCtx = _canvasKey.currentContext;
      final canvasBox = canvasCtx?.findRenderObject() as RenderBox?;
      if (canvasBox == null || !canvasBox.attached) return;
      final zones = <Map<String, dynamic>>[];
      var key = '';
      // Под VPN-плашкой drag не работает: нативный слой перекрыл бы её клики.
      if (!(vpnState == 2 && !vpnDismissed)) {
        for (final it in items) {
          // Перетаскивать можно только скачанное: рабочие и битые строки —
          // не зоны. Кнопки (папка/корзина) остаются вне зоны.
          if (it.state != 'done' || it.files.isEmpty) continue;
          final ctx = _rowKeys[it.id]?.currentContext;
          if (ctx == null) continue;
          final box = ctx.findRenderObject() as RenderBox?;
          if (box == null || !box.attached) continue;
          final topLeft = box.localToGlobal(Offset.zero, ancestor: canvasBox);
          // Зона без правых кнопок (папка/корзина остаются кликабельными).
          final w = box.size.width - 84;
          if (w <= 40) continue;
          zones.add({
            'path': it.files.first,
            'x': topLeft.dx,
            'y': topLeft.dy,
            'w': w,
            'h': box.size.height,
          });
          key += '${it.id}:${topLeft.dx.round()}:'
              '${topLeft.dy.round()}:${w.round()}:${box.size.height.round()};';
        }
      }
      // Раскладка не менялась — нативные виды не трогаем.
      if (key == _lastZonesKey) return;
      _lastZonesKey = key;
      _native.invokeMethod('setZones', zones);
    });
  }

  void _scheduleTracking() {
    // Полоса пробегает редко и медленно, как на старом телевизоре.
    trackTimer = Timer(Duration(milliseconds: 18000 + DateTime.now().millisecondsSinceEpoch % 12000), () {
      if (!mounted) return;
      if (booted) {
        setState(() => tracking = true);
        trackC.forward(from: 0).whenComplete(() {
          if (mounted) setState(() => tracking = false);
        });
      }
      _scheduleTracking();
    });
  }

  // ---- boot ----

  void _finishBoot() {
    if (booted) return;
    setState(() => booted = true);
    // VPN-плашка появляется примерно через секунду после включения.
    Future.delayed(const Duration(seconds: 1), () {
      if (mounted) setState(() {});
    });
  }

  // ---- поиск ----

  void _onInputChanged(String _) {
    setState(() {
      if (rawText.isEmpty) {
        phase = Phase.idle;
        probe = null;
        isBatch = false;
        seekAnim?.cancel();
      } else {
        phase = Phase.idle;
        probe = null;
        isBatch = false;
      }
      chronOn = false;
      chronOpen = false;
      openPanel = '';
      quality = '1080';
      qualityOpen = false;
      videoContainer = 'mp4';
      audioFormat = 'mp3';
      imageFormat = 'jpg';
      playlistLimit = 0;
      countCtrl.clear();
      batchProbing = false;
      showResultList = false;
      selectedResultUrl = null;
      searchResults = const [];
      durationRetried = false;
    });
    debounce?.cancel();
    seekAnim?.cancel();
    if (rawText.isNotEmpty) {
      debounce = Timer(const Duration(milliseconds: 600), _startSeek);
    }
  }

  void _startSeek() {
    final links = core?.splitLinks(query.text) ?? const [];
    setState(() {
      seekPct = 0;
      phase = Phase.seeking;
      openPanel = '';
      chronOn = false;
      chronOpen = false;
      quality = '1080';
      playlistLimit = 0;
      countCtrl.clear();
      batchProbing = false;
      batchIdx = 0;
      batchProcessed = 0;
      batchDuration = 0;
      durationRetried = false;
      resultError = null;
      probe = null;
      // Прямой ссылке — никогда не показывать окно текстовых результатов:
      // старая выдача гасится вместе с началом нового разбора.
      showResultList = false;
      // Текстовый запрос ищем с неопределённой шкалой: честное «ИЩЕМ...»
      // вместо процентов, которые добегают раньше выдачи.
      searchingNow = !_looksLikeLink(rawText);
      if (links.length > 1) {
        isBatch = true;
        batchCount = links.length;
        batchLinks = links;
      } else {
        isBatch = false;
      }
    });
    _probeWatchdog?.cancel();
    if (isBatch) {
      // пачка разбирается локально: короткая анимация и карточка
      _animateSeek(() {
        _showBatch();
        _probeNextBatchLink();
      });
    } else {
      _animateSeek(null);
      // Сторожевой таймер — только для разбора ССЫЛКИ: у текстового
      // поиска свои честные состояния (ИЩЕМ... / ошибка поиска).
      if (!searchingNow) _armProbeWatchdog();
      core?.probeAsync(rawText);
    }
  }

  /// Разбор завис (сеть/VPN пропал) — вместо вечной шкалы честная ошибка
  /// с кнопкой ПОВТОРИТЬ. Приезд разбора таймер гасит.
  void _armProbeWatchdog() {
    _probeWatchdog?.cancel();
    _probeWatchdog = Timer(const Duration(seconds: 25), () {
      if (!mounted) return;
      if (selectedResultUrl != null) {
        final url = selectedResultUrl!;
        setState(() {
          selectedResultUrl = null;
          phase = Phase.found;
          seekingFailed(url);
        });
        return;
      }
      if (phase == Phase.seeking && !isBatch) {
        setState(() {
          phase = Phase.found;
          seekingFailed(rawText);
        });
      }
    });
  }

  void seekingFailed(String text) {
    searchingNow = false;
    probe = KdProbeEvent({
      'ok': false,
      'isSearch': false,
      'link': text,
      'error': 'Не удалось получить данные — проверьте сеть и VPN',
    });
  }

  // Пачка: последовательно собираем длительности, очередь событий ядра
  // возвращает разборы по одному.
  void _probeNextBatchLink() {
    if (!mounted || !isBatch) return;
    if (batchIdx >= batchLinks.length) {
      setState(() => batchProbing = false);
      return;
    }
    batchProbing = true;
    core?.probeAsync(batchLinks[batchIdx]);
  }

  void _animateSeek([VoidCallback? onDone]) {
    seekAnim?.cancel();
    seekPct = 0;
    seekAnim = Timer.periodic(const Duration(milliseconds: 70), (t) {
      setState(() {
        seekPct += 3 + DateTime.now().millisecondsSinceEpoch % 8;
        if (seekPct >= 100) {
          seekPct = 100;
          t.cancel();
          if (onDone != null) onDone();
        }
      });
    });
  }

  /// Все ссылки пачки с одного сервиса — показываем его, смешанные — ПАЧКА.
  String get batchBadge {
    final ids = batchLinks.map((l) => serviceOf(l).id).toSet();
    if (ids.length == 1) return serviceOf(batchLinks.first).name;
    return 'ПАЧКА';
  }

  void _showBatch() {
    setState(() {
      phase = Phase.found;
      mediaMode = true;
      musicOnly = false;
      chronLocked = true;
      chronOn = false;
      openPanel = '';
      mode = 'video';
    });
  }

  void _onProbe(KdProbeEvent p) {
    if (!mounted || rawText.isEmpty) return;
    _probeWatchdog?.cancel();

    // Ждём разбор выбранного результата поиска: если трек защищён или
    // недоступен — честно помечаем строку и остаёмся в списке.
    if (selectedResultUrl != null) {
      setState(() {
        if (p.ok) {
          probe = p;
          phase = Phase.found;
          seekPct = 100;
          resultError = null;
          mediaMode = false;
          final effective = p.resolved.isNotEmpty ? p.resolved : p.link;
          final svc = serviceOf(
              effective.startsWith('http') ? effective : rawText);
          musicOnly = svc.music && !p.isPlaylist;
          mode = musicOnly ? 'music' : 'video';
          chronLocked = p.isPlaylist || p.isPhoto || !p.ok;
        } else {
          resultError = p.error.isEmpty
              ? 'Результат недоступен'
              : p.error;
        }
        selectedResultUrl = null;
        searchingNow = false;
      });
      _scheduleDurationRetry();
      return;
    }

    // Текстовый запрос: сразу окно результатов, карточки ещё нет — она
    // появится только после выбора конкретного результата.
    if (p.isSearch) {
      final results = [
        for (final r in (p.json['results'] as List?) ?? [])
          Map<String, dynamic>.from(r as Map),
      ];
      setState(() {
        phase = Phase.idle;
        probe = null;
        seekPct = 100;
        searchingNow = false;
        searchResults = results;
        showResultList = results.isNotEmpty;
        resultError = results.isEmpty
            ? (p.error.isEmpty ? 'По запросу ничего не нашлось' : p.error)
            : null;
      });
      return;
    }

    // Что реально будет качаться: для поиска по названию это resolved
    // (SoundCloud и т.п.), а не сырой текст из поля.
    final effective = (p.resolved.isNotEmpty ? p.resolved : p.link);
    final svc = serviceOf(effective.startsWith('http') ? effective : rawText);
    setState(() {
      probe = p;
      phase = Phase.found;
      seekPct = 100;
      searchingNow = false;
      mediaMode = false;
      // Список результатов обновляется только когда разбор его принёс:
      // переразбор ссылки не должен стирать открытую выдачу.
      final results = [
        for (final r in (p.json['results'] as List?) ?? [])
          Map<String, dynamic>.from(r as Map),
      ];
      if (results.isNotEmpty) searchResults = results;
      // музыкальный сервис: видео оттуда не скачать — сразу плашка МУЗЫКА
      musicOnly = svc.music && !p.isPlaylist;
      mode = musicOnly ? 'music' : 'video';
      chronLocked = p.isPlaylist || p.isPhoto || !p.ok;
      chronOn = false;
      chronOpen = false;
      openPanel = '';
      quality = '1080';
      qualityOpen = false;
      videoContainer = 'mp4';
      audioFormat = 'mp3';
      imageFormat = 'jpg';
      playlistLimit = 0;
    });
    _scheduleDurationRetry();
  }

  /// Хронометраж не приехал с разбором — один раз переспрашиваем через
  /// паузу; карточка обновляется на месте, без перезапуска.
  void _scheduleDurationRetry() {
    final p = probe;
    final text = rawText;
    if (durationRetried || text.isEmpty) return;
    if (p == null || !p.ok || p.duration > 0
        || p.isPlaylist || p.isPhoto || p.drm || p.isSearch) return;
    durationRetried = true;
    Future.delayed(const Duration(seconds: 5), () {
      if (!mounted || rawText != text) return;
      core?.probeAsync(text);
    });
  }

  /// Выбор результата текстового поиска: карточка открывается МГНОВЕННО
  /// из данных строки (название, источник, длительность, плазма-обложка),
  /// а полный разбор — превью, высоты, хронометраж — догружается в неё же.
  /// Никакой ложной шкалы и зависаний: повторные клики игнорируются,
  /// зависший разбор гасит сторожевой таймер.
  void _selectResult(String url, {Map<String, dynamic>? row}) {
    if (core == null) return;
    if (selectedResultUrl != null) return; // разбор уже идёт
    final svc = ((row?['service'] ?? 0) as num).toInt();
    setState(() {
      selectedResultUrl = url;
      resultError = null;
      showResultList = false;
      searchingNow = false;
      probe = KdProbeEvent({
        'ok': true,
        'isSearch': false,
        'link': url,
        'resolved': url,
        'service': svc,
        'serviceTitle': serviceLabel(svc),
        'title': (row?['title'] ?? '') as String,
        'uploader': (row?['uploader'] ?? '') as String,
        'duration': (row?['duration'] ?? 0) as int,
        'count': 1,
      });
      phase = Phase.found;
      seekPct = 100;
      mediaMode = false;
      musicOnly = false;
      mode = 'video';
      chronLocked = false;
      chronOn = false;
      chronOpen = false;
      openPanel = '';
      quality = '1080';
      qualityOpen = false;
      videoContainer = 'mp4';
      audioFormat = 'mp3';
      imageFormat = 'jpg';
      playlistLimit = 0;
      durationRetried = false;
    });
    _armProbeWatchdog();
    core?.probeAsync(url);
  }

  /// Повтор упавшей задачи: та же ссылка, режим и формат.
  void _retryItem(KdItem it) {
    final c = core;
    if (c == null) return;
    c.remove(it.id);
    c.enqueueBatch([it.link],
        audio: it.isAudio,
        quality: it.maxHeight > 0 ? '${it.maxHeight}' : 'best',
        audioFormat: it.audioFormat,
        container: it.container);
    setState(() => items = c.snapshot());
    _scheduleDragZones();
  }

  // ---- скачать ----

  String? get sectionsArg {
    if (!chronOn || chronLocked) return null;
    final from = normTC(chronFrom.text), to = normTC(chronTo.text);
    if (from.isEmpty || to.isEmpty) return null;
    return '$from-$to';
  }

  void _download() {
    final c = core;
    if (c == null) return;
    final secs = sectionsArg;
    final audio = mode == 'music';
    // Ссылки, которые уже стоят в очереди — второй раз не ставим.
    final busy = items
        .where((it) => it.state == 'queued' || it.state == 'working')
        .map((it) => it.link)
        .toSet();
    String targetLink(String fallback) {
      final p = probe;
      for (final candidate in [p?.resolved ?? '', p?.link ?? '']) {
        if (candidate.startsWith('http')) return candidate;
      }
      return fallback;
    }

    final p = probe;
    final playlist = !isBatch && (p?.isPlaylist ?? false);
    final limit = playlist && playlistLimit > 0 ? playlistLimit : 0;

    if (isBatch) {
      final fresh = batchLinks.where((l) => !busy.contains(l)).toList();
      if (fresh.isEmpty) return;
      c.enqueueBatch(fresh,
          audio: audio,
          quality: quality,
          container: videoContainer,
          durationHint: 0);
    } else {
      if (p == null || !p.ok) return;
      // Pinterest-фотография звука не содержит: предупреждаем до запуска,
      // а не ошибкой в диспетчере после.
      if (audio && p.isPhoto) {
        _showToast('ПО ССЫЛКЕ ФОТОГРАФИЯ — АУДИО НЕДОСТУПНО');
        return;
      }
      if (p.isPhoto) {
        final link = targetLink(rawText);
        if (busy.contains(link)) return;
        c.enqueuePhoto(link, imageFormat: imageFormat);
      } else if (playlist) {
        // Плейлист раскладывается на ОТДЕЛЬНЫЕ задачи: у каждого ролика
        // свой статус, прогресс и повтор; ошибка одного не валит остальные.
        final entries = p.entries;
        final title = p.title.isEmpty ? 'Плейлист' : p.title;
        final folder = '$destFolder/${safeFileName(title)}';
        if (entries.isNotEmpty) {
          var i = 0;
          var queued = 0;
          for (final e in entries) {
            i += 1;
            if (limit > 0 && i > limit) break;
            final url = (e['url'] ?? '') as String;
            if (url.isEmpty || busy.contains(url)) continue;
            final t = (e['title'] ?? '') as String;
            c.enqueueBatch([url],
                dest: folder,
                audio: audio,
                nameOverride: '${i.toString().padLeft(2, '0')} - $t',
                quality: quality,
                container: videoContainer,
                audioFormat: audioFormat);
            queued += 1;
          }
          if (queued == 0 && busy.isNotEmpty) return; // всё уже в очереди
        } else {
          // Разбор без списка роликов — прежний путь целиком.
          final link = p.link;
          if (busy.contains(link)) return;
          c.enqueueBatch([link],
              audio: audio,
              wholePlaylist: 1,
              playlistLimit: limit,
              quality: quality,
              container: videoContainer,
              durationHint: p.duration);
        }
      } else if (p.isSearch) {
        // найденный по названию трек: файл называется запросом
        final link = targetLink('');
        if (link.isEmpty || busy.contains(link)) return;
        c.enqueueBatch([link],
            audio: audio,
            sections: secs,
            nameOverride: rawText,
            quality: quality,
            container: videoContainer,
            audioFormat: audioFormat,
            durationHint: p.duration);
      } else {
        final link = targetLink(rawText);
        if (busy.contains(link)) return;
        c.enqueueBatch([link],
            audio: audio,
            sections: secs,
            quality: quality,
            container: videoContainer,
            audioFormat: audioFormat,
            durationHint: p.duration);
      }
    }
    setState(() {
      items = c.snapshot();
      _scheduleDragZones();
    });
  }

  // ---- очередь: действия ----

  Future<void> _openPath(String path) async {
    try {
      await Process.run('open', [path]);
    } on Object catch (e) {
      debugPrint('open $path: $e');
    }
  }

  /// Круглая кнопка папки: выбор папки назначения для будущих скачиваний.
  Future<void> _chooseDestFolder() async {
    String? path;
    try {
      path = await _native.invokeMethod<String>('chooseFolder');
    } on MissingPluginException {
      _showToast('СИСТЕМНЫЙ ВЫБОР ПАПКИ НЕДОСТУПЕН — КАЧАЕМ В ~/Downloads/K LOAD');
      return;
    } on PlatformException catch (e) {
      debugPrint('chooseFolder: $e');
      return;
    }
    if (path == null || path.isEmpty) return; // отменено
    final chosen = path;
    setState(() => destFolder = chosen);
    final short = chosen.replaceFirst(RegExp('^/Users/[^/]+'), '~');
    _showToast('БУДУЩИЕ ЗАГРУЗКИ — В $short');
  }

  void _showToast(String text) {
    setState(() => toastText = text);
    toastTimer?.cancel();
    toastTimer = Timer(const Duration(milliseconds: 2200), () {
      if (mounted) setState(() => toastText = null);
    });
  }

  Future<void> _trashRow(KdItem it) async {
    // Мусорка удаляет файл с диска и строку из списка (спека).
    if (it.state == 'done') {
      for (final f in it.files) {
        try {
          final file = File(f);
          if (await file.exists()) await file.delete();
        } on Object catch (e) {
          debugPrint('удаление $f: $e');
        }
      }
    }
    core?.remove(it.id);
    setState(() => items = core?.snapshot() ?? items);
    _scheduleDragZones();
  }

  // ---- build ----

  @override
  void dispose() {
    sub?.cancel();
    trackTimer?.cancel();
    debounce?.cancel();
    seekAnim?.cancel();
    toastTimer?.cancel();
    _probeWatchdog?.cancel();
    beamC.dispose();
    trackC.dispose();
    query.dispose();
    searchFocus.dispose();
    chronFrom.dispose();
    chronTo.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    // Телевизор занимает окно целиком: масштаб «каверкой» (щелей не бывает),
    // углы корпуса — под системный радиус окна, микрощели исключены.
    if (openPanel.isNotEmpty) _measurePanel(openPanel);
    // Зоны drag-out зависят от раскладки: пересылаем после каждого кадра,
    // чтобы невидимые зоны никогда не оставались на устаревших местах.
    _scheduleDragZones();
    return Scaffold(
      backgroundColor: const Color(0xFF161413),
      body: LayoutBuilder(builder: (context, box) {
        final scale = (box.maxWidth / tvW) > (box.maxHeight / tvH)
            ? box.maxWidth / tvW
            : box.maxHeight / tvH;
        return OverflowBox(
          maxWidth: tvW * scale,
          maxHeight: tvH * scale,
          child: SizedBox(
            width: tvW * scale,
            height: tvH * scale,
            child: FittedBox(
                fit: BoxFit.fill,
                child: SizedBox(
                    key: _canvasKey,
                    width: tvW,
                    height: tvH,
                    child: _tv())),
          ),
        );
      }),
    );
  }

  Widget _tv() {
    return Container(
      clipBehavior: Clip.antiAlias,
      decoration: BoxDecoration(
        borderRadius: BorderRadius.circular(10.5),
        gradient: const LinearGradient(
          begin: Alignment(-0.6, -1),
          end: Alignment(0.7, 1),
          colors: [Color(0xFF2E2A27), Pal.plastic, Color(0xFF1D1B19), Color(0xFF161413)],
          stops: [0, 0.34, 0.78, 1],
        ),
      ),
      child: Column(children: [
        // верхняя рамка: родные кнопки светофора рисует macOS поверх
        const SizedBox(height: 42),
        Expanded(child: _glass()),
        _band(),
      ]),
    );
  }

  Widget _glass() {
    return Padding(
      padding: const EdgeInsets.symmetric(horizontal: 18),
      child: ClipRRect(
        borderRadius: BorderRadius.circular(16),
        child: Container(
          color: Pal.screenBg,
          child: Stack(fit: StackFit.expand, children: [
            const GlassBackdrop(),
            // Интерфейс проявляется плавно только после луча включения.
            AnimatedOpacity(
              opacity: booted ? 1 : 0,
              duration: const Duration(milliseconds: 450),
              curve: Curves.easeOut,
              child: _ui(),
            ),
            GlassVeil(enabled: booted),
            if (tracking)
              AnimatedBuilder(
                  animation: trackC,
                  builder: (context, _) => TrackingBar(progress: trackC.value)),
            // vpn: затемняем и слегка размываем только стекло телевизора;
            // бэнд и логотип остаются кликабельными и не затемняются.
            if (booted && vpnState == 2) ...[
              AnimatedOpacity(
                opacity: vpnDismissed ? 0 : 1,
                duration: const Duration(milliseconds: 220),
                child: IgnorePointer(
                  ignoring: vpnDismissed,
                  child: GestureDetector(
                    behavior: HitTestBehavior.opaque,
                    onTap: () => setState(() => vpnDismissed = true),
                    child: BackdropFilter(
                      filter: ui.ImageFilter.blur(sigmaX: 2.5, sigmaY: 2.5),
                      child: Container(color: const Color(0x94000000)),
                    ),
                  ),
                ),
              ),
              IgnorePointer(
                ignoring: vpnDismissed,
                child: _vpnPlate(),
              ),
            ],
            if (toastText != null) _toast(),
            if (!booted) _bootOverlay(),
          ]),
        ),
      ),
    );
  }

  Widget _ui() {
    return GestureDetector(
      // Клик вне панелей закрывает их (дети перехватывают свои тапы).
      behavior: HitTestBehavior.translucent,
      onTap: () {
        if (openPanel.isNotEmpty || chronOpen || showResultList) {
          setState(() {
            openPanel = '';
            chronOpen = false;
            showResultList = false;
          });
        }
      },
      child: Padding(
        padding: const EdgeInsets.fromLTRB(20, 22, 20, 14),
        child: Column(
          key: _uiColumnKey,
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            _searchRow(),
            if (phase == Phase.seeking) ...[
              const SizedBox(height: 14),
              _seekBlock(),
            ],
            if (resultsOpen) ...[
              // Открытая выдача занимает основную площадь экрана;
              // диспетчер ужимается до двух последних плашек под ней.
              const SizedBox(height: 14),
              Expanded(child: _resultsPanel()),
              if (items.isNotEmpty) ...[
                const SizedBox(height: 10),
                _compactQueue(),
              ],
            ] else if (searchFailed) ...[
              const SizedBox(height: 14),
              _searchErrorPanel(),
            ] else ...[
              if (cardVisible) ...[
                const SizedBox(height: 18),
                _foundCard(),
                if (!(probe?.drm ?? false)) ...[
                  const SizedBox(height: 18),
                  _modesRow(),
                ],
              ],
              if (items.isNotEmpty) ...[
                const SizedBox(height: 14),
                Expanded(child: _queue()),
              ],
            ],
          ],
        ),
      ),
    );
  }

  // ---- поисковая строка ----

  Widget _searchRow() {
    return GestureDetector(
      onTap: () => searchFocus.requestFocus(),
      child: MouseRegion(
        onEnter: (_) => setState(() => searchHover = true),
        onExit: (_) => setState(() => searchHover = false),
        child: AnimatedBuilder(
          animation: Listenable.merge([searchFocus, query]),
          builder: (context, _) {
            final focused = searchFocus.hasFocus;
            // Свечение нарастает и гаснет плавно, без скачков.
            return TweenAnimationBuilder<double>(
              tween: Tween(end: focused ? 1.0 : (searchHover ? .35 : 0.0)),
              duration: const Duration(milliseconds: 320),
              curve: Curves.easeOutCubic,
              builder: (context, t, _) {
                return CustomPaint(
                  foregroundPainter: DashedBorderPainter(
                      color: Color.lerp(Pal.amberSoft,
                          Pal.amber.withValues(alpha: .9), t)!),
                  child: Container(
                    decoration: BoxDecoration(
                      boxShadow: [
                        BoxShadow(
                            color: Pal.amber.withValues(alpha: .18 * t),
                            blurRadius: 13 + 13 * t),
                      ],
                    ),
                    padding: const EdgeInsets.symmetric(
                        horizontal: 16, vertical: 12),
                    constraints: const BoxConstraints(minHeight: 52),
                    child: Row(children: [
                      const ChainIcon(),
                      const SizedBox(width: 10),
                      Expanded(
                        child: Stack(children: [
                          TextField(
                            controller: query,
                            focusNode: searchFocus,
                            onChanged: _onInputChanged,
                            cursorColor: Pal.amber,
                            style: T.mono(14, c: Pal.amber, ls: .04),
                            decoration: const InputDecoration(
                                isCollapsed: true,
                                border: InputBorder.none),
                            autocorrect: false,
                            enableSuggestions: false,
                            keyboardType: TextInputType.url,
                            // Мультистрочность нужна пачкам: вставка
                            // нескольких ссылок сохраняет переводы строк.
                            maxLines: 3,
                            minLines: 1,
                          ),
                          if (rawText.isEmpty)
                            Text(
                              'Вставьте ссылку или напишите название',
                              style: T.mono(12, c: Pal.dim),
                              maxLines: 1,
                              overflow: TextOverflow.ellipsis,
                            ),
                        ]),
                      ),
                      if (rawText.isNotEmpty) ...[
                        const SizedBox(width: 8),
                        _searchClear(),
                      ],
                    ]),
                  ),
                );
              },
            );
          },
        ),
      ),
    );
  }

  /// Крестик очистки: появляется с текстом, гасит поле, выдачу и все
  /// временные состояния поиска. Диспетчер и файлы не трогает.
  Widget _searchClear() {
    return GestureDetector(
      onTap: _clearSearch,
      child: MouseRegion(
        cursor: SystemMouseCursors.click,
        onEnter: (_) => setState(() => clearHover = true),
        onExit: (_) => setState(() => clearHover = false),
        child: TweenAnimationBuilder<double>(
          tween: Tween(end: clearHover ? 1.0 : 0.0),
          duration: const Duration(milliseconds: 220),
          curve: Curves.easeOut,
          builder: (context, t, _) => Container(
            width: 22,
            height: 22,
            alignment: Alignment.center,
            decoration: BoxDecoration(
              shape: BoxShape.circle,
              border: Border.all(
                  color: Color.lerp(Pal.amberFaint, Pal.amber, t)!),
              boxShadow: t > 0.01
                  ? [BoxShadow(
                      color: Pal.amber.withValues(alpha: .25 * t),
                      blurRadius: 8 * t)]
                  : null,
            ),
            child: Text('×',
                style: TextStyle(
                    fontFamily: T.plex,
                    fontSize: 13,
                    fontWeight: FontWeight.w700,
                    height: 1,
                    color: Color.lerp(Pal.dim, Pal.amber, t))),
          ),
        ),
      ),
    );
  }

  void _clearSearch() {
    setState(() {
      query.clear();
      phase = Phase.idle;
      probe = null;
      isBatch = false;
      showResultList = false;
      selectedResultUrl = null;
      searchResults = const [];
      resultError = null;
      durationRetried = false;
      searchingNow = false;
      openPanel = '';
      chronOn = false;
      chronOpen = false;
      clearHover = false;
      seekAnim?.cancel();
      debounce?.cancel();
      _probeWatchdog?.cancel();
    });
  }

  // ---- ПОИСК: LED-шкала ----

  Widget _seekBlock() {
    // Текстовый запрос: честное неопределённое состояние — «ИЩЕМ...» и
    // сегменты, плавно бегающие туда-обратно. Проценты не показываем:
    // они добегают до 100 раньше настоящей выдачи.
    if (searchingNow) {
      return Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
        Row(mainAxisAlignment: MainAxisAlignment.spaceBetween, children: [
          Text('ПОИСК', style: T.ps(10, c: Pal.soft, ls: .2)),
          Text('ИЩЕМ...', style: T.ps(14, c: Pal.amber)),
        ]),
        const SizedBox(height: 9),
        const SweepLedRow(count: 16, cellHeight: 16),
      ]);
    }
    return Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
      Row(mainAxisAlignment: MainAxisAlignment.spaceBetween, crossAxisAlignment: CrossAxisAlignment.end, children: [
        Text('ПОИСК', style: T.ps(10, c: Pal.soft, ls: .2)),
        Text('$seekPct%', style: T.ps(14, c: Pal.amber)),
      ]),
      const SizedBox(height: 9),
      LedRow(count: 16, filled: (seekPct / 6.25).round().clamp(0, 16)),
    ]);
  }

  // ---- НАЙДЕНО ----

  Widget _resultRow(Map<String, dynamic> r) {
    final url = (r['url'] ?? '') as String;
    final err = selectedResultUrl == url && resultError != null
        ? resultError
        : null;
    final dur = (r['duration'] ?? 0) as int;
    final uploader = (r['uploader'] ?? '') as String;
    final source = serviceLabel(((r['service'] ?? 0) as num).toInt());
    return GestureDetector(
      onTap: () {
        setState(() => showResultList = false); // карточка возвращается сама
        _selectResult(url, row: r);
      },
      child: MouseRegion(
        cursor: SystemMouseCursors.click,
        child: AnimatedContainer(
          duration: const Duration(milliseconds: 140),
          padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 5),
          decoration: BoxDecoration(
            color: err != null
                ? Pal.amber.withValues(alpha: .04)
                : Colors.transparent,
            borderRadius: const BorderRadius.all(Radius.circular(2)),
          ),
          child: Row(children: [
            Expanded(
              child: Column(crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                Text((r['title'] ?? '') as String,
                    style: T.mono(12, c: err != null ? Pal.dim : Pal.soft),
                    maxLines: 1,
                    overflow: TextOverflow.ellipsis),
                const SizedBox(height: 1),
                Row(children: [
                  Flexible(
                    child: Text(uploader.toUpperCase(),
                        style: T.ps(7, c: Pal.dim, ls: .04),
                        maxLines: 1, overflow: TextOverflow.ellipsis),
                  ),
                  Text('  ·  $source',
                      style: T.ps(7, c: Pal.amber, ls: .04),
                      maxLines: 1, overflow: TextOverflow.ellipsis),
                ]),
                if (err != null)
                  Text(err.toUpperCase(),
                      style: T.ps(7, c: Pal.error, ls: .02)),
              ]),
            ),
            const SizedBox(width: 8),
            if (dur > 0) Text(fmtDur(dur), style: T.mono(10, c: Pal.dim)),
          ]),
        ),
      ),
    );
  }

  /// Ошибка текстового поиска: причина и повтор именно поиска.
  Widget _searchErrorPanel() {
    return GlitchIn(
      key: const ValueKey('panel-search-error'),
      child: GestureDetector(
        onTap: () {},
        child: Container(
          decoration: const BoxDecoration(color: Color(0xF5070400)),
          child: DashedBox(
            padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 14),
            child: Column(crossAxisAlignment: CrossAxisAlignment.start,
                mainAxisSize: MainAxisSize.min, children: [
              Text('ПОИСК НЕ УДАЛСЯ', style: T.ps(11, c: Pal.soft)),
              const SizedBox(height: 6),
              Text(resultError ?? '',
                  style: T.mono(11, c: Pal.dim)),
              const SizedBox(height: 10),
              GestureDetector(
                onTap: _startSeek,
                child: MouseRegion(
                  cursor: SystemMouseCursors.click,
                  child: Container(
                    padding: const EdgeInsets.symmetric(
                        horizontal: 10, vertical: 5),
                    decoration: BoxDecoration(
                      border: Border.all(color: Pal.amberFaint),
                      borderRadius:
                          const BorderRadius.all(Radius.circular(2)),
                    ),
                    child: Text('ПОВТОРИТЬ', style: T.ps(8, c: Pal.amber)),
                  ),
                ),
              ),
            ]),
          ),
        ),
      ),
    );
  }

  // ---- окно результатов: занимает основную площадь экрана ----

  Widget _resultsPanel() {
    final rows = searchResults.take(20).toList();
    return GlitchIn(
      key: const ValueKey('panel-results'),
      child: GestureDetector(
        // Панель живёт своей жизнью: тап по ней не закрывает её же.
        onTap: () {},
        child: Container(
          decoration: const BoxDecoration(color: Color(0xF5070400)),
          child: DashedBox(
            padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 9),
            child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Row(children: [
                    Text('РЕЗУЛЬТАТЫ · ${searchResults.length}',
                        style: T.ps(8, c: Pal.soft)),
                    const Spacer(),
                    _panelClose(() => setState(() => showResultList = false)),
                  ]),
                  const SizedBox(height: 6),
                  Expanded(
                    child: ListView.builder(
                      padding: EdgeInsets.zero,
                      itemCount: rows.length,
                      itemBuilder: (context, i) => _resultRow(rows[i]),
                    ),
                  ),
                ]),
          ),
        ),
      ),
    );
  }

  /// Крестик закрытия панели: ховер подсвечивает, геометрию не трогает.
  Widget _panelClose(VoidCallback onTap) {
    return GestureDetector(
      onTap: onTap,
      child: MouseRegion(
        cursor: SystemMouseCursors.click,
        child: Padding(
          padding: const EdgeInsets.all(3),
          child: Text('×',
              style: const TextStyle(
                  fontFamily: T.plex,
                  fontSize: 14,
                  fontWeight: FontWeight.w700,
                  height: 1,
                  color: Pal.dim)),
        ),
      ),
    );
  }

  Widget _foundCard() {
    final p = probe;
    if (isBatch) {
      // Пачка: бейдж по общему сервису, вместо категорий — живой хронометраж.
      final svcNames = <String>[];
      for (final l in batchLinks) {
        final n = serviceOf(l).name;
        if (!svcNames.contains(n)) svcNames.add(n);
      }
      return Row(crossAxisAlignment: CrossAxisAlignment.center, children: [
        _SmartCover(url: null, engineAddr: core?.address),
        const SizedBox(width: 15),
        Expanded(
          child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
            _badge(batchBadge, src: null),
            const SizedBox(height: 8),
            Text('$batchCount ССЫЛОК', style: T.ps(13, c: Pal.soft)),
            if (svcNames.length > 1) ...[
              const SizedBox(height: 6),
              Text(svcNames.join('  ·  '),
                  style: T.ps(7, c: Pal.dim, ls: .06)),
            ],
            const SizedBox(height: 6),
            Text(_batchDurLine(), style: T.mono(11, c: Pal.dim, ls: .04)),
          ]),
        ),
      ]);
    }
    if (p == null) return const SizedBox.shrink();
    if (!p.ok && !p.drm) {
      // Разбор не удался (например, VPN мигнул): честная причина и понятный
      // повтор — состояние экрана при этом не сбрасывается.
      return DashedBox(
        color: Pal.amber.withValues(alpha: .35),
        padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 12),
        child: Column(children: [
          Text('РАЗБОР НЕ УДАЛСЯ', style: T.ps(12, c: Pal.soft)),
          const SizedBox(height: 6),
          Text(p.error.isEmpty ? 'Проверь ссылку или сеть' : p.error,
              style: T.mono(11, c: Pal.dim), textAlign: TextAlign.center),
          const SizedBox(height: 9),
          GestureDetector(
            onTap: _startSeek,
            child: MouseRegion(
              cursor: SystemMouseCursors.click,
              child: Container(
                padding:
                    const EdgeInsets.symmetric(horizontal: 10, vertical: 5),
                decoration: BoxDecoration(
                  border: Border.all(color: Pal.amberFaint),
                  borderRadius: const BorderRadius.all(Radius.circular(2)),
                ),
                child: Text('ПОВТОРИТЬ', style: T.ps(8, c: Pal.amber)),
              ),
            ),
          ),
        ]),
      );
    }
    if (p.drm) {
      // Защищённая запись: карточка собирается из oEmbed, вместо режимов —
      // объяснение, почему скачать нельзя.
      return Row(crossAxisAlignment: CrossAxisAlignment.center, children: [
        _SmartCover(url: p.thumbnail.isEmpty ? null : p.thumbnail, engineAddr: core?.address),
        const SizedBox(width: 15),
        Expanded(
          child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
            _badge(p.serviceTitle.toUpperCase(), src: _sourceUrl(p)),
            const SizedBox(height: 8),
            Text(_drmTitle(p), style: T.mono(15), maxLines: 2, overflow: TextOverflow.ellipsis),
            const SizedBox(height: 4),
            Text(p.error.toUpperCase(),
                style: T.ps(9, c: Pal.error, ls: .04)),
          ]),
        ),
      ]);
    }
    final src = _sourceUrl(p);
    return Row(crossAxisAlignment: CrossAxisAlignment.center, children: [
      _SmartCover(url: p.thumbnail.isEmpty ? null : p.thumbnail, engineAddr: core?.address),
      const SizedBox(width: 15),
      Expanded(
        child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
          Row(children: [
            _badge(p.isPlaylist
                    ? 'ПЛЕЙЛИСТ · ${p.serviceTitle.toUpperCase()}'
                    : p.serviceTitle.toUpperCase(),
                src: src),
            // Вернуться к списку результатов текстового поиска: повторное
            // нажатие закрывает панель, крестик в самой панели тоже.
            if (p.isSearch && searchResults.isNotEmpty) ...[
              const SizedBox(width: 9),
              GestureDetector(
                behavior: HitTestBehavior.opaque,
                onTap: () => setState(() {
                  showResultList = !showResultList;
                  resultError = null;
                }),
                child: Padding(
                  padding: const EdgeInsets.symmetric(
                      horizontal: 6, vertical: 6),
                  child: Text(showResultList
                      ? 'ЗАКРЫТЬ РЕЗУЛЬТАТЫ'
                      : 'К РЕЗУЛЬТАТАМ',
                      style: T.ps(7,
                          c: showResultList ? Pal.amber : Pal.dim, ls: .06)),
                ),
              ),
            ],
          ]),
          // Качество/формат: компактный блок под источником.
          const SizedBox(height: 6),
          if (!(p.isSearch && selectedResultUrl == null && searchResults.isNotEmpty))
            _mediaOptions(p),
          const SizedBox(height: 8),
          Text(p.title, style: T.mono(15),
              maxLines: 2, overflow: TextOverflow.ellipsis),
          const SizedBox(height: 4),
          Text(_durLine(p), style: T.mono(11, c: Pal.dim, ls: .04)),
        ]),
      ),
    ]);
  }

  /// Формат и качество: компактный блок под источником.
  /// Видео — формат (MP4/WEBM/MKV) + реальные высоты; аудио — формат
  /// (MP3/M4A/WAV/FLAC/OGG) + подпись «МАКСИМАЛЬНОЕ КАЧЕСТВО»;
  /// фото — JPG/PNG + подпись. TikTok/Instagram — единый простой вид без
  /// списков: MP4/MP3 · МАКСИМАЛЬНОЕ КАЧЕСТВО.
  Widget _mediaOptions(KdProbeEvent p) {
    if (p.isPhoto || !p.ok || p.isPlaylist) return const SizedBox.shrink();

    if (p.shortVideo) {
      final label = mode == 'music' ? 'MP3' : 'MP4';
      return Padding(
        padding: const EdgeInsets.only(top: 2),
        child: Row(children: [
          Container(
            padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
            decoration: BoxDecoration(
              border: Border.all(color: Pal.amberFaint),
              borderRadius: const BorderRadius.all(Radius.circular(2)),
            ),
            child: Text(label, style: T.ps(8, c: Pal.soft)),
          ),
          const SizedBox(width: 8),
          Text('· МАКСИМАЛЬНОЕ КАЧЕСТВО', style: T.ps(7, c: Pal.dim, ls: .04)),
        ]),
      );
    }

    // Аудио: формат + подпись качества.
    if (mode == 'music') {
      return Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
        Row(children: [
          _dropChip(
              label: _audioLabel(audioFormat),
              open: openPanel == 'aformat',
              onTap: () => setState(() =>
                  openPanel = openPanel == 'aformat' ? '' : 'aformat')),
          const SizedBox(width: 8),
          Text('· МАКСИМАЛЬНОЕ КАЧЕСТВО', style: T.ps(7, c: Pal.dim, ls: .04)),
        ]),
        AnimatedSize(
          duration: const Duration(milliseconds: 220),
          curve: Curves.easeOutCubic,
          alignment: Alignment.topLeft,
          child: openPanel == 'aformat'
              ? Padding(
                  padding: const EdgeInsets.only(top: 4),
                  child: GlitchIn(
                    key: const ValueKey('panel-aformat'),
                    child: _optionsPanel(const [
                      ('MP3', 'mp3'),
                      ('M4A', 'm4a'),
                      ('WAV', 'wav'),
                      ('FLAC', 'flac'),
                      ('OGG', 'ogg'),
                    ], audioFormat, (v) => audioFormat = v),
                  ),
                )
              : const SizedBox(width: double.infinity),
        ),
      ]);
    }

    // Видео: формат слева, качество справа (реальные высоты кадра);
    // если высоты недоступны — подпись МАКСИМАЛЬНОЕ КАЧЕСТВО.
    final hasHeights = p.heights.isNotEmpty;
    return Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
      Row(children: [
        _dropChip(
            label: videoContainer.toUpperCase(),
            open: openPanel == 'vformat',
            onTap: () => setState(() =>
                openPanel = openPanel == 'vformat' ? '' : 'vformat')),
        const SizedBox(width: 6),
        if (hasHeights)
          _dropChip(
              label: '$quality P',
              open: openPanel == 'quality',
              onTap: () => setState(() =>
                  openPanel = openPanel == 'quality' ? '' : 'quality'))
        else
          Text('· МАКСИМАЛЬНОЕ КАЧЕСТВО',
              style: T.ps(7, c: Pal.dim, ls: .04)),
      ]),
      AnimatedSize(
        duration: const Duration(milliseconds: 220),
        curve: Curves.easeOutCubic,
        alignment: Alignment.topLeft,
        child: openPanel == 'vformat'
            ? Padding(
                padding: const EdgeInsets.only(top: 4),
                child: GlitchIn(
                  key: const ValueKey('panel-vformat'),
                  child: _optionsPanel(const [
                    ('MP4', 'mp4'),
                    ('WEBM', 'webm'),
                    ('MKV', 'mkv'),
                  ], videoContainer, (v) => videoContainer = v),
                ),
              )
            : openPanel == 'quality' && hasHeights
                ? Padding(
                    padding: const EdgeInsets.only(top: 4),
                    child: GlitchIn(
                      key: const ValueKey('panel-quality'),
                      child: _optionsPanel(
                        [
                          for (final h in p.heights.take(4)) ('$h P', '$h'),
                        ],
                        quality,
                        (v) => quality = v,
                      ),
                    ),
                  )
                : const SizedBox(width: double.infinity),
      ),
    ]);
  }

  String _audioLabel(String v) {
    switch (v) {
      case 'm4a': return 'M4A';
      case 'wav': return 'WAV';
      case 'flac': return 'FLAC';
      case 'ogg': return 'OGG';
      default: return 'MP3';
    }
  }

  /// Раскрывающийся ярлык с шевроном.
  Widget _dropChip({required String label, required bool open, required VoidCallback onTap}) {
    return GestureDetector(
      behavior: HitTestBehavior.opaque,
      onTap: onTap,
      child: MouseRegion(
        cursor: SystemMouseCursors.click,
        child: AnimatedContainer(
          duration: const Duration(milliseconds: 160),
          padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
          decoration: BoxDecoration(
            border: Border.all(color: open ? Pal.amber : Pal.amberFaint),
            borderRadius: const BorderRadius.all(Radius.circular(2)),
            boxShadow: [
              BoxShadow(
                  color: Pal.amber.withValues(alpha: open ? .14 : 0),
                  blurRadius: open ? 8 : 0),
            ],
          ),
          child: Row(mainAxisSize: MainAxisSize.min, children: [
            Text(label, style: T.ps(8, c: Pal.soft)),
            const SizedBox(width: 5),
            Icon(open ? Icons.expand_less : Icons.expand_more,
                size: 9, color: Pal.soft),
          ]),
        ),
      ),
    );
  }

  /// Вертикальный список опций; выбранная строка подсвечена.
  Widget _optionsPanel(
      List<(String, String)> options, String current, ValueChanged<String> onSelect) {
    return _darkPanel(
      child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
        for (final opt in options)
          Padding(
            padding: const EdgeInsets.only(bottom: 2),
            child: GestureDetector(
              onTap: () => setState(() {
                onSelect(opt.$2);
                openPanel = '';
              }),
              child: MouseRegion(
                cursor: SystemMouseCursors.click,
                child: AnimatedContainer(
                  duration: const Duration(milliseconds: 140),
                  padding:
                      const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
                  color:
                      current == opt.$2 ? Pal.amberFaint : Colors.transparent,
                  child: Text(opt.$1,
                      style: T.ps(8,
                          c: current == opt.$2 ? Pal.amber : Pal.soft)),
                ),
              ),
            ),
          ),
      ]),
    );
  }

  /// «BALLISLIFE by INMYWHITEE» → название + исполнитель на карточке.
  String _drmTitle(KdProbeEvent p) {
    final t = p.title;
    final by = t.indexOf(' by ');
    return by > 0 ? t.substring(0, by) : t;
  }

  /// Источник «где нашлось» — первая настоящая ссылка из разбора.
  String _sourceUrl(KdProbeEvent p) {
    for (final candidate in [p.resolved, p.link, rawText]) {
      if (candidate.startsWith('http')) return candidate;
    }
    return '';
  }

  String _batchDurLine() {
    if (batchProbing || batchProcessed < batchLinks.length) {
      return 'ОБРАБОТАНО $batchProcessed ИЗ ${batchLinks.length}';
    }
    if (batchDuration > 0) {
      return 'ХРОНОМЕТРАЖ · ${fmtLongDur(batchDuration)}';
    }
    return 'ХРОНОМЕТРАЖ —';
  }

  String _durLine(KdProbeEvent p) {
    if (p.isPhoto) return 'ФОТОГРАФИЯ';
    if (p.isPlaylist) {
      return '${p.count} ВИДЕО · ХРОНОМЕТРАЖ · ${fmtLongDur(p.duration)}';
    }
    // Длительность не успела приехать — аккуратное состояние загрузки.
    final dur = p.duration > 0 ? ' · ${fmtDur(p.duration)}' : ' · ЗАГРУЗКА…';
    return 'ХРОНОМЕТРАЖ$dur';
  }

  /// 3725 -> '1:02:05', 754 -> '12:34'.
  String fmtLongDur(int totalSec) {
    if (totalSec <= 0) return '—';
    final h = totalSec ~/ 3600, m = (totalSec % 3600) ~/ 60, sec = totalSec % 60;
    final mm = m.toString().padLeft(2, '0'), ss = sec.toString().padLeft(2, '0');
    return h > 0 ? '$h:$mm:$ss' : '$m:$ss';
  }

  Widget _badge(String text, {String? src}) {
    final badge = Container(
      padding: const EdgeInsets.symmetric(horizontal: 9, vertical: 3),
      color: Pal.amber,
      child: Text(text, style: T.ps(8, c: const Color(0xFF0A0500), ls: .04)),
    );
    return Row(children: [
      badge,
      if (src != null && src.startsWith('http')) ...[
        const SizedBox(width: 9),
        GestureDetector(
          // Открываем исходную каноническую ссылку выбранного контента.
          onTap: () => _openPath(src),
          child: MouseRegion(
            cursor: SystemMouseCursors.click,
            child: Text('[ССЫЛКА]',
                style: T.mono(10, c: Pal.dim, ls: .04)),
          ),
        ),
      ],
    ]);
  }

  /// Имя папки/файла без символ_, на которых спотыкается файловая система.
  String safeFileName(String s) {
    final out = s.replaceAll(RegExp(r'[/\\:%"' + "'" + r'\n\r\t]'), ' ').trim();
    return out.isEmpty ? 'Плейлист' : out.substring(0, out.length.clamp(0, 80));
  }

  /// Аккуратный канонический вид ссылки: без схемы, www и tracking-хвоста.
  String _canonicalShort(String url) {
    var u = url.trim();
    u = u.replaceFirst(RegExp(r'^https?://', caseSensitive: false), '');
    u = u.replaceFirst(RegExp(r'^www\.', caseSensitive: false), '');
    // YouTube: watch?v=ID и shorts/ID → короткая форма с ID.
    final yt = RegExp(r'youtube\.com/(?:watch\?v=|shorts/)([\w-]{6,})')
        .firstMatch(u);
    if (yt != null) return 'youtu.be/${yt.group(1)}';
    final q = u.indexOf('?');
    if (q > 0) u = u.substring(0, q);
    while (u.endsWith('/')) {
      u = u.substring(0, u.length - 1);
    }
    const max = 34;
    if (u.length <= max) return u;
    final hostEnd = u.indexOf('/');
    if (hostEnd > 0 && u.length - hostEnd > 6) {
      final tail = u.substring(u.length - 18);
      return '${u.substring(0, hostEnd)}/…/$tail';
    }
    return '${u.substring(0, max)}…';
  }

  // ---- режимы + СКАЧАТЬ ----

  Widget _modesRow() {
    final p = probe;
    final photo = !isBatch && (p?.isPhoto ?? false);
    final playlist = !isBatch && (p?.isPlaylist ?? false);

    // Ярлык СКАЧАТЬ: пачка — количество ссылок, плейлист — выбранное
    // количество роликов (или весь плейлист), одиночное — 1.
    final goCount = isBatch
        ? batchCount
        : playlist
            ? (playlistLimit > 0 ? playlistLimit : (p?.count ?? 1))
            : 1;
    final goLabel = 'СКАЧАТЬ · $goCount';
    final canDownload = isBatch || (p != null && p.ok && !p.drm);
    final videoLabel = mediaMode ? 'МЕДИА' : 'ВИДЕО';

    return Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
      Row(children: [
        _ModeChip(
            label: videoLabel,
            on: mode == 'video' && !musicOnly,
            locked: false,
            hidden: musicOnly || photo,
            onTap: () => setState(() => mode = 'video')),
        _ModeChip(
            label: 'МУЗЫКА',
            on: mode == 'music',
            locked: false,
            hidden: photo,
            onTap: () => setState(() => mode = 'music')),
        if (!playlist)
          _ModeChip(
              key: chipKeys['chron'],
              label: 'ХРОН',
              on: chronOn,
              locked: chronLocked,
              hidden: false,
              onTap: () => setState(() {
                    if (chronLocked) return;
                    chronOn = !chronOn;
                    chronOpen = chronOn;
                    openPanel = chronOpen ? 'chron' : '';
                    if (chronOpen) _measurePanel('chron');
                  })),
        if (playlist)
          _ModeChip(
              key: chipKeys['count'],
              label: 'КОЛ-ВО',
              on: openPanel == 'count',
              locked: false,
              hidden: false,
              onTap: () => setState(() {
                    openPanel = openPanel == 'count' ? '' : 'count';
                    if (openPanel == 'count') _measurePanel('count');
                  })),

        const Spacer(),
        _GoButton(label: goLabel, enabled: canDownload),
      ]),
      // Панели открываются под своим чипом и плавно раздвигают контент.
      AnimatedSize(
        duration: const Duration(milliseconds: 240),
        curve: Curves.easeOutCubic,
        alignment: Alignment.topLeft,
        child: _panelBelow(),
      ),
    ]);
  }

  Widget _panelBelow() {
    final showChron = chronOpen && chronOn && !chronLocked;
    final showCount = openPanel == 'count';
    if (!showChron && !showCount) {
      return const SizedBox(width: double.infinity);
    }
    return Padding(
      padding: EdgeInsets.only(left: _chronX, top: 8),
      child: GlitchIn(
        key: ValueKey('panel-$openPanel-${showChron ? 'c' : 'n'}'),
        child: showChron ? _chronBox() : _countBox(),
      ),
    );
  }

  void _measurePanel(String which) {
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!mounted || openPanel != which) return;
      if (which == 'chron' && !(chronOpen && chronOn)) return;
      final ctx = chipKeys[which]?.currentContext;
      final colCtx = _uiColumnKey.currentContext;
      if (ctx == null || colCtx == null) return;
      final chipBox = ctx.findRenderObject() as RenderBox?;
      final colBox = colCtx.findRenderObject() as RenderBox?;
      if (chipBox == null || colBox == null || !chipBox.attached) return;
      final x = chipBox.localToGlobal(Offset.zero, ancestor: colBox).dx;
      if ((x - _chronX).abs() > 0.5) setState(() => _chronX = x);
    });
  }

  /// КОЛ-ВО: сколько первых роликов плейлиста скачать; пусто — весь.
  /// Число ограничено фактическим количеством роликов.
  Widget _countBox() {
    final max = (!isBatch && probe != null && probe!.isPlaylist && probe!.count > 0)
        ? probe!.count
        : 0;
    return _darkPanel(
      child: Row(mainAxisSize: MainAxisSize.min, children: [
        Text('КОЛ-ВО${max > 0 ? ' · ДО $max' : ''}',
            style: T.ps(8, c: Pal.soft)),
        const SizedBox(width: 8),
        SizedBox(
            width: 64,
            child: TextField(
              controller: countCtrl,
              keyboardType: TextInputType.number,
              inputFormatters: [FilteringTextInputFormatter.digitsOnly],
              textAlign: TextAlign.center,
              cursorColor: Pal.amber,
              onChanged: (v) => setState(() {
                var n = int.tryParse(v) ?? 0;
                if (max > 0 && n > max) n = max;
                playlistLimit = n;
              }),
              onSubmitted: (_) => FocusScope.of(context).unfocus(),
              style: T.mono(13, c: Pal.amber),
              decoration: const InputDecoration(
                  isCollapsed: true, border: InputBorder.none),
            )),
      ]),
    );
  }

  /// ХРОН: панель с ОТ/ДО.
  Widget _chronBox() {
    return _darkPanel(
      child: Row(mainAxisSize: MainAxisSize.min, children: [
        Text('ОТ', style: T.ps(8, c: Pal.soft)),
        const SizedBox(width: 6),
        SizedBox(
            width: 56,
            child: TextField(
              controller: chronFrom,
              textAlign: TextAlign.center,
              cursorColor: Pal.amber,
              onSubmitted: (_) => FocusScope.of(context).unfocus(),
              style: T.mono(13, c: Pal.amber),
              decoration: const InputDecoration(
                  isCollapsed: true, border: InputBorder.none),
            )),
        const SizedBox(width: 6),
        Text('ДО', style: T.ps(8, c: Pal.soft)),
        const SizedBox(width: 6),
        SizedBox(
            width: 56,
            child: TextField(
              controller: chronTo,
              textAlign: TextAlign.center,
              cursorColor: Pal.amber,
              onSubmitted: (_) => FocusScope.of(context).unfocus(),
              style: T.mono(13, c: Pal.amber),
              decoration: const InputDecoration(
                  isCollapsed: true, border: InputBorder.none),
            )),
      ]),
    );
  }

  Widget _darkPanel({required Widget child}) {
    return Container(
      decoration: const BoxDecoration(color: Color(0xF5070400)),
      child: DashedBox(
        padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 9),
        child: child,
      ),
    );
  }

  // ---- очередь ----

  Widget _queue() {
    return Container(
      key: _queueKey,
      decoration: const BoxDecoration(
        border: Border(top: BorderSide(color: Pal.amberFaint), bottom: BorderSide(color: Pal.amberFaint)),
      ),
      child: CustomPaint(
        foregroundPainter: const DashedBorderPainter(color: Color(0x80FFB000)),
        child: Column(children: [
          _queueHeader(),
          Expanded(
            child: NotificationListener<ScrollNotification>(
              // Прокрутка двигает строки — зоны drag-out должны ехать следом.
              onNotification: (_) {
                _scheduleDragZones();
                return false;
              },
              child: ListView.builder(
                padding: EdgeInsets.zero,
                itemCount: items.length,
                itemBuilder: (context, i) => _queueRow(items[i]),
              ),
            ),
          ),
        ]),
      ),
    );
  }

  /// Компактный диспетчер под открытой выдачей: максимум две последние
  /// плашки; после закрытия результатов диспетчер возвращается к обычной
  /// высоте (см. _ui).
  Widget _compactQueue() {
    return SizedBox(
      height: 138,
      child: Container(
        decoration: const BoxDecoration(
          border: Border(top: BorderSide(color: Pal.amberFaint), bottom: BorderSide(color: Pal.amberFaint)),
        ),
        child: CustomPaint(
          foregroundPainter:
              const DashedBorderPainter(color: Color(0x80FFB000)),
          child: Column(children: [
            _queueHeader(),
            Expanded(
              child: ListView.builder(
                padding: EdgeInsets.zero,
                itemCount: math.min(2, items.length),
                itemBuilder: (context, i) => _queueRow(items[i]),
              ),
            ),
          ]),
        ),
      ),
    );
  }

  Widget _queueHeader() {
    return Padding(
      padding: const EdgeInsets.fromLTRB(13, 6, 13, 2),
      child: Row(children: [
        Text('ДИСПЕТЧЕР ЗАГРУЗОК', style: T.ps(9, c: Pal.soft, ls: .1)),
      ]),
    );
  }

  Widget _queueRow(KdItem it) {
    final done = it.state == 'done';
    final rowKey = _rowKeys.putIfAbsent(it.id, () => GlobalKey());
    final failed = it.state == 'failed';
    final stageColor = done ? Pal.amber : (failed ? Pal.error : Pal.dim);
    // Пока своего названия нет — показываем название из разбора, затем ссылку.
    var title = it.title.isEmpty ? it.link : it.title;
    if (title.isEmpty || title == it.link) {
      final p = probe;
      if (p != null && p.title.isNotEmpty && (p.resolved == it.link || p.link == it.link)) {
        title = p.title;
      }
    }
    return Container(
      key: rowKey,
      padding: const EdgeInsets.symmetric(horizontal: 13, vertical: 10),
      decoration: const BoxDecoration(
        border: Border(bottom: BorderSide(color: Color(0x38FFB000), style: BorderStyle.solid)),
      ),
      child: MouseRegion(
        cursor: done ? SystemMouseCursors.grab : MouseCursor.defer,
        child: Row(children: [
        Expanded(
          child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
            Text(title,
                style: T.mono(13, c: Pal.soft), maxLines: 1, overflow: TextOverflow.ellipsis),
            const SizedBox(height: 3),
            Text(it.stage.toUpperCase(),
                style: T.ps(8, c: stageColor, ls: .04)),
          ]),
        ),
        const SizedBox(width: 12),
        SizedBox(
          width: 130,
          child: AnimatedLedRow(
              count: 8,
              progress: done ? 1 : it.progress,
              blinking: it.state == 'working',
              cellHeight: 14,
              gap: 3),
        ),
        const SizedBox(width: 8),
        if (it.state == 'working' || it.state == 'queued')
          _ActButton(
            icon: (_) => const Icon(Icons.close, size: 13, color: Pal.soft),
            onTap: () => core?.cancel(it.id),
          )
        else if (done) ...[
          _ActButton(
            icon: (hover) => FolderIcon(
                size: 14,
                color: hover ? Pal.soft : Pal.amber,
                glow: hover),
            onTap: () {
              if (it.files.isNotEmpty) {
                _openPath(File(it.files.first).parent.path);
              } else {
                _openPath(destFolder);
              }
            },
          ),
          const SizedBox(width: 6),
          _ActButton(
            icon: (hover) => TrashIcon(
                size: 13, color: hover ? Pal.soft : Pal.amber),
            onTap: () => _trashRow(it),
          ),
        ] else if (failed) ...[
          // ПОВТОРИТЬ: та же ссылка, тот же режим и формат — при ошибке
          // без переоткрытия ссылки.
          _ActButton(
            icon: (hover) => Icon(Icons.refresh,
                size: 15, color: hover ? Pal.soft : Pal.amber),
            onTap: () => _retryItem(it),
          ),
          const SizedBox(width: 6),
          _ActButton(
            icon: (hover) => TrashIcon(
                size: 13, color: hover ? Pal.soft : Pal.amber),
            onTap: () => _trashRow(it),
          ),
        ],
        ]),
      ),
    );
  }

  // ---- vpn ----

  Widget _vpnPlate() {
    return Center(
      child: AnimatedScale(
        scale: vpnDismissed ? .96 : 1,
        duration: const Duration(milliseconds: 220),
        curve: Curves.easeOutCubic,
        child: AnimatedOpacity(
          opacity: vpnDismissed ? 0 : 1,
          duration: const Duration(milliseconds: 220),
          child: GlitchIn(
            key: ValueKey('vpn-plate-$vpnEpoch'),
            child: TweenAnimationBuilder<double>(
              // Появление: лёгкий подъём с масштабом, glitch даёт GlitchIn.
              tween: Tween(begin: .94, end: 1),
              duration: const Duration(milliseconds: 300),
              curve: Curves.easeOutCubic,
              builder: (context, t, child) => Transform.translate(
                offset: Offset(0, (1 - t) * 8),
                child: Transform.scale(scale: t, child: child),
              ),
              child: GestureDetector(
              // Тап по самой плашке её не закрывает — только крестик
              // или клик по свободной области экрана.
              onTap: () {},
              child: Container(
                decoration: const BoxDecoration(color: Color(0xF00D0902)),
                child: DashedBox(
                  padding:
                      const EdgeInsets.symmetric(horizontal: 16, vertical: 13),
                  child: Row(mainAxisSize: MainAxisSize.min, children: [
                    const VpnIcon(),
                    const SizedBox(width: 10),
                    Text('ДЛЯ ЛУЧШЕЙ РАБОТЫ ПРИЛОЖЕНИЯ — ВКЛЮЧИТЕ VPN',
                        style: T.ps(8, c: Pal.soft, ls: .02)),
                    const SizedBox(width: 10),
                    GestureDetector(
                      onTap: () => setState(() => vpnDismissed = true),
                      child: MouseRegion(
                        cursor: SystemMouseCursors.click,
                        child: Padding(
                          padding: const EdgeInsets.all(2),
                          child: Text('×',
                              style: const TextStyle(
                                  fontFamily: T.plex,
                                  fontSize: 13,
                                  fontWeight: FontWeight.w700,
                                  height: 1,
                                  color: Pal.dim)),
                        ),
                      ),
                    ),
                  ]),
                ),
              ),
            ),
          ),
        ),
      ),
    ));
  }

  // ---- тост ----

  Widget _toast() {
    return Positioned(
      left: 0,
      right: 0,
      bottom: 112,
      child: Center(
        child: Container(
          decoration: const BoxDecoration(color: Color(0xE6050300)),
          child: DashedBox(
            padding: const EdgeInsets.symmetric(horizontal: 13, vertical: 9),
            child: Text(toastText!, style: T.mono(11, c: Pal.soft, ls: .02)),
          ),
        ),
      ),
    );
  }

  // ---- boot ----

  Widget _bootOverlay() {
    return Positioned.fill(
      child: AnimatedBuilder(
        animation: beamC,
        builder: (context, _) => CustomPaint(painter: BootBeamPainter(beamC.value)),
      ),
    );
  }

  // ---- нижняя рамка ----

  Widget _band() {
    return SizedBox(
      height: 96,
      child: Stack(children: [
        // папка
        Positioned(
          left: 22,
          top: 48,
          child: Transform.translate(
            offset: const Offset(0, -23),
            child: Plate(
              onPressed: _chooseDestFolder,
              child: const FolderIcon(),
            ),
          ),
        ),
        // логотип kvartal: вдавлен в пластик, при наведении светится фосфором
        Positioned(
          left: 0,
          right: 0,
          top: 55,
          child: Transform.translate(
            offset: const Offset(0, -31),
            child: Center(
              child: _KvartalLogo(onOpen: () => _openPath('https://kvartalrecords.ru')),
            ),
          ),
        ),
        // доллар: Boosty
        Positioned(
          right: 22,
          top: 48,
          child: Transform.translate(
            offset: const Offset(0, -23),
            child: Plate(
              onPressed: () => _openPath('https://boosty.to/kvartalrecords/donate'),
              child: Builder(builder: (context) {
                // \$ красится как иконка папки: тёмный, на ховере — фосфор.
                final hover = PlateHover.of(context)?.hover ?? false;
                return Text('\$',
                    style: TextStyle(
                        fontFamily: T.chakra,
                        fontWeight: FontWeight.w700,
                        fontSize: 20,
                        height: 1,
                        color: hover ? Pal.amber : plateIconIdle));
              }),
            ),
          ),
        ),
        // ручка масштаба — декоративная (масштаб тянет сама система)
        Positioned(
          right: 6,
          bottom: 6,
          child: IgnorePointer(
            child: Opacity(
              opacity: .5,
              child: CustomPaint(size: const Size(18, 18), painter: _GripPainter()),
            ),
          ),
        ),
      ]),
    );
  }
}

class _GripPainter extends CustomPainter {
  @override
  void paint(Canvas canvas, Size size) {
    final paint = Paint()
      ..color = Colors.white.withValues(alpha: .25)
      ..strokeWidth = 1;
    final path = Path();
    for (var d = -18.0; d < 18; d += 5) {
      path.moveTo(d, 18);
      path.lineTo(d + 18, 0);
    }
    canvas.clipRect(Offset.zero & size);
    canvas.drawPath(path, paint);
  }

  @override
  bool shouldRepaint(covariant _GripPainter old) => false;
}


/// kvartal: SVG тремя слоями — тёмная кромка сверху, светлая снизу (вдавлено
/// в пластик), поверх — фосфорное свечение при наведении.
class _KvartalLogo extends StatefulWidget {
  const _KvartalLogo({required this.onOpen});
  final VoidCallback onOpen;

  @override
  State<_KvartalLogo> createState() => _KvartalLogoState();
}

class _KvartalLogoState extends State<_KvartalLogo> {
  bool hover = false;

  Widget _copy(Color color, {double dy = 0, double blur = 0, double opacity = 1}) {
    Widget svg = SvgPicture.asset('assets/kvartal.svg',
        height: 62,
        colorFilter: ColorFilter.mode(color, BlendMode.srcIn));
    if (blur > 0) {
      svg = ImageFiltered(
          imageFilter: ui.ImageFilter.blur(sigmaX: blur, sigmaY: blur), child: svg);
    }
    if (opacity < 1) svg = Opacity(opacity: opacity, child: svg);
    if (dy != 0) svg = Transform.translate(offset: Offset(0, dy), child: svg);
    return svg;
  }

  @override
  Widget build(BuildContext context) {
    return MouseRegion(
      cursor: SystemMouseCursors.click,
      onEnter: (_) => setState(() => hover = true),
      onExit: (_) => setState(() => hover = false),
      child: GestureDetector(
        onTap: widget.onOpen,
        child: SizedBox(
          width: 210,
          height: 62,
          child: Stack(children: [
            Positioned.fill(
              child: IgnorePointer(
                child: AnimatedOpacity(
                  opacity: hover ? 1 : 0,
                  duration: const Duration(milliseconds: 250),
                  child: _copy(Pal.amber, blur: 9),
                ),
              ),
            ),
            _copy(Colors.black, dy: -1, blur: .6, opacity: .45),
            _copy(const Color(0x17FFFFFF), dy: 1.2),
            TweenAnimationBuilder<Color?>(
              tween: ColorTween(
                  begin: const Color(0xFF161412),
                  end: hover ? Pal.amber : const Color(0xFF161412)),
              duration: const Duration(milliseconds: 250),
              builder: (_, c, __) => _copy(c ?? const Color(0xFF161412)),
            ),
          ]),
        ),
      ),
    );
  }
}


// Чип режима: ховер — мягкая светло-оранжевая заливка с тонким свечением.
// Только opacity/цвет: никаких скачков размеров и пересборок layout.
class _ModeChip extends StatefulWidget {
  const _ModeChip({
    super.key,
    required this.label,
    required this.on,
    required this.locked,
    required this.hidden,
    required this.onTap,
  });
  final String label;
  final bool on;
  final bool locked;
  final bool hidden;
  final VoidCallback onTap;

  @override
  State<_ModeChip> createState() => _ModeChipState();
}

class _ModeChipState extends State<_ModeChip> {
  bool hover = false;

  @override
  Widget build(BuildContext context) {
    if (widget.hidden) return const SizedBox.shrink();
    return Padding(
      padding: const EdgeInsets.only(right: 6),
      child: GestureDetector(
        behavior: HitTestBehavior.opaque,
        onTap: widget.onTap,
        child: MouseRegion(
          cursor: widget.locked
              ? SystemMouseCursors.forbidden
              : SystemMouseCursors.click,
          onEnter: (_) => setState(() => hover = true),
          onExit: (_) => setState(() => hover = false),
          child: AnimatedOpacity(
            opacity: widget.locked ? .3 : 1,
            duration: const Duration(milliseconds: 160),
            child: AnimatedContainer(
              duration: const Duration(milliseconds: 180),
              curve: Curves.easeOut,
              padding:
                  const EdgeInsets.symmetric(horizontal: 11, vertical: 8),
              decoration: BoxDecoration(
                color: widget.on
                    ? Pal.amber
                    : (hover
                        ? Pal.amber.withValues(alpha: .12)
                        : Colors.transparent),
                borderRadius: const BorderRadius.all(Radius.circular(2)),
                boxShadow: [
                  // Список постоянной длины — выход из ховера не прыгает.
                  BoxShadow(
                      color: Pal.amber.withValues(
                          alpha: hover && !widget.on ? .16 : 0),
                      blurRadius: hover && !widget.on ? 10 : 0),
                ],
              ),
              child: AnimatedDefaultTextStyle(
                duration: const Duration(milliseconds: 180),
                curve: Curves.easeOut,
                style: T.ps(9,
                    c: widget.on ? const Color(0xFF0A0500) : Pal.soft),
                child: Text(widget.label),
              ),
            ),
          ),
        ),
      ),
    );
  }
}

// Кнопка СКАЧАТЬ: ховер усиливает золотое свечение, нажатие — скейл+сдвиг.
class _GoButton extends StatefulWidget {
  const _GoButton({required this.label, required this.enabled});
  final String label;
  final bool enabled;

  @override
  State<_GoButton> createState() => _GoButtonState();
}

class _GoButtonState extends State<_GoButton> {
  bool hover = false;
  bool pressed = false;

  @override
  Widget build(BuildContext context) {
    return GestureDetector(
      onTap: widget.enabled ? _onTap : null,
      child: MouseRegion(
        cursor: widget.enabled
            ? SystemMouseCursors.click
            : SystemMouseCursors.basic,
        onEnter: (_) => setState(() => hover = true),
        onExit: (_) => setState(() => hover = false),
        child: Listener(
          onPointerDown: (_) => setState(() => pressed = true),
          onPointerUp: (_) => setState(() => pressed = false),
          onPointerCancel: (_) => setState(() => pressed = false),
          child: AnimatedOpacity(
            opacity: widget.enabled ? 1 : .35,
            duration: const Duration(milliseconds: 180),
            child: AnimatedContainer(
              duration: const Duration(milliseconds: 160),
              curve: Curves.easeOutCubic,
              transform: Matrix4.translationValues(0, pressed ? 1 : 0, 0),
              padding:
                  const EdgeInsets.symmetric(horizontal: 16, vertical: 10),
              decoration: BoxDecoration(
                color: Pal.amber,
                boxShadow: [
                  BoxShadow(
                      color: Pal.amber.withValues(
                          alpha: hover ? .45 : .25),
                      blurRadius: hover ? 22 : 16),
                ],
              ),
              child: Text(widget.label,
                  style: T.ps(10,
                      c: const Color(0xFF0A0500), ls: .04)),
            ),
          ),
        ),
      ),
    );
  }

  void _onTap() {
    context
        .findAncestorStateOfType<_KLoadScreenState>()
        ?._download();
  }
}



/// Появление панели с коротким glitch: мягкое проявление, сдвиг на пару
/// пикселей и редкие горизонтальные помехи. Один раз на открытие.
class GlitchIn extends StatefulWidget {
  const GlitchIn({super.key, required this.child});
  final Widget child;

  @override
  State<GlitchIn> createState() => _GlitchInState();
}

class _GlitchInState extends State<GlitchIn>
    with SingleTickerProviderStateMixin {
  late final AnimationController _c = AnimationController(
      vsync: this, duration: const Duration(milliseconds: 280))
    ..forward();

  @override
  void dispose() {
    _c.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return AnimatedBuilder(
      animation: _c,
      builder: (context, child) {
        final t = Curves.easeOut.transform(_c.value);
        // Лёгкое горизонтальное дрожание в первые кадры появления.
        final dx = t < 1 ? math.sin(t * 21) * (1 - t) * 3 : 0.0;
        return Opacity(
          opacity: t.clamp(0, 1),
          child: Transform.translate(
            offset: Offset(dx, 0),
            child: Stack(children: [
              child!,
              Positioned.fill(
                child: IgnorePointer(
                  child: CustomPaint(
                      painter: _GlitchLinesPainter(
                          progress: t,
                          seed: widget.key?.hashCode ?? 0)),
                ),
              ),
            ]),
          ),
        );
      },
      child: widget.child,
    );
  }
}

class _GlitchLinesPainter extends CustomPainter {
  _GlitchLinesPainter({required this.progress, required this.seed});
  final double progress;
  final int seed;

  @override
  void paint(Canvas canvas, Size size) {
    if (progress >= 1) return;
    final rnd = math.Random(seed);
    final alpha = (1 - progress) * .5;
    for (var i = 0; i < 3; i++) {
      final y = rnd.nextDouble() * size.height;
      final h = 1 + rnd.nextDouble() * 2;
      final w = size.width * (.3 + rnd.nextDouble() * .5);
      final x = rnd.nextDouble() * (size.width - w);
      canvas.drawRect(
          Rect.fromLTWH(x, y, w, h),
          Paint()
            ..color = Pal.amber.withValues(alpha: alpha));
    }
  }

  @override
  bool shouldRepaint(covariant _GlitchLinesPainter old) =>
      old.progress != progress;
}



/// Обложка: пока настоящей нет — живая тёплая плазма с дизером; когда
/// картинка загрузилась — плавный кроссфейд. Картинка берётся из дискового
/// кэша ядра (тот же сетевой путь, что у разборов, с кэшем на диске).
/// Если превью получить невозможно, плазма остаётся.
class _SmartCover extends StatefulWidget {
  const _SmartCover({required this.url, this.engineAddr});
  final String? url;
  final int? engineAddr;

  @override
  State<_SmartCover> createState() => _SmartCoverState();
}

class _SmartCoverState extends State<_SmartCover> {
  String? _file; // локальный файл из кэша ядра
  int _gen = 0;
  bool _plasmaGone = false; // плазма глушится после проявления фото

  @override
  void initState() {
    super.initState();
    _load();
  }

  @override
  void didUpdateWidget(covariant _SmartCover old) {
    super.didUpdateWidget(old);
    if (old.url != widget.url) {
      _gen += 1;
      _file = null;
      _plasmaGone = false;
      _load();
    }
  }

  Future<void> _load() async {
    final url = widget.url;
    final addr = widget.engineAddr;
    if (url == null || url.isEmpty || addr == null) return;
    final gen = _gen;
    try {
      // Блокирующее чтение кэша ядра — в стороне, экран не ждёт.
      final path = await KdCore.thumbPathAsync(addr, url);
      if (!mounted || gen != _gen || path.isEmpty) return;
      if (!File(path).existsSync()) return;
      setState(() => _file = path);
      // Анимация анимацией, но 90-миллисекундный таймер плазмы после
      // проявления картинки только жрёт батарейку.
      Future.delayed(const Duration(milliseconds: 600), () {
        if (mounted && gen == _gen && !_plasmaGone) {
          setState(() => _plasmaGone = true);
        }
      });
    } on Object catch (e) {
      debugPrint('превью $url: $e');
    }
  }

  @override
  Widget build(BuildContext context) {
    return CustomPaint(
      foregroundPainter: const DashedBorderPainter(solid: true),
      child: SizedBox(
        width: 118,
        height: 118,
        child: ClipRect(
          child: Stack(fit: StackFit.expand, children: [
            if (!_plasmaGone) const _Plasma(),
            if (_file != null)
              TweenAnimationBuilder<double>(
                // Плавная замена плазмы изображением, когда оно приехало.
                tween: Tween(begin: 0, end: 1),
                duration: const Duration(milliseconds: 450),
                curve: Curves.easeOut,
                builder: (context, t, child) =>
                    Opacity(opacity: t, child: child),
                child: Image.file(
                  File(_file!),
                  fit: BoxFit.cover,
                  errorBuilder: (_, __, ___) => const SizedBox.shrink(),
                ),
              ),
          ]),
        ),
      ),
    );
  }
}

/// Тёплая дизер-плазма: низкоразрешённое поле значений + упорядоченный
/// дизер по Байеру в три янтарных тона. Обновляется степами — «дышит».
class _Plasma extends StatefulWidget {
  const _Plasma();

  @override
  State<_Plasma> createState() => _PlasmaState();
}

class _PlasmaState extends State<_Plasma> {
  double _t = 0;
  Timer? _timer;

  @override
  void initState() {
    super.initState();
    _timer = Timer.periodic(const Duration(milliseconds: 90), (_) {
      if (mounted) setState(() => _t = (_t + 0.09) % 1000);
    });
  }

  @override
  void dispose() {
    _timer?.cancel();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return CustomPaint(painter: _PlasmaPainter(_t));
  }
}

class _PlasmaPainter extends CustomPainter {
  _PlasmaPainter(this.t);
  final double t;

  // Упорядоченный дизер 4x4 (Байер): порог в пределах клетки.
  static const _bayer = [
    [0, 8, 2, 10],
    [12, 4, 14, 6],
    [3, 11, 1, 9],
    [15, 7, 13, 5],
  ];

  @override
  void paint(Canvas canvas, Size size) {
    const cell = 6.0; // крупное «зерно», пиксельная фактура
    final cols = (size.width / cell).ceil();
    final rows = (size.height / cell).ceil();
    const tones = [
      Color(0xFF241708),
      Color(0xFF5A3A0C),
      Color(0xFF96600E),
      Color(0xFFD08A14),
      Color(0xFFFFB000),
    ];
    final paint = Paint();
    for (var gy = 0; gy < rows; gy++) {
      for (var gx = 0; gx < cols; gx++) {
        final nx = gx / 9, ny = gy / 9;
        final v = math.sin(nx * 3.1 + t * .9) +
            math.sin(ny * 2.7 - t * .7) +
            math.sin((nx + ny) * 1.9 + t * .5) +
            math.sin(math.sqrt(nx * nx + ny * ny) * 4.0 - t * 1.1);
        // v в [-4;4] -> 0..1
        var f = (v + 4) / 8;
        f = (f * 1.25).clamp(0.0, 0.999);
        final bayer = _bayer[gy % 4][gx % 4] / 16 - 0.5;
        var level = (f * (tones.length - 1) + bayer * 0.9).round().clamp(0, tones.length - 1);
        paint.color = tones[level];
        canvas.drawRect(
            Rect.fromLTWH(gx * cell, gy * cell, cell + .5, cell + .5), paint);
      }
    }
  }

  @override
  bool shouldRepaint(covariant _PlasmaPainter old) => old.t != t;
}


/// Маленькая кнопка в строке очереди: ховер независим у каждой кнопки,
/// цвет/свечение меняются только у наведённой, геометрия не трогается.
class _ActButton extends StatefulWidget {
  const _ActButton({required this.icon, required this.onTap});
  final Widget Function(bool hover) icon;
  final VoidCallback onTap;

  @override
  State<_ActButton> createState() => _ActButtonState();
}

class _ActButtonState extends State<_ActButton> {
  bool hover = false;

  @override
  Widget build(BuildContext context) {
    return GestureDetector(
      onTap: widget.onTap,
      child: MouseRegion(
        cursor: SystemMouseCursors.click,
        onEnter: (_) => setState(() => hover = true),
        onExit: (_) => setState(() => hover = false),
        child: AnimatedContainer(
          duration: const Duration(milliseconds: 140),
          width: 27,
          height: 27,
          decoration: BoxDecoration(
            borderRadius: const BorderRadius.all(Radius.circular(2)),
            color:
                hover ? Pal.amber.withValues(alpha: .10) : Colors.transparent,
            boxShadow: [
              BoxShadow(
                  color: Pal.amber.withValues(alpha: hover ? .16 : 0),
                  blurRadius: hover ? 8 : 0),
            ],
          ),
          child: CustomPaint(
            foregroundPainter:
                const DashedBorderPainter(color: Color(0x73FFB000)),
            child: Center(child: widget.icon(hover)),
          ),
        ),
      ),
    );
  }
}
