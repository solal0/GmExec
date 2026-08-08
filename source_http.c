#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <setjmp.h>

#pragma comment(lib, "ws2_32.lib")

// The base I made worked but crashed the game after running 20 prints. So I got help from claude to prevent crashes and looks like it's working.

#define CONSOLE true
#define ADDRESS "127.0.0.1"
#define PORT 8080
#define MAX_BYTES 4096 // 4kb of code max otherwise it doesn't run it, change it at your own risk.
#define QUEUE 64 // scripts executed are queued, if there's a 65th one coming it gets ignored
#define FPS_LIMIT 3 // numer of queued scripts it will run at once
#define FPS_BUDGET 2.0 // if a script takes more than 2ms to run, it's skipped
#define EXPIRY 3000 // if a queued script stays in the queue for more than 3 seconds it's deleted
#define THROTTLE 50 // prevents request (POST) spams with a 50ms timeout
#define RECV_WAIT 5000 // if a client's request takes more than 5 seconds, it's closed
#define SEND_WAIT 5000 // same as above but server side
#define HOOK_ID 35

// the rest of the comments below were added by claude

typedef int64_t (*RunStringEx_t)(void*, const char*, const char*, const char*, int, int, int, int);
typedef void*   (*CreateInterfaceFn)(const char*, int*);
typedef void    (*FrameStageNotify_t)(void*, int);

// globals
static void*  g_luaShared          = NULL;
static void*  g_luaInterface       = NULL;
static void*  g_clientDLLInterface = NULL;
static RunStringEx_t g_RunStringEx = NULL;

static volatile bool g_running   = true;
static volatile bool g_hookOK    = false;
static volatile bool g_crashGuardTripped = false;

static CRITICAL_SECTION g_queueLock;
static CRITICAL_SECTION g_luaLock;      // protects interface pointer swap only
static CRITICAL_SECTION g_logLock;

static FrameStageNotify_t g_origFrameStageNotify = NULL;

static SOCKET g_listenSock = INVALID_SOCKET;

// simple atomic-ish stats (Interlocked)
static volatile LONG g_statExecuted = 0;
static volatile LONG g_statRejected = 0;
static volatile LONG g_statErrors   = 0;
static volatile LONG g_statCrashesCaught = 0;

// crash guard (VEH-based, since MinGW has no __try/__except)
static __declspec(thread) jmp_buf g_jmpBuf;
static __declspec(thread) bool    g_jmpArmed = false;

