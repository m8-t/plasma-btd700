pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import org.kde.plasma.components as PlasmaComponents3
import org.kde.plasma.extras as PlasmaExtras
import org.kde.kirigami as Kirigami
import org.btd700ctl.dongle
import "Codecs.js" as Codecs

PlasmaExtras.Representation {
    id: full

    required property DongleClient client
    required property string stateText
    required property string codecText
    required property string rateText

    readonly property bool headphonesUp: client.state === "connected"
        || client.state === "streaming-audio" || client.state === "streaming-voice"

    readonly property var audioModes: [
        { id: "high-quality", label: i18n("High quality") },
        { id: "gaming", label: i18n("Gaming") },
        { id: "broadcast", label: i18n("Broadcast") }
    ]

    function transportText(token) {
        switch (token) {
        case "classic":
            return i18n("Classic");
        case "le-audio":
            return i18n("LE Audio");
        case "multipoint":
            return i18n("Multipoint");
        case "disconnected":
            return i18n("Disconnected");
        default:
            return i18n("Unknown");
        }
    }

    function modeText(token) {
        for (let i = 0; i < audioModes.length; i++) {
            if (audioModes[i].id === token)
                return audioModes[i].label;
        }
        return i18n("Unknown");
    }

    function errorText(name, message) {
        switch (name) {
        case "org.freedesktop.DBus.Error.InvalidArgs":
            return i18n("Invalid request: %1", message);
        case "org.btd700ctl.Error.NotPresent":
            return i18n("The dongle is not plugged in.");
        case "org.btd700ctl.Error.Failed":
            return i18n("The dongle did not accept the command: %1", message);
        case "org.freedesktop.DBus.Error.ServiceUnknown":
            return i18n("btd700d is not running.");
        default:
            return message.length > 0 ? message : name;
        }
    }

    Layout.minimumWidth: Kirigami.Units.gridUnit * 18
    Layout.minimumHeight: Kirigami.Units.gridUnit * 12
    Layout.preferredWidth: Kirigami.Units.gridUnit * 20

    header: PlasmaExtras.PlasmoidHeading {
        RowLayout {
            anchors.fill: parent

            PlasmaExtras.Heading {
                Layout.fillWidth: true
                level: 3
                text: full.stateText
                elide: Text.ElideRight
            }

            PlasmaComponents3.BusyIndicator {
                visible: full.client.busy
                running: visible
                Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium
                Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
            }

            PlasmaComponents3.ToolButton {
                icon.name: "view-refresh"
                text: i18n("Refresh")
                display: PlasmaComponents3.AbstractButton.IconOnly
                enabled: full.client.serviceAvailable && full.client.present && !full.client.busy
                onClicked: full.client.refresh()
                PlasmaComponents3.ToolTip.text: text
                PlasmaComponents3.ToolTip.visible: hovered
            }
        }
    }

    contentItem: ColumnLayout {
        spacing: Kirigami.Units.smallSpacing

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            type: Kirigami.MessageType.Error
            visible: full.client.errorName.length > 0
            text: full.errorText(full.client.errorName, full.client.errorMessage)
            actions: [
                Kirigami.Action {
                    text: i18n("Dismiss")
                    onTriggered: full.client.clearError()
                }
            ]
        }

        PlasmaExtras.PlaceholderMessage {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: !full.client.serviceAvailable
            iconName: "dialog-warning"
            text: i18n("The BTD 700 daemon is not running")
            explanation: i18n("Start it with: systemctl --user enable --now btd700d.service")
        }

        PlasmaExtras.PlaceholderMessage {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: full.client.serviceAvailable && !full.client.present
            iconName: "network-bluetooth"
            text: i18n("Dongle not plugged in")
            explanation: i18n("Plug in the BTD 700 USB dongle.")
        }

        PlasmaExtras.PlaceholderMessage {
            Layout.fillWidth: true
            visible: full.client.serviceAvailable && full.client.present && !full.headphonesUp
            iconName: "network-bluetooth-activated"
            text: i18n("Headphones not connected")
            helpfulAction: Kirigami.Action {
                icon.name: "network-connect"
                text: i18n("Connect")
                enabled: !full.client.busy
                onTriggered: full.client.connectHeadphones()
            }
        }

        GridLayout {
            Layout.fillWidth: true
            visible: full.client.serviceAvailable && full.client.present
            columns: 2
            columnSpacing: Kirigami.Units.largeSpacing

            PlasmaComponents3.Label {
                text: i18n("Codec")
                opacity: 0.7
            }
            PlasmaComponents3.Label {
                Layout.fillWidth: true
                text: full.codecText.length > 0 ? full.codecText : "-"
                elide: Text.ElideRight
            }

            PlasmaComponents3.Label {
                text: i18n("Quality")
                opacity: 0.7
            }
            PlasmaComponents3.Label {
                Layout.fillWidth: true
                text: full.rateText.length > 0 ? full.rateText : "-"
            }

            PlasmaComponents3.Label {
                text: i18n("Audio mode")
                opacity: 0.7
            }
            PlasmaComponents3.Label {
                Layout.fillWidth: true
                text: full.modeText(full.client.audioMode)
            }

            PlasmaComponents3.Label {
                text: i18n("Transport")
                opacity: 0.7
            }
            PlasmaComponents3.Label {
                Layout.fillWidth: true
                text: full.transportText(full.client.transport)
            }

            PlasmaComponents3.Label {
                text: i18n("Firmware")
                opacity: 0.7
            }
            PlasmaComponents3.Label {
                Layout.fillWidth: true
                text: full.client.firmwareVersion.length > 0 ? full.client.firmwareVersion : "-"
            }
        }

        PlasmaExtras.Heading {
            Layout.fillWidth: true
            visible: full.client.serviceAvailable && full.client.present
            level: 5
            text: i18n("Audio mode")
        }

        RowLayout {
            Layout.fillWidth: true
            visible: full.client.serviceAvailable && full.client.present
            enabled: !full.client.busy

            Repeater {
                model: full.audioModes

                PlasmaComponents3.Button {
                    required property var modelData

                    Layout.fillWidth: true
                    text: modelData.label
                    highlighted: full.client.audioMode === modelData.id
                    onClicked: full.client.setAudioMode(modelData.id)
                }
            }
        }

        PlasmaExtras.Heading {
            Layout.fillWidth: true
            visible: full.client.serviceAvailable && full.client.present
                && full.client.supportedCodecs.length > 0
            level: 5
            text: i18n("Codec")
        }

        Flow {
            Layout.fillWidth: true
            visible: full.client.serviceAvailable && full.client.present
            enabled: !full.client.busy
            spacing: Kirigami.Units.smallSpacing

            Repeater {
                model: full.client.supportedCodecs

                PlasmaComponents3.Button {
                    required property string modelData

                    text: Codecs.displayName(modelData)
                    icon.name: highlighted ? "object-select-symbolic" : ""
                    highlighted: full.client.activeCodecs.indexOf(modelData) >= 0
                    onClicked: full.client.setCodec(modelData)
                }
            }
        }

        PlasmaComponents3.Button {
            Layout.alignment: Qt.AlignHCenter
            visible: full.client.serviceAvailable && full.client.present && full.headphonesUp
            enabled: !full.client.busy
            icon.name: "network-disconnect"
            text: i18n("Disconnect")
            onClicked: full.client.disconnectHeadphones()
        }

        Item {
            Layout.fillHeight: true
        }
    }
}
