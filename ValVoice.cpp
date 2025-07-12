#include "framework.h"

#include "Resource.h"

#include <sapi.h>
#include <sphelper.h>
#include <commctrl.h>
#include <VersionHelpers.h>  // ✅ Modern version check
#include <windowsx.h>
#include <vector>
#include <string>
#include <fstream>
#include <codecvt>
#include <locale>
#include <shellapi.h>  // Include the header for ShellExecuteW
#include <dwmapi.h>

#pragma comment(lib, "sapi.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "Shell32.lib")  // Link against Shell32.lib
#pragma comment(lib, "dwmapi.lib")

#define MAX_LOADSTRING 100

// Global Variables
HINSTANCE hInst;
ISpVoice* pVoice = nullptr;
WCHAR szTitle[MAX_LOADSTRING];
WCHAR szWindowClass[MAX_LOADSTRING];

const int g_dailyQuota = 20; // Example: 20 messages per day
int g_messagesToday = 0; // Track the number of messages spoken today
int g_charsToday = 0; // Track the number of characters spoken today
SYSTEMTIME g_lastStatReset = {}; // Track the last reset time
WCHAR g_userId[64] = L"";
bool g_isPremium = false;
UINT g_pttKey = 'V'; // Default to 'V'
bool g_waitingForPTT = false;
std::vector<std::wstring> g_blockedIds;

// Globals for tab dialogs
HWND g_hTabDialogs[3] = { nullptr, nullptr, nullptr };
const int g_tabDialogIds[3] = { IDD_TAB_MAIN, IDD_TAB_INFO, IDD_TAB_SETTINGS };

HFONT g_hSegoeUIFont = nullptr;

// Forward Declarations
ATOM MyRegisterClass(HINSTANCE hInstance);
BOOL InitInstance(HINSTANCE, int);
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK About(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK LoginDlgProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK TabDialogProc(HWND, UINT, WPARAM, LPARAM);

// Add this function near the top, after includes
void PopulateVoices(HWND hCombo) {
    if (!hCombo) return;
    SendMessage(hCombo, CB_RESETCONTENT, 0, 0);
    CComPtr<IEnumSpObjectTokens> cpEnum;
    if (SUCCEEDED(SpEnumTokens(SPCAT_VOICES, NULL, NULL, &cpEnum)) && cpEnum) {
        CComPtr<ISpObjectToken> cpToken;
        ULONG fetched = 0;
        while (cpEnum->Next(1, &cpToken, &fetched) == S_OK) {
            LPWSTR desc = nullptr;
            if (SUCCEEDED(SpGetDescription(cpToken, &desc))) {
                SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)desc);
                CoTaskMemFree(desc);
            }
            cpToken.Release();
        }
    }
}

// Helper: Show only the selected tab dialog
void ShowTabDialog(HWND hParent, int tabIndex) {
    for (int i = 0; i < 3; ++i) {
        if (g_hTabDialogs[i]) {
            ShowWindow(g_hTabDialogs[i], (i == tabIndex) ? SW_SHOW : SW_HIDE);
        }
    }
}

struct AgentProfile {
    LPCWSTR name;
    int rate;
};

AgentProfile agents[] = {
    { L"JetVoice",  4 },
    { L"SovaVoice", 0 },
    { L"BrimGuy",  -2 },
};

void ResetStatsIfNeeded(HWND hWnd) {
    SYSTEMTIME now;
    GetLocalTime(&now);
    if (now.wDay != g_lastStatReset.wDay || now.wMonth != g_lastStatReset.wMonth || now.wYear != g_lastStatReset.wYear) {
        g_messagesToday = 0;
        g_charsToday = 0;
        g_lastStatReset = now;
        if (hWnd) {
            HWND hQuotaBar = GetDlgItem(hWnd, IDC_QUOTA_BAR);
            SendMessage(hQuotaBar, PBM_SETPOS, g_dailyQuota, 0);
            SetDlgItemInt(hWnd, IDC_QUOTA_VALUE, g_dailyQuota, FALSE);
            SetDlgItemInt(hWnd, IDC_STATS_MSGS, 0, FALSE);
            SetDlgItemInt(hWnd, IDC_STATS_CHARS, 0, FALSE);
        }
    }
}

