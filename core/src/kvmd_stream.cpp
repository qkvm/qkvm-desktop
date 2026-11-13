#include <qkvm/core/kvmd_stream.h>

#include <string>
#include <utility>

#include <QNetworkReply>
#include <QNetworkRequest>

#include <qkvm/core/kvmd_device.h>

using namespace Qt::StringLiterals;

namespace qkvm::core {

namespace {

// How much of what waits to be read is taken up at a time.
constexpr qint64 readAtOnce = qint64{64} * 1024;
// The most pictures a second a KVM has reason to send, many times over. Each of them is handed
// to somebody on this thread, and a megabyte holds thousands that are a byte long.
constexpr int mostPicturesASecond = 1000;
// The most that is read of what comes with a refusal, to see whose words it is: kvmd says no in
// a line or two.
constexpr qint64 mostOfARefusal = qint64{64} * 1024;

// The driver's words for an end that only a stream has.
base::Error openAlready()
{
    return {"busy", "A stream of this device's pictures is open already. One is open at a time."};
}
base::Error notAllowed()
{
    return {"not_allowed", "The device signed this program in and refuses its stream of pictures all the same"};
}
base::Error noStreamHere()
{
    return {"no_stream_here", "This device does not send its picture as a stream of JPEGs"};
}

// Whether an answer calls itself a stream of pictures, whatever else it says of itself: by the
// kind it names before the first ";", as kvmd's streamer names its own.
bool callsItselfAStream(const QByteArray& contentType)
{
    const qsizetype more = contentType.indexOf(';');
    const QByteArray kind = (more < 0 ? contentType : contentType.first(more)).trimmed();
    return kind.compare("multipart/x-mixed-replace", Qt::CaseInsensitive) == 0;
}

// Whether an answer is a "no" of which what comes with it says whose it is: kvmd's own, or that
// of what stands before kvmd.
constexpr bool saysNo(int status)
{
    return status == 502 || status == 503 || status == 504;
}

// Why a stream is not read on, in words of the program's own: nothing a KVM sent is among them.
const char* inWords(MultipartPictures::End why)
{
    switch (why) {
    case MultipartPictures::End::NoBoundary:
        return "it does not begin with a picture";
    case MultipartPictures::End::WrongBoundary:
        return "its pictures are not set apart as it said they would be";
    case MultipartPictures::End::BadLineEnd:
        return "its lines do not end as lines do";
    case MultipartPictures::End::HeadersTooLong:
        return "it says far more of a picture than a KVM has reason to";
    case MultipartPictures::End::NoLength:
        return "it does not say how long a picture is";
    case MultipartPictures::End::BadLength:
        return "what it says a picture's length is, is none";
    case MultipartPictures::End::None:
    case MultipartPictures::End::Closed:
    case MultipartPictures::End::TooLarge:
        break; // said otherwise, where the stream ends
    }
    return "it is not read";
}

} // namespace

KvmdStream::KvmdStream(Made /*made*/, KvmdDevice* device) : m_device(device)
{
    m_quiet.setSingleShot(true);
    m_quiet.setInterval(m_device->m_pace.streamQuietMs);
    connect(&m_quiet, &QTimer::timeout, this, [this] { quiet(); });
    m_pictureDue.setSingleShot(true);
    m_pictureDue.setInterval(m_device->m_pace.streamPictureMs);
    connect(&m_pictureDue, &QTimer::timeout, this, [this] { overdue(); });
    m_clock.start();
    if (!m_device->m_streams.isEmpty()) {
        // One stream to a KVM at a time: this one is none of its device's, asks nothing, and
        // says so a turn later, as everything is said.
        m_device = nullptr;
        QTimer::singleShot(0, this, [this] { end(openAlready()); });
        return;
    }
    m_device->m_streams.append(this);
    // A turn later, as the driver hands everything over: whoever asked holds the stream before
    // it says anything.
    QTimer::singleShot(0, this, [this] { open(); });
}

KvmdStream::~KvmdStream()
{
    close();
}

void KvmdStream::close()
{
    m_closed = true;
    m_over = true;
    letGo();
}

qint64 KvmdStream::held() const
{
    const qint64 unread = m_reply.isNull() ? 0 : m_reply->bytesAvailable();
    return unread + (m_pictures ? m_pictures->room() : 0);
}

void KvmdStream::open()
{
    if (m_over || m_device == nullptr) {
        return;
    }
    // A KVM that is away is left alone for a while, by this as by every look.
    if (const std::optional<base::Error> held = m_device->heldBack()) {
        end(*held);
        return;
    }
    m_device->withToken([stream = QPointer<KvmdStream>(this)](const base::Result<QByteArray>& token) {
        if (stream && !stream->m_over && stream->m_device != nullptr) {
            stream->ask(token);
        }
    });
}

void KvmdStream::ask(const base::Result<QByteArray>& token)
{
    if (!token) {
        end(token.error());
        return;
    }
    m_used = token.value();
    // Under this very session the KVM refused its stream a moment ago, though it had only just
    // given the session (answered()): it is not asked again so soon, and no password goes out.
    if (m_used == m_device->m_streamRefusedUnder && m_device->now() < m_device->m_streamRefusedUntil) {
        end(notAllowed());
        return;
    }
    // kvmd rests its video while nobody is on its line for that. Somebody is from here on: the
    // device holds that line for as long as a stream is open.
    m_device->keepAwake();
    KvmdDevice::Ask question;
    question.path = u"/streamer/stream"_s;
    question.endless = true;
    const base::Result<QNetworkReply*> asked = m_device->sendOut(question, m_used);
    if (!asked) {
        end(asked.error());
        return;
    }
    QNetworkReply* reply = asked.value();
    m_reply = reply;
    reply->setReadBufferSize(mostUnreadBytes);
    // (What a question that was given up still says is not heard: it is no longer the stream's.)
    connect(reply, &QNetworkReply::metaDataChanged, this, [this, reply] {
        if (m_reply == reply) {
            answered();
        }
    });
    connect(reply, &QNetworkReply::readyRead, this, [this, reply] {
        if (m_reply == reply) {
            read();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        if (m_reply == reply) {
            finished();
        }
    });
    m_quiet.start();
}

void KvmdStream::answered()
{
    if (m_over || m_open || m_refusedWith != 0 || m_reply.isNull() || m_device == nullptr) {
        return;
    }
    const int status = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 0) {
        return; // it has not said yet
    }
    m_device->answered(); // whatever it says, it is there
    if (status == 401 || status == 403) {
        dropQuestion();
        // Refused under a session the KVM has only just given: this stream signed in anew for
        // it, or was refused under this session before. Then it is not the session that the KVM
        // refuses, but its stream: whoever is signed in may watch none, or what stands before
        // the KVM guards that place by itself. That ends the stream, and leaves the session to
        // every other question, which is served under it. (Given up for lost, as for a question
        // that is refused twice, it cost every look, picture and button its answer until the
        // next sign-in was due.) Nor is it asked for under that session again for a while.
        if (m_relogged || m_used == m_device->m_streamRefusedUnder) {
            m_device->m_streamRefusedUnder = m_used;
            m_device->m_streamRefusedUntil = m_device->now() + m_device->m_pace.signInRetryMs;
            end(notAllowed());
            return;
        }
        // The session ran out, or kvmd was started anew: once more, signed in afresh. (Where
        // that sign-in fails, it is what ends the stream, in its own words.)
        KvmdDevice::Tries tries;
        static_cast<void>(m_device->sessionRefused(m_used, tries));
        m_relogged = true;
        open();
        return;
    }
    if (status == 404) {
        end(noStreamHere());
        return;
    }
    if (saysNo(status)) {
        // Whose word that is, what comes with it says: read when all of it has come, or as
        // much of it as comes in the time it has (finished(), quiet()). That time is counted
        // anew from here, the beginning of the answer, as for a stream: once, and not from
        // each byte that comes, so that words which come a letter at a time are not waited for
        // without end.
        m_refusedWith = status;
        m_quiet.start();
        read();
        return;
    }
    if (status != 200) {
        end({"bad_response", u"Stream failed (HTTP %1)"_s.arg(status).toStdString()});
        return;
    }
    const QByteArray kind = m_reply->rawHeader("Content-Type"_ba);
    const std::optional<QByteArray> boundary = boundaryFrom(kind);
    if (!boundary) {
        // A yes, and no stream this program reads. Where it calls itself a stream of pictures
        // and breaks the rules of one, that says nothing of whether the KVM has a stream. Where
        // it is of another kind, the KVM answers at this place with another thing: it has no
        // stream of JPEGs (one maker's KVM does so while its video is awake: seen on a real
        // one), and whoever asked takes its pictures one at a time instead of asking again.
        end(callsItselfAStream(kind)
                ? base::Error{"bad_response", "The device sent something that is not a stream of pictures"}
                : noStreamHere());
        return;
    }
    m_pictures.emplace(*boundary);
    m_open = true;
    // Silence is counted anew from the beginning of the answer.
    m_quiet.start();
    read();
}

base::Error KvmdStream::refusedIn(int status, const QByteArray& words)
{
    if (!kvmd::isKvmd(words)) {
        return saidByAGateway();
    }
    // In kvmd's own shape it is kvmd that speaks, and 503 is its word for a video that rests,
    // as where a snapshot is asked of it: that is woken.
    if (status == 503 && m_device != nullptr) {
        return m_device->resting();
    }
    return {"bad_response", u"Stream failed (HTTP %1)"_s.arg(status).toStdString()};
}

base::Error KvmdStream::saidByAGateway()
{
    // What stands before kvmd says 502, 503 or 504 of what is behind it, on a page of its own:
    // nothing there serves this place just now. On a stock KVM that is so while its video
    // rests, which is whenever nobody watches. A KVM that has no such stream at all may say the
    // same while its video is awake (one maker's, whose live picture comes another way, was
    // seen to say it while its video rests: a real one). The page does not tell the two apart;
    // whether the video is awake does. Where a picture of the KVM has just come, it is awake,
    // and still there is no stream: this KVM has none. Otherwise its video rests for all that
    // is known, and is woken, and whoever asked asks again.
    if (m_device != nullptr && !m_device->sawItsVideoAwake()) {
        return m_device->resting();
    }
    return noStreamHere();
}

void KvmdStream::read()
{
    if (m_over || m_reading) {
        return;
    }
    if (m_refusedWith != 0) {
        // (More than kvmd ever says in a refusal: not its words, whatever is still to come.)
        if (!m_reply.isNull() && m_reply->bytesAvailable() > mostOfARefusal) {
            end(saidByAGateway());
        }
        return;
    }
    // (Whoever is handed a picture may let the loop turn before it comes back: what arrives
    // meanwhile is read when it has, in its order.)
    if (!m_open) {
        return;
    }
    const QPointer<KvmdStream> stream(this);
    m_reading = true;
    take();
    if (stream) {
        m_reading = false;
    }
}

void KvmdStream::take()
{
    // Whoever is handed a picture may close the stream there, or let go of it: nothing of it is
    // touched after that.
    const QPointer<KvmdStream> stream(this);
    while (!m_reply.isNull() && m_reply->bytesAvailable() > 0) {
        const QByteArray came = m_reply->read(readAtOnce);
        if (came.isEmpty()) {
            return;
        }
        // Something came: that is no silence, whatever it is.
        m_quiet.start();
        QByteArrayView rest(came);
        while (!rest.isEmpty()) {
            if (!m_pictures) {
                return;
            }
            MultipartPictures& pictures = *m_pictures;
            rest = rest.sliced(pictures.feed(rest));
            if (const std::optional<QByteArray> picture = pictures.next()) {
                // Whole: the time it had to become that is over.
                m_pictureBegun = false;
                m_pictureDue.stop();
                if (const qint64 second = m_clock.elapsed() / 1000; second != m_second) {
                    m_second = second;
                    m_cameInIt = 0;
                }
                if (++m_cameInIt > mostPicturesASecond) {
                    end({"bad_response", "The device sends more pictures than a KVM has reason to"});
                    return;
                }
                emit pictureCame(*picture);
                if (!stream || m_over) {
                    return;
                }
                // Somebody has taken it, however long that took: the KVM's silence is counted
                // from here.
                m_quiet.start();
                continue;
            }
            const MultipartPictures::End why = pictures.end();
            if (why == MultipartPictures::End::None) {
                // All of it was taken, and no picture is whole: what came is the beginning of
                // one, or more of one. It has its own time from the first of that. Without
                // it, a KVM that says a picture of megabytes follows and sends a byte of it now
                // and then would hold a line for ever, and never be silent.
                if (!m_pictureBegun) {
                    m_pictureBegun = true;
                    m_pictureDue.start();
                }
                continue;
            }
            if (why == MultipartPictures::End::TooLarge) {
                // Marked and given up, as the driver gives up an answer that is too large: what
                // that is called is said where every question's end is.
                m_reply->setProperty("tooMuch", true);
                m_reply->abort();
            } else if (why == MultipartPictures::End::Closed) {
                end({"dropped", "The device ended the stream"});
            } else {
                end({"bad_response",
                     std::string("The device's stream is not one this program reads: ") + inWords(why)});
            }
            return;
        }
    }
}

void KvmdStream::finished()
{
    if (m_over || m_reply.isNull()) {
        return;
    }
    const QPointer<KvmdStream> stream(this);
    const std::optional<base::Error> why = KvmdDevice::whyNoAnswer(m_reply);
    // (Also where the question was given up for how its answer came, packed, or the line fell
    // behind it: a KVM that said no has answered.)
    const bool saidNo = saysNo(m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt());
    if ((!why || saidNo) && !m_open && m_refusedWith == 0) {
        answered(); // what it answered came with the end of the answer
        if (!stream || m_over) {
            return;
        }
    }
    if (m_open) {
        // Its pictures had begun: those that came whole before the end are handed on. Then the
        // stream is gone, and whether the KVM is there the next look says.
        read();
        if (stream && !m_over) {
            end(why.value_or(base::Error{"dropped", "The device ended the stream"}));
        }
    } else if (m_refusedWith != 0) {
        // It said no, and what it said with that has come: all of it, or as much as ever will,
        // where the line fell under it. A refusal that was begun is a refusal: read as far as
        // it came, and the KVM does not count as away for it. (Of a question that was given up
        // nothing can be read: what came packed is nobody's words here.)
        end(refusedIn(m_refusedWith, m_reply->isOpen() ? m_reply->read(mostOfARefusal) : QByteArray()));
    } else if (why && m_device != nullptr) {
        // It did not answer at all, and counts as away for that, as with every question.
        end(m_device->failed(*why, false));
    }
}

void KvmdStream::quiet()
{
    if (m_over || m_reply.isNull()) {
        return;
    }
    if (m_reading) {
        // A picture is in somebody's hands at this moment: that is not the KVM's silence.
        m_quiet.start();
        return;
    }
    if (m_refusedWith != 0) {
        // It said no, and what comes with that is not whole in the time it has. It is not
        // waited for longer, and read as far as it came: a KVM that said no has answered, and
        // does not count as away. (Taken before the question is given up: of one that was,
        // nothing can be read.)
        end(refusedIn(m_refusedWith, m_reply->read(mostOfARefusal)));
        return;
    }
    if (m_open && m_reply->bytesAvailable() > 0) {
        // This PC was busy elsewhere, and what came waits to be read: that is not the KVM's
        // silence either, and reading it counts the silence anew.
        const QPointer<KvmdStream> stream(this);
        read();
        if (!stream || m_over || m_quiet.isActive()) {
            return;
        }
    }
    // Marked and given up, as the driver gives up every question that nothing comes of.
    m_reply->setProperty("gaveUp", "timeout"_ba);
    m_reply->abort();
}

void KvmdStream::overdue()
{
    if (m_over || m_reply.isNull()) {
        return;
    }
    if (m_open && m_reply->bytesAvailable() > 0) {
        // This PC was busy elsewhere, and what came waits to be read: the picture may be whole
        // in it.
        const QPointer<KvmdStream> stream(this);
        read();
        if (!stream || m_over || !m_pictureBegun || m_pictureDue.isActive()) {
            return;
        }
    }
    // Its bytes came, and it is not whole in the time a picture has: given up as what nothing
    // comes of, by the same word.
    m_reply->setProperty("gaveUp", "timeout"_ba);
    m_reply->abort();
}

void KvmdStream::end(const base::Error& why)
{
    if (m_over) {
        return;
    }
    m_over = true;
    letGo();
    emit ended(QString::fromStdString(why.code), QString::fromStdString(why.message));
}

void KvmdStream::letGo()
{
    m_quiet.stop();
    m_pictureDue.stop();
    m_pictureBegun = false;
    dropQuestion();
    m_pictures.reset();
    if (KvmdDevice* device = std::exchange(m_device, nullptr)) {
        device->m_streams.removeOne(this);
        // Its picture was wanted up to this moment, however long ago the stream was asked for:
        // from here the device counts how long nobody has wanted it, before the video may rest.
        device->m_wantedAt = device->now();
    }
}

void KvmdStream::dropQuestion()
{
    m_open = false;
    m_refusedWith = 0;
    if (QNetworkReply* reply = m_reply.data()) {
        m_reply.clear();
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
    }
}

void KvmdStream::deviceGone()
{
    m_device = nullptr;
    if (m_over) {
        return;
    }
    m_over = true;
    letGo();
    // Said a turn later: by then the device is gone whole, and whoever hears this finds nothing
    // of it half ended.
    QTimer::singleShot(0, this, [this] {
        if (!m_closed) {
            emit ended(u"closed"_s, u"The connection to the device was closed"_s);
        }
    });
}

} // namespace qkvm::core
