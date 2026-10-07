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
            return "network-bluetooth";
        switch (dongle.state) {
        case "streaming-audio":
            return "audio-volume-high";
        case "streaming-voice":
            return "audio-headset";
        case "connected":
            return "audio-headphones";
        default:
            return "network-bluetooth-activated";
        }
    }

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
            source: root.iconName
            active: parent.containsMouse
        }
    }

    fullRepresentation: FullRepresentation {
        client: dongle
        stateText: root.stateText
        codecText: root.codecText
        rateText: root.rateText
    }
}
