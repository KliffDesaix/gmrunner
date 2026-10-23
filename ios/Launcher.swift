// The iOS game list (SwiftUI; Liquid Glass on iOS 26). The runner calls gmr_ios_run_launcher
// before each game: it shows the game folders in the app's Documents folder (On My iPhone >
// gmRunner in the Files app) as a grid or a list, and returns the one tapped. A long
// press renames a game, gives it an icon from Photos or picks how its in-game menu opens; these
// are kept in the game's gmrunner.ini (title=, icon=, menu_shortcut=), as the Android and Switch
// launchers keep them.
import PhotosUI
import SwiftUI
import UIKit

struct GameEntry: Identifiable, Equatable {
    let dir: URL
    var title: String
    var icon: URL?
    var shortcut = "button"  // how the in-game menu opens (menu_shortcut)
    var id: String { dir.path }
    var folderName: String { dir.lastPathComponent }
}

enum GameStore {
    static let dataFiles = ["data.win", "game.unx", "game.droid", "game.ios", "assets/game.unx", "assets/game.droid"]
    static let iconFile = "gmrunner-icon.png"

    static func scan(_ root: URL) -> [GameEntry] {
        let fm = FileManager.default
        let dirs = (try? fm.contentsOfDirectory(at: root, includingPropertiesForKeys: [.isDirectoryKey])) ?? []
        return dirs
            .filter { url in
                var isDir: ObjCBool = false
                return !url.lastPathComponent.hasPrefix(".") && fm.fileExists(atPath: url.path, isDirectory: &isDir) && isDir.boolValue &&
                    dataFiles.contains { fm.fileExists(atPath: url.appendingPathComponent($0).path) }
            }
            .map(load)
            .sorted { $0.title.localizedStandardCompare($1.title) == .orderedAscending }
    }

    static func load(_ dir: URL) -> GameEntry {
        let ini = readIni(dir)
        let title = ini["title"].flatMap { $0.isEmpty ? nil : $0 } ?? dir.lastPathComponent
        let icon = ini["icon"].map { dir.appendingPathComponent($0) }.flatMap { FileManager.default.fileExists(atPath: $0.path) ? $0 : nil }
        return GameEntry(dir: dir, title: title, icon: icon, shortcut: ini["menu_shortcut"] ?? "button")
    }

    // The ways the in-game menu can open besides Start + Select (the runner's GameMenu.cpp).
    static let shortcuts: [(value: String, label: String)] = [
        ("button", "Floating Button"), ("fingers", "Three-Finger Tap"), ("none", "Start + Select Only"),
    ]

    static func setShortcut(_ game: GameEntry, _ value: String) {
        writeIniKey(game.dir, "menu_shortcut", value == "button" ? nil : value)
    }

    static func rename(_ game: GameEntry, _ title: String) {
        let t = title.trimmingCharacters(in: .whitespaces)
        writeIniKey(game.dir, "title", t.isEmpty || t == game.folderName ? nil : t)
    }

    // The picture cropped to a square and scaled to 256 x 256, as the game's icon.
    static func setIcon(_ game: GameEntry, _ image: UIImage) {
        let side = min(image.size.width, image.size.height)
        let renderer = UIGraphicsImageRenderer(size: CGSize(width: 256, height: 256))
        let png = renderer.pngData { _ in
            let scale = 256 / side
            let w = image.size.width * scale, h = image.size.height * scale
            image.draw(in: CGRect(x: (256 - w) / 2, y: (256 - h) / 2, width: w, height: h))
        }
        try? png.write(to: game.dir.appendingPathComponent(iconFile))
        writeIniKey(game.dir, "icon", iconFile)
    }

    static func clearIcon(_ game: GameEntry) {
        try? FileManager.default.removeItem(at: game.dir.appendingPathComponent(iconFile))
        writeIniKey(game.dir, "icon", nil)
    }

