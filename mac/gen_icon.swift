// Генератор иконки K LOAD: тёмный пластик корпуса, впалый экран с янтарным
// свечением, «K» шрифтом Handjet (из ассетов макета) и LED-полоса загрузки.
//
// Запуск: swift scripts/gen_icon.swift
// Пишет все размеры в Assets.xcassets/AppIcon.appiconset (app_icon_N.png).

import AppKit
import CoreText
import ImageIO
import UniformTypeIdentifiers

// Скрипт запускается из app/ (см. шапку): cwd = app.
let cwd = FileManager.default.currentDirectoryPath
let fontURL = URL(fileURLWithPath: cwd + "/../design/assets/fonts/Handjet[ELGR,ELSH,wght].ttf")
    .standardizedFileURL
let iconset = URL(fileURLWithPath: cwd + "/macos/Runner/Assets.xcassets/AppIcon.appiconset")

func rgb(_ hex: UInt32, _ alpha: CGFloat = 1) -> CGColor {
    CGColor(srgbRed: CGFloat((hex >> 16) & 0xFF) / 255,
            green: CGFloat((hex >> 8) & 0xFF) / 255,
            blue: CGFloat(hex & 0xFF) / 255, alpha: alpha)
}

// Handjet — вариативный шрифт: вытягиваем экземпляр wght=800.
guard let descriptors = CTFontManagerCreateFontDescriptorsFromURL(fontURL as CFURL) as? [CTFontDescriptor],
      let base = descriptors.first else { fatalError("Handjet не прочитался: \(fontURL.path)") }
var attributes = (CTFontDescriptorCopyAttributes(base) as? [CFString: Any]) ?? [:]
attributes[kCTFontVariationAttribute] = [0x77676874 /* wght */: 800.0]
let heavyDescriptor = CTFontDescriptorCreateWithAttributes(attributes as CFDictionary)

func handjet(_ size: CGFloat) -> CTFont {
    CTFontCreateWithFontDescriptor(heavyDescriptor, size, nil)
}

