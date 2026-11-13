pragma Singleton
import QtQuick

// Every sentence the program shows, in one place; and for whatever the page also says, the page's
// words (app/qkvm/static/app.js and index.html).
QtObject {
    readonly property string appName: qsTr("qKVM")
    // A window's title: what it shows, then the program's name, as Windows titles go (Qt adds " - qKVM" to a
    // title that does not end with it). What it shows may be several, with " - " between them (Main.qml).
    readonly property string inTheTitle: qsTr("%1 - qKVM")
    readonly property string about: qsTr("About")
    readonly property string close: qsTr("Close")
    readonly property string cancel: qsTr("Cancel")
    readonly property string saveScreenshot: qsTr("Save screenshot")
    readonly property string copyScreenshot: qsTr("Copy screenshot")
    readonly property string relativeMouse: qsTr("Relative mouse")
    readonly property string relativeMouseHelp: qsTr("%1 releases the pointer.")
    readonly property string alwaysOnTop: qsTr("Always on top")
    readonly property string controlSettings: qsTr("Keyboard and mouse")
    readonly property string releaseKey: qsTr("Release key")
    readonly property string releaseKeyHelp: qsTr("Press and release this key on its own to give control back. Changing it ends any current control session.")
    readonly property string relativeMouseActive: qsTr("Relative mouse: the pointer stays in the picture.")

    readonly property string version: qsTr("Version %1")
    readonly property string licence: qsTr("Licence")
    readonly property string freeSoftware: qsTr("qKVM is free software under the GNU Affero General Public License, version 3 (%1). You may use it, study it, change it and share it.")
    readonly property string runsOn: qsTr("Runs on")
    readonly property string theseLibraries: qsTr("These libraries ship with it, each under its own licence, and their source is offered where each says.")
    readonly property string sourceAt: qsTr("Source: %1")

    // Which QKVM, and who.
    readonly property string addressOfYourQkvm: qsTr("Address of your qKVM")
    readonly property string orADisplayLink: qsTr("Or paste a display link here, for a screen nobody sits at.")
    readonly property string connect: qsTr("Connect")
    readonly property string signInTo: qsTr("Sign in to %1")
    readonly property string notEncrypted: qsTr("What you type goes to this qKVM unencrypted: the address is not an https:// one. Sign in this way only on a network you trust.")
    readonly property string isThisYourQkvm: qsTr("Is this your qKVM?")
    readonly property string certificateNotKnown: qsTr("%1 shows a certificate that Windows does not know, as a qKVM does that made its own. Nothing has been sent to it yet.")
    readonly property string itsCertificate: qsTr("Its certificate's SHA-256")
    readonly property string compareWithQkvm: qsTr("qKVM prints this when it starts, after \"certificate SHA-256:\". Where the two are alike, this is your qKVM.")
    readonly property string trustThisCertificate: qsTr("Trust this certificate")
    readonly property string anotherCertificate: qsTr("This qKVM shows another certificate than before")
    readonly property string certificateNotTheSame: qsTr("%1 is known here by another certificate. That happens when qKVM was set up anew or was given a new certificate. It is also what it looks like when something has come between this PC and your qKVM. Nothing is sent to it until you say so.")
    readonly property string itShowsNow: qsTr("It shows now")
    readonly property string itWasKnownBy: qsTr("It was known by")
    readonly property string trustTheNewCertificate: qsTr("Trust the new certificate")
    readonly property string userName: qsTr("User name")
    readonly property string password: qsTr("Password")
    readonly property string showPassword: qsTr("Show password")
    readonly property string hidePassword: qsTr("Hide password")
    readonly property string codeFromYourApp: qsTr("Code from your authenticator app")
    readonly property string keepSignedIn: qsTr("Keep this PC signed in for 30 days")
    readonly property string signIn: qsTr("Sign in")
    readonly property string anotherQkvm: qsTr("Another qKVM")
    readonly property string atTheQkvm: qsTr("on the qKVM at %1")
    readonly property string signOut: qsTr("Sign out")
    readonly property string lostContactAt: qsTr("Lost contact with qKVM at %1. Trying again…")
    readonly property string notEncryptedShort: qsTr("Not encrypted")
    readonly property string notEncryptedHint: qsTr("What is seen and typed here goes to this qKVM unencrypted: its address is not an https:// one.")
    readonly property string notEncryptedHintOwnWall: qsTr("A KVM of this wall is spoken to over plain HTTP: its password, and what is seen and typed there, can be read on the way. Its card says which, with http:// before its address.")
    readonly property string restartedLocked: qsTr("This qKVM was restarted locked, and reaches no machine until somebody has signed in.")
    readonly property string tryingAgainByItsLink: qsTr("This screen opens by its display link, and asks again by itself until its qKVM answers.")

    // The bar.
    readonly property string aboutQkvm: qsTr("About qKVM")
    readonly property string all: qsTr("All")
    readonly property string online: qsTr("Online")
    readonly property string noSignal: qsTr("No signal")
    readonly property string attention: qsTr("Attention")
    readonly property string findAKvm: qsTr("Find a KVM")
    readonly property string findByNameOrAddress: qsTr("Find a KVM by name or address")
    readonly property string allGroups: qsTr("All groups")
    readonly property string group: qsTr("Group")
    readonly property string order: qsTr("Order")
    readonly property string asArranged: qsTr("As arranged")
    readonly property string byName: qsTr("By name")
    readonly property string byGroup: qsTr("By group")
    readonly property string problemsFirst: qsTr("Problems first")
    readonly property string columns: qsTr("Columns")
    readonly property string fit: qsTr("Fit")
    readonly property string aNewPictureEvery: qsTr("A new picture every")
    readonly property string seconds: qsTr("%1 s")
    readonly property string paused: qsTr("Paused")
    readonly property string displayMode: qsTr("Display mode")
    readonly property string newWindow: qsTr("New window")
    readonly property string arrangeTheTiles: qsTr("Arrange the tiles…")
    readonly property string arrange: qsTr("Arrange")
    readonly property string arrangeHow: qsTr("Drag a tile to its place, or type the place it is to have.")
    readonly property string saveThisOrder: qsTr("Save this order")
    readonly property string onePlaceEarlier: qsTr("%1: one place earlier")
    readonly property string onePlaceLater: qsTr("%1: one place later")
    readonly property string placeOf: qsTr("Place of %1")
    readonly property string rotateGroups: qsTr("Rotate groups")
    readonly property string exitDisplay: qsTr("Exit (Esc)")
    readonly property string picturesArePaused: qsTr("No new pictures are asked for: paused. How the wall is shown")
    readonly property string view: qsTr("View")
    readonly property string howTheWallIsShown: qsTr("How the wall is shown: order, columns, how often, display mode")
    readonly property string select: qsTr("Select")
    readonly property string selectKvms: qsTr("Select KVMs")
    readonly property string selected: qsTr("%1 selected")
    readonly property string selectAll: qsTr("Select all")
    readonly property string clear: qsTr("Clear")
    readonly property string leaveSelection: qsTr("Leave selection")
    readonly property string bulkActions: qsTr("Bulk actions")
    readonly property string selectNamed: qsTr("Select %1")
    readonly property string runAction: qsTr("Run action…")
    readonly property string inTheOrderShown: qsTr("In the order shown")
    readonly property string howFast: qsTr("How fast")
    readonly property string stop: qsTr("Stop")
    readonly property string addKvms: qsTr("Add KVMs")
    readonly property string addKvmsOnThePage: qsTr("Add KVMs on the qKVM page, in your browser")
    // Discovery asks only the networks selected here, anonymously, and adds nothing itself.
    readonly property string discoverKvms: qsTr("Find devices")
    readonly property string discoveryHint: qsTr("Choose networks to search from this PC. No passwords are sent and no devices are added until you choose an address and check it.")
    readonly property string discoveryNetwork: qsTr("%1, %2 (%3 addresses)")
    readonly property string discoveryNoNetworks: qsTr("No active private IPv4 networks were found. Enter a reachable private address or range below.")
    readonly property string discoveryScope: qsTr("Additional subnet, range or address")
    readonly property string discoveryScopeHint: qsTr("Examples: 192.168.1.0/24, 192.168.1.10-192.168.1.40, or https://192.168.1.20:8443. Private IPv4 only; at most 4096 addresses per search. Larger adapter networks need a smaller range.")
    readonly property string discoveryConfirm: qsTr("This search will check %1 addresses. Review the selected networks, then start the search below.")
    readonly property string discoveryFound: qsTr("Devices found: %1")
    readonly property string discoveryKvmd: qsTr("PiKVM / Comet (kvmd)")
    readonly property string discoveryJetKvm: qsTr("JetKVM")
    readonly property string discoveryRedfish: qsTr("Server controller (Redfish)")
    readonly property string discoveryCheckAgain: qsTr("Sign-in and certificate will be checked when adding.")
    readonly property string discoveryPlainHttp: qsTr("HTTP connection; traffic is unencrypted.")
    readonly property string discoveryServerNeeded: qsTr("Use a qKVM server for this device family.")
    readonly property string discoveryUseAddress: qsTr("Use address")
    readonly property string discoveryProgress: qsTr("Checked %1 of %2 addresses · %3 devices found")
    readonly property string discoveryComplete: qsTr("Search complete · %1 devices found")
    readonly property string discoveryFailed: qsTr("Search ended before all addresses could be checked.")
    readonly property string discoveryStopped: qsTr("Search stopped after %1 of %2 addresses")
    readonly property string discoveryReady: qsTr("Select a network or enter an address, then start the search.")
    readonly property string discoverySearch: qsTr("Search")
    readonly property string discoverySearchConfirmed: qsTr("Search %1 addresses")

    readonly property string addAKvm: qsTr("Add a KVM")
    readonly property string addAKvmToThisPc: qsTr("Add a KVM to this PC's wall")
    readonly property string address: qsTr("Address")
    readonly property string addressOfAKvmHint: qsTr("IP address or name of the KVM, or of a server's management controller (BMC). GL.iNet Comet, PiKVM, JetKVM and Redfish controllers are spoken to by this program itself.")
    readonly property string none: qsTr("None")
    readonly property string groupHint: qsTr("Optional: a rack, a room, a customer. The wall can show one group at a time.")
    readonly property string itsCertificateIsRemembered: qsTr("It is contacted to check the login. Over HTTPS its certificate is remembered, so qKVM will refuse to send the password to anything else at that address.")
    readonly property string rememberedSha256: qsTr("Remembered SHA-256: %1")
    readonly property string checkAndAdd: qsTr("Check and add")
    readonly property string checking: qsTr("Checking…")
    readonly property string done: qsTr("Done")
    // The box that changes a KVM of this PC's wall, and its look at the KVM's certificate.
    readonly property string editNamed: qsTr("Edit %1")
    readonly property string name: qsTr("Name")
    readonly property string typeOfAKvm: qsTr("Type: GL.iNet Comet / PiKVM")
    readonly property string typeOfAJetKvm: qsTr("Type: JetKVM")
    readonly property string typeOfAServer: qsTr("Type: Server (Redfish)")
    readonly property string unchanged: qsTr("Unchanged")
    readonly property string leaveEmptyToKeep: qsTr("Leave empty to keep the saved one.")
    // A KVM's screen watches, as the page's Edit box has them (app.js).
    readonly property string noSignalFor: qsTr("No signal for")
    readonly property string screenUnchangedFor: qsTr("Screen unchanged for")
    readonly property string watchOff: qsTr("Off")
    readonly property string noSignalHint: qsTr("Minutes. Told once the computer has shown nothing that long, with its last picture.")
    readonly property string unchangedHint: qsTr("Minutes, while its card is on a screen somewhere. Not the same as hung: an idle screen does not change either.")
    readonly property string noCertificateRememberedYet: qsTr("No certificate remembered yet.")
    readonly property string answersOverHttpsNow: qsTr("It answers over HTTPS now.")
    // Where a JetKVM says by its number that it is one of the wall already, as on the page.
    readonly property string keepBoth: qsTr("Keep both")
    readonly property string spokenToOverPlainHttp: qsTr("This KVM is spoken to over plain HTTP: its password, and what is seen and typed there, can be read on the way.")
    readonly property string remove: qsTr("Remove")
    readonly property string reviewCertificate: qsTr("Review certificate")
    readonly property string save: qsTr("Save")
    readonly property string savedNamed: qsTr("%1 saved")
    readonly property string removeNamed: qsTr("Remove %1?")
    readonly property string whatRemovingDoes: qsTr("The KVM disappears from the wall and its saved password is erased. The KVM itself is not touched.")
    readonly property string certificateOf: qsTr("%1 certificate")
    readonly property string rememberedByQkvm: qsTr("Remembered by qKVM:")
    readonly property string noneYet: qsTr("none yet")
    readonly property string presentedRightNow: qsTr("Presented by the device right now:")
    readonly property string theyMatch: qsTr("They match. qKVM is talking to the same KVM as before.")
    readonly property string aNewCertificateOrAnotherDevice: qsTr("A KVM gets a new certificate after a factory reset or some firmware updates. If neither happened, something else may be answering at this address: do not trust it.")
    readonly property string certificateTrusted: qsTr("%1: certificate trusted")
    readonly property string presentedAtTheNewAddress: qsTr("Presented right now at the new address, %1:")
    readonly property string shownAnotherThanSaidYesTo: qsTr("What answers at that address shows another certificate now than the one that was just said yes to.")
    readonly property string changedThen: qsTr("Changed. %1")
    readonly property string noCertificateToCompareWith: qsTr("This KVM is known by no certificate yet, so there is none here to compare this one with: compare it with what the KVM's own page shows. Saying yes sends the password to what shows it, and the KVM is known by it from then on.")
    readonly property string knownByWindowsWordSoNoneToCompareWith: qsTr("This KVM is known by Windows' word and by no certificate of its own, so there is none here to compare this one with: compare it with what the KVM's own page shows. Saying yes sends the password to what shows it, and the KVM is known by this certificate from then on, and no longer by Windows' word.")
    readonly property string windowsTrustsWhatIsShownThere: qsTr("Windows trusts what is shown there for this name")
    readonly property string trustWindowsWord: qsTr("Trust Windows' word")
    readonly property string yesMeansWindowsWordNotItsCertificate: qsTr("Saying yes means that this KVM is known by Windows' word from then on, and no longer by its certificate.")
    readonly property string yesMeansWindowsWord: qsTr("Saying yes means that this KVM is known by Windows' word from then on.")
    readonly property string showsTheCertificateOf: qsTr("What answers at this address shows the certificate of %1, which is on this wall. Either that KVM answers here now, or the two were given the same certificate: then each needs one of its own before both can be on the wall.")
    readonly property string oneThenAnother: qsTr("%1. %2")
    readonly property string plainHttpThereShowsNothing: qsTr("Plain HTTP at %1: nothing shows who answers there, and its password, and what is seen and typed, can be read on the way")
    readonly property string usePlainHttp: qsTr("Use plain HTTP")
    readonly property string windowsWordForItsCertificate: qsTr("Windows' word for the certificate it shows")
    readonly property string afterWindowsWordNotTrusted: qsTr("Windows' word was said yes to for that address, and Windows does not trust what answers there now. It shows this certificate.")
    readonly property string afterWindowsWordAnotherTrusted: qsTr("What answers at that address is not what it was when Windows' word for it was said yes to. Windows trusts what it shows now, too.")
    readonly property string thisPc: qsTr("This PC")
    readonly property string thisPcsOwnWall: qsTr("The KVMs on this wall were added on this PC, which speaks to them itself. No qKVM server is used.")
    readonly property string connectToAQkvmServer: qsTr("Connect to a qKVM server")
    readonly property string thisPcsOwnKvms: qsTr("This PC's own KVMs")
    readonly property string noKvmsYet: qsTr("No KVMs on this wall yet")
    readonly property string noKvmsYetHint: qsTr("Add a KVM by its address and its password. Or connect to a qKVM server, where you have one, and see its wall here.")
    readonly property string leaveFullscreen: qsTr("Leave fullscreen")
    readonly property string fullscreen: qsTr("Fullscreen")
    readonly property string theme: qsTr("Theme")
    readonly property string settings: qsTr("Settings")
    // The bar's menu of what gave way in a narrow window.
    readonly property string more: qsTr("More")
    readonly property string startWithWindows: qsTr("Start qKVM when I sign in to Windows")
    readonly property string keepRunningInTray: qsTr("Keep running in the notification area when the window is closed")
    readonly property string tellMeNews: qsTr("Tell me when a KVM goes away or comes back while the window is out of sight")
    readonly property string themeDark: qsTr("Dark")
    readonly property string themeLight: qsTr("Light")
    readonly property string themeAsWindows: qsTr("As Windows is set")
    readonly property string yourAccount: qsTr("Your account")
    // What the server lets an account do, in the page's words (app.js, ROLE_NAMES).
    readonly property string admin: qsTr("Admin")
    readonly property string operator: qsTr("Operator")
    readonly property string viewer: qsTr("Viewer")
    readonly property string aScreenOnTheWall: qsTr("A screen on the wall")
    // The wall as a file: the page's words for it.
    readonly property string theWallAsAFile: qsTr("The KVMs and servers on this wall, the saved actions and the alert settings can be written to a file, and a file can be taken in here or on another qKVM.")
    readonly property string exportDots: qsTr("Export…")
    readonly property string importDots: qsTr("Import…")
    readonly property string exportToAFile: qsTr("Export to a file")
    readonly property string importFromAFile: qsTr("Import from a file")
    readonly property string passphraseForTheFile: qsTr("Passphrase for the file")
    readonly property string whatAPassphraseDoes: qsTr("With a passphrase the file is encrypted, and holds the passwords of the devices, the saved texts and the address alerts go to. Left empty, it is a plain file with none of those in it.")
    readonly property string yourPassword: qsTr("Your password")
    readonly property string whatIsInNoFile: qsTr("Accounts, tokens and two-factor keys are not in the file: each belongs to the qKVM it was made on.")
    readonly property string saveDots: qsTr("Save…")
    readonly property string chooseAFile: qsTr("Choose a file…")
    readonly property string aFileQkvmExported: qsTr("A .qkvm or .json file that qKVM exported.")
    readonly property string passphraseOfTheFile: qsTr("Passphrase of the file")
    readonly property string readTheFile: qsTr("Read the file")
    readonly property string takeIn: qsTr("Take in")
    readonly property string thisFileHoldsPasswordsInPlainText: qsTr("This file holds passwords in plain text. Whoever wrote it chose to; delete it once it is taken in.")

    // A machine's virtual media through its qKVM, as the page's Media box says it.
    readonly property string mediaImages: qsTr("Images")
    readonly property string mediaUpload: qsTr("Upload…")
    readonly property string mediaUploadHint: qsTr("Upload an image from this computer")
    readonly property string mediaDelete: qsTr("Delete")
    readonly property string mediaDeleteHint: qsTr("Delete the image that is picked")
    readonly property string mediaMount: qsTr("Mount")
    readonly property string mediaUnmount: qsTr("Unmount")
    readonly property string mediaInItsDrive: qsTr("In its drive")
    readonly property string mediaMountAs: qsTr("Mount as")
    readonly property string mediaStartFromIt: qsTr("Start from it")
    readonly property string mediaRestartItNow: qsTr("Restart it now")
    readonly property string mediaRestartItNowHint: qsTr("At once: unsaved work on it is lost. One that is stopped is started.")
    readonly property string nothingInThisFileToTakeIn: qsTr("There is nothing in this file to take in.")
    readonly property string showWhatItTypes: qsTr("show what it types")
    readonly property string hideIt: qsTr("hide it")
    // This PC's own wall as a file: the program makes the file and reads one itself.
    readonly property string thisPcsWallAsAFile: qsTr("Choose KVMs, saved actions and wall settings to keep in a file or bring to this PC. A qKVM server writes and reads the same file.")
    readonly property string whatAPassphraseDoesHere: qsTr("A passphrase encrypts the file, including device passwords and saved text actions. Left empty, the file omits passwords and all text actions.")
    readonly property string makeTheFile: qsTr("Make the file")
    readonly property string makingTheFile: qsTr("Making the file…")
    readonly property string theFileIsMadeEncrypted: qsTr("The file is made, encrypted with its passphrase, and is not saved yet. Before you save it: what is not in it, and what a qKVM server will read differently.")
    readonly property string theFileIsMadePlain: qsTr("The file is made, plain and without passwords, and is not saved yet. Before you save it: what is not in it, and what a qKVM server will read differently.")
    readonly property string readingTheFile: qsTr("Reading the file…")
    readonly property string nothingOnTheWallHasChangedYet: qsTr("Nothing on the wall has changed yet, and nothing was asked at any address. Each KVM that is taken is asked what it is before a password goes to it.")
    readonly property string takeOneKvm: qsTr("Take 1 KVM")
    readonly property string takeKvms: qsTr("Take %1 KVMs")
    readonly property string details: qsTr("Details")
    readonly property string toBeOpenedBeforeAnythingIsTaken: qsTr("To be opened before anything is taken.")
    readonly property string sha256OfTheCertificateTheFileNames: qsTr("SHA-256 of the certificate the file names, to compare with what the KVM's own page shows:")
    readonly property string notTakenColon: qsTr("Not taken:")

    // The wall.
    readonly property string pressEnterToEnlarge: qsTr("%1. Press Enter to enlarge.")
    readonly property string actions: qsTr("Actions")
    readonly property string carryOn: qsTr("Continue")
    readonly property string sendKeys: qsTr("Send keys")
    readonly property string pressedTogether: qsTr("Pressed together on the connected computer, then released.")
    readonly property string key: qsTr("Key")
    readonly property string nothingChosen: qsTr("Nothing chosen")
    readonly property string typeText: qsTr("Type text")
    readonly property string text: qsTr("Text")
    readonly property string typedAsOnItsKeyboard: qsTr("Typed on the connected computer as if on its keyboard (US layout; on a JetKVM, the layout set on its own page). It is not saved, and its content is not written to the activity log.")
    readonly property string pressEnterAfterwards: qsTr("Press Enter afterwards")
    readonly property string actionsFor: qsTr("Actions for %1")
    readonly property string openConsole: qsTr("Open console")
    readonly property string openTheConsoleOf: qsTr("Open the console of %1")
    readonly property string live: qsTr("Live")
    readonly property string previousKvm: qsTr("Previous KVM")
    readonly property string nextKvm: qsTr("Next KVM")
    readonly property string server: qsTr("Server")
    readonly property string locatorLightOn: qsTr("Locator light on")
    readonly property string powerOn: qsTr("On")
    readonly property string powerOff: qsTr("Off")
    readonly property string nothingToShow: qsTr("Nothing to show")
    readonly property string noKvmsMatch: qsTr("No KVMs match")
    readonly property string showAll: qsTr("Show all")

    // Keyboard and mouse of a machine that is shown large.
    readonly property string control: qsTr("Control")
    readonly property string takeKeyboardAndMouseOf: qsTr("Take keyboard and mouse of %1")
    readonly property string keyboardAndMouseGoTo: qsTr("Keyboard and mouse go to %1.")
    readonly property string givesThemBack: qsTr("%1 gives them back.")
    readonly property string ctrlAltDel: qsTr("Ctrl+Alt+Del")
    readonly property string pressCtrlAltDelOn: qsTr("Press Ctrl+Alt+Del on %1")
    readonly property string altF4: qsTr("Alt+F4")
    // Asked before they go, as the page asks: the machine set off as a sentence sets a name off.
    readonly property string ctrlAltDelOn: qsTr("Ctrl+Alt+Del on %1?")
    readonly property string altF4On: qsTr("Alt+F4 on %1? It closes the window that is in front there.")
    readonly property string giveBack: qsTr("Give back")
    readonly property string giveBackToThisPc: qsTr("Give keyboard and mouse back to this PC")
    readonly property string givesThemBackShort: qsTr("%1: back")
    readonly property string askingForKeyboardAndMouseOf: qsTr("Asking %1 for its keyboard and mouse…")
    readonly property string stopAsking: qsTr("Stop asking")
    readonly property string stopAskingFor: qsTr("Stop asking %1 for its keyboard and mouse")
    readonly property string asking: qsTr("Asking...")
    // Control's words while somebody else has them, as the page's (app.js, the label of "focus-control").
    readonly property string takeOverFrom: qsTr("Take over from %1")
    readonly property string takeOverFromAfterConfirmation: qsTr("Take over from %1 after confirmation")
    readonly property string controlledBy: qsTr("Controlled by %1")

    // The settings box, in its sections: the page's groups and sections, of what is this PC's wall's
    // and what is the program's. (A QKVM server's own settings are on its page.)
    readonly property string thisPcsWall: qsTr("This PC's wall")
    readonly property string thisProgram: qsTr("This program")
    readonly property string kvms: qsTr("KVMs")
    readonly property string kvmsAbout: qsTr("The KVMs of this PC's wall: add, change, remove.")
    readonly property string exportAndImport: qsTr("Export and import")
    readonly property string exportAndImportAbout: qsTr("This PC's wall as a file: to keep, or to take to another PC or a qKVM server.")
    // A server's wall as a file, in the page's words (app.js, filesTab).
    readonly property string theServersWall: qsTr("The server's wall")
    readonly property string theWallAsAFileAbout: qsTr("The wall as a file: to keep, or to take to another qKVM.")
    readonly property string serverEncryptedFileHolds: qsTr("Made with a passphrase you choose. It holds the passwords of the devices, the saved texts and the address alerts go to, and opens with that passphrase only.")
    readonly property string serverPlainFileHolds: qsTr("Made without a passphrase. The same, with every password and saved text left out: its devices ask for their password where they are taken in.")
    readonly property string serverAddsWhatIsNotHereYetHow: qsTr("Nothing on this wall is changed or removed by it. The alert and wall settings are replaced only where you tick them.")
    readonly property string whatAServersFileHolds: qsTr("Accounts, tokens, two-factor keys and the recovery key are in no such file: each belongs to the qKVM it was made on. To keep a whole qKVM, accounts and all, keep a copy of its data folder.")
    readonly property string general: qsTr("General")
    readonly property string generalAbout: qsTr("How the program behaves on this PC, and how it looks.")
    readonly property string video: qsTr("Video")
    readonly property string videoAbout: qsTr("How live pictures are decoded and drawn on this PC.")
    readonly property string decoding: qsTr("Decoding")
    readonly property string hardwareAcceleration: qsTr("Hardware acceleration")
    readonly property string hardwareAccelerationHint: qsTr("The graphics card decodes H.264 more smoothly and leaves the processor free. Its driver does that inside qKVM. Automatic uses it for the machine shown large. A change takes effect within seconds.")
    readonly property string automatic: qsTr("Automatic")
    readonly property string switchedOn: qsTr("On")
    readonly property string switchedOff: qsTr("Off")
    readonly property string graphicsCard: qsTr("Graphics card")
    readonly property string useTheCard: qsTr("Draw and decode on")
    readonly property string cardHere: qsTr("On this PC: %1")
    readonly property string cardWithoutTheScreen: qsTr("The screen is on another graphics adapter here: Windows carries every picture to it.")
    readonly property string mostPowerful: qsTr("The most powerful")
    readonly property string powerSaving: qsTr("The one that saves power")
    readonly property string asSetInWindows: qsTr("As set in Windows")
    readonly property string cardAtTheNextStart: qsTr("Takes effect when qKVM starts again.")
    readonly property string aboutAbout: qsTr("What this is, whose it is, and under which licence.")
    // The KVMs, as a list.
    readonly property string connection: qsTr("Connection")
    readonly property string edit: qsTr("Edit")
    readonly property string removeOne: qsTr("Remove %1")
    readonly property string kvmdKind: qsTr("GL.iNet Comet / PiKVM")
    readonly property string jetKvmKind: qsTr("JetKVM")
    readonly property string serverControllerKind: qsTr("Server (Redfish)")
    readonly property string knownByItsCertificate: qsTr("Known by its certificate")
    readonly property string knownByWindowsWord: qsTr("Known by Windows' word")
    readonly property string plainHttpInTheClear: qsTr("Plain HTTP: its password goes in the clear")
    readonly property string sameDeviceAs: qsTr("Same device as %1")
    readonly property string noPasswordSaved: qsTr("No password saved")
    // (A JetKVM out of its box runs without one: nothing is wrong with that.)
    readonly property string noPasswordFineWithout: qsTr("No password: fine if it runs without one")
    readonly property string takenOffThisWall: qsTr("Taken off this wall")
    readonly property string noKvmsYetAddTheFirst: qsTr("No KVMs yet. Add the first one with the button above.")
    readonly property string takenOffMeanwhile: qsTr("%1 was taken off this wall meanwhile.")
    // (A sign-in to that KVM with the same password is out, another window's or the wall's own: one at a time.)
    readonly property string waitsForAnEarlierSignIn: qsTr("Waiting for an earlier sign-in to this KVM to be answered before this one is sent.")
    // This PC's wall as a file: what the page says of the two kinds of file and of taking one in.
    readonly property string fileIsOneOfTwoKinds: qsTr("A file is one of two kinds")
    readonly property string encryptedFile: qsTr("Encrypted (.qkvm)")
    readonly property string encryptedFileHolds: qsTr("Made with a passphrase you choose. Selected KVMs include passwords and saved actions include their text. It opens with that passphrase only.")
    readonly property string plainFile: qsTr("Plain (.json)")
    readonly property string plainFileHolds: qsTr("Made without a passphrase. Device passwords and all saved text actions are left out. Keyboard actions and selected wall settings can still be carried.")
    readonly property string takingAFileIn: qsTr("Taking a file in")
    readonly property string addsWhatIsNotHereYet: qsTr("Adds what is not here yet")
    readonly property string addsWhatIsNotHereYetHow: qsTr("Existing devices and actions are kept. Actions with the same name are skipped. Each new KVM is identified before a password goes to it.")
    readonly property string actionsAlertsAndSettings: qsTr("Saved actions, alerts and a wall's settings")
    readonly property string stayInTheFile: qsTr("Review each saved action before importing it. Wall settings replace the picture interval and automatic update checks only when selected. Server alert settings stay in the file.")
    readonly property string whatAFileOfThisWallHolds: qsTr("Accounts, sessions, the app password, window layout and this PC's other preferences are not portable. Previewing a file changes nothing.")
    // The program's own.
    readonly property string look: qsTr("Look")
    readonly property string whileTheWindowIsOutOfSight: qsTr("While the window is out of sight")
    readonly property string windows: qsTr("Windows")
    readonly property string qkvmServer: qsTr("qKVM server")
    readonly property string theQkvmServerAt: qsTr("The qKVM server at %1")
    readonly property string managedOnItsPage: qsTr("Its machines, its accounts and its own settings are managed on its page, in your browser.")
    readonly property string openItsPage: qsTr("Open its page")
    readonly property string whatThisProgramIs: qsTr("The wall of screens of qKVM, and keyboard and mouse on one machine, as a program of this PC's own.")
    readonly property string whereToFindIt: qsTr("Where to find it")
    readonly property string website: qsTr("Website")
    readonly property string qkvmSite: qsTr("qkvm.com")
    readonly property string itsSource: qsTr("Its source")
    readonly property string library: qsTr("Library")
    readonly property string versionOf: qsTr("Version")
    // The live connection's observed video; no claim about device capture or network latency.
    readonly property string videoDetails: qsTr("Video details")
    readonly property string videoWebRtc: qsTr("Native H.264 over WebRTC")
    readonly property string videoMjpeg: qsTr("JPEG stream")
    readonly property string videoSnapshots: qsTr("Individual pictures")
    readonly property string videoReconnecting: qsTr("Reconnecting video")
    readonly property string hideVideoDetails: qsTr("Hide video details")
    readonly property string videoWaiting: qsTr("Waiting for a live connection")
    readonly property string videoConnected: qsTr("Live connection")
    readonly property string videoUnknown: qsTr("Not measured yet")
    readonly property string videoDecoderGpu: qsTr("Graphics card")
    readonly property string videoDecoderSoftware: qsTr("Software")
    readonly property string videoFormat: qsTr("%1 · %2")
    readonly property string videoResolution: qsTr("%1 × %2")
    readonly property string videoReceiveRate: qsTr("Received: %1 frames/s · %2 Mbit/s payload")
    readonly property string videoReceiveAge: qsTr("Last frame received %1 ms ago")
    readonly property string videoDecoder: qsTr("Decoder: %1")
    readonly property string videoShownRate: qsTr("Shown: %1 frames/s · %2 ms from arrival to screen")
    readonly property string videoNotShown: qsTr("Not shown: %1 made way for a newer one · %2 dropped behind · %3 rejected")
    readonly property string videoLateSteps: qsTr("Gaps over 38 ms: %1 on screen · %2 as they arrived")
    readonly property string videoDrawnOn: qsTr("Drawn on: %1")
    readonly property string videoDecodeTime: qsTr("Average decode: %1 ms")
    readonly property string videoMeasurementsHow: qsTr("An unchanged screen can show 0 frames/s. Time since the last picture arrived is not keyboard or mouse response time.")

    // The words the server and this program share (the web's W8): its states and their labels, a device's codes and
    // the state each is shown under, the wall's refusals with their HTTP statuses, the kinds of device and the words
    // of `can`, as the server's app/qkvm/static/words.json has them. A copy of that file is kept beside this one
    // (ui/words.json; ui/words-from.txt says from where), and ui/tests/test_words.cpp fails on a word of it these
    // lack or name otherwise. (The page's three dots are the one sign here.)
    readonly property var states: ({
            "connecting": qsTr("Connecting…"),
            "online": qsTr("Online"),
            "no_signal": qsTr("No signal"),
            "offline": qsTr("Offline"),
            "auth": qsTr("Login failed"),
            "error": qsTr("Problem"),
            "busy": qsTr("In use elsewhere")
        })
    readonly property var serverStates: ({
            "no_signal": qsTr("Powered off")
        })
    readonly property var codes: ({
            "unreachable": "offline",
            "timeout": "offline",
            "dropped": "offline",
            "auth_failed": "auth",
            "no_password": "auth",
            "no_stream": "connecting",
            "in_use": "busy",
            "no_sign_in": "error",
            "cert_mismatch": "error",
            "cert_invalid": "error",
            "plain_only": "error",
            "another_machine": "error",
            "unknown_kind": "error",
            "not_set_up": "error",
            "bad_response": "error",
            "missing_driver": "error",
            "console_held": "error",
            "console_time_up": "error",
            "no_picture": "error",
            "health": "error",
            "internal": "error"
        })
    readonly property var refusals: ({
            "disconnected": [400],
            "bad_code": [401],
            "bad_key": [401],
            "bad_password": [401],
            "bad_recovery": [401],
            "code_required": [401],
            "unauthorized": [401],
            "bad_setup_code": [403],
            "forbidden": [403],
            "not_allowed": [403],
            "stopped": [403],
            "suspended": [403],
            "totp_required": [403],
            "no_console": [404],
            "no_live": [404],
            "no_virtual_media": [404],
            "no_media": [404],
            "no_picture": [404],
            "not_found": [404],
            "timeout": [408, 504],
            "busy": [409, 429, 503],
            "cert_mismatch": [409, 502],
            "changed": [409],
            "conflict": [409],
            "console_held": [409],
            "container": [409],
            "duplicate": [409],
            "enabled": [409],
            "in_use": [409],
            "no_input": [409],
            "initialized": [409],
            "last_admin": [409],
            "locked": [409, 503],
            "no_password": [409, 502],
            "no_second_step": [409],
            "no_text": [409],
            "no_webhook": [409],
            "not_enabled": [409],
            "not_initialized": [409],
            "not_started": [409],
            "own_account": [409],
            "own_first": [409],
            "own_role": [409],
            "same_device": [409],
            "same_host": [409],
            "unknown_kind": [409],
            "yourself": [409],
            "length_required": [411],
            "too_large": [413, 422],
            "another_machine": [422, 502],
            "damaged": [422],
            "invalid": [422],
            "name_required": [422],
            "newer": [422],
            "not_a_file": [422],
            "passphrase_needed": [422],
            "password_needed": [422],
            "wrong_passphrase": [422],
            "rate_limited": [429],
            "internal": [500],
            "missing_driver": [501],
            "action_failed": [409, 502],
            "auth_failed": [502],
            "bad_response": [502],
            "cert_invalid": [502],
            "console_time_up": [502],
            "dropped": [502],
            "not_delivered": [502],
            "not_set_up": [502],
            "plain_only": [502],
            "unfit": [502],
            "unreachable": [502],
            "update_failed": [502],
            "activity_unavailable": [503],
            "history_unavailable": [503],
            "no_sign_in": [503],
            "no_stream": [503],
            "no_certificate": [507],
            "no_space": [507],
            "not_saved": [507]
        })
    readonly property var kinds: ({
            "kvmd": qsTr("GL.iNet Comet / PiKVM"),
            "jetkvm": qsTr("JetKVM"),
            "redfish": qsTr("Server (Redfish)"),
            "amt": qsTr("PC with Intel AMT"),
            "vnc": qsTr("VNC server"),
            "proxmox": qsTr("Proxmox VM")
        })
    readonly property var can: ({
            "screen": qsTr("Screen"),
            "keys": qsTr("Keys"),
            "text": qsTr("Type text"),
            "jiggler": qsTr("Keep awake"),
            "power": qsTr("Power button"),
            "reset": qsTr("Reset button"),
            "fingerbot": qsTr("Fingerbot"),
            "power_state": qsTr("On and off"),
            "wol": qsTr("Wake-on-LAN"),
            "ports": qsTr("Ports"),
            "reboot": qsTr("Reboot"),
            "resume": qsTr("Show here"),
            "locator": qsTr("Locator light"),
            "console": qsTr("Console, signed in"),
            "live": qsTr("Live picture"),
            "control": qsTr("Keyboard and mouse"),
            "bios_setup": qsTr("Start into BIOS setup"),
            "media": qsTr("Virtual media")
        })
}
