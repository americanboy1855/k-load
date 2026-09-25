// Диагностика окна на реальной Windows-машине (CI-раннер):
// программно меняем размер окна, снимаем содержимое (PrintWindow) и
// проверяем канал beginDrag (HRESULT от DoDragDrop). Результаты — файлы
// в <temp>/kdiag: сырые BGRA-кадры (конвертация в PNG локально) + results.txt.

import 'dart:ffi';
import 'dart:io';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';

import 'package:kload/main.dart' as app;

final DynamicLibrary _user32 = DynamicLibrary.open('user32.dll');
final DynamicLibrary _gdi32 = DynamicLibrary.open('gdi32.dll');

final int Function(Pointer<Utf16>, Pointer<Utf16>) _findWindowW = _user32
    .lookupFunction<Pointer<Utf16> Function(Pointer<Utf16>, Pointer<Utf16>),
        int Function(Pointer<Utf16>, Pointer<Utf16>)>('FindWindowW');
final int Function(int, int, int, int, int, int, int) _setWindowPos = _user32
    .lookupFunction<Int32 Function(IntPtr, IntPtr, Int32, Int32, Int32, Int32, Uint32),
        int Function(int, int, int, int, int, int, int)>('SetWindowPos');
final int Function(int, Pointer<Uint8>) _getClientRect = _user32.lookupFunction<
    Int32 Function(IntPtr, Pointer<Uint8>),
    int Function(int, Pointer<Uint8>)>('GetClientRect');
final int Function(int, int) _getWindowDC = _user32
    .lookupFunction<Int32 Function(IntPtr), int Function(IntPtr)>('GetWindowDC');
final int Function(int, int) _releaseDC = _user32
    .lookupFunction<Int32 Function(IntPtr), int Function(IntPtr)>('ReleaseDC');
final int Function(int) _createCompatibleDC = _gdi32
    .lookupFunction<Int32 Function(IntPtr), int Function(IntPtr)>('CreateCompatibleDC');
final int Function(int) _deleteDC = _gdi32
    .lookupFunction<Int32 Function(IntPtr), int Function(IntPtr)>('DeleteDC');
final int Function(int, int, int) _createCompatibleBitmap = _gdi32.lookupFunction<
    Int32 Function(IntPtr, Int32, Int32),
    int Function(int, int, int)>('CreateCompatibleBitmap');
final int Function(int, int) _selectObject = _gdi32
    .lookupFunction<Int32 Function(IntPtr, int), int Function(int, int)>('SelectObject');
final int Function(int, int, int) _printWindow = _user32.lookupFunction<
    Int32 Function(IntPtr, int, Uint32),
    int Function(int, int, Uint32)>('PrintWindowW');
final int Function(int, int, int, int, Pointer<Uint8>, Pointer<Uint8>, int)
    _getDIBits = _gdi32.lookupFunction<
        Int32 Function(IntPtr, int, Uint32, Int32, Pointer<Uint8>, Pointer<Uint8>, Uint32),
        int Function(int, int, Uint32, int, Pointer<Uint8>, Pointer<Uint8>, Uint32)>('GetDIBits');
final int Function(int) _deleteObject = _gdi32
    .lookupFunction<Int32 Function(IntPtr), int Function(IntPtr)>('DeleteObject');

final _results = <String>[];
void _log(String s) {
  _results.add(s);
  // ignore: avoid_print
  print('DIAG: $s');
}

(int, int, int, int) _clientRect(int hwnd) {
  final p = calloc<Uint8>(16);
  _getClientRect(hwnd, p);
  final bd = p.asTypedList(16).buffer.asByteData();
  final l = bd.getInt32(0, Endian.host);
  final t = bd.getInt32(4, Endian.host);
  final r = bd.getInt32(8, Endian.host);
  final b = bd.getInt32(12, Endian.host);
  calloc.free(p);
  return (l, t, r, b);
}

