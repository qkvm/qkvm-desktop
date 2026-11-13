#include "tray.h"

#include <iterator>
#include <string>
#include <utility>

#include <QImage>
#include <QMetaObject>

// Windows' own headers; the first has to come first.
#include <windows.h>

#include <shellapi.h>
#include <windowsx.h>

namespace qkvm::ui {

namespace {

// What Windows sends when something is done with the icon, and the one icon there is.
constexpr UINT fromTheIcon = WM_APP + 1;
constexpr wchar_t kindOfWindow[] = L"QKVM Desktop notification area";

// Windows' notification area, through the one call it has for it. The icon belongs to a window of
// its own that nobody sees: Windows tells that window what is done with the icon, and that its
// taskbar has started anew (after which every icon has to be put there again).
class WindowsTray final : public TrayIcon {
public:
    WindowsTray()
    {
        WNDCLASSW kind{};
        kind.lpfnWndProc = &WindowsTray::said;
        kind.hInstance = GetModuleHandleW(nullptr);
        kind.lpszClassName = static_cast<const wchar_t*>(kindOfWindow);
        // (Registered already where the program has made one before: that is no failure.)
        RegisterClassW(&kind);
        // Not a window for messages only: such a one is not told that the taskbar is back.
        m_window = CreateWindowExW(0, static_cast<const wchar_t*>(kindOfWindow), L"", WS_OVERLAPPED, 0, 0, 0, 0,
                                   nullptr, nullptr, kind.hInstance, nullptr);
        if (m_window != nullptr) {
            SetWindowLongPtrW(m_window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        }
        m_taskbarIsBack = RegisterWindowMessageW(L"TaskbarCreated");
    }

    ~WindowsTray() override
    {
        takeAway();
        if (m_window != nullptr) {
            SetWindowLongPtrW(m_window, GWLP_USERDATA, 0);
            DestroyWindow(m_window);
        }
        if (m_picture != nullptr) {
            DestroyIcon(m_picture);
        }
    }

    WindowsTray(const WindowsTray&) = delete;
    WindowsTray& operator=(const WindowsTray&) = delete;
    WindowsTray(WindowsTray&&) = delete;
    WindowsTray& operator=(WindowsTray&&) = delete;

    void show(MarkDot dot, const QString& says) override
    {
        m_dot = dot;
        m_says = says;
        m_wanted = true;
        put();
    }

    void hide() override
    {
        m_wanted = false;
        takeAway();
    }

    bool isThere() const override { return m_there; }

    void setMenu(const QList<Row>& rows) override
    {
        if (m_rows == rows)
            return;
        m_rows = rows;
        if (m_menuOpen)
            EndMenu(); // In particular, locking must remove names from an already-open menu.
    }

private:
    void showMessage(const QString& title, const QString& text, bool isWarning) override
    {
        if (!m_there) {
            notificationFinished(false);
            return;
        }
        // Windows 10 can retain a timed-out banner in Notification Center. Its callback has
        // only the icon id, not a message id. Give each message a fresh id so an old click
        // can never open the machine of a newer message. Version 4 carries 16 bits of id.
        if (m_iconId == 65535) {
            notificationFinished(false);
            return;
        }
        NOTIFYICONDATAW previous = named();
        ++m_iconId;
        Shell_NotifyIconW(NIM_DELETE, &previous);
        m_there = false;
        put();
        if (!m_there) {
            notificationFinished(false);
            return;
        }
        NOTIFYICONDATAW icon = named();
        icon.uFlags = NIF_INFO;
        // (Windows takes 63 letters of a heading and 255 of a line; what is longer is cut there.
        // A message without a line is none to Windows: the heading is the line then.)
        const std::wstring heading = (text.isEmpty() ? QString() : title).toStdWString();
        const std::wstring line = (text.isEmpty() ? title : text).toStdWString();
        wcsncpy_s(static_cast<wchar_t*>(icon.szInfoTitle), std::size(icon.szInfoTitle), heading.c_str(), _TRUNCATE);
        wcsncpy_s(static_cast<wchar_t*>(icon.szInfo), std::size(icon.szInfo), line.c_str(), _TRUNCATE);
        icon.dwInfoFlags = (isWarning ? NIIF_WARNING : NIIF_INFO) | NIIF_RESPECT_QUIET_TIME;
        if (Shell_NotifyIconW(NIM_MODIFY, &icon) == FALSE)
            notificationFinished(false);
    }

    NOTIFYICONDATAW named() const
    {
        NOTIFYICONDATAW icon{};
        icon.cbSize = sizeof(icon);
        icon.hWnd = m_window;
        icon.uID = m_iconId;
        return icon;
    }

    void put()
    {
        if (m_window == nullptr) {
            return;
        }
        // As large as Windows draws small icons on this PC.
        HICON picture = markImage(GetSystemMetrics(SM_CXSMICON), m_dot).toHICON();
        NOTIFYICONDATAW icon = named();
        icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        icon.uCallbackMessage = fromTheIcon;
        icon.hIcon = picture;
        // (Windows takes 127 letters, and what is longer is cut there.)
        const std::wstring says = m_says.toStdWString();
        wcsncpy_s(static_cast<wchar_t*>(icon.szTip), std::size(icon.szTip), says.c_str(), _TRUNCATE);
        bool done = m_there && Shell_NotifyIconW(NIM_MODIFY, &icon) != FALSE;
        if (!done) {
            // Not there yet, or Windows has lost it: put there afresh, and told that the program
            // speaks the way of today (which is what makes a click come as "chosen").
            done = Shell_NotifyIconW(NIM_ADD, &icon) != FALSE;
            if (done) {
                icon.uVersion = NOTIFYICON_VERSION_4;
                Shell_NotifyIconW(NIM_SETVERSION, &icon);
            }
        }
        m_there = done;
        if (m_picture != nullptr) {
            DestroyIcon(m_picture);
        }
        m_picture = picture;
    }

    void takeAway()
    {
        if (m_there) {
            NOTIFYICONDATAW icon = named();
            Shell_NotifyIconW(NIM_DELETE, &icon);
            m_there = false;
        }
    }

    static void appendRows(HMENU menu, const QList<Row>& rows)
    {
        for (const auto& row : rows) {
            const auto text = row.text.simplified().replace(QStringLiteral("&"), QStringLiteral("&&")).toStdWString();
            const UINT enabled = row.enabled ? MF_ENABLED : MF_GRAYED;
            if (!row.children.isEmpty()) {
                HMENU child = CreatePopupMenu();
                if (child == nullptr)
                    continue;
                appendRows(child, row.children);
                if (!AppendMenuW(menu, MF_POPUP | enabled, reinterpret_cast<UINT_PTR>(child), text.c_str()))
                    DestroyMenu(child);
            } else if (row.id == 0) {
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            } else {
                AppendMenuW(menu, MF_STRING | enabled, static_cast<UINT_PTR>(row.id), text.c_str());
            }
        }
    }

    void offerMenu(int x, int y)
    {
        HMENU menu = CreatePopupMenu();
        if (menu == nullptr) {
            return;
        }
        const auto snapshot = m_rows;
        appendRows(menu, snapshot);
        // Windows closes a menu on a click elsewhere only where the window it belongs to is in front.
        SetForegroundWindow(m_window);
        m_menuOpen = true;
        const int chosenRow =
            TrackPopupMenuEx(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, x, y, m_window, nullptr);
        m_menuOpen = false;
        DestroyMenu(menu);
        if (chosenRow != 0) {
            // Said once Windows is done with the menu, and not from inside it.
            QMetaObject::invokeMethod(
                this, [this, snapshot, chosenRow] { chooseMenuRow(snapshot, chosenRow); }, Qt::QueuedConnection);
        }
    }

    static LRESULT CALLBACK said(HWND window, UINT message, WPARAM more, LPARAM what)
    {
        auto* self = reinterpret_cast<WindowsTray*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (self == nullptr) {
            return DefWindowProcW(window, message, more, what);
        }
        if (message == fromTheIcon) {
            if (HIWORD(what) != self->m_iconId)
                return 0;
            switch (LOWORD(what)) {
            case NIN_SELECT:
            case NIN_KEYSELECT:
                QMetaObject::invokeMethod(self, [self] { emit self->opened(); }, Qt::QueuedConnection);
                break;
            case WM_CONTEXTMENU:
                self->offerMenu(GET_X_LPARAM(more), GET_Y_LPARAM(more));
                break;
            case NIN_BALLOONUSERCLICK:
                self->notificationFinished(true);
                break;
            case NIN_BALLOONHIDE:
            case NIN_BALLOONTIMEOUT:
                self->notificationFinished(false);
                break;
            default:
                break;
            }
            return 0;
        }
        if (message == self->m_taskbarIsBack && self->m_wanted) {
            self->clearNotifications();
            self->notificationFinished(false);
            self->m_there = false;
            self->put();
            return 0;
        }
        return DefWindowProcW(window, message, more, what);
    }

    HWND m_window = nullptr;
    UINT m_iconId = 1;
    UINT m_taskbarIsBack = 0;
    HICON m_picture = nullptr;
    MarkDot m_dot = MarkDot::None;
    QString m_says;
    QList<Row> m_rows;
    bool m_wanted = false; // somebody asked for it to be there
    bool m_there = false;  // and Windows has it
    bool m_menuOpen = false;
};

} // namespace

std::unique_ptr<TrayIcon> windowsTray()
{
    return std::make_unique<WindowsTray>();
}

} // namespace qkvm::ui
