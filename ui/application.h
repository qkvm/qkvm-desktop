#pragma once

class QQmlEngine;

namespace qkvm::ui {

// What has to be said once, after the QGuiApplication exists and before the first window does: the
// program's name and version, its mark for its windows, the page's letters, and dark until there
// are settings to say otherwise. The program and the tests of its screens both say it through here, so that a
// screen under test looks as it will. On the interface thread.
void prepareApplication();

// Keeps what an engine draws from asking anything of the network: a picture, a font or a page named
// by an address is not fetched, whoever names it. The program's screens come with the program, and
// what it asks of a QKVM server it asks through core::ServerClient and through nothing else. To be
// called before the engine loads anything.
void keepOffTheNetwork(QQmlEngine& engine);

} // namespace qkvm::ui