/// PrintWindow (PW_CLIENTONLY|PW_RENDERFULLCONTENT) → сырой BGRA на диск.
void _capture(int hwnd, String path) {
  final (l, t, r, b) = _clientRect(hwnd);
  final w = r - l, h = b - t;
  final hdc = _getWindowDC(hwnd);
  final mem = _createCompatibleDC(hdc);
  final bmp = _createCompatibleBitmap(hdc, w, h);
  final old = _selectObject(mem, bmp);
  final ok = _printWindow(hwnd, mem, 3);
  _selectObject(mem, old);

  final bi = calloc<Uint8>(40);
  final bibd = bi.asTypedList(40).buffer.asByteData();
  bibd.setUint32(0, 40, Endian.little);
  bibd.setInt32(4, w, Endian.little);
  bibd.setInt32(8, -h, Endian.little); // top-down
  bibd.setUint16(12, 1, Endian.little);
  bibd.setUint16(14, 32, Endian.little);

  final pixels = calloc<Uint8>(w * h * 4);
  _getDIBits(mem, bmp, 0, h, pixels, bi, 0);
  File(path).writeAsBytesSync(pixels);

  calloc.free(pixels);
  calloc.free(bi);
  _deleteObject(bmp);
  _deleteDC(mem);
  _releaseDC(hwnd, hdc);
  _log('кадр: $path (${w}x$h, printWindow ok=$ok)');
}

void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();
  testWidgets('диагностика окна Windows', (tester) async {
    app.main();
    // boot-луч + проявление интерфейса
    await tester.pump(const Duration(seconds: 5));
    await tester.pump(const Duration(seconds: 2));

    final titleUtf16 = 'K LOAD'.toNativeUtf16();
    final hwnd = _findWindowW(nullptr, titleUtf16);
    calloc.free(titleUtf16);
    _log('hwnd = $hwnd');
    if (hwnd == 0) {
      _log('ОКНО НЕ НАЙДЕНО — тест прерван');
      return;
    }

    final dir = Directory('${Directory.systemTemp.path}/kdiag');
    dir.createSync(recursive: true);

    // Канал beginDrag: HRESULT от DoDragDrop (файл существует — цикл
    // стартует и мгновенно завершится: кнопок мыши мы не жмём).
    final tmpFile = File('${dir.path}/drag-probe.txt')..writeAsStringSync('probe');
    int hr = -1;
    Object? err;
    try {
      hr = await const MethodChannel('kload/native')
              .invokeMethod<int>('beginDrag', tmpFile.path) ??
          -1;
    } on Object catch (e) {
      err = e;
    }
    _log('beginDrag: hr=0x${hr.toRadixString(16)} err=$err');

    // Серия размеров: база, пропорция, ЛОМАНАЯ пропорция (ловим деформацию),
    // минимум, максимум.
    final sizes = <(int, int, String)>[
      (560, 670, 'base-560x670'),
      (700, 837, 'aspect-700x837'),
      (700, 938, 'wrong-700x938'),
      (448, 536, 'min-448x536'),
      (784, 938, 'max-784x938'),
    ];
    final mq = MediaQueryData.fromView(View.of(
        tester.state(find.byType(MaterialApp).first)));
    _log('MediaQuery: ${mq.size} dpr=${mq.devicePixelRatio}');

    for (final (w, h, name) in sizes) {
      _setWindowPos(hwnd, 0, 80, 80, w, h, 0x0004); // SWP_NOZORDER
      // даём liveSize-каналу и Dart-компоновке отработать
      for (var i = 0; i < 10; i++) {
        await tester.pump(const Duration(milliseconds: 100));
      }
      final (l, t, r, b) = _clientRect(hwnd);
      final mqNow =
          MediaQueryData.fromView(View.of(tester.state(find.byType(MaterialApp).first)));
      _log('размер $name: окно клиент ${r - l}x${b - t}, '
          'MediaQuery ${mqNow.size}');
      _capture(hwnd, '${dir.path}/$name.png.data');
    }

    File('${dir.path}/results.txt').writeAsStringSync('${_results.join("\n")}\n');
    _log('ДИАГНОСТИКА ЗАВЕРШЕНА');
  });
}
