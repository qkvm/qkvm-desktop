#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <memory>
#include <qkvm/core/device_line.h>
#include <qkvm/core/kvmd_device.h>
#include <qkvm/core/webrtc_receiver.h>

namespace qkvm::core {
// Janus uStreamer protocol used by PiKVM and GL.iNet's kvmd family. View only: the
// existing console line retains all keyboard/mouse ownership. One attempt; retry belongs to KvmdLive.
class KvmdWebRtc : public QObject {
    Q_OBJECT
public:
    struct Pace {
        int firstPictureMs = 15'000;
        int pictureMs = 10'000;
        int keepaliveMs = 20'000;
        int silentMs = 35'000;
    };
    explicit KvmdWebRtc(KvmdDevice& device, QObject* parent = nullptr);
    KvmdWebRtc(KvmdDevice& device, Pace pace, QObject* parent = nullptr);
    ~KvmdWebRtc() override;
    void start();
    void close();
    void requestKey();
    // What its receiver let go for want of room (WebRtcReceiver::picturesLetGo).
    quint64 picturesLetGo() const { return m_receiver ? m_receiver->picturesLetGo() : 0; }
signals:
    // `arrivedUs` as the receiver stamped it (WebRtcReceiver::pictureCame).
    void pictureCame(const QByteArray& annexB, bool key, qint64 arrivedUs);
    void ended(const QString& code, const QString& reason);

private:
    enum class Phase { New, Opening, Creating, Attaching, Watching, Playing, Over };
    void read(const QByteArray& text);
    void send(QJsonObject message, bool handle = false);
    void fail(const QString& code, const QString& reason);
    // What its receiver refused of the KVM's addresses, written to the video's diagnostics.
    void sayWhatWasRefused() const;
    QPointer<KvmdDevice> m_device;
    Pace m_pace;
    std::unique_ptr<DeviceLine> m_line;
    std::unique_ptr<WebRtcReceiver> m_receiver;
    QTimer m_picture;
    QTimer m_keepalive;
    QTimer m_silent;
    Phase m_phase = Phase::New;
    qint64 m_session = 0;
    qint64 m_handle = 0;
    QString m_transaction;
    QString m_create;
    QString m_attach;
    quint64 m_serial = 0;
};
} // namespace qkvm::core
