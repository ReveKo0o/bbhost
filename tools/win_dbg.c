// A minimal Windows debugger for runs that end without a word: starts a
// program under the Win32 debug API and logs what the debuggee's own
// handlers never see or never get to report - every exception as the system
// raises it (first chance, then last chance), with the thread, the faulting
// address, the registers, the module each address lies in and the return
// addresses among the stack words; the modules as they load; the debug
// strings drivers print; the exit code.
//
//   win_dbg.exe [--all] LOG COMMAND LINE...
//
// Without --all, first-chance access violations after the first 20 are left
// out (the write watch takes thousands of them on purpose). Build:
//   x86_64-w64-mingw32-gcc -O2 -o build/win/win_dbg.exe tools/win_dbg.c -lpsapi
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

typedef struct {
    uint64_t base, size;
    char name[128];
} Module;

static Module g_mods[512];
static int g_nmods = 0;
static FILE* g_log;

static void add_module(HANDLE file, void* base) {
    if (g_nmods >= 512) return;
    Module* m = &g_mods[g_nmods];
    m->base = (uint64_t)(uintptr_t)base;
    m->size = 0;
    strcpy(m->name, "?");
    if (file) {
        wchar_t path[MAX_PATH];
        const DWORD n = GetFinalPathNameByHandleW(file, path, MAX_PATH, 0);
        if (n && n < MAX_PATH) {
            const wchar_t* s = wcsrchr(path, L'\\');
            WideCharToMultiByte(CP_ACP, 0, s ? s + 1 : path, -1, m->name, sizeof(m->name), NULL, NULL);
        }
    }
    ++g_nmods;
}

static void size_modules(HANDLE process) {
    for (int i = 0; i < g_nmods; ++i) {
        if (g_mods[i].size) continue;
        IMAGE_DOS_HEADER dos;
        IMAGE_NT_HEADERS64 nt;
        SIZE_T got = 0;
        if (ReadProcessMemory(process, (void*)(uintptr_t)g_mods[i].base, &dos, sizeof(dos), &got) && dos.e_magic == IMAGE_DOS_SIGNATURE &&
            ReadProcessMemory(process, (void*)(uintptr_t)(g_mods[i].base + dos.e_lfanew), &nt, sizeof(nt), &got) &&
            nt.Signature == IMAGE_NT_SIGNATURE) {
            g_mods[i].size = nt.OptionalHeader.SizeOfImage;
        }
    }
}

static const Module* module_of(uint64_t a) {
    for (int i = 0; i < g_nmods; ++i) {
        if (a >= g_mods[i].base && a < g_mods[i].base + g_mods[i].size) return &g_mods[i];
    }
    return NULL;
}

static void where(uint64_t a, char* out, size_t cap) {
    const Module* m = module_of(a);
    if (m) {
        snprintf(out, cap, "%s+0x%llx", m->name, (unsigned long long)(a - m->base));
    } else {
        snprintf(out, cap, "0x%llx", (unsigned long long)a);
    }
}

static void report(HANDLE process, DWORD tid, const EXCEPTION_DEBUG_INFO* ex) {
    const EXCEPTION_RECORD* r = &ex->ExceptionRecord;
    char at[200];
    size_modules(process);
    where((uint64_t)(uintptr_t)r->ExceptionAddress, at, sizeof(at));
    fprintf(g_log, "exception 0x%08lx (%s chance) thread %lu at %s", (unsigned long)r->ExceptionCode,
            ex->dwFirstChance ? "first" : "LAST", (unsigned long)tid, at);
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
        fprintf(g_log, " (%s 0x%llx)", r->ExceptionInformation[0] == 1 ? "write" : r->ExceptionInformation[0] == 8 ? "execute" : "read",
                (unsigned long long)r->ExceptionInformation[1]);
    }
    fprintf(g_log, "\n");
    HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!th) return;
    CONTEXT c;
    memset(&c, 0, sizeof(c));
    c.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(th, &c)) {
        fprintf(g_log, "  rip=%llx rsp=%llx rbp=%llx rax=%llx rbx=%llx rcx=%llx rdx=%llx rsi=%llx rdi=%llx\n", (unsigned long long)c.Rip,
                (unsigned long long)c.Rsp, (unsigned long long)c.Rbp, (unsigned long long)c.Rax, (unsigned long long)c.Rbx,
                (unsigned long long)c.Rcx, (unsigned long long)c.Rdx, (unsigned long long)c.Rsi, (unsigned long long)c.Rdi);
        fprintf(g_log, "  r8=%llx r9=%llx r10=%llx r11=%llx r12=%llx r13=%llx r14=%llx r15=%llx\n", (unsigned long long)c.R8,
                (unsigned long long)c.R9, (unsigned long long)c.R10, (unsigned long long)c.R11, (unsigned long long)c.R12,
                (unsigned long long)c.R13, (unsigned long long)c.R14, (unsigned long long)c.R15);
        uint64_t words[256];
        SIZE_T got = 0;
        if (ReadProcessMemory(process, (void*)(uintptr_t)c.Rsp, words, sizeof(words), &got)) {
            int shown = 0;
            for (size_t k = 0; k < got / 8 && shown < 24; ++k) {
                if (module_of(words[k])) {
                    char w[200];
                    where(words[k], w, sizeof(w));
                    fprintf(g_log, "  [rsp+0x%zx] %s\n", k * 8, w);
                    ++shown;
                }
            }
        } else {
            fprintf(g_log, "  the stack at rsp cannot be read (error %lu)\n", (unsigned long)GetLastError());
        }
    }
    CloseHandle(th);
    fflush(g_log);
}

