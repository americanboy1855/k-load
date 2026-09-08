import 'dart:math' as math;
import 'dart:ui' as ui;

import 'package:flutter/material.dart';

import 'theme.dart';

// Цвет «нажатой в пластик» иконки на корпусе + ховер-состояние плашки.
const plateIconIdle = Color(0xFF151312);

class PlateHover extends InheritedWidget {
  const PlateHover({super.key, required this.hover, required super.child});
  final bool hover;

  static PlateHover? of(BuildContext context) =>
      context.dependOnInheritedWidgetOfExactType<PlateHover>();

  @override
  bool updateShouldNotify(PlateHover old) => old.hover != hover;
}

// ---- иконки-стрелки из макета (тонкие, «пиксельные») ----
// Внутри Plate (PlateHover) иконки тёмные, при наведении — янтарные со
// свечением; вне плашки красятся своим color.

class ChainIcon extends StatelessWidget {
  const ChainIcon({super.key, this.size = 19, this.color = Pal.amber});
  final double size;
  final Color color;

  @override
  Widget build(BuildContext context) {
    final plate = PlateHover.of(context);
    final c = plate != null
        ? (plate.hover ? Pal.amber : plateIconIdle)
        : color;
    return CustomPaint(
        size: Size.square(size),
        painter: _StrokePainter((ctx, p, s) {
          final sc = s.width / 19;
          ctx.save();
          ctx.translate(s.width / 2, s.height / 2);
          ctx.rotate(-0.52);
          for (final dx in [-3.6 * sc, 3.6 * sc]) {
            ctx.drawRRect(
                RRect.fromRectAndRadius(
                    Rect.fromCenter(
                        center: Offset(dx, 0),
                        width: 8.2 * sc,
                        height: 5.0 * sc),
                    Radius.circular(2.5 * sc)),
                p);
          }
          ctx.restore();
        }, c, glow: plate?.hover ?? false));
  }
}

class FolderIcon extends StatelessWidget {
  const FolderIcon({super.key, this.size = 20, this.color = plateIconIdle, this.glow = false});
  final double size;
  final Color color;
  final bool glow;

  @override
  Widget build(BuildContext context) {
    final plate = PlateHover.of(context);
    final c = plate != null
        ? (plate.hover ? Pal.amber : plateIconIdle)
        : color;
    return CustomPaint(
        size: Size.square(size),
        painter: _FillPainter((ctx, p, s) {
          final sc = s.width / 20;
          final path = Path()
            ..moveTo(2.5 * sc, 5.5 * sc)
            ..relativeCubicTo(0, -0.8 * sc, 0.7 * sc, -1.5 * sc, 1.5 * sc, -1.5 * sc)
            ..relativeLineTo(3.2 * sc, 0)
            ..relativeCubicTo(0.4 * sc, 0, 0.8 * sc, 0.2 * sc, 1.1 * sc, 0.5 * sc)
            ..relativeLineTo(0.9 * sc, 1.0 * sc)
            ..relativeLineTo(7.3 * sc, 0)
            ..relativeCubicTo(0.8 * sc, 0, 1.5 * sc, 0.7 * sc, 1.5 * sc, 1.5 * sc)
            ..relativeLineTo(0, 7.5 * sc)
            ..relativeCubicTo(0, 0.8 * sc, -0.7 * sc, 1.5 * sc, -1.5 * sc, 1.5 * sc)
            ..lineTo(4 * sc, 16.5 * sc)
            ..relativeCubicTo(-0.8 * sc, 0, -1.5 * sc, -0.7 * sc, -1.5 * sc, -1.5 * sc)
            ..close();
          ctx.drawPath(path, p);
        }, c, glow: glow || (plate?.hover ?? false)));
  }
}

class TrashIcon extends StatelessWidget {
  const TrashIcon({super.key, this.size = 13, this.color = Pal.white});
  final double size;
  final Color color;

  @override
  Widget build(BuildContext context) {
    final plate = PlateHover.of(context);
    final c = plate != null
        ? (plate.hover ? Pal.amber : plateIconIdle)
        : color;
    return CustomPaint(
        size: Size.square(size),
        painter: _StrokePainter((ctx, p, s) {
          final sc = s.width / 14;
          final path = Path()
            ..moveTo(2 * sc, 3.5 * sc)
            ..lineTo(12 * sc, 3.5 * sc)
            ..moveTo(5.5 * sc, 3.5 * sc)
            ..lineTo(5.5 * sc, 2 * sc)
            ..lineTo(8.5 * sc, 2 * sc)
            ..lineTo(8.5 * sc, 3.5 * sc)
            ..moveTo(3.5 * sc, 3.5 * sc)
            ..lineTo(4.3 * sc, 12 * sc)
            ..lineTo(9.7 * sc, 12 * sc)
            ..lineTo(10.5 * sc, 3.5 * sc);
          ctx.drawPath(path, p);
        }, c));
  }
}

