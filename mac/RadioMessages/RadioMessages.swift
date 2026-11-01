// Radio Messages — a one-window Mac app that listens for Rattlegram messages
// from the radio and shows them in large type. It runs the bundled `rattle`
// receiver (receive only — it never keys the radio) and restarts it if the
// radio goes away.

import AppKit
import SwiftUI
import CoreAudio
import ServiceManagement
import UserNotifications

// MARK: - Model

struct RadioMessage: Identifiable, Codable {
	var id = UUID()
	var time: Date
	var from: String
	var name: String
	var text: String
	var kind: String   // "message", "failed", "ping"
}

@MainActor
final class Model: ObservableObject {
	enum Status: Equatable {
		case starting
		case listening
		case problem(String)
	}

	@Published var messages: [RadioMessage] = []
	@Published var status: Status = .starting
	@Published var alerting = false

	var onAlert: (() -> Void)?

	private var process: Process?
	private var buffer = Data()
	private var lastError = ""
	private var stopping = false
	private var testMode = false
	private var alarm: NSSound?
	private var activity: NSObjectProtocol?

	let supportDir: URL
	var configURL: URL { supportDir.appendingPathComponent("rattle.ini") }
	var historyURL: URL { supportDir.appendingPathComponent("messages.json") }
	var logURL: URL { supportDir.appendingPathComponent("rattle-log.txt") }

	init() {
		let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
		supportDir = base.appendingPathComponent("Radio Messages", isDirectory: true)
		try? FileManager.default.createDirectory(at: supportDir, withIntermediateDirectories: true)
		installDefaultConfig()
		loadHistory()
		// Keep the Mac from going to sleep while we listen (the screen may still sleep).
		activity = ProcessInfo.processInfo.beginActivity(
			options: [.idleSystemSleepDisabled, .suddenTerminationDisabled],
			reason: "Listening for radio messages")
	}

	// MARK: receiver process

	func start() {
		guard !stopping else { return }
		guard let exe = Bundle.main.url(forAuxiliaryExecutable: "rattle") else {
			status = .problem("The app is damaged (receiver missing). Reinstall Radio Messages.")
			return
		}
		status = .starting
		lastError = ""
		buffer.removeAll()

		let p = Process()
		p.executableURL = exe
		p.arguments = ["listen", "--config", configURL.path, "--format", "json",
		               "--no-audio-s", "15", "--log", logURL.path]
		// Test hook: decode a recording instead of the radio (used by the build's screenshot test).
		if let wav = ProcessInfo.processInfo.environment["RADIO_MESSAGES_TEST_WAV"] {
			p.arguments = ["listen", "--config", configURL.path, "--format", "json", "--wav", wav]
			testMode = true
			status = .listening
		}
		p.currentDirectoryURL = supportDir
		let pipe = Pipe()
		p.standardOutput = pipe
		p.standardError = pipe
		pipe.fileHandleForReading.readabilityHandler = { [weak self] h in
			let d = h.availableData
			if d.isEmpty { return }
			Task { @MainActor in self?.consume(d) }
		}
		p.terminationHandler = { [weak self] _ in
			Task { @MainActor in self?.receiverExited() }
		}
		do {
			try p.run()
			process = p
		} catch {
			status = .problem("Could not start the receiver: \(error.localizedDescription)")
			scheduleRestart()
		}
	}

	func stop() {
		stopping = true
		process?.terminate()
		process = nil
	}

	private func receiverExited() {
		process = nil
		if stopping || testMode { return }
		status = .problem(lastError.isEmpty ? "Lost contact with the radio." : lastError)
		scheduleRestart()
	}

	private func scheduleRestart() {
		DispatchQueue.main.asyncAfter(deadline: .now() + 10) { [weak self] in
			Task { @MainActor in self?.start() }
		}
	}

	private func consume(_ data: Data) {
		buffer.append(data)
		while let nl = buffer.firstIndex(of: 0x0A) {
			let line = buffer.subdata(in: buffer.startIndex..<nl)
			buffer.removeSubrange(buffer.startIndex...nl)
			guard let obj = try? JSONSerialization.jsonObject(with: line) as? [String: String],
			      let event = obj["event"] else { continue }
			handle(event, obj)
		}
	}

