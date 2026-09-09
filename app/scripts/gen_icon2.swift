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
let maskSide = 512
let grayCtx = CGContext(data: nil, width: maskSide, height: maskSide,
                        bitsPerComponent: 8, bytesPerRow: maskSide,
                        space: CGColorSpaceCreateDeviceGray(),
                        bitmapInfo: 0)!
grayCtx.setFillColor(gray: 0, alpha: 1)
grayCtx.fill(CGRect(x: 0, y: 0, width: maskSide, height: maskSide))
grayCtx.interpolationQuality = .high
grayCtx.draw(srcImage, in: CGRect(x: 0, y: 0, width: maskSide, height: maskSide))
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

// мягкое янтарное свечение под буквой
ctx.setBlendMode(.plusLighter)
ctx.drawRadialGradient(
    CGGradient(colorsSpace: cs,
               colors: [rgb(0xFFB000, 0.20), rgb(0xFFB000, 0)] as CFArray,
               locations: [0, 1])!,
    startCenter: CGPoint(x: 430, y: 580), startRadius: 0,
    endCenter: CGPoint(x: 430, y: 580), endRadius: 420, options: [])
ctx.setBlendMode(.normal)

// буква K из фавикона: клип по маске + тёплый вертикальный градиент
let kRect = CGRect(x: 96, y: 132, width: 780, height: 780)
ctx.clip(to: kRect, mask: kMask)
let kGrad = CGGradient(colorsSpace: cs, colors: [
    rgb(0xFFD98C), rgb(0xFFB000), rgb(0xE88F06),
] as CFArray, locations: [0, 0.55, 1])!
ctx.drawLinearGradient(kGrad, start: CGPoint(x: 420, y: 912),
                       end: CGPoint(x: 460, y: 132), options: [])
ctx.restoreGState()

// бейдж загрузки: круг с золотым градиентом и тёмной стрелкой вниз
let badgeCenter = CGPoint(x: 764, y: 240)
let badgeR: CGFloat = 128
ctx.saveGState()
ctx.setShadow(offset: CGSize(width: 0, height: -10), blur: 30,
              color: rgb(0x000000, 0.55))
ctx.setFillColor(rgb(0x14100C))
ctx.fillEllipse(in: CGRect(x: badgeCenter.x - badgeR - 10,
                           y: badgeCenter.y - badgeR - 10,
                           width: (badgeR + 10) * 2, height: (badgeR + 10) * 2))
ctx.restoreGState()

ctx.saveGState()
ctx.setShadow(offset: .zero, blur: 24, color: rgb(0xFFB000, 0.45))
let badgeGrad = CGGradient(colorsSpace: cs, colors: [
    rgb(0xFFDD9A), rgb(0xFFB000), rgb(0xF09000),
] as CFArray, locations: [0, 0.55, 1])!
ctx.drawRadialGradient(badgeGrad,
                       startCenter: CGPoint(x: badgeCenter.x - 34, y: badgeCenter.y + 44),
                       startRadius: 0,
                       endCenter: badgeCenter, endRadius: badgeR, options: [])
ctx.restoreGState()

// стрелка вниз: шток + шеврон, тёмная, крупная
ctx.saveGState()
ctx.translateBy(x: badgeCenter.x, y: badgeCenter.y)
ctx.scaleBy(x: badgeR / 128, y: badgeR / 128)
let arrow = CGMutablePath()
arrow.move(to: CGPoint(x: -26, y: 66))
arrow.addLine(to: CGPoint(x: 26, y: 66))
arrow.addLine(to: CGPoint(x: 26, y: -6))
arrow.addLine(to: CGPoint(x: 64, y: -6))
arrow.addLine(to: CGPoint(x: 0, y: -78))
arrow.addLine(to: CGPoint(x: -64, y: -6))
arrow.addLine(to: CGPoint(x: -26, y: -6))
arrow.closeSubpath()
ctx.setFillColor(rgb(0x1A1206))
ctx.addPath(arrow)
ctx.fillPath()
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