void ExportSettingsToFile() {
    std::wofstream ofs(L"ValVoiceSettings.txt");
    if (!ofs) {
        MessageBoxW(NULL, L"Failed to write settings file.", L"Error", MB_ICONERROR);
        return;
    }
    ofs.imbue(std::locale(std::locale::empty(), new std::codecvt_utf8<wchar_t>));
    ofs << L"UserID=" << g_userId << std::endl;
    ofs << L"Premium=" << (g_isPremium ? L"1" : L"0") << std::endl;
    // Remove these lines if you remove the variables:
    // ofs << L"VoiceSource=" << g_voiceSource << std::endl;
    // ofs << L"MicStreaming=" << (g_micStreaming ? L"1" : L"0") << std::endl;
    ofs << L"PTTKey=" << (wchar_t)g_pttKey << std::endl;
    // Add more settings as needed (rate, volume, etc.)
    if (ofs.is_open()) ofs.close();
}

void SaveBlockedIds() {
    std::wofstream ofs(L"BlockedIds.txt");
    if (!ofs) return;
    for (const auto& id : g_blockedIds) ofs << id << std::endl;
}

void LoadBlockedIds() {
    g_blockedIds.clear();
    std::wifstream ifs(L"BlockedIds.txt");
    if (!ifs) return;
    std::wstring id;
    while (std::getline(ifs, id)) {
        if (!id.empty()) g_blockedIds.push_back(id);
    }
}

void SpeakFromUI(HWND hTabWnd) {
    ResetStatsIfNeeded(hTabWnd);

    if (!pVoice) {
        MessageBoxW(hTabWnd, L"Voice engine not available.", L"Error", MB_ICONERROR);
        return;
    }

    // Example: Replace with actual sender Riot ID in your integration
    const wchar_t* senderRiotId = L"SomeRiotID";
    for (const auto& blocked : g_blockedIds) {
        if (wcscmp(senderRiotId, blocked.c_str()) == 0) {
            MessageBoxW(hTabWnd, L"Sender is blocked.", L"Blocked", MB_ICONWARNING);
            return;
        }
    }

    // Enforce quota
    if (g_messagesToday >= g_dailyQuota) {
        MessageBoxW(hTabWnd, L"Daily quota reached.", L"Quota", MB_ICONWARNING);
        return;
    }

    wchar_t text[1024];
    GetDlgItemTextW(hTabWnd, IDC_TEXT_INPUT, text, 1024);
    size_t len = wcslen(text);
    if (len > static_cast<size_t>(INT_MAX)) {
        MessageBoxW(hTabWnd, L"Text length exceeds maximum allowed size.", L"Error", MB_ICONERROR);
        return;
    }
    int intLen = static_cast<int>(len);
    if (intLen == 0) return;

    int uiRate = SendMessage(GetDlgItem(hTabWnd, IDC_RATE_SLIDER), TBM_GETPOS, 0, 0);
    // Map 25–200 to -10 to +10
    int rate = (int)(((uiRate - 25) / 175.0) * 20 - 10 + 0.5);
    int volume = static_cast<int>(SendMessage(GetDlgItem(hTabWnd, IDC_VOLUME_SLIDER), TBM_GETPOS, 0, 0));

    int agentIndex = SendMessage(GetDlgItem(hTabWnd, IDC_AGENT_COMBO), CB_GETCURSEL, 0, 0);
    if (agentIndex >= 0 && agentIndex < _countof(agents)) {
        rate += agents[agentIndex].rate;
    }

    // Set selected voice
    HWND voiceCombo = GetDlgItem(hTabWnd, IDC_VOICE_COMBO);
    int sel = (int)SendMessage(voiceCombo, CB_GETCURSEL, 0, 0);
    if (sel != CB_ERR) {
        CComPtr<IEnumSpObjectTokens> cpEnum;
        if (SUCCEEDED(SpEnumTokens(SPCAT_VOICES, NULL, NULL, &cpEnum)) && cpEnum) {
            CComPtr<ISpObjectToken> cpToken;
            ULONG fetched = 0;
            for (int i = 0; i <= sel; ++i) {
                cpEnum->Next(1, &cpToken, &fetched);
            }
            if (cpToken) {
                pVoice->SetVoice(cpToken);
            }
        }
    }

    if (pVoice) {
        pVoice->SetRate(rate);
        pVoice->SetVolume(volume);
        pVoice->Speak(text, SPF_ASYNC, NULL);

        // Update stats
        g_messagesToday++;
        g_charsToday += intLen;
        SetDlgItemInt(hTabWnd, IDC_STATS_MSGS, g_messagesToday, FALSE);
        SetDlgItemInt(hTabWnd, IDC_STATS_CHARS, g_charsToday, FALSE);

        // Update quota
        HWND hQuotaBar = GetDlgItem(hTabWnd, IDC_QUOTA_BAR);
        SendMessage(hQuotaBar, PBM_SETPOS, g_dailyQuota - g_messagesToday, 0);
        SetDlgItemInt(hTabWnd, IDC_QUOTA_VALUE, g_dailyQuota - g_messagesToday, FALSE);
    }
}

