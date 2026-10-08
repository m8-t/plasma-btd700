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
    required property string rateText

    readonly property bool headphonesUp: client.state === "connected"
        || client.state === "streaming-audio" || client.state === "streaming-voice"

    readonly property bool daemonMissing: !client.serviceAvailable
    readonly property bool dongleMissing: client.serviceAvailable && !client.present
    readonly property bool noHeadphones: client.serviceAvailable && client.present && !headphonesUp
    readonly property bool connected: client.serviceAvailable && client.present && headphonesUp

    readonly property var audioModes: [
        { id: "high-quality", label: i18n("High quality"), icon: "media-optical-audio-symbolic" },
        { id: "gaming", label: i18n("Gaming"), icon: "input-gamepad-symbolic" }
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

    collapseMarginsHint: false

    Layout.minimumWidth: Kirigami.Units.gridUnit * 18
    Layout.minimumHeight: Kirigami.Units.gridUnit * 12
    Layout.preferredWidth: Kirigami.Units.gridUnit * 20
    Layout.preferredHeight: Math.min(Kirigami.Units.gridUnit * 30,
        Math.max(Layout.minimumHeight, (header ? header.implicitHeight : 0) + topPadding + bottomPadding + column.implicitHeight))

    header: PlasmaExtras.PlasmoidHeading {
        RowLayout {
            anchors.fill: parent

            PlasmaExtras.Heading {
                Layout.fillWidth: true
                level: 3
                text: full.connected ? full.stateText : ""
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

    contentItem: PlasmaComponents3.ScrollView {
        id: scroll

        // explicit so the implicit width does not depend on availableWidth (binding loop)
        implicitWidth: Kirigami.Units.gridUnit * 20
        contentWidth: availableWidth
        PlasmaComponents3.ScrollBar.horizontal.policy: PlasmaComponents3.ScrollBar.AlwaysOff

        ColumnLayout {
            id: column

            width: scroll.availableWidth
            height: Math.max(implicitHeight, scroll.availableHeight)
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
                visible: full.daemonMissing
                iconName: "dialog-warning"
                text: i18n("The BTD 700 daemon is not running")
                explanation: i18n("Start it with: systemctl --user enable --now btd700d.service")
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: full.dongleMissing
                spacing: Kirigami.Units.gridUnit

                Item {
                    Layout.fillHeight: true
                }

                EmptyStateIcon {
                    source: Qt.resolvedUrl("../icons/btd700ctl-absent-symbolic.svg")
                    opacity: 0.75
                }

                PlasmaExtras.PlaceholderMessage {
                    Layout.fillWidth: true
                    text: i18n("Dongle not plugged in")
                    explanation: i18n("Plug in the BTD 700 USB dongle.")
                }

                Item {
                    Layout.fillHeight: true
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: full.noHeadphones
                spacing: Kirigami.Units.gridUnit

                Item {
                    Layout.fillHeight: true
                }

                EmptyStateIcon {
                    source: Qt.resolvedUrl("../icons/btd700ctl-idle-symbolic.svg")
                }

                PlasmaExtras.PlaceholderMessage {
                    Layout.fillWidth: true
                    text: i18n("Headphones not connected")
                    helpfulAction: Kirigami.Action {
                        icon.name: "network-connect"
                        text: i18n("Connect")
                        enabled: !full.client.busy
                        onTriggered: full.client.connectHeadphones()
                    }
                }

                Item {
                    Layout.fillHeight: true
                }
            }

            GridLayout {
                Layout.fillWidth: true
                visible: full.connected
                columns: 2
                columnSpacing: Kirigami.Units.largeSpacing

                PlasmaComponents3.Label {
                    text: i18n("Quality")
                    opacity: 0.7
                }
                PlasmaComponents3.Label {
                    Layout.fillWidth: true
                    text: full.rateText.length > 0 ? full.rateText : "-"
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
                visible: full.connected
                level: 5
                text: i18n("Audio mode")
            }

            Row {
                Layout.alignment: Qt.AlignHCenter
                visible: full.connected
                enabled: !full.client.busy
                spacing: Kirigami.Units.largeSpacing

                Repeater {
                    model: full.audioModes

                    PlasmaComponents3.Button {
                        id: tile

                        required property var modelData

                        width: Kirigami.Units.gridUnit * 5
                        height: width
                        text: modelData.label
                        icon.name: modelData.icon

                        // the Plasma style draws only down/checked/focus/hover, never highlighted;
                        // down is driven from the daemon's value so a click cannot change it
                        readonly property bool active: full.client.audioMode === modelData.id
                        down: pressed || active
                        onClicked: if (!active) full.client.setAudioMode(modelData.id)

                        // stock TextUnderIcon content stretches both cells and leaves a gap between icon and label
                        contentItem: Item {
                            Column {
                                anchors.centerIn: parent
                                width: parent.width

                                Kirigami.Icon {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    width: Kirigami.Units.iconSizes.large
                                    height: width
                                    source: tile.icon.name
                                }

                                PlasmaComponents3.Label {
                                    width: parent.width
                                    horizontalAlignment: Text.AlignHCenter
                                    elide: Text.ElideRight
                                    text: tile.text
                                }
                            }
                        }
                    }
                }
            }

            PlasmaExtras.Heading {
                Layout.fillWidth: true
                visible: full.connected && full.client.supportedCodecs.length > 0
                level: 5
                text: i18n("Codec")
            }

            Flow {
                Layout.fillWidth: true
                visible: full.connected
                enabled: !full.client.busy
                spacing: Kirigami.Units.smallSpacing

                Repeater {
                    model: full.client.supportedCodecs

                    PlasmaComponents3.Button {
                        required property string modelData

                        text: Codecs.displayName(modelData)
                        readonly property bool active: full.client.activeCodecs.indexOf(modelData) >= 0
                        icon.name: active ? "object-select-symbolic" : ""
                        down: pressed || active
                        onClicked: if (!active) full.client.setCodec(modelData)
                    }
                }
            }

            Item {
                Layout.fillHeight: true
                visible: full.connected
            }
        }
    }
}