const char* HTML_PAGE =
"<!doctype html><html><head>"
"<meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>GmExec</title>"
"<style>"
"*{box-sizing:border-box}"
"html,body{margin:0;height:100%;overflow:hidden;background:#050505;color:#eee;font:13px Inter,Segoe UI,sans-serif}"
"body{display:flex;flex-direction:column}"
"#bar{height:46px;display:flex;align-items:center;padding:0 14px;gap:10px;background:#090909;border-bottom:1px solid #222;box-shadow:0 2px 20px #000;animation:bar .5s ease}"
"#logo{font-weight:700;letter-spacing:.5px;color:#fff}"
"#status{flex:1;color:#777;font-size:11px;transition:.25s}"
".ok{color:#aaa!important}"
"button{border:1px solid #333;background:#111;color:#ddd;border-radius:5px;padding:6px 13px;font-size:11px;cursor:pointer;transition:.2s}"
"button:hover{background:#fff;color:#000;transform:translateY(-1px);box-shadow:0 4px 15px #000}"
"button:active{transform:scale(.96)}"
"button:disabled{opacity:.35;transform:none}"
"#run{background:#eee;color:#080808;border-color:#eee;font-weight:700}"
"#editor{flex:1;min-height:0}"
".cm-editor{height:100%;font-size:14px;background:#050505!important}"
".cm-scroller{font-family:'JetBrains Mono','Cascadia Code',Consolas,monospace!important;overflow:auto}"
".cm-gutters{background:#080808!important;color:#444!important;border:0!important}"
".cm-activeLine,.cm-activeLineGutter{background:#0c0c0c!important}"
".cm-selectionBackground{background:#333!important}"
"@keyframes bar{from{opacity:0;transform:translateY(-8px)}to{opacity:1;transform:none}}"
"</style></head><body>"
"<header id='bar'>"
"<span id='logo'>GmExec</span>"
"<span id='status'>Initializing Lua...</span>"
"<button id='run'>▶ Execute</button>"
"</header>"
"<main id='editor'></main>"
"<script type='module'>"
"import{EditorState}from'https://esm.sh/@codemirror/state';"
"import{EditorView,keymap,lineNumbers,drawSelection,highlightActiveLine,highlightActiveLineGutter}from'https://esm.sh/@codemirror/view';"
"import{defaultKeymap,indentWithTab}from'https://esm.sh/@codemirror/commands';"
"import{StreamLanguage,indentOnInput,indentUnit,syntaxHighlighting,HighlightStyle}from'https://esm.sh/@codemirror/language';"
"import{tags}from'https://esm.sh/@lezer/highlight';"
"import{lua}from'https://esm.sh/@codemirror/legacy-modes/mode/lua';"
"import{autocompletion,completeFromList}from'https://esm.sh/@codemirror/autocomplete';"
"const status=document.querySelector('#status'),run=document.querySelector('#run');"
"const luaWords=['assert','break','do','else','elseif','end','false','for','function','goto','if','in','local','nil','not','or','repeat','return','then','true','until','while'];"
"const luaAPI=['print','pairs','ipairs','type','tostring','tonumber','require','pcall','xpcall','select','next','error','setmetatable','getmetatable','rawget','rawset'];"
"const colors=HighlightStyle.define([{tag:tags.keyword,color:'#569CD6'},{tag:tags.string,color:'#CE9178'},{tag:tags.number,color:'#B5CEA8'},{tag:tags.comment,color:'#6A9955'},{tag:tags.variableName,color:'#9CDCFE'},{tag:tags.function(tags.variableName),color:'#DCDCAA'},{tag:tags.operator,color:'#D4D4D4'},{tag:tags.bool,color:'#569CD6'},{tag:tags.null,color:'#569CD6'}]);"
"const words=[...luaWords.map(label=>({label,type:'keyword'})),...luaAPI.map(label=>({label,type:'function'}))];"
"function opensBlock(line){"
"line=line.trim();"
"return /^(function\\b.*|if\\b.*\\bthen|for\\b.*\\bdo|while\\b.*\\bdo|do)$/.test(line);"
"}"
"const autoEnd={"
"key:'Enter',"
"run(view){"
"const pos=view.state.selection.main.head;"
"const line=view.state.doc.lineAt(pos);"
"const text=line.text.trim();"
"if(!/^(function\\b.*|if\\b.*\\bthen|for\\b.*\\bdo|while\\b.*\\bdo|do)$/.test(text))return false;"
"const indent=(line.text.match(/^\\s*/)||[''])[0];"
"const inner=indent+'    ';"
"view.dispatch({"
"changes:{"
"from:pos,"
"to:pos,"
"insert:'\\n'+inner+'\\n'+indent+'end'"
"},"
"selection:{"
"anchor:pos+1+inner.length"
"}"
"});"
"return true;"
"}"
"};"

"const state=EditorState.create({"
"doc:'local variable = 1\\n\\nif variable then',"
"extensions:["
"lineNumbers(),"
"StreamLanguage.define(lua),"
"indentUnit.of('    '),"
"indentOnInput(),"
"syntaxHighlighting(colors),"
"keymap.of([autoEnd,indentWithTab,...defaultKeymap]),"
"highlightActiveLine(),"
"highlightActiveLineGutter(),"
"drawSelection(),"
"autocompletion({"
"override:[completeFromList(words)],"
"activateOnTyping:true"
"}),"
"EditorView.theme({"
"'&':{backgroundColor:'#050505',color:'#eee'},"
"'.cm-content':{caretColor:'#fff',padding:'10px 0'},"
"'.cm-cursor':{borderLeftColor:'#fff'},"
"'.cm-tooltip':{backgroundColor:'#111',color:'#eee',border:'1px solid #333'},"
"'.cm-tooltip-autocomplete ul li[aria-selected]':{backgroundColor:'#eee',color:'#000'}"
"})"
"]"
"});"

