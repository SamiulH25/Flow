// Flow — app shell. Library (TagLib → SQLite index) and Player (QMediaPlayer)
// are C++ singletons from src/; Mpris (src/mpris.*) mirrors Player on D-Bus.
// Icons are vector (Icon.qml); fonts are bundled (see main.cpp).
import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtQuick.Dialogs
import QtCore
import Flow

ApplicationWindow {
    id: root
    width: 1280
    height: 800
    minimumWidth: 560
    minimumHeight: 480
    visible: true
    title: Player.hasTrack ? cur.t + " — Flow" : "Flow"
    color: bg

    // ---- theme ----
    readonly property color bg: "#0b0b0e"
    readonly property color panel: "#121216"
    readonly property color panel2: "#0e0e12"
    readonly property color line: "#222228"
    readonly property color line2: "#2e2e37"
    readonly property color ink: "#f4f1ea"
    readonly property color mut: "#9b978c"
    readonly property color dim: "#5e5c55"
    readonly property color acc: "#d9ff3d"
    readonly property color soft: "#cfccc2"
    readonly property color danger: "#ff8a7a"
    readonly property string disp: "Space Grotesk"    // bundled, see main.cpp
    readonly property string mono: "JetBrains Mono"   // bundled, see main.cpp

    // ---- responsive tiers (mirror ui-demo.html stress test) ----
    readonly property bool mid: root.width < 1080      // drawer becomes a slide-over
    readonly property bool narrow: root.width < 820    // rail becomes an overlay
    onNarrowChanged: if (!narrow) railOpen = false
    onMidChanged: if (!mid) drawerOpen = false

    // ---- view state (playback + library state live in C++) ----
    readonly property var cur: Player.current
    readonly property real progress: Player.duration > 0 ? Math.min(1, Player.position / Player.duration) : 0
    property string drawerTab: "now"     // now | queue | lyrics
    property string page: "tracks"       // tracks | albums | downloads (album/mix detail = tracks + Library view)
    property bool tracksOnly: false      // "Tracks" hides the album strip that "Overview" shows
    property bool albumsFromGrid: false  // where BACK from an album returns to
    property bool railOpen: false        // narrow windows: rail shown as an overlay
    property bool drawerOpen: false      // medium windows: drawer shown as a slide-over

    function fmtTime(s) {
        s = Math.max(0, Math.floor(s));
        return Math.floor(s / 60) + ":" + String(s % 60).padStart(2, "0");
    }
    function goTo(k) {
        railOpen = false;
        if (k === "albums" || k === "downloads") { page = k; return; }
        page = "tracks";
        tracksOnly = k === "tracks";
        Library.showAll();
        Library.format = k === "liked" ? "Liked" : "All";
    }
    function nextMixName() {
        return "Mix " + (mixRepeater.count + 1);
    }
    function openDrawer(tab) {
        drawerTab = tab;
        drawerOpen = true;
    }
    readonly property string pageTitle: page === "albums" ? "Albums" : page === "downloads" ? "Downloads" : Library.viewTitle
    function escapeHtml(s) {
        return String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;");
    }

    Shortcut { sequence: "Space"; onActivated: Player.toggle() }
    Shortcut { sequence: "Right"; onActivated: Player.next() }
    Shortcut { sequence: "Left"; onActivated: Player.previous() }
    Shortcut { sequence: "Escape"; onActivated: { drawerOpen = false; railOpen = false; searchField.focus = false; } }
    Shortcut { sequence: "Ctrl+K"; onActivated: searchField.forceActiveFocus() }

    // ================= shared components =================

    // Cover art with the placeholder gradient underneath (shown until/unless art loads).
    component Art: Item {
        id: art
        property string source: ""
        property color c1: "#26262e"
        property color c2: "#0f0f13"
        property real radius: 10
        property bool accent: true
        property real accentX: 30
        property real accentY: -20
        property real accentSize: width * 0.5
        property int decodeSize: 256
        readonly property bool loaded: img.status === Image.Ready
        // MultiEffect needs a GPU scene graph; the software backend gets square corners.
        readonly property bool masked: GraphicsInfo.api !== GraphicsInfo.Software

        Rectangle {
            anchors.fill: parent; radius: art.radius; clip: true
            gradient: Gradient {
                GradientStop { position: 0; color: art.c1 }
                GradientStop { position: 1; color: art.c2 }
            }
            Rectangle { // abstract art accent
                visible: art.accent && !art.loaded
                width: art.accentSize; height: width; radius: width / 2
                color: "#e8e4d8"; opacity: 0.85
                x: art.accentX; y: art.accentY
            }
        }
        Image {
            id: img
            anchors.fill: parent
            source: art.source
            sourceSize: Qt.size(art.decodeSize, art.decodeSize)
            fillMode: Image.PreserveAspectCrop
            asynchronous: true; cache: true
            visible: art.loaded && !art.masked
        }
        MultiEffect {
            anchors.fill: parent
            source: img
            visible: art.loaded && art.masked
            maskEnabled: true
            maskSource: artMask
            maskThresholdMin: 0.5
            maskSpreadAtMin: 1.0
        }
        Item {
            id: artMask
            anchors.fill: parent
            layer.enabled: true
            visible: false
            Rectangle { anchors.fill: parent; radius: art.radius; color: "black" }
        }
    }

    // "Now playing" marker: three bars that dance while playing, rest when paused.
    component EqBars: Item {
        id: eq
        property color color: acc
        property bool running: Player.playing
        implicitWidth: 14; implicitHeight: 12
        Repeater {
            model: 3
            Rectangle {
                id: bar
                required property int index
                x: index * 5; width: 3; radius: 1.5
                anchors.bottom: parent.bottom
                color: eq.color
                height: eq.height * [0.55, 1.0, 0.7][index]
                SequentialAnimation on height {
                    running: eq.running; loops: Animation.Infinite
                    NumberAnimation { to: eq.height * [0.3, 0.45, 0.25][bar.index]; duration: [380, 300, 430][bar.index]; easing.type: Easing.InOutSine }
                    NumberAnimation { to: eq.height * [1.0, 0.8, 0.95][bar.index]; duration: [340, 410, 290][bar.index]; easing.type: Easing.InOutSine }
                }
            }
        }
    }

    // Round icon button (transport, header, row actions).
    component IconButton: Rectangle {
        id: ib
        property string icon
        property real iconSize: 18
        property color iconColor: hover.containsMouse ? ink : "#d8d5cb"
        property bool lit: false
        property string tip: ""
        signal clicked
        implicitWidth: 36; implicitHeight: 36
        radius: width / 2
        color: lit ? "#151a0e" : hover.containsMouse ? "#1e1e25" : "transparent"
        border.color: lit ? "#3d4420" : "transparent"; border.width: 1
        Icon { anchors.centerIn: parent; name: ib.icon; size: ib.iconSize; color: ib.lit ? acc : ib.iconColor }
        MouseArea { id: hover; anchors.fill: parent; hoverEnabled: true; onClicked: ib.clicked() }
        ToolTip.visible: tip !== "" && hover.containsMouse
        ToolTip.delay: 600
        ToolTip.text: tip
    }

    // Small outlined text pill (row quick actions, BACK, EXPORT).
    component Pill: Rectangle {
        id: pill
        property string text
        property string icon: ""
        property color textColor: ink
        property bool solid: false
        signal clicked
        implicitHeight: 26
        implicitWidth: pillRow.implicitWidth + 20
        radius: 8
        color: solid ? (pillHover.containsMouse ? ink : acc) : pillHover.containsMouse ? "#2a2a33" : "#1c1c22"
        border.color: solid ? "transparent" : line2; border.width: 1
        Row {
            id: pillRow
            anchors.centerIn: parent
            spacing: 5
            Icon { visible: pill.icon !== ""; anchors.verticalCenter: parent.verticalCenter; name: pill.icon; size: 12; color: pill.solid ? "#000" : pill.textColor }
            Text { anchors.verticalCenter: parent.verticalCenter; text: pill.text; color: pill.solid ? "#000" : pill.textColor; font.pixelSize: 10; font.family: mono; font.bold: pill.solid }
        }
        MouseArea { id: pillHover; anchors.fill: parent; hoverEnabled: true; onClicked: pill.clicked() }
    }

    // Album tile: click opens the album, the round button plays it.
    component AlbumCard: Item {
        id: card
        property bool fromGrid: false
        height: width + 58
        Art {
            width: card.width; height: card.width
            source: cover; c1: model.c1; c2: model.c2
            accentSize: width * 0.5; accentX: (albumIndex % 3) * width * 0.22 - 8; accentY: (albumIndex % 2) * width * 0.26 - 10
            decodeSize: 300
            Row {
                x: 8; y: 8; spacing: 5
                Rectangle { height: 18; width: fmtLabel.implicitWidth + 14; radius: 9; color: "#000000a0"
                    Text { id: fmtLabel; anchors.centerIn: parent; text: f; color: "#fff"; font.pixelSize: 9; font.family: mono } }
                Rectangle { height: 18; width: hiLabel.implicitWidth + 14; radius: 9; color: acc; visible: hi
                    Text { id: hiLabel; anchors.centerIn: parent; text: "HI-RES"; color: "#101204"; font.pixelSize: 9; font.family: mono; font.bold: true } }
            }
        }
        Text { y: card.width + 7; width: card.width; text: al; color: ink; font.pixelSize: 13; font.family: disp; font.weight: Font.Medium; elide: Text.ElideRight }
        Text { y: card.width + 26; width: card.width; text: khz !== "" ? a + " · " + khz : a; color: mut; font.pixelSize: 12; elide: Text.ElideRight }
        MouseArea {
            id: cardHover
            anchors.fill: parent; hoverEnabled: true
            onClicked: { albumsFromGrid = card.fromGrid; page = "tracks"; Library.showAlbum(albumIndex); }
        }
        Rectangle {
            x: card.width - 44; y: card.width - 44; width: 36; height: 36; radius: 18
            visible: cardHover.containsMouse || playHover.containsMouse
            color: playHover.containsMouse ? ink : acc
            Icon { anchors.centerIn: parent; anchors.horizontalCenterOffset: 1; name: "play"; size: 16; color: "#000" }
            MouseArea { id: playHover; anchors.fill: parent; hoverEnabled: true; onClicked: Player.playTracks(Library.albumTrackIds(albumIndex), 0) }
        }
    }

    component FlowMenuItem: MenuItem {
        id: fmi
        implicitHeight: visible ? 32 : 0
        contentItem: Text {
            leftPadding: 6; text: fmi.text; color: fmi.enabled ? ink : dim
            font.pixelSize: 13; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight
        }
        background: Rectangle { color: fmi.highlighted ? "#1e1e25" : "transparent"; radius: 7 }
    }

    // "+ MIX" on a track row
    Menu {
        id: mixMenu
        property var trackId: -1
        padding: 6
        background: Rectangle { implicitWidth: 220; color: panel; border.color: line2; border.width: 1; radius: 10 }
        FlowMenuItem { text: "ADD TO MIX"; enabled: false }
        Instantiator {
            model: Library.mixes
            delegate: FlowMenuItem {
                required property var mixId
                required property string name
                text: name
                onTriggered: Library.addToMix(mixId, mixMenu.trackId)
            }
            onObjectAdded: (index, object) => mixMenu.insertItem(index + 1, object)
            onObjectRemoved: (index, object) => mixMenu.removeItem(object)
        }
        FlowMenuItem {
            text: "+  New mix"
            onTriggered: {
                const id = Library.createMix(nextMixName());
                if (id >= 0) Library.addToMix(id, mixMenu.trackId);
            }
        }
    }

    // Row menu: right-click on any track, or the ⋯ button on narrow rows.
    Menu {
        id: trackMenu
        property var trackId: -1
        property int slot: -1
        padding: 6
        background: Rectangle { implicitWidth: 200; color: panel; border.color: line2; border.width: 1; radius: 10 }
        FlowMenuItem { text: "Play next"; onTriggered: Player.playNext(trackMenu.trackId) }
        FlowMenuItem { text: "Add to queue"; onTriggered: Player.enqueue(trackMenu.trackId) }
        FlowMenuItem {
            text: "Add to mix…"
            onTriggered: { mixMenu.trackId = trackMenu.trackId; Qt.callLater(() => mixMenu.popup()); }
        }
        FlowMenuItem {
            text: "Remove from mix"; visible: Library.viewKind === "mix"
            onTriggered: Library.removeFromMix(Library.viewMixId, trackMenu.slot)
        }
    }

    // ---- folder picker, M3U import / export + a short status toast ----
    FolderDialog {
        id: folderDialog
        title: "Choose your music folder"
        currentFolder: "file://" + Library.musicDir
        onAccepted: {
            Library.musicDir = selectedFolder;   // accepts the file:// URL
            goTo("overview");
            showToast("Scanning " + Library.musicDir + "…");
        }
    }
    FileDialog {
        id: importDialog
        title: "Import playlist"
        fileMode: FileDialog.OpenFile
        nameFilters: ["Playlists (*.m3u *.m3u8)", "All files (*)"]
        currentFolder: StandardPaths.writableLocation(StandardPaths.MusicLocation)
        onAccepted: {
            const r = Library.importMix(selectedFile);
            if (!r.ok) { showToast("Import failed: " + r.error); return; }
            page = "tracks"; railOpen = false;
            Library.showMix(r.mixId);
            showToast("Imported " + r.added + (r.added === 1 ? " track" : " tracks")
                      + (r.skipped > 0 ? " · " + r.skipped + " not in your library" : ""));
        }
    }
    FileDialog {
        id: exportDialog
        property var mixId: -1
        title: "Export mix"
        fileMode: FileDialog.SaveFile
        defaultSuffix: "m3u8"
        nameFilters: ["M3U8 playlist (*.m3u8)"]
        onAccepted: {
            const r = Library.exportMix(mixId, selectedFile);
            showToast(r.ok ? "Exported " + r.added + (r.added === 1 ? " track" : " tracks")
                           : "Export failed: " + r.error);
        }
    }
    Connections {
        target: Downloader
        function onNotice(text) { showToast(text); }
    }
    property bool toastShown: false
    function showToast(text) { toast.text = text; toastShown = true; toastTimer.restart(); }
    Timer { id: toastTimer; interval: 3500; onTriggered: toastShown = false }

    // ================= header =================
    Rectangle {
        id: chrome
        anchors { top: parent.top; left: parent.left; right: parent.right }
        height: 52; color: bg
        Rectangle { anchors { left: parent.left; right: parent.right; bottom: parent.bottom } height: 1; color: line }

        // left: rail toggle (narrow) or logo + breadcrumb
        Item {
            id: brand
            anchors { left: parent.left; verticalCenter: parent.verticalCenter; leftMargin: 14 }
            height: 34
            width: narrow ? 34 : brandRow.implicitWidth
            IconButton {
                visible: narrow
                width: 34; height: 34; radius: 9
                icon: "menu"; lit: railOpen; tip: "Library"
                onClicked: railOpen = !railOpen
            }
            Row {
                id: brandRow
                visible: !narrow
                anchors.verticalCenter: parent.verticalCenter
                spacing: 9
                Image {
                    anchors.verticalCenter: parent.verticalCenter
                    source: "qrc:/data/flow.svg"; sourceSize: Qt.size(44, 44); width: 22; height: 22
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Flow"; color: ink; font.pixelSize: 14; font.family: disp; font.weight: Font.Bold
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    // Keeps the crumb short so it never reaches the search box.
                    width: Math.min(implicitWidth, Math.max(0, searchBox.x - 24 - (brand.x + 22 + 9 + 40 + 9)))
                    text: "/  " + pageTitle
                    color: mut; font.pixelSize: 13; elide: Text.ElideRight
                }
            }
        }
        Rectangle {
            id: searchBox
            anchors.centerIn: parent
            // As wide as the space between the brand and the avatar allows.
            readonly property real room: parent.width - 2 * Math.max(brand.x + (narrow ? 34 : 120), 62) - 24
            width: Math.max(160, Math.min(460, room)); height: 34; radius: 17
            color: panel; border.color: searchField.activeFocus ? line2 : line; border.width: 1
            Icon { x: 12; anchors.verticalCenter: parent.verticalCenter; name: "search"; size: 15; color: dim }
            TextField {
                id: searchField
                anchors { fill: parent; leftMargin: 34; rightMargin: shortcutHint.visible ? 60 : 12 }
                verticalAlignment: TextInput.AlignVCenter
                placeholderText: "Search your collection…"
                placeholderTextColor: dim
                color: ink; font.pixelSize: 13
                background: Item {}
                onTextEdited: Library.query = text
            }
            Rectangle {
                id: shortcutHint
                anchors { right: parent.right; verticalCenter: parent.verticalCenter; rightMargin: 10 }
                width: hintLabel.implicitWidth + 12; height: 18; radius: 6; color: "transparent"; border.color: line2; border.width: 1
                visible: parent.width >= 300 && !searchField.activeFocus
                Text { id: hintLabel; anchors.centerIn: parent; text: "Ctrl K"; color: dim; font.pixelSize: 9; font.family: mono }
            }
        }
        Rectangle {
            anchors { right: parent.right; verticalCenter: parent.verticalCenter; rightMargin: 16 }
            width: 30; height: 30; radius: 15
            gradient: Gradient {
                GradientStop { position: 0; color: "#e8e4d8" }
                GradientStop { position: 1; color: "#8a877e" }
            }
            Text { anchors.centerIn: parent; text: "B"; font.pixelSize: 12; font.bold: true; color: "#111" }
        }
    }

    // ================= body =================
    Item {
        id: body
        anchors { top: chrome.bottom; left: parent.left; right: parent.right; bottom: dock.top }
        clip: true

        // scrim for the slide-over drawer (medium windows)
        Rectangle {
            anchors.fill: parent; color: "black"; opacity: 0.45; z: 9
            visible: mid && drawerOpen
            MouseArea { anchors.fill: parent; onClicked: drawerOpen = false }
        }
        // scrim for the overlay rail (narrow windows)
        Rectangle {
            anchors.fill: parent; color: "black"; opacity: 0.45; z: 11
            visible: narrow && railOpen
            MouseArea { anchors.fill: parent; onClicked: railOpen = false }
        }

        // ---- rail ----
        Rectangle {
            id: rail
            anchors { top: parent.top; left: parent.left; bottom: parent.bottom }
            width: 224; color: bg; visible: !narrow || railOpen
            z: narrow ? 12 : 0
            Rectangle { anchors { top: parent.top; right: parent.right; bottom: parent.bottom } width: 1; color: line }
            Flickable {
                anchors { fill: parent; margins: 12 }
                contentHeight: railCol.height; clip: true
                boundsBehavior: Flickable.StopAtBounds
                Column {
                    id: railCol
                    width: parent.width
                    spacing: 2
                    Text { text: "LIBRARY"; color: dim; font.pixelSize: 10; font.family: mono; leftPadding: 10; topPadding: 6; bottomPadding: 6 }
                    Repeater {
                        model: [
                            {t: "Overview", i: "grid", k: "overview",
                             on: page === "tracks" && !tracksOnly && Library.viewKind === "all" && Library.format !== "Liked"},
                            {t: "Albums", i: "disc", k: "albums", on: page === "albums" || Library.viewKind === "album"},
                            {t: "Tracks", i: "note", k: "tracks", n: Library.totalCount.toLocaleString(Qt.locale(), "f", 0),
                             on: page === "tracks" && tracksOnly && Library.viewKind === "all" && Library.format !== "Liked"},
                            {t: "Liked", i: "heart", k: "liked", n: String(Library.likedCount),
                             on: page === "tracks" && Library.viewKind === "all" && Library.format === "Liked"},
                            {t: "Downloads", i: "download", k: "downloads", n: Downloader.active > 0 ? String(Downloader.active) : "",
                             on: page === "downloads", busy: Downloader.busy}
                        ]
                        Rectangle {
                            width: parent.width; height: 34; radius: 9
                            color: modelData.on ? "#1e1e25" : railHover.containsMouse ? "#1a1a20" : "transparent"
                            Icon { x: 10; anchors.verticalCenter: parent.verticalCenter; name: modelData.i; size: 16; color: modelData.on ? acc : mut }
                            Text { x: 36; anchors.verticalCenter: parent.verticalCenter; text: modelData.t; color: modelData.on ? "#fff" : soft; font.pixelSize: 13 }
                            Text {
                                anchors { right: parent.right; rightMargin: 10; verticalCenter: parent.verticalCenter }
                                text: modelData.n || ""; color: modelData.busy ? acc : dim; font.pixelSize: 10; font.family: mono
                            }
                            MouseArea { id: railHover; anchors.fill: parent; hoverEnabled: true; onClicked: goTo(modelData.k) }
                        }
                    }
                    Item {
                        width: parent.width; height: 34
                        Text { anchors { left: parent.left; leftMargin: 10; bottom: parent.bottom; bottomMargin: 7 } text: "MIXES"; color: dim; font.pixelSize: 10; font.family: mono }
                        Text {
                            anchors { right: parent.right; rightMargin: 10; bottom: parent.bottom; bottomMargin: 7 }
                            text: "IMPORT"; font.pixelSize: 10; font.family: mono
                            color: importHover.containsMouse ? ink : dim
                            MouseArea { id: importHover; anchors.fill: parent; anchors.margins: -5; hoverEnabled: true; onClicked: importDialog.open() }
                        }
                    }
                    Repeater {
                        id: mixRepeater
                        model: Library.mixes
                        Rectangle {
                            id: mixRow
                            readonly property bool on: page === "tracks" && Library.viewKind === "mix" && Library.viewMixId === mixId
                            property bool renaming: false
                            property bool confirmDelete: false
                            width: parent.width; height: 32; radius: 9
                            color: on ? "#1e1e25" : mixHover.containsMouse ? "#1a1a20" : "transparent"
                            MouseArea {
                                id: mixHover
                                anchors.fill: parent; hoverEnabled: true
                                onClicked: { page = "tracks"; railOpen = false; Library.showMix(mixId); }
                                onDoubleClicked: mixRow.renaming = true
                                onExited: mixRow.confirmDelete = false
                            }
                            Rectangle { x: 14; anchors.verticalCenter: parent.verticalCenter; width: 8; height: 8; radius: 4; color: mixRow.on ? acc : "#3a3a42"; visible: !mixRow.renaming }
                            Text {
                                x: 36; anchors.verticalCenter: parent.verticalCenter; visible: !mixRow.renaming
                                width: parent.width - 36 - 44; text: name; color: soft; font.pixelSize: 13; elide: Text.ElideRight
                            }
                            // count, or delete (two clicks: the first arms, the second deletes)
                            Item {
                                anchors { right: parent.right; rightMargin: 6; verticalCenter: parent.verticalCenter }
                                width: 34; height: 24; visible: !mixRow.renaming
                                readonly property bool active: mixHover.containsMouse || delArea.containsMouse
                                Text {
                                    anchors { right: parent.right; rightMargin: 4; verticalCenter: parent.verticalCenter }
                                    visible: !parent.active && !mixRow.confirmDelete
                                    text: count; color: dim; font.pixelSize: 10; font.family: mono
                                }
                                Icon {
                                    anchors { right: parent.right; rightMargin: 2; verticalCenter: parent.verticalCenter }
                                    visible: parent.active && !mixRow.confirmDelete
                                    name: "close"; size: 14; color: delArea.containsMouse ? ink : mut
                                }
                                Text {
                                    anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                                    visible: mixRow.confirmDelete
                                    text: "DEL?"; color: danger; font.pixelSize: 10; font.family: mono; font.bold: true
                                }
                                MouseArea {
                                    id: delArea
                                    anchors.fill: parent; hoverEnabled: true
                                    onClicked: {
                                        if (mixRow.confirmDelete) Library.deleteMix(mixId);
                                        else mixRow.confirmDelete = true;
                                    }
                                    onExited: mixRow.confirmDelete = false
                                }
                            }
                            TextField {
                                anchors { fill: parent; leftMargin: 4; rightMargin: 4 }
                                visible: mixRow.renaming
                                text: name; color: ink; font.pixelSize: 13
                                background: Rectangle { radius: 7; color: panel2; border.color: line2 }
                                onVisibleChanged: if (visible) { forceActiveFocus(); selectAll(); }
                                onAccepted: { Library.renameMix(mixId, text); mixRow.renaming = false; }
                                onActiveFocusChanged: if (!activeFocus) mixRow.renaming = false
                                Keys.onEscapePressed: mixRow.renaming = false
                            }
                        }
                    }
                    Rectangle {
                        width: parent.width; height: 32; radius: 9
                        color: newMixHover.containsMouse ? "#1a1a20" : "transparent"
                        Icon { x: 10; anchors.verticalCenter: parent.verticalCenter; name: "plus"; size: 16; color: newMixHover.containsMouse ? ink : dim }
                        Text { x: 36; anchors.verticalCenter: parent.verticalCenter; text: "New mix"; color: newMixHover.containsMouse ? ink : dim; font.pixelSize: 13 }
                        MouseArea {
                            id: newMixHover
                            anchors.fill: parent; hoverEnabled: true
                            onClicked: {
                                const id = Library.createMix(nextMixName());
                                if (id >= 0) { page = "tracks"; railOpen = false; Library.showMix(id); }
                            }
                        }
                    }
                    Text {
                        width: parent.width; leftPadding: 10; topPadding: 2; wrapMode: Text.WordWrap
                        visible: mixRepeater.count > 0
                        text: "Double-click a mix to rename it."; color: dim; font.pixelSize: 10
                    }
                    Item { width: 1; height: 12 }
                    Rectangle {
                        width: parent.width; height: 92; radius: 11; color: panel; border.color: cardHover.containsMouse ? line2 : line; border.width: 1
                        MouseArea { id: cardHover; anchors.fill: parent; hoverEnabled: true; onClicked: folderDialog.open() }
                        Column {
                            anchors { fill: parent; margins: 12 } spacing: 4
                            Item {
                                width: parent.width; height: libTitle.implicitHeight
                                Text { id: libTitle; text: "Local library"; color: ink; font.pixelSize: 12; font.bold: true }
                                Text { anchors.right: parent.right; text: "CHANGE"; color: cardHover.containsMouse ? ink : dim; font.pixelSize: 10; font.family: mono }
                            }
                            Text { width: parent.width; text: Library.musicDir; color: mut; font.pixelSize: 11; font.family: mono; elide: Text.ElideMiddle }
                            Text { width: parent.width; text: Library.storageText; color: mut; font.pixelSize: 11; elide: Text.ElideRight }
                            Rectangle {
                                width: parent.width; height: 4; radius: 2; color: "#26262e"
                                Rectangle { width: parent.width * Library.storageUsed; height: 4; radius: 2; color: acc }
                            }
                        }
                    }
                }
            }
        }

        // ---- center ----
        Item {
            id: center
            anchors {
                top: parent.top; bottom: parent.bottom
                left: parent.left; leftMargin: narrow ? 0 : 224
                right: parent.right; rightMargin: mid ? 0 : 322
            }
            // title row: back · title · counts
            Item {
                id: listhead
                anchors { top: parent.top; left: parent.left; right: parent.right; topMargin: 14; leftMargin: 24; rightMargin: 24 }
                height: 30
                readonly property bool detail: page === "tracks" && Library.viewKind !== "all"
                Pill {
                    id: backPill
                    visible: listhead.detail
                    anchors.verticalCenter: parent.verticalCenter
                    text: "BACK"; icon: "back"
                    onClicked: {
                        const fromAlbum = Library.viewKind === "album";
                        Library.showAll();
                        if (fromAlbum && albumsFromGrid) page = "albums";
                    }
                }
                Text {
                    anchors { left: listhead.detail ? backPill.right : parent.left; leftMargin: listhead.detail ? 12 : 0
                              right: parent.right; verticalCenter: parent.verticalCenter }
                    textFormat: Text.StyledText; elide: Text.ElideRight
                    text: "<font face='" + disp + "' size='5' color='" + ink + "'><b>"
                          + escapeHtml(pageTitle) + "</b></font>"
                          + "<font face='" + mono + "' size='2' color='" + dim + "'>&nbsp;&nbsp;&nbsp;"
                          + (page === "downloads"
                             ? (Downloader.active > 0
                                ? (Downloader.runningCount + " running"
                                   + (Downloader.active > Downloader.runningCount
                                      ? " · " + (Downloader.active - Downloader.runningCount) + " queued" : ""))
                                : "YouTube Music")
                             : page === "albums"
                             ? albumGrid.count + " albums"
                             : (Library.viewSubtitle !== "" ? escapeHtml(Library.viewSubtitle) + " · " : "")
                               + Library.shownCount.toLocaleString(Qt.locale(), "f", 0) + " shown"
                               + (Library.viewKind === "all" ? " · " + Library.totalCount.toLocaleString(Qt.locale(), "f", 0) + " tracks" : ""))
                          + (Library.scanning ? " · scanning " + Library.scanProgress + "…" : "") + "</font>"
                }
            }
            // filter / sort / actions; scrolls sideways when it doesn't fit
            Flickable {
                id: chips
                anchors { top: listhead.bottom; left: parent.left; right: parent.right; topMargin: 8; leftMargin: 24; rightMargin: 24 }
                height: 32
                visible: page === "tracks"
                contentWidth: chipRow.implicitWidth; contentHeight: height
                flickableDirection: Flickable.HorizontalFlick
                boundsBehavior: Flickable.StopAtBounds
                interactive: contentWidth > width
                clip: true
                Row {
                    id: chipRow
                    height: 32; spacing: 8
                    Repeater {
                        model: ["All", "FLAC", "MP3", "WAV", "Liked"]
                        Rectangle {
                            id: chip
                            anchors.verticalCenter: parent.verticalCenter
                            readonly property bool on: Library.format === modelData
                            height: 30; width: chipContent.implicitWidth + 26; radius: 15
                            color: on ? ink : chipHover.containsMouse ? "#1a1a20" : panel
                            border.color: on ? ink : line2; border.width: 1
                            Row {
                                id: chipContent
                                anchors.centerIn: parent; spacing: 5
                                Icon { visible: modelData === "Liked"; anchors.verticalCenter: parent.verticalCenter; name: "heart-fill"; size: 12; color: chip.on ? "#000" : soft }
                                Text { anchors.verticalCenter: parent.verticalCenter; text: modelData; color: chip.on ? "#000" : soft; font.pixelSize: 12; font.weight: chip.on ? Font.DemiBold : Font.Normal }
                            }
                            MouseArea { id: chipHover; anchors.fill: parent; hoverEnabled: true; onClicked: Library.format = modelData }
                        }
                    }
                    Item { width: 4; height: 1 }
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        height: 30; width: sortLabel.implicitWidth + 26; radius: 9
                        color: sortHover.containsMouse ? "#1a1a20" : panel; border.color: line2; border.width: 1
                        Text { id: sortLabel; anchors.centerIn: parent; color: ink; font.pixelSize: 11; font.family: mono
                            text: Library.sortMode === "title" ? "Title A–Z" : Library.sortMode === "artist" ? "Artist A–Z"
                                : Library.sortMode === "dur" ? "Longest first"
                                : Library.viewKind === "album" ? "Track order" : Library.viewKind === "mix" ? "Mix order" : "Recently added" }
                        MouseArea { id: sortHover; anchors.fill: parent; hoverEnabled: true; onClicked: {
                            const modes = ["def", "title", "artist", "dur"];
                            Library.sortMode = modes[(modes.indexOf(Library.sortMode) + 1) % modes.length];
                        } }
                    }
                    Pill {
                        anchors.verticalCenter: parent.verticalCenter
                        visible: Library.viewKind === "mix"; implicitHeight: 30
                        text: "EXPORT M3U"
                        onClicked: {
                            exportDialog.mixId = Library.viewMixId;
                            const folder = StandardPaths.writableLocation(StandardPaths.MusicLocation);
                            exportDialog.currentFolder = folder;
                            exportDialog.selectedFile = folder + "/" + encodeURIComponent(Library.viewTitle.replace(/\//g, "-")) + ".m3u8";
                            exportDialog.open();
                        }
                    }
                    Pill {
                        anchors.verticalCenter: parent.verticalCenter
                        visible: Library.shownCount > 0; implicitHeight: 30; radius: 15
                        solid: true; text: "PLAY"; icon: "play"
                        onClicked: Player.playTracks(Library.visibleTrackIds(), 0)
                    }
                }
            }
            // albums page
            GridView {
                id: albumGrid
                anchors { top: listhead.bottom; left: parent.left; right: parent.right; bottom: parent.bottom; topMargin: 16; leftMargin: 24; rightMargin: 12 }
                visible: page === "albums"
                clip: true; model: Library.albums
                readonly property int columns: Math.max(1, Math.floor(width / 170))
                cellWidth: width / columns; cellHeight: Math.min(150, cellWidth - 12) + 72
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                delegate: Item {
                    width: albumGrid.cellWidth; height: albumGrid.cellHeight
                    AlbumCard { width: Math.min(150, parent.width - 12); fromGrid: true }
                }
                Text {
                    anchors { top: parent.top; left: parent.left; right: parent.right; topMargin: 40 }
                    visible: albumGrid.count === 0; horizontalAlignment: Text.AlignHCenter
                    text: Library.scanning ? "Scanning your music…" : "No albums yet."; color: mut; font.pixelSize: 13
                }
            }
            // downloads page: YouTube / YouTube Music links → MP3s in the library (yt-dlp)
            Item {
                id: downloadsPage
                anchors { top: listhead.bottom; left: parent.left; right: parent.right; bottom: parent.bottom; topMargin: 14; leftMargin: 24; rightMargin: 24 }
                visible: page === "downloads"
                readonly property string link: linkField.text.trim()
                // watch?v=…&list=… is a song played from a list (often an endless
                // radio): the song unless PLAYLIST is picked. Playlist links are lists.
                readonly property bool hasList: /[?&]list=|\/browse\//.test(link)
                readonly property bool onlyList: Downloader.isPlaylistUrl(link)
                property bool wantList: false
                readonly property bool asList: onlyList || (hasList && wantList)
                readonly property bool ready: Downloader.available && Downloader.isSupportedUrl(link)
                function submit() {
                    if (link === "") return;
                    const error = Downloader.add(link, asList);
                    if (error !== "") { showToast(error); return; }
                    linkField.text = "";
                    wantList = false;
                }

                Rectangle {
                    id: linkBox
                    width: parent.width; height: 52; radius: 12
                    color: panel; border.color: linkField.activeFocus ? line2 : line; border.width: 1
                    enabled: Downloader.available; opacity: enabled ? 1 : 0.5
                    Icon { x: 15; anchors.verticalCenter: parent.verticalCenter; name: "link"; size: 16; color: linkField.activeFocus ? soft : dim }
                    TextField {
                        id: linkField
                        anchors { left: parent.left; leftMargin: 40; right: modeRow.visible ? modeRow.left : goPill.left; rightMargin: 10; verticalCenter: parent.verticalCenter }
                        placeholderText: linkBox.width >= 520 ? "Paste a YouTube Music link — song, playlist or album" : "Paste a YouTube Music link"
                        placeholderTextColor: dim
                        color: ink; font.pixelSize: 13; selectByMouse: true
                        background: Item {}
                        onAccepted: downloadsPage.submit()
                    }
                    // SONG / PLAYLIST, only when the link has a list in it
                    Row {
                        id: modeRow
                        anchors { right: goPill.left; rightMargin: 10; verticalCenter: parent.verticalCenter }
                        visible: downloadsPage.hasList
                        spacing: 4
                        Repeater {
                            model: [{t: "SONG", list: false}, {t: "PLAYLIST", list: true}]
                            Rectangle {
                                id: modeChip
                                readonly property bool on: downloadsPage.asList === modelData.list
                                anchors.verticalCenter: parent.verticalCenter
                                height: 26; width: modeLabel.implicitWidth + 18; radius: 13
                                enabled: modelData.list || !downloadsPage.onlyList
                                opacity: enabled ? 1 : 0.4
                                color: on ? ink : modeHover.containsMouse ? "#1e1e25" : "transparent"
                                border.color: on ? ink : line2; border.width: 1
                                Text { id: modeLabel; anchors.centerIn: parent; text: modelData.t; color: modeChip.on ? "#000" : soft; font.pixelSize: 10; font.family: mono; font.bold: modeChip.on }
                                MouseArea { id: modeHover; anchors.fill: parent; hoverEnabled: true; onClicked: downloadsPage.wantList = modelData.list }
                            }
                        }
                    }
                    Pill {
                        id: goPill
                        anchors { right: parent.right; rightMargin: 10; verticalCenter: parent.verticalCenter }
                        implicitHeight: 32; radius: 16
                        solid: true; icon: "download"; text: linkBox.width >= 440 ? "DOWNLOAD" : ""
                        opacity: downloadsPage.ready ? 1 : 0.45
                        onClicked: downloadsPage.submit()
                    }
                }
                Text {
                    id: downloadsHint
                    anchors { top: linkBox.bottom; left: parent.left; right: parent.right; topMargin: 10; leftMargin: 4 }
                    wrapMode: Text.WordWrap; font.pixelSize: 11
                    color: Downloader.available ? dim : danger
                    text: Downloader.available
                          ? "MP3 with tags and cover art, saved to " + Downloader.destination.replace(StandardPaths.writableLocation(StandardPaths.HomeLocation).toString().replace("file://", ""), "~")
                            + ". Playlists become mixes; albums show up in Albums."
                          : "Downloading needs " + Downloader.missing + ". Install it (sudo pacman -S yt-dlp ffmpeg) and restart Flow."
                }
                Item {
                    id: jobsHead
                    anchors { top: downloadsHint.bottom; left: parent.left; right: parent.right; topMargin: 22 }
                    height: 26
                    visible: jobList.count > 0
                    Text { anchors { left: parent.left; leftMargin: 4; verticalCenter: parent.verticalCenter } text: "QUEUE"; color: dim; font.pixelSize: 10; font.family: mono }
                    Row {
                        anchors { right: parent.right; rightMargin: 4; verticalCenter: parent.verticalCenter }
                        spacing: 12
                        Row {
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 6
                            Text { anchors.verticalCenter: parent.verticalCenter; text: Downloader.maxConcurrent + " PARALLEL"; color: dim; font.pixelSize: 10; font.family: mono }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: "−"; color: Downloader.maxConcurrent > 1 ? soft : dim; font.pixelSize: 14
                                MouseArea { anchors.fill: parent; anchors.margins: -6; hoverEnabled: true; onClicked: if (Downloader.maxConcurrent > 1) Downloader.maxConcurrent -= 1 }
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: "+"; color: Downloader.maxConcurrent < 4 ? soft : dim; font.pixelSize: 14
                                MouseArea { anchors.fill: parent; anchors.margins: -6; hoverEnabled: true; onClicked: if (Downloader.maxConcurrent < 4) Downloader.maxConcurrent += 1 }
                            }
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            visible: Downloader.count > Downloader.active
                            text: "CLEAR FINISHED"; font.pixelSize: 10; font.family: mono
                            color: clearHover.containsMouse ? ink : dim
                            MouseArea { id: clearHover; anchors.fill: parent; anchors.margins: -5; hoverEnabled: true; onClicked: Downloader.clearFinished() }
                        }
                    }
                }
                ListView {
                    id: jobList
                    anchors { top: jobsHead.bottom; left: parent.left; right: parent.right; bottom: parent.bottom; topMargin: 6 }
                    clip: true; spacing: 8; model: Downloader.jobs
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                    delegate: Rectangle {
                        id: jobRow
                        readonly property bool active: status === "queued" || status === "running"
                        readonly property bool expandable: itemCount > 0
                        property bool expanded: false
                        width: jobList.width
                        height: 60 + (expanded && expandable ? trackBlock.height + 8 : 0)
                        radius: 11
                        color: panel; border.color: status === "running" ? "#3d4420" : line; border.width: 1
                        clip: true
                        Column {
                            anchors { left: parent.left; right: parent.right; top: parent.top }
                            Item {
                                id: jobHeader
                                width: parent.width; height: 60
                                Rectangle {
                                    x: 12; anchors.verticalCenter: parent.verticalCenter
                                    width: 36; height: 36; radius: 9
                                    color: status === "running" ? "#151a0e" : "#1c1c22"
                                    Icon {
                                        anchors.centerIn: parent; size: 16
                                        name: status === "failed" ? "close" : status === "running" || status === "queued" ? "download" : isPlaylist ? "queue" : "note"
                                        color: status === "failed" ? danger : status === "running" ? acc : status === "done" ? soft : dim
                                    }
                                }
                                Column {
                                    anchors { left: parent.left; leftMargin: 60; right: jobSide.left; rightMargin: 8; verticalCenter: parent.verticalCenter }
                                    spacing: 3
                                    width: parent.width - 60 - jobSide.width - 20
                                    Text { width: parent.width; text: title; color: status === "canceled" ? mut : ink; font.pixelSize: 13; font.family: disp; font.weight: Font.Medium; elide: Text.ElideRight }
                                    Text { width: parent.width; text: detail; color: status === "failed" ? danger : mut; font.pixelSize: 11; elide: Text.ElideRight }
                                }
                                Row {
                                    id: jobSide
                                    anchors { right: parent.right; rightMargin: 8; verticalCenter: parent.verticalCenter }
                                    spacing: 4
                                    Text {
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: status === "running" ? Math.floor(progress * 100) + "%" : status.toUpperCase()
                                        color: status === "running" ? acc : status === "failed" ? danger : dim
                                        font.pixelSize: 10; font.family: mono
                                    }
                                    Text {
                                        anchors.verticalCenter: parent.verticalCenter
                                        visible: jobRow.expandable
                                        text: jobRow.expanded ? "▾" : "▸"; color: dim; font.pixelSize: 12
                                    }
                                    IconButton {
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: 30; height: 30; icon: "close"; iconSize: 14
                                        visible: jobRow.active; tip: "Cancel"
                                        onClicked: Downloader.cancel(jobId)
                                    }
                                    Item { width: 4; height: 1; visible: !jobRow.active }
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    enabled: jobRow.expandable
                                    onClicked: jobRow.expanded = !jobRow.expanded
                                }
                            }
                            Column {
                                id: trackBlock
                                width: parent.width
                                visible: jobRow.expanded && jobRow.expandable
                                spacing: 0
                                Repeater {
                                    model: items
                                    delegate: Item {
                                        required property var modelData
                                        width: trackBlock.width; height: 30
                                        Row {
                                            anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter; leftMargin: 60; rightMargin: 12 }
                                            spacing: 8
                                            Text {
                                                anchors.verticalCenter: parent.verticalCenter
                                                width: 28
                                                text: String(modelData.index).padStart(2, "0")
                                                color: dim; font.pixelSize: 10; font.family: mono
                                            }
                                            Text {
                                                anchors.verticalCenter: parent.verticalCenter
                                                width: parent.width - 28 - 8 - 86
                                                text: modelData.title; color: modelData.state === "failed" ? dim : ink
                                                font.pixelSize: 12; elide: Text.ElideRight
                                            }
                                            Text {
                                                anchors.verticalCenter: parent.verticalCenter
                                                width: 78; horizontalAlignment: Text.AlignRight
                                                text: modelData.state === "downloading" ? Math.floor(modelData.progress * 100) + "%"
                                                      : modelData.state === "done" ? "DONE"
                                                      : modelData.state === "failed" ? "SKIPPED" : "QUEUED"
                                                color: modelData.state === "downloading" ? acc
                                                       : modelData.state === "done" ? soft
                                                       : modelData.state === "failed" ? danger : dim
                                                font.pixelSize: 10; font.family: mono
                                            }
                                        }
                                        Rectangle {
                                            anchors { left: parent.left; leftMargin: 60; right: parent.right; rightMargin: 12; bottom: parent.bottom }
                                            height: 2; radius: 1; color: "#232329"; visible: modelData.state === "downloading"
                                            Rectangle { width: parent.width * modelData.progress; height: 2; radius: 1; color: acc }
                                        }
                                        Rectangle {
                                            anchors { left: parent.left; leftMargin: 60; right: parent.right; bottom: parent.bottom }
                                            height: 1; color: line; opacity: 0.5; visible: modelData.state !== "downloading"
                                        }
                                    }
                                }
                                Item { width: 1; height: 6 }
                            }
                        }
                        Rectangle {   // progress across the whole job
                            anchors { left: parent.left; bottom: parent.bottom }
                            visible: status === "running"
                            height: 3; width: parent.width * progress; color: acc
                            Behavior on width { NumberAnimation { duration: 180 } }
                        }
                    }
                    Text {
                        anchors { top: parent.top; left: parent.left; right: parent.right; topMargin: 30 }
                        visible: jobList.count === 0 && Downloader.available
                        horizontalAlignment: Text.AlignHCenter; wrapMode: Text.WordWrap
                        color: mut; font.pixelSize: 12; lineHeight: 1.3
                        text: "Nothing downloading.\nIn YouTube Music, pick Share → Copy link on a song, playlist or album and paste it above."
                    }
                }
            }
            // track list; the album strip and the column header scroll with it
            ListView {
                id: trackList
                anchors { top: chips.bottom; left: parent.left; right: parent.right; bottom: parent.bottom; topMargin: 12; leftMargin: 24; rightMargin: 24 }
                visible: page === "tracks"
                clip: true; model: Library.tracks; spacing: 0
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                // Drag-reordering only makes sense on the unfiltered mix in its own order.
                readonly property bool reorderable: Library.viewKind === "mix" && Library.sortMode === "def"
                                                    && Library.query === "" && Library.format === "All"
                property int dragFrom: -1
                property int dropAt: -1

                // One column grid shared by the header and every row.
                readonly property real pad: 12
                readonly property real numW: 26
                readonly property real heartW: 28
                readonly property real timeW: 44
                readonly property real gap: 16
                readonly property bool showAlbum: width >= 560
                readonly property real xTitle: pad + numW + 12
                readonly property real xHeart: width - pad - heartW
                readonly property real xTime: xHeart - 8 - timeW
                readonly property real albumW: showAlbum ? Math.min(260, (xTime - gap - xTitle) * 0.4) : 0
                readonly property real xAlbum: xTime - gap - albumW
                readonly property real titleW: (showAlbum ? xAlbum : xTime) - gap - xTitle
                // Inline NEXT UP / + QUEUE / + MIX pills need room; otherwise a ⋯ menu.
                readonly property bool inlineActions: width >= 640

                header: Column {
                    width: trackList.width
                    // The strip appears once the list knows its height; re-anchor to the
                    // top when the header grows or shrinks, or it starts half scrolled.
                    onHeightChanged: Qt.callLater(trackList.positionViewAtBeginning)
                    ListView {
                        id: albumStrip
                        width: parent.width
                        // Short panes (quarter tiles) keep the room for tracks.
                        visible: !tracksOnly && Library.viewKind === "all" && Library.query === "" && count > 0
                                 && trackList.height >= 420
                        height: visible ? 208 + 18 : 0
                        orientation: ListView.Horizontal; spacing: 14
                        clip: true; model: Library.albums
                        boundsBehavior: Flickable.StopAtBounds
                        delegate: AlbumCard { width: 150 }
                    }
                    Item {
                        width: parent.width; height: 32
                        Rectangle { anchors { left: parent.left; right: parent.right; top: parent.top } height: 1; color: line }
                        Rectangle { anchors { left: parent.left; right: parent.right; bottom: parent.bottom } height: 1; color: line }
                        Text { x: trackList.pad; width: trackList.numW; anchors.verticalCenter: parent.verticalCenter; text: "#"; color: dim; font.pixelSize: 10; font.family: mono }
                        Text { x: trackList.xTitle; anchors.verticalCenter: parent.verticalCenter; text: "TITLE"; color: dim; font.pixelSize: 10; font.family: mono }
                        Text { x: trackList.xAlbum; visible: trackList.showAlbum; anchors.verticalCenter: parent.verticalCenter; text: "ALBUM"; color: dim; font.pixelSize: 10; font.family: mono }
                        Text { x: trackList.xTime; width: trackList.timeW; horizontalAlignment: Text.AlignRight; anchors.verticalCenter: parent.verticalCenter; text: "TIME"; color: dim; font.pixelSize: 10; font.family: mono }
                    }
                    Item { width: 1; height: 4 }
                }

                delegate: Rectangle {
                    id: trackRow
                    width: trackList.width; height: 56; radius: 9
                    readonly property bool isCur: ti === Player.currentId
                    readonly property bool hovered: rowHover.containsMouse || actionsHover.hovered || handleArea.containsMouse || likeArea.containsMouse
                    color: isCur ? "#151a0e" : hovered ? "#141419" : "transparent"
                    border.color: trackList.dropAt === index && trackList.dragFrom !== index ? acc : "transparent"
                    border.width: 1
                    opacity: trackList.dragFrom === index ? 0.5 : 1

                    MouseArea {
                        id: rowHover
                        anchors.fill: parent; hoverEnabled: true
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        onClicked: mouse => {
                            if (mouse.button === Qt.RightButton) {
                                trackMenu.trackId = ti; trackMenu.slot = slot; trackMenu.popup();
                            } else {
                                Player.playTracks(Library.visibleTrackIds(), index);
                            }
                        }
                    }
                    // number / now-playing / drag handle
                    Item {
                        x: trackList.pad; width: trackList.numW; height: parent.height
                        readonly property bool showHandle: trackList.reorderable && (trackRow.hovered || handleArea.pressed)
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            visible: !trackRow.isCur && !parent.showHandle
                            text: String(index + 1).padStart(2, "0"); color: dim; font.pixelSize: 11; font.family: mono
                        }
                        EqBars { anchors.verticalCenter: parent.verticalCenter; visible: trackRow.isCur && !parent.showHandle }
                        Icon { anchors.verticalCenter: parent.verticalCenter; x: -3; visible: parent.showHandle; name: "drag"; size: 18; color: handleArea.containsMouse || handleArea.pressed ? ink : mut }
                    }
                    Column {
                        x: trackList.xTitle; width: trackList.titleW
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 2
                        Text { width: parent.width; text: t; color: trackRow.isCur ? acc : ink; font.pixelSize: 13; font.family: disp; font.weight: Font.Medium; elide: Text.ElideRight }
                        Text { width: parent.width; text: a; color: mut; font.pixelSize: 12; elide: Text.ElideRight }
                    }
                    Text {
                        // hidden under the hover actions rather than peeking out beside them
                        x: trackList.xAlbum; width: trackList.albumW; visible: trackList.showAlbum && !(actions.visible && trackList.inlineActions)
                        anchors.verticalCenter: parent.verticalCenter
                        text: al; color: mut; font.pixelSize: 12; elide: Text.ElideRight
                    }
                    Text {
                        x: trackList.xTime; width: trackList.timeW; horizontalAlignment: Text.AlignRight
                        anchors.verticalCenter: parent.verticalCenter
                        visible: !actions.visible
                        text: d; color: mut; font.pixelSize: 12; font.family: mono
                    }
                    Item {
                        x: trackList.xHeart; width: trackList.heartW; height: parent.height
                        Icon { anchors.centerIn: parent; name: liked ? "heart-fill" : "heart"; size: 16; color: liked ? acc : likeArea.containsMouse ? ink : dim }
                        MouseArea { id: likeArea; anchors.fill: parent; hoverEnabled: true; onClicked: Library.toggleLike(ti) }
                    }
                    // mix reorder handle (over the number column)
                    MouseArea {
                        id: handleArea
                        x: 4; width: trackList.numW + 12; height: parent.height
                        enabled: trackList.reorderable; hoverEnabled: true
                        cursorShape: enabled ? (pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor) : Qt.ArrowCursor
                        preventStealing: true
                        onPressed: { trackList.dragFrom = index; trackList.dropAt = index; }
                        onPositionChanged: mouse => {
                            if (!pressed) return;
                            const p = mapToItem(trackList.contentItem, mouse.x, mouse.y);
                            const at = trackList.indexAt(trackList.width / 2, p.y);
                            if (at >= 0) trackList.dropAt = at;
                            else trackList.dropAt = p.y < 0 ? 0 : trackList.count - 1;
                        }
                        onReleased: {
                            const from = trackList.dragFrom, to = trackList.dropAt;
                            trackList.dragFrom = -1; trackList.dropAt = -1;
                            if (from >= 0 && to >= 0 && from !== to) Library.moveInMix(Library.viewMixId, from, to);
                        }
                        onCanceled: { trackList.dragFrom = -1; trackList.dropAt = -1; }
                    }
                    // hover actions sit on a solid backing so the album/time underneath never shows through
                    Rectangle {
                        id: actions
                        visible: trackRow.hovered && trackList.dragFrom < 0
                        anchors { right: parent.right; rightMargin: trackList.pad + trackList.heartW + 4; verticalCenter: parent.verticalCenter }
                        width: actionRow.implicitWidth + 16; height: 40; radius: 8
                        color: trackRow.color === "transparent" ? "#141419" : trackRow.color
                        HoverHandler { id: actionsHover }
                        Row {
                            id: actionRow
                            anchors.centerIn: parent
                            spacing: 6
                            Repeater {
                                model: !trackList.inlineActions ? []
                                     : Library.viewKind === "mix"
                                       ? [{t: "NEXT UP", a: "next"}, {t: "+ QUEUE", a: "end"}, {t: "+ MIX", a: "mix"}, {t: "REMOVE", a: "remove"}]
                                       : [{t: "NEXT UP", a: "next"}, {t: "+ QUEUE", a: "end"}, {t: "+ MIX", a: "mix"}]
                                Pill {
                                    text: modelData.t
                                    textColor: modelData.a === "remove" ? danger : ink
                                    onClicked: {
                                        if (modelData.a === "next") Player.playNext(ti);
                                        else if (modelData.a === "end") Player.enqueue(ti);
                                        else if (modelData.a === "mix") { mixMenu.trackId = ti; mixMenu.popup(); }
                                        else Library.removeFromMix(Library.viewMixId, slot);
                                    }
                                }
                            }
                            IconButton {
                                visible: !trackList.inlineActions
                                width: 30; height: 30; icon: "more"; iconSize: 16; tip: "More"
                                onClicked: { trackMenu.trackId = ti; trackMenu.slot = slot; trackMenu.popup(); }
                            }
                        }
                    }
                }

                // empty states (below the header)
                Column {
                    y: (trackList.headerItem ? trackList.headerItem.height : 0) + 36
                    width: parent.width
                    spacing: 10
                    visible: trackList.count === 0
                    readonly property bool emptyMix: Library.viewKind === "mix" && Library.query === "" && Library.format === "All"
                    Text {
                        width: parent.width; horizontalAlignment: Text.AlignHCenter
                        color: ink; font.pixelSize: 16; font.family: disp; font.weight: Font.Medium
                        text: parent.emptyMix ? "This mix is empty"
                            : Library.totalCount > 0 ? "Nothing matches"
                            : Library.scanning ? "Scanning your music…" : "No music found"
                    }
                    Text {
                        width: parent.width; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.WordWrap
                        color: mut; font.pixelSize: 12
                        text: parent.emptyMix ? "Right-click any track (or use + MIX) to add it here."
                            : Library.totalCount > 0 ? "Try another search or filter."
                            : Library.error !== "" ? Library.error
                            : "Flow plays the FLAC, MP3 and WAV files in " + Library.musicDir + "."
                    }
                    Pill {
                        visible: !parent.emptyMix && Library.totalCount === 0 && !Library.scanning
                        anchors.horizontalCenter: parent.horizontalCenter
                        implicitHeight: 34; radius: 17; solid: true
                        text: "CHOOSE MUSIC FOLDER"; icon: "folder"
                        onClicked: folderDialog.open()
                    }
                }
            }
        }

        // ---- now-playing drawer (column in wide mode, slide-over when mid) ----
        Rectangle {
            id: drawer
            anchors { top: parent.top; right: parent.right; bottom: parent.bottom }
            width: Math.min(322, parent.width * 0.92)
            visible: !mid || drawerOpen
            z: 10
            color: panel
            Rectangle { anchors { top: parent.top; bottom: parent.bottom; left: parent.left } width: 1; color: line }
            Row {
                id: tabs
                anchors { top: parent.top; left: parent.left; right: parent.right; topMargin: 12; leftMargin: 12; rightMargin: mid ? 50 : 12 }
                height: 34; spacing: 4
                Repeater {
                    model: [{t: "NOW", k: "now"}, {t: "QUEUE", k: "queue"}, {t: "LYRICS", k: "lyrics"}]
                    Rectangle {
                        width: (tabs.width - 8) / 3; height: 34
                        radius: 9; color: drawerTab === modelData.k ? panel2 : tabHover.containsMouse ? "#17171c" : "transparent"
                        border.color: drawerTab === modelData.k ? line : "transparent"; border.width: 1
                        Text { anchors.centerIn: parent; color: drawerTab === modelData.k ? ink : dim; font.pixelSize: 10; font.family: mono
                            text: modelData.k === "queue" && Player.queueCount > 0 ? modelData.t + " · " + Player.queueCount : modelData.t }
                        MouseArea { id: tabHover; anchors.fill: parent; hoverEnabled: true; onClicked: drawerTab = modelData.k }
                    }
                }
            }
            IconButton {
                visible: mid
                anchors { right: parent.right; rightMargin: 10; verticalCenter: tabs.verticalCenter }
                width: 32; height: 32; icon: "close"; iconSize: 16; tip: "Close"
                onClicked: drawerOpen = false
            }
            // NOW tab
            Flickable {
                anchors { top: tabs.bottom; left: parent.left; right: parent.right; bottom: parent.bottom; topMargin: 14; leftMargin: 14; rightMargin: 14; bottomMargin: 14 }
                contentHeight: nowCol.height; clip: true
                boundsBehavior: Flickable.StopAtBounds
                visible: drawerTab === "now"
                Column {
                    id: nowCol
                    width: parent.width; spacing: 12
                    Art {
                        width: parent.width; height: parent.width; radius: 12
                        source: cur.cover; c1: cur.c1; c2: cur.c2
                        decodeSize: 640
                    }
                    Text { width: parent.width; text: Player.hasTrack ? cur.t : "Nothing playing"; color: ink; font.pixelSize: 20; font.family: disp; font.weight: Font.DemiBold; wrapMode: Text.WordWrap }
                    Text { width: parent.width; text: Player.hasTrack ? cur.a + " — " + cur.al : "Pick a track or album to start."; color: mut; font.pixelSize: 12; elide: Text.ElideRight }
                    Flow {
                        width: parent.width; spacing: 6
                        visible: Player.hasTrack
                        Repeater {
                            model: [cur.f + (cur.hi ? " HI-RES" : ""), cur.khz, cur.br].filter(s => s !== "")
                            Rectangle {
                                height: 24; width: specLabel.implicitWidth + 16; radius: 7
                                color: index === 0 && cur.hi ? acc : "#131318"
                                border.color: index === 0 && cur.hi ? acc : line2; border.width: 1
                                Text { id: specLabel; anchors.centerIn: parent; text: modelData; font.pixelSize: 10; font.family: mono
                                    color: index === 0 && cur.hi ? "#000" : soft; font.bold: index === 0 && cur.hi }
                            }
                        }
                    }
                    Text {
                        width: parent.width; visible: Player.error !== ""; text: Player.error
                        color: danger; font.pixelSize: 11; wrapMode: Text.WordWrap
                    }
                    Item {
                        width: parent.width; height: 14
                        Rectangle { anchors.verticalCenter: parent.verticalCenter; width: 5; height: 5; radius: 2.5; color: dim }
                        Text { x: 11; anchors.verticalCenter: parent.verticalCenter; text: "CLICK TO SEEK"; color: dim; font.pixelSize: 10; font.family: mono }
                        Text { anchors { right: parent.right; verticalCenter: parent.verticalCenter } text: fmtTime(Player.position) + " / " + fmtTime(Player.duration); color: dim; font.pixelSize: 10; font.family: mono }
                    }
                    Item {
                        id: waveArea
                        width: parent.width; height: 76
                        Rectangle { anchors.fill: parent; radius: 10; color: "#0a0a0d"; border.color: line; border.width: 1 }
                        Row {
                            anchors { fill: parent; margins: 8 }
                            spacing: 2
                            Repeater {
                                model: 96
                                Rectangle {
                                    // Real loudness per bar once analysed; a flat line meanwhile.
                                    readonly property real level: Player.peaks.length === 96 ? Player.peaks[index] : 0
                                    width: Math.max(1, (waveArea.width - 16 - 95 * 2) / 96)
                                    height: 3 + level * 57
                                    Behavior on height { NumberAnimation { duration: 250; easing.type: Easing.OutCubic } }
                                    anchors.verticalCenter: parent.verticalCenter
                                    color: (index / 96) < progress ? acc : "#3f3f46"
                                }
                            }
                        }
                        MouseArea { anchors.fill: parent; onClicked: mouse => Player.seek(Player.duration * (mouse.x / width)) }
                    }
                    Text { text: "UP NEXT"; color: dim; font.pixelSize: 10; font.family: mono; visible: upNext.count > 0 }
                    Repeater {
                        id: upNext
                        model: Math.max(0, Math.min(3, Player.queueCount - Player.queueIndex - 1))
                        Rectangle {
                            id: upRow
                            readonly property int row: Player.queueIndex + 1 + index
                            readonly property var k: { Player.queueCount; Player.queueIndex; return Player.queueAt(row); }
                            width: parent.width; height: 48; radius: 9
                            color: upHover.containsMouse ? "#17171c" : "transparent"; border.color: line; border.width: 1
                            Art {
                                x: 8; anchors.verticalCenter: parent.verticalCenter; width: 32; height: 32; radius: 6
                                source: upRow.k.cover; c1: upRow.k.c1; c2: upRow.k.c2; accent: false; decodeSize: 64
                            }
                            Column {
                                x: 50; width: parent.width - 50 - 52
                                anchors.verticalCenter: parent.verticalCenter
                                Text { width: parent.width; text: upRow.k.t; color: ink; font.pixelSize: 12; font.family: disp; font.weight: Font.Medium; elide: Text.ElideRight }
                                Text { width: parent.width; text: upRow.k.a; color: mut; font.pixelSize: 11; elide: Text.ElideRight }
                            }
                            Text { anchors { right: parent.right; rightMargin: 10; verticalCenter: parent.verticalCenter } text: upRow.k.d; color: dim; font.pixelSize: 11; font.family: mono }
                            MouseArea { id: upHover; anchors.fill: parent; hoverEnabled: true; onClicked: Player.jump(upRow.row) }
                        }
                    }
                }
            }
            // QUEUE tab: click to play, drag the handle to reorder, ✕ to remove
            ListView {
                id: queueList
                anchors { top: tabs.bottom; left: parent.left; right: parent.right; bottom: parent.bottom; topMargin: 14; leftMargin: 14; rightMargin: 14; bottomMargin: 14 }
                clip: true; spacing: 8; model: Player.queue
                boundsBehavior: Flickable.StopAtBounds
                visible: drawerTab === "queue"
                property int dragFrom: -1
                property int dropAt: -1
                delegate: Column {
                    id: qDelegate
                    width: ListView.view.width; spacing: 0
                    readonly property bool isCur: index === Player.queueIndex
                    Text { text: div; color: mut; font.pixelSize: 11; font.bold: true; visible: div !== ""; bottomPadding: 5; width: parent.width; elide: Text.ElideRight }
                    Rectangle {
                        id: qRow
                        width: parent.width; height: 46; radius: 9
                        color: qDelegate.isCur ? "#151a0e" : qHover.containsMouse ? "#141419" : panel2
                        border.color: queueList.dropAt === index && queueList.dragFrom !== index ? acc : line; border.width: 1
                        opacity: queueList.dragFrom === index ? 0.5 : 1
                        readonly property bool hovered: qHover.containsMouse || removeArea.containsMouse || dragArea.containsMouse
                        MouseArea { id: qHover; anchors.fill: parent; hoverEnabled: true; onClicked: Player.jump(index) }
                        Item {
                            x: 10; width: 22; height: parent.height
                            Text { anchors.verticalCenter: parent.verticalCenter; visible: !qDelegate.isCur; text: String(index + 1).padStart(2, "0"); color: dim; font.pixelSize: 11; font.family: mono }
                            EqBars { anchors.verticalCenter: parent.verticalCenter; visible: qDelegate.isCur }
                        }
                        Column {
                            x: 40; width: parent.width - 40 - 104
                            anchors.verticalCenter: parent.verticalCenter
                            Text { width: parent.width; text: t; color: qDelegate.isCur ? acc : ink; font.pixelSize: 12; font.family: disp; font.weight: Font.Medium; elide: Text.ElideRight }
                            Text { width: parent.width; text: a + " · " + f; color: mut; font.pixelSize: 11; elide: Text.ElideRight }
                        }
                        Text { anchors { right: parent.right; rightMargin: 66; verticalCenter: parent.verticalCenter } text: d; color: dim; font.pixelSize: 11; font.family: mono }
                        Item {
                            anchors { right: parent.right; rightMargin: 34; verticalCenter: parent.verticalCenter }
                            width: 26; height: 26
                            opacity: qRow.hovered ? 1 : 0
                            Icon { anchors.centerIn: parent; name: "close"; size: 14; color: removeArea.containsMouse ? ink : mut }
                            MouseArea { id: removeArea; anchors.fill: parent; hoverEnabled: true; onClicked: Player.removeAt(index) }
                        }
                        Item {
                            anchors { right: parent.right; rightMargin: 6; verticalCenter: parent.verticalCenter }
                            width: 26; height: 32
                            Icon { anchors.centerIn: parent; name: "drag"; size: 16; color: dragArea.containsMouse || dragArea.pressed ? ink : dim }
                            MouseArea {
                                id: dragArea
                                anchors.fill: parent; hoverEnabled: true
                                cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
                                preventStealing: true
                                onPressed: { queueList.dragFrom = index; queueList.dropAt = index; }
                                onPositionChanged: mouse => {
                                    if (!pressed) return;
                                    const p = mapToItem(queueList.contentItem, mouse.x, mouse.y);
                                    const at = queueList.indexAt(queueList.width / 2, p.y);
                                    if (at >= 0) queueList.dropAt = at;
                                    else queueList.dropAt = p.y < 0 ? 0 : queueList.count - 1;
                                }
                                onReleased: {
                                    const from = queueList.dragFrom, to = queueList.dropAt;
                                    queueList.dragFrom = -1; queueList.dropAt = -1;
                                    if (from >= 0 && to >= 0 && from !== to) Player.move(from, to);
                                }
                                onCanceled: { queueList.dragFrom = -1; queueList.dropAt = -1; }
                            }
                        }
                    }
                }
                Text {
                    anchors { top: parent.top; left: parent.left; right: parent.right; topMargin: 24 }
                    visible: queueList.count === 0; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.WordWrap
                    text: "Queue is empty.\nPlay something, or right-click a track → Add to queue."; color: mut; font.pixelSize: 12
                }
            }
            // LYRICS tab: .lrc sidecar or embedded lyrics; timed lines follow playback
            ListView {
                id: lyricsList
                anchors { top: tabs.bottom; left: parent.left; right: parent.right; bottom: parent.bottom; topMargin: 14; leftMargin: 14; rightMargin: 14; bottomMargin: 14 }
                clip: true; spacing: 2
                visible: drawerTab === "lyrics"
                model: Player.lyrics
                currentIndex: Player.lyricsTimed ? Player.lyricIndex : -1
                highlightRangeMode: ListView.ApplyRange
                preferredHighlightBegin: height * 0.3
                preferredHighlightEnd: height * 0.45
                highlightMoveDuration: 350
                delegate: Text {
                    width: ListView.view.width; text: modelData.text; font.pixelSize: 15; font.family: disp
                    wrapMode: Text.WordWrap; padding: 6
                    color: !Player.lyricsTimed ? soft
                         : index === Player.lyricIndex ? ink : index < Player.lyricIndex ? mut : dim
                    MouseArea {
                        anchors.fill: parent; enabled: Player.lyricsTimed
                        onClicked: Player.seek(modelData.ms / 1000)
                    }
                }
                Text {
                    anchors { top: parent.top; left: parent.left; right: parent.right; topMargin: 24 }
                    visible: lyricsList.count === 0; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.WordWrap
                    color: mut; font.pixelSize: 12
                    text: Player.hasTrack ? "No lyrics for this track.\nPut a .lrc file with the same name next to it."
                                          : "Lyrics show up here while a track plays."
                }
            }
        }
    }

    Rectangle {
        id: toastBox
        anchors { horizontalCenter: parent.horizontalCenter; bottom: dock.top; bottomMargin: 14 }
        z: 20; radius: 10; height: 36; width: Math.min(root.width - 32, toast.implicitWidth + 32)
        color: panel; border.color: line2; border.width: 1
        opacity: toastShown ? 1 : 0; visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 200 } }
        Text { id: toast; anchors.centerIn: parent; width: parent.width - 32
               color: ink; font.pixelSize: 12; elide: Text.ElideRight; horizontalAlignment: Text.AlignHCenter }
    }

    // ================= dock =================
    Rectangle {
        id: dock
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        height: 86; color: "#121218"
        Rectangle { anchors { left: parent.left; right: parent.right; top: parent.top } height: 1; color: line }
        // seek
        Item {
            id: seek
            anchors { top: parent.top; left: parent.left; right: parent.right; topMargin: 6; leftMargin: 16; rightMargin: 16 }
            height: 14
            Rectangle { anchors.verticalCenter: parent.verticalCenter; width: parent.width; height: seekArea.containsMouse ? 6 : 4; radius: 3; color: "#232329" }
            Rectangle { anchors.verticalCenter: parent.verticalCenter; width: parent.width * progress; height: seekArea.containsMouse ? 6 : 4; radius: 3; color: acc }
            MouseArea { id: seekArea; anchors.fill: parent; hoverEnabled: true; onClicked: mouse => Player.seek(Player.duration * (mouse.x / width)) }
        }
        // left: now playing (click opens the drawer's NOW tab)
        Item {
            id: nowInfo
            anchors { left: parent.left; leftMargin: 16; verticalCenter: parent.verticalCenter; verticalCenterOffset: 6 }
            height: 44
            // Everything left of the centred transport, minus a 16px gap.
            readonly property real room: transport.x - 16 - 16
            width: Math.max(42, room)
            Art {
                id: dockArt
                anchors.verticalCenter: parent.verticalCenter
                width: 42; height: 42; radius: 8
                source: cur.cover; c1: cur.c1; c2: cur.c2; accent: false; decodeSize: 96
            }
            Column {
                id: nowText
                anchors { left: dockArt.right; leftMargin: 12; verticalCenter: parent.verticalCenter }
                // title gets what is left after art, gaps and the heart
                width: Math.max(0, Math.min(Math.max(nowTitle.implicitWidth, nowArtist.implicitWidth), nowInfo.width - 42 - 12 - 40))
                visible: width >= 40
                spacing: 2
                Text { id: nowTitle; width: parent.width; text: Player.hasTrack ? cur.t : "Nothing playing"; color: ink; font.pixelSize: 13; font.family: disp; font.weight: Font.Medium; elide: Text.ElideRight }
                Text { id: nowArtist; width: parent.width; text: cur.a; color: mut; font.pixelSize: 12; elide: Text.ElideRight; visible: text !== "" }
            }
            MouseArea {
                anchors { left: parent.left; right: nowText.visible ? nowText.right : dockArt.right; top: parent.top; bottom: parent.bottom }
                enabled: Player.hasTrack
                cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                onClicked: openDrawer("now")
            }
            Item {
                anchors { left: nowText.visible ? nowText.right : dockArt.right; leftMargin: 8; verticalCenter: parent.verticalCenter }
                width: 28; height: 28
                visible: Player.hasTrack
                Icon { anchors.centerIn: parent; name: cur.liked ? "heart-fill" : "heart"; size: 16; color: cur.liked ? acc : dockLike.containsMouse ? ink : dim }
                MouseArea { id: dockLike; anchors.fill: parent; hoverEnabled: true; onClicked: Library.toggleLike(Player.currentId) }
            }
        }
        // center: transport
        Row {
            id: transport
            anchors { horizontalCenter: parent.horizontalCenter; verticalCenter: parent.verticalCenter; verticalCenterOffset: 6 }
            spacing: 6
            IconButton { anchors.verticalCenter: parent.verticalCenter; icon: "shuffle"; lit: Player.shuffle; tip: "Shuffle"; onClicked: Player.shuffle = !Player.shuffle }
            IconButton { anchors.verticalCenter: parent.verticalCenter; icon: "prev"; iconSize: 16; tip: "Previous"; onClicked: Player.previous() }
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: 44; height: 44; radius: 22
                color: playHoverDock.containsMouse ? acc : ink
                Icon { anchors.centerIn: parent; anchors.horizontalCenterOffset: Player.playing ? 0 : 1; name: Player.playing ? "pause" : "play"; size: 18; color: "#000" }
                MouseArea { id: playHoverDock; anchors.fill: parent; hoverEnabled: true; onClicked: Player.toggle() }
            }
            IconButton { anchors.verticalCenter: parent.verticalCenter; icon: "next"; iconSize: 16; tip: "Next"; onClicked: Player.next() }
            IconButton {
                anchors.verticalCenter: parent.verticalCenter
                icon: "repeat"; lit: Player.repeatMode > 0
                tip: Player.repeatMode === 2 ? "Repeat track" : Player.repeatMode === 1 ? "Repeat queue" : "Repeat off"
                onClicked: Player.repeatMode = (Player.repeatMode + 1) % 3   // off → all → one
                Rectangle {
                    visible: Player.repeatMode === 2
                    anchors { right: parent.right; top: parent.top; rightMargin: 2; topMargin: 2 }
                    width: 13; height: 13; radius: 6.5; color: acc
                    Text { anchors.centerIn: parent; text: "1"; color: "#000"; font.pixelSize: 9; font.family: mono; font.bold: true }
                }
            }
            IconButton { anchors.verticalCenter: parent.verticalCenter; icon: "queue"; lit: drawerTab === "queue" && (!mid || drawerOpen); tip: "Queue"; onClicked: openDrawer("queue") }
            IconButton { anchors.verticalCenter: parent.verticalCenter; icon: "lyrics"; lit: drawerTab === "lyrics" && (!mid || drawerOpen); tip: "Lyrics"; onClicked: openDrawer("lyrics") }
        }
        // right: time + volume
        Row {
            anchors { right: parent.right; rightMargin: 16; verticalCenter: parent.verticalCenter; verticalCenterOffset: 6 }
            spacing: 10
            Text {
                anchors.verticalCenter: parent.verticalCenter
                // hidden rather than overlapping the transport on the narrowest tiles
                visible: dock.width - (transport.x + transport.width) > implicitWidth + 40
                text: fmtTime(Player.position) + " / " + fmtTime(Player.duration); color: mut; font.pixelSize: 11; font.family: mono
            }
            Icon { anchors.verticalCenter: parent.verticalCenter; visible: vol.visible; name: "volume"; size: 16; color: mut }
            Item {
                id: vol
                visible: dock.width - (transport.x + transport.width) > 250
                anchors.verticalCenter: parent.verticalCenter; width: 90; height: 16
                Rectangle { anchors.verticalCenter: parent.verticalCenter; width: parent.width; height: 4; radius: 2; color: "#26262e" }
                Rectangle { anchors.verticalCenter: parent.verticalCenter; width: parent.width * Player.volume; height: 4; radius: 2; color: "#d8d5cb" }
                MouseArea {
                    anchors.fill: parent
                    onPressed: mouse => Player.volume = mouse.x / width
                    onPositionChanged: mouse => { if (pressed) Player.volume = mouse.x / width; }
                }
                WheelHandler { onWheel: event => Player.volume = Player.volume + (event.angleDelta.y > 0 ? 0.05 : -0.05) }
            }
        }
    }
}
