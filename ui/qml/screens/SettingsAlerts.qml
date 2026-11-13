pragma ComponentBehavior: Bound

import QtQuick
import Qkvm

Section {
    id: section
    required property AlertsView alerts
    required property SettingsView settings
    objectName: "settingsAlerts"
    name: qsTr("Alerts and incidents")
    about: qsTr("Review changes on this wall, including while its windows are hidden.")
    problem: alerts !== null ? alerts.problem : ""

    Intro {
        text: section.alerts !== null && section.alerts.sources.length > 0 ? qsTr("History is saved on this PC. Saved KVMs and servers with a saved sign-in are checked while qKVM is running, including when another wall is open. Choose a source to review its incidents.") : qsTr("Unlock qKVM and choose a connected source to review its incidents.")
    }
    Block {
        title: qsTr("Monitored sources")
        Line {
            label: qsTr("Source")
            hint: section.alerts !== null ? section.alerts.sourceStatus : ""
            Choice {
                objectName: "incidentSource"
                width: 220
                hint: qsTr("Source for incident history")
                choices: section.alerts !== null ? section.alerts.sources : []
                value: section.alerts !== null ? section.alerts.scope : ""
                enabled: choices.length > 0
                onPicked: value => section.alerts.choose(value)
            }
            Button {
                objectName: "openIncidentSource"
                text: qsTr("Open source")
                enabled: section.alerts !== null && section.alerts.scope !== ""
                onClicked: section.alerts.openSource()
            }
        }
    }
    Block {
        title: qsTr("Notifications")
        Line {
            label: qsTr("Windows notifications")
            hint: qsTr("Notify when all wall windows are hidden. History is kept whether notifications are on or off.")
            Check {
                objectName: "alertNotifications"
                Accessible.name: qsTr("Windows notifications")
                checked: section.settings.tellsNews
                enabled: section.alerts !== null && section.alerts.allowed
                onToggled: section.settings.tellsNews = checked
            }
        }
        Line {
            label: qsTr("Maintenance pause")
            hint: section.alerts !== null && section.alerts.paused ? qsTr("Notifications paused until %1. Incidents are still recorded.").arg(section.alerts.pauseEnds) : qsTr("Pause notifications for this wall for one hour. Resume reports only changes that still apply.")
            Button {
                objectName: "pauseAlerts"
                text: section.alerts !== null && section.alerts.paused ? qsTr("Resume alerts") : qsTr("Pause for 1 hour")
                enabled: section.alerts !== null && section.alerts.allowed
                onClicked: section.alerts.pause(!section.alerts.paused)
            }
        }
    }
    Block {
        title: qsTr("Incidents")
        Line {
            label: section.alerts !== null ? qsTr("%1 need attention").arg(section.alerts.attention) : qsTr("No wall open")
            hint: qsTr("Acknowledging records that you have seen an incident; recovery is recorded when the device is healthy again.")
            Check {
                objectName: "showIncidentHistory"
                text: qsTr("Include history")
                enabled: section.alerts !== null && section.alerts.allowed
                checked: section.alerts !== null && section.alerts.history
                onToggled: section.alerts.history = checked
            }
        }
    }
    Listed {
        id: incidents
        objectName: "incidentList"
        columns: [qsTr("Machine / incident"), qsTr("When / status")]
        lines: [
            {
                "text": 2,
                "under": [2]
            },
            {
                "text": 1,
                "under": [2, 1]
            }
        ]
        doesWidth: Math.ceil(acknowledgeWidth.advanceWidth + pauseWidth.advanceWidth) + 2 * 24 + 8
        nothing: qsTr("No incidents to show.")
        model: section.alerts !== null ? section.alerts.entries : []
        delegate: ListRow {
            id: row
            required property var modelData
            objectName: "incidentRow"
            of: incidents
            cells: [
                {
                    "text": modelData.name,
                    "under": [modelData.state],
                    "tone": modelData.active ? "bad" : ""
                },
                {
                    "text": modelData.active ? qsTr("Active") : modelData.removed ? qsTr("No longer monitored") : qsTr("Recovered"),
                    "under": [qsTr("Started %1").arg(modelData.opened) + "\n" + (modelData.active ? qsTr("Updated %1").arg(modelData.changed) : qsTr("Ended %1").arg(modelData.closed)), modelData.muted ? qsTr("Notifications paused") : modelData.acknowledged ? qsTr("Acknowledged") : qsTr("Not acknowledged")]
                }
            ]
            Button {
                objectName: "acknowledgeIncident"
                text: row.modelData.acknowledged ? qsTr("Acknowledged") : qsTr("Acknowledge")
                enabled: !row.modelData.acknowledged
                onClicked: section.alerts.acknowledge(row.modelData.id)
            }
            Button {
                objectName: "pauseDeviceAlerts"
                text: row.modelData.muted ? qsTr("Resume") : qsTr("Pause 1 hour")
                enabled: row.modelData.canPause && !section.alerts.paused
                onClicked: section.alerts.pauseDevice(row.modelData.id, !row.modelData.muted)
            }
        }
    }
    TextMetrics {
        id: acknowledgeWidth
        font.family: Application.font.family
        font.pixelSize: Theme.textSize
        font.weight: Font.Medium
        text: qsTr("Acknowledged")
    }
    TextMetrics {
        id: pauseWidth
        font.family: Application.font.family
        font.pixelSize: Theme.textSize
        font.weight: Font.Medium
        text: qsTr("Pause 1 hour")
    }
    Block {
        visible: section.alerts !== null && section.alerts.damaged
        Line {
            label: qsTr("Unreadable saved history")
            hint: qsTr("Keep the unreadable file as a backup and start a new history with the incidents observed in this run.")
            Button {
                objectName: "repairIncidentHistory"
                text: qsTr("Restart history")
                onClicked: section.alerts.repair()
            }
        }
    }
    Footnote {
        text: qsTr("Keeps active incidents and the latest 1,000 closed incidents. Initial observations are recorded quietly; changes must settle before notifying. Restarted and reconnected walls get a short grace period.")
    }
}
