// The in-game menu on iOS: a SwiftUI sheet over the paused game, built from the runner's
// description of the game's controls (GameMenu.cpp). Changes go straight into the game's
// gmrunner.ini, which the runner reads again when the sheet closes.
import SwiftUI
import UIKit

struct MenuTarget: Decodable, Identifiable {
    let id: String  // as gmrunner.ini writes it: "Z", "vk_space", "gp_face1"
    let name: String
    let inGame: Bool
}

struct MenuButton: Decodable, Identifiable {
    let id: String     // map_<id>
    let label: String
    let pad: String    // its own gamepad button
    var set: Bool      // chosen by the player rather than automatic
    var targets: [String]
    let auto: [String]
}

struct MenuInfo: Decodable {
    let ini: String
    let readsKeyboard: Bool
    let readsGamepad: Bool
    var touchMode: String
    var shortcut: String
    var buttons: [MenuButton]
    let keys: [MenuTarget]
    let pads: [MenuTarget]
}

final class MenuModel: ObservableObject {
    @Published var info: MenuInfo
    let dir: URL
    let title: String

    init(_ info: MenuInfo) {
        self.info = info
        dir = URL(fileURLWithPath: info.ini).deletingLastPathComponent()
        title = GameStore.load(dir).title
    }

    func name(_ id: String) -> String { (info.keys + info.pads).first { $0.id == id }?.name ?? id }

    // What a button presses; its own gamepad button only when that is all (the usual case).
    func summary(_ targets: [String], own: String) -> String {
        let shown = targets.filter { $0 != own || targets.count == 1 }
        return shown.isEmpty ? "Nothing" : shown.map(name).joined(separator: " + ")
    }

    func setAutomatic(_ i: Int, _ on: Bool) {
        info.buttons[i].set = !on
        if on { info.buttons[i].targets = info.buttons[i].auto }
        save(i)
    }

    func toggle(_ i: Int, _ id: String) {
        info.buttons[i].set = true
        if let at = info.buttons[i].targets.firstIndex(of: id) { info.buttons[i].targets.remove(at: at) }
        else { info.buttons[i].targets.append(id) }
        save(i)
    }

    private func save(_ i: Int) {
        let b = info.buttons[i]
        GameStore.writeIniKey(dir, "map_" + b.id, b.set ? (b.targets.isEmpty ? "none" : b.targets.joined(separator: " ")) : nil)
    }

    func setTouchMode(_ mode: String) {
        info.touchMode = mode
        GameStore.writeIniKey(dir, "touch_controls", mode == "auto" ? nil : mode)
    }

    func setShortcut(_ value: String) {
        info.shortcut = value
        GameStore.writeIniKey(dir, "menu_shortcut", value == "button" ? nil : value)
    }
}

struct GameMenuView: View {
    @ObservedObject var model: MenuModel
    let close: (Int32) -> Void  // 0 resume, 1 exit

    var body: some View {
        NavigationStack {
            List {
                Section("Controls") {
                    ForEach(model.info.buttons.indices, id: \.self) { i in
                        let b = model.info.buttons[i]
                        NavigationLink {
                            ButtonChoiceView(model: model, index: i)
                        } label: {
                            LabeledContent(b.label, value: model.summary(b.targets, own: b.pad))
                        }
                    }
                }
                Section {
                    Picker("On-Screen Controls", selection: Binding(get: { model.info.touchMode }, set: model.setTouchMode)) {
                        Text("Automatic").tag("auto")
                        Text("On").tag("on")
                        Text("Off").tag("off")
                    }
                    Picker("Menu Shortcut", selection: Binding(get: { model.info.shortcut }, set: model.setShortcut)) {
                        ForEach(GameStore.shortcuts, id: \.value) { Text($0.label).tag($0.value) }
                    }
                } footer: {
                    Text("Start and Select held together on a controller always open this menu.")
                }
                Section {
                    Button(role: .destructive) { close(1) } label: { Label("Exit Game", systemImage: "xmark.circle") }
                }
            }
            .navigationTitle(model.title)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) { Button("Resume") { close(0) } }
            }
        }
    }
}

struct ButtonChoiceView: View {
    @ObservedObject var model: MenuModel
    let index: Int

    var body: some View {
        let b = model.info.buttons[index]
        List {
            Section {
                Toggle("Automatic", isOn: Binding(get: { !b.set }, set: { model.setAutomatic(index, $0) }))
            } footer: {
                Text("Automatic for this game: \(model.summary(b.auto, own: b.pad))")
            }
            if b.set {
                let keysFirst = model.info.readsKeyboard || !model.info.readsGamepad
                if keysFirst { section("Keys", model.info.keys, b) }
                section("Gamepad", model.info.pads, b)
                if !keysFirst { section("Keys", model.info.keys, b) }
            }
        }
        .navigationTitle("\(b.label) Button")
    }

    private func section(_ title: String, _ targets: [MenuTarget], _ b: MenuButton) -> some View {
        Section(title) {
            ForEach(targets) { t in
                Button {
                    model.toggle(index, t.id)
                } label: {
                    HStack {
                        VStack(alignment: .leading) {
                            Text(t.name)
                            if t.inGame { Text("Used by the game").font(.caption).foregroundStyle(.secondary) }
                        }
                        Spacer()
                        if b.targets.contains(t.id) { Image(systemName: "checkmark").foregroundStyle(.tint) }
                    }
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
            }
        }
    }
}

// The sheet's state between the runner's calls: its result, and a swipe down counting as Resume.
final class GameMenuHost: NSObject, UIAdaptivePresentationControllerDelegate {
    static let shared = GameMenuHost()
    var result: Int32 = 0
    weak var controller: UIViewController?

    func presentationControllerDidDismiss(_ presentationController: UIPresentationController) {
        if result < 0 { result = 0 }
    }

    func close(_ r: Int32) {
        if result < 0 { result = r }
        controller?.dismiss(animated: true)
    }
}

// Called by the runner (TargetMobile.cpp) when its menu opens: shows the sheet; 0 if it can't.
@_cdecl("gmr_ios_show_menu")
public func gmrIosShowMenu(_ json: UnsafePointer<CChar>) -> Int32 {
    guard let data = String(cString: json).data(using: .utf8),
          let info = try? JSONDecoder().decode(MenuInfo.self, from: data),
          var top = GMRSceneDelegate.scene?.keyWindow?.rootViewController
    else { return 0 }
    while let presented = top.presentedViewController { top = presented }
    let host = GameMenuHost.shared
    host.result = -1
    let controller = UIHostingController(rootView: GameMenuView(model: MenuModel(info)) { host.close($0) })
    controller.presentationController?.delegate = host
    host.controller = controller
    top.present(controller, animated: true)
    return 1
}

// -1 while the sheet is up, then 0 (resume) or 1 (exit the game).
@_cdecl("gmr_ios_menu_result")
public func gmrIosMenuResult() -> Int32 { GameMenuHost.shared.result }
