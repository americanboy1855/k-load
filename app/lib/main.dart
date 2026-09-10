import 'package:flutter/material.dart';

import 'ui/screen.dart';
import 'ui/theme.dart';

void main() {
  runApp(const KloadApp());
}

class KloadApp extends StatelessWidget {
  const KloadApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'K LOAD',
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        brightness: Brightness.dark,
        useMaterial3: true,
        scaffoldBackgroundColor: Pal.desk,
        colorScheme: const ColorScheme.dark(primary: Pal.amber, surface: Pal.desk),
        // Полоса прокрутки — часть интерфейса: тёмная тонкая дорожка
        // (цвет погасшего LED) и золотистый прямоугольный ползунок с
        // прямыми углами, без системной серой трубы.
        scrollbarTheme: ScrollbarThemeData(
          thickness: const WidgetStatePropertyAll(4.0),
          radius: Radius.zero,
          mainAxisMargin: 0,
          crossAxisMargin: 0,
          minThumbLength: 36,
          thumbVisibility: const WidgetStatePropertyAll(true),
          trackVisibility: const WidgetStatePropertyAll(true),
          trackColor: const WidgetStatePropertyAll(Pal.ledOff),
          trackBorderColor: const WidgetStatePropertyAll(Colors.transparent),
          thumbColor: WidgetStateProperty.resolveWith((states) {
            final active = states.contains(WidgetState.hovered) ||
                states.contains(WidgetState.dragged);
            return Pal.amber.withValues(alpha: active ? .9 : .45);
          }),
        ),
      ),
      home: const KLoadScreen(),
    );
  }
}
