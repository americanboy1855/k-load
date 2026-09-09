import Cocoa
import FlutterMacOS

class MainFlutterWindow: NSWindow {
  override func awakeFromNib() {
    let flutterViewController = FlutterViewController()
    let windowFrame = self.frame
    self.contentViewController = flutterViewController
    self.setFrame(windowFrame, display: true)

    // K LOAD — «телевизор»: рамку рисует само приложение, от системы
    // поверх нашей панели остаются только кнопки светофора.
    titleVisibility = .hidden
    titlebarAppearsTransparent = true
    styleMask.insert(.fullSizeContentView)
    isOpaque = false
    // Пластик корпуса, не чёрный: на закруглённых углах окна микрощелей нет.
    backgroundColor = NSColor(red: 0x16/255.0, green: 0x14/255.0, blue: 0x13/255.0, alpha: 1)
    // Во весь экран недоступно: зум-кнопка мертва и комбинация не срабатывает.
    standardWindowButton(.zoomButton)?.isEnabled = false
    collectionBehavior.insert(.fullScreenDisallowsTiling)

    // Окно 560×670; тянется только по диагонали, пропорционально.
    let design = NSSize(width: 560, height: 670)
    setContentSize(design)
    contentAspectRatio = design
    minSize = NSSize(width: design.width * 0.8, height: design.height * 0.8)
    maxSize = NSSize(width: design.width * 1.4, height: design.height * 1.4)
    center()

    RegisterGeneratedPlugins(registry: flutterViewController)

    // Мост для системных диалогов и drag-out скачанных файлов.
    let channel = FlutterMethodChannel(
        name: "kload/native",
        binaryMessenger: flutterViewController.engine.binaryMessenger)
    let dragHelper = DragOutHelper(contentView: contentView!)
    channel.setMethodCallHandler { call, result in
        switch call.method {
        case "chooseFolder":
            let panel = NSOpenPanel()
            panel.canChooseFiles = false
            panel.canChooseDirectories = true
            panel.allowsMultipleSelection = false
            panel.canCreateDirectories = true
            panel.message = "Выберите папку для загрузок K LOAD"
            panel.prompt = "Выбрать"
            panel.begin { response in
                if response == .OK, let url = panel.urls.first {
                    result(url.path)
                } else {
                    result(nil) // отменено
                }
            }
        case "setDragZones":
            // Зоны приходят в дизайн-координатах (канвас 560×670, оси сверху-
            // слева); переводим в координаты contentView (снизу-слева) с учётом
            // cover-масштаба, которым Flutter укладывает телевизор в окно.
            if let zones = call.arguments as? [[String: Any]] {
                let cw = contentView.bounds.width
                let ch = contentView.bounds.height
                let scale = max(cw / 560.0, ch / 670.0)
                let ox = (cw - 560.0 * scale) / 2
                let oy = (ch - 670.0 * scale) / 2
                dragHelper.zones = zones.compactMap { zone in
                    guard let path = zone["path"] as? String,
                          let x = zone["x"] as? Double,
                          let y = zone["y"] as? Double,
                          let w = zone["w"] as? Double,
                          let h = zone["h"] as? Double else { return nil }
                    let nx = ox + x * scale
                    let ny = ch - (oy + (y + h) * scale)
                    return (path, CGRect(x: nx, y: ny,
                                         width: w * scale, height: h * scale))
                }
            }
            result(nil)
        default:
            result(FlutterMethodNotImplemented)
        }
    }

    super.awakeFromNib()
  }
}


// Перетаскивание скачанных файлов: Flutter присылает зоны готовых файлов
// (прямоугольники в координатах contentView), локальный монитор события
// mouseDown начинает системную drag-сессию с реальным файлом.
final class DragSourceView: NSView, NSDraggingSource {
    func draggingSession(_ session: NSDraggingSession,
                         sourceOperationMaskFor draggingContext: NSDraggingContext) -> NSDragOperation {
        return .copy
    }

    func draggingSession(_ session: NSDraggingSession, endedAt screenPoint: NSPoint,
                         operation: NSDragOperation) {
        removeFromSuperview()
    }
}

final class DragOutHelper: NSObject {
    private weak var contentView: NSView?
    private var zones: [(path: String, rect: CGRect)] = []
    private var monitor: Any?

    init(contentView: NSView) {
        self.contentView = contentView
        super.init()
        monitor = NSEvent.addLocalMonitorForEvents(matching: .leftMouseDown) {
            [weak self] event in
            return self?.handle(event: event) ?? event
        }
    }

    deinit {
        if let monitor { NSEvent.removeMonitor(monitor) }
    }

    private func handle(event: NSEvent) -> NSEvent? {
        guard let contentView, !zones.isEmpty,
              event.window === contentView.window else { return event }
        let loc = event.locationInWindow // снизу-слева окна
        for zone in zones where zone.rect.contains(loc) {
            guard let url = NSURL(fileURLWithPath: zone.path) as URL? else { continue }
            let source = DragSourceView(frame: CGRect(x: loc.x - 2, y: loc.y - 2,
                                                      width: 4, height: 4))
            contentView.addSubview(source, positioned: .above, relativeTo: nil)
            let item = NSDraggingItem(pasteboardWriter: url)
            item.setDraggingFrame(CGRect(x: 0, y: 0, width: 64, height: 64),
                                  contents: nil)
            _ = source.beginDraggingSession(with: [item], event: event,
                                            source: source)
            return nil // нажатие поглощено — Flutter не получает клик
        }
        return event
    }
}
