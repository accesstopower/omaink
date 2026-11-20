import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The tools, colors and sizes, shown while drawing.
Rectangle {
    id: bar
    signal helpRequested()

    width: row.implicitWidth + 16
    height: row.implicitHeight + 12
    radius: theme.radius
    color: "#ee18191b"
    border.color: "#30ffffff"
    Material.roundedScale: theme.radius

    // Clicks on the bar never reach the ink underneath.
    MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons }

    readonly property var tools: [
        { name: "pen", label: qsTr("Pen"), key: "P" },
        { name: "calligraphy", label: qsTr("Nib"), key: "C" },
        { name: "marker", label: qsTr("Marker"), key: "M" },
        { name: "highlighter", label: qsTr("Highlight"), key: "H" },
        { name: "laser", label: qsTr("Laser"), key: "L" },
        { name: "arrow", label: qsTr("Arrow"), key: "A" },
        { name: "rect", label: qsTr("Box"), key: "R" },
        { name: "ellipse", label: qsTr("Circle"), key: "O" },
        { name: "eraser", label: qsTr("Eraser"), key: "E" }
    ]

    Row {
        id: row
        x: 8
        y: 6
        spacing: 2

        Repeater {
            model: bar.tools
            delegate: ToolButton {
                required property var modelData
                text: modelData.label
                checkable: true
                checked: overlay.tool === modelData.name
                font.pixelSize: 13
                implicitHeight: 34
                padding: 8
                Material.roundedScale: Math.min(4, theme.radius)
                onClicked: overlay.tool = modelData.name
                ToolTip.visible: hovered
                ToolTip.delay: 500
                ToolTip.text: modelData.label + "  " + modelData.key
            }
        }

        Rectangle { width: 1; height: 34; color: "#30ffffff" }

        Repeater {
            model: overlay.palette
            delegate: Item {
                required property string modelData
                required property int index
                width: 28
                height: 34
                Rectangle {
                    anchors.centerIn: parent
                    width: overlay.color === modelData ? 22 : 18
                    height: width
                    radius: width / 2
                    color: modelData
                    border.width: overlay.color === modelData ? 2 : 1
                    border.color: overlay.color === modelData ? theme.accent : "#55ffffff"
                }
                MouseArea {
                    anchors.fill: parent
                    onClicked: overlay.color = modelData
                }
                ToolTip.visible: colorHover.hovered
                ToolTip.delay: 500
                ToolTip.text: String(index + 1)
                HoverHandler { id: colorHover }
            }
        }

        Rectangle { width: 1; height: 34; color: "#30ffffff" }

        Repeater {
            model: overlay.sizes
            delegate: Item {
                required property real modelData
                required property int index
                width: 26
                height: 34
                Rectangle {
                    anchors.centerIn: parent
                    width: 4 + modelData * 1.3
                    height: width
                    radius: width / 2
                    color: overlay.sizeIndex === index ? theme.accent : "#bdbdbd"
                }
                MouseArea {
                    anchors.fill: parent
                    onClicked: overlay.sizeIndex = index
                }
            }
        }

        ToolButton {
            visible: overlay.tool === "calligraphy"
            text: Number(overlay.nibAngle).toLocaleString(Qt.locale(), 'f', 0) + "°"
            font.pixelSize: 13
            implicitHeight: 34
            padding: 8
            Material.roundedScale: Math.min(4, theme.radius)
            onClicked: overlay.cycleNib()
            ToolTip.visible: hovered
            ToolTip.delay: 500
            ToolTip.text: qsTr("Nib angle") + "  N"
        }

        Rectangle { width: 1; height: 34; color: "#30ffffff" }

        Repeater {
            model: [
                { label: qsTr("Undo"), key: "Ctrl+Z", enabled: overlay.canUndo, run: function() { overlay.undo() } },
                { label: qsTr("Redo"), key: "Ctrl+Shift+Z", enabled: overlay.canRedo, run: function() { overlay.redo() } },
                { label: qsTr("Clear"), key: "Delete", enabled: !overlay.empty, run: function() { overlay.clear() } },
                { label: overlay.board === "" ? qsTr("Board") : overlay.board === "white" ? qsTr("Dark board") : qsTr("No board"),
                  key: "W", enabled: true, run: function() { overlay.cycleBoard() } },
                { label: "?", key: "?", enabled: true, run: function() { bar.helpRequested() } },
                { label: qsTr("Done"), key: "Esc", enabled: true, run: function() { overlay.setDrawing(false) } }
            ]
            delegate: ToolButton {
                required property var modelData
                text: modelData.label
                enabled: modelData.enabled
                font.pixelSize: 13
                implicitHeight: 34
                padding: 8
                Material.roundedScale: Math.min(4, theme.radius)
                onClicked: modelData.run()
                ToolTip.visible: hovered
                ToolTip.delay: 500
                ToolTip.text: modelData.key
            }
        }
    }
}
