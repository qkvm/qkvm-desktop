#include "actions_view.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QMimeData>
#include <qkvm/core/own_actions.h>

#include "machine_menu.h"

namespace qkvm::ui {

ActionsView::ActionsView(QObject* parent, int noticesStayPercent)
    : QObject(parent), m_notices(nullptr, noticesStayPercent)
{
    m_clipboard = [] {
        const QClipboard* clipboard = QGuiApplication::clipboard();
        const QMimeData* data = clipboard != nullptr ? clipboard->mimeData() : nullptr;
        return data != nullptr && data->hasText() ? data->text() : QString();
    };
    connect(&m_media, &MediaView::told, this, &ActionsView::tell);
}

void ActionsView::open(const QString& machine)
{
    emit openAsked(machine);
}

void ActionsView::close()
{
    emit closeAsked();
}

void ActionsView::choose(const QString& row)
{
    emit chosen(row);
}

void ActionsView::answer(bool yes)
{
    emit answered(yes, -1, false);
}

void ActionsView::answerWith(bool yes, int paceSeconds, bool ticked)
{
    emit answered(yes, paceSeconds, ticked);
}

void ActionsView::openPicked()
{
    emit pickedAsked();
}

void ActionsView::stopRun()
{
    emit runStopAsked();
}

void ActionsView::retryRun()
{
    emit runRetryAsked();
}

void ActionsView::closeRun()
{
    emit runCloseAsked();
}

void ActionsView::showRun(const QVariantMap& run)
{
    if (run != m_run) {
        m_run = run;
        emit runChanged();
    }
}

void ActionsView::showPort(const QString& machine, int port)
{
    emit portAsked(machine, port);
}

void ActionsView::openConsole(const QString& machine)
{
    emit consoleAsked(machine);
}

void ActionsView::cardAction(const QString& machine)
{
    emit cardActionAsked(machine);
}

void ActionsView::sendKeys(const QString& keys)
{
    emit keysAsked(keys);
}

void ActionsView::typeText(const QString& text, bool enter)
{
    emit textAsked(text, enter);
}

void ActionsView::loadClipboard()
{
    if (!m_allowed || m_busy || m_form.value(QStringLiteral("kind")).toString() != QStringLiteral("text") ||
        !m_clipboard) {
        return;
    }
    const QVariantMap before = m_form;
    const QString text = m_clipboard();
    if (!m_allowed || m_busy || m_form != before) {
        return;
    }
    QString problem;
    if (text.isEmpty()) {
        problem = tr("The clipboard has no text to type.");
    } else if (!text.isValidUtf16() || !core::action::isTypedAsOne(text)) {
        problem = tr("The clipboard text cannot be typed. Use at most 2,000 characters and complete Unicode "
                     "characters. Nothing was sent.");
    }
    QVariantMap form = m_form;
    form.insert(QStringLiteral("problem"), problem);
    showForm(form);
    if (problem.isEmpty() && m_allowed && !m_busy && m_form == form) {
        emit clipboardLoaded(text);
    }
}

void ActionsView::closeForm()
{
    emit formCloseAsked();
}

void ActionsView::tell(const QString& said, bool good)
{
    m_notices.tell(good ? Notices::Kind::Done : Notices::Kind::Failed, said);
}

void ActionsView::tellWhenSeen(const QString& said, bool good)
{
    m_notices.tellWhenSeen(good ? Notices::Kind::Done : Notices::Kind::Failed, said);
}

QVariantList ActionsView::heldKeys()
{
    return ui::heldKeys();
}

QVariantList ActionsView::otherKeys()
{
    return ui::otherKeys();
}

QString ActionsView::read(const QString& keys)
{
    return keysLabel(keys);
}

void ActionsView::showForm(const QVariantMap& form)
{
    if (form != m_form) {
        m_form = form;
        emit formChanged();
    }
}

void ActionsView::setAllowed(bool allowed)
{
    if (allowed != m_allowed) {
        m_allowed = allowed;
        emit allowedChanged();
    }
}

void ActionsView::showMenu(const QVariantMap& menu)
{
    if (menu != m_menu) {
        m_menu = menu;
        emit menuChanged();
    }
}

void ActionsView::setBusy(bool busy)
{
    if (busy != m_busy) {
        m_busy = busy;
        emit busyChanged();
    }
}

void ActionsView::showQuestion(const QVariantMap& question)
{
    if (question != m_question) {
        m_question = question;
        emit questionChanged();
    }
}

} // namespace qkvm::ui
