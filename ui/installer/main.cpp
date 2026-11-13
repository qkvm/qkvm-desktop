#include "error_code.h"
#include "health_exchange.h"
#include "native_health.h"
#include "setup_context.h"
#include "setup_launch.h"
#include "transaction.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <appxpackaging.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <softpub.h>
#include <windows.h>

#include <appmodel.h>
#include <wintrust.h>
#include <wrl/client.h>

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Management.Deployment.h>
#include <winrt/Windows.Storage.h>

#include "../win/cache_paths.h"
#include "installer_config.h"

using Microsoft::WRL::ComPtr;
using namespace qkvm::installer;
using namespace winrt::Windows::Data::Json;
using winrt::Windows::Management::Deployment::DeploymentOptions;
using winrt::Windows::Management::Deployment::PackageManager;
namespace fs = std::filesystem;

namespace {
class Handle {
public:
    explicit Handle(HANDLE value) : m_value(value) {}
    ~Handle()
    {
        if (m_value != nullptr && m_value != INVALID_HANDLE_VALUE)
            CloseHandle(m_value);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return m_value; }

private:
    HANDLE m_value;
};

class Progress {
public:
    Progress()
    {
        std::promise<DWORD> ready;
        auto window = ready.get_future();
        m_thread = std::thread([ready = std::move(ready)]() mutable {
            HWND shown = CreateWindowExW(WS_EX_TOPMOST, L"STATIC", L"qKVM Setup", WS_OVERLAPPED | WS_CAPTION,
                                         CW_USEDEFAULT, CW_USEDEFAULT, 460, 120, nullptr, nullptr, nullptr, nullptr);
            if (shown != nullptr) {
                CreateWindowExW(
                    0, L"STATIC",
                    L"Checking and applying the installation.\nPlease wait for the result before reopening qKVM.",
                    WS_CHILD | WS_VISIBLE, 18, 18, 420, 60, shown, nullptr, nullptr, nullptr);
                ShowWindow(shown, SW_SHOWNORMAL);
                UpdateWindow(shown);
            }
            ready.set_value(shown != nullptr ? GetCurrentThreadId() : 0);
            if (shown == nullptr)
                return;
            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            DestroyWindow(shown);
        });
        m_threadId = window.get();
    }
    ~Progress()
    {
        if (m_threadId != 0)
            PostThreadMessageW(m_threadId, WM_QUIT, 0, 0);
        if (m_thread.joinable())
            m_thread.join();
    }
    Progress(const Progress&) = delete;
    Progress& operator=(const Progress&) = delete;

private:
    DWORD m_threadId = 0;
    std::thread m_thread;
};

void require(bool yes, const char* message)
{
    if (!yes)
        throw std::runtime_error(message);
}

std::wstring uniqueName()
{
    GUID id{};
    winrt::check_hresult(CoCreateGuid(&id));
    wchar_t text[40]{};
    require(StringFromGUID2(id, text, 40) != 0, "A unique installation name could not be created.");
    return text;
}

bool plainPath(const fs::path& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

fs::path cacheRoot()
{
    const auto family = qkvm::storage::familyFor(packageName, packagePublisher);
    const auto root = qkvm::storage::cachePath(qkvm::storage::localData(), family, qkvm::storage::Area::Installer);
    qkvm::storage::prepareDirectory(root);
    return root;
}

JsonObject readJson(const fs::path& file)
{
    require(plainPath(file) && fs::file_size(file) <= 16384, "The installation record is missing or invalid.");
    std::ifstream input(file, std::ios::binary);
    require(input.good(), "The installation record could not be read.");
    const std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    return JsonObject::Parse(winrt::to_hstring(bytes));
}

std::uint64_t versionOf(const winrt::Windows::ApplicationModel::PackageVersion& version)
{
    return (static_cast<std::uint64_t>(version.Major) << 48U) | (static_cast<std::uint64_t>(version.Minor) << 32U) |
           (static_cast<std::uint64_t>(version.Build) << 16U) | version.Revision;
}

std::wstring versionText(std::uint64_t version)
{
    return std::to_wstring(version >> 48U) + L"." + std::to_wstring((version >> 32U) & 65535U) + L"." +
           std::to_wstring((version >> 16U) & 65535U);
}

std::wstring appxString(const std::function<HRESULT(LPWSTR*)>& get)
{
    LPWSTR text = nullptr;
    winrt::check_hresult(get(&text));
    const std::wstring value(text);
    CoTaskMemFree(text);
    return value;
}

Package inspect(const fs::path& file, bool trust)
{
    require(plainPath(file) && !fs::is_directory(file) && fs::file_size(file) <= 1024ULL * 1024 * 1024,
            "The installer package is missing, redirected or too large.");
    const Handle locked(CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    require(locked.get() != INVALID_HANDLE_VALUE, "The installer package could not be locked for verification.");
    if (trust) {
        WINTRUST_FILE_INFO information{};
        information.cbStruct = sizeof(information);
        information.pcwszFilePath = file.c_str();
        WINTRUST_DATA data{};
        data.cbStruct = sizeof(data);
        data.dwUIChoice = WTD_UI_NONE;
        data.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
        data.dwUnionChoice = WTD_CHOICE_FILE;
        data.pFile = &information;
        data.dwStateAction = WTD_STATEACTION_VERIFY;
        data.dwProvFlags = WTD_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT;
        GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
        const LONG result = WinVerifyTrust(nullptr, &policy, &data);
        data.dwStateAction = WTD_STATEACTION_CLOSE;
        static_cast<void>(WinVerifyTrust(nullptr, &policy, &data));
        require(result == ERROR_SUCCESS, "Windows does not trust this installer signature. Nothing was installed.");
    }
    ComPtr<IStream> stream;
    winrt::check_hresult(
        SHCreateStreamOnFileEx(file.c_str(), STGM_READ | STGM_SHARE_DENY_WRITE, 0, FALSE, nullptr, &stream));
    ComPtr<IAppxFactory> factory;
    winrt::check_hresult(CoCreateInstance(CLSID_AppxFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
    ComPtr<IAppxPackageReader> reader;
    winrt::check_hresult(factory->CreatePackageReader(stream.Get(), &reader));
    ComPtr<IAppxManifestReader> manifest;
    winrt::check_hresult(reader->GetManifest(&manifest));
    ComPtr<IAppxManifestPackageId> identity;
    winrt::check_hresult(manifest->GetPackageId(&identity));
    const auto name = appxString([&](LPWSTR* value) { return identity->GetName(value); });
    const auto publisher = appxString([&](LPWSTR* value) { return identity->GetPublisher(value); });
    const std::wstring expectedName = packagePublisher[0] == L'\0' ? L"qKVM.Desktop.Development" : packageName;
    const std::wstring expectedPublisher = packagePublisher[0] == L'\0' ? L"CN=qKVM Development" : packagePublisher;
    require(name == expectedName && publisher == expectedPublisher && (!trust || packagePublisher[0] != L'\0'),
            "This installer is not configured for that qKVM publisher and product.");
    APPX_PACKAGE_ARCHITECTURE architecture{};
    winrt::check_hresult(identity->GetArchitecture(&architecture));
    require(architecture == APPX_PACKAGE_ARCHITECTURE_X64, "This installer requires the Windows x64 package.");
    UINT64 version = 0;
    winrt::check_hresult(identity->GetVersion(&version));
    ComPtr<IAppxFile> schemaFile;
    winrt::check_hresult(reader->GetPayloadFile(L"qkvm-release.json", &schemaFile));
    UINT64 size = 0;
    winrt::check_hresult(schemaFile->GetSize(&size));
    require(size > 0 && size <= 16384, "The package has no valid profile compatibility record.");
    ComPtr<IStream> schemaStream;
    winrt::check_hresult(schemaFile->GetStream(&schemaStream));
    std::string bytes(static_cast<std::size_t>(size), '\0');
    ULONG got = 0;
    winrt::check_hresult(schemaStream->Read(bytes.data(), static_cast<ULONG>(size), &got));
    require(got == size, "The package compatibility record is incomplete.");
    const auto schema = JsonObject::Parse(winrt::to_hstring(bytes));
    require(schema.GetNamedNumber(L"profileSchema") == profileSchema,
            "This package needs a profile migration that this installer does not support.");
    require(schema.GetNamedString(L"version") == versionText(version) && (version & 65535U) == 0,
            "The installer version and compatibility record disagree.");
    return {file.wstring(), appxString([&](LPWSTR* value) { return identity->GetPackageFamilyName(value); }), version,
            profileSchema};
}

std::optional<Package> installed(PackageManager& manager, fs::path* location = nullptr)
{
    std::optional<Package> found;
    for (const auto& item : manager.FindPackagesForUser(L"")) {
        if (item.Id().Name() == packageName && item.Id().Publisher() == packagePublisher) {
            require(!found, "More than one qKVM package is registered. Repair it in Windows Settings first.");
            const fs::path where(item.InstalledLocation().Path().c_str());
            const auto schema = readJson(where / L"qkvm-release.json");
            require(schema.GetNamedNumber(L"profileSchema") == profileSchema,
                    "The installed version uses a different profile format.");
            found = Package{L"", item.Id().FamilyName().c_str(), versionOf(item.Id().Version()), profileSchema};
            if (location != nullptr)
                *location = where;
        }
    }
    return found;
}

JsonObject record(const Package& package)
{
    JsonObject object;
    object.Insert(L"file", JsonValue::CreateStringValue(fs::path(package.file).filename().wstring()));
    object.Insert(L"family", JsonValue::CreateStringValue(package.family));
    object.Insert(L"version", JsonValue::CreateStringValue(std::to_wstring(package.version)));
    object.Insert(L"profileSchema", JsonValue::CreateNumberValue(package.profileSchema));
    return object;
}

Package unrecord(const JsonObject& object, const fs::path& root)
{
    const std::wstring name(object.GetNamedString(L"file"));
    require(!name.empty() && name.find_first_of(L"/\\:") == std::wstring::npos &&
                fs::path(name).extension() == L".msix",
            "The installation history has an invalid package path.");
    const std::wstring number(object.GetNamedString(L"version"));
    require(!number.empty() && number.find_first_not_of(L"0123456789") == std::wstring::npos,
            "The installation history has an invalid version.");
    require(object.GetNamedNumber(L"profileSchema") == profileSchema, "The saved profile format is unsupported.");
    return {(root / name).wstring(), object.GetNamedString(L"family").c_str(), std::stoull(number), profileSchema};
}

History readHistory(const fs::path& root)
{
    const fs::path file = root / L"history.json";
    if (!fs::exists(file))
        return {};
    const auto json = readJson(file);
    require(json.GetNamedNumber(L"schema") == 1, "The installation history format is unsupported.");
    History history;
    if (json.HasKey(L"current"))
        history.current = unrecord(json.GetNamedObject(L"current"), root);
    if (json.HasKey(L"previous"))
        history.previous = unrecord(json.GetNamedObject(L"previous"), root);
    if (json.HasKey(L"pending"))
        history.pending = unrecord(json.GetNamedObject(L"pending"), root);
    return history;
}

bool saveHistory(const fs::path& root, const History& history)
{
    JsonObject json;
    json.Insert(L"schema", JsonValue::CreateNumberValue(1));
    if (history.current)
        json.Insert(L"current", record(*history.current));
    if (history.previous)
        json.Insert(L"previous", record(*history.previous));
    if (history.pending)
        json.Insert(L"pending", record(*history.pending));
    const std::string bytes = winrt::to_string(json.Stringify());
    const fs::path temporary = root / (uniqueName() + L".tmp");
    const fs::path destination = root / L"history.json";
    {
        const Handle file(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr));
        if (file.get() == INVALID_HANDLE_VALUE)
            return false;
        DWORD written = 0;
        if (!WriteFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) ||
            written != bytes.size() || !FlushFileBuffers(file.get()))
            return false;
    }
    return MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

bool healthy(PackageManager& manager, const Package& package, std::wstring& problem)
{
    NativeHealthApi api;
    api.registered = [&] {
        fs::path location;
        const auto registration = installed(manager, &location);
        return HealthRegistration{registration, (location / L"qkvm-desktop.exe").wstring()};
    };
    return qkvm::installer::healthy(package, problem, api);
}
void pruneOldPackages(const fs::path& root, const History& history)
{
    if (history.pending)
        return;
    for (const auto& entry : fs::directory_iterator(root)) {
        const auto& path = entry.path();
        const auto name = path.filename().wstring();
        if (!plainPath(path) || !entry.is_regular_file() || name.size() != 43 || name.front() != L'{' ||
            name.substr(37) != L"}.msix" ||
            name.substr(1, 36).find_first_not_of(L"0123456789abcdefABCDEF-") != std::wstring::npos ||
            (history.current && path == fs::path(history.current->file)) ||
            (history.previous && path == fs::path(history.previous->file)))
            continue;
        if (fs::file_time_type::clock::now() - entry.last_write_time() > std::chrono::hours(24 * 7))
            static_cast<void>(DeleteFileW(path.c_str()));
    }
}

void showSetupFailure(const std::wstring& problem)
{
    MessageBoxW(nullptr, problem.c_str(), L"qKVM Setup", MB_OK | MB_ICONERROR);
}

[[maybe_unused]] int run(std::vector<std::wstring> arguments, const SetupContextApi& context = {},
                         const SetupLaunchApi& launch = {},
                         const std::function<void(const std::wstring&)>& report = showSetupFailure)
{
    if (arguments.size() == 3 && arguments[1] == L"--check-helper" && arguments[2] == L"--relay") {
        const auto result =
            launchSetup(fs::absolute(arguments[0]).wstring(), {L"--check-helper"}, SetupHop::Relay, true);
        if (result == 0)
            std::cout << "QKVM_RELAY_OK\n";
        return static_cast<int>(result);
    }
    if (arguments.size() > 2 && arguments[1] == L"--relay") {
        const std::vector<std::wstring> forwarded(arguments.begin() + 2, arguments.end());
        const bool check = forwarded == std::vector<std::wstring>{L"--check-helper"};
        const auto error =
            launchSetup(fs::absolute(arguments[0]).wstring(), forwarded, SetupHop::Installer, check, launch);
        if (error != ERROR_SUCCESS && !check)
            report(L"Setup could not start (" + errorCode(L"Win32", error) +
                   L"). No installation was started. Open the separate Setup folder and try again.");
        return static_cast<int>(error);
    }
    if (arguments.size() == 2 && arguments[1] == L"--check-helper") {
        std::cout << "QKVM_SETUP_OK\n";
        return 0;
    }
    if (arguments.size() == 3 && (arguments[1] == L"--inspect" || arguments[1] == L"--verify")) {
        const Package package = inspect(arguments[2], arguments[1] == L"--verify");
        std::cout << "QKVM_INSTALLER_PACKAGE_OK " << winrt::to_string(versionText(package.version)) << '\n';
        return 0;
    }
    if (const auto problem = setupContextProblem(context); !problem.empty())
        throw winrt::hresult_error(E_ACCESSDENIED, problem);
#ifdef QKVM_INSTALLER_TEST
    // The compiled run() probe may reach the guard, never any deployment or user confirmation.
    require(false, "The test reached the deployment boundary.");
#endif
    require(packagePublisher[0] != L'\0',
            "This development build has no Windows signing publisher. Production installation is disabled.");
    if (arguments.size() == 1) {
        const auto package = fs::absolute(arguments[0]).parent_path() / L"qkvm.msix";
        require(fs::is_regular_file(package),
                "Open Settings > Updates, or use the qKVM Setup folder containing qkvm.msix.");
        arguments.emplace_back(L"--install");
        arguments.push_back(package.wstring());
    }
    const bool rollback = arguments.size() == 2 && arguments[1] == L"--rollback";
    const bool repair = arguments.size() == 2 && arguments[1] == L"--recover";
    const bool install = (arguments.size() == 3 || (arguments.size() == 5 && arguments[3] == L"--version")) &&
                         arguments[1] == L"--install";
    require(rollback || repair || install,
            "Use qKVM Setup to install an MSIX, return to the previous version, or recover an interrupted update.");
    std::optional<Package> offered;
    if (install) {
        offered = inspect(fs::absolute(arguments[2]), true);
        require(arguments.size() != 5 || arguments[4] == versionText(offered->version),
                "The installer version does not match the update that was offered. Check for updates again.");
    }
    std::wstring prompt =
        rollback ? L"Return to the previous verified version of qKVM?" : L"Recover the interrupted qKVM installation?";
    if (offered)
        prompt = L"Install qKVM " + versionText(offered->version) + L"?";
    if (MessageBoxW(nullptr,
                    (prompt + L"\n\nClose all qKVM windows and choose Quit from its tray icon first. "
                              L"Your settings and saved machines will be kept. If an update cannot start, "
                              L"Setup will try to restore the previous version. No running app is forced to close.")
                        .c_str(),
                    L"qKVM Setup", MB_OKCANCEL | MB_ICONINFORMATION | MB_DEFBUTTON2) != IDOK)
        return 0;
    // Hold the application's session mutex throughout deployment and its profile-free check.
    const Handle application(CreateMutexW(nullptr, FALSE, L"Local\\QKVM Desktop"));
    const DWORD applicationStatus = GetLastError();
    require(application.get() != nullptr && applicationStatus != ERROR_ALREADY_EXISTS,
            "qKVM is still running. Choose Quit from its tray icon, then try Setup again.");
    const fs::path root = cacheRoot();
    const Handle lock(CreateFileW((root / L"setup.lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr));
    require(lock.get() != INVALID_HANDLE_VALUE, "Another qKVM installation is already running.");
    History history = readHistory(root);
    PackageManager manager;
    std::wstring deployProblem;
    std::wstring healthProblem;
    const Backend backend{
        [&](const Package& package) {
            try {
                return inspect(package.file, true) == package;
            } catch (...) {
                return false;
            }
        },
        [&] { return installed(manager); }, [&](const History& next) { return saveHistory(root, next); },
        [&](const Package& package, bool older) {
            try {
                // This read handle remains held through deployment, not just verification.
                const Handle held(CreateFileW(package.file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                              OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
                require(held.get() != INVALID_HANDLE_VALUE && inspect(package.file, true) == package,
                        "The cached package no longer passes verification.");
                std::vector<wchar_t> url(65536, L'\0');
                DWORD length = static_cast<DWORD>(url.size());
                winrt::check_hresult(UrlCreateFromPathW(package.file.c_str(), url.data(), &length, 0));
                const auto uri = winrt::Windows::Foundation::Uri(url.data());
                const auto result =
                    manager
                        .AddPackageAsync(uri, nullptr,
                                         older ? DeploymentOptions::ForceUpdateFromAnyVersion : DeploymentOptions::None)
                        .get();
                if (result.ExtendedErrorCode().value < 0) {
                    deployProblem = result.ErrorText();
                    return false;
                }
                return true;
            } catch (const winrt::hresult_error& error) {
                deployProblem = error.message();
                return false;
            } catch (const std::exception& error) {
                deployProblem = winrt::to_hstring(error.what());
                return false;
            }
        },
        [&](const Package& package) { return healthy(manager, package, healthProblem); }};
    Result result = Result::Refused;
    {
        const Progress progress;
        if (repair) {
            result = recover(history, backend);
        } else {
            require(!history.pending,
                    "An interrupted installation needs recovery before another update. Choose Recover installation.");
            Package candidate;
            if (rollback) {
                require(history.previous.has_value(), "No previous verified installer is available on this PC.");
                candidate = *history.previous;
            } else {
                const fs::path source = fs::absolute(arguments[2]);
                static_cast<void>(inspect(source, true));
                const fs::path copy = root / (uniqueName() + L".msix");
                fs::copy_file(source, copy, fs::copy_options::none);
                candidate = inspect(copy, true);
            }
            result = apply(history, candidate, rollback, backend);
        }
    }
    // Only obsolete installer packages are pruned; current/previous and unresolved recovery
    // records always survive. Cache maintenance cannot turn a completed update into failure.
    try {
        pruneOldPackages(root, history);
    } catch (...) {
        OutputDebugStringW(L"qKVM Setup retained obsolete packages because cache cleanup was unavailable.\n");
    }
    const std::wstring running = history.current ? versionText(history.current->version) : std::wstring();
    const auto detail = resultMessage(result, running, deployProblem, healthProblem);
    MessageBoxW(nullptr, detail.c_str(), L"qKVM Setup",
                MB_OK | (informational(result) ? MB_ICONINFORMATION : MB_ICONWARNING));
    return setupExitCode(result, repair);
}

#ifdef QKVM_INSTALLER_TEST
BOOL WINAPI refuseRelayChild(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID,
                             LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION)
{
    SetLastError(ERROR_FILE_NOT_FOUND);
    return FALSE;
}
int testStorage(const fs::path& parent)
{
    for (int kind = 0; kind < 3; ++kind) {
        SetupContextApi context;
        context.packageStatus = [=] { return kind == 0 ? ERROR_INSUFFICIENT_BUFFER : APPMODEL_ERROR_NO_PACKAGE; };
        context.elevation = [=](bool& elevated) {
            elevated = kind == 1;
            return kind == 2 ? ERROR_ACCESS_DENIED : ERROR_SUCCESS;
        };
        bool refused = false;
        try {
            static_cast<void>(run({L"synthetic-setup.exe", L"--recover"}, context));
        } catch (const winrt::hresult_error& error) {
            refused = error.code() == E_ACCESSDENIED && std::wstring(error.message()).starts_with(L"Setup ");
        }
        require(refused, "run() did not enforce its context guard before the deployment boundary.");
    }
    std::cout << "Setup run context guard: packaged, elevated and unreadable token refused before deployment.\n";
    SetupLaunchApi launch;
    launch.create = refuseRelayChild;
    int reports = 0;
    std::wstring reported;
    const auto report = [&](const std::wstring& problem) {
        ++reports;
        reported = problem;
    };
    const int code = run({L"synthetic-setup.exe", L"--relay", L"--rollback"}, {}, launch, report);
    require(code == ERROR_FILE_NOT_FOUND, "The relay lost its child-start error.");
    require(reports == 1 && reported.find(L"Win32 0x00000002") != std::wstring::npos,
            "The relay silently lost its child-start error.");
    reports = 0;
    static_cast<void>(run({L"synthetic-setup.exe", L"--relay", L"--check-helper"}, {}, launch, report));
    require(reports == 0, "The read-only diagnostic displayed an interactive error.");
    std::cout << "Setup relay: child-start failure is visible; read-only check remains noninteractive.\n";

    const fs::path root = parent / uniqueName();
    fs::create_directories(root);
    const Package first{(root / L"first.msix").wstring(), L"test_family", 1, 1};
    const Package second{(root / L"second.msix").wstring(), L"test_family", 2, 1};
    History history{first, std::nullopt, std::nullopt};
    require(saveHistory(root, history), "Initial journal write failed.");
    require(readHistory(root) == history, "Initial journal did not round-trip.");
    history.pending = second;
    require(saveHistory(root, history), "Pending journal replacement failed.");
    require(readHistory(root) == history, "Pending journal was not retained.");
    {
        const Handle locked(CreateFileW((root / L"history.json").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        require(locked.get() != INVALID_HANDLE_VALUE, "Journal test lock failed.");
        require(!saveHistory(root, {}), "A locked journal was incorrectly reported as replaced.");
        require(readHistory(root) == history, "Failed replacement damaged the prior journal.");
    }
    History committed{second, first, std::nullopt};
    require(saveHistory(root, committed) && readHistory(root) == committed, "Commit did not replace the journal.");
    JsonObject invalid = record(first);
    invalid.SetNamedValue(L"file", JsonValue::CreateStringValue(L"../outside.msix"));
    bool refused = false;
    try {
        static_cast<void>(unrecord(invalid, root));
    } catch (...) {
        refused = true;
    }
    require(refused, "A history path escaped the installation cache.");
    std::cout
        << "Installer storage: atomic replacement, interrupted record, write failure and path rejection passed.\n";
    return 0;
}
#endif
} // namespace

#ifndef QKVM_INSTALLER_TEST
// A shipped program (its own test, test_installer_storage, is not): base/tests/quiet_failures.cpp defines this too.
extern "C" const int qkvmTestProgram = 0;
#endif

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int count = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &count);
    if (argv == nullptr)
        return 1;
    std::vector<std::wstring> arguments(argv, argv + count);
    LocalFree(static_cast<void*>(argv));
#ifdef QKVM_INSTALLER_TEST
    const bool checking = true;
#else
    const bool checking =
        arguments.size() > 1 &&
        (arguments[1] == L"--verify" || arguments[1] == L"--inspect" || arguments[1] == L"--check-helper" ||
         (arguments.size() == 3 && arguments[1] == L"--relay" && arguments[2] == L"--check-helper"));
#endif
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
#ifdef QKVM_INSTALLER_TEST
        require(arguments.size() == 2, "The storage test needs an explicit disposable directory.");
        return testStorage(arguments[1]);
#else
        return run(arguments);
#endif
    } catch (const winrt::hresult_error& error) {
        std::cerr << winrt::to_string(error.message()) << '\n';
        if (!checking)
            MessageBoxW(nullptr, error.message().c_str(), L"qKVM Setup", MB_OK | MB_ICONERROR);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        if (!checking)
            MessageBoxW(nullptr, winrt::to_hstring(error.what()).c_str(), L"qKVM Setup", MB_OK | MB_ICONERROR);
    }
    return 1;
}
