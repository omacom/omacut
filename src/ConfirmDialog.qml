import QtQuick
import QtQuick.Controls

// A question over the dimmed window, answered from the keyboard or the mouse.
// Focus starts on initialFocus; the arrows and Tab move it between the
// buttons, wrapping around; Return, Enter or Space presses the focused one;
// Alt+ a button's underlined letter presses that one; a click outside
// dismisses it, as does Escape, which the owner handles. The owner shows it,
// and hides it on chosen or dismissed.
Rectangle {
    id: dialog
    anchors.fill: parent
    color: "#cc000000"
    // Right-to-left languages read the buttons, and the arrows, the other way.
    LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
    LayoutMirroring.childrenInherit: true
    readonly property int forward: LayoutMirroring.enabled ? -1 : 1

    property string title: ""
    property string message: ""
    // Each {text, mnemonic, primary}.
    property var buttons: []
    property int initialFocus: 0
    signal chosen(int index)
    signal dismissed()

    // Wraps around: index runs from -1 to the button count.
    function focusButton(index) {
        var count = buttonRepeater.count
        buttonRepeater.itemAt((index + count) % count).forceActiveFocus()
    }
    onVisibleChanged: if (visible) focusButton(initialFocus)

    MouseArea {
        anchors.fill: parent
        onClicked: dialog.dismissed()
    }

    DialogPanel {
        anchors.centerIn: parent
        width: column.width + 64
        height: column.height + 48

        // Clicks on the panel stay on it.
        MouseArea { anchors.fill: parent }

        Column {
            id: column
            anchors.centerIn: parent
            spacing: 8

            Label {
                text: dialog.title
                color: "white"
                font.pixelSize: 16
                font.weight: Font.DemiBold
            }
            Label {
                width: Math.min(implicitWidth, dialog.width - 160)
                text: dialog.message
                color: "#d6d6da"
                font.pixelSize: 13
                wrapMode: Text.WordWrap
                bottomPadding: 12
            }
            Row {
                anchors.right: parent.right
                spacing: 10
                Repeater {
                    id: buttonRepeater
                    model: dialog.buttons
                    DialogButton {
                        required property var modelData
                        required property int index
                        text: modelData.text
                        mnemonic: modelData.mnemonic || ""
                        primary: modelData.primary === true
                        onClicked: dialog.chosen(index)
                        Keys.onLeftPressed: dialog.focusButton(index - dialog.forward)
                        Keys.onRightPressed: dialog.focusButton(index + dialog.forward)
                        Keys.onBacktabPressed: dialog.focusButton(index - 1)
                        Keys.onTabPressed: dialog.focusButton(index + 1)
                    }
                }
            }
        }
    }
}
