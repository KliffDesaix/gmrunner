package org.gmrunner.app

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.activity.compose.setContent
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyListScope
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.rounded.ArrowBack
import androidx.compose.material.icons.rounded.PlayArrow
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Checkbox
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.SegmentedButton
import androidx.compose.material3.SegmentedButtonDefaults
import androidx.compose.material3.SingleChoiceSegmentedButtonRow
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import org.json.JSONArray
import org.json.JSONObject
import java.io.File

// The in-game menu on Android: Material 3 over the paused game, started by GameActivity when the
// runner's menu opens, from the runner's description of the game's controls (GameMenu.cpp).
// Changes go straight into the game's gmrunner.ini, which the runner reads again on return.
class MenuActivity : ComponentActivity() {
    companion object {
        const val EXTRA_JSON = "json"
        const val RESULT_EXIT = RESULT_FIRST_USER
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val info = MenuInfo.parse(intent.getStringExtra(EXTRA_JSON) ?: "{}")
        setContent {
            LauncherTheme {
                GameMenu(info) { exit ->
                    setResult(if (exit) RESULT_EXIT else RESULT_OK)
                    finish()
                }
            }
        }
    }
}

class MenuTarget(val id: String, val name: String, val inGame: Boolean)

// One button: what it presses ("targets", ids as gmrunner.ini writes them) and what it would
// press automatically; `pad` is its own gamepad button.
class MenuButton(val id: String, val label: String, val pad: String, set: Boolean, targets: List<String>, val auto: List<String>) {
    var set by mutableStateOf(set)
    var targets by mutableStateOf(targets)
}

