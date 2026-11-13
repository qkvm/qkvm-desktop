#include <qkvm/core/h264_rtp.h>

#include <QtEndian>
#include <utility>

namespace qkvm::core {
namespace {
constexpr auto start = "\0\0\0\1";
constexpr qsizetype parameterLimit = qsizetype{64} * 1024;
quint8 byte(QByteArrayView data, qsizetype index)
{
    return static_cast<quint8>(data[index]);
}
} // namespace

void H264Rtp::clearPicture()
{
    m_picture.clear();
    m_fragment.clear();
    m_key = false;
    m_slice = false;
    m_broken = false;
}

void H264Rtp::lose()
{
    clearPicture();
    m_needKey = true;
    m_broken = true;
}

bool H264Rtp::parameters(QByteArrayView sps, QByteArrayView pps)
{
    if (sps.isEmpty() || pps.isEmpty() || sps.size() > parameterLimit || pps.size() > parameterLimit ||
        (byte(sps, 0) & 0x9F) != 7 || (byte(pps, 0) & 0x9F) != 8) {
        return false;
    }
    m_sps = sps.toByteArray();
    m_pps = pps.toByteArray();
    return true;
}

bool H264Rtp::nal(QByteArrayView bytes)
{
    if (bytes.isEmpty() || (byte(bytes, 0) & 0x80) != 0 || m_picture.size() + bytes.size() + 4 > largestPicture) {
        return false;
    }
    const int type = byte(bytes, 0) & 0x1F;
    if (type < 1 || type > 23) {
        return false;
    }
    if (type == 7 || type == 8) {
        if (bytes.size() > parameterLimit) {
            return false;
        }
        QByteArray& kept = type == 7 ? m_sps : m_pps;
        const QByteArray value = bytes.toByteArray();
        if (kept != value) {
            m_needKey = true;
        }
        kept = value;
    }
    m_key = m_key || type == 5;
    m_slice = m_slice || type == 1 || type == 5;
    m_picture.append(start, 4);
    m_picture.append(bytes.data(), bytes.size());
    return true;
}

std::optional<H264Rtp::Picture> H264Rtp::take(QByteArrayView packet)
{
    if (packet.size() < 12 || (byte(packet, 0) >> 6) != 2) {
        lose();
        return {};
    }
    const quint8 flags = byte(packet, 0);
    if ((byte(packet, 1) & 0x7F) != m_payloadType) {
        return {};
    }
    qsizetype end = packet.size();
    qsizetype at = 12 + (flags & 15) * 4;
    if ((flags & 0x20) != 0) {
        const int padding = byte(packet, end - 1);
        if (padding == 0 || padding > end - at) {
            lose();
            return {};
        }
        end -= padding;
    }
    if (at > end) {
        lose();
        return {};
    }
    if ((flags & 0x10) != 0) {
        if (at + 4 > end) {
            lose();
            return {};
        }
        at += 4 + qFromBigEndian<quint16>(packet.data() + at + 2) * 4;
    }
    if (at >= end) {
        lose();
        return {};
    }
    const quint16 sequence = qFromBigEndian<quint16>(packet.data() + 2);
    const quint32 time = qFromBigEndian<quint32>(packet.data() + 4);
    const quint32 ssrc = qFromBigEndian<quint32>(packet.data() + 8);
    if (m_ssrc && *m_ssrc != ssrc) {
        lose();
        m_next.reset();
        m_time.reset();
        m_sps.clear();
        m_pps.clear();
    }
    m_ssrc = ssrc;
    if (m_next && sequence != *m_next) {
        const auto distance = static_cast<quint16>(sequence - *m_next);
        // Duplicates and late packets never rewind the sequence or contaminate a new frame.
        if (distance >= 0x8000) {
            return {};
        }
        lose();
    }
    m_next = static_cast<quint16>(sequence + 1);
    if (!m_time || time != *m_time) {
        if (!m_picture.isEmpty() || !m_fragment.isEmpty()) {
            m_needKey = true;
        }
        clearPicture();
        m_time = time;
    }
    if (m_broken) {
        return {};
    }
    const QByteArrayView payload = packet.sliced(at, end - at);
    const int type = byte(payload, 0) & 31;
    bool valid = true;
    if (type >= 1 && type <= 23) {
        valid = m_fragment.isEmpty() && nal(payload);
    } else if (type == 24 && m_fragment.isEmpty()) {
        qsizetype offset = 1;
        while (valid && offset < payload.size()) {
            if (offset + 2 > payload.size()) {
                valid = false;
                break;
            }
            const quint16 length = qFromBigEndian<quint16>(payload.data() + offset);
            offset += 2;
            if (length == 0 || offset + length > payload.size()) {
                valid = false;
                break;
            }
            valid = nal(payload.sliced(offset, length));
            offset += length;
        }
        valid = valid && offset > 1;
    } else if (type == 28 && payload.size() > 2) {
        const quint8 header = byte(payload, 1);
        const bool begins = (header & 0x80) != 0;
        const bool ends = (header & 0x40) != 0;
        const auto original = static_cast<char>((byte(payload, 0) & 0xE0) | (header & 31));
        valid = (header & 0x20) == 0 && (header & 31) > 0 && (header & 31) < 24 && !(begins && ends);
        if (begins) {
            valid = valid && m_fragment.isEmpty();
            m_fragment.append(original);
        } else {
            valid = valid && !m_fragment.isEmpty() && m_fragment.front() == original;
        }
        valid = valid && m_fragment.size() + payload.size() - 2 + m_picture.size() + 4 <= largestPicture;
        if (valid) {
            m_fragment.append(payload.data() + 2, payload.size() - 2);
            if (ends) {
                valid = nal(m_fragment);
                m_fragment.clear();
            }
        }
    } else {
        valid = false;
    }
    if (!valid) {
        lose();
        return {};
    }
    if ((byte(packet, 1) & 0x80) == 0) {
        return {};
    }
    if (!m_fragment.isEmpty()) {
        lose();
        return {};
    }
    std::optional<Picture> result;
    if (m_slice && (!m_needKey || m_key) && !m_sps.isEmpty() && !m_pps.isEmpty()) {
        QByteArray bytes;
        if (m_key) {
            bytes.append(start, 4);
            bytes.append(m_sps);
            bytes.append(start, 4);
            bytes.append(m_pps);
        }
        if (bytes.size() + m_picture.size() <= largestPicture) {
            bytes.append(m_picture);
            result = Picture{std::move(bytes), m_key};
            m_needKey = false;
        } else {
            m_needKey = true;
        }
    }
    clearPicture();
    return result;
}
} // namespace qkvm::core
