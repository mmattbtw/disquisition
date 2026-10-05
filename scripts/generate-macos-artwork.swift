#!/usr/bin/env swift
// Regenerate the checked-in artwork on macOS. No build-time graphics tools are needed.
import AppKit

let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
let assets = root.appendingPathComponent("resources/macos")
let background = NSColor(srgbRed: 40 / 255, green: 44 / 255, blue: 52 / 255, alpha: 1)
let mint = NSColor(srgbRed: 157 / 255, green: 204 / 255, blue: 202 / 255, alpha: 1)
try FileManager.default.createDirectory(at: assets, withIntermediateDirectories: true)

func bitmap(width: Int, height: Int, draw: () -> Void) -> NSBitmapImageRep {
    let bitmap = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: width, pixelsHigh: height,
        bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
        colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: bitmap)
    draw()
    NSGraphicsContext.restoreGraphicsState()
    return bitmap
}

func png(_ url: URL, width: Int, height: Int, draw: () -> Void) throws {
    try bitmap(width: width, height: height, draw: draw)
        .representation(using: .png, properties: [:])!.write(to: url)
}

func icon(size: Int, url: URL) throws {
    try png(url, width: size, height: size) {
        let scale = CGFloat(size) / 1024
        let transform = NSAffineTransform()
        transform.scale(by: scale)
        transform.concat()
        background.setFill()
        NSBezierPath(roundedRect: NSRect(x: 72, y: 72, width: 880, height: 880),
                     xRadius: 196, yRadius: 196).fill()
        mint.setStroke()
        let prompt = NSBezierPath()
        prompt.lineWidth = 66
        prompt.lineCapStyle = .square
        prompt.lineJoinStyle = .miter
        prompt.move(to: NSPoint(x: 300, y: 663))
        prompt.line(to: NSPoint(x: 462, y: 512))
        prompt.line(to: NSPoint(x: 300, y: 361))
        prompt.stroke()
        let cursor = NSBezierPath()
        cursor.lineWidth = 66
        cursor.move(to: NSPoint(x: 555, y: 349))
        cursor.line(to: NSPoint(x: 751, y: 349))
        cursor.stroke()
    }
}

func centered(_ string: String, y: CGFloat, font: NSFont, color: NSColor) {
    let attributes: [NSAttributedString.Key: Any] = [.font: font, .foregroundColor: color]
    let size = string.size(withAttributes: attributes)
    string.draw(at: NSPoint(x: (580 - size.width) / 2, y: y), withAttributes: attributes)
}

try icon(size: 1024, url: assets.appendingPathComponent("disquisition.png"))
let temporary = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
let iconset = temporary.appendingPathComponent("disquisition.iconset")
try FileManager.default.createDirectory(at: iconset, withIntermediateDirectories: true)
defer { try? FileManager.default.removeItem(at: temporary) }
for size in [16, 32, 128, 256, 512] {
    try icon(size: size, url: iconset.appendingPathComponent("icon_\(size)x\(size).png"))
    try icon(size: size * 2, url: iconset.appendingPathComponent("icon_\(size)x\(size)@2x.png"))
}
let process = Process()
process.executableURL = URL(fileURLWithPath: "/usr/bin/iconutil")
process.arguments = ["-c", "icns", iconset.path, "-o", assets.appendingPathComponent("disquisition.icns").path]
try process.run()
process.waitUntilExit()
guard process.terminationStatus == 0 else { fatalError("iconutil failed") }

func installerBackground() {
    NSColor(srgbRed: 239 / 255, green: 241 / 255, blue: 240 / 255, alpha: 1).setFill()
    NSRect(x: 0, y: 0, width: 580, height: 380).fill()
    centered("disquisition", y: 294, font: .monospacedSystemFont(ofSize: 30, weight: .bold), color: background)
    centered("Drag disquisition to Applications to install.", y: 252,
             font: .systemFont(ofSize: 17), color: background)
    NSColor(srgbRed: 70 / 255, green: 130 / 255, blue: 128 / 255, alpha: 1).setStroke()
    let arrow = NSBezierPath()
    arrow.lineWidth = 3
    arrow.lineCapStyle = .round
    arrow.lineJoinStyle = .round
    arrow.move(to: NSPoint(x: 264, y: 155))
    arrow.line(to: NSPoint(x: 316, y: 155))
    arrow.move(to: NSPoint(x: 306, y: 165))
    arrow.line(to: NSPoint(x: 316, y: 155))
    arrow.line(to: NSPoint(x: 306, y: 145))
    arrow.stroke()
    centered("Then open disquisition from Applications.", y: 38,
             font: .systemFont(ofSize: 14), color: background)
}
let installerReps = [1, 2].map { scale in
    let rep = bitmap(width: 580 * scale, height: 380 * scale) {
        let transform = NSAffineTransform()
        transform.scale(by: CGFloat(scale))
        transform.concat()
        installerBackground()
    }
    rep.size = NSSize(width: 580, height: 380)
    return rep
}
let installerImage = NSImage(size: NSSize(width: 580, height: 380))
installerReps.forEach { installerImage.addRepresentation($0) }
try installerImage.tiffRepresentation(using: .lzw, factor: 1)!
    .write(to: assets.appendingPathComponent("dmg-background.tiff"))
print("Generated macOS icon and DMG background in \(assets.path)")
