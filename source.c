#include <windows.h>
#include <winsock2.h>
#include <psapi.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#pragma comment(lib, "ws2_32.lib")

// Console colors
#define COLOR_INFO 0x07
#define COLOR_SUCCESS 0x0A
#define COLOR_ERROR 0x0C
#define COLOR_WARNING 0x0E

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

// Global state
static void* g_luaInterface = NULL;
static void* g_luaShared = NULL;
static RunStringEx_t g_RunStringEx = NULL;
static int g_running = 1;

// GUI
HWND g_hEdit = NULL;
HWND g_hButton = NULL;
HWND g_hOutput = NULL;
int g_guiRunning = 1;

// Log
FILE* g_logFile = NULL;

// Forward declarations
void SetupConsole(void);
void LogInfo(const char* fmt, ...);
void LogSuccess(const char* fmt, ...);
void LogError(const char* fmt, ...);
void LogWarning(const char* fmt, ...);
void ExecuteLua(const char* code);
void RefreshInterface(void);
DWORD WINAPI MainThread(LPVOID lpParam);
DWORD WINAPI InterfaceWatcher(LPVOID lpParam);
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
void CreateGUI(void);

// ====== LOGGING ======

void SetupConsole() {
    if (AllocConsole()) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
        freopen("CONIN$", "r", stdin);
        SetConsoleTitleA("GMod Lua Executor v7.0");
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        SetConsoleTextAttribute(h, COLOR_INFO);
        printf("========================================\n");
        printf("   GMod x64 Lua Executor - v7.0\n");
        printf("========================================\n\n");
    }
}

void LogInfo(const char* fmt, ...) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTextAttribute(h, COLOR_INFO);
    printf("[INFO] ");
    va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a);
    printf("\n");
    SetConsoleTextAttribute(h, COLOR_INFO);
}

void LogSuccess(const char* fmt, ...) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTextAttribute(h, COLOR_SUCCESS);
    printf("[+] ");
    va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a);
    printf("\n");
    SetConsoleTextAttribute(h, COLOR_INFO);
}

void LogError(const char* fmt, ...) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTextAttribute(h, COLOR_ERROR);
    printf("[!] ");
    va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a);
    printf("\n");
    SetConsoleTextAttribute(h, COLOR_INFO);
}

void LogWarning(const char* fmt, ...) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTextAttribute(h, COLOR_WARNING);
    printf("[*] ");
    va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a);
    printf("\n");
    SetConsoleTextAttribute(h, COLOR_INFO);
}

// ====== INTERFACE FUNCTIONS ======

// Get Source Engine interface (CreateInterface pattern)
void* GetInterface(const char* moduleName, const char* interfaceName) {
    HMODULE hMod = GetModuleHandleA(moduleName);
    if (!hMod) {
        LogError("Module %s not found", moduleName);
        return NULL;
    }
    
    CreateInterfaceFn createInterface = (CreateInterfaceFn)GetProcAddress(hMod, "CreateInterface");
    if (!createInterface) {
        LogError("CreateInterface not found in %s", moduleName);
        return NULL;
    }
    
    void* iface = createInterface(interfaceName, NULL);
    if (!iface) {
        LogError("Interface %s not found", interfaceName);
        return NULL;
    }
    
    LogSuccess("Got interface %s at: 0x%llX", interfaceName, (uintptr_t)iface);
    return iface;
}

// Get the client Lua interface (type 0)
void* GetLuaInterface(void* luaShared) {
    if (!luaShared) return NULL;
    
    // CLuaShared vtable: GetLuaInterface is at index 6
    void** vtable = *(void***)luaShared;
    typedef void* (__fastcall* GetLuaInterface_t)(void* _this, int type);
    GetLuaInterface_t fn = (GetLuaInterface_t)vtable[6];
    
    // Try client (0) first, then menu (2), then server (1)
    void* iface = fn(luaShared, 0); // Client
    if (iface) {
        LogSuccess("Got CLIENT Lua interface at: 0x%llX", (uintptr_t)iface);
        return iface;
    }
    
    iface = fn(luaShared, 2); // Menu
    if (iface) {
        LogSuccess("Got MENU Lua interface at: 0x%llX", (uintptr_t)iface);
        return iface;
    }
    
    iface = fn(luaShared, 1); // Server
    if (iface) {
        LogSuccess("Got SERVER Lua interface at: 0x%llX", (uintptr_t)iface);
        return iface;
    }
    
    return NULL;
}

// Get RunStringEx from the Lua interface vtable (index 111)
RunStringEx_t GetRunStringEx(void* luaInterface) {
    if (!luaInterface) return NULL;
    
    void** vtable = *(void***)luaInterface;
    RunStringEx_t runStringEx = (RunStringEx_t)vtable[111];
    
    if (runStringEx) {
        LogSuccess("Got RunStringEx (vtable[111]) at: 0x%llX", (uintptr_t)runStringEx);
    }
    
    return runStringEx;
}

// Refresh the interface (handles disconnect/reconnect)
void RefreshInterface(void) {
    if (!g_luaShared) return;
    
    void* newInterface = GetLuaInterface(g_luaShared);
    if (newInterface && newInterface != g_luaInterface) {
        g_luaInterface = newInterface;
        g_RunStringEx = GetRunStringEx(g_luaInterface);
        LogSuccess("Lua interface refreshed: 0x%llX", (uintptr_t)g_luaInterface);
    }
}

