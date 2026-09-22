// Flow — app shell. Library (TagLib → SQLite index) and Player (QMediaPlayer)
// are C++ singletons from src/; Mpris (src/mpris.*) mirrors Player on D-Bus.
import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import Flow

ApplicationWindow {
    id: root
    width: 1280
    height: 800
    minimumWidth: 560
    minimumHeight: 620
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
    readonly property string disp: "Space Grotesk"
    readonly property string mono: "JetBrainsMono Nerd Font"

    // ---- responsive tiers (mirror ui-demo.html stress test) ----
    readonly property bool mid: root.width < 1080
    readonly property bool narrow: root.width < 820

    // ---- view state (playback + library state live in C++) ----
    readonly property var cur: Player.current
    readonly property real progress: Player.duration > 0 ? Math.min(1, Player.position / Player.duration) : 0
    property string drawerTab: "now" // now | queue | lyrics
    property string page: "tracks"   // tracks | albums (album/mix detail = tracks + Library view)
    property bool tracksOnly: false   // "Tracks" hides the album strip that "Overview" shows
    property bool albumsFromGrid: false  // where "← BACK" from an album returns to
    property bool drawerOpen: true

    function fmtTime(s) {
        s = Math.max(0, Math.floor(s));
        return Math.floor(s / 60) + ":" + String(s % 60).padStart(2, "0");
    }
    function goTo(k) {
        if (k === "albums") { page = "albums"; return; }
        page = "tracks";
        tracksOnly = k === "tracks";
        Library.showAll();
        Library.format = k === "liked" ? "Liked" : "All";
    }
    function nextMixName() {
        return "Mix " + (mixRepeater.count + 1);
    }

    Shortcut { sequence: "Space"; onActivated: Player.toggle() }
    Shortcut { sequence: "Right"; onActivated: Player.next() }
    Shortcut { sequence: "Left"; onActivated: Player.previous() }
    Shortcut { sequence: "Escape"; onActivated: { drawerOpen = false; searchField.focus = false; } }
    Shortcut { sequence: "Ctrl+K"; onActivated: searchField.forceActiveFocus() }

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

    // Album tile: click opens the album, the round button plays it.
    component AlbumCard: Item {
        id: card
        property bool fromGrid: false
        height: width + 58
        Art {
            id: cardArt
            width: card.width; height: card.width
            source: cover; c1: model.c1; c2: model.c2
            accentSize: width * 0.5; accentX: (albumIndex % 3) * width * 0.22 - 8; accentY: (albumIndex % 2) * width * 0.26 - 10
            decodeSize: 300
            Row {
                x: 8; y: 8; spacing: 5
                Rectangle { height: 18; width: 40; radius: 9; color: "#00000099";
                    Text { anchors.centerIn: parent; text: f; color: "#fff"; font.pixelSize: 9; font.family: mono } }
                Rectangle { height: 18; width: 52; radius: 9; color: acc; visible: hi;
                    Text { anchors.centerIn: parent; text: "HI-RES"; color: "#101204"; font.pixelSize: 9; font.family: mono; font.bold: true } }
            }
        }
        Text { y: card.width + 6; width: card.width; text: al; color: ink; font.pixelSize: 13; font.family: disp; elide: Text.ElideRight }
        Text { y: card.width + 25; width: card.width; text: khz !== "" ? a + " · " + khz : a; color: mut; font.pixelSize: 12; elide: Text.ElideRight }
        MouseArea {
            id: cardHover
            anchors.fill: parent; hoverEnabled: true
            onClicked: { albumsFromGrid = card.fromGrid; page = "tracks"; Library.showAlbum(albumIndex); }
        }
        Rectangle {
            x: card.width - 44; y: card.width - 44; width: 36; height: 36; radius: 18
            visible: cardHover.containsMouse || playHover.containsMouse
            color: playHover.containsMouse ? ink : acc
            Text { anchors.centerIn: parent; anchors.horizontalCenterOffset: 1; text: "▶"; color: "#000"; font.pixelSize: 13 }
            MouseArea { id: playHover; anchors.fill: parent; hoverEnabled: true; onClicked: Player.playTracks(Library.albumTrackIds(albumIndex), 0) }
        }
    }

    component FlowMenuItem: MenuItem {
        id: fmi
        implicitHeight: 32
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

    // ================= header =================
    Rectangle {
        id: chrome
        anchors { top: parent.top; left: parent.left; right: parent.right }
        height: 52; color: bg
        Rectangle { anchors { left: parent.left; right: parent.right; bottom: parent.bottom } height: 1; color: line }

        Row {
            anchors { left: parent.left; verticalCenter: parent.verticalCenter; leftMargin: 16 }
            spacing: 7
            Repeater { model: 3; Rectangle { width: 11; height: 11; radius: 5.5; color: "#2c2c34" } }
        }
        Text {
            anchors { left: parent.left; verticalCenter: parent.verticalCenter; leftMargin: 62 }
            text: "<font color='" + ink + "'><b>Flow</b></font> <font color='" + mut + "'>/ Collection</font>"
            font.pixelSize: 13; textFormat: Text.RichText
            visible: !narrow
        }
        Rectangle {
            id: searchBox
            anchors.centerIn: parent
            width: Math.min(460, parent.width * 0.42); height: 34; radius: 17
            color: panel; border.color: searchField.activeFocus ? line2 : line; border.width: 1
            TextField {
                id: searchField
                anchors.fill: parent; anchors.leftMargin: 34; anchors.rightMargin: 52
                verticalAlignment: TextInput.AlignVCenter
                placeholderText: "Search your collection…"
                placeholderTextColor: dim
                color: ink; font.pixelSize: 13
                background: Item {}
                onTextEdited: Library.query = text
            }
            Text { x: 13; anchors.verticalCenter: parent.verticalCenter; text: "⌕"; color: dim; font.pixelSize: 14 }
            Rectangle {
                anchors { right: parent.right; verticalCenter: parent.verticalCenter; rightMargin: 10 }
                width: 34; height: 18; radius: 6; color: "transparent"; border.color: line2; border.width: 1
                visible: !narrow
                Text { anchors.centerIn: parent; text: "⌃K"; color: dim; font.pixelSize: 10; font.family: mono }
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
        // scrim for drawer mode
        Rectangle {
            anchors.fill: parent; color: "black"; opacity: 0.45; z: 9
            visible: mid && drawerOpen
            MouseArea { anchors.fill: parent; onClicked: drawerOpen = false }
        }

        // ---- rail ----
        Rectangle {
            id: rail
            anchors { top: parent.top; left: parent.left; bottom: parent.bottom }
            width: 224; color: bg; visible: !narrow
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
                        {t: "◈  Overview", k: "overview",
                         on: page === "tracks" && !tracksOnly && Library.viewKind === "all" && Library.format !== "Liked"},
                        {t: "◎  Albums", k: "albums", on: page === "albums" || Library.viewKind === "album"},
                        {t: "♫  Tracks · " + Library.totalCount.toLocaleString(Qt.locale(), "f", 0), k: "tracks",
                         on: page === "tracks" && tracksOnly && Library.viewKind === "all" && Library.format !== "Liked"},
                        {t: "♥  Liked · " + Library.likedCount, k: "liked",
                         on: page === "tracks" && Library.viewKind === "all" && Library.format === "Liked"}
                    ]
                    Rectangle {
                        width: parent.width; height: 34; radius: 9
                        color: modelData.on ? "#1e1e25" : railHover.containsMouse ? "#1a1a20" : "transparent"
                        Text { anchors { left: parent.left; verticalCenter: parent.verticalCenter; leftMargin: 10 } text: modelData.t; color: modelData.on ? "#fff" : "#cfccc2"; font.pixelSize: 13 }
                        MouseArea {
                            id: railHover
                            anchors.fill: parent; hoverEnabled: true
                            onClicked: goTo(modelData.k)
                        }
                    }
                }
                Text { text: "MIXES"; color: dim; font.pixelSize: 10; font.family: mono; leftPadding: 10; topPadding: 12; bottomPadding: 6 }
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
                            onClicked: { page = "tracks"; Library.showMix(mixId); }
                            onDoubleClicked: mixRow.renaming = true
                            onExited: mixRow.confirmDelete = false
                        }
                        Row {
                            anchors { fill: parent; leftMargin: 10; rightMargin: 8 }
                            spacing: 9
                            visible: !mixRow.renaming
                            Rectangle { anchors.verticalCenter: parent.verticalCenter; width: 8; height: 8; radius: 4; color: mixRow.on ? acc : "#3a3a42" }
                            Text { anchors.verticalCenter: parent.verticalCenter; width: parent.width - 60; text: name; color: "#cfccc2"; font.pixelSize: 13; elide: Text.ElideRight }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter; width: 26; horizontalAlignment: Text.AlignRight
                                text: mixRow.confirmDelete ? "del?" : mixHover.containsMouse || delArea.containsMouse ? "✕" : count
                                color: mixRow.confirmDelete ? "#ff8a7a" : delArea.containsMouse ? ink : dim
                                font.pixelSize: 10; font.family: mono
                                MouseArea {
                                    id: delArea
                                    anchors.fill: parent; anchors.margins: -4; hoverEnabled: true
                                    // Two clicks: the first arms, the second deletes.
                                    onClicked: {
                                        if (mixRow.confirmDelete) Library.deleteMix(mixId);
                                        else mixRow.confirmDelete = true;
                                    }
                                    onExited: mixRow.confirmDelete = false
                                }
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
                    Text {
                        anchors { left: parent.left; verticalCenter: parent.verticalCenter; leftMargin: 10 }
                        text: "+  New mix"; color: newMixHover.containsMouse ? ink : dim; font.pixelSize: 13
                    }
                    MouseArea {
                        id: newMixHover
                        anchors.fill: parent; hoverEnabled: true
                        onClicked: {
                            const id = Library.createMix(nextMixName());
                            if (id >= 0) { page = "tracks"; Library.showMix(id); }
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
                    width: parent.width; height: 76; radius: 11; color: panel; border.color: line; border.width: 1
                    Column {
                        anchors { fill: parent; margins: 12 } spacing: 4
                        Text { text: "Local library"; color: ink; font.pixelSize: 12; font.bold: true }
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
            // header: back · title · counts
            Item {
                id: listhead
                anchors { top: parent.top; left: parent.left; right: parent.right; topMargin: 14; leftMargin: 24; rightMargin: 24 }
                height: 28
                readonly property bool detail: page === "tracks" && Library.viewKind !== "all"
                Row {
                    anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter }
                    spacing: 12
                    Rectangle {
                        visible: listhead.detail
                        anchors.verticalCenter: parent.verticalCenter
                        height: 24; width: backLabel.implicitWidth + 18; radius: 7
                        color: backHover.containsMouse ? "#1e1e25" : "transparent"; border.color: line2; border.width: 1
                        Text { id: backLabel; anchors.centerIn: parent; text: "← BACK"; color: ink; font.pixelSize: 10; font.family: mono }
                        MouseArea {
                            id: backHover
                            anchors.fill: parent; hoverEnabled: true
                            onClicked: {
                                const fromAlbum = Library.viewKind === "album";
                                Library.showAll();
                                if (fromAlbum && albumsFromGrid) page = "albums";
                            }
                        }
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - (listhead.detail ? backLabel.implicitWidth + 30 : 0)
                        textFormat: Text.RichText; elide: Text.ElideRight
                        text: "<font face='" + disp + "' size='4' color='" + ink + "'><b>"
                              + (page === "albums" ? "Albums" : Library.viewTitle.replace(/&/g, "&amp;").replace(/</g, "&lt;")) + "</b></font>"
                              + "<font face='" + mono + "' size='2' color='" + dim + "'>&nbsp;&nbsp;&nbsp;"
                              + (page === "albums"
                                 ? albumGrid.count + " albums"
                                 : (Library.viewSubtitle !== "" ? Library.viewSubtitle.replace(/&/g, "&amp;").replace(/</g, "&lt;") + " · " : "")
                                   + Library.shownCount.toLocaleString(Qt.locale(), "f", 0) + " shown"
                                   + (Library.viewKind === "all" ? " · " + Library.totalCount.toLocaleString(Qt.locale(), "f", 0) + " tracks" : ""))
                              + (Library.scanning ? " · scanning " + Library.scanProgress + "…" : "") + "</font>"
                    }
                }
            }
            Row {
                id: chips
                anchors { top: listhead.bottom; left: parent.left; right: parent.right; topMargin: 6; leftMargin: 24; rightMargin: 24 }
                height: 36; spacing: 8
                visible: page === "tracks"
                Repeater {
                    model: ["All", "FLAC", "MP3", "WAV", "Liked"]
                    Rectangle {
                        height: 30; width: chipLabel.implicitWidth + 26; radius: 15
                        color: Library.format === modelData ? ink : panel
                        border.color: Library.format === modelData ? ink : line2; border.width: 1
                        Text {
                            id: chipLabel
                            anchors.centerIn: parent
                            text: modelData === "Liked" ? "♥ Liked" : modelData
                            color: Library.format === modelData ? "#000" : "#cfccc2"
                            font.pixelSize: 12; font.bold: Library.format === modelData
                        }
                        MouseArea { anchors.fill: parent; onClicked: Library.format = modelData }
                    }
                }
                Item { width: 1; height: 1 }
                Rectangle {
                    height: 30; width: sortLabel.implicitWidth + 26; radius: 9
                    color: panel; border.color: line2; border.width: 1
                    Text { id: sortLabel; anchors.centerIn: parent; color: ink; font.pixelSize: 12; font.family: mono
                        text: Library.sortMode === "title" ? "Title A–Z" : Library.sortMode === "artist" ? "Artist A–Z"
                            : Library.sortMode === "dur" ? "Longest first"
                            : Library.viewKind === "album" ? "Track order" : Library.viewKind === "mix" ? "Mix order" : "Recently added" }
                    MouseArea { anchors.fill: parent; onClicked: {
                        const modes = ["def", "title", "artist", "dur"];
                        Library.sortMode = modes[(modes.indexOf(Library.sortMode) + 1) % modes.length];
                    } }
                }
                Rectangle {
                    visible: Library.shownCount > 0
                    height: 30; width: playAllLabel.implicitWidth + 26; radius: 15
                    color: playAllHover.containsMouse ? ink : acc
                    Text { id: playAllLabel; anchors.centerIn: parent; text: "▶ PLAY"; color: "#000"; font.pixelSize: 11; font.family: mono; font.bold: true }
                    MouseArea { id: playAllHover; anchors.fill: parent; hoverEnabled: true; onClicked: Player.playTracks(Library.visibleTrackIds(), 0) }
                }
            }
            // album strip (Overview only)
            ListView {
                id: albumStrip
                anchors { top: chips.bottom; left: parent.left; right: parent.right; topMargin: visible ? 10 : 0; leftMargin: 24; rightMargin: 24 }
                visible: page === "tracks" && !tracksOnly && Library.viewKind === "all" && count > 0
                height: visible ? 208 : 0
                orientation: ListView.Horizontal; spacing: 14
                clip: true; model: Library.albums
                delegate: AlbumCard { width: 150 }
            }
            // albums page
            GridView {
                id: albumGrid
                anchors { top: listhead.bottom; left: parent.left; right: parent.right; bottom: parent.bottom; topMargin: 14; leftMargin: 24; rightMargin: 12 }
                visible: page === "albums"
                clip: true; model: Library.albums
                readonly property int columns: Math.max(1, Math.floor(width / 170))
                cellWidth: width / columns; cellHeight: 230
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
            // track header
            Item {
                id: thead
                anchors { top: albumStrip.bottom; left: parent.left; right: parent.right; topMargin: 8; leftMargin: 24; rightMargin: 24 }
                height: 30
                visible: page === "tracks"
                Rectangle { anchors { left: parent.left; right: parent.right; top: parent.top } height: 1; color: line }
                Rectangle { anchors { left: parent.left; right: parent.right; bottom: parent.bottom } height: 1; color: line }
                Text { x: 12; anchors.verticalCenter: parent.verticalCenter; text: "#"; color: dim; font.pixelSize: 10; font.family: mono }
                Text { x: 44; anchors.verticalCenter: parent.verticalCenter; text: "TITLE"; color: dim; font.pixelSize: 10; font.family: mono }
                Text { x: parent.width - 320; anchors.verticalCenter: parent.verticalCenter; text: "ALBUM"; color: dim; font.pixelSize: 10; font.family: mono; visible: !mid }
                Text { anchors { right: parent.right; verticalCenter: parent.verticalCenter; rightMargin: 48 } text: "TIME"; color: dim; font.pixelSize: 10; font.family: mono }
            }
            // track list
            ListView {
                id: trackList
                anchors { top: thead.bottom; left: parent.left; right: parent.right; bottom: parent.bottom; leftMargin: 24; rightMargin: 24 }
                visible: page === "tracks"
                clip: true; model: Library.tracks; spacing: 0
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                // Drag-reordering only makes sense on the unfiltered mix in its own order.
                readonly property bool reorderable: Library.viewKind === "mix" && Library.sortMode === "def"
                                                    && Library.query === "" && Library.format === "All"
                property int dragFrom: -1
                property int dropAt: -1
                delegate: Rectangle {
                    id: trackRow
                    width: trackList.width; height: 56; radius: 9
                    readonly property bool isCur: ti === Player.currentId
                    readonly property bool hovered: rowHover.containsMouse || quickHover.hovered || handleArea.containsMouse
                    color: isCur ? "#151a0e" : hovered ? "#141419" : "transparent"
                    border.color: trackList.dropAt === index && trackList.dragFrom !== index ? acc : "transparent"
                    border.width: 1
                    opacity: trackList.dragFrom === index ? 0.5 : 1
                    Row {
                        anchors { fill: parent; leftMargin: 12; rightMargin: 12 }
                        spacing: 10
                        Text { anchors.verticalCenter: parent.verticalCenter; width: 22
                            text: trackList.reorderable && (trackRow.hovered || handleArea.pressed) ? "⋮⋮"
                                : isCur ? (Player.playing ? "‖" : "▶") : String(index + 1).padStart(2, "0")
                            color: isCur ? acc : trackList.reorderable && trackRow.hovered ? ink : dim
                            font.pixelSize: 12; font.family: mono }
                        Column {
                            anchors.verticalCenter: parent.verticalCenter
                            width: mid ? parent.width - 170 : parent.width - 340
                            Text { width: parent.width; text: t; color: ink; font.pixelSize: 13; font.family: disp; elide: Text.ElideRight }
                            Text { width: parent.width; text: a; color: mut; font.pixelSize: 12; elide: Text.ElideRight }
                        }
                        Text { anchors.verticalCenter: parent.verticalCenter; width: 160; text: al; color: mut; font.pixelSize: 12; elide: Text.ElideRight; visible: !mid }
                        Text { anchors.verticalCenter: parent.verticalCenter; width: 52; text: d; color: mut; font.pixelSize: 12; font.family: mono; horizontalAlignment: Text.AlignRight }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter; width: 24; text: liked ? "♥" : "♡"
                            color: liked ? acc : dim; font.pixelSize: 15; horizontalAlignment: Text.AlignHCenter
                            MouseArea { anchors.fill: parent; onClicked: Library.toggleLike(ti) }
                        }
                    }
                    MouseArea {
                        id: rowHover
                        anchors.fill: parent; hoverEnabled: true; acceptedButtons: Qt.LeftButton
                        onClicked: Player.playTracks(Library.visibleTrackIds(), index)
                    }
                    // mix reorder handle (over the number column)
                    MouseArea {
                        id: handleArea
                        x: 6; width: 34; height: parent.height
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
                    // hover quick actions (above rowHover so they get the clicks)
                    Row {
                        anchors { right: parent.right; verticalCenter: parent.verticalCenter; rightMargin: 44 }
                        spacing: 6; visible: trackRow.hovered && trackList.dragFrom < 0
                        HoverHandler { id: quickHover }
                        Repeater {
                            model: Library.viewKind === "mix"
                                   ? [{t: "NEXT UP", a: "next"}, {t: "+ QUEUE", a: "end"}, {t: "+ MIX", a: "mix"}, {t: "REMOVE", a: "remove"}]
                                   : [{t: "NEXT UP", a: "next"}, {t: "+ QUEUE", a: "end"}, {t: "+ MIX", a: "mix"}]
                            Rectangle {
                                height: 24; width: 64; radius: 7; color: "#22222a"; border.color: line2; border.width: 1
                                Text { anchors.centerIn: parent; text: modelData.t; color: modelData.a === "remove" ? "#ff8a7a" : ink; font.pixelSize: 10; font.family: mono }
                                MouseArea { anchors.fill: parent; onClicked: {
                                    if (modelData.a === "next") Player.playNext(ti);
                                    else if (modelData.a === "end") Player.enqueue(ti);
                                    else if (modelData.a === "mix") { mixMenu.trackId = ti; mixMenu.popup(); }
                                    else Library.removeFromMix(Library.viewMixId, slot);
                                } }
                            }
                        }
                    }
                }
                // empty states
                Column {
                    anchors { top: parent.top; left: parent.left; right: parent.right; topMargin: 40 }
                    spacing: 8
                    visible: trackList.count === 0
                    readonly property bool emptyMix: Library.viewKind === "mix" && Library.query === "" && Library.format === "All"
                    Text {
                        width: parent.width; horizontalAlignment: Text.AlignHCenter
                        color: ink; font.pixelSize: 15; font.family: disp
                        text: parent.emptyMix ? "This mix is empty"
                            : Library.totalCount > 0 ? "Nothing matches"
                            : Library.scanning ? "Scanning your music…" : "No music found"
                    }
                    Text {
                        width: parent.width; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.WordWrap
                        color: mut; font.pixelSize: 12
                        text: parent.emptyMix ? "Hover any track and use + MIX to add it here."
                            : Library.totalCount > 0 ? "Try another search or filter."
                            : Library.error !== "" ? Library.error
                            : "Flow reads FLAC, MP3 and WAV from " + Library.musicDir
                              + ".\nStart it with --library=/path/to/music to use another folder."
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
            color: panel; border.color: line; border.width: 1
            Row {
                id: tabs
                anchors { top: parent.top; left: parent.left; right: parent.right; topMargin: 12; leftMargin: 12; rightMargin: 12 }
                height: 34; spacing: 4
                Repeater {
                    model: [{t: "NOW", k: "now"}, {t: "QUEUE", k: "queue"}, {t: "LYRICS", k: "lyrics"}]
                    Rectangle {
                        width: (tabs.width - 8) / 3; height: 34
                        radius: 9; color: drawerTab === modelData.k ? panel2 : "transparent"
                        border.color: drawerTab === modelData.k ? line : "transparent"; border.width: 1
                        Text { anchors.centerIn: parent; color: drawerTab === modelData.k ? ink : dim; font.pixelSize: 10; font.family: mono
                            text: modelData.k === "queue" && Player.queueCount > 0 ? modelData.t + " · " + Player.queueCount : modelData.t }
                        MouseArea { anchors.fill: parent; onClicked: drawerTab = modelData.k }
                    }
                }
            }
            // NOW tab
            Flickable {
                anchors { top: tabs.bottom; left: parent.left; right: parent.right; bottom: parent.bottom; topMargin: 14; leftMargin: 14; rightMargin: 14; bottomMargin: 14 }
                contentHeight: nowCol.height; clip: true
                visible: drawerTab === "now"
                Column {
                    id: nowCol
                    width: parent.width; spacing: 12
                    Art {
                        width: parent.width; height: parent.width; radius: 12
                        source: cur.cover; c1: cur.c1; c2: cur.c2
                        decodeSize: 640
                    }
                    Text { width: parent.width; text: Player.hasTrack ? cur.t : "Nothing playing"; color: ink; font.pixelSize: 19; font.family: disp; wrapMode: Text.WordWrap }
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
                                Text { id: specLabel; anchors.centerIn: parent; text: modelData; font.pixelSize: 10; font.family: mono;
                                    color: index === 0 && cur.hi ? "#000" : "#cfccc2"; font.bold: index === 0 && cur.hi }
                            }
                        }
                    }
                    Text {
                        width: parent.width; visible: Player.error !== ""; text: Player.error
                        color: "#ff8a7a"; font.pixelSize: 11; wrapMode: Text.WordWrap
                    }
                    Text { text: "● CLICK TO SEEK    " + fmtTime(Player.position) + " / " + fmtTime(Player.duration); color: dim; font.pixelSize: 10; font.family: mono }
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
                            width: parent.width; height: 48; radius: 9; color: "transparent"; border.color: line; border.width: 1
                            Row {
                                anchors { fill: parent; leftMargin: 8; rightMargin: 10 }
                                spacing: 9
                                Art {
                                    anchors.verticalCenter: parent.verticalCenter; width: 30; height: 30; radius: 6
                                    source: upRow.k.cover; c1: upRow.k.c1; c2: upRow.k.c2; accent: false; decodeSize: 64
                                }
                                Column {
                                    anchors.verticalCenter: parent.verticalCenter; width: parent.width - 100
                                    Text { width: parent.width; text: upRow.k.t; color: ink; font.pixelSize: 12; font.family: disp; elide: Text.ElideRight }
                                    Text { width: parent.width; text: upRow.k.a; color: mut; font.pixelSize: 11; elide: Text.ElideRight }
                                }
                                Text { anchors.verticalCenter: parent.verticalCenter; text: upRow.k.d; color: dim; font.pixelSize: 11; font.family: mono }
                            }
                            MouseArea { anchors.fill: parent; onClicked: Player.jump(upRow.row) }
                        }
                    }
                }
            }
            // QUEUE tab: click to play, drag the handle to reorder, ✕ to remove
            ListView {
                id: queueList
                anchors { top: tabs.bottom; left: parent.left; right: parent.right; bottom: parent.bottom; topMargin: 14; leftMargin: 14; rightMargin: 14; bottomMargin: 14 }
                clip: true; spacing: 8; model: Player.queue
                visible: drawerTab === "queue"
                property int dragFrom: -1
                property int dropAt: -1
                delegate: Column {
                    id: qRow
                    width: ListView.view.width; spacing: 0
                    readonly property bool isCur: index === Player.queueIndex
                    Text { text: div; color: mut; font.pixelSize: 11; font.bold: true; visible: div !== ""; bottomPadding: 4; width: parent.width; elide: Text.ElideRight }
                    Rectangle {
                        width: parent.width; height: 46; radius: 9
                        color: isCur ? "#151a0e" : panel2
                        border.color: queueList.dropAt === index && queueList.dragFrom !== index ? acc : line; border.width: 1
                        opacity: queueList.dragFrom === index ? 0.5 : 1
                        MouseArea { id: qHover; anchors.fill: parent; hoverEnabled: true; onClicked: Player.jump(index) }
                        Row {
                            anchors { fill: parent; leftMargin: 10; rightMargin: 6 }
                            spacing: 9
                            Text { anchors.verticalCenter: parent.verticalCenter; width: 18; text: isCur ? "▶" : String(index + 1).padStart(2, "0"); color: isCur ? acc : dim; font.pixelSize: 11; font.family: mono }
                            Column {
                                anchors.verticalCenter: parent.verticalCenter; width: parent.width - 132
                                Text { width: parent.width; text: t; color: ink; font.pixelSize: 12; font.family: disp; elide: Text.ElideRight }
                                Text { width: parent.width; text: a + " · " + f; color: mut; font.pixelSize: 11; elide: Text.ElideRight }
                            }
                            Text { anchors.verticalCenter: parent.verticalCenter; width: 36; text: d; color: dim; font.pixelSize: 11; font.family: mono; horizontalAlignment: Text.AlignRight }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter; width: 18; text: "✕"
                                color: removeArea.containsMouse ? ink : dim; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter
                                opacity: qHover.containsMouse || removeArea.containsMouse ? 1 : 0
                                MouseArea { id: removeArea; anchors.fill: parent; anchors.margins: -6; hoverEnabled: true; onClicked: Player.removeAt(index) }
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter; width: 18; text: "⋮⋮"
                                color: dragArea.containsMouse || dragArea.pressed ? ink : dim; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter
                                MouseArea {
                                    id: dragArea
                                    anchors.fill: parent; anchors.margins: -6; hoverEnabled: true
                                    cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
                                    preventStealing: true
                                    onPressed: { queueList.dragFrom = index; queueList.dropAt = index; }
                                    onPositionChanged: mouse => {
                                        if (!pressed) return;
                                        const p = mapToItem(queueList.contentItem, mouse.x, mouse.y);
                                        const at = queueList.indexAt(queueList.width / 2, p.y);
                                        if (at >= 0) queueList.dropAt = at;
                                        else if (p.y < 0) queueList.dropAt = 0;
                                        else queueList.dropAt = queueList.count - 1;
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
                }
                Text {
                    anchors { top: parent.top; left: parent.left; right: parent.right; topMargin: 24 }
                    visible: queueList.count === 0; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.WordWrap
                    text: "Queue is empty.\nUse NEXT UP or + QUEUE on a track."; color: mut; font.pixelSize: 12
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
                    width: ListView.view.width; text: modelData.text; font.pixelSize: 14
                    wrapMode: Text.WordWrap; padding: 6
                    color: !Player.lyricsTimed ? "#cfccc2"
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

    // ================= dock =================
    Rectangle {
        id: dock
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        height: 86; color: "#121218f0"
        Rectangle { anchors { left: parent.left; right: parent.right; top: parent.top } height: 1; color: line }
        // seek
        Item {
            id: seek
            anchors { top: parent.top; left: parent.left; right: parent.right; topMargin: 8; leftMargin: 16; rightMargin: 16 }
            height: 14
            Rectangle { anchors.verticalCenter: parent.verticalCenter; width: parent.width; height: 5; radius: 2.5; color: "#232329" }
            Rectangle { anchors.verticalCenter: parent.verticalCenter; width: parent.width * progress; height: 5; radius: 2.5; color: acc }
            MouseArea { anchors.fill: parent; onClicked: mouse => Player.seek(Player.duration * (mouse.x / width)) }
        }
        // left: track
        Row {
            anchors { left: parent.left; verticalCenter: parent.verticalCenter; leftMargin: 16; topMargin: 12 }
            spacing: 11
            Art {
                anchors.verticalCenter: parent.verticalCenter; width: 42; height: 42; radius: 8
                source: cur.cover; c1: cur.c1; c2: cur.c2; accent: false; decodeSize: 96
            }
            Column {
                anchors.verticalCenter: parent.verticalCenter
                Text { text: Player.hasTrack ? cur.t : "Nothing playing"; color: ink; font.pixelSize: 13; font.family: disp; elide: Text.ElideRight; width: Math.min(200, dock.width * 0.2) }
                Text { text: cur.a; color: mut; font.pixelSize: 12; visible: root.width > 640; elide: Text.ElideRight; width: Math.min(200, dock.width * 0.2) }
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter; text: cur.liked ? "♥" : "♡"
                color: cur.liked ? acc : dim; font.pixelSize: 15
                visible: Player.hasTrack
                MouseArea { anchors.fill: parent; onClicked: Library.toggleLike(Player.currentId) }
            }
        }
        // center: transport
        Row {
            anchors.centerIn: parent; anchors.verticalCenterOffset: 8
            spacing: 6
            Repeater {
                model: [
                    {t: "⇄", k: "sh"}, {t: "◀◀", k: "prev"}, {t: "PLAY", k: "play"},
                    {t: "▶▶", k: "next"}, {t: "↻", k: "rp"}, {t: "QUEUE", k: "q"}, {t: "LYR", k: "l"}
                ]
                Rectangle {
                    readonly property bool lit: (modelData.k === "sh" && Player.shuffle) || (modelData.k === "rp" && Player.repeat)
                    anchors.verticalCenter: parent.verticalCenter
                    width: modelData.k === "play" ? 44 : 36; height: modelData.k === "play" ? 44 : 36
                    radius: modelData.k === "play" ? 22 : 18
                    color: modelData.k === "play" ? (btnHover.containsMouse ? acc : ink)
                         : lit ? "#151a0e" : "transparent"
                    border.color: lit ? "#3d4420" : "transparent"
                    border.width: 1
                    Text {
                        anchors.centerIn: parent
                        text: modelData.k === "play" ? (Player.playing ? "❚❚" : "▶") : modelData.t
                        color: modelData.k === "play" ? "#000" : lit ? acc : "#d8d5cb"
                        font.pixelSize: modelData.k === "q" || modelData.k === "l" ? 10 : 14
                        font.family: mono
                    }
                    MouseArea {
                        id: btnHover
                        anchors.fill: parent; hoverEnabled: true
                        onClicked: {
                            if (modelData.k === "play") Player.toggle();
                            else if (modelData.k === "prev") Player.previous();
                            else if (modelData.k === "next") Player.next();
                            else if (modelData.k === "sh") Player.shuffle = !Player.shuffle;
                            else if (modelData.k === "rp") Player.repeat = !Player.repeat;
                            else if (modelData.k === "q") { drawerTab = "queue"; drawerOpen = true; }
                            else if (modelData.k === "l") { drawerTab = "lyrics"; drawerOpen = true; }
                        }
                    }
                }
            }
        }
        // right: time + volume
        Row {
            anchors { right: parent.right; verticalCenter: parent.verticalCenter; rightMargin: 16; topMargin: 12 }
            spacing: 10
            Text { anchors.verticalCenter: parent.verticalCenter; text: fmtTime(Player.position) + " / " + fmtTime(Player.duration); color: mut; font.pixelSize: 11; font.family: mono }
            Item {
                id: vol
                anchors.verticalCenter: parent.verticalCenter; width: 96; height: 14
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
