import QtQuick
import Qkvm

// The settings' section of how live pictures are decoded and drawn on this PC: whether the graphics
// card decodes them, which every window takes up at each stream's next key frame; and which graphics
// card the program uses, with the name of the card each choice comes to here, which it takes up when
// it starts again. Under that choice, two lines that come and go: that the screen is on another
// adapter, and that the choice takes effect at the next start. The choice itself stays where it is;
// the second line may move up into the place of the first.
Section {
    id: section

    required property SettingsView settings

    // The card chosen on this PC: what it is called (nothing where Windows names none); and whether
    // the screen is on another adapter than it here, where one of them has it (over Remote Desktop
    // none has, and nothing is said).
    readonly property var chosen: settings.graphicsCards.find(card => card.value === settings.graphicsCard)
    readonly property string chosenCard: chosen ? chosen.adapter : ""
    readonly property bool screenElsewhere: chosen !== undefined && chosen.monitor === false && settings.graphicsCards.some(card => card.monitor !== false)

    objectName: "settingsVideo"
    name: Words.video
    about: Words.videoAbout

    Block {
        title: Words.decoding

        Line {
            label: Words.hardwareAcceleration
            hint: Words.hardwareAccelerationHint

            Choice {
                objectName: "hardwareDecodingChoice"
                width: Math.max(160, Math.ceil(widest) + 44)
                hint: Words.hardwareAcceleration
                choices: [
                    {
                        "value": "automatic",
                        "label": Words.automatic
                    },
                    {
                        "value": "on",
                        "label": Words.switchedOn
                    },
                    {
                        "value": "off",
                        "label": Words.switchedOff
                    }
                ]
                value: section.settings.hardwareDecoding
                onPicked: value => section.settings.hardwareDecoding = value
            }
        }
    }
    Block {
        title: Words.graphicsCard

        Line {
            label: Words.useTheCard
            hint: section.chosenCard !== "" ? Words.cardHere.arg(section.chosenCard) : ""

            Choice {
                objectName: "graphicsCardChoice"
                width: Math.max(160, Math.ceil(widest) + 44)
                hint: Words.graphicsCard
                choices: [
                    {
                        "value": "most-powerful",
                        "label": Words.mostPowerful
                    },
                    {
                        "value": "power-saving",
                        "label": Words.powerSaving
                    },
                    {
                        "value": "windows",
                        "label": Words.asSetInWindows
                    }
                ]
                value: section.settings.graphicsCard
                onPicked: value => section.settings.graphicsCard = value
            }
        }
        // (A line of its own, under the choice, which stays where it is.)
        Line {
            objectName: "cardWithoutTheScreen"
            visible: section.screenElsewhere
            label: Words.cardWithoutTheScreen
        }
        Line {
            objectName: "cardAtTheNextStart"
            visible: section.settings.cardChangesAtTheNextStart
            label: Words.cardAtTheNextStart
        }
    }
}
