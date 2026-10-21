
import QtQuick
import org.kde.plasma.components as PC
import org.kde.plasma.private.clipboard as Qs
Item {
    width: 400; height: 60
    property string selectedText: "Точный выбранный текст\n'$()`; & <tag> и Unicode 😀"
    function sendSelected() { tools.trigger(Qs.DelegateToolButtons.ButtonRole.SendToPhone); }
    PC.ItemDelegate {
        id: mockItem; visible: false
        property int type: 2
        property var model: ({display: selectedText, starred: false})
    }
    Qs.DelegateToolButtons {
        id: tools; menuItem: mockItem; shouldUseOverflowButton: false
    }
}