    static func readIni(_ dir: URL) -> [String: String] {
        guard let text = try? String(contentsOf: dir.appendingPathComponent("gmrunner.ini"), encoding: .utf8) else { return [:] }
        var out: [String: String] = [:]
        for line in text.split(whereSeparator: \.isNewline) {
            guard let eq = line.firstIndex(of: "="), !line.hasPrefix("#"), !line.hasPrefix(";") else { continue }
            out[line[..<eq].trimmingCharacters(in: .whitespaces)] = line[line.index(after: eq)...].trimmingCharacters(in: .whitespaces)
        }
        return out
    }

    // Sets (or with nil removes) one key, keeping the file's other lines as they are.
    static func writeIniKey(_ dir: URL, _ key: String, _ value: String?) {
        let url = dir.appendingPathComponent("gmrunner.ini")
        var lines = ((try? String(contentsOf: url, encoding: .utf8)) ?? "").components(separatedBy: "\n")
        if lines.last == "" { lines.removeLast() }
        let i = lines.firstIndex { $0.split(separator: "=", maxSplits: 1).first?.trimmingCharacters(in: .whitespaces) == key }
        switch (value, i) {
        case (nil, let i?): lines.remove(at: i)
        case (nil, nil): return
        case (let v?, let i?): lines[i] = "\(key) = \(v)"
        case (let v?, nil): lines.append("\(key) = \(v)")
        }
        try? (lines.joined(separator: "\n") + "\n").write(to: url, atomically: true, encoding: .utf8)
    }
}

final class LauncherModel: ObservableObject {
    let root: URL
    @Published var games: [GameEntry] = []
    @Published var grid: Bool { didSet { UserDefaults.standard.set(grid, forKey: "grid") } }
    var onPlay: (GameEntry) -> Void = { _ in }

    init(root: URL) {
        self.root = root
        grid = UserDefaults.standard.object(forKey: "grid") as? Bool ?? true
    }

    func reload() { games = GameStore.scan(root) }
}

// Liquid Glass on iOS 26, a material before.
struct GlassTile: ViewModifier {
    let cornerRadius: CGFloat
    func body(content: Content) -> some View {
        #if compiler(>=6.2)
        if #available(iOS 26.0, *) {
            content.glassEffect(.regular.tint(.accentColor.opacity(0.22)).interactive(), in: .rect(cornerRadius: cornerRadius))
        } else {
            content.background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: cornerRadius, style: .continuous))
        }
        #else
        content.background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: cornerRadius, style: .continuous))
        #endif
    }
}

// The game's own icon, or the standard one: a game controller on a glass tile.
struct GameIconView: View {
    let game: GameEntry
    let size: CGFloat

    var body: some View {
        if let url = game.icon, let image = UIImage(contentsOfFile: url.path) {
            Image(uiImage: image)
                .resizable()
                .scaledToFill()
                .frame(width: size, height: size)
                .clipShape(RoundedRectangle(cornerRadius: size * 0.225, style: .continuous))
        } else {
            Image(systemName: "gamecontroller.fill")
                .font(.system(size: size * 0.4, weight: .semibold))
                .foregroundStyle(.tint)
                .frame(width: size, height: size)
                .modifier(GlassTile(cornerRadius: size * 0.225))
        }
    }
}

// The third-party software in the app and its licenses (bundled from the repository's licenses/).
struct LicenseComponent: Identifiable {
    let name: String
    let license: String
    let file: String
    var id: String { file }

