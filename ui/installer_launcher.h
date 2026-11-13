#pragma once

#include <QString>

namespace qkvm::ui {
bool installerConfigured();
// Copies the dependency-free helper out of the installed package before launching it.
// An explicit user action is the only caller; this never closes the application.
bool startInstaller(const QString& action, const QString& package = {}, const QString& version = {});
} // namespace qkvm::ui