int main(int argc, char** argv) {
    int all = 0, a = 1;
    if (a < argc && strcmp(argv[a], "--all") == 0) all = 1, ++a;
    if (argc - a < 2) {
        fprintf(stderr, "usage: win_dbg.exe [--all] LOG COMMAND LINE...\n");
        return 2;
    }
    g_log = fopen(argv[a], "w");
    if (!g_log) return 2;
    ++a;
    // The command line as given, after LOG.
    const char* full = GetCommandLineA();
    const char* p = strstr(full, argv[a - 1]);
    p = p ? p + strlen(argv[a - 1]) : full;
    while (*p == ' ' || *p == '"') ++p;
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "%s", p);
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, DEBUG_ONLY_THIS_PROCESS, NULL, NULL, &si, &pi)) {
        fprintf(g_log, "CreateProcess failed: %lu (%s)\n", (unsigned long)GetLastError(), cmd);
        return 2;
    }
    fprintf(g_log, "started pid %lu: %s\n", (unsigned long)pi.dwProcessId, cmd);
    fflush(g_log);
    int av_first = 0, others = 0;
    DWORD exit_code = 0;
    for (;;) {
        DEBUG_EVENT ev;
        if (!WaitForDebugEvent(&ev, INFINITE)) break;
        DWORD cont = DBG_CONTINUE;
        switch (ev.dwDebugEventCode) {
        case CREATE_PROCESS_DEBUG_EVENT:
            add_module(ev.u.CreateProcessInfo.hFile, ev.u.CreateProcessInfo.lpBaseOfImage);
            if (ev.u.CreateProcessInfo.hFile) CloseHandle(ev.u.CreateProcessInfo.hFile);
            break;
        case LOAD_DLL_DEBUG_EVENT:
            add_module(ev.u.LoadDll.hFile, ev.u.LoadDll.lpBaseOfDll);
            if (g_nmods) fprintf(g_log, "load %s at 0x%llx\n", g_mods[g_nmods - 1].name, (unsigned long long)g_mods[g_nmods - 1].base);
            if (ev.u.LoadDll.hFile) CloseHandle(ev.u.LoadDll.hFile);
            break;
        case OUTPUT_DEBUG_STRING_EVENT: {
            char buf[1024];
            SIZE_T got = 0;
            const DWORD n = ev.u.DebugString.nDebugStringLength;
            if (!ev.u.DebugString.fUnicode && n && ReadProcessMemory(pi.hProcess, ev.u.DebugString.lpDebugStringData, buf,
                                                                    n < sizeof(buf) ? n : sizeof(buf) - 1, &got)) {
                buf[got < sizeof(buf) ? got : sizeof(buf) - 1] = 0;
                fprintf(g_log, "debug string (thread %lu): %s%s", (unsigned long)ev.dwThreadId, buf,
                        got && buf[got - 1] == '\n' ? "" : "\n");
            }
            break;
        }
        case EXCEPTION_DEBUG_EVENT: {
            const EXCEPTION_DEBUG_INFO* ex = &ev.u.Exception;
            const DWORD code = ex->ExceptionRecord.ExceptionCode;
            if (code == EXCEPTION_BREAKPOINT && others == 0) {
                ++others;  // the loader's breakpoint
                break;
            }
            cont = DBG_EXCEPTION_NOT_HANDLED;
            const int is_av = code == EXCEPTION_ACCESS_VIOLATION;
            if (!ex->dwFirstChance || all || (is_av ? av_first++ < 20 : others++ < 200)) report(pi.hProcess, ev.dwThreadId, ex);
            break;
        }
        case EXIT_PROCESS_DEBUG_EVENT:
            exit_code = ev.u.ExitProcess.dwExitCode;
            fprintf(g_log, "exit 0x%08lx (%ld); first-chance access violations %d\n", (unsigned long)exit_code, (long)exit_code, av_first);
            fclose(g_log);
            ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, DBG_CONTINUE);
            return (int)exit_code;
        default:
            break;
        }
        ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, cont);
    }
    return 0;
}
