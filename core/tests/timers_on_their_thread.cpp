// Compiled into every test program that has a stand-in (qkvm_core_stand_in: every program that speaks to a server or a
// KVM over the network). Where Qt says that an object was used from another thread than its own, the program ends at
// once, with what Qt said and where it was said on stderr (the quiet failures of base/tests: no window).
//
// Why (the fix round of 2026-11-05): a rare access violation in test_ui_server_session, about once in ten whole runs on
// the loaded slow cores, came after Qt's "QObject::killTimer: Timers cannot be stopped from another thread" and
// "QObject::~QObject: Timers cannot be stopped from another thread": a main-thread object with a running timer
// destroyed on another thread, its timer firing later on what was gone. A warning in a test's log was read by nobody,
// and the crash came later in another case. From now on the case where it is said fails there, and says from where.
//
// Installed at the first turn of the loop, behind Qt Test's own handler (which Qt Test installs as its run begins and
// which hands nothing on), and handing on to it whatever else is said.

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <QCoreApplication>
#include <QString>
#include <QTimer>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

namespace {

QtMessageHandler heardBefore = nullptr;

// Where it was said: each frame's library and, where Windows can tell it, its function.
void sayWhere()
{
    void* frames[48]{};
    const USHORT count = CaptureStackBackTrace(0, 48, frames, nullptr);
    const HANDLE process = GetCurrentProcess();
    static const bool symbols = SymInitialize(process, nullptr, TRUE) != FALSE;
    for (USHORT at = 0; at < count; ++at) {
        HMODULE module = nullptr;
        char path[MAX_PATH]{};
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCSTR>(frames[at]), &module) != FALSE) {
            GetModuleFileNameA(module, path, MAX_PATH);
        }
        alignas(SYMBOL_INFO) char room[sizeof(SYMBOL_INFO) + 256]{};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(room);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;
        DWORD64 offset = 0;
        const bool named = symbols && SymFromAddr(process, reinterpret_cast<DWORD64>(frames[at]), &offset, symbol);
        const char* name = std::strrchr(path, '\\');
        std::fprintf(stderr, "QKVM test:   %2u %s %s\n", static_cast<unsigned>(at), name != nullptr ? name + 1 : path,
                     named ? symbol->Name : "?");
    }
}

void heard(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    if (message.contains(QLatin1StringView("Timers cannot be stopped from another thread")) ||
        message.contains(QLatin1StringView("Cannot create children for a parent that is in a different thread"))) {
        std::fprintf(stderr, "QKVM test: Qt says an object was used from another thread than its own: %s\n",
                     qPrintable(message));
        sayWhere();
        std::fflush(stderr);
        std::abort();
    }
    if (heardBefore != nullptr) {
        heardBefore(type, context, message);
    }
}

void listenSoon()
{
    QTimer::singleShot(0, QCoreApplication::instance(), [] {
        if (heardBefore == nullptr) {
            heardBefore = qInstallMessageHandler(heard);
        }
    });
}

} // namespace

Q_COREAPP_STARTUP_FUNCTION(listenSoon)
