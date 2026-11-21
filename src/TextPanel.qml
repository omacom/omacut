import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import "Format.js" as Format

// Where the edit's texts are written and styled. Each is a chip to pick; the
// picked one has its words, when it shows, how it comes and goes, and where
// and how big it is. Texts sit on their own track along the edit, so
// cutting or moving clips doesn't move them. The preview draws them live.
DialogPanel {
    id: panel
    objectName: "textPanel"

    readonly property var timeline: backend.timeline
    readonly property var texts: timeline.texts
    // The picked text, an index into texts, or -1.
    property int current: -1
    readonly property var picked: current >= 0 && current < texts.length ? texts[current] : null
    // The playhead, for setting a text's start or end to it.
    property real playheadSec: 0
    // Typing here must not also run the editor's single-key shortcuts.
    readonly property bool fieldFocused: words.activeFocus || startField.activeFocus || endField.activeFocus
        || sizeField.activeFocus || fontBox.activeFocus || fontBox.popup.visible
    // Ask the window to show the frame at a time, so a picked text is on screen.
    signal seek(real seconds)
    signal closeRequested()

    readonly property var effectChoices: [
        { value: "none", label: qsTr("None") },
        { value: "fade", label: qsTr("Fade") },
        { value: "slide", label: qsTr("Slide up") },
        { value: "pop", label: qsTr("Pop") }
    ]
    readonly property var positionChoices: [
        { value: 0.06, label: qsTr("Top") },
        { value: 0.5, label: qsTr("Middle") },
        { value: 0.94, label: qsTr("Bottom") }
    ]
    readonly property var sizeChoices: [
        { value: 0.05, label: qsTr("Small") },
        { value: 0.08, label: qsTr("Medium") },
        { value: 0.12, label: qsTr("Large") }
    ]
    // Every installed family the filter can name, with the default first.
    readonly property var fontChoices: {
        var plain = /^[\p{L}\p{N}][\p{L}\p{N} .+]{0,63}$/u;
        return ["Sans"].concat(Qt.fontFamilies().filter(f => f !== "Sans" && plain.test(f)));
    }
    readonly property var colorChoices: ["#ffffff", "#ffd60a", "#ff453a", "#30d158", "#0a84ff", "#000000"]

    implicitHeight: body.implicitHeight + 24

    onTextsChanged: {
        if (current >= texts.length)
            current = texts.length - 1;
        if (current < 0 && texts.length > 0)
            current = 0;
    }

    // Picks a text and shows it on screen; with typing, puts the cursor in
    // its words with the starter text selected, ready to be replaced.
    function select(index, typing) {
        current = index;
        var text = texts[index];
        // Only if it isn't on screen already: a text just added at the
        // playhead stays where it is. Aims past its fade-in, so it's all there.
        if (text && (playheadSec < text.start || playheadSec > text.end))
            seek(Math.min(text.start + 0.4, (text.start + text.end) / 2));
        if (typing) {
            words.forceActiveFocus();
            words.selectAll();
        }
    }
    function blurFields() {
        panel.forceActiveFocus();
    }
    function setProperty(name, value) {
        if (current >= 0)
            timeline.setTextProperty(current, name, value);
    }
    function snippet(text) {
        var line = String(text).split("\n")[0];
        return line === "" ? "…" : line.length > 16 ? line.slice(0, 15) + "…" : line;
    }

    component Chip: Rectangle {
        id: chip
        property string label: ""
        property bool active: false
        property color swatch: "transparent"
        signal clicked()
        implicitWidth: swatch.a > 0 ? 26 : chipLabel.implicitWidth + 22
        implicitHeight: 28
        radius: Math.min(4, theme.radius)
        color: swatch.a > 0 ? swatch : active ? theme.accent : chipHover.hovered ? "#35353a" : "#2c2c2f"
        border.width: active ? 2 : 1
        border.color: active ? (swatch.a > 0 ? theme.accent : Qt.darker(theme.accent, 1.15)) : "#3a3a3e"
        Label {
            id: chipLabel
            anchors.centerIn: parent
            visible: chip.swatch.a === 0
            text: chip.label
            textFormat: Text.PlainText
            color: chip.active ? theme.accentForeground : "white"
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

    ColumnLayout {
        id: body
        anchors.fill: parent
        anchors.margins: 12
        spacing: 8

        // The texts, and add and close.
        RowLayout {
            Layout.fillWidth: true
            spacing: 6

            Flow {
                Layout.fillWidth: true
                spacing: 6
                Repeater {
                    model: panel.texts.length
                    delegate: Chip {
                        required property int index
                        label: panel.snippet(panel.texts[index].text)
                        active: index === panel.current
                        onClicked: panel.select(index, false)
                    }
                }
                Chip {
                    label: "+ " + qsTr("Add text")
                    onClicked: panel.addRequested()
                }
            }
            Chip {
                label: "×"
                implicitWidth: 28
                Layout.alignment: Qt.AlignTop
                onClicked: panel.closeRequested()
            }
        }

        // What the picked one says, and when it shows.
        RowLayout {
            visible: panel.picked !== null
            Layout.fillWidth: true
            spacing: 12

            TextArea {
                id: words
                objectName: "textWords"
                Layout.fillWidth: true
                Layout.preferredHeight: 62
                placeholderText: qsTr("Type the text")
                color: "white"
                font.pixelSize: 14
                wrapMode: TextEdit.Wrap
                selectByMouse: true
                Material.accent: theme.accent
                text: panel.picked ? panel.picked.text : ""
                onTextChanged: {
                    // Only a person's typing is an edit; picking another text
                    // or an undo changes this too.
                    if (activeFocus && panel.picked && text !== panel.picked.text)
                        panel.setProperty("text", text);
                }
                onActiveFocusChanged: if (!activeFocus) panel.timeline.endGesture()
                Keys.onEscapePressed: panel.blurFields()
            }

            GridLayout {
                columns: 3
                columnSpacing: 8
                rowSpacing: 6
                Caption { text: qsTr("Start") }
                TextField {
                    id: startField
                    objectName: "textStart"
                    Layout.preferredWidth: 96
                    text: panel.picked ? Format.fmt(panel.picked.start) : ""
                    font.family: "monospace"
                    font.pixelSize: 13
                    selectByMouse: true
                    Material.accent: theme.accent
                    onAccepted: panel.blurFields()
                    onEditingFinished: {
                        var seconds = Format.parseTime(text);
                        if (!isNaN(seconds))
                            panel.setProperty("start", seconds);
                        text = Qt.binding(() => panel.picked ? Format.fmt(panel.picked.start) : "");
                    }
                    Keys.onEscapePressed: panel.blurFields()
                }
                Chip {
                    label: qsTr("At playhead")
                    onClicked: panel.setProperty("start", panel.playheadSec)
                }
                Caption { text: qsTr("End") }
                TextField {
                    id: endField
                    objectName: "textEnd"
                    Layout.preferredWidth: 96
                    text: panel.picked ? Format.fmt(panel.picked.end) : ""
                    font.family: "monospace"
                    font.pixelSize: 13
                    selectByMouse: true
                    Material.accent: theme.accent
                    onAccepted: panel.blurFields()
                    onEditingFinished: {
                        var seconds = Format.parseTime(text);
                        if (!isNaN(seconds))
                            panel.setProperty("end", seconds);
                        text = Qt.binding(() => panel.picked ? Format.fmt(panel.picked.end) : "");
                    }
                    Keys.onEscapePressed: panel.blurFields()
                }
                Chip {
                    label: qsTr("At playhead")
                    onClicked: panel.setProperty("end", panel.playheadSec)
                }
            }
        }

        // How it looks.
        Flow {
            visible: panel.picked !== null
            Layout.fillWidth: true
            spacing: 6
            Layout.topMargin: 2

            Repeater {
                model: panel.effectChoices
                delegate: Chip {
                    required property var modelData
                    label: modelData.label
                    active: panel.picked !== null && panel.picked.effect === modelData.value
                    onClicked: panel.setProperty("effect", modelData.value)
                }
            }
            Item { width: 10; height: 1 }
            Repeater {
                model: panel.positionChoices
                delegate: Chip {
                    required property var modelData
                    label: modelData.label
                    active: panel.picked !== null && Math.abs(panel.picked.y - modelData.value) < 0.001
                    onClicked: panel.setProperty("y", modelData.value)
                }
            }
            Item { width: 10; height: 1 }
            Repeater {
                model: panel.sizeChoices
                delegate: Chip {
                    required property var modelData
                    label: modelData.label
                    active: panel.picked !== null && Math.abs(panel.picked.size - modelData.value) < 0.001
                    onClicked: panel.setProperty("size", modelData.value)
                }
            }
            Caption { text: qsTr("Size") + " %" }
            TextField {
                id: sizeField
                objectName: "textSize"
                width: 56
                text: panel.picked ? String(Math.round(panel.picked.size * 1000) / 10) : ""
                font.family: "monospace"
                font.pixelSize: 13
                selectByMouse: true
                Material.accent: theme.accent
                validator: DoubleValidator { bottom: 2; top: 30; decimals: 1; notation: DoubleValidator.StandardNotation }
                onAccepted: panel.blurFields()
                onEditingFinished: {
                    var percent = parseFloat(text.replace(",", "."));
                    if (!isNaN(percent))
                        panel.setProperty("size", percent / 100);
                    text = Qt.binding(() => panel.picked ? String(Math.round(panel.picked.size * 1000) / 10) : "");
                }
                Keys.onEscapePressed: panel.blurFields()
            }
            Item { width: 10; height: 1 }
            ComboBox {
                id: fontBox
                objectName: "textFont"
                width: 190
                model: panel.fontChoices
                font.pixelSize: 13
                Material.accent: theme.accent
                currentIndex: panel.picked ? Math.max(0, panel.fontChoices.indexOf(panel.picked.font)) : 0
                onActivated: (index) => panel.setProperty("font", panel.fontChoices[index])
            }
            Item { width: 10; height: 1 }
            Repeater {
                model: panel.colorChoices
                delegate: Chip {
                    required property string modelData
                    swatch: modelData
                    active: panel.picked !== null && panel.picked.color === modelData
                    onClicked: panel.setProperty("color", modelData)
                }
            }
            Item { width: 10; height: 1 }
            Chip {
                label: qsTr("Box")
                active: panel.picked !== null && panel.picked.box
                onClicked: panel.setProperty("box", !panel.picked.box)
            }
            Chip {
                label: qsTr("Remove text")
                onClicked: panel.timeline.removeText(panel.current)
            }
        }
    }

    signal addRequested()
}