    // License files are wrapped for 80-100 columns: join each paragraph's lines so the text wraps
    // to the screen, keeping numbered and bulleted items on lines of their own.
    var text: String {
        let base = (file as NSString).deletingPathExtension
        guard let url = Bundle.main.url(forResource: base, withExtension: "txt", subdirectory: "licenses")
            ?? Bundle.main.url(forResource: base, withExtension: "txt"),
            let raw = try? String(contentsOf: url, encoding: .utf8) else { return "" }
        let isItem = try! NSRegularExpression(pattern: "^(\\d+\\.|[-*\u{2022}]|\\([a-z0-9]+\\))\\s")
        return raw.trimmingCharacters(in: .whitespacesAndNewlines)
            .components(separatedBy: "\n\n")
            .map { para in
                var out = ""
                for raw in para.split(separator: "\n") {
                    let line = raw.trimmingCharacters(in: .whitespaces)
                    if line.isEmpty { continue }
                    if !out.isEmpty {
                        let item = isItem.firstMatch(in: line, range: NSRange(line.startIndex..., in: line)) != nil
                        out += item ? "\n" : " "
                    }
                    out += line
                }
                return out
            }
            .joined(separator: "\n\n")
    }
}

let licenseComponents = [
    LicenseComponent(name: "gmRunner", license: "GNU GPL version 3 or later", file: "GPL-3.0.txt"),
    LicenseComponent(name: "SDL2", license: "zlib License", file: "SDL2.txt"),
    LicenseComponent(name: "stb_image, stb_truetype, stb_vorbis", license: "MIT License or public domain", file: "stb.txt"),
    LicenseComponent(name: "Liberation Fonts", license: "SIL Open Font License 1.1", file: "liberation-fonts.txt"),
    LicenseComponent(name: "bzip2 (randomisation table)", license: "bzip2 License", file: "bzip2.txt"),
    LicenseComponent(name: "GameMaker HTML5 runner (YoYo Games)", license: "Apache License 2.0", file: "gamemaker-html5.txt"),
    LicenseComponent(name: "Apache License 2.0", license: "Full text", file: "Apache-2.0.txt"),
]

struct LicensesView: View {
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            List {
                Section {
                    ForEach(licenseComponents) { c in
                        NavigationLink {
                            ScrollView {
                                Text(c.text)
                                    .font(.footnote)
                                    .foregroundStyle(.secondary)
                                    .frame(maxWidth: .infinity, alignment: .leading)
                                    .padding()
                            }
                            .navigationTitle(c.name)
                            .navigationBarTitleDisplayMode(.inline)
                        } label: {
                            VStack(alignment: .leading, spacing: 2) {
                                Text(c.name)
                                Text(c.license).font(.caption).foregroundStyle(.secondary)
                            }
                        }
                    }
                } footer: {
                    Text("gmRunner is free software under the GNU General Public License, version 3 or later, and comes with NO WARRANTY. Source: github.com/KliffDesaix/gmrunner. It includes the other open-source software above.")
                }
            }
            .navigationTitle("Licenses")
            .toolbar {
                ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } }
            }
        }
    }
}

struct LauncherView: View {
    @ObservedObject var model: LauncherModel
    @State private var renaming: GameEntry?
    @State private var newTitle = ""
    @State private var iconFor: GameEntry?
    @State private var photo: PhotosPickerItem?
    @State private var showPhotos = false
    @State private var showLicenses = false

    var body: some View {
        NavigationStack {
            Group {
                if model.games.isEmpty {
                    empty
                } else if model.grid {
                    grid
                } else {
                    list
                }
            }
            .navigationTitle("gmRunner")
            .toolbar {
                ToolbarItem(placement: .topBarLeading) {
                    Button {
                        showLicenses = true
                    } label: {
                        Image(systemName: "info.circle")
                    }
                    .accessibilityLabel("Licenses")
                }
                ToolbarItem(placement: .topBarTrailing) {
                    Button {
                        withAnimation(.snappy) { model.grid.toggle() }
                    } label: {
                        Image(systemName: model.grid ? "list.bullet" : "square.grid.2x2")
                    }
                    .accessibilityLabel(model.grid ? "Show as List" : "Show as Grid")
                }
            }
        }
        .alert("Rename Game", isPresented: Binding(get: { renaming != nil }, set: { if !$0 { renaming = nil } })) {
            TextField("Title", text: $newTitle)
            Button("Save") {
                if let g = renaming { GameStore.rename(g, newTitle) }
                renaming = nil
                model.reload()
            }
            Button("Cancel", role: .cancel) { renaming = nil }
        } message: {
            Text(renaming?.folderName ?? "")
        }
        .sheet(isPresented: $showLicenses) { LicensesView() }
        .photosPicker(isPresented: $showPhotos, selection: $photo, matching: .images)
        .onChange(of: photo) { _, item in
            guard let item, let game = iconFor else { return }
            Task {
                if let data = try? await item.loadTransferable(type: Data.self), let image = UIImage(data: data) {
                    GameStore.setIcon(game, image)
                }
                await MainActor.run {
                    photo = nil
                    model.reload()
                }
            }
        }
        .onReceive(NotificationCenter.default.publisher(for: UIApplication.didBecomeActiveNotification)) { _ in model.reload() }
    }

