import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The logo's controls: the image, how big and how opaque it is, and where it
// sits. It can also be dragged in the preview. The preview draws it live.
DialogPanel {
    id: panel
    objectName: "logoPanel"

    readonly property var logo: backend.logo
    // The size of the picture in the preview, which corners are worked out in.
    property real frameWidth: 0
    property real frameHeight: 0
    // Where the playhead is, seconds along the edit, for starting or ending a logo there.
    property real playhead: 0
    signal changeRequested()
    signal addRequested()
    signal closeRequested()

    implicitHeight: body.implicitHeight + 24

    component Chip: Rectangle {
        id: chip
        property string label: ""
        signal clicked()
        implicitWidth: chipLabel.implicitWidth + 22
        implicitHeight: 28
        radius: Math.min(4, theme.radius)
        color: chipHover.hovered ? "#35353a" : "#2c2c2f"
        border.width: 1
        border.color: "#3a3a3e"
        Label {
            id: chipLabel
            anchors.centerIn: parent
            text: chip.label
            textFormat: Text.PlainText
            color: "white"
            font.pixelSize: 13
            font.weight: Font.DemiBold
        }
        HoverHandler { id: chipHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: chip.clicked() }
    }

    component Caption: Label {
        color: "#9a9aa0"
        font.pixelSize: 12
        Layout.alignment: Qt.AlignVCenter
    }

    // A slider with its value in words beside it.
    component Setting: RowLayout {
        id: setting
        property string label: ""
        property alias from: slider.from
        property alias to: slider.to
        property real value: 0
        // How the value is written beside the slider: scaled, to some digits, with a unit.
        property real factor: 100
        property int digits: 0
        property string suffix: "%"
        signal moved(real value)
        spacing: 10
        Caption { text: setting.label; Layout.preferredWidth: 64 }
        Slider {
            id: slider
            Layout.fillWidth: true
            Layout.minimumWidth: 120
            value: setting.value
            focusPolicy: Qt.NoFocus
            Material.accent: theme.accent
            onMoved: setting.moved(value)
            onPressedChanged: pressed ? panel.logo.beginGesture() : panel.logo.endGesture()
        }
        Label {
            text: (slider.value * setting.factor).toFixed(setting.digits) + setting.suffix
            color: "#d6d6da"
            font.pixelSize: 13
            font.family: "monospace"
            Layout.preferredWidth: 40
            horizontalAlignment: Text.AlignRight
        }
    }

    ColumnLayout {
        id: body
        anchors.fill: parent
        anchors.margins: 12
        spacing: 8

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            // The image, on a checkerboard so transparent parts show.
            Rectangle {
                Layout.preferredWidth: 72
                Layout.preferredHeight: 44
                radius: Math.min(4, theme.radius)
                color: "#2c2c2f"
                clip: true
                Image {
                    anchors.fill: parent
                    anchors.margins: 4
                    source: panel.logo.source
                    fillMode: Image.PreserveAspectFit
                    sourceSize.height: 96
                    asynchronous: true
                    smooth: true
                }
            }
            Chip {
                label: qsTr("Change image")
                onClicked: panel.changeRequested()
            }
            Chip {
                label: qsTr("Add image")
                onClicked: panel.addRequested()
            }
            Chip {
                label: qsTr("Remove logo")
                onClicked: panel.logo.remove()
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("Drag the logo in the preview to place it, and its strip to time it")
                color: "#9a9aa0"
                font.pixelSize: 12
                elide: Text.ElideRight
            }
            Chip {
                label: "×"
                implicitWidth: 28
                onClicked: panel.closeRequested()
            }
        }

        Flow {
            Layout.fillWidth: true
            spacing: 6
            Caption { text: qsTr("Place in"); height: 28; verticalAlignment: Text.AlignVCenter; rightPadding: 4 }
            Chip { label: qsTr("Top left"); onClicked: panel.logo.anchor(-1, -1, panel.frameWidth, panel.frameHeight) }
            Chip { label: qsTr("Top right"); onClicked: panel.logo.anchor(1, -1, panel.frameWidth, panel.frameHeight) }
            Chip { label: qsTr("Bottom left"); onClicked: panel.logo.anchor(-1, 1, panel.frameWidth, panel.frameHeight) }
            Chip { label: qsTr("Bottom right"); onClicked: panel.logo.anchor(1, 1, panel.frameWidth, panel.frameHeight) }
            Chip { label: qsTr("Center"); onClicked: panel.logo.anchor(0, 0, panel.frameWidth, panel.frameHeight) }
        }

        Flow {
            Layout.fillWidth: true
            spacing: 6
            Caption { text: qsTr("Shown"); height: 28; verticalAlignment: Text.AlignVCenter; rightPadding: 4 }
            Chip { label: qsTr("From the playhead"); onClicked: panel.logo.setStart(panel.playhead) }
            Chip { label: qsTr("Until the playhead"); onClicked: panel.logo.setEnd(panel.playhead) }
            Chip {
                label: qsTr("The whole video")
                onClicked: { panel.logo.setStart(0); panel.logo.setEnd(1e9); }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 24
            Setting {
                Layout.fillWidth: true
                label: qsTr("Fade in")
                from: 0
                to: 5
                factor: 1
                digits: 1
                suffix: " s"
                value: panel.logo.fadeIn
                onMoved: (value) => panel.logo.setFadeIn(value)
            }
            Setting {
                Layout.fillWidth: true
                label: qsTr("Fade out")
                from: 0
                to: 5
                factor: 1
                digits: 1
                suffix: " s"
                value: panel.logo.fadeOut
                onMoved: (value) => panel.logo.setFadeOut(value)
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 24
            Setting {
                Layout.fillWidth: true
                label: qsTr("Size")
                from: 0.02
                to: 0.6
                value: panel.logo.size
                onMoved: (value) => panel.logo.setSize(value)
            }
            Setting {
                Layout.fillWidth: true
                label: qsTr("Opacity")
                from: 0.05
                to: 1
                value: panel.logo.opacity
                onMoved: (value) => panel.logo.setOpacity(value)
            }
        }
    }
}
