// Генератор иконки K LOAD из Фавикон.png: гранжевая буква K занимает почти
// всю площадь, в правом нижнем углу — круглая бейдж-иконка загрузки с
// тёплым золотым градиентом. Запуск из app/: swift scripts/gen_icon2.swift

import AppKit
import CoreGraphics
import ImageIO
import UniformTypeIdentifiers

let cwd = FileManager.default.currentDirectoryPath
let faviconURL = URL(fileURLWithPath: cwd + "/../Фавикон.png").standardizedFileURL
let iconset = URL(fileURLWithPath: cwd + "/macos/Runner/Assets.xcassets/AppIcon.appiconset")

func rgb(_ hex: UInt32, _ alpha: CGFloat = 1) -> CGColor {
    CGColor(srgbRed: CGFloat((hex >> 16) & 0xFF) / 255,
            green: CGFloat((hex >> 8) & 0xFF) / 255,
            blue: CGFloat(hex & 0xFF) / 255, alpha: alpha)
}

// ---- маска буквы: фавикон -> DeviceGray без альфы (белая K = непрозрачно) ----
guard let srcImage = NSImage(contentsOf: faviconURL)?.cgImage(
    forProposedRect: nil, context: nil, hints: nil)
else { fatalError("Фавикон.png не прочитался") }
// Пиксельная K: ужимаем фавикон до грубой сетки и растягиваем ступеньками.
let tinySide = 40
let tinyCtx = CGContext(data: nil, width: tinySide, height: tinySide,
                        bitsPerComponent: 8, bytesPerRow: tinySide,
                        space: CGColorSpaceCreateDeviceGray(),
                        bitmapInfo: 0)!
tinyCtx.setFillColor(gray: 0, alpha: 1)
tinyCtx.fill(CGRect(x: 0, y: 0, width: tinySide, height: tinySide))
tinyCtx.interpolationQuality = .medium
tinyCtx.draw(srcImage, in: CGRect(x: 0, y: 0, width: tinySide, height: tinySide))
guard let tinyMask = tinyCtx.makeImage() else { fatalError("tiny mask") }

let maskSide = 512
let grayCtx = CGContext(data: nil, width: maskSide, height: maskSide,
                        bitsPerComponent: 8, bytesPerRow: maskSide,
                        space: CGColorSpaceCreateDeviceGray(),
                        bitmapInfo: 0)!
grayCtx.setFillColor(gray: 0, alpha: 1)
grayCtx.fill(CGRect(x: 0, y: 0, width: maskSide, height: maskSide))
grayCtx.interpolationQuality = .none // ступеньки без сглаживания
grayCtx.draw(tinyMask, in: CGRect(x: 0, y: 0, width: maskSide, height: maskSide))
let kMask = grayCtx.makeImage()!

// ---- полотно ----
let canvas = 1024
let cs = CGColorSpace(name: CGColorSpace.sRGB)!
let ctx = CGContext(data: nil, width: canvas, height: canvas,
                    bitsPerComponent: 8, bytesPerRow: 0, space: cs,
                    bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
ctx.setShouldAntialias(true)
ctx.setAllowsAntialiasing(true)
ctx.interpolationQuality = .high

// корпус: тёмный пластик на всю площадь, радиус под macOS
let body = CGRect(x: 0, y: 0, width: 1024, height: 1024)
let bodyPath = CGPath(roundedRect: body, cornerWidth: 186, cornerHeight: 186, transform: nil)
ctx.saveGState()
ctx.addPath(bodyPath)
ctx.clip()
let plastic = CGGradient(colorsSpace: cs, colors: [
    rgb(0x35302C), rgb(0x262321), rgb(0x191715), rgb(0x120F0E),
] as CFArray, locations: [0, 0.38, 0.75, 1])!
ctx.drawLinearGradient(plastic, start: CGPoint(x: 120, y: 940),
                       end: CGPoint(x: 920, y: 90), options: [])

// мягкое тёплое свечение за белой буквой (не портит читаемость)
ctx.setBlendMode(.plusLighter)
ctx.drawRadialGradient(
    CGGradient(colorsSpace: cs,
               colors: [rgb(0xFFB000, 0.14), rgb(0xFFB000, 0)] as CFArray,
               locations: [0, 1])!,
    startCenter: CGPoint(x: 430, y: 580), startRadius: 0,
    endCenter: CGPoint(x: 430, y: 580), endRadius: 420, options: [])
ctx.setBlendMode(.normal)

// буква K из фавикона: клип по маске, белая — почти весь тайтл
let kRect = CGRect(x: 32, y: 32, width: 880, height: 880)
ctx.clip(to: kRect, mask: kMask)
ctx.setFillColor(rgb(0xFFFFFF))
ctx.fill(kRect)
ctx.restoreGState()

// пиксельная стрелка вниз (оранжевая, без круга) в правом нижнем углу
let cell: CGFloat = 56
let pixelGrid: [[Int]] = [
    [0, 0, 1, 0, 0],
    [0, 0, 1, 0, 0],
    [0, 0, 1, 0, 0],
    [1, 1, 1, 1, 1],
    [0, 1, 1, 1, 0],
    [0, 0, 1, 0, 0],
]
let gridW = CGFloat(pixelGrid[0].count) * cell
let gridH = CGFloat(pixelGrid.count) * cell
let origin = CGPoint(x: 1024 - 64 - gridW, y: 64) // CG: y вверх, низ иконки
ctx.saveGState()
ctx.setShadow(offset: .zero, blur: 18, color: rgb(0xFFB000, 0.5))
ctx.setFillColor(rgb(0xFFB000))
for (row, line) in pixelGrid.enumerated() {
    // ось Y у CGContext снизу вверх — переворачиваем строки сетки
    let flipped = pixelGrid.count - 1 - row
    for (col, v) in line.enumerated() where v == 1 {
        let r = CGRect(x: origin.x + CGFloat(col) * cell,
                       y: origin.y + CGFloat(flipped) * cell,
                       width: cell - 3, height: cell - 3)
        ctx.fill(r)
    }
}
ctx.restoreGState()

// ---- запись PNG всех размеров ----
let image = ctx.makeImage()!
func write(_ img: CGImage, _ side: Int) {
    let out = CGImageDestinationCreateWithURL(
        iconset.appendingPathComponent("app_icon_\(side).png") as CFURL,
        UTType.png.identifier as CFString, 1, nil)!
    CGImageDestinationAddImage(out, img, nil)
    CGImageDestinationFinalize(out)
}
write(image, 1024)
for side in [512, 256, 128, 64, 32, 16] {
    let c = CGContext(data: nil, width: side, height: side, bitsPerComponent: 8,
                      bytesPerRow: 0, space: cs,
                      bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    c.interpolationQuality = .high
    c.draw(image, in: CGRect(x: 0, y: 0, width: side, height: side))
    write(c.makeImage()!, side)
}
print("иконка записана: \(iconset.path)")
