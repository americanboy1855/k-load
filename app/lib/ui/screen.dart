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

  // источник поиска текстового запроса: null/«auto» — автомат
  String? searchSource;
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

  final sourceFilter = TextEditingController();

  // раскрытая панель: '' | chron | count | quality | source
  String openPanel = '';
  final chipKeys = {
    'chron': GlobalKey(),
    'count': GlobalKey(),
    'quality': GlobalKey(),
    'source': GlobalKey(),
  };
  double panelX = 0;

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

  // системный выбор папки
  static const _native = MethodChannel('kload/native');

  // позиция чипа, под которым раскрыта панель
  final _uiColumnKey = GlobalKey();
  double _chronX = 0;

  String get rawText => query.text.trim();
  bool get vpnVisible => booted && vpnState == 2;
  bool get cardVisible => phase == Phase.found;

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
      setState(() {
        final wasOff = vpnState == 2;
        vpnState = e.on ? 1 : 2;
        if (e.on) vpnDismissed = false;
        // Новое появление плашки при каждом переходе вкл -> выкл.
        if (wasOff && !e.on) vpnEpoch += 1;
      });
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
    }
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
      searchSource = null;
      playlistLimit = 0;
      countCtrl.clear();
      batchProbing = false;
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
      if (links.length > 1) {
        isBatch = true;
        batchCount = links.length;
        batchLinks = links;
      } else {
        isBatch = false;
      }
    });
    if (isBatch) {
      // пачка разбирается локально: короткая анимация и карточка
      _animateSeek(() {
        _showBatch();
        _probeNextBatchLink();
      });
    } else {
      _animateSeek(null);
      core?.probeAsync(rawText, source: searchSource);
    }
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
    // Что реально будет качаться: для поиска по названию это resolved
    // (SoundCloud и т.п.), а не сырой текст из поля.
    final effective = (p.resolved.isNotEmpty ? p.resolved : p.link);
    final svc = serviceOf(effective.startsWith('http') ? effective : rawText);
    setState(() {
      probe = p;
      phase = Phase.found;
      seekPct = 100;
      mediaMode = false;
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
          audio: audio, quality: quality, container: videoContainer);
    } else {
      if (p == null || !p.ok) return;
      if (p.isPhoto) {
        final link = targetLink(rawText);
        if (busy.contains(link)) return;
        c.enqueuePhoto(link, imageFormat: imageFormat);
      } else if (playlist) {
        final link = p.link;
        if (busy.contains(link)) return;
        c.enqueueBatch([link],
            audio: audio,
            wholePlaylist: 1,
            playlistLimit: limit,
            quality: quality,
            container: videoContainer);
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
            audioFormat: audioFormat);
      } else {
        final link = targetLink(rawText);
        if (busy.contains(link)) return;
        c.enqueueBatch([link],
            audio: audio,
            sections: secs,
            quality: quality,
            container: videoContainer,
            audioFormat: audioFormat);
      }
    }
    setState(() => items = c.snapshot());
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
  }

  // ---- build ----

  @override
  void dispose() {
    sub?.cancel();
    trackTimer?.cancel();
    debounce?.cancel();
    seekAnim?.cancel();
    toastTimer?.cancel();
    beamC.dispose();
    trackC.dispose();
    query.dispose();
    searchFocus.dispose();
    chronFrom.dispose();
    chronTo.dispose();
    sourceFilter.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    // Телевизор занимает окно целиком: масштаб «каверкой» (щелей не бывает),
    // углы корпуса — под системный радиус окна, микрощели исключены.
    if (openPanel.isNotEmpty) _measurePanel(openPanel);
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
            child: FittedBox(fit: BoxFit.fill, child: SizedBox(width: tvW, height: tvH, child: _tv())),
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
      // Клик вне панели хрона закрывает её (дети перехватывают свои тапы).
      behavior: HitTestBehavior.translucent,
      onTap: () {
        if (openPanel.isNotEmpty || chronOpen) {
          setState(() {
            openPanel = '';
            chronOpen = false;
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

  // ---- ПОИСК: LED-шкала ----

  Widget _seekBlock() {
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
        _SmartCover(url: null),
        const SizedBox(width: 15),
        Expanded(
          child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
            _badge(batchBadge, src: null, dropdown: false),
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
      return DashedBox(
        color: Pal.amber.withValues(alpha: .35),
        padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 12),
        child: Column(children: [
          Text('РАЗБОР НЕ УДАЛСЯ', style: T.ps(12, c: Pal.soft)),
          const SizedBox(height: 6),
          Text(p.error.isEmpty ? 'Проверь ссылку или сеть' : p.error,
              style: T.mono(11, c: Pal.dim), textAlign: TextAlign.center),
        ]),
      );
    }
    if (p.drm) {
      // Защищённая запись: карточка собирается из oEmbed, вместо режимов —
      // объяснение, почему скачать нельзя.
      return Row(crossAxisAlignment: CrossAxisAlignment.center, children: [
        _SmartCover(url: p.thumbnail.isEmpty ? null : p.thumbnail),
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
      _SmartCover(url: p.thumbnail.isEmpty ? null : p.thumbnail),
      const SizedBox(width: 15),
      Expanded(
        child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
          _badge(p.isPlaylist
                  ? 'ПЛЕЙЛИСТ · ${p.serviceTitle.toUpperCase()}'
                  : p.serviceTitle.toUpperCase(),
              src: src,
              dropdown: p.isSearch, // выбор источника — только для запроса
              badgeKey: chipKeys['source']),
          // Выпадающий источник для текстового запроса.
          AnimatedSize(
            duration: const Duration(milliseconds: 220),
            curve: Curves.easeOutCubic,
            alignment: Alignment.topLeft,
            child: openPanel == 'source'
                ? Padding(
                    padding: const EdgeInsets.only(top: 6),
                    child: GlitchIn(
                      key: const ValueKey('panel-source'),
                      child: _sourceBox(),
                    ),
                  )
                : const SizedBox(width: double.infinity),
          ),
          // Качество: компактный блок прямо под источником.
          const SizedBox(height: 6),
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
  /// фото — JPG/PNG + подпись. Один-единственный вариант — без списка.
  Widget _mediaOptions(KdProbeEvent p) {
    if (p.isPhoto || !p.ok || p.isPlaylist) return const SizedBox.shrink();

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

    // Видео: формат слева, качество справа (реальные высоты кадра).
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
              label: quality == 'best' ? 'МАКС' : '$quality P',
              open: openPanel == 'quality',
              onTap: () => setState(() =>
                  openPanel = openPanel == 'quality' ? '' : 'quality')),
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

  /// Источники поиска для текстового запроса: все сервисы, которые ядро
  /// реально умеет искать. Внутри — поиск по названию сервиса.
  Widget _sourceBox() {
    final options = <(String, String)>[
      ('АВТО', 'auto'),
      ('YOUTUBE', 'youtube'),
      ('SOUNDCLOUD', 'soundcloud'),
      ('APPLE MUSIC', 'apple'),
      ('SPOTIFY', 'spotify'),
      ('ЯНДЕКС МУЗЫКА', 'yandex'),
      ('PINTEREST', 'pinterest'),
    ];
    final current = (searchSource == null || searchSource == 'auto')
        ? 'auto'
        : searchSource!;
    final q = sourceFilter.text.trim().toUpperCase();
    final visible = q.isEmpty
        ? options
        : options.where((o) => o.$1.contains(q)).toList();
    return _darkPanel(
      child: SizedBox(
        width: 190,
        child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
          TextField(
            controller: sourceFilter,
            autofocus: true,
            onChanged: (_) => setState(() {}),
            cursorColor: Pal.amber,
            style: T.mono(10, c: Pal.amber),
            decoration: const InputDecoration(
                isCollapsed: true,
                border: InputBorder.none,
                hintText: 'ПОИСК',
                hintStyle: TextStyle(
                    fontFamily: 'Press Start 2P',
                    fontSize: 7,
                    color: Pal.dim)),
          ),
          const SizedBox(height: 4),
          for (final opt in visible)
            Padding(
              padding: const EdgeInsets.only(bottom: 2),
              child: GestureDetector(
                onTap: () {
                  setState(() {
                    searchSource = opt.$2 == 'auto' ? null : opt.$2;
                    openPanel = '';
                    sourceFilter.clear();
                  });
                  _startSeek(); // переразбор запроса в выбранном источнике
                },
                child: MouseRegion(
                  cursor: SystemMouseCursors.click,
                  child: AnimatedContainer(
                    duration: const Duration(milliseconds: 140),
                    padding: const EdgeInsets.symmetric(
                        horizontal: 8, vertical: 4),
                    color: current == opt.$2
                        ? Pal.amberFaint
                        : Colors.transparent,
                    child: Text(opt.$1,
                        style: T.ps(8,
                            c: current == opt.$2 ? Pal.amber : Pal.soft)),
                  ),
                ),
              ),
            ),
          if (visible.isEmpty)
            Padding(
              padding: const EdgeInsets.symmetric(vertical: 4),
              child: Text('НЕ НАЙДЕН', style: T.ps(7, c: Pal.dim)),
            ),
        ]),
      ),
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
    final dur = p.duration > 0 ? ' · ${fmtDur(p.duration)}' : '';
    return 'ХРОНОМЕТРАЖ$dur';
  }

  /// 3725 -> '1:02:05', 754 -> '12:34'.
  String fmtLongDur(int totalSec) {
    if (totalSec <= 0) return '—';
    final h = totalSec ~/ 3600, m = (totalSec % 3600) ~/ 60, sec = totalSec % 60;
    final mm = m.toString().padLeft(2, '0'), ss = sec.toString().padLeft(2, '0');
    return h > 0 ? '$h:$mm:$ss' : '$m:$ss';
  }

  Widget _badge(String text,
      {String? src, bool dropdown = false, Key? badgeKey}) {
    final badge = Container(
      padding: const EdgeInsets.symmetric(horizontal: 9, vertical: 3),
      color: Pal.amber,
      child: Row(mainAxisSize: MainAxisSize.min, children: [
        Text(text, style: T.ps(8, c: const Color(0xFF0A0500), ls: .04)),
        if (dropdown) ...[
          const SizedBox(width: 6),
          const Icon(Icons.expand_more,
              size: 10, color: Color(0xFF0A0500)),
        ],
      ]),
    );
    return Row(children: [
      dropdown
          ? GestureDetector(
              key: badgeKey,
              onTap: () => setState(() =>
                  openPanel = openPanel == 'source' ? '' : 'source'),
              child: MouseRegion(
                cursor: SystemMouseCursors.click,
                child: badge,
              ),
            )
          : badge,
      if (src != null && src.startsWith('http')) ...[
        const SizedBox(width: 9),
        GestureDetector(
          onTap: () => _openPath(src),
          child: MouseRegion(
            cursor: SystemMouseCursors.click,
            child: Text('[Ссылка]', style: T.mono(10, c: Pal.dim, ls: .04)),
          ),
        ),
      ],
    ]);
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
  Widget _countBox() {
    return _darkPanel(
      child: Row(mainAxisSize: MainAxisSize.min, children: [
        Text('КОЛ-ВО', style: T.ps(8, c: Pal.soft)),
        const SizedBox(width: 8),
        SizedBox(
            width: 64,
            child: TextField(
              controller: countCtrl,
              keyboardType: TextInputType.number,
              inputFormatters: [FilteringTextInputFormatter.digitsOnly],
              textAlign: TextAlign.center,
              cursorColor: Pal.amber,
              onChanged: (v) =>
                  setState(() => playlistLimit = int.tryParse(v) ?? 0),
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
      decoration: const BoxDecoration(
        border: Border(top: BorderSide(color: Pal.amberFaint), bottom: BorderSide(color: Pal.amberFaint)),
      ),
      child: CustomPaint(
        foregroundPainter: const DashedBorderPainter(color: Color(0x80FFB000)),
        child: ListView.builder(
          padding: EdgeInsets.zero,
          itemCount: items.length,
          itemBuilder: (context, i) => _queueRow(items[i]),
        ),
      ),
    );
  }

  Widget _queueRow(KdItem it) {
    final done = it.state == 'done';
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
      padding: const EdgeInsets.symmetric(horizontal: 13, vertical: 10),
      decoration: const BoxDecoration(
        border: Border(bottom: BorderSide(color: Color(0x38FFB000), style: BorderStyle.solid)),
      ),
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
        ] else if (failed)
          _ActButton(
            icon: (hover) => TrashIcon(
                size: 13, color: hover ? Pal.soft : Pal.amber),
            onTap: () => _trashRow(it),
          ),
      ]),
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
    );
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

class _CheckerPainter extends CustomPainter {
  @override
  void paint(Canvas canvas, Size size) {
    final paint = Paint()..color = const Color(0x1AFFB000);
    const cell = 8.0;
    for (var y = 0.0; y < size.height; y += cell) {
      for (var x = 0.0; x < size.width; x += cell) {
        if (((x / cell).round() + (y / cell).round()) % 2 == 0) {
          canvas.drawRect(Rect.fromLTWH(x, y, cell, cell), paint);
        }
      }
    }
  }

  @override
  bool shouldRepaint(covariant _CheckerPainter old) => false;
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
/// картинка загрузилась — плавный кроссфейд. Без обложки плазма остаётся.
class _SmartCover extends StatefulWidget {
  const _SmartCover({required this.url});
  final String? url;

  @override
  State<_SmartCover> createState() => _SmartCoverState();
}

class _SmartCoverState extends State<_SmartCover> {
  bool _loaded = false;
  bool _failed = false;
  ImageStream? _stream;
  ImageStreamListener? _listener;

  @override
  void didUpdateWidget(covariant _SmartCover old) {
    super.didUpdateWidget(old);
    if (old.url != widget.url) {
      _loaded = false;
      _failed = false;
      _stream = null;
    }
  }

  void _listen(ImageStream stream) {
    _listener ??= ImageStreamListener((info, _) {
      if (mounted && !_loaded) setState(() => _loaded = true);
    }, onError: (_, __) {
      if (mounted) setState(() => _failed = true);
    });
    stream.addListener(_listener!);
  }

  @override
  Widget build(BuildContext context) {
    final hasArt = widget.url != null && widget.url!.isNotEmpty && !_failed;
    return CustomPaint(
      foregroundPainter: const DashedBorderPainter(solid: true),
      child: SizedBox(
        width: 118,
        height: 118,
        child: ClipRect(
          child: Stack(fit: StackFit.expand, children: [
            const _Plasma(),
            if (hasArt)
              AnimatedOpacity(
                opacity: _loaded ? 1 : 0,
                duration: const Duration(milliseconds: 450),
                child: Image.network(
                  widget.url!,
                  fit: BoxFit.cover,
                  frameBuilder: (context, child, frame, wasLoaded) {
                    // Подписываемся на поток, чтобы узнать о завершении.
                    if (!_loaded && (frame ?? 0) > 0) {
                      WidgetsBinding.instance.addPostFrameCallback((_) {
                        if (mounted && !_loaded) setState(() => _loaded = true);
                      });
                    }
                    return child;
                  },
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
