// Colour themes. A preset is eight base colours plus an accent policy; every
// other tone in the UI is derived from them (mixes of panel with ink), so light
// and dark presets both work without per-theme tuning. The choice is kept in
// Flow.conf under [appearance].
pragma Singleton
import QtQuick
import QtCore

Item {
    id: theme

    Settings {
        id: prefs
        category: "appearance"
        property string themeId: "graphite"
    }

    readonly property var presets: [
        { id: "graphite", name: "Graphite", note: "Near-black with mint. The accent follows the cover.",
          bg: "#0b0b0e", panel: "#121216", panel2: "#0e0e12", line: "#222228", line2: "#2e2e37",
          ink: "#f4f1ea", mut: "#9b978c", dim: "#5e5c55", soft: "#cfccc2", danger: "#ff8a7a",
          accent: "#d9ff3d", coverAccent: true },
        { id: "paper", name: "Paper", note: "Warm cream, ink and rust. Light, for daylight.",
          bg: "#efe9dc", panel: "#f8f4ea", panel2: "#e6dfcf", line: "#d6cdb8", line2: "#bfb49b",
          ink: "#201d17", mut: "#645d4f", dim: "#8e8676", soft: "#3b372e", danger: "#b8392a",
          accent: "#b8470f", coverAccent: false },
        { id: "glacier", name: "Glacier", note: "Cool blue-grey with a cobalt accent. Light.",
          bg: "#e4ebf1", panel: "#f4f7fa", panel2: "#d9e2ea", line: "#c6d2dd", line2: "#aebdcb",
          ink: "#13202c", mut: "#4d6175", dim: "#7b8fa2", soft: "#2a3b4c", danger: "#c93c34",
          accent: "#0b6fc2", coverAccent: false },
        { id: "nocturne", name: "Nocturne", note: "Deep navy with lamp-amber. The accent follows the cover.",
          bg: "#070a14", panel: "#0e1424", panel2: "#0a0f1c", line: "#1b2440", line2: "#29355a",
          ink: "#e8ecf8", mut: "#8e98b8", dim: "#56607f", soft: "#c3cbe2", danger: "#ff7b8c",
          accent: "#ffb454", coverAccent: true },
        { id: "synthwave", name: "Synthwave", note: "Violet night and a hot-pink signal.",
          bg: "#0c0615", panel: "#171029", panel2: "#110a20", line: "#2c1a48", line2: "#41286a",
          ink: "#fdeaff", mut: "#b59ad6", dim: "#7d64a0", soft: "#e4d2f2", danger: "#ff5f8f",
          accent: "#ff3ea5", coverAccent: false },
        { id: "phosphor", name: "Phosphor", note: "Amber on black, like a terminal. Fixed accent.",
          bg: "#060503", panel: "#0d0b06", panel2: "#0a0804", line: "#241d0c", line2: "#3d3114",
          ink: "#ffd27a", mut: "#b8924a", dim: "#7a5f2c", soft: "#eabb6a", danger: "#ff6a3d",
          accent: "#ffb000", coverAccent: false },
        { id: "matcha", name: "Matcha", note: "Moss-dark with peach. The accent follows the cover.",
          bg: "#0a100c", panel: "#121b14", panel2: "#0e160f", line: "#1e2b21", line2: "#2e4031",
          ink: "#eef3e4", mut: "#9aaa90", dim: "#62735c", soft: "#cfdac4", danger: "#ffa08c",
          accent: "#ffb48a", coverAccent: true },
        { id: "xp", name: "Luna XP", note: "Beige chrome and Start-menu blue. Light.",
          bg: "#ece9d8", panel: "#f7f6f1", panel2: "#e3e0d0", line: "#c4bfa8", line2: "#9e9a86",
          ink: "#1a1a1a", mut: "#4f4c44", dim: "#7d7a6e", soft: "#2b2a26", danger: "#c00000",
          accent: "#245edb", coverAccent: false },
        { id: "win95", name: "Classic 95", note: "Grey bevels and navy selection, the way it was in '95.",
          bg: "#c0c0c0", panel: "#d4d4d4", panel2: "#b4b4b4", line: "#808080", line2: "#404040",
          ink: "#000000", mut: "#2e2e2e", dim: "#5a5a5a", soft: "#111111", danger: "#b00000",
          accent: "#000080", coverAccent: false },
        { id: "aero", name: "Aero Glass", note: "Deep glass blue with a bright Aero glow.",
          bg: "#0f1d2b", panel: "#18293c", panel2: "#132234", line: "#2a4560", line2: "#3b5f80",
          ink: "#eef6ff", mut: "#98b3cc", dim: "#5f7d96", soft: "#d2e3f3", danger: "#ff8577",
          accent: "#3fb6ff", coverAccent: false },
        { id: "fluent", name: "Fluent Mica", note: "Charcoal mica with the Windows 11 accent blue.",
          bg: "#202020", panel: "#2c2c2c", panel2: "#1f1f1f", line: "#3b3b3b", line2: "#4d4d4d",
          ink: "#ffffff", mut: "#c7c7c7", dim: "#8c8c8c", soft: "#e6e6e6", danger: "#ff99a4",
          accent: "#60cdff", coverAccent: false },
        { id: "bsod", name: "Blue Screen", note: "A fatal error you'll want to keep. Yellow on blue.",
          bg: "#0000aa", panel: "#0c0cb4", panel2: "#000090", line: "#5a5ae0", line2: "#8080ff",
          ink: "#ffffff", mut: "#c4c4ff", dim: "#8f8fe0", soft: "#e8e8ff", danger: "#ff7070",
          accent: "#ffff55", coverAccent: false }
    ]

    readonly property var preset: presets.find(p => p.id === prefs.themeId) || presets[0]
    readonly property string key: preset.id
    readonly property string name: preset.name
    readonly property string note: preset.note

    readonly property color bg: preset.bg
    readonly property color panel: preset.panel
    readonly property color panel2: preset.panel2
    readonly property color line: preset.line
    readonly property color line2: preset.line2
    readonly property color ink: preset.ink
    readonly property color mut: preset.mut
    readonly property color dim: preset.dim
    readonly property color soft: preset.soft
    readonly property color danger: preset.danger
    readonly property color accent: preset.accent
    readonly property bool coverAccent: preset.coverAccent

    // Derived tones, from panel towards ink. Hover, selection, tracks and idle marks.
    readonly property color hovSoft: mix(panel, ink, 0.025)
    readonly property color hov: mix(panel, ink, 0.05)
    readonly property color chip: mix(panel, ink, 0.06)
    readonly property color faint: mix(panel, ink, 0.07)
    readonly property color sel: mix(panel, ink, 0.08)
    readonly property color track: mix(panel, ink, 0.10)
    readonly property color hovStrong: mix(panel, ink, 0.12)
    readonly property color idle: mix(panel, ink, 0.22)
    // Sunken wells (art frames, the spectrum box): a step darker than the background.
    // Dark themes go toward black; light themes only dip toward ink, or the well reads grey.
    readonly property color sunk: readableOn(bg) === "#ffffff" ? mix(bg, "#000000", 0.35) : mix(bg, ink, 0.06)
    // Text on an ink-coloured surface (chips, the play button).
    readonly property color inkText: readableOn(ink)

    function select(key) { prefs.themeId = key; }

    // Plain RGB blend: plenty for tints a few percent deep.
    function mix(a, b, t) {
        const x = Qt.color(a), y = Qt.color(b);
        return Qt.rgba(x.r + (y.r - x.r) * t, x.g + (y.g - x.g) * t, x.b + (y.b - x.b) * t, 1);
    }

    // Black or white, whichever has the better WCAG contrast against c.
    function readableOn(c) {
        const x = Qt.color(c);
        const lin = v => v <= 0.03928 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4);
        const l = 0.2126 * lin(x.r) + 0.7152 * lin(x.g) + 0.0722 * lin(x.b);
        return 1.05 / (l + 0.05) > (l + 0.05) / 0.05 ? "#ffffff" : "#0a0a0c";
    }
}
