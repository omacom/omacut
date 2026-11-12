import QtQuick
import QtQuick.Controls

// A dialog button the way Omarchy's apps draw them (Omawrite's square dialog
// buttons, as Plain uses them): the window's corners, capped small, a bright
// border on the one with focus, and the accent on the primary one. Its
// mnemonic letter is underlined, and Alt+ that letter presses it.
Button {
    id: control
    property bool primary: false
    property string mnemonic: ""

    leftPadding: 16
    rightPadding: 16
    topPadding: 8
    bottomPadding: 8
    Keys.onReturnPressed: clicked()
    Keys.onEnterPressed: clicked()

    // The text with its mnemonic underlined, the first match either case.
    readonly property string formattedText: {
        var at = mnemonic.length ? text.toLowerCase().indexOf(mnemonic.toLowerCase()) : -1
        return at < 0 ? text : text.slice(0, at) + "<u>" + text.charAt(at) + "</u>" + text.slice(at + 1)
    }

    contentItem: Label {
        text: control.formattedText
        textFormat: Text.StyledText
        color: control.primary ? theme.accentForeground : "white"
        font.pixelSize: 13
        font.weight: Font.DemiBold
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }
    background: Rectangle {
        implicitWidth: 88
        implicitHeight: 34
        radius: Math.min(4, theme.radius)
        color: control.primary
            ? Qt.darker(theme.accent, control.down ? 1.2 : control.hovered ? 1.08 : 1)
            : control.down ? "#3c3c40" : control.hovered ? "#35353a" : "#2c2c2f"
        border.width: control.activeFocus ? 2 : 1
        border.color: control.activeFocus ? "white"
            : control.primary ? Qt.darker(theme.accent, 1.15) : "#3a3a3e"
    }
    HoverHandler { cursorShape: Qt.PointingHandCursor }
    Shortcut {
        sequence: "Alt+" + control.mnemonic
        enabled: control.visible && control.mnemonic !== ""
        context: Qt.ApplicationShortcut
        onActivated: control.clicked()
    }
}
