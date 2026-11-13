#pragma once

// How long a KVM is given, by whoever speaks to one: the driver that signs in to it (KvmdDevice)
// and the asking of what answers at its address, which goes before every sign-in (KvmProbe). One
// place for both, so that the two cannot come apart again: an asking that had less time than the
// driver said "nothing answers" of a KVM the driver would have reached (one behind a VPN or a
// relay, or one that is busy, lets a line in after two to four seconds), and since nothing is
// signed in to that was not asked first, that KVM was never on the wall.
namespace qkvm::core::kvm {

// To be let in at all. A KVM that is not there takes Windows far longer to give up on.
inline constexpr int letInWithinMs = 4500;
// The longest nothing may come while an answer is waited for.
inline constexpr int quietMs = 8000;
// And how much longer than those two together a whole answer may take.
inline constexpr int transferMs = 24'000;

} // namespace qkvm::core::kvm