    private var grid: some View {
        ScrollView {
            LazyVGrid(columns: [GridItem(.adaptive(minimum: 104, maximum: 150), spacing: 18)], spacing: 24) {
                ForEach(model.games) { game in
                    Button {
                        model.onPlay(game)
                    } label: {
                        VStack(spacing: 8) {
                            GameIconView(game: game, size: 96)
                            Text(game.title)
                                .font(.footnote.weight(.medium))
                                .foregroundStyle(.primary)
                                .multilineTextAlignment(.center)
                                .lineLimit(2)
                        }
                    }
                    .buttonStyle(.plain)
                    .contextMenu { menu(for: game) }
                }
            }
            .padding()
        }
        .refreshable { model.reload() }
    }

    private var list: some View {
        List(model.games) { game in
            Button {
                model.onPlay(game)
            } label: {
                HStack(spacing: 14) {
                    GameIconView(game: game, size: 48)
                    VStack(alignment: .leading, spacing: 2) {
                        Text(game.title).foregroundStyle(.primary)
                        if game.title != game.folderName {
                            Text(game.folderName).font(.caption).foregroundStyle(.secondary)
                        }
                    }
                }
            }
            .contextMenu { menu(for: game) }
        }
        .refreshable { model.reload() }
    }

    @ViewBuilder
    private func menu(for game: GameEntry) -> some View {
        Button {
            newTitle = game.title
            renaming = game
        } label: {
            Label("Rename", systemImage: "pencil")
        }
        Button {
            iconFor = game
            showPhotos = true
        } label: {
            Label("Change Icon", systemImage: "photo")
        }
        if game.icon != nil {
            Button {
                GameStore.clearIcon(game)
                model.reload()
            } label: {
                Label("Standard Icon", systemImage: "gamecontroller")
            }
        }
        Picker(selection: Binding(get: { game.shortcut }, set: { GameStore.setShortcut(game, $0); model.reload() })) {
            ForEach(GameStore.shortcuts, id: \.value) { Text($0.label).tag($0.value) }
        } label: {
            Label("In-Game Menu", systemImage: "line.3.horizontal.circle")
        }
        .pickerStyle(.menu)
    }

    private var empty: some View {
        ContentUnavailableView {
            Label("No Games", systemImage: "folder")
        } description: {
            Text("Open the Files app, go to On My iPhone › gmRunner, and copy each game's folder (the one with data.win) there.")
        } actions: {
            Button("Show in Files") {
                // shareddocuments:// opens the Files app at this app's folder.
                if let url = URL(string: "shareddocuments://" + model.root.path) { UIApplication.shared.open(url) }
            }
            Button("Look Again") { model.reload() }
        }
    }
}

// The app's one window scene (Info.plist: UIApplicationSceneManifest). SDL2 predates scenes: its
// app delegate still gets the lifecycle calls it expects, forwarded from here, and its windows are
// attached to this scene by gmr_ios_attach_window.
@objc(GMRSceneDelegate)
final class GMRSceneDelegate: UIResponder, UIWindowSceneDelegate {
    static weak var scene: UIWindowScene?

    func scene(_ scene: UIScene, willConnectTo session: UISceneSession, options connectionOptions: UIScene.ConnectionOptions) {
        GMRSceneDelegate.scene = scene as? UIWindowScene
    }

