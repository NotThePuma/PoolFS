#include "gui_dialogs.h"

#include <commdlg.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace poolfs::gui {
namespace {

constexpr wchar_t kPasswordDialogClass[] = L"PoolFS.PasswordDialog.1";
constexpr int kFirstPassword = 1001;
constexpr int kSecondPassword = 1002;
constexpr std::size_t kMaximumPasswordUnits = 1024;
constexpr std::size_t kMaximumPasswordBytes = 4096;

struct WipedWide {
    std::array<wchar_t, kMaximumPasswordUnits + 1> data{};
    ~WipedWide() { SecureZeroMemory(data.data(), data.size() * sizeof(wchar_t)); }
};

struct PasswordDialogState {
    bool confirm = false;
    bool done = false;
    HWND first = nullptr;
    HWND second = nullptr;
    std::optional<password::Secret> result;
};

int scaled(HWND parent, int pixels) {
    const UINT dpi = parent ? GetDpiForWindow(parent) : 96;
    return MulDiv(pixels, dpi ? static_cast<int>(dpi) : 96, 96);
}

void use_gui_font(HWND control) {
    SendMessageW(control, WM_SETFONT,
        reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
}

HWND make_control(HWND parent, const wchar_t* klass, const wchar_t* text,
                  DWORD style, int x, int y, int width, int height, int id) {
    HWND control = CreateWindowExW(0, klass, text, WS_CHILD | WS_VISIBLE | style,
        x, y, width, height, parent, reinterpret_cast<HMENU>(
            static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    if (control) use_gui_font(control);
    return control;
}

password::Secret from_edit(HWND edit) {
    const int count = GetWindowTextLengthW(edit);
    if (count <= 0 || count > static_cast<int>(kMaximumPasswordUnits))
        throw std::runtime_error("Password length must be 1 to 1024 characters.");
    WipedWide wide;
    if (GetWindowTextW(edit, wide.data.data(),
            static_cast<int>(wide.data.size())) != count)
        throw std::runtime_error("Could not read the password field.");
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        wide.data.data(), count, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0 || bytes > static_cast<int>(kMaximumPasswordBytes))
        throw std::runtime_error("Password contains invalid Unicode or is too long.");
    std::string utf8(static_cast<std::size_t>(bytes), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            wide.data.data(), count, utf8.data(), bytes, nullptr, nullptr) != bytes) {
        SecureZeroMemory(utf8.data(), utf8.size());
        throw std::runtime_error("Could not convert the password to UTF-8.");
    }
    return password::Secret(std::move(utf8));
}

void clear_password_controls(PasswordDialogState& state) {
    if (state.first) {
        SetWindowTextW(state.first, L"");
        SendMessageW(state.first, EM_EMPTYUNDOBUFFER, 0, 0);
    }
    if (state.second) {
        SetWindowTextW(state.second, L"");
        SendMessageW(state.second, EM_EMPTYUNDOBUFFER, 0, 0);
    }
}

void accept_password(HWND window, PasswordDialogState& state) {
    try {
        auto first = from_edit(state.first);
        if (state.confirm) {
            auto second = from_edit(state.second);
            const auto a = first.view();
            const auto b = second.view();
            unsigned difference = static_cast<unsigned>(a.size() ^ b.size());
            const std::size_t length = a.size() > b.size() ? a.size() : b.size();
            for (std::size_t i = 0; i < length; ++i) {
                const unsigned left = i < a.size() ?
                    static_cast<unsigned char>(a[i]) : 0;
                const unsigned right = i < b.size() ?
                    static_cast<unsigned char>(b[i]) : 0;
                difference |= left ^ right;
            }
            if (difference)
                throw std::runtime_error("PoolFS passwords did not match.");
            if (a.size() < 12)
                throw std::runtime_error("Use at least 12 UTF-8 bytes for a new pool password.");
        }
        state.result.emplace(std::move(first));
        clear_password_controls(state);
        state.done = true;
        DestroyWindow(window);
    } catch (const std::exception& error) {
        clear_password_controls(state);
        MessageBoxA(window, error.what(), "PoolFS password", MB_OK | MB_ICONERROR);
        SetFocus(state.first);
    }
}

LRESULT CALLBACK password_window_proc(HWND window, UINT message,
                                       WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<PasswordDialogState*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        state = static_cast<PasswordDialogState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(state));
        return TRUE;
    }
    if (!state) return DefWindowProcW(window, message, wparam, lparam);
    switch (message) {
    case WM_CREATE: {
        const int x = scaled(window, 18);
        const int width = scaled(window, 390);
        const int label_height = scaled(window, 20);
        const int edit_height = scaled(window, 28);
        make_control(window, L"STATIC",
            state->confirm ? L"Enter a new password for this pool:" :
                             L"Enter the PoolFS password:",
            0, x, scaled(window, 17), width, label_height, 0);
        state->first = make_control(window, L"EDIT", L"",
            WS_BORDER | WS_TABSTOP | ES_PASSWORD | ES_AUTOHSCROLL,
            x, scaled(window, 43), width, edit_height, kFirstPassword);
        if (state->first)
            SendMessageW(state->first, EM_LIMITTEXT, kMaximumPasswordUnits, 0);
        int button_y = scaled(window, 91);
        if (state->confirm) {
            make_control(window, L"STATIC", L"Confirm password:",
                0, x, scaled(window, 81), width, label_height, 0);
            state->second = make_control(window, L"EDIT", L"",
                WS_BORDER | WS_TABSTOP | ES_PASSWORD | ES_AUTOHSCROLL,
                x, scaled(window, 107), width, edit_height, kSecondPassword);
            if (state->second)
                SendMessageW(state->second, EM_LIMITTEXT, kMaximumPasswordUnits, 0);
            button_y = scaled(window, 151);
        }
        make_control(window, L"BUTTON", L"Cancel", WS_TABSTOP | BS_PUSHBUTTON,
            scaled(window, 216), button_y, scaled(window, 90),
            scaled(window, 29), IDCANCEL);
        make_control(window, L"BUTTON", state->confirm ? L"Create" : L"Unlock",
            WS_TABSTOP | BS_DEFPUSHBUTTON,
            scaled(window, 314), button_y, scaled(window, 94),
            scaled(window, 29), IDOK);
        if (!state->first) return -1;
        SetFocus(state->first);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wparam) == IDOK) {
            accept_password(window, *state);
            return 0;
        }
        if (LOWORD(wparam) == IDCANCEL) {
            clear_password_controls(*state);
            state->done = true;
            DestroyWindow(window);
            return 0;
        }
        break;
    case WM_CLOSE:
        clear_password_controls(*state);
        state->done = true;
        DestroyWindow(window);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

std::optional<std::filesystem::path> choose_file(HWND parent, bool save,
                                                 const wchar_t* title,
                                                 const wchar_t* filter,
                                                 const wchar_t* extension) {
    std::array<wchar_t, 32768> buffer{};
    OPENFILENAMEW options{};
    options.lStructSize = sizeof(options);
    options.hwndOwner = parent;
    options.lpstrFilter = filter;
    options.lpstrFile = buffer.data();
    options.nMaxFile = static_cast<DWORD>(buffer.size());
    options.lpstrTitle = title;
    options.lpstrDefExt = extension;
    options.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST |
        (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    const BOOL selected = save ? GetSaveFileNameW(&options) :
                                 GetOpenFileNameW(&options);
    if (!selected) return std::nullopt;
    return std::filesystem::path(buffer.data());
}

void register_dialog_class(const wchar_t* name, WNDPROC proc) {
    WNDCLASSEXW klass{};
    klass.cbSize = sizeof(klass);
    klass.lpfnWndProc = proc;
    klass.hInstance = GetModuleHandleW(nullptr);
    klass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    klass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    klass.lpszClassName = name;
    if (!RegisterClassExW(&klass) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        throw std::runtime_error("Could not register a PoolFS dialog.");
}

void center_on_owner(HWND parent, int width, int height, int& x, int& y) {
    RECT owner{};
    if (!parent || !GetWindowRect(parent, &owner)) {
        owner.left = 0;
        owner.top = 0;
        owner.right = GetSystemMetrics(SM_CXSCREEN);
        owner.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    x = owner.left + ((owner.right - owner.left) - width) / 2;
    y = owner.top + ((owner.bottom - owner.top) - height) / 2;
}

void run_modal(HWND parent, HWND window, bool& done) {
    const bool parent_was_enabled = parent && IsWindowEnabled(parent);
    if (parent_was_enabled) EnableWindow(parent, FALSE);
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    SetForegroundWindow(window);
    MSG message{};
    while (!done) {
        const BOOL received = GetMessageW(&message, nullptr, 0, 0);
        if (received <= 0) {
            if (received == 0) PostQuitMessage(static_cast<int>(message.wParam));
            break;
        }
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (IsWindow(window)) DestroyWindow(window);
    if (parent_was_enabled) {
        EnableWindow(parent, TRUE);
        SetForegroundWindow(parent);
    }
}

constexpr wchar_t kPartitionOptionsClass[] = L"PoolFS.PartitionOptions.1";
constexpr wchar_t kPartitionConfirmClass[] = L"PoolFS.PartitionConfirm.1";
constexpr int kUseMaximum = 2101;
constexpr int kSizeMiB = 2102;
constexpr int kFileSystem = 2103;
constexpr int kVolumeLabel = 2104;
constexpr int kDriveLetter = 2105;
constexpr int kTypedConfirmation = 2201;

struct PartitionOptionsState {
    bool creating = false;
    bool change_letter = false;
    bool done = false;
    HWND use_maximum = nullptr;
    HWND size = nullptr;
    HWND filesystem = nullptr;
    HWND label = nullptr;
    HWND letter = nullptr;
    std::optional<PartitionOptions> result;
};

void fill_letters(HWND combo) {
    const DWORD occupied = GetLogicalDrives();
    int preferred = -1;
    for (wchar_t letter = L'D'; letter <= L'Z'; ++letter) {
        const wchar_t text[] = {letter, L':', L'\0'};
        const LRESULT index = SendMessageW(combo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(text));
        SendMessageW(combo, CB_SETITEMDATA, static_cast<WPARAM>(index), letter);
        if (preferred < 0 && !(occupied & (1U << (letter - L'A'))))
            preferred = static_cast<int>(index);
    }
    SendMessageW(combo, CB_SETCURSEL,
        static_cast<WPARAM>(preferred < 0 ? 0 : preferred), 0);
}

std::wstring control_text(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(control, text.data(), length + 1);
    text.resize(static_cast<std::size_t>(copied));
    return text;
}

bool get_partition_options(HWND window, PartitionOptionsState& state,
                           PartitionOptions& options) {
    if (state.creating) {
        options.use_max = SendMessageW(state.use_maximum, BM_GETCHECK, 0, 0) ==
            BST_CHECKED;
        if (!options.use_max) {
            const auto text = control_text(state.size);
            if (text.empty() || !std::all_of(text.begin(), text.end(),
                    [](wchar_t c) { return c >= L'0' && c <= L'9'; })) {
                MessageBoxW(window, L"Enter a positive partition size in MiB.",
                    L"Partition size", MB_OK | MB_ICONERROR);
                SetFocus(state.size);
                return false;
            }
            wchar_t* end = nullptr;
            errno = 0;
            const auto parsed = std::wcstoull(text.c_str(), &end, 10);
            if (errno || !end || *end || parsed == 0 ||
                parsed > static_cast<unsigned long long>(
                    std::numeric_limits<std::int64_t>::max() / (1024 * 1024))) {
                MessageBoxW(window, L"Partition size is outside the supported range.",
                    L"Partition size", MB_OK | MB_ICONERROR);
                SetFocus(state.size);
                return false;
            }
            options.size_mib = parsed;
        }
    }
    if (state.creating || state.change_letter) {
        const LRESULT index = SendMessageW(state.letter, CB_GETCURSEL, 0, 0);
        if (index == CB_ERR) {
            MessageBoxW(window, L"Choose a drive-letter option.",
                L"Drive letter", MB_OK | MB_ICONERROR);
            return false;
        }
        options.drive_letter = static_cast<wchar_t>(SendMessageW(
            state.letter, CB_GETITEMDATA, static_cast<WPARAM>(index), 0));
    }
    if (!state.change_letter) {
        const LRESULT index = SendMessageW(state.filesystem, CB_GETCURSEL, 0, 0);
        if (index == CB_ERR) {
            MessageBoxW(window, L"Choose a filesystem.",
                L"Filesystem", MB_OK | MB_ICONERROR);
            return false;
        }
        options.filesystem = control_text(state.filesystem);
        options.label = control_text(state.label);
    }
    return true;
}

LRESULT CALLBACK partition_options_proc(HWND window, UINT message,
                                         WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<PartitionOptionsState*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        state = static_cast<PartitionOptionsState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(state));
        return TRUE;
    }
    if (!state) return DefWindowProcW(window, message, wparam, lparam);
    switch (message) {
    case WM_CREATE: {
        const int left = scaled(window, 18);
        const int field_x = scaled(window, 125);
        const int field_width = scaled(window, 325);
        if (state->creating) {
            state->use_maximum = make_control(window, L"BUTTON",
                L"Use all available space", BS_AUTOCHECKBOX | WS_TABSTOP,
                left, scaled(window, 16), scaled(window, 270),
                scaled(window, 24), kUseMaximum);
            SendMessageW(state->use_maximum, BM_SETCHECK, BST_CHECKED, 0);
            make_control(window, L"STATIC", L"Size (MiB):", 0,
                left, scaled(window, 53), scaled(window, 100),
                scaled(window, 20), 0);
            state->size = make_control(window, L"EDIT", L"512",
                WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP,
                field_x, scaled(window, 48), scaled(window, 115),
                scaled(window, 27), kSizeMiB);
            EnableWindow(state->size, FALSE);
            SendMessageW(state->size, EM_LIMITTEXT, 19, 0);
        }
        if (!state->change_letter) {
            const int filesystem_y = state->creating ? 83 : 26;
            const int label_y = state->creating ? 122 : 68;
            make_control(window, L"STATIC", L"Filesystem:", 0,
                left, scaled(window, filesystem_y + 5),
                scaled(window, 100), scaled(window, 20), 0);
            state->filesystem = make_control(window, L"COMBOBOX", L"",
                CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                field_x, scaled(window, filesystem_y), field_width,
                scaled(window, 150), kFileSystem);
            for (const wchar_t* name : {L"NTFS", L"exFAT", L"FAT32"})
                SendMessageW(state->filesystem, CB_ADDSTRING, 0,
                    reinterpret_cast<LPARAM>(name));
            SendMessageW(state->filesystem, CB_SETCURSEL, 0, 0);
            make_control(window, L"STATIC", L"Volume label:", 0,
                left, scaled(window, label_y + 5),
                scaled(window, 100), scaled(window, 20), 0);
            state->label = make_control(window, L"EDIT", L"",
                WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP,
                field_x, scaled(window, label_y), field_width,
                scaled(window, 27), kVolumeLabel);
            SendMessageW(state->label, EM_LIMITTEXT, 32, 0);
        }
        if (state->creating || state->change_letter) {
            const int letter_y = state->creating ? 161 : 29;
            make_control(window, L"STATIC", L"Drive letter:", 0,
                left, scaled(window, letter_y + 5),
                scaled(window, 100), scaled(window, 20), 0);
            state->letter = make_control(window, L"COMBOBOX", L"",
                CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                field_x, scaled(window, letter_y), field_width,
                scaled(window, 280), kDriveLetter);
            fill_letters(state->letter);
        }
        const int buttons_y = state->creating ? 215 :
            (state->change_letter ? 87 : 128);
        make_control(window, L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP,
            scaled(window, 260), scaled(window, buttons_y),
            scaled(window, 90), scaled(window, 30), IDCANCEL);
        make_control(window, L"BUTTON", L"Continue", BS_DEFPUSHBUTTON | WS_TABSTOP,
            scaled(window, 358), scaled(window, buttons_y),
            scaled(window, 92), scaled(window, 30), IDOK);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wparam) == kUseMaximum && HIWORD(wparam) == BN_CLICKED) {
            EnableWindow(state->size,
                SendMessageW(state->use_maximum, BM_GETCHECK, 0, 0) != BST_CHECKED);
            return 0;
        }
        if (LOWORD(wparam) == IDOK) {
            PartitionOptions options;
            if (get_partition_options(window, *state, options)) {
                state->result.emplace(std::move(options));
                state->done = true;
                DestroyWindow(window);
            }
            return 0;
        }
        if (LOWORD(wparam) == IDCANCEL) {
            state->done = true;
            DestroyWindow(window);
            return 0;
        }
        break;
    case WM_CLOSE:
        state->done = true;
        DestroyWindow(window);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

struct PartitionConfirmState {
    std::wstring summary;
    std::wstring phrase;
    bool done = false;
    bool accepted = false;
    HWND typed = nullptr;
    HWND apply = nullptr;
};

bool phrase_matches(const PartitionConfirmState& state) {
    return state.typed && control_text(state.typed) == state.phrase;
}

LRESULT CALLBACK partition_confirm_proc(HWND window, UINT message,
                                         WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<PartitionConfirmState*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        state = static_cast<PartitionConfirmState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(state));
        return TRUE;
    }
    if (!state) return DefWindowProcW(window, message, wparam, lparam);
    switch (message) {
    case WM_CREATE: {
        make_control(window, L"STATIC", L"Review the exact target and operation:",
            0, scaled(window, 18), scaled(window, 16),
            scaled(window, 580), scaled(window, 22), 0);
        make_control(window, L"EDIT", state->summary.c_str(),
            WS_BORDER | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL |
                WS_VSCROLL,
            scaled(window, 18), scaled(window, 44),
            scaled(window, 580), scaled(window, 126), 0);
        make_control(window, L"STATIC", L"Type this exact phrase to apply the change:",
            0, scaled(window, 18), scaled(window, 184),
            scaled(window, 580), scaled(window, 20), 0);
        make_control(window, L"EDIT", state->phrase.c_str(),
            WS_BORDER | ES_READONLY | ES_AUTOHSCROLL,
            scaled(window, 18), scaled(window, 207),
            scaled(window, 580), scaled(window, 27), 0);
        state->typed = make_control(window, L"EDIT", L"",
            WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP,
            scaled(window, 18), scaled(window, 245),
            scaled(window, 580), scaled(window, 28), kTypedConfirmation);
        SendMessageW(state->typed, EM_LIMITTEXT, state->phrase.size(), 0);
        make_control(window, L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP,
            scaled(window, 405), scaled(window, 285),
            scaled(window, 92), scaled(window, 30), IDCANCEL);
        state->apply = make_control(window, L"BUTTON", L"Apply change",
            BS_DEFPUSHBUTTON | WS_TABSTOP,
            scaled(window, 505), scaled(window, 285),
            scaled(window, 93), scaled(window, 30), IDOK);
        EnableWindow(state->apply, FALSE);
        SetFocus(state->typed);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wparam) == kTypedConfirmation && HIWORD(wparam) == EN_CHANGE) {
            EnableWindow(state->apply, phrase_matches(*state));
            return 0;
        }
        if (LOWORD(wparam) == IDOK) {
            if (phrase_matches(*state)) {
                state->accepted = true;
                state->done = true;
                DestroyWindow(window);
            }
            return 0;
        }
        if (LOWORD(wparam) == IDCANCEL) {
            state->done = true;
            DestroyWindow(window);
            return 0;
        }
        break;
    case WM_CLOSE:
        state->done = true;
        DestroyWindow(window);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace

std::optional<password::Secret> prompt_password(HWND parent, bool confirm) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW klass{};
        klass.cbSize = sizeof(klass);
        klass.lpfnWndProc = password_window_proc;
        klass.hInstance = GetModuleHandleW(nullptr);
        klass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        klass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        klass.lpszClassName = kPasswordDialogClass;
        if (!RegisterClassExW(&klass) &&
            GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            throw std::runtime_error("Could not register the password dialog.");
        registered = true;
    }
    PasswordDialogState state;
    state.confirm = confirm;
    const int width = scaled(parent, 428);
    const int height = scaled(parent, confirm ? 224 : 164);
    RECT owner{};
    if (!parent || !GetWindowRect(parent, &owner)) {
        owner.left = 0;
        owner.top = 0;
        owner.right = GetSystemMetrics(SM_CXSCREEN);
        owner.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    const int x = owner.left + ((owner.right - owner.left) - width) / 2;
    const int y = owner.top + ((owner.bottom - owner.top) - height) / 2;
    HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME, kPasswordDialogClass,
        confirm ? L"Create encrypted PoolFS pool" : L"Unlock PoolFS pool",
        WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, width, height,
        parent, nullptr, GetModuleHandleW(nullptr), &state);
    if (!window) throw std::runtime_error("Could not open the password dialog.");
    const bool parent_was_enabled = parent && IsWindowEnabled(parent);
    if (parent_was_enabled) EnableWindow(parent, FALSE);
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    SetForegroundWindow(window);
    SetFocus(state.first);
    MSG message{};
    while (!state.done) {
        const BOOL received = GetMessageW(&message, nullptr, 0, 0);
        if (received <= 0) {
            if (received == 0) PostQuitMessage(static_cast<int>(message.wParam));
            break;
        }
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (IsWindow(window)) {
        clear_password_controls(state);
        DestroyWindow(window);
    }
    if (parent_was_enabled) {
        EnableWindow(parent, TRUE);
        SetForegroundWindow(parent);
    }
    return std::move(state.result);
}

std::optional<std::filesystem::path> pick_open_config(HWND parent) {
    return choose_file(parent, false, L"Open PoolFS mount config",
        L"JSON files (*.json)\0*.json\0All files (*.*)\0*.*\0", L"json");
}

std::optional<std::filesystem::path> pick_save_config(HWND parent) {
    return choose_file(parent, true, L"Save PoolFS mount config",
        L"JSON files (*.json)\0*.json\0All files (*.*)\0*.*\0", L"json");
}

std::optional<std::filesystem::path> pick_existing_image(HWND parent) {
    return choose_file(parent, false, L"Select existing PoolFS image",
        L"PoolFS images (*.img)\0*.img\0All files (*.*)\0*.*\0", L"img");
}

std::optional<std::filesystem::path> pick_new_image_path(HWND parent) {
    return choose_file(parent, true, L"Choose new PoolFS image path",
        L"PoolFS images (*.img)\0*.img\0All files (*.*)\0*.*\0", L"img");
}

std::optional<std::filesystem::path> pick_folder(HWND parent) {
    BROWSEINFOW browse{};
    browse.hwndOwner = parent;
    browse.lpszTitle = L"Choose a folder containing PoolFS images";
    browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE selected = SHBrowseForFolderW(&browse);
    if (!selected) return std::nullopt;
    std::array<wchar_t, MAX_PATH> path{};
    const BOOL resolved = SHGetPathFromIDListW(selected, path.data());
    CoTaskMemFree(selected);
    if (!resolved) return std::nullopt;
    return std::filesystem::path(path.data());
}

std::optional<PartitionOptions> prompt_partition_options(
        HWND parent, bool creating, bool change_letter) {
    if (creating && change_letter)
        throw std::invalid_argument("partition options mode is ambiguous");
    register_dialog_class(kPartitionOptionsClass, partition_options_proc);
    PartitionOptionsState state;
    state.creating = creating;
    state.change_letter = change_letter;
    const int width = scaled(parent, 470);
    const int height = scaled(parent,
        creating ? 305 : (change_letter ? 175 : 215));
    int x = 0, y = 0;
    center_on_owner(parent, width, height, x, y);
    const wchar_t* title = creating ? L"Create partition options" :
        (change_letter ? L"Change drive letter" : L"Format partition options");
    HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME,
        kPartitionOptionsClass, title, WS_POPUP | WS_CAPTION | WS_SYSMENU,
        x, y, width, height, parent, nullptr, GetModuleHandleW(nullptr), &state);
    if (!window) throw std::runtime_error("Could not open partition options.");
    run_modal(parent, window, state.done);
    return std::move(state.result);
}

bool confirm_partition_action(HWND parent, std::wstring_view summary,
                              std::wstring_view required_phrase) {
    if (summary.empty() || required_phrase.empty() || required_phrase.size() > 128 ||
        std::any_of(required_phrase.begin(), required_phrase.end(),
            [](wchar_t c) { return c < L' ' || c == 0x7f; }))
        throw std::invalid_argument("partition confirmation needs a visible target and a short phrase");
    register_dialog_class(kPartitionConfirmClass, partition_confirm_proc);
    PartitionConfirmState state;
    state.summary = summary;
    state.phrase = required_phrase;
    const int width = scaled(parent, 620);
    const int height = scaled(parent, 360);
    int x = 0, y = 0;
    center_on_owner(parent, width, height, x, y);
    HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME,
        kPartitionConfirmClass, L"Confirm storage change",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        x, y, width, height, parent, nullptr, GetModuleHandleW(nullptr), &state);
    if (!window) throw std::runtime_error("Could not open storage confirmation.");
    run_modal(parent, window, state.done);
    return state.accepted;
}

} // namespace poolfs::gui
