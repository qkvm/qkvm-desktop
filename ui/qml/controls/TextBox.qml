import QtQuick
import QtQuick.Templates as T
import Qkvm

// Several lines to type into, with what they are for written above them and a sentence under them,
// as the page's forms have it. Not more than `most` letters are taken. Tab goes on to the next
// control, as in every other box: a text for a machine's keyboard has no use for a tab of its own.
Column {
    id: box

    property string label
    property string hint
    property alias text: input.text
    property alias placeholder: input.placeholderText
    property int most: 2000
    property bool sensitive: false

    function takeFocus() {
        input.forceActiveFocus();
    }

    spacing: 6

    // As a line's (Field.qml): as the page writes it.
    Text {
        textFormat: Text.PlainText
        text: box.label
        color: Theme.text
        font.pixelSize: Theme.textSize
        font.weight: Font.Medium
    }
    Flickable {
        id: scroller

        width: box.width
        height: 96
        contentHeight: input.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        T.TextArea.flickable: T.TextArea {
            id: input

            objectName: "textBox"
            leftPadding: 10
            rightPadding: 10
            topPadding: 8
            bottomPadding: 8
            color: Theme.text
            placeholderTextColor: Theme.muted
            selectionColor: Theme.accent
            selectedTextColor: Theme.textOnAccent
            font.family: Application.font.family
            font.pixelSize: Theme.textSize
            selectByMouse: true
            inputMethodHints: box.sensitive ? Qt.ImhSensitiveData | Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase : Qt.ImhNone
            // Letters, also where what is pasted into it was written like a page.
            textFormat: TextEdit.PlainText
            wrapMode: TextEdit.Wrap
            Accessible.name: box.label
            Accessible.description: box.hint
            Keys.onTabPressed: nextItemInFocusChain(true).forceActiveFocus(Qt.TabFocusReason)
            Keys.onBacktabPressed: nextItemInFocusChain(false).forceActiveFocus(Qt.BacktabFocusReason)
            onTextChanged: {
                let end = 0;
                let characters = 0;
                while (end < text.length && characters < box.most) {
                    const first = text.charCodeAt(end++);
                    if (first >= 0xD800 && first <= 0xDBFF && end < text.length) {
                        const second = text.charCodeAt(end);
                        if (second >= 0xDC00 && second <= 0xDFFF)
                            ++end;
                    }
                    ++characters;
                }
                if (end < text.length)
                    remove(end, length);
            }

            Text {
                textFormat: Text.PlainText
                x: input.leftPadding
                y: input.topPadding
                visible: input.text === "" && input.preeditText === ""
                text: input.placeholderText
                color: input.placeholderTextColor
                font: input.font
            }

            background: Rectangle {
                radius: Theme.radius
                color: Theme.inset
                border.width: 1
                border.color: input.activeFocus ? Theme.accentText : Theme.lineStrong
            }
        }
    }
    Text {
        textFormat: Text.PlainText
        width: box.width
        visible: text !== ""
        text: box.hint
        color: Theme.muted
        font.pixelSize: Theme.smallSize
        wrapMode: Text.WordWrap
    }
}