class VpnIcon extends StatelessWidget {
  const VpnIcon({super.key, this.color = Pal.amber});
  final Color color;

  @override
  Widget build(BuildContext context) => CustomPaint(
      size: const Size(14, 16),
      painter: _FillPainter((ctx, p, s) {
        final sc = s.width / 14;
        ctx.drawRect(Rect.fromLTWH(6 * sc, 1 * sc, 3 * sc, 9 * sc), p);
        ctx.drawRect(Rect.fromLTWH(6 * sc, 12 * sc, 3 * sc, 3 * sc), p);
      }, color));
}

class _FillPainter extends CustomPainter {
  _FillPainter(this.painter, this.color, {this.glow = false});
  final void Function(Canvas canvas, Paint paint, Size size) painter;
  final Color color;
  final bool glow;

  @override
  void paint(Canvas canvas, Size size) {
    if (glow) {
      final g = Paint()
        ..color = Pal.amber.withValues(alpha: .55)
        ..maskFilter = const MaskFilter.blur(ui.BlurStyle.normal, 4);
      painter(canvas, g, size);
    }
    final p = Paint()..color = color;
    painter(canvas, p, size);
  }

  @override
  bool shouldRepaint(covariant _FillPainter old) =>
      old.color != color || old.glow != glow;
}

class _StrokePainter extends CustomPainter {
  _StrokePainter(this.painter, this.color, {this.glow = false});
  final void Function(Canvas canvas, Paint paint, Size size) painter;
  final Color color;
  final bool glow;

  @override
  void paint(Canvas canvas, Size size) {
    if (glow) {
      final g = Paint()
        ..color = Pal.amber.withValues(alpha: .55)
        ..style = PaintingStyle.stroke
        ..strokeWidth = 1.4
        ..maskFilter = const MaskFilter.blur(ui.BlurStyle.normal, 4);
      painter(canvas, g, size);
    }
    final p = Paint()
      ..color = color
      ..style = PaintingStyle.stroke
      ..strokeWidth = 1.4;
    painter(canvas, p, size);
  }

  @override
  bool shouldRepaint(covariant _StrokePainter old) =>
      old.color != color || old.glow != glow;
}

// ---- LED-полосы ----

// Мгновенная полоса (шкала ПОИСКа).
class LedRow extends StatelessWidget {
  const LedRow({super.key, required this.count, required this.filled, this.cellHeight = 16, this.gap = 4});
  final int count;
  final int filled;
  final int cellHeight;
  final double gap;

  @override
  Widget build(BuildContext context) {
    return LayoutBuilder(builder: (context, box) {
      final cell = (box.maxWidth - gap * (count - 1)) / count;
      return Row(
        children: [
          for (var i = 0; i < count; i++)
            Container(
              width: cell,
              height: cellHeight.toDouble(),
              margin: EdgeInsets.only(right: i == count - 1 ? 0 : gap),
              decoration: BoxDecoration(
                color: i < filled ? Pal.amber : Pal.ledOff,
                borderRadius: const BorderRadius.all(Radius.circular(1)),
                boxShadow: i < filled
                    ? [BoxShadow(color: Pal.amber.withValues(alpha: .55), blurRadius: 9)]
                    : null,
              ),
            ),
        ],
      );
    });
  }
}

// Полоса очереди: плавно догоняет реальный прогресс (нестабильная индикация
// видеомагнитофона — ведущий сегмент слабо подмигивает).
class AnimatedLedRow extends StatefulWidget {
  const AnimatedLedRow(
      {super.key,
      required this.count,
      required this.progress,
      this.cellHeight = 14,
      this.gap = 3,
      this.blinking = false});
  final int count;
  final double progress; // 0..1
  final int cellHeight;
  final double gap;
  final bool blinking;

  @override
  State<AnimatedLedRow> createState() => _AnimatedLedRowState();
}

