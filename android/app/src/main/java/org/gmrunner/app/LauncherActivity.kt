package org.gmrunner.app

import android.Manifest
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.BitmapFactory
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.PickVisualMediaRequest
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.rounded.ViewList
import androidx.compose.material.icons.rounded.FolderOpen
import androidx.compose.material.icons.rounded.GridView
import androidx.compose.material.icons.rounded.Image
import androidx.compose.material.icons.rounded.Info
import androidx.compose.material.icons.rounded.Refresh
import androidx.compose.material.icons.rounded.SportsEsports
import androidx.compose.material3.Button
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ExperimentalMaterial3ExpressiveApi
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.FilledTonalIconToggleButton
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LargeFlexibleTopAppBar
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.LoadingIndicator
import androidx.compose.material3.MaterialExpressiveTheme
import androidx.compose.material3.MaterialShapes
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.MotionScheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.dynamicDarkColorScheme
import androidx.compose.material3.dynamicLightColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.material3.toShape
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Shape
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.LifecycleResumeEffect
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

class LauncherActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent { LauncherTheme { Launcher() } }
    }
}

@OptIn(ExperimentalMaterial3ExpressiveApi::class)
@Composable
fun LauncherTheme(content: @Composable () -> Unit) {
    val context = LocalContext.current
    val dark = isSystemInDarkTheme()
    val colors = when {
        Build.VERSION.SDK_INT >= 31 -> if (dark) dynamicDarkColorScheme(context) else dynamicLightColorScheme(context)
        dark -> darkColorScheme()
        else -> lightColorScheme()
    }
    MaterialExpressiveTheme(colorScheme = colors, motionScheme = MotionScheme.expressive(), content = content)
}

private fun play(context: Context, game: Game) {
    context.startActivity(Intent(context, GameActivity::class.java).putExtra(GameActivity.EXTRA_GAME, game.dir.absolutePath))
}

private fun askForStorage(context: Context) {
    if (Build.VERSION.SDK_INT >= 30) {
        try {
            context.startActivity(Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:" + context.packageName)))
        } catch (e: Exception) {
            context.startActivity(Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION))
        }
    } else if (context is ComponentActivity &&
        context.checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE) != PackageManager.PERMISSION_GRANTED) {
        context.requestPermissions(arrayOf(Manifest.permission.READ_EXTERNAL_STORAGE, Manifest.permission.WRITE_EXTERNAL_STORAGE), 1)
    }
}

@OptIn(ExperimentalMaterial3Api::class, ExperimentalMaterial3ExpressiveApi::class)
@Composable
fun Launcher() {
    val context = LocalContext.current
    val prefs = remember { context.getSharedPreferences("launcher", Context.MODE_PRIVATE) }
    var grid by remember { mutableStateOf(prefs.getBoolean("grid", true)) }
    var games by remember { mutableStateOf<List<Game>?>(null) }
    var root by remember { mutableStateOf<File?>(null) }
    var access by remember { mutableStateOf(Games.hasSharedAccess()) }
    var refreshes by remember { mutableIntStateOf(0) }
    var editing by remember { mutableStateOf<Game?>(null) }
    var licenses by remember { mutableStateOf(false) }
    if (licenses) {
        LicensesScreen(onBack = { licenses = false })
        return
    }

    // Look again whenever the launcher comes back (files copied in, access granted, a game ended).
    LifecycleResumeEffect(Unit) {
        access = Games.hasSharedAccess()
        refreshes++
        onPauseOrDispose { }
    }
    LaunchedEffect(refreshes) {
        val r = Games.root(context)
        val found = withContext(Dispatchers.IO) { Games.scan(r) }
        root = r
        games = found
    }

    val scroll = TopAppBarDefaults.exitUntilCollapsedScrollBehavior()
    Scaffold(
        modifier = Modifier.fillMaxSize().nestedScroll(scroll.nestedScrollConnection),
        topBar = {
            LargeFlexibleTopAppBar(
                title = { Text("gmRunner") },
                subtitle = {
                    val n = games?.size ?: 0
                    Text(if (n == 1) "1 game" else "$n games")
                },
                actions = {
                    IconButton(onClick = { licenses = true }) { Icon(Icons.Rounded.Info, contentDescription = "Licenses") }
                    IconButton(onClick = { refreshes++ }) { Icon(Icons.Rounded.Refresh, contentDescription = "Refresh") }
                    FilledTonalIconToggleButton(
                        checked = grid,
                        onCheckedChange = {
                            grid = it
                            prefs.edit().putBoolean("grid", it).apply()
                        },
                    ) {
                        Icon(if (grid) Icons.Rounded.GridView else Icons.AutoMirrored.Rounded.ViewList,
                            contentDescription = if (grid) "Show as list" else "Show as grid")
                    }
                },
                scrollBehavior = scroll,
            )
        },
    ) { padding ->
        val list = games
        when {
            list == null -> Box(Modifier.fillMaxSize().padding(padding), Alignment.Center) { LoadingIndicator() }
            list.isEmpty() -> EmptyState(Modifier.padding(padding), root, access) { askForStorage(context) }
            grid -> LazyVerticalGrid(
                columns = GridCells.Adaptive(132.dp),
                contentPadding = PaddingValues(16.dp, padding.calculateTopPadding() + 8.dp, 16.dp, padding.calculateBottomPadding() + 24.dp),
                horizontalArrangement = Arrangement.spacedBy(16.dp),
                verticalArrangement = Arrangement.spacedBy(20.dp),
            ) {
                items(list, key = { it.dir.path }) { game -> GridTile(game, { play(context, game) }, { editing = game }) }
            }
            else -> LazyColumn(
                contentPadding = PaddingValues(12.dp, padding.calculateTopPadding() + 4.dp, 12.dp, padding.calculateBottomPadding() + 24.dp),
                verticalArrangement = Arrangement.spacedBy(4.dp),
            ) {
                items(list, key = { it.dir.path }) { game -> ListRow(game, { play(context, game) }, { editing = game }) }
            }
        }
    }

    editing?.let { game ->
        EditSheet(game, onDone = {
            editing = null
            refreshes++
        })
    }
}