"const view=new EditorView({"
"state,"
"parent:document.querySelector('#editor')"
"});"

"status.textContent='Lua ready';"
"status.className='ok';"

"run.onclick=async()=>{"
"if(run.disabled)return;"
"const code=view.state.doc.toString();"
"if(!code.trim()){status.textContent='Empty code';return;}"
"if(code.length>4096){status.textContent='Maximum 4096 characters';return;}"
"run.disabled=true;"
"status.textContent='Executing...';"
"try{"
"const r=await fetch('/execute',{"
"method:'POST',"
"headers:{'Content-Type':'text/plain'},"
"body:code"
"});"
"status.textContent="
"r.status===200?'Executed!':"
"r.status===429?'Rate limited':"
"r.status===401?'Unauthorized':"
"r.status===413?'Too large':"
"r.status===503?'Executor unavailable':"
"'Error '+r.status;"
"}catch(e){"
"status.textContent='Network error';"
"}"
"run.disabled=false;"
"setTimeout(()=>status.textContent='Lua ready',2000);"
"};"

"view.dom.addEventListener('keydown',e=>{"
"if((e.ctrlKey||e.metaKey)&&e.key==='Enter'){"
"e.preventDefault();"
"run.click();"
"}"
"});"
"</script></body></html>";

// ================= LOGGING =================
void LogMsg(const char* fmt, ...) {
    if (!CONSOLE) return;
    EnterCriticalSection(&g_logLock);
    va_list args;
    va_start(args, fmt);
    SYSTEMTIME st; GetLocalTime(&st);
    printf("[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);
    vprintf(fmt, args);
    printf("\n");
    va_end(args);
    LeaveCriticalSection(&g_logLock);
}

// ============= MEMORY VALIDATION =============
// Never trust a stored pointer blindly before calling through it.
// This won't catch everything, but it stops the obvious "module unloaded /
// pointer went stale / vtable got smashed" class of crashes.

bool IsMemoryReadable(const void* ptr, size_t len) {
    if (!ptr) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & PAGE_NOACCESS) return false;
    if (mbi.Protect & PAGE_GUARD) return false;
    // rough check that the whole range is in this region
    uintptr_t regionEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    if ((uintptr_t)ptr + len > regionEnd) return false;
    return true;
}

bool IsCodePointerValid(const void* ptr) {
    if (!ptr) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    DWORD execFlags = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!(mbi.Protect & execFlags)) return false;
    return true;
}

bool IsInterfaceSane(void* iface) {
    if (!IsMemoryReadable(iface, sizeof(void*))) return false;
    void** vtable = *(void***)iface;
    if (!IsMemoryReadable(vtable, sizeof(void*) * 8)) return false;
    return true;
}

// ============= CRASH GUARD (VEH) =============
// MinGW has no __try/__except. This uses a Vectored Exception Handler +
// setjmp/longjmp as a last-resort net around the actual Lua call.
// It is NOT a substitute for the main-thread fix below -- it's a backstop
// for genuine access violations inside the call we can't otherwise predict.

