#pragma once

namespace qkvm::ui {

// Checks the deployed runtime and bundled screens before settings, credentials, single-instance
// activation or network sources are touched. Used by the packaging gate, never a user session.
int checkPackage();

} // namespace qkvm::ui
