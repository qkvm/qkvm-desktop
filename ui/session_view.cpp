#include "session_view.h"

namespace qkvm::ui {

SessionView::SessionView(QObject* parent) : QObject(parent) {}

void SessionView::showSources(const QVariantList& sources, const QString& selected, bool allowed)
{
    if (m_sources == sources && m_source == selected && m_canChooseSource == allowed)
        return;
    m_sources = sources;
    m_source = selected;
    m_canChooseSource = allowed;
    emit sourcesChanged();
}

void SessionView::showSourceTree(const QVariantList& rows)
{
    if (m_sourceTree != rows) {
        m_sourceTree = rows;
        emit sourceTreeChanged();
    }
}

void SessionView::show(const Shown& shown)
{
    if (shown != m_shown) {
        m_shown = shown;
        emit changed();
    }
}

void SessionView::connectTo(const QString& address)
{
    emit connectAsked(address);
}

void SessionView::signIn(const QString& name, const QString& password, const QString& code, bool keepSignedIn)
{
    emit signInAsked(name, password, code, keepSignedIn);
}

void SessionView::trustCertificate()
{
    emit trustAsked();
}

void SessionView::signOut()
{
    emit signOutAsked();
}

void SessionView::openPage()
{
    emit pageAsked();
}

void SessionView::anotherServer()
{
    emit anotherServerAsked();
}

void SessionView::goToOwnWall()
{
    emit ownWallAsked();
}

void SessionView::stopTrying()
{
    emit stopTryingAsked();
}

} // namespace qkvm::ui