LONG CALLBACK CrashGuardVEH(EXCEPTION_POINTERS* info) {
    DWORD code = info->ExceptionRecord->ExceptionCode;
    bool fatal = (code == EXCEPTION_ACCESS_VIOLATION) ||
                 (code == EXCEPTION_ILLEGAL_INSTRUCTION) ||
                 (code == EXCEPTION_STACK_OVERFLOW) ||
                 (code == EXCEPTION_PRIV_INSTRUCTION) ||
                 (code == EXCEPTION_IN_PAGE_ERROR);

    if (fatal && g_jmpArmed) {
        InterlockedIncrement(&g_statCrashesCaught);
        g_crashGuardTripped = true;

        if (code == EXCEPTION_STACK_OVERFLOW) {
            // restore stack guard page so the process can keep running
            _resetstkoflw();
        }

        g_jmpArmed = false;
        longjmp(g_jmpBuf, 1);
        // never returns
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// ================= INTERFACE ACQUISITION =================
void* GetInterface(const char* module, const char* iface) {
    HMODULE hMod = GetModuleHandleA(module);
    if (!hMod) return NULL;
    CreateInterfaceFn fn = (CreateInterfaceFn)GetProcAddress(hMod, "CreateInterface");
    if (!fn || !IsCodePointerValid((void*)fn)) return NULL;
    int status = 0;
    void* result = fn(iface, &status);
    return result;
}

// Try several known version strings since these drift between GMod builds.
void* GetInterfaceAnyVersion(const char* module, const char* base, int minV, int maxV) {
    char buf[64];
    for (int v = maxV; v >= minV; v--) {
        sprintf(buf, "%s%03d", base, v);
        void* r = GetInterface(module, buf);
        if (r) {
            LogMsg("Resolved interface %s", buf);
            return r;
        }
    }
    return NULL;
}

void* GetLuaInterfaceFromShared(void* shared) {
    if (!IsInterfaceSane(shared)) return NULL;
    void** vtable = *(void***)shared;
    if (!IsMemoryReadable(&vtable[6], sizeof(void*))) return NULL;

    typedef void* (*GetLuaIface_t)(void*, int);
    GetLuaIface_t fn = (GetLuaIface_t)vtable[6];
    if (!IsCodePointerValid((void*)fn)) return NULL;

    void* iface = fn(shared, 0); // Client
    if (!iface) iface = fn(shared, 2); // Menu
    if (!iface) iface = fn(shared, 1); // Server
    if (iface && !IsInterfaceSane(iface)) return NULL;
    return iface;
}

bool RefreshLuaInterface(void) {
    if (!g_luaShared) return false;
    void* newIface = GetLuaInterfaceFromShared(g_luaShared);
    if (!newIface) return false;

    EnterCriticalSection(&g_luaLock);
    bool changed = (newIface != g_luaInterface);
    if (changed) {
        g_luaInterface = newIface;
        void** vtable = *(void***)g_luaInterface;
        if (IsMemoryReadable(&vtable[111], sizeof(void*)) && IsCodePointerValid(vtable[111])) {
            g_RunStringEx = (RunStringEx_t)vtable[111];
        } else {
            g_RunStringEx = NULL;
            g_luaInterface = NULL;
            changed = false;
        }
    }
    LeaveCriticalSection(&g_luaLock);
    return changed;
}

// ================= EXECUTION QUEUE =================
typedef struct {
    char  code[MAX_BYTES];
    DWORD queuedAt;
} QueuedExec;

static QueuedExec g_queue[QUEUE];
static int g_qHead = 0, g_qTail = 0, g_qCount = 0;

bool QueuePush(const char* code, size_t len) {
    bool ok = false;
    EnterCriticalSection(&g_queueLock);
    if (g_qCount < QUEUE) {
        size_t copyLen = (len < MAX_BYTES - 1) ? len : (MAX_BYTES - 1);
        memcpy(g_queue[g_qTail].code, code, copyLen);
        g_queue[g_qTail].code[copyLen] = '\0';
        g_queue[g_qTail].queuedAt = GetTickCount();
        g_qTail = (g_qTail + 1) % QUEUE;
        g_qCount++;
        ok = true;
    }
    LeaveCriticalSection(&g_queueLock);
    return ok;
}

// Pops one entry; returns false if empty. Skips (drops) stale entries.
bool QueuePop(char* out, size_t outSize) {
    bool got = false;
    EnterCriticalSection(&g_queueLock);
    while (g_qCount > 0) {
        QueuedExec* e = &g_queue[g_qHead];
        g_qHead = (g_qHead + 1) % QUEUE;
        g_qCount--;

        DWORD age = GetTickCount() - e->queuedAt;
        if (age > EXPIRY) {
            InterlockedIncrement(&g_statRejected);
            continue; // drop stale, keep looking
        }
        strncpy(out, e->code, outSize - 1);
        out[outSize - 1] = '\0';
        got = true;
        break;
    }
    LeaveCriticalSection(&g_queueLock);
    return got;
}

int QueueLen(void) {
    EnterCriticalSection(&g_queueLock);
    int n = g_qCount;
    LeaveCriticalSection(&g_queueLock);
    return n;
}

// ================= VALIDATION / RATE LIMIT =================
bool ValidateCode(const char* code, size_t len) {
    if (!code) return false;
    if (len == 0 || len >= MAX_BYTES) return false;
    // reject embedded NUL (memchr since we know length from Content-Length)
    if (memchr(code, '\0', len)) return false;
    return true;
}

static DWORD g_lastAcceptedAt = 0;

bool RateLimitOK(void) {
    DWORD now = GetTickCount();
    if (now - g_lastAcceptedAt < THROTTLE) return false;
    g_lastAcceptedAt = now;
    return true;
}

// Called only from HTTP thread. Never touches Lua directly.
int SubmitExecution(const char* code, size_t len) {
    if (!g_hookOK) return 503; // executor not ready, refuse rather than risk anything
    if (!ValidateCode(code, len)) return 400;
    if (!RateLimitOK()) return 429;
    if (!QueuePush(code, len)) return 429; // queue full
    return 200;
}

// ================= MAIN-THREAD EXECUTION (the actual fix) =================
// This is the ONLY place RunStringEx is ever called, and it only ever runs
// from inside the engine's own frame callback -- i.e. the game's main thread.

void SafeRunLua(const char* code) {
    EnterCriticalSection(&g_luaLock);

    if (!g_luaInterface || !g_RunStringEx ||
        !IsInterfaceSane(g_luaInterface) ||
        !IsCodePointerValid((void*)g_RunStringEx)) {
        LeaveCriticalSection(&g_luaLock);
        InterlockedIncrement(&g_statErrors);
        RefreshLuaInterface();
        return;
    }

    g_jmpArmed = true;
    if (setjmp(g_jmpBuf) == 0) {
        g_RunStringEx(g_luaInterface, "GmExec", "", code, 1, 0, 0, 0);
        g_jmpArmed = false;
        InterlockedIncrement(&g_statExecuted);
    } else {
        // We longjmp'd back here from the VEH after a fatal exception.
        LogMsg("!! Caught fatal exception during Lua execution, recovered.");
        InterlockedIncrement(&g_statErrors);
        g_luaInterface = NULL;
        g_RunStringEx  = NULL;
    }

    LeaveCriticalSection(&g_luaLock);
}

void DrainQueueOnMainThread(void) {
    LARGE_INTEGER freq, start, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);

    int executed = 0;
    char code[MAX_BYTES];

    while (executed < FPS_LIMIT && QueuePop(code, sizeof(code))) {
        SafeRunLua(code);
        executed++;

        QueryPerformanceCounter(&now);
        double elapsedMs = (double)(now.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart;
        if (elapsedMs >= FPS_BUDGET) break; // don't hitch the frame
    }
}

// ================= ENGINE HOOK (runs the drain on the main thread) =================
void HookedFrameStageNotify(void* thisptr, int stage) {
    if (IsCodePointerValid((void*)g_origFrameStageNotify)) {
        g_origFrameStageNotify(thisptr, stage);
    }
    DrainQueueOnMainThread();
}

void* HookVTableSlot(void* instance, int index, void* newFunc) {
    if (!IsInterfaceSane(instance)) return NULL;
    void** vtable = *(void***)instance;

    if (!IsMemoryReadable(&vtable[index], sizeof(void*))) return NULL;
    void* orig = vtable[index];
    if (!IsCodePointerValid(orig)) return NULL; // slot doesn't look like a real function

    DWORD oldProtect;
    if (!VirtualProtect(&vtable[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect))
        return NULL;

    vtable[index] = newFunc;

    DWORD tmp;
    VirtualProtect(&vtable[index], sizeof(void*), oldProtect, &tmp);
    return orig;
}

bool InstallFrameHook(void) {
    if (g_hookOK) return true;

    g_clientDLLInterface = GetInterfaceAnyVersion("client.dll", "VClient", 14, 20);
    if (!g_clientDLLInterface) {
        LogMsg("Could not resolve client.dll interface.");
        return false;
    }

    void* orig = HookVTableSlot(g_clientDLLInterface, HOOK_ID,
                                 (void*)HookedFrameStageNotify);
    if (!orig) {
        LogMsg("Failed to hook FrameStageNotify (bad index? verify HOOK_ID).");
        return false;
    }

    g_origFrameStageNotify = (FrameStageNotify_t)orig;
    g_hookOK = true;
    LogMsg("Frame hook installed. Main-thread execution active.");
    return true;
}

// ================= INTERFACE WATCHER THREAD =================
DWORD WINAPI InterfaceWatcher(LPVOID lpParam) {
    while (g_running) {
        RefreshLuaInterface();

        if (!g_hookOK) {
            InstallFrameHook();
        }

        Sleep(250);
    }
    return 0;
}

// ================= HTTP HELPERS =================
static void SendResponse(SOCKET s, int code, const char* codeText) {
    char resp[256];
    sprintf(resp,
        "HTTP/1.1 %d %s\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n\r\n",
        code, codeText);
    send(s, resp, (int)strlen(resp), 0);
}

// Reads a full HTTP request (headers + body) with a hard size cap and timeout,
// instead of trusting a single recv() call to contain everything.
static bool RecvFullRequest(SOCKET s, char* buf, size_t bufSize, int* outLen) {
    int total = 0;
    int headerEnd = -1;
    int contentLength = -1;

    while (total < (int)bufSize - 1) {
        int n = recv(s, buf + total, (int)bufSize - 1 - total, 0);
        if (n <= 0) break;
        total += n;
        buf[total] = '\0';

        if (headerEnd < 0) {
            char* p = strstr(buf, "\r\n\r\n");
            if (p) {
                headerEnd = (int)(p - buf) + 4;
                char* cl = strstr(buf, "Content-Length:");
                if (!cl) cl = strstr(buf, "content-length:");
                if (cl) contentLength = atoi(cl + 15);

                if (contentLength < 0) contentLength = 0;
                if (contentLength > MAX_BYTES) {
                    *outLen = total;
                    return false; // caller should respond 413
                }
            }
        }

        if (headerEnd >= 0) {
            int bodyHave = total - headerEnd;
            if (bodyHave >= contentLength) break; // got everything we need
        }
    }

    *outLen = total;
    return true;
}

static bool CheckAuthToken(const char* req) {
#if REQUIRE_AUTH_TOKEN
    const char* hdr = strstr(req, "X-Auth-Token:");
    if (!hdr) hdr = strstr(req, "x-auth-token:");
    if (!hdr) return false;
    hdr += 13;
    while (*hdr == ' ') hdr++;
    return strncmp(hdr, AUTH_TOKEN, strlen(AUTH_TOKEN)) == 0;
#else
    (void)req;
    return true;
#endif
}

// ================= HTTP SERVER THREAD =================
DWORD WINAPI HTTPServer(LPVOID lpParam) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        LogMsg("WSAStartup failed.");
        return 1;
    }

    g_listenSock = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listenSock == INVALID_SOCKET) {
        LogMsg("socket() failed.");
        WSACleanup();
        return 1;
    }

    int reuse = 1;
    setsockopt(g_listenSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(ADDRESS); // localhost only
    addr.sin_port = htons(PORT);

    if (bind(g_listenSock, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        LogMsg("bind() failed (port in use?).");
        closesocket(g_listenSock);
        WSACleanup();
        return 1;
    }

    if (listen(g_listenSock, SOMAXCONN) == SOCKET_ERROR) {
        LogMsg("listen() failed.");
        closesocket(g_listenSock);
        WSACleanup();
        return 1;
    }

    LogMsg("HTTP server listening on http://%s:%d", ADDRESS, PORT);

    while (g_running) {
        SOCKET client = accept(g_listenSock, NULL, NULL);
        if (client == INVALID_SOCKET) {
            if (!g_running) break;
            continue;
        }

        DWORD timeout = RECV_WAIT;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
        timeout = SEND_WAIT;
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));

        static __declspec(thread) char buffer[65536];
        int bytes = 0;
        bool okSize = RecvFullRequest(client, buffer, sizeof(buffer), &bytes);

        if (!okSize) {
            SendResponse(client, 413, "Payload Too Large");
            closesocket(client);
            continue;
        }

        if (bytes <= 0) {
            closesocket(client);
            continue;
        }

        if (strncmp(buffer, "GET / ", 6) == 0 || strncmp(buffer, "GET /index", 10) == 0) {
            char header[512];
            sprintf(header,
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/html; charset=utf-8\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Content-Length: %d\r\n"
                "Connection: close\r\n\r\n",
                (int)strlen(HTML_PAGE));
            send(client, header, (int)strlen(header), 0);
            send(client, HTML_PAGE, (int)strlen(HTML_PAGE), 0);
        }
        else if (strncmp(buffer, "GET /status", 11) == 0) {
            char json[512];
            sprintf(json,
                "{\"hook\":%s,\"queue\":%d,\"executed\":%ld,\"errors\":%ld,\"rejected\":%ld,\"crashesCaught\":%ld}",
                g_hookOK ? "true" : "false", QueueLen(),
                g_statExecuted, g_statErrors, g_statRejected, g_statCrashesCaught);
            char header[256];
            sprintf(header,
                "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                "Access-Control-Allow-Origin: *\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
                (int)strlen(json));
            send(client, header, (int)strlen(header), 0);
            send(client, json, (int)strlen(json), 0);
        }
        else if (strncmp(buffer, "POST /execute", 13) == 0) {
            if (!CheckAuthToken(buffer)) {
                SendResponse(client, 401, "Unauthorized");
            } else {
                char* bodyStart = strstr(buffer, "\r\n\r\n");
                if (!bodyStart) {
                    SendResponse(client, 400, "Bad Request");
                } else {
                    bodyStart += 4;
                    size_t bodyLen = (size_t)(bytes - (bodyStart - buffer));
                    int status = SubmitExecution(bodyStart, bodyLen);
                    switch (status) {
                        case 200: SendResponse(client, 200, "OK"); break;
                        case 400: SendResponse(client, 400, "Bad Request"); break;
                        case 429: SendResponse(client, 429, "Too Many Requests"); break;
                        case 503: SendResponse(client, 503, "Executor Not Ready"); break;
                        default:  SendResponse(client, 500, "Internal Error"); break;
                    }
                }
            }
        }
        else {
            SendResponse(client, 404, "Not Found");
        }

        shutdown(client, SD_BOTH);
        closesocket(client);
    }

    closesocket(g_listenSock);
    g_listenSock = INVALID_SOCKET;
    WSACleanup();
    return 0;
}

