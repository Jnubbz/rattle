// Draws the app icon: white radio waves on a red rounded square. Usage: swift icon.swift out.png
import AppKit
let size = 1024.0
let img = NSImage(size: NSSize(width: size, height: size))
img.lockFocus()
let bg = NSBezierPath(roundedRect: NSRect(x: 80, y: 80, width: 864, height: 864), xRadius: 190, yRadius: 190)
NSColor(calibratedRed: 0.82, green: 0.16, blue: 0.16, alpha: 1).setFill()
bg.fill()
let cfg = NSImage.SymbolConfiguration(pointSize: 520, weight: .semibold)
	.applying(.init(paletteColors: [.white]))
if let sym = NSImage(systemSymbolName: "antenna.radiowaves.left.and.right", accessibilityDescription: nil)?
	.withSymbolConfiguration(cfg) {
	let s = sym.size
	sym.draw(in: NSRect(x: (size - s.width) / 2, y: (size - s.height) / 2, width: s.width, height: s.height))
}
img.unlockFocus()
let rep = NSBitmapImageRep(data: img.tiffRepresentation!)!
try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: CommandLine.arguments[1]))
