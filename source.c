#include <windows.h>
#include <winsock2.h>
#include <psapi.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#pragma comment(lib, "ws2_32.lib")

// RunStringEx signature (x64 __fastcall)
typedef int64_t(__fastcall* RunStringEx_t)(
    void* _this,
    const char* filename,
    const char* path,
    const char* stringToRun,
    int run,
    int printErrors,
    int dontPushErrors,
    int noReturns
);

// CreateInterface signature
typedef void* (*CreateInterfaceFn)(const char* name, int* returnCode);

static void* g_luaInterface = NULL;
static void* g_luaShared = NULL;
static RunStringEx_t g_RunStringEx = NULL;
static int g_running = 1;

HWND g_hEdit = NULL;
HWND g_hButton = NULL;
HWND g_hOutput = NULL;
int g_guiRunning = 1;

FILE* g_logFile = NULL;

void SetupConsole(void);
void Info(const char* fmt, ...);
void Success(const char* fmt, ...);
void Error(const char* fmt, ...);
void Warn(const char* fmt, ...);
void ExecuteLua(const char* code);
void RefreshInterface(void);
DWORD WINAPI MainThread(LPVOID lpParam);
DWORD WINAPI InterfaceWatcher(LPVOID lpParam);
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
void CreateGUI(void);

void SetupConsole() {
    if (AllocConsole()) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
        freopen("CONIN$", "r", stdin);
        SetConsoleTitleA("GmExec v3.0 - github.com/solal0/GmExec");
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        SetConsoleTextAttribute(h, 0x07);
        printf("[i] GmExec - v3.0\n");
        printf("[i] Repo at https://github.com/solal0/GmExec\n\n");
    }
}

typedef struct
{
    char message[4096];
    char prefix[16];
    WORD color;
    int count;
    int valid;
} LastLog;

static LastLog lastLog = { 0 };

void PrintLog(WORD color, const char* prefix, const char* fmt, va_list args)
{
    char message[4096];
    vsnprintf_s(message,sizeof(message),_TRUNCATE,fmt,args);
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);

    if (lastLog.valid &&
        strcmp(lastLog.message, message) == 0 &&
        strcmp(lastLog.prefix, prefix) == 0)
    {
        lastLog.count++;

        CONSOLE_SCREEN_BUFFER_INFO csbi;

        if (GetConsoleScreenBufferInfo(h, &csbi))
        {
            COORD currentPos = csbi.dwCursorPosition;
            COORD previousLine = currentPos;
            previousLine.Y--;
            previousLine.X = 0;

            SetConsoleCursorPosition(h, previousLine);

            DWORD written;

            FillConsoleOutputCharacterA(h,' ',csbi.dwSize.X,previousLine,&written);
            SetConsoleCursorPosition(h, previousLine);
            SetConsoleTextAttribute(h, lastLog.color);
            printf("%s%s x%d",lastLog.prefix,lastLog.message,lastLog.count);
            SetConsoleCursorPosition(h, currentPos);
            SetConsoleTextAttribute(h, 0x07);
        }
        return;
    }

    SetConsoleTextAttribute(h, color);
    printf("%s%s\n", prefix, message);
    SetConsoleTextAttribute(h, 0x07);
    strcpy_s(lastLog.message, sizeof(lastLog.message), message);
    strcpy_s(lastLog.prefix, sizeof(lastLog.prefix), prefix);

    lastLog.color = color;
    lastLog.count = 1;
    lastLog.valid = 1;
}

void Info(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    PrintLog(0x07, "[i] ", fmt, args);
    va_end(args);
}

void Success(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    PrintLog(0x0A, "[+] ", fmt, args);
    va_end(args);
}

void Error(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    PrintLog(0x0C, "[!] ", fmt, args);
    va_end(args);
}

void Warn(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    PrintLog(0x0E, "[*] ", fmt, args);
    va_end(args);
}

