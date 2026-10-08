pragma ComponentBehavior: Bound

import QtQuick
import org.kde.plasma.plasmoid
import org.kde.kirigami as Kirigami
import org.btd700ctl.dongle
import "Codecs.js" as Codecs

PlasmoidItem {
    id: root

    DongleClient {
        id: dongle
    }

    readonly property bool linked: dongle.serviceAvailable && dongle.present

    readonly property string iconName: {
        if (!dongle.serviceAvailable)
            return "dialog-warning";
        if (!dongle.present)
            return "btd700ctl-idle-symbolic";
        switch (dongle.state) {
        case "streaming-audio":
        case "streaming-voice":
            return "btd700ctl-streaming-symbolic";
        case "connected":
            return "btd700ctl-connected-symbolic";
        default:
            return "btd700ctl-idle-symbolic";
        }
    }

    readonly property bool dongleAbsent: dongle.serviceAvailable && !dongle.present
    readonly property bool customIcon: iconName.startsWith("btd700ctl-")

    readonly property string stateText: {
        if (!dongle.serviceAvailable)
            return i18n("btd700d is not running");
        if (!dongle.present)
            return i18n("Dongle not plugged in");
        switch (dongle.state) {
        case "streaming-audio":
            return i18n("Streaming audio");
        case "streaming-voice":
            return i18n("Streaming voice");
        case "connected":
            return i18n("Headphones connected");
        default:
            return i18n("Headphones not connected");
        }
    }

    readonly property string codecText: dongle.activeCodecs.map(Codecs.displayName).join(", ")

    readonly property string rateText: {
        if (dongle.sampleRate === 0)
            return "";
        const khz = dongle.sampleRate / 1000;
        const rate = i18n("%1 kHz", Number(khz.toFixed(1)));
        return dongle.bitDepth > 0 ? i18n("%1 / %2-bit", rate, dongle.bitDepth) : rate;
    }

    Plasmoid.icon: iconName
    toolTipMainText: stateText
    toolTipSubText: {
        const parts = [];
        if (linked && dongle.headsetBattery >= 0)
            parts.push(i18n("Battery %1%", dongle.headsetBattery));
        if (linked && codecText.length > 0)
            parts.push(codecText);
        if (linked && rateText.length > 0)
            parts.push(rateText);
        return parts.join("\n");
    }

    compactRepresentation: MouseArea {
        activeFocusOnTab: true
        hoverEnabled: true
        onClicked: root.expanded = !root.expanded

        Kirigami.Icon {
            anchors.fill: parent
            source: root.customIcon ? Qt.resolvedUrl("../icons/" + root.iconName + ".svg") : root.iconName
            isMask: root.customIcon
            color: {
                if (root.dongleAbsent)
                    return Kirigami.Theme.disabledTextColor;
                switch (root.iconName) {
                case "btd700ctl-streaming-symbolic":
                    return Kirigami.Theme.highlightColor;
                case "btd700ctl-connected-symbolic":
                    return Kirigami.Theme.positiveTextColor;
                default:
                    return Kirigami.Theme.textColor;
                }
            }
            active: parent.containsMouse
        }
    }

    fullRepresentation: FullRepresentation {
        client: dongle
        stateText: root.stateText
        rateText: root.rateText
    }
}