void EnableDarkMode(HWND hwnd) {
    BOOL dark = TRUE;
    // 20 = DWMWA_USE_IMMERSIVE_DARK_MODE before Windows 11, 19 for Windows 11+
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
    DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    LoadStringW(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
    LoadStringW(hInstance, IDC_VALVOICE, szWindowClass, MAX_LOADSTRING);
    MyRegisterClass(hInstance);

    if (!InitInstance(hInstance, nCmdShow))
        return FALSE;

    HACCEL hAccelTable = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_VALVOICE));
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        if (!TranslateAccelerator(msg.hwnd, hAccelTable, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        if (msg.message == WM_KEYDOWN && msg.wParam == g_pttKey) {
            // PTT key pressed: start/enable voice/mic
        }
        if (msg.message == WM_KEYUP && msg.wParam == g_pttKey) {
            // PTT key released: stop/disable voice/mic
        }
    }

    if (pVoice) { pVoice->Release(); pVoice = nullptr; }
    CoUninitialize();

    // At app exit, before return:
    g_blockedIds.clear();

    if (g_hSegoeUIFont) {
        DeleteObject(g_hSegoeUIFont);
        g_hSegoeUIFont = nullptr;
    }

    return (int)msg.wParam;
}

ATOM MyRegisterClass(HINSTANCE hInstance) {
    WNDCLASSEXW wcex = { sizeof(WNDCLASSEX) };
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.hInstance = hInstance;
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszClassName = szWindowClass;
    wcex.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_VALVOICE));
    wcex.lpszMenuName = MAKEINTRESOURCEW(IDC_VALVOICE);
    wcex.hIconSm = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_SMALL));
    return RegisterClassExW(&wcex);
}

