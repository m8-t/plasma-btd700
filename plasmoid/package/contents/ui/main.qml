pragma ComponentBehavior: Bound

import QtQuick
import org.kde.plasma.core as PlasmaCore
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

    // unix seconds, ticks while a reading is shown so its age stays current
    property real nowSeconds: Date.now() / 1000

    // the daemon cannot read the battery while the dongle streams, so say how old the value is
    readonly property string batteryText: {
        if (!linked || dongle.headsetBattery < 0)
            return "";
        const level = i18n("%1%", dongle.headsetBattery);
        const minutes = Math.floor((nowSeconds - dongle.headsetBatteryUpdated) / 60);
        if (minutes < 10)
            return level;
        const age = minutes < 60 ? i18np("%1 minute ago", "%1 minutes ago", minutes)
                                 : i18np("%1 hour ago", "%1 hours ago", Math.floor(minutes / 60));
        return i18nc("battery level, age of the reading", "%1 (%2)", level, age);
    }

    Timer {
        interval: 60000
        repeat: true
        triggeredOnStart: true
        running: root.batteryText.length > 0
        onTriggered: root.nowSeconds = Date.now() / 1000
    }

    Connections {
        target: dongle
        function onHeadsetBatteryUpdatedChanged() {
            root.nowSeconds = Date.now() / 1000;
        }
    }

    readonly property string rateText: {
        if (dongle.sampleRate === 0)
            return "";
        const khz = dongle.sampleRate / 1000;
        const rate = i18n("%1 kHz", Number(khz.toFixed(1)));
        return dongle.bitDepth > 0 ? i18n("%1 / %2-bit", rate, dongle.bitDepth) : rate;
    }

    Plasmoid.icon: iconName
    Plasmoid.contextualActions: [
        PlasmaCore.Action {
            text: i18n("Read Headphone Battery")
            // the tray shows this as an icon-only button in the popup header
            icon.name: "battery-good-symbolic"
            checkable: true
            checked: dongle.batteryReading
            enabled: dongle.serviceAvailable
            onTriggered: (on) => {
                dongle.setBatteryReading(on);
                // the click toggled checked behind the binding, re-evaluate it so the
                // entry shows the daemon's value even if the call fails
                checked = Qt.binding(() => dongle.batteryReading);
            }
        }
    ]
    toolTipMainText: stateText
    toolTipSubText: {
        const parts = [];
        if (batteryText.length > 0)
            parts.push(i18nc("@info:tooltip battery level with optional age", "Battery %1", batteryText));
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
        batteryText: root.batteryText
    }
}
