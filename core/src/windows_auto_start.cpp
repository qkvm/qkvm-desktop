#include <qkvm/core/auto_start.h>

#include <future>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <QDir>

// Windows' own header.
#include <windows.h>

#include <appmodel.h>

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.h>

namespace qkvm::core {

namespace {

// More than this is not a line that starts a program, nor a note of Windows' about one.
constexpr DWORD largestValueBytes = 8192;

base::Error refused(LSTATUS status)
{
    return {"auto_start", "Windows did not take it (error " + std::to_string(status) + ")."};
}

// What a value of the user's own registry holds, where it is of this kind; nothing where there is
// none, or where it is of another kind or larger than such a value is.
std::optional<std::vector<BYTE>> valueOf(const QString& key, const QString& name, DWORD kind)
{
    const std::wstring where = key.toStdWString();
    const std::wstring what = name.toStdWString();
    std::vector<BYTE> held(largestValueBytes);
    auto size = static_cast<DWORD>(held.size());
    if (RegGetValueW(HKEY_CURRENT_USER, where.c_str(), what.c_str(), kind, nullptr, held.data(), &size) !=
        ERROR_SUCCESS) {
        return std::nullopt;
    }
    held.resize(size);
    return held;
}

// Gone afterwards: taken away, or never there.
base::Result<void> remove(const QString& key, const QString& name)
{
    const LSTATUS status =
        RegDeleteKeyValueW(HKEY_CURRENT_USER, key.toStdWString().c_str(), name.toStdWString().c_str());
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND && status != ERROR_PATH_NOT_FOUND) {
        return refused(status);
    }
    return {};
}

class WindowsAutoStart final : public AutoStart {
public:
    WindowsAutoStart(const QString& program, QString name, StartKeys keys)
        : m_line(u'"' + QDir::toNativeSeparators(program) + u'"'), m_name(std::move(name)), m_keys(std::move(keys))
    {}

    bool isOn() override
    {
        const std::optional<std::vector<BYTE>> listed = valueOf(m_keys.list, m_name, RRF_RT_REG_SZ);
        if (!listed) {
            return false;
        }
        // An entry for a file somewhere else is not this program's: a copy that was moved or
        // removed since. (Windows does not tell capitals from small letters in a place.)
        QString line = QString::fromWCharArray(reinterpret_cast<const wchar_t*>(listed->data()),
                                               static_cast<qsizetype>(listed->size() / sizeof(wchar_t)));
        // (Windows hands a line over with the zero that ends it.)
        if (const qsizetype end = line.indexOf(QChar(u'\0')); end >= 0) {
            line.truncate(end);
        }
        if (line.compare(m_line, Qt::CaseInsensitive) != 0) {
            return false;
        }
        // What somebody switched off in Windows' settings stays on the list, and Windows notes
        // beside it that it is off: a first byte that is odd.
        const std::optional<std::vector<BYTE>> noted = valueOf(m_keys.switchedOff, m_name, RRF_RT_REG_BINARY);
        return !noted || noted->empty() || (noted->front() & 1U) == 0;
    }

    base::Result<void> set(bool on) override
    {
        if (!on) {
            if (const base::Result<void> gone = remove(m_keys.list, m_name); !gone) {
                return gone;
            }
            return remove(m_keys.switchedOff, m_name);
        }
        const std::wstring line = m_line.toStdWString();
        const LSTATUS status =
            RegSetKeyValueW(HKEY_CURRENT_USER, m_keys.list.toStdWString().c_str(), m_name.toStdWString().c_str(),
                            REG_SZ, line.c_str(), static_cast<DWORD>((line.size() + 1) * sizeof(wchar_t)));
        if (status != ERROR_SUCCESS) {
            return refused(status);
        }
        return remove(m_keys.switchedOff, m_name);
    }

private:
    QString m_line;
    QString m_name;
    StartKeys m_keys;
};

// Packaged startup belongs to Windows' package registration, so upgrades keep it and uninstall
// removes it. Windows' explicit user/policy disablement is respected. No raw Run entry is added.
class Apartment {
public:
    Apartment() { winrt::init_apartment(winrt::apartment_type::multi_threaded); }
    ~Apartment() { winrt::uninit_apartment(); }
    Apartment(const Apartment&) = delete;
    Apartment& operator=(const Apartment&) = delete;
};

class PackagedAutoStart final : public AutoStart {
public:
    bool isOn() override
    {
        if (windowsPackageFamily().isEmpty())
            return false;
        try {
            return std::async(std::launch::async,
                              [] {
                                  Apartment apartment;
                                  const auto operation =
                                      winrt::Windows::ApplicationModel::StartupTask::GetAsync(L"qKVMStartup");
                                  if (!operation)
                                      return false;
                                  const auto task = operation.get();
                                  using State = winrt::Windows::ApplicationModel::StartupTaskState;
                                  const auto state = task ? task.State() : State::Disabled;
                                  return state == State::Enabled || state == State::EnabledByPolicy;
                              })
                .get();
        } catch (...) {
            return false;
        }
    }

    base::Result<void> set(bool on) override
    {
        if (windowsPackageFamily().isEmpty())
            return base::Error{"auto_start", "This qKVM process has no Windows package identity."};
        try {
            return std::async(std::launch::async,
                              [on]() -> base::Result<void> {
                                  Apartment apartment;
                                  const auto operation =
                                      winrt::Windows::ApplicationModel::StartupTask::GetAsync(L"qKVMStartup");
                                  if (!operation)
                                      return base::Error{"auto_start", "Windows startup is unavailable."};
                                  const auto task = operation.get();
                                  if (!task)
                                      return base::Error{
                                          "auto_start",
                                          "Windows has no startup registration for this qKVM installation."};
                                  using State = winrt::Windows::ApplicationModel::StartupTaskState;
                                  if (!on) {
                                      task.Disable();
                                      if (task.State() == State::EnabledByPolicy)
                                          return base::Error{"auto_start",
                                                             "Windows policy requires this app to start at sign-in."};
                                      return {};
                                  }
                                  const auto state = task.RequestEnableAsync().get();
                                  if (state == State::Enabled || state == State::EnabledByPolicy)
                                      return {};
                                  return base::Error{"auto_start",
                                                     "Windows has disabled startup for qKVM. Enable it in Windows "
                                                     "Settings > Apps > Startup, if your policy allows it."};
                              })
                .get();
        } catch (const winrt::hresult_error&) {
            return base::Error{"auto_start",
                               "Windows could not change qKVM startup. Check Windows Settings > Apps > Startup."};
        } catch (...) {
            return base::Error{"auto_start", "Windows startup is unavailable."};
        }
    }
};

} // namespace

std::unique_ptr<AutoStart> windowsAutoStart(const QString& program, const QString& name, const StartKeys& keys,
                                            const QString& packageFamily)
{
    if (!packageFamily.isEmpty())
        return std::make_unique<PackagedAutoStart>();
    return std::make_unique<WindowsAutoStart>(program, name, keys);
}

QString windowsPackageFamily()
{
    UINT32 length = 0;
    if (GetCurrentPackageFamilyName(&length, nullptr) != ERROR_INSUFFICIENT_BUFFER || length > 1024)
        return {};
    std::vector<wchar_t> family(length);
    return GetCurrentPackageFamilyName(&length, family.data()) == ERROR_SUCCESS ? QString::fromWCharArray(family.data())
                                                                                : QString();
}

} // namespace qkvm::core
