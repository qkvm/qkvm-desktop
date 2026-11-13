#pragma once

#include <functional>
#include <memory>
#include <thread>

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QString>

#include <qkvm/base/result.h>
#include <qkvm/core/exchange.h>

namespace qkvm::core::exchange {

// Reads and writes a file of the wall off the interface thread: a passphrase's work takes about
// 128 MiB and half a second or more, and nothing waits on the interface thread (DESIGN.md
// section 10).
//
// On the interface thread, as everything of core is. read() and write() come back at once; what
// came of it is handed to `done` later, on the same thread, exactly once, unless this is gone by
// then, or whoever asked is: `asker` is whose answer it is, an object of the interface thread,
// and once that is gone `done` is not called (it may hold on to what went with it). Without an
// asker nobody could be told, and nothing is worked on. Whoever is told may ask for the next
// file there and then; but does not end this there and then, only a turn of the loop later
// (deleteLater), as with everything that tells on its own thread.
//
// One file at a time, in the whole program and not for each of these: each takes its 128 MiB,
// or twice that where a file asks for it, and nothing here lets them pile up. What is asked
// while another is worked on, by this or by another FileWork, is refused there and then ("busy",
// the server's word for it), through `done` like everything else, a turn of the loop later.
//
// Where the work cannot be begun at all, because this PC has no memory for it or Windows no
// thread, or because something was thrown there that nobody thought of, that is said through
// `done` too ("desktop:no_memory", "desktop:failed"), the program's one place is given back,
// and the next file is worked on as if nothing had been. (Said with what little memory a
// sentence takes: where there is not even that, what the library threw leaves read() or
// write() as it is, with the place given back.)
//
// When this goes while a file is worked on, the work is told to stop and is waited for, its
// memory is wiped and given back, and nothing of it is left running that would keep the
// program from ending. How long that wait is, is NOT "a moment" whatever the file:
// * The passphrase's work, which is the long part of an ordinary file, ends at its next step:
//   it looks at every one of them whether anybody still waits, and a step is a few millionths
//   of a second.
// * Nothing else is cut short. Not what stands before the passphrase's work and behind it: the
//   reading of the file's text (up to 5.3 million signs) and of what it holds (up to 4 million,
//   with up to 2000 devices, of whose certificates Windows is asked); and where a file is
//   written, the writing out of as much, the reading of it once more, and the sealing. And not
//   the wiping of the memory the work had: up to 257 MiB.
// So the true bound is what a file of the largest size takes without its passphrase's work,
// and the wiping. Where this was written: about a second to write such a file and half a
// second or a little more to read one in the build that is worked with, and about a tenth of a
// second each in the build that is handed out (core/tests/test_exchange_written.cpp says it
// at every run); and 60 to 80 ms from the word to stop until work at the largest cost a file
// may ask for had ended, its memory wiped (core/tests/test_scrypt.cpp, the same).
//
// When only the asker goes, the work runs to its end and is told to nobody; the next file
// waits for that.
//
// The passphrase is taken in as its bytes there and then; those are wiped when the work has
// ended, whichever way.
class FileWork : public QObject {
    Q_OBJECT

public:
    // What is done just before the thread for a file is begun: nothing. A test hands in
    // something that throws what the beginning of a thread throws where there is none to be
    // had, or no memory for one.
    using BeforeTheThread = std::function<void()>;

    explicit FileWork(QObject* parent = nullptr);
    // With the cost a file is written with, and the passphrase's work, of a test's choosing.
    explicit FileWork(Sealing sealing, QObject* parent = nullptr);
    FileWork(Sealing sealing, BeforeTheThread beforeTheThread, QObject* parent = nullptr);
    ~FileWork() override;

    // As exchange::read and exchange::write, with their refusals.
    void read(const QByteArray& file, const QString& passphrase, QObject* asker,
              std::function<void(base::Result<Content>)> done);
    void write(const Content& content, const QString& passphrase, QObject* asker,
               std::function<void(base::Result<Written>)> done);

    // A file is being worked on, by this or by another FileWork of the program, and another
    // would be refused.
    bool isBusy() const;

private:
    template <typename Answer, typename What>
    void work(QObject* asker, What what, std::function<void(base::Result<Answer>)> done);

    Sealing m_sealing;
    BeforeTheThread m_beforeTheThread;
    // This one has the program's one place, and gives it back when its file is told or it goes.
    bool m_working = false;
    // Whoever is told what came of the file that is worked on, and whose answer it is. Kept
    // here, and never handed to the thread that does the work: it is called on this thread and
    // let go on this thread, whatever it holds on to.
    std::shared_ptr<void> m_told;
    QPointer<QObject> m_asker;
    // Last, so that it goes first: it is told to stop and waited for before anything it uses goes.
    std::jthread m_thread;
};

} // namespace qkvm::core::exchange