BOOL InitInstance(HINSTANCE hInstance, int nCmdShow) {
    INITCOMMONCONTROLSEX icex = { sizeof(icex), ICC_BAR_CLASSES };
    InitCommonControlsEx(&icex);

    HRESULT hr = CoInitialize(NULL);
    if (FAILED(hr)) {
        MessageBoxW(NULL, L"COM initialization failed.", L"Error", MB_ICONERROR);
        return FALSE;
    }
    hr = CoCreateInstance(CLSID_SpVoice, NULL, CLSCTX_ALL, IID_ISpVoice, (void**)&pVoice);
    if (FAILED(hr)) {
        MessageBoxW(NULL, L"Could not initialize SAPI voice.", L"Error", MB_ICONERROR);
        CoUninitialize();
        return FALSE;
    }

    // Prompt for login
    if (DialogBox(hInstance, MAKEINTRESOURCE(IDD_LOGIN), NULL, LoginDlgProc) != IDOK) {
        return FALSE; // Exit if user cancels login
    }

    LoadBlockedIds();

    hInst = hInstance;
    HWND hWnd = CreateDialog(hInstance, MAKEINTRESOURCE(IDD_VALVOICE_DIALOG), NULL, WndProc);
    if (!hWnd) return FALSE;

    // Init sliders
    SendDlgItemMessage(hWnd, IDC_RATE_SLIDER, TBM_SETRANGE, TRUE, MAKELPARAM(25, 200));
    SendDlgItemMessage(hWnd, IDC_RATE_SLIDER, TBM_SETPOS, TRUE, 100); // Default to 100

    SendDlgItemMessage(hWnd, IDC_VOLUME_SLIDER, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendDlgItemMessage(hWnd, IDC_VOLUME_SLIDER, TBM_SETPOS, TRUE, 100);

    // Agent combo
    HWND combo = GetDlgItem(hWnd, IDC_AGENT_COMBO);
    for (auto& agent : agents) {
        SendMessage(combo, CB_ADDSTRING, 0, (LPARAM)agent.name);
    }
    SendMessage(combo, CB_SETCURSEL, 0, 0);

    // Quota bar
    HWND hQuotaBar = GetDlgItem(hWnd, IDC_QUOTA_BAR);
    SendMessage(hQuotaBar, PBM_SETRANGE, 0, MAKELPARAM(0, g_dailyQuota));
    SendMessage(hQuotaBar, PBM_SETPOS, g_dailyQuota - g_messagesToday, 0);
    SetDlgItemInt(hWnd, IDC_QUOTA_VALUE, g_dailyQuota - g_messagesToday, FALSE);

    ResetStatsIfNeeded(hWnd);

    // Center the main window on the screen
    RECT rc;
    GetWindowRect(hWnd, &rc);
    int winWidth = rc.right - rc.left;
    int winHeight = rc.bottom - rc.top;

    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);

    int x = (screenWidth - winWidth) / 2;
    int y = (screenHeight - winHeight) / 2;

    SetWindowPos(hWnd, HWND_TOP, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    EnableDarkMode(hWnd);

    return TRUE;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    static int currentTab = 0;
    switch (message) {
    case WM_INITDIALOG: {
        HWND hTab = GetDlgItem(hWnd, IDC_TAB_MAIN);

        // Add tab items
        TCITEM tie = { 0 };
        tie.mask = TCIF_TEXT;
        tie.pszText = (LPWSTR)L"Main";
        TabCtrl_InsertItem(hTab, 0, &tie);

        tie.pszText = (LPWSTR)L"Info";
        TabCtrl_InsertItem(hTab, 1, &tie);

        tie.pszText = (LPWSTR)L"Settings";
        TabCtrl_InsertItem(hTab, 2, &tie);

        // Create child dialogs for each tab
        RECT rc;
        GetClientRect(hTab, &rc);
        TabCtrl_AdjustRect(hTab, FALSE, &rc);
        MapWindowPoints(hTab, hWnd, (LPPOINT)&rc, 2);

        for (int i = 0; i < 3; ++i) {
            g_hTabDialogs[i] = CreateDialogParam(
                hInst,
                MAKEINTRESOURCE(g_tabDialogIds[i]),
                hWnd,
                TabDialogProc,
                0
            );
            SetWindowPos(g_hTabDialogs[i], HWND_TOP, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, SWP_SHOWWINDOW);
            ShowWindow(g_hTabDialogs[i], (i == 0) ? SW_SHOW : SW_HIDE);
        }

        // --- Initialize controls for each tab after creation ---
        // Main Tab
        HWND hTabMain = g_hTabDialogs[0];
        if (hTabMain) {
            SendDlgItemMessage(hTabMain, IDC_RATE_SLIDER, TBM_SETRANGE, TRUE, MAKELPARAM(25, 200));
            SendDlgItemMessage(hTabMain, IDC_RATE_SLIDER, TBM_SETPOS, TRUE, 100);
            SendDlgItemMessage(hTabMain, IDC_VOLUME_SLIDER, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
            SendDlgItemMessage(hTabMain, IDC_VOLUME_SLIDER, TBM_SETPOS, TRUE, 100);

            HWND combo = GetDlgItem(hTabMain, IDC_AGENT_COMBO);
            for (auto& agent : agents) {
                SendMessage(combo, CB_ADDSTRING, 0, (LPARAM)agent.name);
            }
            SendMessage(combo, CB_SETCURSEL, 0, 0);

            HWND hQuotaBar = GetDlgItem(hTabMain, IDC_QUOTA_BAR);
            SendMessage(hQuotaBar, PBM_SETRANGE, 0, MAKELPARAM(0, g_dailyQuota));
            SendMessage(hQuotaBar, PBM_SETPOS, g_dailyQuota - g_messagesToday, 0);
            SetDlgItemInt(hTabMain, IDC_QUOTA_VALUE, g_dailyQuota - g_messagesToday, FALSE);

            HWND hVoiceCombo = GetDlgItem(hTabMain, IDC_VOICE_COMBO);
            PopulateVoices(hVoiceCombo);
            SendMessage(hVoiceCombo, CB_SETCURSEL, 0, 0);

            // Assuming hTextInput is the HWND of your "Text to Speak" edit control
            HWND hTextInput = GetDlgItem(hTabMain, IDC_TEXT_INPUT);
            SendMessageW(hTextInput, EM_SETCUEBANNER, 0, (LPARAM)L"Type your message here...");

            if (!g_hSegoeUIFont) {
                g_hSegoeUIFont = CreateFontW(
                    -11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                    DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI"
                );
            }

            SendMessageW(GetDlgItem(hTabMain, IDC_TEXT_INPUT), WM_SETFONT, (WPARAM)g_hSegoeUIFont, TRUE);
            SendMessageW(GetDlgItem(hTabMain, IDC_SPEAK_BUTTON), WM_SETFONT, (WPARAM)g_hSegoeUIFont, TRUE);
            SendMessageW(GetDlgItem(hTabMain, IDC_STOP_BUTTON), WM_SETFONT, (WPARAM)g_hSegoeUIFont, TRUE);
            // ...repeat for other controls as needed
        }

        // Info Tab (now also handles settings controls)
        HWND hTabInfo = g_hTabDialogs[1];
        if (hTabInfo) {
            // Set profile picture (replace IDI_USER_ICON with your icon resource)
            HICON hIcon = (HICON)LoadImage(hInst, MAKEINTRESOURCE(IDI_USER_ICON), IMAGE_ICON, 48, 48, LR_DEFAULTCOLOR);
            SendDlgItemMessage(hTabInfo, IDC_PROFILE_PIC, STM_SETICON, (WPARAM)hIcon, 0);

            SetDlgItemTextW(hTabInfo, IDC_INFO_USERID, g_userId);
            WCHAR quotaText[32];
            wsprintf(quotaText, L"%d/%d", g_dailyQuota - g_messagesToday, g_dailyQuota);
            SetDlgItemTextW(hTabInfo, IDC_INFO_QUOTA, quotaText);
            SetDlgItemTextW(hTabInfo, IDC_INFO_PREMIUM, g_isPremium ? L"Yes" : L"No");

            HWND hList = GetDlgItem(hTabInfo, IDC_BLOCK_LIST);
            for (const auto& id : g_blockedIds)
                SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)id.c_str());
        }

        // Settings Tab
        HWND hTabSettings = g_hTabDialogs[2];
        if (hTabSettings) {
            // Initialize "Narrator Source" combo box
            HWND hNarratorSource = GetDlgItem(hTabSettings, IDC_SETTINGS_NARRATOR_SOURCE);
            if (hNarratorSource) {
                SendMessage(hNarratorSource, CB_ADDSTRING, 0, (LPARAM)L"SELF");
                SendMessage(hNarratorSource, CB_ADDSTRING, 0, (LPARAM)L"TEAM");
                SendMessage(hNarratorSource, CB_ADDSTRING, 0, (LPARAM)L"ALL");
                SendMessage(hNarratorSource, CB_SETCURSEL, 0, 0); // Default to "SELF"
            }

            // Initialize "Toggle Private" checkbox (unchecked by default)
            HWND hTogglePrivate = GetDlgItem(hTabSettings, IDC_SETTINGS_TOGGLE_PRIVATE);
            if (hTogglePrivate) {
                Button_SetCheck(hTogglePrivate, BST_UNCHECKED);
            }

            // Initialize "System Mic" checkbox (unchecked by default)
            HWND hSystemMic = GetDlgItem(hTabSettings, IDC_SETTINGS_SYSTEM_MIC);
            if (hSystemMic) {
                Button_SetCheck(hSystemMic, BST_UNCHECKED);
            }

            // Initialize "Team Push To Talk Key" edit box (default to 'V')
            HWND hPTTKey = GetDlgItem(hTabSettings, IDC_SETTINGS_PTT_KEY);
            if (hPTTKey) {
                WCHAR keyStr[2] = { (WCHAR)g_pttKey, 0 };
                SetWindowTextW(hPTTKey, keyStr);
            }

            // Initialize "Toggle Team Key" checkbox (unchecked by default)
            HWND hToggleTeamKey = GetDlgItem(hTabSettings, IDC_SETTINGS_TOGGLE_TEAM_KEY);
            if (hToggleTeamKey) {
                Button_SetCheck(hToggleTeamKey, BST_UNCHECKED);
            }

            // Initialize "Sync Voice Settings" checkbox (unchecked by default)
            HWND hSyncVoice = GetDlgItem(hTabSettings, IDC_SETTINGS_SYNC_VOICE);
            if (hSyncVoice) {
                Button_SetCheck(hSyncVoice, BST_UNCHECKED);
            }


        }

        return TRUE;
    }
    case WM_NOTIFY: {
        LPNMHDR pnmh = (LPNMHDR)lParam;
        if (pnmh->idFrom == IDC_TAB_MAIN && pnmh->code == TCN_SELCHANGE) {
            HWND hTab = GetDlgItem(hWnd, IDC_TAB_MAIN);
            int sel = TabCtrl_GetCurSel(hTab);
            ShowTabDialog(hWnd, sel);
            currentTab = sel;
        }
        break;
    }
    case WM_COMMAND: {
        HWND hTabWnd = g_hTabDialogs[currentTab];
        int id = LOWORD(wParam);

        // Handle global commands (menu, etc.)
        if (id == IDM_ABOUT) {
            DialogBox(hInst, MAKEINTRESOURCE(IDD_ABOUTBOX), hWnd, About);
            break;
        }
        if (id == IDM_EXIT) {
            DestroyWindow(hWnd);
            break;
        }

        // Now handle tab-specific controls using hTabWnd
        switch (id) {
            // --- Main Tab ---
        case IDC_SPEAK_BUTTON:
            SpeakFromUI(hTabWnd);
            break;
        case IDC_STOP_BUTTON:
            if (!pVoice) {
                MessageBoxW(hTabWnd, L"Voice engine not available.", L"Error", MB_ICONERROR);
                return FALSE;
            }
            pVoice->Speak(NULL, SPF_PURGEBEFORESPEAK, NULL);
            break;

            // --- Info Tab (now also handles settings controls) ---
        case IDC_INFO_PREMIUM_BTN:
            MessageBoxW(hTabWnd, L"Redirecting to premium purchase...", L"Get Premium", MB_OK | MB_ICONINFORMATION);
            // Optionally, open a URL or show a premium dialog
            break;
        case IDC_INFO_DISCORD_BTN:
            ShellExecuteW(NULL, L"open", L"https://discord.gg/yourserver", NULL, NULL, SW_SHOWNORMAL);
            break;
        case IDC_BLOCK_ADD: {
            wchar_t buf[64];
            GetDlgItemTextW(hTabWnd, IDC_BLOCK_INPUT, buf, 64);
            if (wcslen(buf) > 0) {
                g_blockedIds.push_back(buf);
                HWND hList = GetDlgItem(hTabWnd, IDC_BLOCK_LIST);
                SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)buf);
                SetDlgItemTextW(hTabWnd, IDC_BLOCK_INPUT, L"");
                SaveBlockedIds();
            }
            break;
        }
        case IDC_BLOCK_REMOVE: {
            HWND hList = GetDlgItem(hTabWnd, IDC_BLOCK_LIST);
            int sel = (int)SendMessageW(hList, LB_GETCURSEL, 0, 0);
            if (sel != LB_ERR) {
                SendMessageW(hList, LB_DELETESTRING, sel, 0);
                g_blockedIds.erase(g_blockedIds.begin() + sel);
                SaveBlockedIds();
            }
            break;
        }
        case IDC_SYNC_SETTINGS:
            ExportSettingsToFile();
            MessageBoxW(hWnd, L"Settings exported to ValVoiceSettings.txt.\nYou can use this file with a companion tool or overlay.", L"Sync Complete", MB_OK | MB_ICONINFORMATION);
            break;
        case IDC_SETTINGS_SYNC_BTN:
            MessageBoxW(hTabWnd, L"Sync voice settings to valorant clicked.\n(Implement sync logic here.)", L"Settings", MB_OK | MB_ICONINFORMATION);
            break;
        }
        break;
    }
    case WM_KEYDOWN:
        if (g_waitingForPTT) {
            g_pttKey = (UINT)wParam;
            WCHAR keyName[16];
            wsprintf(keyName, L"%c", g_pttKey);
            g_waitingForPTT = false;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hWnd);
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    }
    return FALSE;
}