	private func handle(_ event: String, _ f: [String: String]) {
		let from = f["from"] ?? ""
		let name = (f["name"]?.isEmpty == false ? f["name"]! : from)
		switch event {
		case "listening":
			status = .listening
		case "error":
			lastError = friendly(f["message"] ?? "")
		case "message":
			add(RadioMessage(time: Date(), from: from, name: name, text: f["text"] ?? "", kind: "message"), loud: true)
		case "failed":
			add(RadioMessage(time: Date(), from: from, name: name, text: "", kind: "failed"), loud: true)
		case "ping":
			add(RadioMessage(time: Date(), from: from, name: name, text: "", kind: "ping"), loud: false)
		default:
			break
		}
	}

	private func friendly(_ raw: String) -> String {
		let r = raw.lowercased()
		if r.contains("input device") || r.contains("no audio") || r.contains("cannot open input") {
			return "Can't hear the radio. Make sure the radio is turned on and its USB cable is plugged into the Mac."
		}
		return raw
	}

	// MARK: messages and alerts

	private func add(_ m: RadioMessage, loud: Bool) {
		messages.append(m)
		if messages.count > 500 { messages.removeFirst(messages.count - 500) }
		saveHistory()
		notify(m)
		onAlert?()
		if loud {
			alerting = true
			playAlarm(loop: true)
		} else {
			playAlarm(loop: false)
		}
	}

	func acknowledge() {
		alerting = false
		alarm?.stop()
	}

	private func playAlarm(loop: Bool) {
		alarm?.stop()
		guard let s = NSSound(named: NSSound.Name("Sosumi"))?.copy() as? NSSound else { return }
		// The Mac's default output may be the radio's sound card; play on the Mac's own speakers.
		if let uid = AudioDevices.speakerUID() { s.playbackDeviceIdentifier = uid }
		s.volume = 1.0
		s.loops = loop
		s.play()
		alarm = s
	}

	private func notify(_ m: RadioMessage) {
		let c = UNMutableNotificationContent()
		switch m.kind {
		case "message":
			c.title = "Radio message from \(m.name)"
			c.body = m.text
		case "failed":
			c.title = "A radio message from \(m.name) came in"
			c.body = "It was too weak to read. \(m.name) may try again."
		default:
			c.title = "\(m.name) sent a test signal"
			c.body = "The radio link is working."
		}
		c.sound = nil
		let req = UNNotificationRequest(identifier: m.id.uuidString, content: c, trigger: nil)
		UNUserNotificationCenter.current().add(req)
	}

	private func loadHistory() {
		guard let d = try? Data(contentsOf: historyURL) else { return }
		let dec = JSONDecoder()
		dec.dateDecodingStrategy = .iso8601
		messages = (try? dec.decode([RadioMessage].self, from: d)) ?? []
	}

	private func saveHistory() {
		let enc = JSONEncoder()
		enc.dateEncodingStrategy = .iso8601
		if let d = try? enc.encode(messages) { try? d.write(to: historyURL, options: .atomic) }
	}

	private func installDefaultConfig() {
		guard !FileManager.default.fileExists(atPath: configURL.path),
		      let src = Bundle.main.url(forResource: "rattle", withExtension: "ini") else { return }
		try? FileManager.default.copyItem(at: src, to: configURL)
	}
}

// MARK: - Audio device lookup (for the alarm)

enum AudioDevices {
	static func speakerUID() -> String? {
		var addr = AudioObjectPropertyAddress(
			mSelector: kAudioHardwarePropertyDevices,
			mScope: kAudioObjectPropertyScopeGlobal,
			mElement: kAudioObjectPropertyElementMain)
		var size: UInt32 = 0
		let sys = AudioObjectID(kAudioObjectSystemObject)
		guard AudioObjectGetPropertyDataSize(sys, &addr, 0, nil, &size) == noErr else { return nil }
		var ids = [AudioDeviceID](repeating: 0, count: Int(size) / MemoryLayout<AudioDeviceID>.size)
		guard AudioObjectGetPropertyData(sys, &addr, 0, nil, &size, &ids) == noErr else { return nil }

		var fallback: String?
		for id in ids where hasOutput(id) {
			guard let name = string(id, kAudioObjectPropertyName),
			      let uid = string(id, kAudioDevicePropertyDeviceUID) else { continue }
			let n = name.lowercased()
			if n.contains("usb audio") || n.contains("codec") || n.contains("splashtop") { continue }
			if n.contains("speaker") { return uid }
			if fallback == nil { fallback = uid }
		}
		return fallback
	}

