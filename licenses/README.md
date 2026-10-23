# Licenses

gmRunner itself is licensed under the GNU General Public License, version 3 or later
([../LICENSE](../LICENSE); [GPL-3.0.txt](GPL-3.0.txt) is the copy the apps show, with gmRunner's
copyright notice). The license texts of the third-party software in gmRunner's builds. Each app shows the ones it
contains (Android and iOS: the info button in the game list; Switch: **−** in the game list;
PortMaster: `gmrunner/licenses/` in the port folder).

| Component | License | Android | iOS | Switch | PortMaster | Desktop |
|---|---|:-:|:-:|:-:|:-:|:-:|
| SDL2 ([SDL2.txt](SDL2.txt)) | zlib | yes | yes | yes | from the device | system |
| GameMaker HTML5 runner, whose behaviour parts of the runtime follow ([gamemaker-html5.txt](gamemaker-html5.txt)) | Apache 2.0 | yes | yes | yes | yes | yes |
| stb_image, stb_truetype, stb_vorbis ([stb.txt](stb.txt)) | MIT or public domain | yes | yes | yes | yes | yes |
| Liberation Fonts: Sans, Serif, Mono ([liberation-fonts.txt](liberation-fonts.txt)) | SIL Open Font License 1.1 | yes | yes | yes | yes | yes |
| AndroidX, Jetpack Compose, Material 3, Material Icons ([androidx.txt](androidx.txt)) | Apache 2.0 | yes | | | | |
| Kotlin standard library, kotlinx.coroutines ([kotlin.txt](kotlin.txt)) | Apache 2.0 | yes | | | | |
| libnx ([libnx.txt](libnx.txt)) | ISC | | | yes | | |
| Mesa ([mesa.txt](mesa.txt)) | MIT and others | | | yes | | |
| libdrm ([libdrm.txt](libdrm.txt)) | MIT | | | yes | | |
| FFmpeg ([ffmpeg.txt](ffmpeg.txt)) | **GPL 2.0 or later** (devkitPro's build, Switch); LGPL 2.1 or later (PortMaster) | | | yes | yes | if installed |
| dav1d ([dav1d.txt](dav1d.txt)) | BSD 2-Clause | | | yes | | |
| bzip2 ([bzip2.txt](bzip2.txt)) | bzip2 (BSD-like) | yes | yes | yes | yes | if installed |
| zlib ([zlib.txt](zlib.txt)) | zlib | | | yes | | |
| Borealis ([borealis.txt](borealis.txt)) | Apache 2.0 | | | yes | | |
| NanoVG, Yoga, {fmt}, TinyXML-2, Tweeny, libretro-common, switch-libpulsar, glad (part of Borealis) | zlib / MIT | | | yes | | |
| Material Icons font (Borealis) ([material-icons-font.txt](material-icons-font.txt)) | Apache 2.0 | | | yes | | |

Full license texts: [Apache-2.0.txt](Apache-2.0.txt), [GPL-2.0.txt](GPL-2.0.txt), [LGPL-2.1.txt](LGPL-2.1.txt).

The toolchains' own runtime libraries (libc++ on Android, the Swift runtime on iOS, newlib and
libstdc++ on the Switch, libstdc++ in the PortMaster build) are covered by their compilers'
runtime exceptions and need no notice.

Desktop builds use the SDL2, FFmpeg and bzip2 installed on the system and are not distributed.

The Switch build links devkitPro's FFmpeg, which is configured with `--enable-gpl` (GPL version 2
or later); together with gmRunner (GPL version 3 or later) the NRO is distributed under the GPL
version 3. Whoever distributes builds must make the corresponding source available (gmRunner's
repository, plus the third-party sources linked from each notice).
