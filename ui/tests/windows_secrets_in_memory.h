#pragma once

// The build's own core/src/windows_secrets.cpp, compiled once more with every call of Windows' Credential Manager going
// to the one in memory (credential_capture.h): capturedWindowsSecrets() is that build's windowsSecrets(), with that
// build's own default of what its secrets are kept under, writing and reading records in memory exactly as it would in
// Windows' store (review 012, M3). Included first, before anything that includes secrets.h, in exactly one source of a
// test program; nothing of it reaches Windows' own store: any other call of Credential Manager stops the build here.

#include <windows.h>

#include <wincred.h>

#include "credential_capture.h"

// (Before secrets.h is first read: its declarations, with that build's defaults, are the captured ones.)
#define windowsSecrets capturedWindowsSecrets
#define windowsTargetOf capturedWindowsTargetOf
#include <qkvm/core/secrets.h>

#define CredWriteW qkvm::testing::credentials::write
#define CredReadW qkvm::testing::credentials::read
#define CredEnumerateW qkvm::testing::credentials::enumerate
#define CredDeleteW qkvm::testing::credentials::erase
#define CredFree qkvm::testing::credentials::release
// Any other call of Credential Manager is none this in memory has: it does not build.
#define CredWriteA not_in_memory
#define CredReadA not_in_memory
#define CredEnumerateA not_in_memory
#define CredDeleteA not_in_memory
#define CredRenameW not_in_memory
#define CredRenameA not_in_memory
#define CredWriteDomainCredentialsW not_in_memory
#define CredReadDomainCredentialsW not_in_memory
#define CredGetTargetInfoW not_in_memory
#define CredMarshalCredentialW not_in_memory
#define CredUnmarshalCredentialW not_in_memory
#define CredProtectW not_in_memory
#define CredUnprotectW not_in_memory
#define CredFindBestCredentialW not_in_memory
#define CredGetSessionTypes not_in_memory

// (The source itself, on purpose: this build's own code, once more, against the store in memory.)
#include "../../core/src/windows_secrets.cpp" // NOLINT(bugprone-suspicious-include)

#undef CredWriteW
#undef CredReadW
#undef CredEnumerateW
#undef CredDeleteW
#undef CredFree
#undef CredWriteA
#undef CredReadA
#undef CredEnumerateA
#undef CredDeleteA
#undef CredRenameW
#undef CredRenameA
#undef CredWriteDomainCredentialsW
#undef CredReadDomainCredentialsW
#undef CredGetTargetInfoW
#undef CredMarshalCredentialW
#undef CredUnmarshalCredentialW
#undef CredProtectW
#undef CredUnprotectW
#undef CredFindBestCredentialW
#undef CredGetSessionTypes
#undef windowsSecrets
#undef windowsTargetOf
