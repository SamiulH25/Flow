# Flow — Stack Decision

Date: 2026-09-22 (rev 2 — Qt6 QML)
Goal: supremely stylish + functional native music player
Scope v1: Linux-first (Hyprland/Wayland), pixel-perfect custom UI, local playback only (mp3/flac/wav, mixes, metadata, seek)

## Decision: C++ + Qt6 QML

Truly native: GPU-rendered QtQuick scene graph, no webview. Verified on this machine:
Qt 6.11.2 (`base`, `declarative`, `multimedia` + `ffmpeg` backend, `wayland`, `svg`,
`shadertools`), TagLib 2.3.2, ffmpeg 9, sqlite, cmake, g++ 16. Session: `wayland / Hyprland`.

### Frontend — QML (QtQuick + QtQuick.Controls, fully custom-styled)
- Why: the industry answer for stylish native players. Custom components, shaders,
  transitions — no system-widget look. QML units are device-independent + anchor/Layout
  based, so tiling resizes on Hyprland reflow instead of breaking (see scaling rules below).
- Stock Controls only as behavior primitives; every visual is custom (matches `ui-demo.html` v3).
- Dev tools: `qmllint`, `qmlformat` (`sudo pacman -S qt6-tools ninja` — recommended, not yet installed).

### Backend — C++17/20
- `QMediaPlayer` + `QAudioOutput` (ffmpeg backend): mp3/flac/wav, seek, volume, queue.
  Known limit: no guaranteed gapless in QtMultimedia — v1 accepts it; escape hatch is a
  custom `QAudioSink` + decoder pipeline if gapless becomes a requirement.
- `TagLib` (installed): tags + embedded cover art (artist, album, track, art).
- `QtSql` + SQLite: library index (fast search/sort over 1k+ tracks).
- `QFileSystemWatcher`: library rescan on file change.
- `QtDBus` MPRIS (`org.mpris.MediaPlayer2`): `playerctl`, Hyprland keybinds, tray/shell integration.

### Why not Tauri (rejected rev 1)
- Tauri's UI is HTML in a system webview (WebKitGTK) — native binary, not native rendering.
- WebKitGTK on Linux carries NVIDIA/DMABUF workarounds and weaker perf history.
- The `ui-demo.html` stays as the clickable design spec; visuals get ported to QML 1:1.

