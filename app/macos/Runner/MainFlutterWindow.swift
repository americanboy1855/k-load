import Cocoa
import FlutterMacOS

class MainFlutterWindow: NSWindow {
  var dragContentView: NSView?
  // Канал нужен и натив→Dart: события drop (hover/payload) уходят на экран.
  var nativeChannel: FlutterMethodChannel?

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
    nativeChannel = channel
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

    // Приём перетаскивания (ссылки из браузеров/Telegram, выделенный текст,
    // ярлыки .webloc из Finder). Catcher ставится УРОВНЕМ НИЖЕ FlutterView:
    // клики всегда достаются Flutter (он выше и ловит hit-test), а drag-
    // сессия — FlutterView умолчанием NSView не принимает drag — поднимается
    // по иерархии до catcher с зарегистрированными типами.
    if let fv = contentView, let host = fv.superview {
        let catcher = DropCatcherView(frame: fv.frame)
        catcher.autoresizingMask = [.width, .height]
        catcher.onHover = { [weak self] hover in
            self?.nativeChannel?.invokeMethod("dropHover", arguments: hover)
        }
        catcher.onPayload = { [weak self] payload in
            self?.nativeChannel?.invokeMethod("dropPayload", arguments: payload)
        }
        host.addSubview(catcher, positioned: .below, relativeTo: fv)
        catcher.registerForDraggedTypes([.URL, .fileURL, .string])
    }

    super.awakeFromNib()
  }
}


// Приёмник drop-ов: невидим, лежит под FlutterView, поэтому мышь во Flutter
// не попадает; участвует только в drag-сессиях. NSView уже NSDraggingDestination.
final class DropCatcherView: NSView {
    var onHover: ((Bool) -> Void)?
    var onPayload: ((String) -> Void)?

    override func draw(_ dirtyRect: NSRect) {} // невидим

    override func draggingEntered(_ sender: NSDraggingInfo) -> NSDragOperation {
        onHover?(true)
        return .copy
    }

    override func draggingUpdated(_ sender: NSDraggingInfo) -> NSDragOperation {
        .copy
    }

    override func draggingExited(_ sender: NSDraggingInfo?) {
        onHover?(false)
    }

    override func prepareForDragOperation(_ sender: NSDraggingInfo) -> Bool {
        return true
    }

    override func performDragOperation(_ sender: NSDraggingInfo) -> Bool {
        onHover?(false)
        if let payload = Self.extractPayload(sender.draggingPasteboard) {
            onPayload?(payload)
        }
        return true
    }

    /// Приоритет данных в пейпборде: веб-адрес (public.url — адресная строка
    /// и ссылки из браузеров/Telegram) → ярлык .webloc (Finder; URL внутри
    /// plist) → простой текст (ссылка или название трека — решит приложение).
    /// Прочие файлы (не .webloc) молча игнорируются.
    static func extractPayload(_ pb: NSPasteboard) -> String? {
        let objects = (pb.readObjects(forClasses: [NSURL.self], options: nil)
            as? [NSURL]) ?? []
        for o in objects {
            guard let scheme = o.scheme, !scheme.isEmpty,
                  scheme.lowercased() != "file",
                  let s = o.absoluteString, !s.isEmpty else { continue }
            return s // веб-адрес из браузера или Telegram
        }
        for o in objects {
            guard let scheme = o.scheme, scheme.lowercased() == "file",
                  o.pathExtension?.lowercased() == "webloc",
                  let data = try? Data(contentsOf: o as URL),
                  let plist = try? PropertyListSerialization.propertyList(
                      from: data, format: nil) as? [String: Any],
                  let s = plist["URL"] as? String, !s.isEmpty else { continue }
            return s // URL из ярлыка .webloc
        }
        if let s = pb.string(forType: .string), !s.isEmpty {
            return s // простой текст: ссылка или название трека
        }
        return nil
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
