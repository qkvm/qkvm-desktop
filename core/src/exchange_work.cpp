#include <qkvm/core/exchange_work.h>

#include <atomic>
#include <memory>
#include <new>
#include <string>
#include <system_error>
#include <utility>

#include <QCoreApplication>

namespace qkvm::core::exchange {

namespace {

QString words(const char* text)
{
    return QCoreApplication::translate("Exchange", text);
}

base::Error busy()
{
    return {"busy", words("Another file is being worked on. Try again in a moment.").toStdString()};
}

// What is told where the work ended in a way that nothing of ours ends: something threw.
base::Error failed()
{
    return {"desktop:failed", words("The file could not be worked on.").toStdString()};
}

// What is told where the work could not be begun, or could not go on, for want of memory: in
// the code the passphrase's work has for it.
base::Error noMemory()
{
    return {scrypt::noMemory, words("This PC does not have the memory free to work on the file. Close another "
                                    "program and try again.")
                                  .toStdString()};
}

// And where Windows had no thread for it.
base::Error noThread()
{
    return {"desktop:failed",
            words("Windows could not begin the work on the file. Try again in a moment.").toStdString()};
}

// The one place the program has for a file that is worked on: taken by the FileWork that works
// on one, and given back by it when its file is told or it goes. (Each FileWork had a place of
// its own once, and two of them did the work of two passphrases at a time: twice 257 MiB.)
std::atomic<bool> aFileIsWorkedOn{false};

} // namespace

FileWork::FileWork(QObject* parent) : FileWork(Sealing(), parent) {}

FileWork::FileWork(Sealing sealing, QObject* parent) : FileWork(std::move(sealing), BeforeTheThread(), parent) {}

FileWork::FileWork(Sealing sealing, BeforeTheThread beforeTheThread, QObject* parent)
    : QObject(parent), m_sealing(std::move(sealing)), m_beforeTheThread(std::move(beforeTheThread))
{}

FileWork::~FileWork()
{
    // Told to stop, and waited for, while everything it may still touch is here. What it has
    // put on its way to this thread meanwhile goes with the object, undelivered.
    m_thread.request_stop();
    if (m_thread.joinable()) {
        m_thread.join();
    }
    if (m_working) {
        aFileIsWorkedOn.store(false);
    }
}

bool FileWork::isBusy() const
{
    return aFileIsWorkedOn.load();
}

template <typename Answer, typename What>
void FileWork::work(QObject* asker, What what, std::function<void(base::Result<Answer>)> done)
{
    using Told = std::function<void(base::Result<Answer>)>;
    if (asker == nullptr) {
        return;
    }
    const QPointer<QObject> whom(asker);
    // A refusal is told as an answer is: a turn of the loop later, on this thread, and to nobody
    // once this is gone or whoever asked is.
    const auto refuse = [this, &whom](Told told, base::Error why) {
        QMetaObject::invokeMethod(
            this,
            [whom, told = std::move(told), why = std::move(why)] {
                if (whom && told) {
                    told(why);
                }
            },
            Qt::QueuedConnection);
    };
    if (aFileIsWorkedOn.exchange(true)) {
        refuse(std::move(done), busy());
        return;
    }
    // What came of it, told on the interface thread, a turn of its loop later; and not at all
    // once this is gone. From whichever thread: it carries the answer, and nothing else.
    const auto tell = [this](base::Result<Answer> answer) {
        QMetaObject::invokeMethod(
            this,
            [this, answer = std::move(answer)] {
                const std::shared_ptr<Told> told = std::static_pointer_cast<Told>(std::exchange(m_told, nullptr));
                const QPointer<QObject> asked = std::exchange(m_asker, nullptr);
                m_working = false;
                aFileIsWorkedOn.store(false);
                if (asked && told != nullptr && *told) {
                    (*told)(answer);
                }
            },
            Qt::QueuedConnection);
    };
    // From here to where the thread runs, memory may be wanting, or a thread, or something may
    // be thrown that nobody thought of: then the place is given back, and that is told. (The
    // telling takes a little memory too. Where there is not even that, what the library throws
    // leaves here, with the place given back all the same.)
    std::shared_ptr<Told> whoIsTold;
    try {
        whoIsTold = std::make_shared<Told>();
    } catch (const std::bad_alloc&) {
        aFileIsWorkedOn.store(false);
        refuse(std::move(done), noMemory());
        return;
    }
    *whoIsTold = std::move(done);
    try {
        if (m_beforeTheThread) {
            m_beforeTheThread();
        }
        // (The thread before this one has ended: it said so, or the place would be taken.)
        m_thread = std::jthread([tell, what = std::move(what)](const std::stop_token& stop) mutable {
            // Nothing leaves a thread flying (DESIGN.md section 7).
            try {
                tell(what(stop));
            } catch (const std::bad_alloc&) {
                tell(noMemory());
            } catch (...) {
                tell(failed());
            }
        });
    } catch (const std::bad_alloc&) {
        aFileIsWorkedOn.store(false);
        refuse(std::move(*whoIsTold), noMemory());
        return;
    } catch (const std::system_error&) {
        aFileIsWorkedOn.store(false);
        refuse(std::move(*whoIsTold), noThread());
        return;
    } catch (...) {
        // Whatever else was thrown there, by whatever it was: the place is the program's one,
        // and is given back all the same; and whoever asked is told. (Left taken, no file could
        // be worked on again until the program was started anew.)
        aFileIsWorkedOn.store(false);
        refuse(std::move(*whoIsTold), failed());
        return;
    }
    m_told = std::move(whoIsTold);
    m_asker = whom;
    m_working = true;
}

void FileWork::read(const QByteArray& file, const QString& passphrase, QObject* asker,
                    std::function<void(base::Result<Content>)> done)
{
    work<Content>(
        asker,
        [file, typed = SecretBytes::utf8Of(passphrase), derive = m_sealing.derive](const std::stop_token& stop) {
            return readWith(file, typed, derive, stop);
        },
        std::move(done));
}

void FileWork::write(const Content& content, const QString& passphrase, QObject* asker,
                     std::function<void(base::Result<Written>)> done)
{
    work<Written>(
        asker,
        [content, typed = SecretBytes::utf8Of(passphrase), how = m_sealing](const std::stop_token& stop) {
            return writeWith(content, typed, how, stop);
        },
        std::move(done));
}

} // namespace qkvm::core::exchange
