import 'dart:async';

import 'package:flutter/material.dart';

import 'kd/kd_bindings.dart';

void main() {
  runApp(const KloadApp());
}

class KloadApp extends StatelessWidget {
  const KloadApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'K LOAD',
      theme: ThemeData(brightness: Brightness.dark, useMaterial3: true),
      home: const TechShell(),
    );
  }
}

/// ТЕХНИЧЕСКИЙ КАРКАС: проверка трубы «UI → FFI → kdcore → события → UI».
/// Дизайн собирается блоками по референсам заказчика — этот экран-заглушка
/// идёт под замену целиком.
class TechShell extends StatefulWidget {
  const TechShell({super.key});

  @override
  State<TechShell> createState() => _TechShellState();
}

class _TechShellState extends State<TechShell> {
  KdCore? core;
  String status = 'поднимаю ядро…';
  List<KdItem> items = [];
  KdProbeEvent? probe;
  bool vpnOn = false;
  Map<String, dynamic>? tools;
  Map<String, dynamic>? dest;
  final input = TextEditingController();
  StreamSubscription<KdEvent>? sub;

  @override
  void initState() {
    super.initState();
    _boot();
  }

  Future<void> _boot() async {
    try {
      final c = KdCore.start();
      sub = c.events.listen((e) {
        if (e is KdProbeEvent) probe = e;
        if (e is KdVpnEvent) vpnOn = e.on;
        setState(() => items = c.snapshot());
      });
      setState(() {
        core = c;
        items = c.snapshot();
        tools = c.toolsStatus();
        dest = c.defaultDest();
        vpnOn = c.vpnState() == 1;
        status = 'ядро ${c.version} · инструменты: '
            '${tools!['found'] == true ? 'найдены' : 'НЕ найдены'}';
        debugPrint('KD_BOOT: $status · dest: ${dest!['folder']}');
      });
    } on Object catch (e) {
      setState(() => status = 'ОШИБКА ЯДРА: $e');
    }
  }

  @override
  void dispose() {
    sub?.cancel();
    input.dispose();
    super.dispose();
  }

  void _enqueue() {
    final c = core;
    if (c == null) return;
    final links = c.splitLinks(input.text);
    if (links.isNotEmpty) {
      c.enqueueBatch(links, quality: 'best');
    } else if (input.text.trim().isNotEmpty) {
      // Название трека: фоновый разбор; карточка и «качать» — следующий блок.
      c.probeAsync(input.text.trim());
    }
    setState(() => items = c.snapshot());
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      body: Padding(
        padding: const EdgeInsets.all(20),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            Text('K LOAD — технический каркас',
                style: Theme.of(context).textTheme.titleLarge),
            const SizedBox(height: 4),
            Text(status, style: Theme.of(context).textTheme.bodySmall),
            Text('VPN: ${vpnOn ? 'включён' : 'выключен'}',
                style: Theme.of(context).textTheme.bodySmall),
            if (dest != null)
              Text('Папка: ${dest!['folder']}',
                  style: Theme.of(context).textTheme.bodySmall),
            const Divider(height: 24),
            TextField(
              controller: input,
              maxLines: 3,
              decoration: const InputDecoration(
                hintText: 'Ссылки или название трека — по одной в строке',
                border: OutlineInputBorder(),
              ),
            ),
            const SizedBox(height: 10),
            Wrap(
              spacing: 8,
              children: [
                FilledButton(onPressed: _enqueue, child: const Text('Разобрать / в очередь')),
                OutlinedButton(
                  onPressed: core == null ? null : () => core!.probeAsync(input.text.trim()),
                  child: const Text('probe'),
                ),
                OutlinedButton(
                  onPressed: core == null ? null : () => setState(() => core!.clearFinished()),
                  child: const Text('очистить завершённые'),
                ),
              ],
            ),
            if (probe != null) ...[
              const SizedBox(height: 12),
              Text(probe!.ok
                  ? 'РАЗБОР: [${probe!.serviceTitle}] ${probe!.title}'
                  : 'РАЗБОР: ${probe!.error}'),
            ],
            const SizedBox(height: 12),
            Expanded(
              child: ListView.builder(
                itemCount: items.length,
                itemBuilder: (_, i) {
                  final it = items[i];
                  return Card(
                    child: ListTile(
                      title: Text(it.title.isEmpty ? it.link : it.title,
                          maxLines: 1, overflow: TextOverflow.ellipsis),
                      subtitle: Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: [
                          Text('${it.serviceTitle} · ${it.stage}'),
                          if (it.state == 'working')
                            LinearProgressIndicator(value: it.progress),
                          for (final f in it.files)
                            Text(f, style: const TextStyle(fontSize: 11)),
                        ],
                      ),
                      trailing: Wrap(spacing: 4, children: [
                        if (it.state == 'working')
                          IconButton(
                              onPressed: () => core!.cancel(it.id),
                              icon: const Icon(Icons.close)),
                        IconButton(
                            onPressed: () => core!.remove(it.id),
                            icon: const Icon(Icons.delete_outline)),
                      ]),
                    ),
                  );
                },
              ),
            ),
          ],
        ),
      ),
    );
  }
}
