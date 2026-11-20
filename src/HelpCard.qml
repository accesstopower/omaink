import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Every shortcut, and how to draw from a tablet.
Rectangle {
    id: card
    signal closeRequested()

    implicitHeight: content.implicitHeight + 48
    radius: theme.radius
    color: "#f21a1b1e"
    border.color: "#30ffffff"
    Material.roundedScale: theme.radius

    MouseArea { anchors.fill: parent; onClicked: card.closeRequested() }

    readonly property var shortcuts: [
        ["P  C  M  H", qsTr("Pen, nib, marker, highlighter")],
        ["L", qsTr("Laser pointer that fades")],
        ["A  R  O", qsTr("Arrow, box, circle")],
        ["E", qsTr("Eraser (right-click erases with any tool)")],
        ["1 – 8", qsTr("Colors")],
        ["[  ]", qsTr("Thinner, thicker")],
        ["N", qsTr("Nib angle")],
        ["W", qsTr("White board, dark board, none")],
        [qsTr("Ctrl+Z"), qsTr("Undo")],
        [qsTr("Ctrl+Shift+Z"), qsTr("Redo")],
        [qsTr("Delete"), qsTr("Clear (undo brings it back)")],
        ["T", qsTr("Move the toolbar to the other edge")],
        [qsTr("Esc  Space"), qsTr("Stop drawing; clicks go to your apps again")],
        ["Q", qsTr("Quit")]
    ]

    RowLayout {
        id: content
        anchors.fill: parent
        anchors.margins: 24
        spacing: 32

        ColumnLayout {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignTop
            spacing: 10

            Label {
                text: "OmaInk"
                font.pixelSize: 24
                font.bold: true
                color: "#f2f2f2"
            }
            GridLayout {
                columns: 2
                columnSpacing: 18
                rowSpacing: 6
                Repeater {
                    model: card.shortcuts.length * 2
                    delegate: Label {
                        required property int index
                        readonly property var row: card.shortcuts[Math.floor(index / 2)]
                        text: row[index % 2]
                        color: index % 2 === 0 ? theme.accent : "#d6d6d6"
                        font.family: index % 2 === 0 ? "monospace" : Qt.application.font.family
                        font.pixelSize: 14
                        Layout.fillWidth: index % 2 === 1
                        wrapMode: Text.WordWrap
                    }
                }
            }
        }

        ColumnLayout {
            Layout.preferredWidth: 250
            Layout.alignment: Qt.AlignTop
            spacing: 10

            Label {
                text: qsTr("Draw from a tablet")
                font.pixelSize: 17
                font.bold: true
                color: "#f2f2f2"
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: "#c8c8c8"
                font.pixelSize: 13
                text: overlay.padUrls.length > 0
                      ? qsTr("Scan this on an iPad or tablet on the same Wi-Fi, or open the address in its browser.")
                      : overlay.padError !== "" ? overlay.padError
                      : qsTr("The tablet pad is turned off.")
            }
            Rectangle {
                visible: overlay.qrSource !== "" && overlay.padUrls.length > 0
                Layout.preferredWidth: 200
                Layout.preferredHeight: 200
                Layout.alignment: Qt.AlignHCenter
                color: "white"
                radius: Math.min(4, theme.radius)
                Image {
                    anchors.fill: parent
                    anchors.margins: 6
                    source: overlay.qrSource
                    smooth: false
                    fillMode: Image.PreserveAspectFit
                }
            }
            Label {
                visible: overlay.padUrls.length > 0
                Layout.fillWidth: true
                text: overlay.padUrls.length > 0 ? overlay.padUrls[0] : ""
                wrapMode: Text.WrapAnywhere
                color: "#f2f2f2"
                font.family: "monospace"
                font.pixelSize: 12
            }
            Label {
                visible: overlay.padUrls.length > 0
                color: overlay.padClients > 0 ? theme.accent : "#9a9a9a"
                font.pixelSize: 13
                text: overlay.padClients > 0 ? qsTr("%n tablets connected", "", overlay.padClients)
                                             : qsTr("No tablet connected yet")
            }
        }
    }
}
