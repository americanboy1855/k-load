import 'dart:async';
import 'dart:io';
import 'dart:ui' as ui;

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

  // очередь
  List<KdItem> items = [];
  String destFolder = '';
  bool toolsFound = true;

  // тост папки
  String? toastText;
  Timer? toastTimer;

  // системный выбор папки
  static const _native = MethodChannel('kload/native');

  bool _actHover = false;

  // хрон: позиция чипа, чтобы панель открывалась прямо под ним
  final _chronChipKey = GlobalKey();
  final _uiColumnKey = GlobalKey();
  double _chronX = 0;

  String get rawText => query.text.trim();
  bool get vpnVisible => booted && vpnState == 2 && !vpnDismissed;
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
        vpnState = e.on ? 1 : 2;
        if (e.on) vpnDismissed = false;
      });
    } else if (e is KdProbeEvent) {
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
    // VPN-плашка появляется через паузу, как в макете.
    Future.delayed(const Duration(seconds: 2), () {
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
      _animateSeek(() => _showBatch());
    } else {
      _animateSeek(null);
      core?.probeAsync(rawText);
    }
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

  void _showBatch() {
    setState(() {
      phase = Phase.found;
      mediaMode = true;
      musicOnly = false;
      chronLocked = true;
      chronOn = false;
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

    if (isBatch) {
      final fresh = batchLinks.where((l) => !busy.contains(l)).toList();
      if (fresh.isEmpty) return;
      c.enqueueBatch(fresh, audio: audio, sections: secs);
    } else {
      final p = probe;
      if (p == null || !p.ok) return;
      if (p.isPhoto) {
        final link = targetLink(rawText);
        if (busy.contains(link)) return;
        c.enqueuePhoto(link);
      } else if (p.isPlaylist) {
        final link = p.link;
        if (busy.contains(link)) return;
        c.enqueueBatch([link], audio: audio, wholePlaylist: 1);
      } else if (p.isSearch) {
        // найденный по названию трек: файл называется запросом
        final link = targetLink('');
        if (link.isEmpty || busy.contains(link)) return;
        c.enqueueBatch([link], audio: audio, sections: secs, nameOverride: rawText);
      } else {
        final link = targetLink(rawText);
        if (busy.contains(link)) return;
        c.enqueueBatch([link], audio: audio, sections: secs);
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
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    // Телевизор занимает окно целиком: масштаб «каверкой» (щелей не бывает),
    // углы корпуса — под системный радиус окна, микрощели исключены.
    if (chronOpen && chronOn) _measureChron();
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
            _ui(),
            GlassVeil(enabled: booted),
            if (tracking)
              AnimatedBuilder(
                  animation: trackC,
                  builder: (context, _) => TrackingBar(progress: trackC.value)),
            // vpn
            if (vpnVisible) ...[
              GestureDetector(
                  onTap: () => setState(() => vpnDismissed = true),
                  child: Container(color: const Color(0x94000000))),
              _vpnPlate(),
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
        if (chronOpen) setState(() => chronOpen = false);
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
                            style: T.h(21,
                                w: FontWeight.w500, c: Pal.amber, ls: .045),
                            decoration: const InputDecoration(
                                isCollapsed: true,
                                border: InputBorder.none),
                            autocorrect: false,
                            enableSuggestions: false,
                            keyboardType: TextInputType.url,
                          ),
                          if (rawText.isEmpty)
                            Text(
                              'Вставьте ссылку или напишите название того что нужно скачать',
                              style: T.h(18, w: FontWeight.w500, c: Pal.dim),
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
        Text('ПОИСК', style: T.h(17, c: Pal.soft, ls: .3)),
        Text('$seekPct%', style: T.h(24, c: Pal.amber)),
      ]),
      const SizedBox(height: 9),
      LedRow(count: 16, filled: (seekPct / 6.25).round().clamp(0, 16)),
    ]);
  }

  // ---- НАЙДЕНО ----

  Widget _foundCard() {
    final p = probe;
    if (isBatch) {
      return Row(crossAxisAlignment: CrossAxisAlignment.center, children: [
        _cover(null, const Color(0xFF5A4A2A)),
        const SizedBox(width: 15),
        Expanded(
          child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
            _badge('ПАЧКА', src: null),
            const SizedBox(height: 8),
            Text('$batchCount ССЫЛОК', style: T.h(26)),
            const SizedBox(height: 4),
            Text('ВИДЕО + ФОТО + МУЗЫКА', style: T.h(17, w: FontWeight.w500, c: Pal.dim, ls: .06)),
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
          Text('РАЗБОР НЕ УДАЛСЯ', style: T.h(26)),
          const SizedBox(height: 6),
          Text(p.error.isEmpty ? 'Проверь ссылку или сеть' : p.error,
              style: T.h(16, w: FontWeight.w500, c: Pal.dim), textAlign: TextAlign.center),
        ]),
      );
    }
    if (p.drm) {
      // Защищённая запись: карточка собирается из oEmbed, вместо режимов —
      // объяснение, почему скачать нельзя.
      return Row(crossAxisAlignment: CrossAxisAlignment.center, children: [
        _cover(p.thumbnail.isEmpty ? null : p.thumbnail, const Color(0xFF31415F)),
        const SizedBox(width: 15),
        Expanded(
          child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
            _badge(p.serviceTitle.toUpperCase(), src: _sourceUrl(p)),
            const SizedBox(height: 8),
            Text(_drmTitle(p), style: T.h(26), maxLines: 2, overflow: TextOverflow.ellipsis),
            const SizedBox(height: 4),
            Text(p.error.toUpperCase(),
                style: T.h(17, w: FontWeight.w600, c: Pal.error, ls: .06)),
          ]),
        ),
      ]);
    }
    final src = _sourceUrl(p);
    return Row(crossAxisAlignment: CrossAxisAlignment.center, children: [
      _cover(p.thumbnail.isEmpty ? null : p.thumbnail, const Color(0xFF31415F)),
      const SizedBox(width: 15),
      Expanded(
        child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
          _badge(p.serviceTitle.toUpperCase(), src: src),
          const SizedBox(height: 8),
          Text(p.title, style: T.h(26),
              maxLines: 2, overflow: TextOverflow.ellipsis),
          const SizedBox(height: 4),
          Text(_durLine(p), style: T.h(17, w: FontWeight.w500, c: Pal.dim, ls: .06)),
        ]),
      ),
    ]);
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

  String _durLine(KdProbeEvent p) {
    if (p.isPhoto) return 'ФОТОГРАФИЯ';
    if (p.isPlaylist) {
      final mins = (p.duration * p.count / 60).round();
      return '${p.count} ВИДЕО · $mins МИН';
    }
    final dur = p.duration > 0 ? ' · ${fmtDur(p.duration)}' : '';
    return 'ХРОНОМЕТРАЖ$dur';
  }

  Widget _badge(String text, {String? src}) {
    return Row(children: [
      Container(
        padding: const EdgeInsets.symmetric(horizontal: 9, vertical: 2),
        color: Pal.amber,
        child: Text(text, style: T.h(19, c: const Color(0xFF0A0500), ls: .14)),
      ),
      if (src != null && src.startsWith('http')) ...[
        const SizedBox(width: 9),
        GestureDetector(
          onTap: () => _openPath(src),
          child: MouseRegion(
            cursor: SystemMouseCursors.click,
            child: Text('[Ссылка]', style: T.h(15, w: FontWeight.w500, c: Pal.dim, ls: .08)),
          ),
        ),
      ],
    ]);
  }

  Widget _cover(String? url, Color fallbackFrom) {
    return CustomPaint(
      foregroundPainter: const DashedBorderPainter(solid: true),
      child: SizedBox(
        width: 118,
        height: 118,
        child: url != null
            ? Image.network(url,
                fit: BoxFit.cover,
                errorBuilder: (_, __, ___) => _coverPlaceholder(fallbackFrom))
            : _coverPlaceholder(fallbackFrom),
      ),
    );
  }

  Widget _coverPlaceholder(Color from) {
    // шахматка «обложки нет» + тёмная заливка
    return Container(
      decoration: BoxDecoration(
        gradient: LinearGradient(begin: Alignment.topLeft, end: Alignment.bottomRight, colors: [from, Colors.black]),
      ),
      child: CustomPaint(painter: _CheckerPainter()),
    );
  }

  // ---- режимы + СКАЧАТЬ ----

  Widget _modesRow() {
    final p = probe;
    final photo = !isBatch && (p?.isPhoto ?? false);
    final goLabel = isBatch
        ? 'СКАЧАТЬ · $batchCount'
        : p != null && p.isPlaylist
            ? 'СКАЧАТЬ · ${p.count}'
            : 'СКАЧАТЬ · 1';
    final canDownload = isBatch || (p != null && p.ok);
    final videoLabel = mediaMode ? 'МЕДИА' : 'ВИДЕО';
    return Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
      Row(children: [
        _ModeChip(
            key: const ValueKey('video'),
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
        _ModeChip(
            key: _chronChipKey,
            label: 'ХРОН',
            on: chronOn,
            locked: chronLocked,
            hidden: false,
            onTap: () => setState(() {
                  if (chronLocked) return;
                  chronOn = !chronOn;
                  chronOpen = chronOn;
                  if (chronOpen) _measureChron();
                })),
        const Spacer(),
        _GoButton(label: goLabel, enabled: canDownload),
      ]),
      // Панель хрона открывается прямо под чипом и плавно раздвигает
      // следующий контент (очередь уезжает вниз, ничего не перекрывается).
      AnimatedSize(
        duration: const Duration(milliseconds: 240),
        curve: Curves.easeOutCubic,
        alignment: Alignment.topLeft,
        child: chronOpen && chronOn && !chronLocked
            ? Padding(
                padding: EdgeInsets.only(left: _chronX, top: 8),
                child: _chronBox(),
              )
            : const SizedBox(width: double.infinity),
      ),
    ]);
  }

  void _measureChron() {
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!mounted || !chronOpen || !chronOn) return;
      final chipCtx = _chronChipKey.currentContext;
      final colCtx = _uiColumnKey.currentContext;
      if (chipCtx == null || colCtx == null) return;
      final chipBox = chipCtx.findRenderObject() as RenderBox?;
      final colBox = colCtx.findRenderObject() as RenderBox?;
      if (chipBox == null || colBox == null || !chipBox.attached) return;
      final x = chipBox.localToGlobal(Offset.zero, ancestor: colBox).dx;
      if ((x - _chronX).abs() > 0.5) setState(() => _chronX = x);
    });
  }

  Widget _chronBox() {
    return Container(
      decoration: const BoxDecoration(
        color: Color(0xF5070400),
      ),
      child: DashedBox(
        padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 9),
        child: Row(mainAxisSize: MainAxisSize.min, children: [
          Text('ОТ', style: T.h(16, ls: .1)),
          const SizedBox(width: 6),
          SizedBox(
              width: 56,
              child: TextField(
                controller: chronFrom,
                textAlign: TextAlign.center,
                cursorColor: Pal.amber,
                onSubmitted: (_) => FocusScope.of(context).unfocus(),
                style: T.h(20, c: Pal.amber, ls: .04),
                decoration: const InputDecoration(isCollapsed: true, border: InputBorder.none),
              )),
          const SizedBox(width: 8),
          Text('ДО', style: T.h(16, ls: .1)),
          const SizedBox(width: 6),
          SizedBox(
              width: 56,
              child: TextField(
                controller: chronTo,
                textAlign: TextAlign.center,
                cursorColor: Pal.amber,
                onSubmitted: (_) => FocusScope.of(context).unfocus(),
                style: T.h(20, c: Pal.amber, ls: .04),
                decoration: const InputDecoration(isCollapsed: true, border: InputBorder.none),
              )),
        ]),
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
                style: T.h(20, c: Pal.soft), maxLines: 1, overflow: TextOverflow.ellipsis),
            const SizedBox(height: 3),
            Text(it.stage.toUpperCase(),
                style: T.h(15, w: FontWeight.w500, c: stageColor, ls: .1)),
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
          _actButton((_) => const Icon(Icons.close, size: 13, color: Pal.soft),
              () => core?.cancel(it.id))
        else if (done) ...[
          _actButton(
              (hover) => FolderIcon(
                  size: 14, color: hover ? Pal.soft : Pal.amber), () {
            if (it.files.isNotEmpty) {
              _openPath(File(it.files.first).parent.path);
            } else {
              _openPath(destFolder);
            }
          }),
          const SizedBox(width: 6),
          _actButton(
              (hover) => TrashIcon(size: 13, color: hover ? Pal.soft : Pal.amber),
              () => _trashRow(it)),
        ] else if (failed)
          _actButton(
              (hover) =>
                  TrashIcon(size: 13, color: hover ? Pal.soft : Pal.amber),
              () => _trashRow(it)),
      ]),
    );
  }

  Widget _actButton(Widget Function(bool hover) build, VoidCallback onTap) {
    return GestureDetector(
      onTap: onTap,
      child: MouseRegion(
        cursor: SystemMouseCursors.click,
        onEnter: (_) => setState(() => _actHover = true),
        onExit: (_) => setState(() => _actHover = false),
        child: SizedBox(
          width: 27,
          height: 27,
          child: CustomPaint(
            foregroundPainter:
                const DashedBorderPainter(color: Color(0x73FFB000)),
            child: Center(child: build(_actHover)),
          ),
        ),
      ),
    );
  }

  // ---- vpn ----

  Widget _vpnPlate() {
    return Center(
      child: Container(
        decoration: const BoxDecoration(color: Color(0xF00D0902)),
        child: DashedBox(
          padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 13),
          child: Row(mainAxisSize: MainAxisSize.min, children: [
            const VpnIcon(),
            const SizedBox(width: 10),
            Text('Для лучшей работы загрузчика - включите VPN',
                style: T.h(17, w: FontWeight.w500, c: Pal.soft)),
            const SizedBox(width: 4),
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
            child: Text(toastText!, style: T.h(15, w: FontWeight.w500, c: Pal.soft, ls: .04)),
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
      padding: const EdgeInsets.only(right: 9),
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
                  const EdgeInsets.symmetric(horizontal: 15, vertical: 8),
              decoration: BoxDecoration(
                color: widget.on
                    ? Pal.amber
                    : (hover
                        ? Pal.amber.withValues(alpha: .12)
                        : Colors.transparent),
                borderRadius: const BorderRadius.all(Radius.circular(2)),
                boxShadow: hover && !widget.on
                    ? [BoxShadow(
                        color: Pal.amber.withValues(alpha: .16),
                        blurRadius: 10)]
                    : const [],
              ),
              child: AnimatedDefaultTextStyle(
                duration: const Duration(milliseconds: 180),
                curve: Curves.easeOut,
                style: T.h(19,
                    c: widget.on ? const Color(0xFF0A0500) : Pal.soft,
                    ls: .06),
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
                  const EdgeInsets.symmetric(horizontal: 22, vertical: 10),
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
                  style: T.h(20,
                      w: FontWeight.w700,
                      c: const Color(0xFF0A0500),
                      ls: .06)),
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

