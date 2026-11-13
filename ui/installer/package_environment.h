#pragma once
namespace qkvm::installer {
// Before Qt starts: clear inherited overrides in both CRT and Windows environments.
void preparePackageCheckEnvironment();
} // namespace qkvm::installer