	private static func hasOutput(_ id: AudioDeviceID) -> Bool {
		var addr = AudioObjectPropertyAddress(
			mSelector: kAudioDevicePropertyStreams,
			mScope: kAudioDevicePropertyScopeOutput,
			mElement: kAudioObjectPropertyElementMain)
		var size: UInt32 = 0
		return AudioObjectGetPropertyDataSize(id, &addr, 0, nil, &size) == noErr && size > 0
	}

	private static func string(_ id: AudioObjectID, _ selector: AudioObjectPropertySelector) -> String? {
		var addr = AudioObjectPropertyAddress(
			mSelector: selector,
			mScope: kAudioObjectPropertyScopeGlobal,
			mElement: kAudioObjectPropertyElementMain)
		var value: Unmanaged<CFString>?
		var size = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
		let status = withUnsafeMutablePointer(to: &value) {
			AudioObjectGetPropertyData(id, &addr, 0, nil, &size, $0)
		}
		guard status == noErr, let v = value else { return nil }
		return v.takeRetainedValue() as String
	}
}

// MARK: - Views

struct ContentView: View {
	@ObservedObject var model: Model

	var body: some View {
		VStack(alignment: .leading, spacing: 18) {
			StatusBar(status: model.status)

			if model.alerting {
				Button(action: model.acknowledge) {
					Text("I've read it")
						.font(.system(size: 26, weight: .bold))
						.frame(maxWidth: .infinity)
						.padding(.vertical, 10)
				}
				.buttonStyle(.borderedProminent)
				.tint(.red)
				.controlSize(.large)
			}

			if model.messages.isEmpty {
				Spacer()
				Text("No messages yet.\nLeave this window open — when a message comes in by radio it will appear here and an alarm will sound.")
					.font(.system(size: 20))
					.foregroundStyle(.secondary)
					.multilineTextAlignment(.center)
					.frame(maxWidth: .infinity)
				Spacer()
			} else {
				ScrollView {
					LazyVStack(alignment: .leading, spacing: 14) {
						ForEach(model.messages.reversed()) { m in
							MessageRow(message: m)
						}
					}
				}
			}
		}
		.padding(24)
		.frame(minWidth: 560, minHeight: 440)
	}
}

struct StatusBar: View {
	let status: Model.Status

	var body: some View {
		HStack(alignment: .top, spacing: 12) {
			Circle().fill(color).frame(width: 16, height: 16).padding(.top, 5)
			Text(text).font(.system(size: 20, weight: .semibold))
				.fixedSize(horizontal: false, vertical: true)
		}
		.padding(14)
		.frame(maxWidth: .infinity, alignment: .leading)
		.background(RoundedRectangle(cornerRadius: 12).fill(color.opacity(0.12)))
	}

	private var color: Color {
		switch status {
		case .listening: return .green
		case .starting: return .orange
		case .problem: return .red
		}
	}

	private var text: String {
		switch status {
		case .listening: return "Listening for radio messages"
		case .starting: return "Starting…"
		case .problem(let why): return why + "\nTrying again automatically."
		}
	}
}

struct MessageRow: View {
	let message: RadioMessage

	var body: some View {
		VStack(alignment: .leading, spacing: 8) {
			Text("\(message.name)  ·  \(when)")
				.font(.system(size: 16, weight: .medium))
				.foregroundStyle(.secondary)
			switch message.kind {
			case "message":
				Text(message.text)
					.font(.system(size: 30))
					.textSelection(.enabled)
					.fixedSize(horizontal: false, vertical: true)
			case "failed":
				Text("A message came in but it was too weak to read. \(message.name) may try again.")
					.font(.system(size: 22))
					.foregroundStyle(.orange)
			default:
				Text("Test signal received — the radio link is working.")
					.font(.system(size: 18))
					.foregroundStyle(.secondary)
			}
		}
		.padding(16)
		.frame(maxWidth: .infinity, alignment: .leading)
		.background(RoundedRectangle(cornerRadius: 12).fill(Color(nsColor: .controlBackgroundColor)))
	}