// The game's own icon, or the standard one: a gamepad on an expressive "cookie" shape.
@OptIn(ExperimentalMaterial3ExpressiveApi::class)
@Composable
fun GameIcon(game: Game, size: Dp, modifier: Modifier = Modifier) {
    val bitmap = remember(game.icon?.path, game.icon?.lastModified()) {
        game.icon?.let { BitmapFactory.decodeFile(it.path)?.asImageBitmap() }
    }
    if (bitmap != null) {
        Image(bitmap, contentDescription = null, contentScale = ContentScale.Crop,
            modifier = modifier.size(size).clip(RoundedCornerShape(size * 0.24f)))
    } else {
        val shape: Shape = MaterialShapes.Cookie9Sided.toShape()
        Box(modifier.size(size).clip(shape).background(MaterialTheme.colorScheme.primaryContainer), Alignment.Center) {
            Icon(Icons.Rounded.SportsEsports, contentDescription = null, tint = MaterialTheme.colorScheme.onPrimaryContainer,
                modifier = Modifier.size(size * 0.46f))
        }
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
fun GridTile(game: Game, onClick: () -> Unit, onLongClick: () -> Unit) {
    Column(
        Modifier.clip(RoundedCornerShape(28.dp)).combinedClickable(onClick = onClick, onLongClick = onLongClick).padding(8.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        BoxWithConstraints(Modifier.fillMaxWidth().aspectRatio(1f), Alignment.Center) { GameIcon(game, maxWidth * 0.92f) }
        Spacer(Modifier.height(10.dp))
        Text(game.title, style = MaterialTheme.typography.titleSmall, textAlign = TextAlign.Center, maxLines = 2,
            overflow = TextOverflow.Ellipsis)
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
fun ListRow(game: Game, onClick: () -> Unit, onLongClick: () -> Unit) {
    Surface(shape = RoundedCornerShape(24.dp), color = MaterialTheme.colorScheme.surfaceContainer) {
        ListItem(
            modifier = Modifier.combinedClickable(onClick = onClick, onLongClick = onLongClick),
            colors = ListItemDefaults.colors(containerColor = MaterialTheme.colorScheme.surfaceContainer),
            leadingContent = { GameIcon(game, 52.dp) },
            headlineContent = { Text(game.title, maxLines = 1, overflow = TextOverflow.Ellipsis) },
            supportingContent = {
                if (game.title != game.dir.name) Text(game.dir.name, maxLines = 1, overflow = TextOverflow.Ellipsis)
            },
        )
    }
}

@OptIn(ExperimentalMaterial3ExpressiveApi::class)
@Composable
fun EmptyState(modifier: Modifier, root: File?, access: Boolean, onAskAccess: () -> Unit) {
    Column(
        modifier.fillMaxSize().padding(32.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center,
    ) {
        Box(Modifier.size(120.dp).clip(MaterialShapes.Sunny.toShape()).background(MaterialTheme.colorScheme.tertiaryContainer),
            Alignment.Center) {
            Icon(Icons.Rounded.FolderOpen, contentDescription = null, tint = MaterialTheme.colorScheme.onTertiaryContainer,
                modifier = Modifier.size(52.dp))
        }
        Spacer(Modifier.height(24.dp))
        Text("No games yet", style = MaterialTheme.typography.headlineSmall)
        Spacer(Modifier.height(12.dp))
        Text("Copy each game's folder (the one with data.win) into:", textAlign = TextAlign.Center)
        Text(root?.path ?: "", style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.primary,
            textAlign = TextAlign.Center, modifier = Modifier.padding(top = 4.dp))
        if (!access) {
            Spacer(Modifier.height(20.dp))
            Text("To use the gmrunner folder on internal storage instead, allow All files access.",
                textAlign = TextAlign.Center, style = MaterialTheme.typography.bodyMedium)
            Spacer(Modifier.height(12.dp))
            Button(onClick = onAskAccess) { Text("Allow file access") }
        }
    }
}

// Long press: rename the game and change its icon.
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun EditSheet(game: Game, onDone: () -> Unit) {
    val context = LocalContext.current
    val sheet = rememberModalBottomSheetState(skipPartiallyExpanded = true)
    val scope = rememberCoroutineScope()
    var title by remember { mutableStateOf(game.title) }
    var current by remember { mutableStateOf(game) }
    var shortcut by remember { mutableStateOf(Games.shortcut(game)) }
    var shortcutMenu by remember { mutableStateOf(false) }
    val pickImage = rememberLauncherForActivityResult(ActivityResultContracts.PickVisualMedia()) { uri ->
        if (uri != null) scope.launch {
            withContext(Dispatchers.IO) { Games.setIcon(context, current, uri) }
            current = Games.load(current.dir)
        }
    }
    ModalBottomSheet(onDismissRequest = onDone, sheetState = sheet) {
        Column(Modifier.fillMaxWidth().padding(horizontal = 24.dp).navigationBarsPadding().padding(bottom = 16.dp),
            horizontalAlignment = Alignment.CenterHorizontally) {
            GameIcon(current, 96.dp)
            Spacer(Modifier.height(8.dp))
            Text(game.dir.name, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            Spacer(Modifier.height(16.dp))
            OutlinedTextField(value = title, onValueChange = { title = it }, label = { Text("Title") }, singleLine = true,
                modifier = Modifier.fillMaxWidth())
            Spacer(Modifier.height(12.dp))
            Box(Modifier.fillMaxWidth()) {
                OutlinedButton(onClick = { shortcutMenu = true }, modifier = Modifier.fillMaxWidth()) {
                    Text("In-game menu: " + (Games.SHORTCUTS.firstOrNull { it.first == shortcut }?.second ?: "Floating button"))
                }
                DropdownMenu(expanded = shortcutMenu, onDismissRequest = { shortcutMenu = false }) {
                    Games.SHORTCUTS.forEach { (value, label) ->
                        DropdownMenuItem(text = { Text(label) }, onClick = { shortcut = value; shortcutMenu = false })
                    }
                }
            }
            Spacer(Modifier.height(16.dp))
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp), modifier = Modifier.fillMaxWidth()) {
                FilledTonalButton(
                    onClick = { pickImage.launch(PickVisualMediaRequest(ActivityResultContracts.PickVisualMedia.ImageOnly)) },
                    modifier = Modifier.weight(1f),
                ) {
                    Icon(Icons.Rounded.Image, contentDescription = null, modifier = Modifier.size(18.dp))
                    Spacer(Modifier.size(8.dp))
                    Text("Change icon")
                }
                if (current.icon != null) {
                    TextButton(onClick = {
                        Games.clearIcon(current)
                        current = Games.load(current.dir)
                    }) { Text("Standard icon") }
                }
            }
            Spacer(Modifier.height(12.dp))
            Button(onClick = {
                Games.rename(current, title)
                Games.setShortcut(current, shortcut)
                scope.launch { sheet.hide() }.invokeOnCompletion { onDone() }
            }, modifier = Modifier.fillMaxWidth()) { Text("Save") }
        }
    }
}
