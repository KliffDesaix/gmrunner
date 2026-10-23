package org.gmrunner.app

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.net.Uri
import android.os.Build
import android.os.Environment
import java.io.File

// A game folder and what the launcher shows for it. The title and icon the player sets are
// kept in the game's own gmrunner.ini (title=, icon=), where the iOS and Switch launchers find
// them too; the runner ignores those keys.
data class Game(val dir: File, val title: String, val icon: File?)

object Games {
    private val dataFiles = listOf("data.win", "game.unx", "game.droid", "game.ios", "assets/game.unx", "assets/game.droid")
    const val ICON_FILE = "gmrunner-icon.png"

    fun hasSharedAccess(): Boolean =
        if (Build.VERSION.SDK_INT >= 30) Environment.isExternalStorageManager()
        else true  // the storage permission, asked for by the launcher

    // Where games go: the gmrunner folder on internal storage when the app may read it, else the
    // app's own folder (the native runner picks the same one).
    fun root(context: Context): File {
        val shared = File(Environment.getExternalStorageDirectory(), "gmrunner")
        if (hasSharedAccess()) {
            shared.mkdirs()
            if (shared.canRead()) return shared
        }
        return context.getExternalFilesDir(null) ?: context.filesDir
    }

    fun scan(root: File): List<Game> =
        (root.listFiles() ?: emptyArray())
            .filter { it.isDirectory && !it.name.startsWith(".") && dataFiles.any { f -> File(it, f).isFile } }
            .map { load(it) }
            .sortedBy { it.title.lowercase() }

    fun load(dir: File): Game {
        val ini = readIni(dir)
        val title = ini["title"]?.takeIf { it.isNotBlank() } ?: dir.name
        val icon = ini["icon"]?.let { File(dir, it) }?.takeIf { it.isFile }
        return Game(dir, title, icon)
    }

    fun rename(game: Game, title: String) =
        writeIniKey(game.dir, "title", title.trim().takeIf { it.isNotEmpty() && it != game.dir.name })

    // The chosen picture, cropped to a square and scaled to 256 x 256, as the game's icon.
    fun setIcon(context: Context, game: Game, uri: Uri): Boolean {
        val source = context.contentResolver.openInputStream(uri)?.use { BitmapFactory.decodeStream(it) } ?: return false
        val side = minOf(source.width, source.height)
        val square = Bitmap.createBitmap(source, (source.width - side) / 2, (source.height - side) / 2, side, side)
        val scaled = Bitmap.createScaledBitmap(square, 256, 256, true)
        File(game.dir, ICON_FILE).outputStream().use { scaled.compress(Bitmap.CompressFormat.PNG, 100, it) }
        writeIniKey(game.dir, "icon", ICON_FILE)
        return true
    }

    fun clearIcon(game: Game) {
        File(game.dir, ICON_FILE).delete()
        writeIniKey(game.dir, "icon", null)
    }

    // How the in-game menu opens (menu_shortcut, read by the runner's GameMenu.cpp): ini value, label.
    val SHORTCUTS = listOf("button" to "Floating button", "fingers" to "Three-finger tap",
        "back" to "Back button", "none" to "Start + Select only")
    fun shortcut(game: Game): String = readIni(game.dir)["menu_shortcut"] ?: "button"
    fun setShortcut(game: Game, value: String) = writeIniKey(game.dir, "menu_shortcut", value.takeIf { it != "button" })

    private fun readIni(dir: File): Map<String, String> {
        val f = File(dir, "gmrunner.ini")
        if (!f.isFile) return emptyMap()
        return f.readLines().mapNotNull { line ->
            val eq = line.indexOf('=')
            if (eq <= 0 || line.startsWith("#") || line.startsWith(";")) null
            else line.substring(0, eq).trim() to line.substring(eq + 1).trim()
        }.toMap()
    }

    // Sets (or with null removes) one key, keeping the file's other lines as they are.
    fun writeIniKey(dir: File, key: String, value: String?) {
        val f = File(dir, "gmrunner.ini")
        val lines = if (f.isFile) f.readLines().toMutableList() else mutableListOf()
        val i = lines.indexOfFirst { it.substringBefore('=').trim() == key }
        when {
            value == null && i >= 0 -> lines.removeAt(i)
            value == null -> return
            i >= 0 -> lines[i] = "$key = $value"
            else -> lines.add("$key = $value")
        }
        f.writeText(lines.joinToString("\n", postfix = "\n"))
    }
}