// get source engine interface
void* GetInterface(const char* moduleName, const char* interfaceName) {
    HMODULE hMod = GetModuleHandleA(moduleName);
    if (!hMod) {
        Error("Module %s not found", moduleName);
        return NULL;
    }
    
    CreateInterfaceFn createInterface = (CreateInterfaceFn)GetProcAddress(hMod, "CreateInterface");
    if (!createInterface) {
        Error("CreateInterface not found in %s", moduleName);
        return NULL;
    }
    
    void* iface = createInterface(interfaceName, NULL);
    if (!iface) {
        Error("Interface %s not found", interfaceName);
        return NULL;
    }
    
    Success("Got interface %s at: 0x%llX", interfaceName, (uintptr_t)iface);
    return iface;
}

// get the client lua interface (type 0)
void* GetLuaInterface(void* luaShared) {
    if (!luaShared) return NULL;
    
    // CLuaShared vtable: GetLuaInterface is at index 6
    void** vtable = *(void***)luaShared;
    typedef void* (__fastcall* GetLuaInterface_t)(void* _this, int type);
    GetLuaInterface_t fn = (GetLuaInterface_t)vtable[6];
    
    // try client (0) first, then menu (2), then server (1)
    void* iface = fn(luaShared, 0); // client
    if (iface) {
        Success("Got CLIENT Lua interface at: 0x%llX", (uintptr_t)iface);
        return iface;
    }
    
    iface = fn(luaShared, 2); // menu
    if (iface) {
        Success("Got MENU Lua interface at: 0x%llX", (uintptr_t)iface);
        return iface;
    }
    
    iface = fn(luaShared, 1); // server
    if (iface) {
        Success("Got SERVER Lua interface at: 0x%llX", (uintptr_t)iface);
        return iface;
    }
    
    return NULL;
}

// get RunStringEx from the lua interface vtable (index 111)
RunStringEx_t GetRunStringEx(void* luaInterface) {
    if (!luaInterface) return NULL;
    
    void** vtable = *(void***)luaInterface;
    RunStringEx_t runStringEx = (RunStringEx_t)vtable[111];
    
    if (runStringEx) {
        Success("Got RunStringEx (vtable[111]) at: 0x%llX", (uintptr_t)runStringEx);
    }
    
    return runStringEx;
}

// refresh the interface (handles disconnect/reconnect)
void RefreshInterface(void) {
    if (!g_luaShared) return;
    
    void* newInterface = GetLuaInterface(g_luaShared);
    if (newInterface && newInterface != g_luaInterface) {
        g_luaInterface = newInterface;
        g_RunStringEx = GetRunStringEx(g_luaInterface);
        Success("Lua interface refreshed: 0x%llX", (uintptr_t)g_luaInterface);
    }
}

void ExecuteLua(const char* code) {
    if (!g_luaInterface || !g_RunStringEx) {
        Error("Lua interface not ready!");
        RefreshInterface();
        if (!g_luaInterface || !g_RunStringEx) {
            Error("Still not ready after refresh");
            return;
        }
    }
    
    Info("Executing %zu bytes of code...", strlen(code));

    // params to call RunStringEx: this, filename, path, code, run, printErrors, dontPushErrors, noReturns
    int64_t result = g_RunStringEx(
        g_luaInterface,
        "GmExec",     // filename
        "",           // path
        code,         // the actual code
        1,            // run
        1,            // printErrors
        0,            // dontPushErrors
        0             // noReturns
    );
    
    Success("RunStringEx returned: %lld", result);
    
    // Update GUI
    if (g_hOutput) {
        char msg[512];
        sprintf(msg, "[+] Executed %zu bytes\r\n", strlen(code));
        SendMessageA(g_hOutput, EM_SETSEL, -1, -1);
        SendMessageA(g_hOutput, EM_REPLACESEL, FALSE, (LPARAM)msg);
    }
}

