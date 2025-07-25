#include "framework.h"

#include "Resource.h"


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
#include <windows.h>
#include <winhttp.h>
#include <thread>
#include <mmsystem.h>


#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "Shell32.lib")  // Link against Shell32.lib
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "winmm.lib")

#define MAX_LOADSTRING 100



// Global Variables
HINSTANCE hInst;
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

// Forward declarations for TTS/audio helper functions
HINTERNET CreateTtsConnection(const wchar_t* server, INTERNET_PORT port);
bool SendTtsRequest(HINTERNET hConnect, const std::wstring& text, std::vector<BYTE>& audioData);
bool SaveAudioToFile(const std::vector<BYTE>& audioData, const wchar_t* filename);
void PlayAudioFile(const wchar_t* filename);
void LogTtsError(const wchar_t* context, const wchar_t* message);
void DeleteAudioFile(const wchar_t* filename);

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
    wchar_t text[1024];
    GetDlgItemTextW(hTabWnd, IDC_TEXT_INPUT, text, 1024);
    if (wcslen(text) == 0) {
        MessageBoxW(hTabWnd, L"Please enter text to speak.", L"Info", MB_ICONINFORMATION);
        return;
    }

    // Copy text for thread safety
    std::wstring textCopy(text);

    std::thread([hTabWnd, textCopy]() {
        // Try Coqui TTS server first
        HINTERNET hConnect = CreateTtsConnection(L"localhost", 5002);
        bool coquiSuccess = false;
        const wchar_t* audioFile = L"tts_output.wav";
        if (hConnect) {
            std::vector<BYTE> audioData;
            if (SendTtsRequest(hConnect, textCopy, audioData)) {
                if (SaveAudioToFile(audioData, audioFile)) {
                    PlayAudioFile(audioFile);
                    coquiSuccess = true;
                    DeleteAudioFile(audioFile); // Clean up after playback
                } else {
                    LogTtsError(L"Coqui", L"Failed to save audio file.");
                }
            } else {
                LogTtsError(L"Coqui", L"TTS request failed.");
            }
            WinHttpCloseHandle(hConnect);
        } else {
            LogTtsError(L"Coqui", L"Failed to connect to TTS server.");
        }

    }).detach();
}

void EnableDarkMode(HWND hwnd) {
    BOOL dark = TRUE;
    // 20 = DWMWA_USE_IMMERSIVE_DARK_MODE before Windows 11, 19 for Windows 11+
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
    DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));
}

std::string WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine, _In_ int nCmdShow) {
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    //// Launch Riot Client at app startup
    //HINSTANCE result = ShellExecuteW(
    //    NULL,
    //    L"open",
    //    L"C:\\Riot Games\\Riot Client\\RiotClientServices.exe", // Use Riot Client path
    //    NULL,
    //    NULL,
    //    SW_SHOWNORMAL
    //);
    //if ((INT_PTR)result <= 32) {
    //    WCHAR buf[64];
    //    wsprintf(buf, L"Failed to launch Riot Client. Error code: %d", (int)(INT_PTR)result);
    //    MessageBoxW(NULL, buf, L"Error", MB_OK | MB_ICONERROR);
    //}

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
            //PopulateVoices(hVoiceCombo);
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

            HWND hNarratorVoiceCombo = GetDlgItem(hTabSettings, IDC_NARRATOR_VOICE_COMBO);
            //PopulateNarratorVoices(hNarratorVoiceCombo);
            SendMessage(hNarratorVoiceCombo, CB_SETCURSEL, 0, 0); // Select first voice by default
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

HINTERNET CreateTtsConnection(const wchar_t* server, INTERNET_PORT port) {
    // Open a WinHTTP session
    HINTERNET hSession = WinHttpOpen(L"ValVoice/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!hSession) return NULL;

    // Connect to the TTS server
    HINTERNET hConnect = WinHttpConnect(hSession, server, port, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return NULL;
    }

    // Note: hSession must be closed after hConnect is closed (see next steps)
    return hConnect;
}

bool SendTtsRequest(HINTERNET hConnect, const std::wstring& text, std::vector<BYTE>& audioData) {
    const wchar_t* path = L"/api/tts";
    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, L"POST", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hRequest) return false;

    std::string utf8Text = WideToUtf8(text);
    std::string json = "{\"text\": \"" + utf8Text + "\"}";

    BOOL bResult = WinHttpSendRequest(
        hRequest,
        L"Content-Type: application/json\r\n",
        0,
        (LPVOID)json.c_str(),
        json.size(),
        json.size(),
        0
    );
    if (!bResult) {
        WinHttpCloseHandle(hRequest);
        return false;
    }

    if (!WinHttpReceiveResponse(hRequest, NULL)) {
        WinHttpCloseHandle(hRequest);
        return false;
    }

    DWORD dwSize = 0;
    do {
        DWORD dwDownloaded = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &dwSize) || dwSize == 0) break;
        std::vector<BYTE> buffer(dwSize);
        if (!WinHttpReadData(hRequest, buffer.data(), dwSize, &dwDownloaded)) break;
        audioData.insert(audioData.end(), buffer.begin(), buffer.begin() + dwDownloaded);
    } while (dwSize > 0);

    WinHttpCloseHandle(hRequest);
    return !audioData.empty();
}

bool SaveAudioToFile(const std::vector<BYTE>& audioData, const wchar_t* filename) {
    std::ofstream ofs(filename, std::ios::binary);
    if (!ofs) return false;
    ofs.write(reinterpret_cast<const char*>(audioData.data()), audioData.size());
    return true;
}

void PlayAudioFile(const wchar_t* filename) {
    PlaySoundW(filename, NULL, SND_FILENAME | SND_ASYNC);
}

void LogTtsError(const wchar_t* context, const wchar_t* message) {
    std::wofstream ofs(L"TTS_ErrorLog.txt", std::ios::app);
    if (!ofs) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    ofs << L"[" << st.wYear << L"-" << st.wMonth << L"-" << st.wDay << L" "
        << st.wHour << L":" << st.wMinute << L":" << st.wSecond << L"] "
        << context << L": " << message << std::endl;
}

void DeleteAudioFile(const wchar_t* filename) {
    DeleteFileW(filename);
}