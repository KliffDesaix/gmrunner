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

// One controller, by its number (the second connected controller is controller 2): its buttons,
// and the name of the controller connected as it ("" when none is).
struct MenuController: Decodable {
    let name: String
    var buttons: [MenuButton]
}

struct MenuInfo: Decodable {
    let ini: String
    let readsKeyboard: Bool
    let readsGamepad: Bool
    var touchMode: String
    var shortcut: String
    var controllers: [MenuController]
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

    func controllerName(_ c: Int) -> String {
        let name = info.controllers[c].name
        return name.isEmpty ? "Not connected" : name
    }

    func setAutomatic(_ c: Int, _ i: Int, _ on: Bool) {
        info.controllers[c].buttons[i].set = !on
        if on { info.controllers[c].buttons[i].targets = info.controllers[c].buttons[i].auto }
        save(c, i)
    }

    func toggle(_ c: Int, _ i: Int, _ id: String) {
        info.controllers[c].buttons[i].set = true
        if let at = info.controllers[c].buttons[i].targets.firstIndex(of: id) { info.controllers[c].buttons[i].targets.remove(at: at) }
        else { info.controllers[c].buttons[i].targets.append(id) }
        save(c, i)
    }

    // map_a for controller 1, p2_map_a for controller 2, ...
    private func save(_ c: Int, _ i: Int) {
        let b = info.controllers[c].buttons[i]
        let key = (c == 0 ? "" : "p\(c + 1)_") + "map_" + b.id
        GameStore.writeIniKey(dir, key, b.set ? (b.targets.isEmpty ? "none" : b.targets.joined(separator: " ")) : nil)
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
                Section {
                    ForEach(model.info.controllers.indices, id: \.self) { c in
                        NavigationLink {
                            ControllerView(model: model, controller: c)
                        } label: {
                            LabeledContent("Controller \(c + 1)", value: model.controllerName(c))
                        }
                    }
                } header: {
                    Text("Controls")
                } footer: {
                    Text("Each controller has its own buttons. For co-op in a game that only reads the keyboard, give controllers 2 to 4 keys of their own.")
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

// What one controller's buttons press.
struct ControllerView: View {
    @ObservedObject var model: MenuModel
    let controller: Int

    var body: some View {
        List {
            Section {
                ForEach(model.info.controllers[controller].buttons.indices, id: \.self) { i in
                    let b = model.info.controllers[controller].buttons[i]
                    NavigationLink {
                        ButtonChoiceView(model: model, controller: controller, index: i)
                    } label: {
                        LabeledContent(b.label, value: model.summary(b.targets, own: b.pad))
                    }
                }
            } footer: {
                Text(model.controllerName(controller))
            }
        }
        .navigationTitle("Controller \(controller + 1)")
    }
}

struct ButtonChoiceView: View {
    @ObservedObject var model: MenuModel
    let controller: Int
    let index: Int

    var body: some View {
        let b = model.info.controllers[controller].buttons[index]
        List {
            Section {
                Toggle("Automatic", isOn: Binding(get: { !b.set }, set: { model.setAutomatic(controller, index, $0) }))
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
                    model.toggle(controller, index, t.id)
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
