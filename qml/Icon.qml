// Vector icons drawn with QtQuick.Shapes on a 24×24 grid. Icons used to be
// Unicode glyphs, which came from whichever fallback font had them (Adwaita,
// Liberation, Noto Color Emoji…) and rendered with mismatched sizes,
// baselines and even emoji colours. Paths are ours; stroked icons use a
// 2-unit round stroke, filled ones (fill: true) are solid.
import QtQuick
import QtQuick.Shapes

Item {
    id: icon
    property string name
    property color color: "#f4f1ea"
    property real size: 18
    implicitWidth: size
    implicitHeight: size

    readonly property var paths: ({
        "search":   { d: "M11 4a7 7 0 1 0 0 14a7 7 0 1 0 0-14z M20 20l-4.2-4.2" },
        "menu":     { d: "M4 7h16 M4 12h16 M4 17h16" },
        "close":    { d: "M6 6l12 12 M18 6L6 18" },
        "back":     { d: "M14.5 5.5L8 12l6.5 6.5" },
        "plus":     { d: "M12 5v14 M5 12h14" },
        "grid":     { d: "M4 4h6.5v6.5H4z M13.5 4H20v6.5h-6.5z M4 13.5h6.5V20H4z M13.5 13.5H20V20h-6.5z" },
        "disc":     { d: "M12 3a9 9 0 1 0 0 18a9 9 0 1 0 0-18z M12 9.5a2.5 2.5 0 1 0 0 5a2.5 2.5 0 1 0 0-5z" },
        "note":     { d: "M9 18V6l11-2v12 M9 18a3 3 0 1 1-6 0a3 3 0 1 1 6 0z M20 16a3 3 0 1 1-6 0a3 3 0 1 1 6 0z" },
        "heart":    { d: "M12 20s-7-4.35-9.2-8.6C1.3 8.4 3 5 6.5 5c2.1 0 3.6 1.2 5.5 3.2C13.9 6.2 15.4 5 17.5 5C21 5 22.7 8.4 21.2 11.4C19 15.65 12 20 12 20z" },
        "heart-fill": { d: "M12 20s-7-4.35-9.2-8.6C1.3 8.4 3 5 6.5 5c2.1 0 3.6 1.2 5.5 3.2C13.9 6.2 15.4 5 17.5 5C21 5 22.7 8.4 21.2 11.4C19 15.65 12 20 12 20z", fill: true },
        "play":     { d: "M8 5.2v13.6c0 .8.9 1.3 1.6.8l10.2-6.8c.6-.4.6-1.2 0-1.6L9.6 4.4C8.9 3.9 8 4.4 8 5.2z", fill: true },
        "pause":    { d: "M6.5 5h4v14h-4z M13.5 5h4v14h-4z", fill: true },
        "prev":     { d: "M5 5h2.4v14H5z M19 5.9v12.2c0 .8-.9 1.3-1.6.8l-8.6-6.1c-.6-.4-.6-1.2 0-1.6l8.6-6.1c.7-.5 1.6 0 1.6.8z", fill: true },
        "next":     { d: "M16.6 5H19v14h-2.4z M5 5.9v12.2c0 .8.9 1.3 1.6.8l8.6-6.1c.6-.4.6-1.2 0-1.6L6.6 5.1C5.9 4.6 5 5.1 5 5.9z", fill: true },
        "shuffle":  { d: "M3 7h3.5c4.5 0 5.5 10 10 10H21 M18 14l3 3-3 3 M3 17h3.5c1.4 0 2.4-1 3.3-2.4 M13.2 9.4C14.1 8 15.1 7 16.5 7H21 M18 4l3 3-3 3" },
        "repeat":   { d: "M17 3l3 3-3 3 M4 11V9.5A3.5 3.5 0 0 1 7.5 6H20 M7 21l-3-3 3-3 M20 13v1.5a3.5 3.5 0 0 1-3.5 3.5H4" },
        "queue":    { d: "M4 6h12 M4 11h12 M4 16h7 M15 14.5v6l5-3z" },
        "lyrics":   { d: "M5 5h14v10H10l-4 4v-4H5z M8.5 9h7 M8.5 12h4" },
        "drag":     { d: "M7.4 6a1.6 1.6 0 1 0 3.2 0a1.6 1.6 0 1 0-3.2 0z M13.4 6a1.6 1.6 0 1 0 3.2 0a1.6 1.6 0 1 0-3.2 0z M7.4 12a1.6 1.6 0 1 0 3.2 0a1.6 1.6 0 1 0-3.2 0z M13.4 12a1.6 1.6 0 1 0 3.2 0a1.6 1.6 0 1 0-3.2 0z M7.4 18a1.6 1.6 0 1 0 3.2 0a1.6 1.6 0 1 0-3.2 0z M13.4 18a1.6 1.6 0 1 0 3.2 0a1.6 1.6 0 1 0-3.2 0z", fill: true },
        "volume":   { d: "M4 9.5h3.5L12 5.5v13l-4.5-4H4z M15.5 9a4 4 0 0 1 0 6 M18 6.5a7.5 7.5 0 0 1 0 11" },
        "more":     { d: "M4.4 12a1.6 1.6 0 1 0 3.2 0a1.6 1.6 0 1 0-3.2 0z M10.4 12a1.6 1.6 0 1 0 3.2 0a1.6 1.6 0 1 0-3.2 0z M16.4 12a1.6 1.6 0 1 0 3.2 0a1.6 1.6 0 1 0-3.2 0z", fill: true },
        "folder":   { d: "M3.5 6.5A1.5 1.5 0 0 1 5 5h4.5l2 2.5H19a1.5 1.5 0 0 1 1.5 1.5v8.5A1.5 1.5 0 0 1 19 19H5a1.5 1.5 0 0 1-1.5-1.5z" }
    })
    readonly property var def: paths[name] || paths["close"]

    Shape {
        width: 24; height: 24
        scale: icon.size / 24
        transformOrigin: Item.TopLeft
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            strokeColor: icon.def.fill ? "transparent" : icon.color
            fillColor: icon.def.fill ? icon.color : "transparent"
            strokeWidth: icon.def.fill ? 0 : 2
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            PathSvg { path: icon.def.d }
        }
    }
}
