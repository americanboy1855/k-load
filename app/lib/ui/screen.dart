import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
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
  late final AnimationController swimC =
      AnimationController(vsync: this, duration: const Duration(milliseconds: 9000));
  late final AnimationController flickC =
      AnimationController(vsync: this, duration: const Duration(milliseconds: 6500));
  late final AnimationController trackC =
      AnimationController(vsync: this, duration: const Duration(milliseconds: 1700));
  bool tracking = false;
  Timer? bootAutoTimer, trackTimer;

  // vpn
  int vpnState = 0; // 0 неизвестно, 1 вкл, 2 выкл
  bool vpnDismissed = false;

  // поиск
  final query = TextEditingController();
  final searchFocus = FocusNode();
  bool searchFocused = false;
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
  bool chronOn = false;
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

  String get rawText => query.text.trim();
  bool get vpnVisible => booted && vpnState == 2 && !vpnDismissed;
  bool get cardVisible => phase == Phase.found;

  @override
  void initState() {
    super.initState();
    flickC.repeat();
    _scheduleTracking();
    _boot();
    if (!booted) {
      bootAutoTimer = Timer(const Duration(milliseconds: 2600), () {
        if (!booted) _finishBoot();
      });
    }
    beamC.addStatusListener((s) {
      if (s == AnimationStatus.completed) _finishBoot();
    });
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
    trackTimer = Timer(Duration(milliseconds: 9000 + DateTime.now().millisecondsSinceEpoch % 5000), () {
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

  void _startBeam() {
    if (booted || beamStarted) return;
    setState(() => beamStarted = true);
    beamC.forward(from: 0);
  }

  void _finishBoot() {
    bootAutoTimer?.cancel();
    setState(() => booted = true);
    swimC.repeat();
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
    final link = p.link.isNotEmpty ? p.link : (p.resolved.isNotEmpty ? p.resolved : rawText);
    final svc = serviceOf(link);
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
    if (isBatch) {
      c.enqueueBatch(batchLinks, audio: audio, sections: secs);
    } else {
      final p = probe;
      if (p == null || !p.ok) return;
      if (p.isPhoto) {
        c.enqueuePhoto(p.link.isNotEmpty ? p.link : rawText);
      } else if (p.isPlaylist) {
        c.enqueueBatch([p.link], audio: audio, wholePlaylist: 1);
      } else if (p.isSearch) {
        // найденный по названию трек: файл называется запросом
        final link = p.resolved.isNotEmpty ? p.resolved : p.link;
        if (link.isEmpty) return;
        c.enqueueBatch([link], audio: audio, sections: secs, nameOverride: rawText);
      } else {
        final link = p.link.isNotEmpty ? p.link : rawText;
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

  void _openFolderButton() {
    final dir = destFolder;
    if (dir.isEmpty) return;
    Directory(dir).create(recursive: true).then((_) => _openPath(dir));
    _showToast('КАЧАЕМ В ~/Downloads/K LOAD');
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
    bootAutoTimer?.cancel();
    trackTimer?.cancel();
    debounce?.cancel();
    seekAnim?.cancel();
    toastTimer?.cancel();
    beamC.dispose();
    swimC.dispose();
    flickC.dispose();
    trackC.dispose();
    query.dispose();
    searchFocus.dispose();
    chronFrom.dispose();
    chronTo.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    // Окно держат пропорции сами (contentAspectRatio) — масштаб по ширине.
    return Scaffold(
      backgroundColor: Pal.desk,
      body: FittedBox(
        fit: BoxFit.contain,
        child: SizedBox(
          width: tvW,
          height: tvH,
          child: _tv(),
        ),
      ),
    );
  }

  Widget _tv() {
    return Container(
      clipBehavior: Clip.antiAlias,
      decoration: BoxDecoration(
        borderRadius: BorderRadius.circular(26),
        gradient: const LinearGradient(
          begin: Alignment(-0.6, -1),
          end: Alignment(0.7, 1),
          colors: [Color(0xFF2E2A27), Pal.plastic, Color(0xFF1D1B19), Color(0xFF161413)],
          stops: [0, 0.34, 0.78, 1],
        ),
        boxShadow: const [
          BoxShadow(color: Color(0x14000000), offset: Offset(1, 0), blurRadius: 0, spreadRadius: 0),
        ],
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
            // содержимое плавает, как на живом кинескопе
            AnimatedBuilder(
              animation: swimC,
              builder: (context, _) {
                if (!booted) return _ui();
                final t = swimC.value;
                final dx = _swimOffset(t);
                return Transform.translate(offset: Offset(dx, 0), child: _ui());
              },
            ),
            GlassOverlay(flicker: bootless ? 0 : flickValue()),
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

  bool get bootless => !booted;

  double flickValue() {
    final t = flickC.value;
    double v = 0;
    double pulse(double a, double b, double peak) {
      if (t < a || t > b) return 0;
      final k = (t - a) / (b - a);
      return peak * (1 - (2 * k - 1).abs());
    }
    v = pulse(0.46, 0.48, 0.028) + pulse(0.79, 0.81, 0.016);
    return v;
  }

  double _swimOffset(double t) {
    const keys = [0.0, 0.12, 0.26, 0.40, 0.52, 0.64, 0.81, 1.0];
    const dxs = [0.0, -1.0, 0.9, -0.5, 1.8, 0.6, -0.9, 0.0];
    for (var i = 0; i < keys.length - 1; i++) {
      if (t >= keys[i] && t <= keys[i + 1]) {
        final k = (t - keys[i]) / (keys[i + 1] - keys[i]);
        return dxs[i] + (dxs[i + 1] - dxs[i]) * k;
      }
    }
    return 0;
  }

  Widget _ui() {
    return Padding(
      padding: const EdgeInsets.fromLTRB(20, 22, 20, 14),
      child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
        _searchRow(),
        if (phase == Phase.seeking) ...[
          const SizedBox(height: 14),
          _seekBlock(),
        ],
        if (cardVisible) ...[
          const SizedBox(height: 18),
          _foundCard(),
          const SizedBox(height: 18),
          _modesRow(),
        ],
        if (items.isNotEmpty) ...[
          const SizedBox(height: 14),
          Expanded(child: _queue()),
        ],
      ]),
    );
  }

  // ---- поисковая строка ----

  Widget _searchRow() {
    return GestureDetector(
      onTap: () => searchFocus.requestFocus(),
      child: AnimatedBuilder(
        animation: Listenable.merge([searchFocus, query]),
        builder: (context, _) {
          final focused = searchFocus.hasFocus;
          return CustomPaint(
            foregroundPainter: DashedBorderPainter(
                color: focused ? Pal.amber.withValues(alpha: .85) : Pal.amberSoft),
            child: Container(
              decoration: BoxDecoration(
                boxShadow: focused
                    ? [BoxShadow(color: Pal.amber.withValues(alpha: .16), blurRadius: 22)]
                    : [],
              ),
              padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 12),
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
                      style: T.h(21, w: FontWeight.w500, c: Pal.amber, ls: .045),
                      decoration: const InputDecoration(
                          isCollapsed: true, border: InputBorder.none),
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
    if (!p.ok) {
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
    final src = p.link.isNotEmpty ? p.link : (p.resolved.isNotEmpty ? p.resolved : rawText);
    return Row(crossAxisAlignment: CrossAxisAlignment.center, children: [
      _cover(p.thumbnail.isEmpty ? null : p.thumbnail, const Color(0xFF31415F)),
      const SizedBox(width: 15),
      Expanded(
        child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
          _badge(p.serviceTitle.toUpperCase(), src: src),
          const SizedBox(height: 8),
          Text(p.title, style: T.h(26), maxLines: 2, overflow: TextOverflow.ellipsis),
          const SizedBox(height: 4),
          Text(_durLine(p), style: T.h(17, w: FontWeight.w500, c: Pal.dim, ls: .06)),
        ]),
      ),
    ]);
  }

  String _durLine(KdProbeEvent p) {
    if (p.isPhoto) return 'ФОТОГРАФИЯ';
    if (p.isPlaylist) {
      final mins = (p.duration * p.count / 60).round();
      return '${p.count} ВИДЕО · $mins МИН';
    }
    final dur = p.duration > 0 ? ' · ${fmtDur(p.duration)}' : '';
    final only = musicOnly ? ' · ТОЛЬКО ЗВУК' : '';
    return 'ХРОНОМЕТРАЖ$dur$only';
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
      foregroundPainter: const DashedBorderPainter(),
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
    return Stack(clipBehavior: Clip.none, children: [
      Row(children: [
        _chip(videoLabel,
            on: mode == 'video' && !musicOnly,
            hidden: musicOnly || photo,
            onTap: () => setState(() => mode = 'video')),
        _chip('МУЗЫКА',
            on: mode == 'music',
            hidden: photo,
            onTap: () => setState(() => mode = 'music')),
        _chip('ХРОН',
            on: chronOn,
            locked: chronLocked,
            onTap: () => setState(() {
                  if (chronLocked) return;
                  chronOn = !chronOn;
                })),
        const Spacer(),
        _goButton(goLabel, enabled: canDownload),
      ]),
      if (chronOn && !chronLocked)
        Positioned(top: 40, left: 0, child: _chronBox()),
    ]);
  }

  Widget _chip(String text, {bool on = false, bool locked = false, bool hidden = false, VoidCallback? onTap}) {
    if (hidden) return const SizedBox.shrink();
    return Padding(
      padding: const EdgeInsets.only(right: 9),
      child: GestureDetector(
        onTap: onTap,
        child: MouseRegion(
          cursor: locked ? SystemMouseCursors.forbidden : SystemMouseCursors.click,
          child: Opacity(
            opacity: locked ? .3 : 1,
            child: Container(
              padding: const EdgeInsets.symmetric(horizontal: 15, vertical: 8),
              color: on ? Pal.amber : Colors.transparent,
              child: Text(text,
                  style: T.h(19, c: on ? const Color(0xFF0A0500) : Pal.soft, ls: .06)),
            ),
          ),
        ),
      ),
    );
  }

  Widget _goButton(String label, {bool enabled = true}) {
    return GestureDetector(
      onTap: enabled ? _download : null,
      child: MouseRegion(
        cursor: enabled ? SystemMouseCursors.click : SystemMouseCursors.basic,
        child: Opacity(
          opacity: enabled ? 1 : .35,
          child: Container(
            padding: const EdgeInsets.symmetric(horizontal: 22, vertical: 10),
            decoration: BoxDecoration(
              color: Pal.amber,
              boxShadow: [BoxShadow(color: Pal.amber.withValues(alpha: .25), blurRadius: 16)],
            ),
            child: Text(label,
                style: T.h(20, w: FontWeight.w700, c: const Color(0xFF0A0500), ls: .06)),
          ),
        ),
      ),
    );
  }

  Widget _chronBox() {
    return Container(
      decoration: const BoxDecoration(
        color: Color(0xF5070400),
        boxShadow: [BoxShadow(color: Color(0x8C000000), offset: Offset(0, 10), blurRadius: 26)],
      ),
      child: DashedBox(
        padding: const EdgeInsets.all(12),
        child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
          Row(children: [
            Text('ОТ', style: T.h(16, ls: .14)),
            const SizedBox(width: 8),
            SizedBox(
                width: 66,
                child: TextField(
                  controller: chronFrom,
                  textAlign: TextAlign.center,
                  cursorColor: Pal.amber,
                  onSubmitted: (_) => FocusScope.of(context).unfocus(),
                  style: T.h(20, c: Pal.amber, ls: .04),
                  decoration: const InputDecoration(isCollapsed: true, border: InputBorder.none),
                )),
            const SizedBox(width: 8),
            Text('ДО', style: T.h(16, ls: .14)),
            const SizedBox(width: 8),
            SizedBox(
                width: 66,
                child: TextField(
                  controller: chronTo,
                  textAlign: TextAlign.center,
                  cursorColor: Pal.amber,
                  onSubmitted: (_) => FocusScope.of(context).unfocus(),
                  style: T.h(20, c: Pal.amber, ls: .04),
                  decoration: const InputDecoration(isCollapsed: true, border: InputBorder.none),
                )),
          ]),
          const SizedBox(height: 9),
          Text('ОТРЕЗОК · ТОЛЬКО ОДИНОЧНЫЙ ФАЙЛ · М:СС',
              style: T.h(13, w: FontWeight.w500, c: Pal.dim, ls: .1)),
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
    final title = it.title.isEmpty ? it.link : it.title;
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
          child: LedRow(
              count: 8, filled: done ? 8 : (it.progress * 8).round().clamp(0, 8), cellHeight: 14, gap: 3),
        ),
        const SizedBox(width: 8),
        if (it.state == 'working')
          _actButton(const Icon(Icons.close, size: 13, color: Pal.soft), 'Отменить',
              () => core?.cancel(it.id))
        else if (done) ...[
          _actButton(const FolderIcon(size: 14), 'Открыть папку', () {
            if (it.files.isNotEmpty) {
              _openPath(File(it.files.first).parent.path);
            } else {
              _openFolderButton();
            }
          }),
          const SizedBox(width: 6),
          _actButton(const TrashIcon(), 'Удалить (и файл с диска)', () => _trashRow(it)),
        ],
      ]),
    );
  }

  Widget _actButton(Widget child, String tooltip, VoidCallback onTap) {
    return Tooltip(
      message: tooltip,
      child: GestureDetector(
        onTap: onTap,
        child: MouseRegion(
          cursor: SystemMouseCursors.click,
          child: SizedBox(
            width: 27,
            height: 27,
            child: CustomPaint(
              foregroundPainter: const DashedBorderPainter(color: Color(0x73FFB000)),
              child: Center(child: child),
            ),
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
      child: GestureDetector(
        onTap: _startBeam,
        behavior: HitTestBehavior.opaque,
        child: Container(
          color: Colors.black,
          child: AnimatedBuilder(
            animation: beamC,
            builder: (context, _) => beamStarted
                ? CustomPaint(painter: BootBeamPainter(beamC.value))
                : Center(
                    child: Text('НАЖМИ, ЧТОБЫ ВКЛЮЧИТЬ',
                        style: T.h(16, w: FontWeight.w500, c: const Color(0xFF3A3428), ls: .3)),
                  ),
          ),
        ),
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
              tooltip: 'Папка загрузок',
              onPressed: _openFolderButton,
              child: const FolderIcon(),
            ),
          ),
        ),
        // логотип kvartal
        Positioned(
          left: 0,
          right: 0,
          top: 48,
          child: Transform.translate(
            offset: const Offset(0, -31),
            child: Center(
              child: MouseRegion(
                cursor: SystemMouseCursors.click,
                child: GestureDetector(
                  onTap: () => _openPath('https://kvartalrecords.ru'),
                  child: SvgPicture.asset('assets/kvartal.svg',
                      height: 62,
                      colorFilter: const ColorFilter.mode(Color(0xFF161412), BlendMode.srcIn)),
                ),
              ),
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
              tooltip: 'Поддержать на Boosty',
              onPressed: () => _openPath('https://boosty.to/kvartalrecords/donate'),
              child: const Text('\$',
                  style: TextStyle(
                      fontFamily: T.chakra,
                      fontWeight: FontWeight.w700,
                      fontSize: 20,
                      height: 1)),
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
