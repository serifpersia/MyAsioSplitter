#include "config.h"

#include <objbase.h>

#include <cwchar>

namespace {

constexpr wchar_t kIniSection[] = L"MyAsioSplitter";
constexpr wchar_t kIniKey[] = L"CLSID";
constexpr wchar_t kPickerClass[] = L"MyAsioSplitterPicker";
constexpr int kComboId = 1000;

std::wstring Trim(std::wstring text) {
    const size_t first = text.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return {};
    const size_t last = text.find_last_not_of(L" \t\r\n");
    return text.substr(first, last - first + 1);
}

bool ParseClsidString(const std::wstring& text, CLSID& out) {
    std::wstring value = Trim(text);
    if (value.empty()) return false;
    if (value.front() != L'{') value = L"{" + value + L"}";
    return CLSIDFromString(value.c_str(), &out) == S_OK;
}

std::wstring ReadRegString(HKEY key, const wchar_t* name) {
    wchar_t buffer[512] = {};
    DWORD bytes = sizeof(buffer) - sizeof(wchar_t);
    DWORD type = 0;
    const LSTATUS status =
        RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(buffer), &bytes);
    if (status != ERROR_SUCCESS) return {};
    if (type != REG_SZ && type != REG_EXPAND_SZ) return {};

    size_t chars = bytes / sizeof(wchar_t);
    if (chars >= _countof(buffer)) chars = _countof(buffer) - 1;
    buffer[chars] = L'\0';
    return Trim(buffer);
}

std::wstring ConfigFilePath() {
    wchar_t appData[MAX_PATH] = {};
    const DWORD len = GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return {};

    const std::wstring dir = std::wstring(appData) + L"\\MyAsioSplitter";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\config.ini";
}

struct PickerState {
    const std::vector<AsioDriverInfo>* drivers = nullptr;
    HWND combo = nullptr;
    int selected = -1;
    bool done = false;
};

LRESULT CALLBACK PickerProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<PickerState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (message) {
        case WM_CREATE: {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            state = static_cast<PickerState*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));

            HINSTANCE instance = GetModuleHandleW(nullptr);
            HWND label = CreateWindowExW(0, L"STATIC", L"Select hardware ASIO device:", WS_CHILD | WS_VISIBLE,
                                         12, 12, 340, 20, hwnd, nullptr, instance, nullptr);
            state->combo = CreateWindowExW(
                0, L"COMBOBOX", nullptr,
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, 12, 34, 340, 220, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kComboId)), instance, nullptr);
            HWND ok = CreateWindowExW(0, L"BUTTON", L"OK",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 202, 76, 70, 26,
                                      hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)), instance, nullptr);
            HWND cancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                          282, 76, 70, 26, hwnd,
                                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)), instance,
                                          nullptr);

            auto font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            for (HWND control : {label, state->combo, ok, cancel}) {
                SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            }
            for (const AsioDriverInfo& driver : *state->drivers) {
                SendMessageW(state->combo, CB_ADDSTRING, 0,
                             reinterpret_cast<LPARAM>(driver.name.c_str()));
            }
            SendMessageW(state->combo, CB_SETCURSEL, 0, 0);
            SetFocus(state->combo);
            return 0;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            if (id == IDOK && HIWORD(wParam) == BN_CLICKED) {
                if (state) {
                    state->selected =
                        static_cast<int>(SendMessageW(state->combo, CB_GETCURSEL, 0, 0));
                    state->done = true;
                }
                DestroyWindow(hwnd);
                return 0;
            }
            if (id == IDCANCEL) {
                if (state) {
                    state->selected = -1;
                    state->done = true;
                }
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        }
        case WM_CLOSE:
            if (state) {
                state->selected = -1;
                state->done = true;
            }
            DestroyWindow(hwnd);
            return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // namespace

std::vector<AsioDriverInfo> EnumerateAsioDrivers(REFCLSID exclude1, REFCLSID exclude2) {
    std::vector<AsioDriverInfo> drivers;

    HKEY asio = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ, &asio) != ERROR_SUCCESS) {
        return drivers;
    }

    for (DWORD index = 0;; ++index) {
        wchar_t subkey[256] = {};
        DWORD nameLength = _countof(subkey);
        const LSTATUS status =
            RegEnumKeyExW(asio, index, subkey, &nameLength, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status != ERROR_SUCCESS) break;

        HKEY driverKey = nullptr;
        if (RegOpenKeyExW(asio, subkey, 0, KEY_READ, &driverKey) != ERROR_SUCCESS) continue;

        CLSID clsid = {};
        const bool validClsid = ParseClsidString(ReadRegString(driverKey, L"CLSID"), clsid);
        std::wstring name = ReadRegString(driverKey, L"Description");
        RegCloseKey(driverKey);

        if (!validClsid) continue;
        if (IsEqualGUID(clsid, exclude1) || IsEqualGUID(clsid, exclude2)) continue;
        if (name.empty()) name = subkey;

        drivers.push_back(AsioDriverInfo{name, clsid});
    }

    RegCloseKey(asio);
    return drivers;
}

bool LoadSelectedClsid(CLSID& out) {
    const std::wstring path = ConfigFilePath();
    if (path.empty()) return false;

    wchar_t text[128] = {};
    GetPrivateProfileStringW(kIniSection, kIniKey, L"", text, _countof(text), path.c_str());
    return ParseClsidString(text, out);
}

bool SaveSelectedClsid(REFCLSID clsid) {
    const std::wstring path = ConfigFilePath();
    if (path.empty()) return false;

    wchar_t text[64] = {};
    if (StringFromGUID2(clsid, text, _countof(text)) <= 0) return false;
    return WritePrivateProfileStringW(kIniSection, kIniKey, text, path.c_str()) != FALSE;
}

int ShowAsioDevicePicker(const std::vector<AsioDriverInfo>& drivers, const CLSID* preselect) {
    if (drivers.empty()) return -1;

    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = PickerProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    windowClass.lpszClassName = kPickerClass;
    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return -1;

    PickerState state;
    state.drivers = &drivers;

    RECT rect = {0, 0, 364, 114};
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRectEx(&rect, style, FALSE, WS_EX_CONTROLPARENT);
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    const int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    const int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;

    HWND owner = GetActiveWindow();
    HWND hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, kPickerClass,
                                L"MyASIO Splitter - Select ASIO Device", style, x, y, width, height,
                                owner, nullptr, windowClass.hInstance, &state);
    if (!hwnd) return -1;

    if (owner && owner != hwnd) EnableWindow(owner, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);

    if (preselect && state.combo) {
        for (size_t i = 0; i < drivers.size(); ++i) {
            if (IsEqualGUID(drivers[i].clsid, *preselect)) {
                SendMessageW(state.combo, CB_SETCURSEL, static_cast<WPARAM>(i), 0);
                break;
            }
        }
    }

    MSG message = {};
    while (!state.done) {
        const BOOL result = GetMessageW(&message, nullptr, 0, 0);
        if (result == 0) {
            PostQuitMessage(static_cast<int>(message.wParam));
            break;
        }
        if (result == -1) break;

        if (!IsDialogMessageW(hwnd, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (!IsWindow(hwnd)) state.done = true;
    }

    if (IsWindow(hwnd)) DestroyWindow(hwnd);
    if (owner && IsWindow(owner)) {
        EnableWindow(owner, TRUE);
        SetActiveWindow(owner);
    }
    return state.done ? state.selected : -1;
}
