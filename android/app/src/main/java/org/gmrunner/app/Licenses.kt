package org.gmrunner.app

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.rounded.ArrowBack
import androidx.compose.material.icons.rounded.ExpandLess
import androidx.compose.material.icons.rounded.ExpandMore
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp

// The third-party software in the APK and its licenses (assets/licenses, from the repository's
// licenses/ folder).
private data class Component(val name: String, val license: String, val file: String)

private val components = listOf(
    Component("gmRunner", "GNU GPL version 3 or later", "GPL-3.0.txt"),
    Component("SDL2", "zlib License", "SDL2.txt"),
    Component("GameMaker HTML5 runner (YoYo Games)", "Apache License 2.0", "gamemaker-html5.txt"),
    Component("stb_image, stb_truetype, stb_vorbis", "MIT License or public domain", "stb.txt"),
    Component("Liberation Fonts", "SIL Open Font License 1.1", "liberation-fonts.txt"),
    Component("bzip2 (randomisation table)", "bzip2 License", "bzip2.txt"),
    Component("Android Jetpack, Jetpack Compose, Material 3", "Apache License 2.0", "androidx.txt"),
    Component("Kotlin standard library, kotlinx.coroutines", "Apache License 2.0", "kotlin.txt"),
    Component("Apache License 2.0", "Full text", "Apache-2.0.txt"),
)

// License files are wrapped for 80-100 columns: join each paragraph's lines so the text wraps to
// the screen instead, keeping numbered and bulleted items on lines of their own.
private val listItem = Regex("""^(\d+\.|[-*\u2022]|\([a-z0-9]+\))\s""")

private fun reflow(text: String): String =
    text.trim().split(Regex("\n\\s*\n")).joinToString("\n\n") { para ->
        val out = StringBuilder()
        for (raw in para.lines()) {
            val line = raw.trim()
            if (line.isEmpty()) continue
            if (out.isNotEmpty()) out.append(if (listItem.containsMatchIn(line)) "\n" else " ")
            out.append(line)
        }
        out.toString()
    }

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun LicensesScreen(onBack: () -> Unit) {
    BackHandler(onBack = onBack)
    val context = LocalContext.current
    Scaffold(topBar = {
        TopAppBar(
            title = { Text("Licenses") },
            navigationIcon = {
                IconButton(onClick = onBack) { Icon(Icons.AutoMirrored.Rounded.ArrowBack, contentDescription = "Back") }
            },
        )
    }) { padding ->
        LazyColumn(
            contentPadding = PaddingValues(12.dp, padding.calculateTopPadding() + 4.dp, 12.dp, padding.calculateBottomPadding() + 24.dp),
            verticalArrangement = Arrangement.spacedBy(4.dp),
            modifier = Modifier.fillMaxSize(),
        ) {
            item {
                Text("gmRunner is free software under the GNU General Public License, version 3 or later, and comes with NO WARRANTY. Source: github.com/KliffDesaix/gmrunner. It includes the following open-source software.", style = MaterialTheme.typography.bodyMedium,
                    modifier = Modifier.padding(horizontal = 8.dp, vertical = 8.dp))
            }
            items(components) { c ->
                var open by remember { mutableStateOf(false) }
                val text = remember(c.file) {
                    runCatching { reflow(context.assets.open("licenses/" + c.file).bufferedReader().readText()) }.getOrDefault("")
                }
                Surface(shape = RoundedCornerShape(20.dp), color = MaterialTheme.colorScheme.surfaceContainer) {
                    Column {
                        ListItem(
                            modifier = Modifier.clickable { open = !open },
                            colors = ListItemDefaults.colors(containerColor = MaterialTheme.colorScheme.surfaceContainer),
                            headlineContent = { Text(c.name) },
                            supportingContent = { Text(c.license) },
                            trailingContent = { Icon(if (open) Icons.Rounded.ExpandLess else Icons.Rounded.ExpandMore, contentDescription = null) },
                        )
                        if (open) {
                            Text(text, style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                                modifier = Modifier.padding(start = 16.dp, end = 16.dp, bottom = 16.dp))
                        }
                    }
                }
            }
        }
    }
}