### Why not the other natives
- GTK4/libadwaita: system look, wrong fit for custom UI (also: we're on Hyprland, not GNOME).
- Slint: good, but GPL/commercial-license friction + smaller ecosystem than Qt.
- Iced/egui: pure Rust but styling is code, not declarative — slower to reach the demo's polish.

## Hyprland / Wayland notes
- `qt6-wayland` installed; Qt picks the Wayland platform automatically. No XWayland needed.
- Keep a normal toplevel window — Hyprland draws borders/focus; QML draws its own in-app
  top bar (like the demo). No frameless hacks for v1.
- Fractional scaling: rely on Qt + Wayland output scaling; never hardcode pixels in QML
  (use anchors, Layouts, `font.pixelSize` sparingly vs `pointSize`).

## Scaling rules (from demo stress test: 1600/1100/800/560px)
1. No fixed widths on panels — `Layout.fillWidth`, `anchors`, `minmax`-style fluid grids.
2. Tiered collapse, same as demo: full 3-pane → now-playing slide-over drawer (≤ ~1080px
   equivalent) → rail hidden (narrow tiles) → condensed dock with time on second row.
3. Decorative `::before`-style art must be clipped to its item (`clip: true` in QML —
   this exact bug produced a giant lime ellipse in the demo).
4. Header labels must hide in sync with their columns when the track grid collapses.

## Project layout
```
Flow/
  CMakeLists.txt
  src/            # flowcore (static lib, no QML): track, librarydb (SQLite, schema v3),
                  #   libraryscanner (TagLib), lyrics (.lrc / embedded), playqueue, m3u,
                  #   ytdlp (yt-dlp command line + output protocol)
                  # QML singletons: library (views, mixes, M3U, watcher, scan thread),
                  #   player (QMediaPlayer, queue persistence), waveform (QAudioDecoder)
                  #   downloader (yt-dlp jobs → <music folder>/YouTube)
                  # mpris: org.mpris.MediaPlayer2 over QtDBus
  qml/Main.qml    # ports ui-demo.html v3; `import Flow` for Library / Player
  data/           # flow.desktop, flow.svg (app icon, also embedded as the window icon)
  packaging/arch/ # PKGBUILD (builds this checkout's committed HEAD)
  tests/          # QtTest: playqueue, scanner, lyrics, m3u, ytdlp, waveform, library,
                  #   downloader (fake yt-dlp script, no network), player, mpris
  STACK.md (this file)
  ui-demo.html    # clickable design spec (served via python http.server :8000)
```

Dev build + tests: `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build && ctest --test-dir build`
(tst_player / tst_waveform play or decode short silent WAVs; tst_mpris runs on a private
bus via dbus-run-session.)
Release + install: `cmake -S . -B build-release -G Ninja && cmake --build build-release &&
sudo cmake --install build-release` — or on Arch: `cd packaging/arch && makepkg -si`.
Per-user (no sudo, shows up in the Omarchy launcher): configure with
`-DCMAKE_INSTALL_PREFIX=$HOME/.local`, then `cmake --install`; the desktop entry's Exec is
absolute, so it does not depend on the launcher's PATH. The music folder can be picked in
the app (Local library → CHANGE), so menu launches need no arguments.
Run: `flow --library /path/to/music` (remembered; default `~/Music`). `flow --help`.
Index: `~/.local/share/Flow/Flow/library.db`, cover cache: `~/.cache/Flow/Flow/covers/`.
Settings + saved queue: `~/.config/Flow/Flow.conf`.

## Status (v1 complete)
- Playback: QMediaPlayer transport, seek, volume, shuffle, repeat off / all / one, queue
  (play next / add / remove / drag-reorder), synced lyrics, unplayable files skipped, queue +
  position restored on launch (paused).
- Library: TagLib scan → SQLite, incremental rescans, QFileSystemWatcher, likes, embedded +
  folder cover art, search / format filters / sorts.
- Views: Overview, Albums grid → album detail, Tracks, Liked, mixes (create / rename /
  delete, + MIX from any track, remove, drag-reorder, M3U import / export). Deleted files
  leave mixes (FK cascade).
- Waveform: real RMS overview per track, decoded at 8 kHz mono, cached in the index.
- MPRIS: playerctl / Hyprland keybinds / shell widgets (metadata + art, seek, volume, loop
  None/Playlist/Track, shuffle, Raise, Quit).
- Layout: full 3-pane → drawer slide-over (<1080px) → rail as overlay behind ☰ (<820px);
  dock and filter chips adapt down to the 560px minimum.
- Desktop integration: .desktop entry + icon, Wayland app_id `flow`.
- Downloads (rail → Downloads): paste a YouTube / YouTube Music link — a song, a playlist
  or an album. `yt-dlp` (+ `ffmpeg`, optional deps) fetches best audio → MP3 VBR V0 with
  tags and a square-cropped cover into `<music folder>/YouTube/<artist>/<album>/`; the
  rescan picks them up. Playlists also become a mix in playlist order; albums
  (`browse/MPREb…`, `list=OLAK5uy…`) get track numbers and show up under Albums.
  One job at a time, cancellable (kills yt-dlp's whole process group), unavailable
  videos skipped. Work in progress lives in `~/.cache/Flow/Flow/yt-dlp/`. `watch?v=…&list=…`
  links default to the song (they are often endless radios); PLAYLIST takes the list.

Deliberately not in v1:
- Gapless playback (QtMultimedia limit; escape hatch above: QAudioSink + decoder pipeline).
- Editing tags, streaming / remote sources (downloads are the way in), multiple library folders.
