#pragma once
#include <QDateTime>
#include <QString>
namespace qkvm::core {
// Only old one-file transfer/helper folders (update-*: qkvm.msix, qkvm.zip or qkvm-desktop-setup.exe; qkvm-setup-*:
// qkvm-setup.exe), and, with the prefix "setup-temp-" and no file name, Setup's own old TEMP folders whole, where all
// of what is in them is a week old; never follows a link or a junction, and leaves a folder that holds one.
int pruneUpdateCache(const QString& root, const QString& prefix, const QString& fileName, const QDateTime& now);
} // namespace qkvm::core
