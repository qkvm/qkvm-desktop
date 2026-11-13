#pragma once

#include <functional>

#include <QByteArray>
#include <QString>

#include <qkvm/base/result.h>

namespace qkvm::ui {

// What the boxes for a wall as a file share, whoever makes the file and reads it (a QKVM server:
// files_view.h; the program itself: own_files_view.h): how somebody says where a file is kept
// and which one is opened, and the file itself on the disk, read whole up to a limit and written
// whole or not at all.

// How a file is chosen on this PC: Windows' own boxes in the program (win/file_dialogs.h), a
// test's own in a test. Each gives the file's whole name; nothing where whoever was asked said
// no; and why not, where they could not be asked. The program goes on while they are asked.
// Where a file is saved, the name that is suggested says which of the two kinds of file it is,
// by its ending, and only that kind is offered.
struct FileDoor {
    std::function<base::Result<QString>(const QString& suggested)> whereToSave;
    std::function<base::Result<QString>()> whichToOpen;
};

// The name a file of the wall is offered to be kept under, as the page names one: "qkvm-" and
// the day, and ".qkvm" for one that is encrypted, ".json" for a plain one.
QString suggestedFileName(bool encrypted);

// A name as Windows takes it: without the dots and blanks at its end, which Windows drops of the
// last part of a name before it looks for a file of that name, or makes one. So "wall.qkvm." and
// "wall.qkvm . " name the file "wall.qkvm". (That is what Windows' own reckoning of a name,
// GetFullPathNameW, makes of them, which a test shows. That Windows' Save box, where it asked
// whether to write over a file of the name it hands back, asked it of that file, is taken from
// how Windows looks for a file: no test here shows the box itself, nor what it hands back for a
// name that was typed with a dot or a blank at its end.)
QString asWindowsTakesIt(const QString& path);

// The name a file of the wall is kept under, made to say what the file is, whatever was chosen
// or typed in Windows' box: it ends in ".qkvm" where the file is encrypted, and in ".json" where
// it is plain. It is taken as Windows takes it first (asWindowsTakesIt). A name that ends so then
// is handed back so. One that ends in the other of the two has that taken off (and what stood
// before it is looked at again: "wall.qkvm.json", encrypted, is "wall.qkvm"); one that ends in
// anything else, or in nothing, is given the right ending after what it has. (An ending is
// looked at without regard to large and small letters, as Windows looks at a name.)
QString namedAsWhatItIs(const QString& path, bool encrypted);

// A file that was chosen, read whole: its bytes as they are. Refused, each with a sentence to
// show: one that cannot be opened ("unreadable"), and one that has more than `atMostBytes`
// ("too_large": never more than that and one byte is read of it, so that a film, a disk image or
// something that only calls itself a file and has no end is not read in).
base::Result<QByteArray> readChosenFile(const QString& path, qsizetype atMostBytes);

// Writes a file whole, or not at all: a file with half a wall in it is worse than none. What
// stood under that name stays as it was where the writing fails ("unwritten", with a sentence).
base::Result<void> writeWholeFile(const QString& path, const QByteArray& text);

// What a window's corner says of a file that was kept, in the page's words: that an encrypted
// one opens with its passphrase only; or that a plain one is without passwords. With the name it
// was written under, where whoever wrote it says it ("Exported to wall.qkvm"): this PC's box
// does, as the name may be another than Windows' box was given.
QString saidOfAFileThatWasKept(bool encrypted, const QString& name = {});

} // namespace qkvm::ui
