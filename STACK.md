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
  src/cli.cpp     # flow-cli: the headless front end (download / scan / list / mixes / doctor)
  data/           # flow.desktop, flow.svg (app icon, also embedded as the window icon)
  packaging/arch/ # PKGBUILD (builds this checkout's committed HEAD)
  tests/          # QtTest: playqueue, scanner, lyrics, m3u, ytdlp, waveform, library,
                  #   downloader (fake yt-dlp script, no network), player, mpris
  STACK.md (this file)
  ui-demo.html    # clickable design spec (served via python http.server :8000)
  ui-demo-console.html  # second design spec — "dark instrument panel". Near-black
                       #   elevation ladder, dense 42px rows, one mint accent used only
                       #   for playback state, abstract CSS cover art (8 compositions).
                       #   Sidebar / ledger / now-playing + player bar. No texture.
```

Design specs: `python3 -m http.server 8100 --bind 127.0.0.1` from the repo root, then
<http://127.0.0.1:8100/ui-demo-console.html> (or `/ui-demo.html`). Both load
`data/fonts/*.ttf` from disk, so they work offline.

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
Headless: `flow-cli <command>` — `download <url>…` queues exactly what the app's Downloads
page queues (a song, or a whole playlist / album; a playlist also becomes a mix), while
`scan`, `list`, `mixes` and `doctor` read what the app has indexed. Same index, same yt-dlp
backend, same settings, no window: global `--library`, `--yt-dlp`, `--json`, `--quiet`;
`download --playlist/--song --enrich -j/--jobs`; `list --search/--format/--sort/--limit`;
`mixes --tracks`; `scan --enrich`. Progress and diagnostics go to stderr, the result to
stdout, so `flow-cli list --json | jq` pipes cleanly. Exit codes: 0 done, 1 a download
failed, 2 a link or an option was rejected, 130 interrupted by SIGINT (143 by SIGTERM):
either signal unwinds through the same destructors the app uses, so yt-dlp's process group
dies with the run and no working folder is left behind. It drives the same `Downloader` the
QML page drives: job
queue and per-track status, playlist → mix, album track numbers and the rescan that picks
the files up are the app's own code paths. Downloads wait out the library's debounced
rescan before the summary prints, so what it reports is what the index holds.
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
  Up to 3 jobs at once (QUEUE header steps 1–4), each cancellable (kills yt-dlp's
  whole process group), unavailable videos skipped. Click a queued playlist to
  expand its per-track status (queued / % / done / skipped). Work in progress
  lives in `~/.cache/Flow/Flow/yt-dlp/`. `watch?v=…&list=…`
  links default to the song (they are often endless radios); PLAYLIST takes the list.

## Status additions (visualizer, enrichment, loudness)

- Visualizer: a live 48-band spectrum plus an oscilloscope in the now-playing
  panel. The decoded PCM is tapped with `QAudioBufferOutput` (Qt 6.8+, ffmpeg
  backend only) and analysed in C++ — a 2048-point radix-2 FFT, log-spaced
  bands 30 Hz .. 16 kHz, min/max scope columns, all smoothed at 60 Hz so QML
  only draws numbers. Two things the backend forces: the tap is read in a
  *direct* connection (the samples are only alive while the signal runs, so a
  context object would let Qt hand the buffer over asynchronously), and only
  `frameCount()` samples per block are read (the backend reports a full
  interleaved length in `byteCount()` but hands over a shorter run; reading the
  rest walks off the end of it). A 1 kHz tone still lands in its own band.
  Without a tap (a non-ffmpeg backend) the analyser simply rests at zero.

- Metadata enrichment: MusicBrainz recording search plus the Cover Art
  Archive, into the index only — **nothing is ever written back to the audio
  files**. Results land in their own `enrichment` table (schema v4) and survive
  rescans; gaps in a file's own tags are filled from it at load time, so a
  hand-corrected tag always wins. A match has to agree on title *and* length,
  and when the title names the artist ("Eminem - Stan") the recording's artist
  has to be that artist, which is what keeps tribute albums and cover versions
  out. yt-dlp's shape is undone first: "Artist - Title" is split, "ft. X" and a
  trailing "(Short Version)" are dropped, and a channel name in the artist
  field ("WheatusOfficial", "CLS Videos") loses to the name in the title.
  "&" reads as "and", and a search that comes back empty with an artist gets
  one title-only retry, which is what finds "Mumford and Sons" against MusicBrainz's
  "Mumford & Sons". A compilation release contributes the recording but not its
  album artist, year or cover. Requests are serialised at one per 1.1 s
  (MusicBrainz asks for one per second), genres and album artist are fetched
  once per release so a whole album costs one extra request, and a 5xx is
  retried once. Triggered per track or per view from the row menu, and
  automatically for whatever a download just produced.

- Loudness: the pass that draws the waveform also measures the track — RMS in
  dBFS and the highest sample — and caches both in the index. The now-playing
  panel shows them; the dock's MATCH chip turns a track louder than the target
  down to meet it. Matching is still one-sided: the mixer below has a gain
  stage, but a track-level boost would have to be applied per track and could
  not be undone by the limiter without changing the sound, so quiet tracks are
  left where the user put them and the library is levelled down instead.

- Full sound (`Mixer` + `Mastering`): `QMediaPlayer` still decodes and still
  owns the clock, but its `QAudioOutput` is muted and the decoded PCM is tapped
  from `QAudioBufferOutput` *before* the output's volume stage. That tap is
  pushed through a fixed chain — low shelf (100 Hz, +4 dB), presence bell
  (3.2 kHz, +1.8 dB), air shelf (11 kHz, +1 dB), stereo-linked RMS compression
  (2:1, −22 dBFS, 6 dB makeup), mid/side width ×1.10 and a peak limiter at
  −0.45 dBFS — then played on a second `QAudioSink` at the tap's fixed format
  (48 kHz stereo float). Two streams on one device share a clock, so the tap
  rate and the sink's consumption do not drift; a 2 s ring absorbs the
  renderer's bursts and drops the oldest audio if the sink stalls. The player's
  own sink stays muted (its volume is zeroed too, so a fallback can never
  blast). The dock's FULL chip toggles it.
  Because the whole design leans on the ffmpeg backend's tap, a backend without
  it (gstreamer) would otherwise play silence: on the first play the player
  arms a 1.5 s watchdog, and if no buffer has reached the mixer it unmutes the
  plain output, reports `fullnessActive` false and leaves the mode off for the
  session. A sink that refuses to open takes the same exit. The tap leads the
  player's clock by the backend's ~110 ms buffer and the mixer's sink adds its
  own ~40–80 ms, so the mix is a few dozen ms ahead of `Player.position` —
  inaudible for music and lyrics, and not compensated.
  The chain is 48 kHz fixed: asking the tap for the device format would put a
  resampler on every buffer and fork the DSP by sample format.

Deliberately not done: a user-editable EQ (the chain is one taste, not a
parametric EQ), AcoustID fingerprinting, and any write-back of tags to files.

Tests: `tst_mastering` (low-end vs presence balance, compression of a 20 dB
step, transient limiting, silence), `tst_visualizer` (FFT bin placement, band
mapping, scope envelope, empty and unknown windows) and `tst_enricher` (matching rules, yt-dlp title shapes,
compilations, store + survive a rescan, queue pacing, retry on 5xx) run against
a fake backend, so neither needs a network connection.

Navidrome (Subsonic API): a server's library can be browsed and streamed; nothing
is copied to disk. Settings are `navidrome/url`, `navidrome/user` and
`navidrome/password` in `Flow.conf`. A sync (`Library::navidromeSync`) pages
`getAlbumList2`, reads each album with `getAlbum`, and upserts its songs into the
existing `tracks` table under `navidrome://<server-key>/<songId>`, so the schema did
not change. The server key is a hash of the URL and user, not the password. Playback
goes through `Library::sourceUrl`, which builds `/rest/stream?id=…` with a fresh salt
and token (`t = md5(password + s)`) each time and never stores it. Covers are
downloaded into the cover directory as `navidrome-<sha1>.<ext>`, so the UI and MPRIS
see ordinary local files. A sync is one transaction: it upserts, prunes remote rows
the server no longer lists, and rolls back on any error, so a failed sync leaves the
index unchanged. Remote tracks never come from the folder scan. Other servers' remote
tracks stay in the index but are hidden. Limits: the password is plaintext in the
settings file (owner-only permissions); remote tracks are skipped by M3U export and
the count is reported; lyrics are not fetched from the server; each sync rewrites the
remote rows, so their waveforms are recomputed on the next play; there are no server
playlists, scrobbling, ratings, or CLI commands. Tests: `tst_navidrome` against an
in-process Subsonic server (token rule, path round-trip, stream URL, paging, cover
cache, wrong password, index prune, failed-sync rollback, forget, M3U skip). It was
also smoke-tested against Navidrome 0.64.2: sync, a 206 range stream, QMediaPlayer
playback, and waveform decode over HTTP.

Deliberately not in v1:
- Gapless playback (QtMultimedia limit; escape hatch above: QAudioSink + decoder pipeline).
- Editing tags, multiple library folders.