class _AnimatedLedRowState extends State<AnimatedLedRow>
    with SingleTickerProviderStateMixin {
  late final AnimationController _blink =
      AnimationController(vsync: this, duration: const Duration(milliseconds: 1100))
        ..repeat();

  @override
  void dispose() {
    _blink.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return LayoutBuilder(builder: (context, box) {
      final cell = (box.maxWidth - widget.gap * (widget.count - 1)) / widget.count;
      final target = (widget.progress * widget.count).clamp(0.0, widget.count.toDouble());
      return TweenAnimationBuilder<double>(
        tween: Tween(end: target),
        // Полоса заметно медленнее реального прогресса — так честнее выглядит.
        duration: const Duration(milliseconds: 900),
        curve: Curves.easeOutCubic,
        builder: (context, value, _) {
          return AnimatedBuilder(
            animation: _blink,
            builder: (context, _) {
              final lead = value.floor();
              final leadFrac = value - lead;
              // Последний заполненный сегмент слабо подмигивает.
              final blinkCell = widget.blinking
                  ? (leadFrac > .55 ? lead : lead - 1)
                  : -1;
              final dim = .6 + .4 * _blink.value;
              return Row(
                children: [
                  for (var i = 0; i < widget.count; i++)
                    Container(
                      width: cell,
                      height: widget.cellHeight.toDouble(),
                      margin: EdgeInsets.only(
                          right: i == widget.count - 1 ? 0 : widget.gap),
                      decoration: BoxDecoration(
                        color: i <= lead && value > i ? Pal.amber : Pal.ledOff,
                        borderRadius:
                            const BorderRadius.all(Radius.circular(1)),
                        boxShadow: i <= lead && value > i
                            ? [BoxShadow(
                                color: Pal.amber.withValues(
                                    alpha: .55 *
                                        (i == blinkCell ? dim : 1.0)),
                                blurRadius: 9)]
                            : null,
                      ),
                    ),
                ],
              );
            },
          );
        },
      );
    });
  }
}

// ---- кинескоп ----

// Фон экрана: сканлайны, виньетка «рыбий глаз», блик. Рисуется ПОД текстом,
// поэтому буквы остаются резкими.
class GlassBackdrop extends StatelessWidget {
  const GlassBackdrop({super.key});

  @override
  Widget build(BuildContext context) {
    return IgnorePointer(
      child: CustomPaint(size: Size.infinite, painter: _BackdropPainter()),
    );
  }
}

class _BackdropPainter extends CustomPainter {
  @override
  void paint(Canvas canvas, Size size) {
    final line = Paint()..color = const Color(0x22000000);
    for (var y = 0.0; y < size.height; y += 3) {
      canvas.drawLine(Offset(0, y), Offset(size.width, y), line);
    }
    final vignette = Paint()
      ..shader = RadialGradient(
        center: const Alignment(0, -0.1),
        radius: 1.4,
        colors: const [
          Color(0x00151410),
          Color(0x59100E0A),
          Color(0xA0000000),
        ],
        stops: const [0.45, 0.78, 1],
      ).createShader(Offset.zero & size);
    canvas.drawRect(Offset.zero & size, vignette);
    final sheen = Paint()
      ..shader = RadialGradient(
        center: const Alignment(0, -0.45),
        radius: 1.1,
        colors: const [Color(0x0DFFECC8), Color(0x00FFECC8)],
      ).createShader(Offset.zero & size);
    canvas.drawRect(Offset.zero & size, sheen);
  }

  @override
  bool shouldRepaint(covariant _BackdropPainter old) => false;
}

// Вуаль над контентом: редкое слабое мерцание + аналоговый шум (почти
// незаметные, читаемость не трогают).
class GlassVeil extends StatelessWidget {
  const GlassVeil({super.key, this.flicker = 0, this.noiseSeed = 0});
  final double flicker; // 0..0.03
  final int noiseSeed;

  @override
  Widget build(BuildContext context) {
    return IgnorePointer(
      child: CustomPaint(
        size: Size.infinite,
        painter: _VeilPainter(flicker, noiseSeed),
      ),
    );
  }
}

class _VeilPainter extends CustomPainter {
  _VeilPainter(this.flicker, this.noiseSeed);
  final double flicker;
  final int noiseSeed;

  @override
  void paint(Canvas canvas, Size size) {
    // аналоговый шум: редкие зёрна, меняются время от времени
    final rnd = math.Random(noiseSeed);
    for (var i = 0; i < 260; i++) {
      final x = rnd.nextDouble() * size.width;
      final y = rnd.nextDouble() * size.height;
      final bright = rnd.nextBool();
      canvas.drawRect(
          Rect.fromLTWH(x, y, 2, 1),
          Paint()
            ..color = (bright ? Colors.white : Colors.black)
                .withValues(alpha: .03));
    }
    if (flicker > 0) {
      canvas.drawRect(
          Offset.zero & size,
          Paint()
            ..color = Colors.white.withValues(alpha: flicker.clamp(0, .03)));
    }
  }

  @override
  bool shouldRepaint(covariant _VeilPainter old) =>
      old.flicker != flicker || old.noiseSeed != noiseSeed;
}

// ---- строка трекинга, изредка пробегает по экрану (узкая, как в ТВ) ----

class TrackingBar extends StatelessWidget {
  const TrackingBar({super.key, required this.progress});
  final double progress; // 0..1

