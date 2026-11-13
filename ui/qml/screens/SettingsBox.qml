pragma ComponentBehavior: Bound

import QtQuick
import Qkvm

// The program's settings, in the shape the QKVM server's page has (its `settingsDialog`): at the left
// a list of sections under small group headings, the one shown marked; beside it the section, every
// one of one shape (Section.qml). This PC's wall has sections of its own, listed only where the window
// shows that wall; a QKVM server's wall has its wall as a file, listed only for whoever manages it,
// as the page lists it for an admin; the program's are always listed. The rest of a QKVM server's
// settings stays on its page. Which sections there are, and which the box opens at, is the
// program's (ui/settings_view.h): where it was left, also in an earlier run of the program.
//
// The box holds still. It has one size and one place while it is open, whichever section it shows
// and whatever changes in one: as large as the page's (1080 by 780) and no larger than its window
// has room for, in the middle of it, and never as large as what is in it. Only a section's own
// content scrolls, under its first line. The sections it lists are those of what the window showed
// when it was opened; where the window comes to show another wall meanwhile (or none), the box
// closes. In a narrow window the sections lie in a row above the section.
//
// The boxes its sections open (adding a KVM, changing one, taking one off; this PC's wall or a
// server's written to a file, and a file taken in) stand over this one, as the page stacks them; it shows
// what came of them, and the keyboard is back where it was when they close. A KVM that another window
// takes off while its box stands over this one (its question, its lines) is said to be gone by that
// box, which stays; and by its row, which stays too.
//
// It shows what it shows only while its window is in sight: a window that goes to the
// notification area with the box open on the list of KVMs holds that list for nobody.
Dialog {
    id: box

    required property SettingsView settings
    required property Look look
    required property OwnKvmsView kvms
    required property AboutModel about
    required property SessionView session

    // What the window shows, by the word its sections are listed for: this PC's own wall, a QKVM
    // server's that whoever is signed in manages ("managed"; not while the app lock holds that
    // back), another QKVM server's, or no wall yet.
    readonly property bool locked: settings.security !== null && settings.security.restricted
    readonly property string shows: session.stage === "wall" ? (session.ownWall ? "pc" : session.manages && !locked ? "managed" : "server") : "none"
    // What they are listed for while the box is open: what the window showed when it was opened.
    property string listedFor: "none"
    // The section shown, by its key.
    property string current
    // The window is narrow: the sections lie in a row.
    readonly property bool narrow: (parent ? parent.width : 1280) < Theme.narrowWindow
    // Its window is in sight: not out of sight behind the program's icon, and not made small.
    readonly property bool inSight: inside.inSight
    // The title of a section's editor that is open with changes that are not saved (Saved actions'); empty otherwise.
    readonly property string unsavedEdit: (shown.item as SettingsActions)?.unsavedTitle ?? ""

    // The names, icons and group headings of the sections, by their keys (the page's).
    readonly property var names: ({
            "kvms": Words.kvms,
            "files": Words.exportAndImport,
            "actions": qsTr("Saved actions"),
            "security": qsTr("App lock"),
            "general": Words.general,
            "video": Words.video,
            "updates": qsTr("Updates"),
            "alerts": qsTr("Alerts and incidents"),
            "about": Words.about
        })
    readonly property var icons: ({
            "kvms": "grid",
            "files": "archive",
            "actions": "command",
            "security": "lock",
            "general": "sliders",
            "video": "zap",
            "updates": "download",
            "alerts": "alert",
            "about": "info"
        })
    readonly property var headings: ({
            "pc": Words.thisPcsWall,
            "server": Words.theServersWall,
            "program": Words.thisProgram
        })

    signal addAsked
    signal editAsked(string machine)
    signal removeAsked(string machine)
    signal exportAsked
    signal importAsked
    // A server's wall as a file: written out by the server, or taken in.
    signal serverExportAsked
    signal serverImportAsked
    // It closed because the window came to show another wall, or the same with less in it: what
    // stood over it of the wall it listed goes too.
    signal closedForAnotherWall

    function choose(key: string) {
        current = key;
        settings.show(key);
    }

    title: Words.settings
    // (Inside its frame's line.)
    padding: 1
    width: Math.min(1080, parent ? parent.width - 24 : 1080)
    height: Math.min(780, parent ? parent.height - 24 : 780)
    // How things are is Windows' to say, and is asked whenever the box is opened. And it opens where
    // it was left, of what is listed for what the window shows now. (Where that is not listed, at
    // the first that is; which is not where it was left by that.)
    onAboutToShow: {
        settings.lookAgain();
        listedFor = shows;
        current = settings.opensAt(listedFor);
        // (At its top, as a section is whenever it is shown.)
        const section = shown.item as Section;
        if (section !== null)
            section.flickable.contentY = 0;
    }
    onOpened: sections.takeKeyboard(false)
    // Where it was left is written down, for the next start.
    onClosed: settings.closed()

    // The window comes to show another wall, or none, or the same with less in it (whoever is
    // signed in no longer manages it, or the app lock holds that back): what it lists is not that
    // one's, and it closes. Where there is only more (the lock lifted, a role that manages), it
    // lists that too, in place.
    onShowsChanged: {
        if (!box.visible || box.shows === box.listedFor)
            return;
        if (box.listedFor === "server" && box.shows === "managed") {
            box.listedFor = box.shows;
            return;
        }
        box.close();
        box.closedForAnotherWall();
    }

    // (What the box holds under its title: the title's line is not counted by the box's own room.)
    Item {
        id: inside

        // (As its window is.)
        readonly property bool inSight: Window.visibility !== Window.Hidden && Window.visibility !== Window.Minimized

        width: box.availableWidth
        height: box.height - box.implicitHeaderHeight - box.topPadding - box.bottomPadding

        // Behind the list of sections: down the side, or along the top in a narrow window.
        Rectangle {
            id: aside

            width: box.narrow ? parent.width : 212
            height: box.narrow ? sections.implicitHeight : parent.height
            color: Theme.inset
            bottomLeftRadius: box.narrow ? 0 : Theme.radius + 1

            Rectangle {
                x: box.narrow ? 0 : parent.width - 1
                y: box.narrow ? parent.height - 1 : 0
                width: box.narrow ? parent.width : 1
                height: box.narrow ? 1 : parent.height
                color: Theme.line
            }
        }
        SectionList {
            id: sections

            objectName: "settingsSections"
            width: box.narrow ? aside.width : aside.width - 1
            height: aside.height - (box.narrow ? 1 : 0)
            inRow: box.narrow
            current: box.current
            groups: box.settings.sectionsFor(box.listedFor).map(group => ({
                        "title": box.headings[group.group] ?? "",
                        "sections": group.sections.map(key => ({
                                    "key": key,
                                    "name": box.names[key] ?? key,
                                    "icon": box.icons[key] ?? ""
                                }))
                    }))
            onChosen: key => box.choose(key)
        }
        // The section shown: made when it is chosen.
        Loader {
            id: shown

            objectName: "settingsShown"
            x: box.narrow ? 0 : aside.width
            y: box.narrow ? aside.height : 0
            width: parent.width - x
            height: parent.height - y
            sourceComponent: box.current === "kvms" ? kvmsSection : box.current === "actions" ? actionsSection : box.current === "security" ? securitySection : box.current === "files" ? (box.listedFor === "managed" ? serverFilesSection : filesSection) : box.current === "alerts" ? alertsSection : box.current === "updates" ? updatesSection : box.current === "about" ? aboutSection : box.current === "video" ? videoSection : generalSection
        }
    }

    Component {
        id: kvmsSection

        SettingsKvms {
            kvms: box.kvms
            shown: box.visible && box.inSight
            onAddAsked: box.addAsked()
            onEditAsked: machine => box.editAsked(machine)
            onRemoveAsked: machine => box.removeAsked(machine)
            onKeyboardToTheList: sections.takeKeyboard(false)
        }
    }
    Component {
        id: filesSection

        SettingsFiles {
            onExportAsked: box.exportAsked()
            onImportAsked: box.importAsked()
        }
    }
    Component {
        id: serverFilesSection

        SettingsServerFiles {
            onExportAsked: box.serverExportAsked()
            onImportAsked: box.serverImportAsked()
        }
    }
    Component {
        id: generalSection

        SettingsGeneral {
            settings: box.settings
            look: box.look
            session: box.session
            atAServersWall: box.listedFor === "server" || box.listedFor === "managed"
            onPageAsked: box.session.openPage()
        }
    }
    Component {
        id: videoSection

        SettingsVideo {
            settings: box.settings
        }
    }
    Component {
        id: alertsSection
        SettingsAlerts {
            alerts: box.settings.alerts
            settings: box.settings
        }
    }
    Component {
        id: updatesSection

        SettingsUpdates {
            updates: box.settings.updates
        }
    }
    Component {
        id: actionsSection
        SettingsActions {
            actions: box.settings.savedActions
            shown: box.visible && box.inSight
        }
    }
    Component {
        id: securitySection
        SettingsSecurity {
            security: box.settings.security
        }
    }
    Component {
        id: aboutSection

        SettingsAbout {
            program: box.about
        }
    }
}
