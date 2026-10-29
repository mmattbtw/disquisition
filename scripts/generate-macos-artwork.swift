#!/usr/bin/env swift
// Export the Icon Composer source and installer artwork on macOS with Xcode 26+.
// Generated files are checked in, so normal builds need no graphics tools.
import AppKit

let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
let assets = root.appendingPathComponent("resources/macos")
let background = NSColor(srgbRed: 40 / 255, green: 44 / 255, blue: 52 / 255, alpha: 1)
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

func centered(_ string: String, y: CGFloat, font: NSFont, color: NSColor) {
    let attributes: [NSAttributedString.Key: Any] = [.font: font, .foregroundColor: color]
    let size = string.size(withAttributes: attributes)
    string.draw(at: NSPoint(x: (580 - size.width) / 2, y: y), withAttributes: attributes)
}

let icons = root.appendingPathComponent("resources/icons")
let windows = root.appendingPathComponent("resources/windows")
let web = root.appendingPathComponent("src/web/public")
try FileManager.default.createDirectory(at: windows, withIntermediateDirectories: true)
let temporary = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
try FileManager.default.createDirectory(at: temporary, withIntermediateDirectories: true)
defer { try? FileManager.default.removeItem(at: temporary) }

func run(_ executable: String, _ arguments: [String]) throws {
    let process = Process()
    process.executableURL = URL(fileURLWithPath: executable)
    process.arguments = arguments
    try process.run()
    process.waitUntilExit()
    guard process.terminationStatus == 0 else { fatalError("\(executable) failed") }
}

let developerPipe = Pipe()
let developer = Process()
developer.executableURL = URL(fileURLWithPath: "/usr/bin/xcode-select")
developer.arguments = ["-p"]
developer.standardOutput = developerPipe
try developer.run()
let developerPath = String(data: developerPipe.fileHandleForReading.readDataToEndOfFile(),
                           encoding: .utf8)!.trimmingCharacters(in: .whitespacesAndNewlines)
developer.waitUntilExit()
guard developer.terminationStatus == 0 else { fatalError("xcode-select failed") }
let ictool = URL(fileURLWithPath: developerPath).deletingLastPathComponent()
    .appendingPathComponent("Applications/Icon Composer.app/Contents/Executables/ictool")
let source = icons.appendingPathComponent("disquisition.icon")
try run(ictool.path, [source.path, "--export-image", "--output-file",
    icons.appendingPathComponent("disquisition.png").path, "--platform", "macOS",
    "--rendition", "Default", "--width", "1024", "--height", "1024", "--scale", "1",
    "--design-generation", "26"])
try run("/usr/bin/xcrun", ["actool", source.path, "--compile", temporary.path,
    "--platform", "macosx", "--target-device", "mac", "--minimum-deployment-target", "11.0",
    "--app-icon", "disquisition", "--output-partial-info-plist",
    temporary.appendingPathComponent("partial.plist").path])
for name in ["Assets.car", "disquisition.icns"] {
    try Data(contentsOf: temporary.appendingPathComponent(name)).write(to: assets.appendingPathComponent(name))
}

try run(ictool.path, [source.path, "--export-image", "--output-file",
    icons.appendingPathComponent("disquisition-dark.png").path, "--platform", "macOS",
    "--rendition", "Dark", "--width", "1024", "--height", "1024", "--scale", "1",
    "--design-generation", "26"])
let image = NSImage(contentsOf: icons.appendingPathComponent("disquisition.png"))!
let darkImage = NSImage(contentsOf: icons.appendingPathComponent("disquisition-dark.png"))!
func resizedPNG(_ size: Int, dark: Bool = true) -> Data {
    bitmap(width: size, height: size) {
        NSGraphicsContext.current?.imageInterpolation = .high
        (dark ? darkImage : image).draw(in: NSRect(x: 0, y: 0, width: size, height: size),
                   from: .zero, operation: .copy, fraction: 1)
    }.representation(using: .png, properties: [:])!
}

// ICO supports PNG frames, including alpha, on every supported Windows version.
func ico(_ sizes: [Int], allDark: Bool = false) -> Data {
    let frames = sizes.map { resizedPNG($0, dark: allDark || $0 <= 64) }
    var data = Data()
    func word(_ value: UInt16) {
        var value = value.littleEndian
        withUnsafeBytes(of: &value) { data.append(contentsOf: $0) }
    }
    func dword(_ value: UInt32) {
        var value = value.littleEndian
        withUnsafeBytes(of: &value) { data.append(contentsOf: $0) }
    }
    word(0); word(1); word(UInt16(sizes.count))
    var offset = 6 + 16 * sizes.count
    for (size, frame) in zip(sizes, frames) {
        data.append(contentsOf: [UInt8(size % 256), UInt8(size % 256), 0, 0])
        word(1); word(32)
        dword(UInt32(frame.count)); dword(UInt32(offset))
        offset += frame.count
    }
    frames.forEach { data.append($0) }
    return data
}
try ico([16, 20, 24, 32, 40, 48, 64, 96, 128, 256])
    .write(to: windows.appendingPathComponent("disquisition.ico"))
try ico([16, 32, 48], allDark: true).write(to: web.appendingPathComponent("favicon.ico"))
try resizedPNG(64).write(to: web.appendingPathComponent("favicon.png"))
try resizedPNG(180).write(to: web.appendingPathComponent("apple-touch-icon.png"))

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
print("Generated desktop and web icons, and macOS DMG background")
