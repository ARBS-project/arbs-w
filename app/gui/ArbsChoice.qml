import QtQuick 2.9
import QtQuick.Controls 2.2

import StreamingPreferences 1.0

// ARBS-W (B2 D9): one APEX setting = title + combo box bound to StreamingPreferences[prefKey].
// options = [{ text: "...", val: <value> }, ...]. sync() re-reads the preference (used after "Restore ASB defaults").
Column {
    id: choiceRoot
    property string title
    property string prefKey
    property var options: []

    width: parent ? parent.width : 0
    spacing: 2

    function sync() {
        var cur = StreamingPreferences[prefKey]
        var idx = 0
        for (var i = 0; i < options.length; i++) {
            if (Math.abs(options[i].val - cur) < 0.000001) {
                idx = i
                break
            }
        }
        box.currentIndex = idx
        box.recalculateWidth()
    }

    Label {
        width: parent.width
        text: choiceRoot.title
        font.pointSize: 12
        wrapMode: Text.Wrap
    }

    AutoResizingComboBox {
        id: box
        textRole: "text"
        model: choiceRoot.options
        Component.onCompleted: choiceRoot.sync()
        onActivated: {
            StreamingPreferences[choiceRoot.prefKey] = choiceRoot.options[currentIndex].val
        }
    }
}
