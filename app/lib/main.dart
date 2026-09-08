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
      ),
      home: const KLoadScreen(),
    );
  }
}
