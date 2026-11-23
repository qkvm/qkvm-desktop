// signer_far_away.h's word with Windows: which program a line on this PC's own address comes from.

#include "signer_far_away.h"

#include <QtEndian>

#include <vector>

#include <winsock2.h>
// (After winsock2.h, which it leans on.)
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")

namespace qkvm::core::testing {

qint64 programOfALine(quint16 theirPort, quint16 ourPort)
{
    // (Asked again where the table grew between asking its size and reading it.)
    for (int again = 0; again < 4; ++again) {
        ULONG size = 0;
        GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_CONNECTIONS, 0);
        std::vector<MIB_TCPROW_OWNER_PID> rows(size / sizeof(MIB_TCPROW_OWNER_PID) + 16);
        size = static_cast<ULONG>(rows.size() * sizeof(MIB_TCPROW_OWNER_PID));
        const DWORD said = GetExtendedTcpTable(rows.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_CONNECTIONS, 0);
        if (said == ERROR_INSUFFICIENT_BUFFER) {
            continue;
        }
        if (said != NO_ERROR) {
            return 0;
        }
        // (The table is its count and its rows after it, as Windows lays it out in what it was given.)
        const auto* table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(rows.data());
        for (DWORD at = 0; at < table->dwNumEntries; ++at) {
            const MIB_TCPROW_OWNER_PID& row =
                table->table[at]; // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
            // (Each port in the low two bytes, in the network's order.)
            if (qFromBigEndian(static_cast<quint16>(row.dwLocalPort)) == theirPort &&
                qFromBigEndian(static_cast<quint16>(row.dwRemotePort)) == ourPort) {
                return row.dwOwningPid;
            }
        }
        return 0;
    }
    return 0;
}

} // namespace qkvm::core::testing
