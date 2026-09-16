// Регрессия: клик по [ОЧИСТИТЬ] в хедере диспетчера (репорт пользователя).
// _TextLink приватный — здесь его точная копия.
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

class _TextLink extends StatefulWidget {
  const _TextLink({
    required this.label,
    required this.onTap,
    required this.style,
  });
  final String label;
  final VoidCallback onTap;
  final TextStyle style;

  @override
  State<_TextLink> createState() => _TextLinkState();
}

class _TextLinkState extends State<_TextLink> {
  bool _hover = false;

  void _setHover(bool v) {
    if (_hover != v) setState(() => _hover = v);
  }

  @override
  Widget build(BuildContext context) {
    return GestureDetector(
      behavior: HitTestBehavior.opaque,
      onTap: () {
        _setHover(false);
        widget.onTap();
      },
      child: MouseRegion(
        cursor: SystemMouseCursors.click,
        onEnter: (_) => _setHover(true),
        onExit: (_) => _setHover(false),
        onHover: (_) => _setHover(true),
        child: TweenAnimationBuilder<double>(
          tween: Tween(end: _hover ? 1.0 : 0.0),
          duration: const Duration(milliseconds: 220),
          curve: Curves.easeOut,
          builder: (context, t, _) => Text(
            widget.label,
            style: widget.style.copyWith(
              color: Color.lerp(widget.style.color ?? const Color(0xFF888888),
                  const Color(0xFFFFB000), t),
            ),
          ),
        ),
      ),
    );
  }
}

void main() {
  testWidgets('текст-ссылка получает тап', (tester) async {
    bool tapped = false;
    await tester.pumpWidget(MaterialApp(
      home: Scaffold(
        body: Center(
          child: _TextLink(
            label: '[ОЧИСТИТЬ]',
            onTap: () => tapped = true,
            style: const TextStyle(fontSize: 9),
          ),
        ),
      ),
    ));
    await tester.tap(find.text('[ОЧИСТИТЬ]'), warnIfMissed: false);
    await tester.pumpAndSettle();
    expect(tapped, isTrue, reason: 'тап по _TextLink должен срабатывать');
  });

  testWidgets('тот же виджет в строке хедера (Row+Spacer+Transform)', (tester) async {
    bool tapped = false;
    await tester.pumpWidget(MaterialApp(
      home: Scaffold(
        body: SizedBox(
          width: 500,
          child: Padding(
            padding: const EdgeInsets.fromLTRB(13, 6, 13, 2),
            child: Row(
              crossAxisAlignment: CrossAxisAlignment.baseline,
              textBaseline: TextBaseline.alphabetic,
              children: [
                const Text('ДИСПЕТЧЕР ЗАГРУЗОК'),
                const Spacer(),
                Transform.translate(
                  offset: const Offset(3.1, 0),
                  child: _TextLink(
                    label: '[ОЧИСТИТЬ]',
                    onTap: () => tapped = true,
                    style: const TextStyle(fontSize: 9),
                  ),
                ),
              ],
            ),
          ),
        ),
      ),
    ));
    final center = tester.getCenter(find.text('[ОЧИСТИТЬ]'));
    await tester.tapAt(center);
    await tester.pumpAndSettle();
    expect(tapped, isTrue,
        reason: 'тап по центру текста в хедере должен доходить');
  });
}