	private var when: String {
		let f = DateFormatter()
		f.doesRelativeDateFormatting = true
		f.dateStyle = .medium
		f.timeStyle = .short
		return f.string(from: message.time)
	}
}

// MARK: - App

@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
	let model = Model()
	var window: NSWindow!
	private var poweringOff = false

	func applicationDidFinishLaunching(_ note: Notification) {
		NSApp.applicationIconImage = NSImage(systemSymbolName: "antenna.radiowaves.left.and.right",
		                                     accessibilityDescription: nil)
		buildMenu()

		window = NSWindow(
			contentRect: NSRect(x: 0, y: 0, width: 640, height: 560),
			styleMask: [.titled, .closable, .miniaturizable, .resizable],
			backing: .buffered, defer: false)
		window.title = "Radio Messages"
		window.isReleasedWhenClosed = false
		window.contentView = NSHostingView(rootView: ContentView(model: model))
		window.center()
		window.setFrameAutosaveName("RadioMessagesWindow")
		showWindow()

		model.onAlert = { [weak self] in self?.showWindow() }
		model.start()

		// Start automatically when the Mac starts or she logs in.
		try? SMAppService.mainApp.register()
		UNUserNotificationCenter.current().requestAuthorization(options: [.alert, .sound]) { _, _ in }

		NSWorkspace.shared.notificationCenter.addObserver(
			forName: NSWorkspace.willPowerOffNotification, object: nil, queue: .main) { [weak self] _ in
			Task { @MainActor in self?.poweringOff = true }
		}
	}

	func showWindow() {
		if window.isMiniaturized { window.deminiaturize(nil) }
		window.makeKeyAndOrderFront(nil)
		NSApp.activate(ignoringOtherApps: true)
	}

	// Closing the window keeps the app listening; clicking the Dock icon brings it back.
	func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { false }

	func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
		showWindow()
		return true
	}

	func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
		if poweringOff { return .terminateNow }
		let a = NSAlert()
		a.messageText = "Quit Radio Messages?"
		a.informativeText = "While it's closed, radio messages won't be received."
		a.addButton(withTitle: "Keep Listening")
		a.addButton(withTitle: "Quit")
		return a.runModal() == .alertSecondButtonReturn ? .terminateNow : .terminateCancel
	}

	func applicationWillTerminate(_ note: Notification) {
		model.stop()
	}

	private func buildMenu() {
		let main = NSMenu()
		let appItem = NSMenuItem()
		main.addItem(appItem)
		let appMenu = NSMenu()
		appMenu.addItem(withTitle: "Show Messages", action: #selector(showFromMenu), keyEquivalent: "1")
		appMenu.addItem(withTitle: "Open Settings Folder", action: #selector(openSettings), keyEquivalent: "")
		appMenu.addItem(.separator())
		appMenu.addItem(withTitle: "Quit Radio Messages", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
		appItem.submenu = appMenu

		let editItem = NSMenuItem()
		main.addItem(editItem)
		let edit = NSMenu(title: "Edit")
		edit.addItem(withTitle: "Copy", action: #selector(NSText.copy(_:)), keyEquivalent: "c")
		edit.addItem(withTitle: "Select All", action: #selector(NSText.selectAll(_:)), keyEquivalent: "a")
		editItem.submenu = edit

		NSApp.mainMenu = main
	}

	@objc private func showFromMenu() { showWindow() }

	@objc private func openSettings() {
		NSWorkspace.shared.open(model.supportDir)
	}
}

@main
struct RadioMessagesMain {
	@MainActor static func main() {
		let app = NSApplication.shared
		let delegate = AppDelegate()
		app.delegate = delegate
		app.setActivationPolicy(.regular)
		app.run()
	}
}