    private func forward(_ selector: Selector) {
        let app = UIApplication.shared
        if let delegate = app.delegate as? NSObject, delegate.responds(to: selector) {
            delegate.perform(selector, with: app)
        }
    }

    func sceneWillResignActive(_ scene: UIScene) { forward(#selector(UIApplicationDelegate.applicationWillResignActive(_:))) }
    func sceneDidEnterBackground(_ scene: UIScene) { forward(#selector(UIApplicationDelegate.applicationDidEnterBackground(_:))) }
    func sceneWillEnterForeground(_ scene: UIScene) { forward(#selector(UIApplicationDelegate.applicationWillEnterForeground(_:))) }
    func sceneDidBecomeActive(_ scene: UIScene) { forward(#selector(UIApplicationDelegate.applicationDidBecomeActive(_:))) }
}

// The scene, once UIKit has connected it (SDL starts the runner right after launch, possibly before).
func waitForScene() -> UIWindowScene? {
    let deadline = Date(timeIntervalSinceNow: 10)
    while Date() < deadline {
        if let s = GMRSceneDelegate.scene ?? UIApplication.shared.connectedScenes.first as? UIWindowScene { return s }
        _ = RunLoop.current.run(mode: .default, before: Date(timeIntervalSinceNow: 0.02))
    }
    return nil
}

// Puts a window SDL created into the app's scene (iOS shows no window outside one).
@_cdecl("gmr_ios_attach_window")
public func gmrIosAttachWindow(_ window: UnsafeMutableRawPointer) {
    let w = Unmanaged<UIWindow>.fromOpaque(window).takeUnretainedValue()
    if let scene = waitForScene() {
        w.windowScene = scene
        w.frame = scene.coordinateSpace.bounds
        w.makeKeyAndVisible()
        if #available(iOS 16.0, *) {
            // The game's window may want another orientation than the launcher had.
            w.rootViewController?.setNeedsUpdateOfSupportedInterfaceOrientations()
        }
    }
}

// The window's safe-area insets in points (left, top, right, bottom), for the on-screen controls.
@_cdecl("gmr_ios_safe_area")
public func gmrIosSafeArea(_ window: UnsafeMutableRawPointer, _ insets: UnsafeMutablePointer<Double>) {
    let i = Unmanaged<UIWindow>.fromOpaque(window).takeUnretainedValue().safeAreaInsets
    insets[0] = i.left
    insets[1] = i.top
    insets[2] = i.right
    insets[3] = i.bottom
}

// Called by the runner (TargetMobile.cpp) on the main thread before each game: shows the launcher
// in a window of its own and runs the main run loop until a game is chosen, then hides it again
// for SDL's window. Writes the game folder into `out`; returns 1.
@_cdecl("gmr_ios_run_launcher")
public func gmrIosRunLauncher(_ root: UnsafePointer<CChar>, _ out: UnsafeMutablePointer<CChar>, _ outSize: Int32) -> Int32 {
    let model = LauncherModel(root: URL(fileURLWithPath: String(cString: root)))
    model.reload()
    var chosen: String?
    model.onPlay = { chosen = $0.dir.path }
    let window: UIWindow
    if let scene = waitForScene() {
        window = UIWindow(windowScene: scene)
    } else {
        window = UIWindow(frame: UIScreen.main.bounds)
    }
    window.rootViewController = UIHostingController(rootView: LauncherView(model: model))
    window.makeKeyAndVisible()
    while chosen == nil {
        _ = RunLoop.current.run(mode: .default, before: Date(timeIntervalSinceNow: 0.05))
    }
    window.isHidden = true
    window.rootViewController = nil
    let bytes = Array((chosen ?? "").utf8.prefix(Int(outSize) - 1))
    for (i, b) in bytes.enumerated() { out[i] = CChar(bitPattern: b) }
    out[bytes.count] = 0
    return 1
}
