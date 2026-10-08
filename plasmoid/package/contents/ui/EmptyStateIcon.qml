import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// Same size as the icon in PlasmaExtras.PlaceholderMessage, but tinted with the
// text colour because that component only recolours icons it resolves by theme name
Kirigami.Icon {
    Layout.alignment: Qt.AlignHCenter
    Layout.preferredWidth: Math.round(Kirigami.Units.iconSizes.huge * 1.5)
    Layout.preferredHeight: Math.round(Kirigami.Units.iconSizes.huge * 1.5)
    isMask: true
    color: Kirigami.Theme.textColor
}
