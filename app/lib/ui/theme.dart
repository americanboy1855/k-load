import 'package:flutter/material.dart';

// Палитра B «amber terminal» + пластик корпуса — 1:1 из design/main-screen.html.
class Pal {
  static const desk = Color(0xFF0B0B0D);
  static const plastic = Color(0xFF262321);
  static const plasticEdge = Color(0xFF0C0B0A);
  static const amber = Color(0xFFFFB000);
  static const soft = Color(0xFFFFCF7A);
  static const dim = Color(0xFF8A6E38);
  static const white = Color(0xFFEDE8DC);
  static const error = Color(0xFFFF5F57);
  static const ledOff = Color(0xFF241A08);

  static const amberSoft = Color(0x8CFFB000); // 55%
  static const amberFaint = Color(0x40FFB000); // 25%
  static const amberGhost = Color(0x26FFB000); // 15%

  static const screenBg = Color(0xFF070402);
}

// Типографика макета: Handjet — все цифры и слова на экране,
// IBM Plex Mono — только технический ×.
class T {
  static const handjet = 'Handjet';
  static const chakra = 'Chakra Petch';
  static const plex = 'IBM Plex Mono';

  static TextStyle h(double size,
      {FontWeight w = FontWeight.w600, Color c = Pal.soft, double ls = 0,
      bool glow = false}) {
    return TextStyle(
        fontFamily: handjet,
        fontSize: size,
        fontWeight: w,
        color: c,
        letterSpacing: ls,
        height: 1.05,
        shadows: glow
            ? [Shadow(color: c.withValues(alpha: .5), blurRadius: 9)]
            : null);
  }
}

// Пунктирная (или цельная — solid) рамка — фирменный приём макета.
class DashedBorderPainter extends CustomPainter {
  const DashedBorderPainter(
      {this.color = Pal.amberSoft,
      this.radius = 0,
      this.dash = 5,
      this.gap = 4,
      this.solid = false});
  final Color color;
  final double radius;
  final double dash;
  final double gap;
  final bool solid;

  @override
  void paint(Canvas canvas, Size size) {
    final paint = Paint()
      ..style = PaintingStyle.stroke
      ..strokeWidth = 1
      ..color = color;
    final rrect = RRect.fromRectAndRadius(Offset.zero & size, Radius.circular(radius));
    final path = Path()..addRRect(rrect);
    if (solid) {
      canvas.drawPath(path, paint);
      return;
    }
    for (final metric in path.computeMetrics()) {
      var dist = 0.0;
      while (dist < metric.length) {
        final next = (dist + dash).clamp(0.0, metric.length);
        canvas.drawPath(metric.extractPath(dist, next), paint);
        dist = next + gap;
      }
    }
  }

  @override
  bool shouldRepaint(covariant DashedBorderPainter old) =>
      old.color != color || old.radius != radius || old.solid != solid;
}

// Контейнер с пунктирной рамкой.
class DashedBox extends StatelessWidget {
  const DashedBox(
      {super.key,
      required this.child,
      this.color = Pal.amberSoft,
      this.radius = 0,
      this.padding = EdgeInsets.zero,
      this.onTap,
      this.alignment});
  final Widget child;
  final Color color;
  final double radius;
  final EdgeInsets padding;
  final VoidCallback? onTap;
  final AlignmentGeometry? alignment;

  @override
  Widget build(BuildContext context) {
    return GestureDetector(
      onTap: onTap,
      child: CustomPaint(
        foregroundPainter: DashedBorderPainter(color: color, radius: radius),
        child: Padding(padding: padding, child: alignment == null ? child : Align(alignment: alignment!, child: child)),
      ),
    );
  }
}