let canvas = 1024
let cs = CGColorSpace(name: CGColorSpace.sRGB)!
let ctx = CGContext(data: nil, width: canvas, height: canvas, bitsPerComponent: 8,
                    bytesPerRow: 0, space: cs,
                    bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
ctx.setShouldAntialias(true)
ctx.setAllowsAntialiasing(true)
ctx.interpolationQuality = .high

// Ось Y у CGContext снизу вверх.
let body = CGRect(x: 99, y: 99, width: 826, height: 826)
let bodyRadius: CGFloat = 186

// ---- корпус: пластик с диагональным светом ----
let bodyPath = CGPath(roundedRect: body, cornerWidth: bodyRadius, cornerHeight: bodyRadius, transform: nil)
ctx.saveGState()
ctx.setShadow(offset: CGSize(width: 0, height: -26), blur: 52, color: rgb(0x000000, 0.45))
ctx.addPath(bodyPath)
ctx.setFillColor(rgb(0x262321))
ctx.fillPath()
ctx.restoreGState()

ctx.saveGState()
ctx.addPath(bodyPath)
ctx.clip()
let plastic = CGGradient(colorsSpace: cs, colors: [
    rgb(0x3B3632), rgb(0x2A2623), rgb(0x1C1A18), rgb(0x141211)
] as CFArray, locations: [0, 0.38, 0.75, 1])!
ctx.drawLinearGradient(plastic, start: CGPoint(x: 140, y: 900), end: CGPoint(x: 900, y: 130), options: [])
// верхняя кромка ловит свет
ctx.setBlendMode(.plusLighter)
let rim = CGGradient(colorsSpace: cs, colors: [rgb(0xFFF4DC, 0.16), rgb(0xFFF4DC, 0)] as CFArray, locations: [0, 1])!
ctx.drawLinearGradient(rim, start: CGPoint(x: 512, y: 925), end: CGPoint(x: 512, y: 780), options: [])
ctx.setBlendMode(.normal)
ctx.restoreGState()

// ---- экран: впалый, почти чёрный, с янтарным свечением ----
let screen = body.insetBy(dx: 76, dy: 76)
let screenRadius: CGFloat = 132
let screenPath = CGPath(roundedRect: screen, cornerWidth: screenRadius, cornerHeight: screenRadius, transform: nil)
ctx.saveGState()
ctx.addPath(screenPath)
ctx.clip()
ctx.setFillColor(rgb(0x060302))
ctx.fill(screen)
let glowCenter = CGPoint(x: 512, y: 512 + 40)
ctx.drawRadialGradient(
    CGGradient(colorsSpace: cs, colors: [rgb(0x1A1004), rgb(0x0A0502), rgb(0x030201)] as CFArray,
               locations: [0, 0.55, 1])!,
    startCenter: glowCenter, startRadius: 0,
    endCenter: glowCenter, endRadius: 470, options: [])
// сканлайны кинескопа
ctx.setFillColor(rgb(0x000000, 0.22))
var y = screen.minY
while y < screen.maxY {
    ctx.fill(CGRect(x: screen.minX, y: y, width: screen.width, height: 1.5))
    y += 5
}
// янтарное пятно под знаком
ctx.setBlendMode(.plusLighter)
ctx.drawRadialGradient(
    CGGradient(colorsSpace: cs, colors: [rgb(0xFFB000, 0.20), rgb(0xFFB000, 0)] as CFArray, locations: [0, 1])!,
    startCenter: glowCenter, startRadius: 0,
    endCenter: glowCenter, endRadius: 330, options: [])
ctx.setBlendMode(.normal)
ctx.restoreGState()

// внутренняя тень экрана: тёмная кайма с размытием внутрь
ctx.saveGState()
ctx.addPath(screenPath)
ctx.clip()
ctx.setShadow(offset: .zero, blur: 26, color: rgb(0x000000, 0.85))
ctx.setStrokeColor(rgb(0x000000, 0.85))
ctx.setLineWidth(10)
ctx.addPath(screenPath)
ctx.strokePath()
ctx.restoreGState()

// ---- «K» ----
func drawAmber(_ text: String, font: CTFont, center: CGPoint, glow: CGFloat, fill: CGColor) {
    let line = CTLineCreateWithAttributedString(NSAttributedString(string: text, attributes: [
        .font: font, .foregroundColor: NSColor(cgColor: fill)!,
    ]))
    let width = CGFloat(CTLineGetTypographicBounds(line, nil, nil, nil))
    let ascent = CTFontGetAscent(font), descent = CTFontGetDescent(font)
    let origin = CGPoint(x: center.x - width / 2, y: center.y - (ascent - descent) / 2)
    ctx.saveGState()
    ctx.textMatrix = CGAffineTransform(scaleX: 1, y: 1)
    ctx.setShadow(offset: .zero, blur: glow, color: rgb(0xFFB000, 0.55))
    ctx.textPosition = origin
    CTLineDraw(line, ctx)
    // второй проход — плотнее ядро свечения
    ctx.setShadow(offset: .zero, blur: glow / 3, color: rgb(0xFFB000, 0.45))
    ctx.textPosition = origin
    CTLineDraw(line, ctx)
    ctx.restoreGState()
}

drawAmber("K", font: handjet(520), center: CGPoint(x: 512, y: 556), glow: 34, fill: rgb(0xFFB000))

// ---- LED-полоса: 3 из 5 горят — «загрузка идёт» ----
let cell = CGSize(width: 74, height: 30), gap: CGFloat = 18
let barWidth = 5 * cell.width + 4 * gap
let barY = screen.minY + 118
for i in 0 ..< 5 {
    let rect = CGRect(x: 512 - barWidth / 2 + CGFloat(i) * (cell.width + gap), y: barY,
                      width: cell.width, height: cell.height)
    let path = CGPath(roundedRect: rect, cornerWidth: 5, cornerHeight: 5, transform: nil)
    ctx.saveGState()
    if i < 3 {
        ctx.setShadow(offset: .zero, blur: 14, color: rgb(0xFFB000, 0.6))
        ctx.setFillColor(rgb(0xFFB000))
    } else {
        ctx.setFillColor(rgb(0x241A08))
    }
    ctx.addPath(path)
    ctx.fillPath()
    ctx.restoreGState()
}

// ---- запись PNG всех размеров ----
let image = ctx.makeImage()!
let dest = CGImageDestinationCreateWithURL(iconset.appendingPathComponent("app_icon_1024.png") as CFURL,
                                           UTType.png.identifier as CFString, 1, nil)!
CGImageDestinationAddImage(dest, image, nil)
CGImageDestinationFinalize(dest)

for side in [512, 256, 128, 64, 32, 16] {
    guard let resized = image.resized(to: CGSize(width: side, height: side)) else { fatalError("resize \(side)") }
    let out = CGImageDestinationCreateWithURL(iconset.appendingPathComponent("app_icon_\(side).png") as CFURL,
                                              UTType.png.identifier as CFString, 1, nil)!
    CGImageDestinationAddImage(out, resized, nil)
    CGImageDestinationFinalize(out)
}
print("иконка записана: \(iconset.path)")

extension CGImage {
    func resized(to size: CGSize) -> CGImage? {
        let context = CGContext(data: nil, width: Int(size.width), height: Int(size.height),
                                bitsPerComponent: 8, bytesPerRow: 0,
                                space: colorSpace ?? CGColorSpace(name: CGColorSpace.sRGB)!,
                                bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
        context.interpolationQuality = .high
        context.draw(self, in: CGRect(origin: .zero, size: size))
        return context.makeImage()
    }
}
