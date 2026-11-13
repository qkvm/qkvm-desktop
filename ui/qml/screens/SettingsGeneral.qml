import QtQuick
import Qkvm

// The settings' section of the program's own on this PC: how it looks; whether it goes on behind its
// icon in the notification area when its window is closed, and whether Windows shows a message of
// the wall while nobody sees it; and whether it starts with Windows, with what went wrong in a line of
// its own where Windows did not take a change. That line comes and goes last, where nothing stands
// under it. Where the window shows a QKVM server's wall, first: that the server's machines, accounts
// and own settings are managed on its page, with the button that opens it.
Section {
    id: section

    required property SettingsView settings
    required property Look look
    required property SessionView session
    // The window shows a QKVM server's wall.
    property bool atAServersWall: false

    signal pageAsked

    objectName: "settingsGeneral"
    name: Words.general
    about: Words.generalAbout

    Block {
        objectName: "settingsServer"
        visible: section.atAServersWall
        title: Words.qkvmServer

        Line {
            label: Words.theQkvmServerAt.arg(section.session.address)
            hint: Words.managedOnItsPage

            Button {
                objectName: "settingsOpenPage"
                symbol: "external"
                text: Words.openItsPage
                onClicked: section.pageAsked()
            }
        }
    }
    Block {
        title: Words.look

        Line {
            label: Words.theme

            Choice {
                objectName: "themeChoice"
                // As wide as the widest of the three, whichever is chosen: it stays where it stands.
                width: Math.max(160, Math.ceil(widest) + 44)
                hint: Words.theme
                choices: [
                    {
                        "value": "dark",
                        "label": Words.themeDark
                    },
                    {
                        "value": "light",
                        "label": Words.themeLight
                    },
                    {
                        "value": "system",
                        "label": Words.themeAsWindows
                    }
                ]
                value: section.look.theme
                onPicked: value => section.look.choose(value)
            }
        }
    }
    Block {
        title: Words.controlSettings

        Line {
            label: Words.releaseKey
            hint: Words.releaseKeyHelp

            Choice {
                objectName: "releaseKeyChoice"
                width: Math.max(160, Math.ceil(widest) + 44)
                hint: Words.releaseKey
                choices: [
                    {
                        "value": "right-ctrl",
                        "label": qsTr("Right Ctrl")
                    },
                    {
                        "value": "scroll-lock",
                        "label": qsTr("Scroll Lock")
                    },
                    {
                        "value": "f12",
                        "label": qsTr("F12")
                    }
                ]
                value: section.settings.releaseKey
                onPicked: value => section.settings.releaseKey = value
            }
        }
    }
    Block {
        title: Words.whileTheWindowIsOutOfSight

        Line {
            label: Words.keepRunningInTray

            Check {
                objectName: "staysInTray"
                checked: section.settings.staysInTray
                Accessible.name: Words.keepRunningInTray
                onToggled: section.settings.staysInTray = checked
            }
        }
        Line {
            label: Words.tellMeNews

            Check {
                objectName: "tellsNews"
                checked: section.settings.tellsNews
                Accessible.name: Words.tellMeNews
                onToggled: section.settings.tellsNews = checked
            }
        }
    }
    Block {
        title: Words.windows

        Line {
            label: Words.startWithWindows

            Check {
                objectName: "startsWithWindows"
                // What is shown is what is so, also where Windows did not take a change: the view
                // says "changed" after every one that was asked for, and the tick a click has set
                // by itself follows.
                checked: section.settings.startsWithWindows
                Accessible.name: Words.startWithWindows
                onToggled: section.settings.startsWithWindows = checked
            }
        }
        // What went wrong where Windows did not take it.
        Line {
            objectName: "settingsProblem"
            visible: section.settings.problem !== ""
            label: section.settings.problem
            tone: "bad"
        }
    }
}