// keeps interface alive across map changes
DWORD WINAPI InterfaceWatcher(LPVOID lpParam) {
    while (g_running) {
        if (g_luaShared) {
            void* newInterface = GetLuaInterface(g_luaShared);
            if (newInterface && newInterface != g_luaInterface) {
                g_luaInterface = newInterface;
                g_RunStringEx = GetRunStringEx(g_luaInterface);
                Success("Interface updated: 0x%llX", (uintptr_t)g_luaInterface);
                
                if (g_hOutput) {
                    SendMessageA(g_hOutput, EM_REPLACESEL, 0, (LPARAM)"[+] Interface refreshed after map change\n");
                }
            }
        }
        Sleep(100);
    }
    return 0;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            g_hOutput = CreateWindowExA(
                0, "EDIT", "",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
                10, 10, 580, 200,
                hWnd, NULL, NULL, NULL
            );

            g_hEdit = CreateWindowExA(
                0, "EDIT", "",
                WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL,
                10, 220, 580, 120,
                hWnd, NULL, NULL, NULL
            );

            g_hButton = CreateWindowExA(
                0, "BUTTON", "Execute",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                480, 350, 110, 30,
                hWnd, (HMENU)1, NULL, NULL
            );

            HFONT hFont = CreateFontA(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                DEFAULT_QUALITY, FIXED_PITCH, "Consolas");
            SendMessageA(g_hEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessageA(g_hOutput, WM_SETFONT, (WPARAM)hFont, TRUE);
            SetWindowTextA(g_hEdit, "print('Hello from GmExec!')");
            break;
        }
        case WM_COMMAND: {
            if (LOWORD(wParam) == 1) {
                char buffer[65536];
                GetWindowTextA(g_hEdit, buffer, sizeof(buffer));
                
                if (strlen(buffer) > 0) {
                    // show what we're executing
                    char output[512];
                    sprintf(output, "\n> %.60s%s\r\n", buffer, strlen(buffer) > 60 ? "..." : "");
                    SendMessageA(g_hOutput, EM_SETSEL, -1, -1);
                    SendMessageA(g_hOutput, EM_REPLACESEL, FALSE, (LPARAM)output);
                    ExecuteLua(buffer); // execute
                }
            }
            break;
        }
        case WM_CLOSE:
            DestroyWindow(hWnd);
            break;
        case WM_DESTROY:
            g_guiRunning = 0;
            PostQuitMessage(0);
            break;
        default:
            return DefWindowProc(hWnd, msg, wParam, lParam);
    }
    return 0;
}

void CreateGUI(void) {
    WNDCLASSEXA wc = {0};
    wc.cbSize = sizeof(WNDCLASSEXA);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "GmExec";
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    
    RegisterClassExA(&wc);
    
    HWND hWnd = CreateWindowExA(
        0, "GmExec", "GmExec v3.0 - github.com/solal0/GmExec",
        WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 616, 420,
        NULL, NULL, wc.hInstance, NULL
    );
    
    ShowWindow(hWnd, SW_SHOW);
    UpdateWindow(hWnd);
    
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) && g_guiRunning) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}

DWORD WINAPI MainThread(LPVOID lpParam) {
    Sleep(3000);
    
    SetupConsole();
    
    Info("GmExec v3.0 initializing...");
    Info("PID: %d", GetCurrentProcessId());
    
    // get lua_shared interface
    Info("Getting LUASHARED003 interface...");
    g_luaShared = GetInterface("lua_shared.dll", "LUASHARED003");
    if (!g_luaShared) {
        Error("Failed to get lua_shared interface!");
        return 1;
    }
    
    // wait for and get client lua interface
    Info("Waiting for client Lua interface...");
    int attempts = 0;
    while (!g_luaInterface && g_running && attempts < 100) {
        g_luaInterface = GetLuaInterface(g_luaShared);
        if (!g_luaInterface) {
            Sleep(100);
            attempts++;
        }
    }
    
    if (!g_luaInterface) {
        Error("Failed to get Lua interface after %d attempts!", attempts);
        return 1;
    }
    
    // get RunStringEx from vtable
    Info("Getting RunStringEx from vtable...");
    g_RunStringEx = GetRunStringEx(g_luaInterface);
    if (!g_RunStringEx) {
        Error("Failed to get RunStringEx!");
        return 1;
    }
    
    // start interface watcher thread
    Info("Starting interface watcher...");
    CreateThread(NULL, 0, InterfaceWatcher, NULL, 0, NULL);
    
    Success("GmExec 3.0 initialized successfully, enjoy !");
    Info("Lua interface: 0x%llX", (uintptr_t)g_luaInterface);
    Info("RunStringEx: 0x%llX", (uintptr_t)g_RunStringEx);
    CreateGUI(); // open the GUI
    
    g_running = 0;
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        CreateThread(NULL, 0, MainThread, NULL, 0, NULL);
    }
    return TRUE;
}