// Execute Lua code
void ExecuteLua(const char* code) {
    if (!g_luaInterface || !g_RunStringEx) {
        LogError("Lua interface not ready!");
        RefreshInterface();
        if (!g_luaInterface || !g_RunStringEx) {
            LogError("Still not ready after refresh");
            return;
        }
    }
    
    LogInfo("Executing %zu bytes of code...", strlen(code));
    
    // Call RunStringEx
    // Parameters: this, filename, path, code, run, printErrors, dontPushErrors, noReturns
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
    
    LogSuccess("RunStringEx returned: %lld", result);
    
    // Update GUI
    if (g_hOutput) {
        char msg[512];
        sprintf(msg, "[+] Executed %zu bytes\n", strlen(code));
        SendMessageA(g_hOutput, EM_REPLACESEL, 0, (LPARAM)msg);
    }
}

// Interface watcher thread - keeps interface alive across map changes
DWORD WINAPI InterfaceWatcher(LPVOID lpParam) {
    while (g_running) {
        if (g_luaShared) {
            void* newInterface = GetLuaInterface(g_luaShared);
            if (newInterface && newInterface != g_luaInterface) {
                g_luaInterface = newInterface;
                g_RunStringEx = GetRunStringEx(g_luaInterface);
                LogSuccess("Interface updated: 0x%llX", (uintptr_t)g_luaInterface);
                
                if (g_hOutput) {
                    SendMessageA(g_hOutput, EM_REPLACESEL, 0, 
                        (LPARAM)"[+] Interface refreshed after map change\n");
                }
            }
        }
        Sleep(100);
    }
    return 0;
}

// ====== GUI ======

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            // Output
            g_hOutput = CreateWindowExA(
                0, "EDIT", "",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
                10, 10, 580, 200,
                hWnd, NULL, NULL, NULL
            );
            
            // Input
            g_hEdit = CreateWindowExA(
                0, "EDIT", "",
                WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL,
                10, 220, 580, 120,
                hWnd, NULL, NULL, NULL
            );
            
            // Execute button
            g_hButton = CreateWindowExA(
                0, "BUTTON", "Execute Lua",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                480, 350, 110, 30,
                hWnd, (HMENU)1, NULL, NULL
            );
            
            // Font
            HFONT hFont = CreateFontA(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                DEFAULT_QUALITY, FIXED_PITCH, "Consolas");
            SendMessageA(g_hEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessageA(g_hOutput, WM_SETFONT, (WPARAM)hFont, TRUE);
            
            // Defaults
            SetWindowTextA(g_hEdit, "print('Hello from GmExec!')");
            SetWindowTextA(g_hOutput, "GMod Lua Executor v7.0\r\nReady to execute!\r\n\r\n");
            break;
        }
        case WM_COMMAND: {
            if (LOWORD(wParam) == 1) {
                char buffer[65536];
                GetWindowTextA(g_hEdit, buffer, sizeof(buffer));
                
                if (strlen(buffer) > 0) {
                    // Show what we're executing
                    char output[512];
                    sprintf(output, "> %.60s%s\r\n", buffer, strlen(buffer) > 60 ? "..." : "");
                    SendMessageA(g_hOutput, EM_REPLACESEL, 0, (LPARAM)output);
                    
                    // Execute
                    ExecuteLua(buffer);
                }
            }
            break;
        }
        case WM_KEYDOWN: {
            if (wParam == VK_F5) {
                // F5 to execute
                SendMessageA(hWnd, WM_COMMAND, 1, 0);
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
    wc.lpszClassName = "GModLuaExec";
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    
    RegisterClassExA(&wc);
    
    HWND hWnd = CreateWindowExA(
        0, "GModLuaExec", "GMod Lua Executor v7.0",
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

// ====== MAIN ======

DWORD WINAPI MainThread(LPVOID lpParam) {
    Sleep(3000);
    
    SetupConsole();
    
    LogInfo("GMod Lua Executor v7.0 initializing...");
    LogInfo("PID: %d", GetCurrentProcessId());
    
    // Step 1: Get lua_shared interface
    LogInfo("\nStep 1: Getting LUASHARED003 interface...");
    g_luaShared = GetInterface("lua_shared.dll", "LUASHARED003");
    if (!g_luaShared) {
        LogError("Failed to get lua_shared interface!");
        return 1;
    }
    
    // Step 2: Wait for and get client Lua interface
    LogInfo("\nStep 2: Waiting for client Lua interface...");
    int attempts = 0;
    while (!g_luaInterface && g_running && attempts < 100) {
        g_luaInterface = GetLuaInterface(g_luaShared);
        if (!g_luaInterface) {
            Sleep(100);
            attempts++;
        }
    }
    
    if (!g_luaInterface) {
        LogError("Failed to get Lua interface after %d attempts!", attempts);
        return 1;
    }
    
    // Step 3: Get RunStringEx from vtable
    LogInfo("\nStep 3: Getting RunStringEx from vtable...");
    g_RunStringEx = GetRunStringEx(g_luaInterface);
    if (!g_RunStringEx) {
        LogError("Failed to get RunStringEx!");
        return 1;
    }
    
    // Step 4: Start interface watcher thread
    LogInfo("\nStep 4: Starting interface watcher...");
    CreateThread(NULL, 0, InterfaceWatcher, NULL, 0, NULL);
    
    LogSuccess("\n=== READY TO EXECUTE LUA! ===");
    LogInfo("Lua interface: 0x%llX", (uintptr_t)g_luaInterface);
    LogInfo("RunStringEx: 0x%llX", (uintptr_t)g_RunStringEx);
    LogInfo("Press Execute or F5 to run code");
    
    // Step 5: Open GUI
    LogInfo("\nOpening executor window...");
    CreateGUI();
    
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