#pragma once

#include <functional>
#include <memory>

#include <QIODevice>
#include <QObject>
#include <QString>

#include <qkvm/base/result.h>

namespace qkvm::core {

// A file for Qt to send (an image carried through a server: ServerClient::uploadMedia), read on a thread of its own
// and never on the thread that sends it. It is opened there, its length found there, and it is read there ahead of
// what is sent, a piece at a time and `ahead` bytes at the most; what is read of this device is taken from those at
// once, and never waits for the disk or the share the file is on. Where that stops answering, nothing more comes:
// whoever reads this is given nothing for as long, and goes on with everything else meanwhile (an upload then waits,
// and is ended by the server, or as cut off, as one whose line stops). A read of the file that fails ends it: this
// says so (failure()), and a read of it fails then as well.
//
// It is read at a place, as a file is: of a length known before a byte goes, and moved to the place a read is to begin
// at (seek), from where the file is then read again (Qt does so where it sends a request again on another line). Lives
// on the thread it was made on, and is used only there; the thread that reads the file is let go of when this ends,
// without waiting for it: where it is in a read that does not come back, it ends when that does, and touches nothing
// of this any more.
class ReadAhead final : public QIODevice {
    Q_OBJECT

public:
    // What is read: made, and opened for reading, on the thread of the reading; or why it cannot be. A file is read at
    // a place, with a length of its own.
    using Source = std::function<base::Result<std::unique_ptr<QIODevice>>()>;
    // The file at this path, opened read only, there.
    static Source fileAt(const QString& path);
    // A piece read at once, and how much is read ahead at the most.
    static constexpr qint64 pieceBytes = qint64{256} << 10;
    static constexpr qint64 aheadBytes = qint64{4} << 20;

    // Begins it: the source is made and opened on the thread of its own. `opened` is told on the thread that asked,
    // once that is done, of this device (open, of the file's length), or why not; never where `context` is gone by
    // then (the thread lets go of the file then).
    static void open(Source source, QObject* context,
                     std::function<void(base::Result<std::unique_ptr<ReadAhead>>)> opened, qint64 ahead = aheadBytes);
    // A device that is open already, read on the thread of its own from where it stands: moved to that thread, which
    // reads it from then on. Its length is asked here, once. Nothing where it is not open for reading, is not read at
    // a place, or has a parent.
    static std::unique_ptr<ReadAhead> of(std::unique_ptr<QIODevice> device, qint64 ahead = aheadBytes);

    ~ReadAhead() override;

    bool isSequential() const override { return false; }
    // The file's length, as it was when it was opened.
    qint64 size() const override { return m_size; }
    bool seek(qint64 pos) override;
    // Why a read of the file failed, as its device said it; empty while none did.
    QString failure() const;
    // Whether a read of this was given nothing, and the file has given nothing more since: it is being waited for.
    bool waitingForTheFile() const;

protected:
    qint64 readData(char* data, qint64 most) override;
    qint64 writeData(const char* data, qint64 length) override;

private:
    struct Shared;
    ReadAhead(std::shared_ptr<Shared> shared, qint64 size);
    // On the thread that reads the file: tells the device, while it stands, that something came after a read of it
    // was given nothing; and the reading itself, from `next` on.
    static void tellTheReader(const std::shared_ptr<Shared>& shared);
    static void readOn(const std::shared_ptr<Shared>& shared, std::unique_ptr<QIODevice> device, qint64 next);

    std::shared_ptr<Shared> m_shared;
    qint64 m_size = 0;
};

} // namespace qkvm::core