class MenuInfo(
    val dir: File, val title: String, val readsKeyboard: Boolean, val readsGamepad: Boolean,
    touchMode: String, shortcut: String, val buttons: List<MenuButton>, val keys: List<MenuTarget>, val pads: List<MenuTarget>,
) {
    var touchMode by mutableStateOf(touchMode)
    var shortcut by mutableStateOf(shortcut)

    fun name(id: String) = (keys + pads).firstOrNull { it.id == id }?.name ?: id

    // What a button presses; its own gamepad button only when that is all (the usual case).
    fun summary(targets: List<String>, own: String): String {
        val shown = targets.filter { it != own || targets.size == 1 }
        return if (shown.isEmpty()) "Nothing" else shown.joinToString(" + ") { name(it) }
    }

    fun setAutomatic(b: MenuButton, on: Boolean) {
        b.set = !on
        if (on) b.targets = b.auto
        save(b)
    }

    fun toggle(b: MenuButton, id: String) {
        b.set = true
        b.targets = if (id in b.targets) b.targets - id else b.targets + id
        save(b)
    }

    private fun save(b: MenuButton) = Games.writeIniKey(dir, "map_${b.id}",
        if (!b.set) null else if (b.targets.isEmpty()) "none" else b.targets.joinToString(" "))

    fun changeTouchMode(mode: String) {
        touchMode = mode
        Games.writeIniKey(dir, "touch_controls", mode.takeIf { it != "auto" })
    }

    fun changeShortcut(value: String) {
        shortcut = value
        Games.writeIniKey(dir, "menu_shortcut", value.takeIf { it != "button" })
    }

    companion object {
        fun parse(json: String): MenuInfo {
            val o = JSONObject(json)
            val dir = File(o.optString("ini", "/gmrunner.ini")).parentFile ?: File("/")
            fun strings(a: JSONArray) = List(a.length()) { a.getString(it) }
            fun targets(a: JSONArray) = List(a.length()) {
                val t = a.getJSONObject(it)
                MenuTarget(t.getString("id"), t.getString("name"), t.optBoolean("inGame"))
            }
            val buttons = o.optJSONArray("buttons") ?: JSONArray()
            return MenuInfo(
                dir, Games.load(dir).title, o.optBoolean("readsKeyboard"), o.optBoolean("readsGamepad"),
                o.optString("touchMode", "auto"), o.optString("shortcut", "button"),
                List(buttons.length()) {
                    val b = buttons.getJSONObject(it)
                    MenuButton(b.getString("id"), b.getString("label"), b.getString("pad"), b.optBoolean("set"),
                        strings(b.getJSONArray("targets")), strings(b.getJSONArray("auto")))
                },
                targets(o.optJSONArray("keys") ?: JSONArray()), targets(o.optJSONArray("pads") ?: JSONArray()),
            )
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun GameMenu(info: MenuInfo, onClose: (exit: Boolean) -> Unit) {
    val sheet = rememberModalBottomSheetState(skipPartiallyExpanded = true)
    var editing by remember { mutableStateOf<MenuButton?>(null) }
    ModalBottomSheet(onDismissRequest = { onClose(false) }, sheetState = sheet) {
        BackHandler(enabled = editing != null) { editing = null }
        LazyColumn(Modifier.fillMaxWidth().navigationBarsPadding().padding(bottom = 16.dp)) {
            val b = editing
            if (b == null) mainPage(info, onEdit = { editing = it }, onClose = onClose)
            else buttonPage(info, b, onBack = { editing = null })
        }
    }
}

private fun LazyListScope.sectionTitle(text: String) = item {
    Text(text, style = MaterialTheme.typography.titleSmall, color = MaterialTheme.colorScheme.primary,
        modifier = Modifier.padding(start = 24.dp, end = 24.dp, top = 20.dp, bottom = 4.dp))
}

private fun LazyListScope.mainPage(info: MenuInfo, onEdit: (MenuButton) -> Unit, onClose: (Boolean) -> Unit) {
    item {
        Row(Modifier.fillMaxWidth().padding(horizontal = 24.dp), verticalAlignment = Alignment.CenterVertically) {
            Text(info.title, style = MaterialTheme.typography.headlineSmall, maxLines = 1, overflow = TextOverflow.Ellipsis,
                modifier = Modifier.weight(1f))
            Button(onClick = { onClose(false) }) {
                Icon(Icons.Rounded.PlayArrow, contentDescription = null, modifier = Modifier.size(18.dp))
                Spacer(Modifier.size(8.dp))
                Text("Resume")
            }
        }
    }
    sectionTitle("Controls")
    items(info.buttons, key = { it.id }) { b ->
        ListItem(
            modifier = Modifier.clickable { onEdit(b) }.padding(horizontal = 8.dp),
            colors = ListItemDefaults.colors(containerColor = MaterialTheme.colorScheme.surfaceContainerLow),
            headlineContent = { Text(b.label) },
            trailingContent = { Text(info.summary(b.targets, b.pad), color = MaterialTheme.colorScheme.onSurfaceVariant) },
        )
    }
    sectionTitle("On-screen controls")
    item {
        val modes = listOf("auto" to "Automatic", "on" to "On", "off" to "Off")
        SingleChoiceSegmentedButtonRow(Modifier.fillMaxWidth().padding(horizontal = 24.dp)) {
            modes.forEachIndexed { i, (value, label) ->
                SegmentedButton(selected = info.touchMode == value, onClick = { info.changeTouchMode(value) },
                    shape = SegmentedButtonDefaults.itemShape(i, modes.size)) { Text(label) }
            }
        }
    }
    sectionTitle("Menu shortcut")
    item {
        var open by remember { mutableStateOf(false) }
        Box(Modifier.fillMaxWidth().padding(horizontal = 24.dp)) {
            OutlinedButton(onClick = { open = true }, modifier = Modifier.fillMaxWidth()) {
                Text(Games.SHORTCUTS.firstOrNull { it.first == info.shortcut }?.second ?: "Floating button")
            }
            DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
                Games.SHORTCUTS.forEach { (value, label) ->
                    DropdownMenuItem(text = { Text(label) }, onClick = { info.changeShortcut(value); open = false })
                }
            }
        }
        Text("Start and Select held together on a controller always open this menu.",
            style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(horizontal = 24.dp, vertical = 8.dp))
    }
    item {
        Button(
            onClick = { onClose(true) },
            colors = ButtonDefaults.buttonColors(containerColor = MaterialTheme.colorScheme.errorContainer,
                contentColor = MaterialTheme.colorScheme.onErrorContainer),
            modifier = Modifier.fillMaxWidth().padding(horizontal = 24.dp, vertical = 12.dp),
        ) { Text("Exit game") }
    }
}

private fun LazyListScope.buttonPage(info: MenuInfo, b: MenuButton, onBack: () -> Unit) {
    item {
        Row(Modifier.fillMaxWidth().padding(horizontal = 12.dp), verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(4.dp)) {
            IconButton(onClick = onBack) { Icon(Icons.AutoMirrored.Rounded.ArrowBack, contentDescription = "Back") }
            Text("${b.label} button", style = MaterialTheme.typography.headlineSmall)
        }
    }
    item {
        ListItem(
            modifier = Modifier.clickable { info.setAutomatic(b, b.set) }.padding(horizontal = 8.dp),
            headlineContent = { Text("Automatic") },
            supportingContent = { Text("For this game: ${info.summary(b.auto, b.pad)}") },
            trailingContent = { Switch(checked = !b.set, onCheckedChange = { info.setAutomatic(b, it) }) },
        )
    }
    if (!b.set) return
    val keysFirst = info.readsKeyboard || !info.readsGamepad
    val sections = listOf("Keys" to info.keys, "Gamepad" to info.pads).let { if (keysFirst) it else it.reversed() }
    for ((title, targets) in sections) {
        sectionTitle(title)
        items(targets, key = { title + it.id }) { t ->
            ListItem(
                modifier = Modifier.clickable { info.toggle(b, t.id) }.padding(horizontal = 8.dp),
                leadingContent = { Checkbox(checked = t.id in b.targets, onCheckedChange = { info.toggle(b, t.id) }) },
                headlineContent = { Text(t.name) },
                supportingContent = if (t.inGame) ({ Text("Used by the game") }) else null,
            )
        }
    }
}
