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
  src/            # flowcore (static lib, no QML): track, librarydb (SQLite),
                  #   libraryscanner (TagLib), lyrics (.lrc / embedded), playqueue
                  # QML singletons: library (models, watcher, scan thread), player (QMediaPlayer)
  qml/Main.qml    # ports ui-demo.html v3; `import Flow` for Library / Player
  tests/          # QtTest: tst_playqueue, tst_scanner, tst_lyrics, tst_player (real playback)
  STACK.md (this file)
  ui-demo.html    # clickable design spec (served via python http.server :8000)
```

Build + test: `cmake -S . -B build -G Ninja && cmake --build build && (cd build && ctest)`
Run: `build/flow --library=/path/to/music` (remembered; default `~/Music`).
Index: `~/.local/share/Flow/Flow/library.db`, cover cache: `~/.cache/Flow/Flow/covers/`.

## Status
Done:
1. C++ Player (QMediaPlayer) + QML transport, seek, volume, shuffle/repeat, queue
   (play next / add / remove / drag-reorder), synced lyrics, skip unplayable files.
2. TagLib scan → SQLite → LibraryModel/AlbumModel → QML views; incremental rescans,
   QFileSystemWatcher, likes persisted, embedded + folder cover art.

Next:
3. MPRIS (`org.mpris.MediaPlayer2`) over QtDBus.
4. Albums view + mixes/playlists (rail entries are still placeholders).
5. Real waveform peaks (the drawer waveform is decorative), persist queue across restarts.
