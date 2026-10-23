// Two-pixel bevel edges for the Classic 95 and Luna XP looks. Lay it over a
// surface with anchors.fill. "raised" lights the top-left and shadows the
// bottom-right; "sunken" is the reverse. Invisible unless the theme's bevel is
// not "none", so callers can use it unconditionally.
import QtQuick

Item {
    id: bevel
    property bool raised: true
    readonly property bool active: Theme.bevel !== "none"
    readonly property color light: raised ? Theme.hi : Theme.sh
    readonly property color dark: raised ? Theme.sh : Theme.hi
    visible: active

    // Outer edge (1px) and inner edge (1px) on the top and left in light, bottom and right in dark.
    Rectangle { x: 0; y: 0; width: parent.width; height: 1; color: bevel.light }
    Rectangle { x: 0; y: 0; width: 1; height: parent.height; color: bevel.light }
    Rectangle { x: 1; y: 1; width: parent.width - 2; height: 1; color: bevel.light; opacity: 0.6 }
    Rectangle { x: 1; y: 1; width: 1; height: parent.height - 2; color: bevel.light; opacity: 0.6 }
    Rectangle { x: 0; y: parent.height - 1; width: parent.width; height: 1; color: bevel.dark }
    Rectangle { x: parent.width - 1; y: 0; width: 1; height: parent.height; color: bevel.dark }
    Rectangle { x: 1; y: parent.height - 2; width: parent.width - 2; height: 1; color: bevel.dark; opacity: 0.6 }
    Rectangle { x: parent.width - 2; y: 1; width: 1; height: parent.height - 2; color: bevel.dark; opacity: 0.6 }
}
