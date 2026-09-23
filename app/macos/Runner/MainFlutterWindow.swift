import Cocoa
import FlutterMacOS

class MainFlutterWindow: NSWindow {
  var dragContentView: NSView?

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

    // Мост для системных диалогов, drag-out скачанных файлов и приёма drop-ов.
    let channel = FlutterMethodChannel(
        name: "kload/native",
        binaryMessenger: flutterViewController.engine.binaryMessenger)
    let dragHelper = DragOutHelper(contentView: contentView!)
    dragContentView = contentView
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
        case "appVersion":
            // Версия из pubspec (flutter подставляет её в CFBundleShortVersionString).
            result(Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString")
                    as? String ?? "")
        case "setZones":
            // Зоны приходят в дизайн-координатах (канвас 560×670, оси сверху-
            // слева); переводим в координаты contentView (снизу-слева) с учётом
            // cover-масштаба, которым Flutter укладывает телевизор в окно.
            if let zones = call.arguments as? [[String: Any]],
               let contentView = self.dragContentView {
                let cw = contentView.bounds.width
                let ch = contentView.bounds.height
                let scale = max(cw / 560.0, ch / 670.0)
                let ox = (cw - 560.0 * scale) / 2
                let oy = (ch - 670.0 * scale) / 2
                let converted: [(path: String, rect: CGRect)] = zones.compactMap { zone in
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
                dragHelper.setZones(converted)
            }
            result(nil)
        default:
            result(FlutterMethodNotImplemented)
        }
    }

    super.awakeFromNib()
  }
}


// Перетаскивание скачанных файлов: Flutter присылает зоны готовых строк
// (прямоугольники в координатах канваса 560×670), для каждой держится
// прозрачный NSView. Зажатие начинает системную drag-сессию с реальным
// файлом. Зона ничего не рисует и не меняет курсор: никакой подсветки,
// свечения и «серых экранов», файл просто перетаскивается. Кнопки справа
// от зоны (папка/корзина) остаются кликабельными.
final class DragSourceView: NSView, NSDraggingSource {
    let filePath: String

    init(frame: NSRect, path: String) {
        self.filePath = path
        super.init(frame: frame)
    }

    required init?(coder: NSCoder) { fatalError("not used") }

    override func draw(_ dirtyRect: NSRect) {
        // Зона невидима: подсветка и курсор запрещены спецификацией.
    }

    override func mouseDown(with event: NSEvent) {
        guard FileManager.default.fileExists(atPath: filePath) else { return }
        let url = URL(fileURLWithPath: filePath)
        let item = NSDraggingItem(pasteboardWriter: url as NSURL)
        item.setDraggingFrame(CGRect(x: 0, y: 0, width: bounds.width,
                                     height: bounds.height), contents: nil)
        _ = beginDraggingSession(with: [item], event: event, source: self)
    }

    func draggingSession(_ session: NSDraggingSession,
                         sourceOperationMaskFor draggingContext: NSDraggingContext) -> NSDragOperation {
        return .copy
    }
}

final class DragOutHelper: NSObject {
    private weak var contentView: NSView?
    private var views: [DragSourceView] = []

    init(contentView: NSView) {
        self.contentView = contentView
        super.init()
    }

    func setZones(_ zones: [(path: String, rect: CGRect)]) {
        guard let contentView else { return }
        for v in views { v.removeFromSuperview() }
        views.removeAll()
        for zone in zones {
            let v = DragSourceView(frame: zone.rect, path: zone.path)
            contentView.addSubview(v, positioned: .above, relativeTo: nil)
            views.append(v)
        }
    }
}
