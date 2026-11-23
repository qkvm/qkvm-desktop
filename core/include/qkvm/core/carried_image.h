#pragma once

#include <memory>

#include <QCryptographicHash>
#include <QIODevice>
#include <QString>

namespace qkvm::core {

// An image on its way from this PC to a KVM of its own wall (a kvmd's: kvmd_media.cpp; a JetKVM's: jetkvm_media.cpp):
// read from its file as Qt sends it, counted and hashed as it passes (what is hashed is what went), and kept nowhere
// (the file reads a few megabytes ahead at the most: ReadAhead). Read at a place, as a file is; moved back to its
// beginning, it is counted and hashed anew from there (Qt does so where it sends a request again), and to any other
// place not at all. On the thread it was made on.
class CarriedImage final : public QIODevice {
public:
    explicit CarriedImage(std::unique_ptr<QIODevice> file);
    CarriedImage(const CarriedImage&) = delete;
    CarriedImage& operator=(const CarriedImage&) = delete;
    CarriedImage(CarriedImage&&) = delete;
    CarriedImage& operator=(CarriedImage&&) = delete;
    ~CarriedImage() override;

    bool isSequential() const override { return false; }
    qint64 size() const override;
    bool seek(qint64 pos) override;
    // Its SHA-256 once every byte of it has passed; empty before.
    QString sha256() const;
    // Why a read of its file failed, as the file said it (ReadAhead::failure); empty while none did.
    QString failure() const;

protected:
    qint64 readData(char* data, qint64 most) override;
    qint64 writeData(const char* data, qint64 length) override;

private:
    std::unique_ptr<QIODevice> m_file;
    qint64 m_from = 0;
    QCryptographicHash m_hash{QCryptographicHash::Sha256};
    qint64 m_counted = 0;
    QString m_failed;
};

} // namespace qkvm::core