  @override
  Widget build(BuildContext context) {
    return Align(
      alignment: Alignment(-1, -1 + progress * 2),
      child: Container(
        height: 8,
        decoration: BoxDecoration(
          gradient: LinearGradient(colors: [
            Colors.white.withValues(alpha: 0),
            Colors.white.withValues(alpha: .05),
            Colors.white.withValues(alpha: .085),
            Colors.white.withValues(alpha: .03),
            Colors.white.withValues(alpha: 0),
          ]),
        ),
      ),
    );
  }
}

// ---- boot: луч кинескопа (по кадрам из макета) ----

class BootBeamPainter extends CustomPainter {
  BootBeamPainter(this.t); // 0..1 за 1.5с
  final double t;

  @override
  void paint(Canvas canvas, Size size) {
    double sx, sy, opacity;
    if (t < 0.12) {
      final k = t / 0.12;
      sx = sy = 0.004 + 0.002 * k;
      opacity = k;
    } else if (t < 0.42) {
      final k = (t - 0.12) / 0.30;
      sx = 0.006 + (1 - 0.006) * k;
      sy = 0.006;
      opacity = 1;
    } else if (t < 0.62) {
      final k = (t - 0.42) / 0.20;
      sx = 1;
      sy = 0.006 + (1 - 0.006) * k;
      opacity = 0.95;
    } else if (t < 0.80) {
      sx = sy = 1;
      opacity = 0.95 - 0.35 * ((t - 0.62) / 0.18);
    } else {
      sx = sy = 1;
      opacity = 0.6 * (1 - (t - 0.80) / 0.20);
    }
    final w = size.width * sx;
    final h = size.height * sy;
    final rect = Rect.fromCenter(
        center: size.center(Offset.zero),
        width: math.max(w, 2),
        height: math.max(h, 2));
    final paint = Paint()
      ..color = Colors.white.withValues(alpha: opacity.clamp(0, 1))
      ..maskFilter = const MaskFilter.blur(BlurStyle.solid, 2);
    // ореол
    canvas.drawRect(rect, Paint()..color = Colors.white.withValues(alpha: opacity * 0.25)..maskFilter = const MaskFilter.blur(BlurStyle.normal, 24));
    canvas.drawRRect(
        RRect.fromRectAndRadius(rect, Radius.circular(12 * (t > 0.42 ? 1 : 0))),
        paint);
  }

  @override
  bool shouldRepaint(covariant BootBeamPainter old) => old.t != t;
}

// ---- круглая «плашка» на корпусе (папка / доллар) ----

class Plate extends StatefulWidget {
  const Plate({super.key, required this.child, this.onPressed, this.tooltip});
  final Widget child;
  final VoidCallback? onPressed;
  final String? tooltip;

  @override
  State<Plate> createState() => _PlateState();
}

class _PlateState extends State<Plate> {
  bool hover = false;
  bool pressed = false;

  @override
  Widget build(BuildContext context) {
    Widget plate = GestureDetector(
      onTap: widget.onPressed,
      child: MouseRegion(
        cursor: SystemMouseCursors.click,
        onEnter: (_) => setState(() => hover = true),
        onExit: (_) => setState(() => hover = false),
        child: AnimatedContainer(
          duration: const Duration(milliseconds: 250),
          width: 46,
          height: 46,
          transform: Matrix4.translationValues(0, pressed ? 1 : 0, 0),
          decoration: BoxDecoration(
            shape: BoxShape.circle,
            border: Border.all(color: Pal.plasticEdge),
            gradient: const RadialGradient(
              center: Alignment(-0.3, -0.45),
              colors: [Color(0xFF413D39), Color(0xFF2A2724), Color(0xFF1B1917)],
              stops: [0, 0.52, 1],
            ),
            boxShadow: [
              BoxShadow(color: Colors.black.withValues(alpha: .6), offset: const Offset(0, 3), blurRadius: 5),
              BoxShadow(color: Colors.black.withValues(alpha: .5), offset: const Offset(0, 1), blurRadius: 2),
              const BoxShadow(color: Color(0x1DFFFFFF), offset: Offset(0, 1), blurRadius: 0, spreadRadius: -1),
              if (hover)
                BoxShadow(
                    color: Pal.amber.withValues(alpha: .30), blurRadius: 18),
            ],
          ),
          child: Center(
            child: AnimatedScale(
              scale: pressed ? 0.97 : 1,
              duration: const Duration(milliseconds: 60),
              child: PlateHover(
                hover: hover,
                child: widget.child,
              ),
            ),
          ),
        ),
      ),
    );
    if (widget.tooltip != null) {
      plate = Tooltip(message: widget.tooltip!, child: plate);
    }
    return Listener(
      onPointerDown: (_) => setState(() => pressed = true),
      onPointerUp: (_) => setState(() => pressed = false),
      onPointerCancel: (_) => setState(() => pressed = false),
      child: plate,
    );
  }
}
