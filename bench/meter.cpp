#include "meter.h"

#include <algorithm>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>

#include <qkvm/base/clock.h>

namespace qkvm::bench {

namespace {

std::uint64_t ticks(const FILETIME& time) noexcept
{
    return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

} // namespace

// Windows keeps a counter for each engine of each graphics card as each program uses it. The ones
// of this program, of one kind, are asked for together and added up.
struct Meter::Counters {
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER decoding = nullptr;
    PDH_HCOUNTER drawing = nullptr;
    PDH_HCOUNTER video = nullptr; // (each card's own memory this program holds: GPU Process Memory)

    Counters()
    {
        if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) {
            query = nullptr;
            return;
        }
        const std::wstring pid = std::to_wstring(GetCurrentProcessId());
        const std::wstring ours = L"\\GPU Engine(pid_" + pid + L"_*engtype_";
        PdhAddEnglishCounterW(query, (ours + L"VideoDecode)\\Utilization Percentage").c_str(), 0, &decoding);
        PdhAddEnglishCounterW(query, (ours + L"3D)\\Utilization Percentage").c_str(), 0, &drawing);
        PdhAddEnglishCounterW(query, (L"\\GPU Process Memory(pid_" + pid + L"_*)\\Dedicated Usage").c_str(), 0, &video);
        PdhCollectQueryData(query);
    }

    ~Counters()
    {
        if (query != nullptr) {
            PdhCloseQuery(query);
        }
    }

    static double sumOf(PDH_HCOUNTER counter)
    {
        if (counter == nullptr) {
            return 0;
        }
        DWORD bytes = 0;
        DWORD count = 0;
        if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bytes, &count, nullptr) !=
            static_cast<PDH_STATUS>(PDH_MORE_DATA)) {
            return 0;
        }
        std::vector<std::byte> room(bytes);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(room.data());
        if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bytes, &count, items) != ERROR_SUCCESS) {
            return 0;
        }
        double sum = 0;
        for (DWORD index = 0; index < count; ++index) {
            if (items[index].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA ||
                items[index].FmtValue.CStatus == PDH_CSTATUS_NEW_DATA) {
                sum += items[index].FmtValue.doubleValue;
            }
        }
        return sum;
    }
};

Meter::Meter() : m_counters(std::make_unique<Counters>()) {}

Meter::~Meter() = default;

Cost Meter::ask()
{
    Cost cost;
    const HANDLE self = GetCurrentProcess();

    FILETIME made{};
    FILETIME ended{};
    FILETIME inWindows{};
    FILETIME inTheProgram{};
    const std::int64_t now = base::steadyNowUs();
    if (GetProcessTimes(self, &made, &ended, &inWindows, &inTheProgram) != 0) {
        const std::uint64_t used = ticks(inWindows) + ticks(inTheProgram); // in tenths of a microsecond
        const unsigned processors = std::max(1U, std::thread::hardware_concurrency());
        if (m_lastAskedUs != 0 && now > m_lastAskedUs) {
            cost.processorPercent = static_cast<double>(used - m_lastProcessorTime) / 10.0 /
                                    static_cast<double>(now - m_lastAskedUs) / processors * 100.0;
        }
        m_lastProcessorTime = used;
        m_lastAskedUs = now;
    }

    if (m_counters->query != nullptr && PdhCollectQueryData(m_counters->query) == ERROR_SUCCESS) {
        cost.decodingPercent = Counters::sumOf(m_counters->decoding);
        cost.drawingPercent = Counters::sumOf(m_counters->drawing);
        cost.videoBytes = static_cast<std::uint64_t>(std::max(0.0, Counters::sumOf(m_counters->video)));
    }

    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    if (GetProcessMemoryInfo(self, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)) != 0) {
        cost.committedBytes = memory.PrivateUsage;
    }
    DWORD handles = 0;
    if (GetProcessHandleCount(self, &handles) != 0) {
        cost.handles = handles;
    }
    cost.windowObjects = GetGuiResources(self, GR_GDIOBJECTS) + GetGuiResources(self, GR_USEROBJECTS);
    return cost;
}

} // namespace qkvm::bench
