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

    super.awakeFromNib()
  }
}
