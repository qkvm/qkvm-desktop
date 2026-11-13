#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <optional>

namespace qkvm::core {

// RFC 6184 packetization mode 1, deliberately without an old-picture reorder queue.
// Loss/reordering discards dependent pictures until an IDR with parameter sets arrives.
// One assembly <= 4 MiB, cached parameter sets <= 64 KiB each. No allocation trusts a length.
class H264Rtp {
public:
    static constexpr qsizetype largestPicture = qsizetype{4} * 1024 * 1024;
    struct Picture {
        QByteArray bytes;
        bool key = false;
    };
    explicit H264Rtp(int payloadType = 96) : m_payloadType(payloadType) {}
    std::optional<Picture> take(QByteArrayView packet);
    void lose();
    bool needsKey() const { return m_needKey; }
    bool parameters(QByteArrayView sps, QByteArrayView pps);

private:
    bool nal(QByteArrayView bytes);
    void clearPicture();
    int m_payloadType;
    std::optional<quint16> m_next;
    std::optional<quint32> m_ssrc;
    std::optional<quint32> m_time;
    QByteArray m_picture;
    QByteArray m_fragment;
    QByteArray m_sps;
    QByteArray m_pps;
    bool m_needKey = true;
    bool m_key = false;
    bool m_slice = false;
    bool m_broken = false;
};

} // namespace qkvm::core