// ================= MAIN THREAD =================
DWORD WINAPI MainThread(LPVOID lpParam) {
    Sleep(3000); // let the game finish loading its modules

    InitializeCriticalSection(&g_luaLock);
    InitializeCriticalSection(&g_queueLock);
    InitializeCriticalSection(&g_logLock);

    AddVectoredExceptionHandler(1, CrashGuardVEH);

    if (CONSOLE && AllocConsole()) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONIN$", "r", stdin);
        SetConsoleTitleA("GmExec");
        LogMsg("GmExec starting...");
        LogMsg("Bind: %s:%d  MaxCode:%d  QueueCap:%d", ADDRESS, PORT,
               MAX_BYTES, QUEUE);
    }

    g_luaShared = GetInterface("lua_shared.dll", "LUASHARED003");
    if (!g_luaShared) {
        LogMsg("FATAL: could not get LUASHARED003.");
        return 1;
    }

    for (int i = 0; i < 100 && !g_luaInterface; i++) {
        RefreshLuaInterface();
        if (!g_luaInterface) Sleep(100);
    }

    if (!g_luaInterface) {
        LogMsg("FATAL: could not resolve Lua interface after 10s.");
        return 1;
    }

    LogMsg("Lua interface acquired.");

    // Try to install the frame hook immediately; watcher thread will retry
    // if this fails (e.g. client.dll not loaded yet in this branch order).
    InstallFrameHook();

    CreateThread(NULL, 0, InterfaceWatcher, NULL, 0, NULL);
    CreateThread(NULL, 0, HTTPServer, NULL, 0, NULL);

    LogMsg("Startup complete.");
    return 0;
}

// ================= DLL ENTRY =================
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hModule);
            CreateThread(NULL, 0, MainThread, NULL, 0, NULL);
            break;
        case DLL_PROCESS_DETACH:
            g_running = false;
            if (g_listenSock != INVALID_SOCKET) {
                closesocket(g_listenSock);
            }
            break;
    }
    return TRUE;
}