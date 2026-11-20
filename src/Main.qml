import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import OmaInk

// The overlay: one transparent window over the whole screen. The C++ side
// puts it on the overlay layer and shows it, and lets clicks pass through it
// whenever you aren't drawing.
Window {
    id: win
    visible: false
    color: "transparent"
    title: "OmaInk"

    Material.theme: Material.Dark
    Material.accent: theme.accent
    Material.roundedScale: theme.radius

    property bool helpVisible: false
    property bool confirmingQuit: false
    property bool toolbarAtBottom: false
    readonly property bool keysLive: overlay.drawing && !confirmingQuit

    function quitOrAsk() {
        if (overlay.empty)
            Qt.quit()
        else
            win.confirmingQuit = true
    }

    function toast(message) {
        toastLabel.text = message
        toastBox.opacity = 1
        toastTimer.restart()
    }

    Connections {
        target: overlay
        function onNotify(message) { win.toast(message) }
        function onDrawingChanged() {
            if (overlay.drawing)
                win.toast(qsTr("Drawing. Esc to finish, ? for help"))
            else {
                win.helpVisible = false
                win.confirmingQuit = false
            }
        }
    }

    Shortcut { enabled: win.keysLive; sequence: "?"; context: Qt.ApplicationShortcut; onActivated: win.helpVisible = !win.helpVisible }
    Shortcut { enabled: win.keysLive; sequence: "Q"; context: Qt.ApplicationShortcut; onActivated: win.quitOrAsk() }
    Shortcut { enabled: win.keysLive; sequences: ["Space"]; context: Qt.ApplicationShortcut; onActivated: overlay.setDrawing(false) }
    Shortcut {
        enabled: overlay.drawing
        sequence: "Escape"
        context: Qt.ApplicationShortcut
        onActivated: {
            if (win.confirmingQuit) win.confirmingQuit = false
            else if (win.helpVisible) win.helpVisible = false
            else overlay.setDrawing(false)
        }
    }
    Shortcut { enabled: win.confirmingQuit; sequences: ["Return", "Enter", "Q"]; context: Qt.ApplicationShortcut; onActivated: Qt.quit() }

    Shortcut { enabled: win.keysLive; sequence: "P"; context: Qt.ApplicationShortcut; onActivated: overlay.tool = "pen" }
    Shortcut { enabled: win.keysLive; sequence: "C"; context: Qt.ApplicationShortcut; onActivated: overlay.tool = "calligraphy" }
    Shortcut { enabled: win.keysLive; sequence: "M"; context: Qt.ApplicationShortcut; onActivated: overlay.tool = "marker" }
    Shortcut { enabled: win.keysLive; sequence: "H"; context: Qt.ApplicationShortcut; onActivated: overlay.tool = "highlighter" }
    Shortcut { enabled: win.keysLive; sequence: "L"; context: Qt.ApplicationShortcut; onActivated: overlay.tool = "laser" }
    Shortcut { enabled: win.keysLive; sequence: "A"; context: Qt.ApplicationShortcut; onActivated: overlay.tool = "arrow" }
    Shortcut { enabled: win.keysLive; sequence: "R"; context: Qt.ApplicationShortcut; onActivated: overlay.tool = "rect" }
    Shortcut { enabled: win.keysLive; sequence: "O"; context: Qt.ApplicationShortcut; onActivated: overlay.tool = "ellipse" }
    Shortcut { enabled: win.keysLive; sequence: "E"; context: Qt.ApplicationShortcut; onActivated: overlay.tool = "eraser" }
    Shortcut { enabled: win.keysLive; sequence: "N"; context: Qt.ApplicationShortcut; onActivated: overlay.cycleNib() }
    Shortcut { enabled: win.keysLive; sequence: "W"; context: Qt.ApplicationShortcut; onActivated: overlay.cycleBoard() }
    Shortcut { enabled: win.keysLive; sequence: "T"; context: Qt.ApplicationShortcut; onActivated: win.toolbarAtBottom = !win.toolbarAtBottom }
    Shortcut { enabled: win.keysLive; sequence: "["; context: Qt.ApplicationShortcut; onActivated: overlay.stepSize(-1) }
    Shortcut { enabled: win.keysLive; sequence: "]"; context: Qt.ApplicationShortcut; onActivated: overlay.stepSize(1) }
    Shortcut { enabled: win.keysLive; sequences: [StandardKey.Undo]; context: Qt.ApplicationShortcut; onActivated: overlay.undo() }
    Shortcut { enabled: win.keysLive; sequences: ["Ctrl+Shift+Z", "Ctrl+Y"]; context: Qt.ApplicationShortcut; onActivated: overlay.redo() }
    Shortcut { enabled: win.keysLive; sequences: ["Delete", "Backspace"]; context: Qt.ApplicationShortcut; onActivated: overlay.clear() }
    Repeater {
        model: overlay.palette.length
        delegate: Item {
            required property int index
            Shortcut {
                enabled: win.keysLive
                sequence: String(index + 1)
                context: Qt.ApplicationShortcut
                onActivated: overlay.color = overlay.palette[index]
            }
        }
    }

    // Right-to-left languages mirror the window; the Translator sets the
    // application's layout direction.
    Item {
        anchors.fill: parent
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true

        InkCanvas {
            anchors.fill: parent
            controller: overlay
            visible: overlay.inkVisible
        }

        // A faint frame says clicks are going to the ink, not the apps.
        Rectangle {
            anchors.fill: parent
            visible: overlay.drawing
            color: "transparent"
            border.width: 2
            border.color: theme.accent
            opacity: 0.55
        }

        InkToolbar {
            visible: overlay.drawing
            anchors.horizontalCenter: parent.horizontalCenter
            y: win.toolbarAtBottom ? parent.height - height - 18 : 18
            // One row of tools; on a narrow screen it shrinks rather than wraps.
            scale: Math.min(1, (parent.width - 32) / width)
            transformOrigin: win.toolbarAtBottom ? Item.Bottom : Item.Top
            onHelpRequested: win.helpVisible = !win.helpVisible
        }

        HelpCard {
            visible: win.helpVisible && overlay.drawing
            anchors.centerIn: parent
            width: Math.min(820, parent.width - 48)
            height: Math.min(implicitHeight, parent.height - 48)
            onCloseRequested: win.helpVisible = false
        }

        Rectangle {
            id: toastBox
            anchors.horizontalCenter: parent.horizontalCenter
            y: win.toolbarAtBottom ? 24 : parent.height - height - 32
            width: toastLabel.implicitWidth + 32
            height: toastLabel.implicitHeight + 18
            radius: theme.radius
            color: "#e6161618"
            opacity: 0
            visible: opacity > 0
            Behavior on opacity { NumberAnimation { duration: 180 } }
            Label {
                id: toastLabel
                anchors.centerIn: parent
                color: "#f2f2f2"
                font.pixelSize: 14
            }
            Timer {
                id: toastTimer
                interval: 2200
                onTriggered: toastBox.opacity = 0
            }
        }

        // Quitting throws the ink away, so it asks first when there is some.
        Rectangle {
            anchors.fill: parent
            visible: win.confirmingQuit
            color: "#99000000"
            MouseArea { anchors.fill: parent; onClicked: win.confirmingQuit = false }

            Rectangle {
                anchors.centerIn: parent
                width: Math.min(420, parent.width - 48)
                height: confirmColumn.implicitHeight + 40
                radius: theme.radius
                color: "#1d1e21"
                border.color: "#33ffffff"

                ColumnLayout {
                    id: confirmColumn
                    anchors.fill: parent
                    anchors.margins: 20
                    spacing: 14
                    Label {
                        text: qsTr("Quit OmaInk?")
                        font.pixelSize: 19
                        font.bold: true
                        color: "#f2f2f2"
                    }
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("The ink on screen will be cleared.")
                        wrapMode: Text.WordWrap
                        color: "#c8c8c8"
                    }
                    RowLayout {
                        Layout.alignment: Qt.AlignRight
                        spacing: 8
                        Button {
                            text: qsTr("Cancel")
                            flat: true
                            onClicked: win.confirmingQuit = false
                        }
                        Button {
                            text: qsTr("Quit")
                            highlighted: true
                            onClicked: Qt.quit()
                        }
                    }
                }
            }
        }
    }
}
