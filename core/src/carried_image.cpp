#include <qkvm/core/carried_image.h>

#include <utility>

#include <qkvm/core/read_ahead.h>

namespace qkvm::core {

CarriedImage::CarriedImage(std::unique_ptr<QIODevice> file) : m_file(std::move(file)), m_from(m_file->pos())
{
    connect(m_file.get(), &QIODevice::readyRead, this, &QIODevice::readyRead);
    connect(m_file.get(), &QIODevice::readChannelFinished, this, &QIODevice::readChannelFinished);
    QIODevice::open(QIODevice::ReadOnly | QIODevice::Unbuffered);
}

CarriedImage::~CarriedImage() = default;

qint64 CarriedImage::size() const
{
    return m_file->size() - m_from;
}

bool CarriedImage::seek(qint64 pos)
{
    if (pos != 0 || !m_file->seek(m_from) || !QIODevice::seek(0)) {
        return false;
    }
    m_hash.reset();
    m_counted = 0;
    return true;
}

QString CarriedImage::sha256() const
{
    return m_counted == size() ? QString::fromLatin1(m_hash.result().toHex()) : QString();
}

QString CarriedImage::failure() const
{
    const auto* reader = qobject_cast<const ReadAhead*>(m_file.get());
    return reader != nullptr ? reader->failure() : m_failed;
}

qint64 CarriedImage::readData(char* data, qint64 most)
{
    const qint64 got = m_file->read(data, most);
    if (got > 0) {
        m_hash.addData(QByteArrayView(data, got));
        m_counted += got;
    } else if (got < 0) {
        m_failed = m_file->errorString();
        setErrorString(m_failed);
    }
    return got;
}

qint64 CarriedImage::writeData(const char* /*data*/, qint64 /*length*/)
{
    return -1;
}

} // namespace qkvm::core