INT_PTR CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam) {
    UNREFERENCED_PARAMETER(lParam);
    switch (message) {
    case WM_INITDIALOG: return (INT_PTR)TRUE;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
            EndDialog(hDlg, LOWORD(wParam));
            return (INT_PTR)TRUE;
        }
        break;
    }
    return (INT_PTR)FALSE;
}

INT_PTR CALLBACK LoginDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_INITDIALOG:
        // Center the login dialog on the screen
    {
        RECT rc;
        GetWindowRect(hDlg, &rc);
        int winWidth = rc.right - rc.left;
        int winHeight = rc.bottom - rc.top;

        int screenWidth = GetSystemMetrics(SM_CXSCREEN);
        int screenHeight = GetSystemMetrics(SM_CYSCREEN);

        int x = (screenWidth - winWidth) / 2;
        int y = (screenHeight - winHeight) / 2;

        SetWindowPos(hDlg, HWND_TOP, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
    SetDlgItemTextW(hDlg, IDC_LOGIN_USERID, g_userId);
    Button_SetCheck(GetDlgItem(hDlg, IDC_LOGIN_PREMIUM), g_isPremium ? BST_CHECKED : BST_UNCHECKED);
    return (INT_PTR)TRUE;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            GetDlgItemTextW(hDlg, IDC_LOGIN_USERID, g_userId, _countof(g_userId));
            g_isPremium = (Button_GetCheck(GetDlgItem(hDlg, IDC_LOGIN_PREMIUM)) == BST_CHECKED);
            EndDialog(hDlg, IDOK);
            return (INT_PTR)TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            EndDialog(hDlg, IDCANCEL);
            return (INT_PTR)TRUE;
        }
        break;
    }
    return (INT_PTR)FALSE;
}

// Comment above TabDialogProc
// Handles messages for all tab dialogs (extend as needed)
INT_PTR CALLBACK TabDialogProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_INITDIALOG:
        LoadBlockedIds();
        return TRUE;
    case WM_COMMAND:
        // Add cases for WM_COMMAND, etc., if needed.
        break;
    }
    return FALSE;
}