// =====================================================================================
//  MEC Coop - journal des plantages : si le jeu plante, log_overlay.txt dit où (module + décalage)
//  et quels modules étaient sur la pile. Sert à voir qui plante quand deux mods sont chargés (GridNode, 25/10).
//  Gestionnaire « vectoriel » : il ne fait que noter, le jeu continue comme s'il n'était pas là.
// =====================================================================================
#include "game.h"
#include <stdio.h>

namespace game {
namespace crashlog {

static volatile LONG g_any = 0, g_foreign = 0;

static bool moduleOf(uintptr_t a, char* name, size_t cap, uintptr_t* base) {
    HMODULE h = NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)a, &h) || !h)
        return false;
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(h, path, sizeof(path));
    const char* s = path;
    for (DWORD i = 0; i < n; i++)
        if (path[i] == '\\' || path[i] == '/') s = path + i + 1;
    lstrcpynA(name, n ? s : "?", (int)cap);
    *base = (uintptr_t)h;
    return true;
}

static void where(uintptr_t a, char* out, size_t cap) {
    char m[64];
    uintptr_t b = 0;
    if (moduleOf(a, m, sizeof(m), &b)) {
        if (b == moduleBase()) _snprintf(out, cap, "jeu+%llx (VA %llx)", (unsigned long long)(a - b), (unsigned long long)(a - b + 0x140000000ull));
        else _snprintf(out, cap, "%s+%llx", m, (unsigned long long)(a - b));
    } else {
        _snprintf(out, cap, "%llx (hors module)", (unsigned long long)a);
    }
}

static LONG CALLBACK handler(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW &&
        code != EXCEPTION_PRIV_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != 0xC0000409)
        return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t rip = (uintptr_t)ep->ContextRecord->Rip;
    char m[64];
    uintptr_t b = 0;
    bool inGame = moduleOf(rip, m, sizeof(m), &b) && b == moduleBase();
    // le jeu peut avoir des erreurs « normales » rattrapées : on en note peu, et plus si elles viennent d'ailleurs
    if (inGame ? InterlockedIncrement(&g_any) > 4 : InterlockedIncrement(&g_foreign) > 8) return EXCEPTION_CONTINUE_SEARCH;
    char w[160];
    where(rip, w, sizeof(w));
    uintptr_t addr = ep->ExceptionRecord->NumberParameters >= 2 ? (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1] : 0;
    logf("ERREUR %08lx dans %s (adresse lue/écrite %llx, fil %lu)", code, w, (unsigned long long)addr, GetCurrentThreadId());
    // pile : les valeurs qui pointent dans du code (adresses de retour probables)
    uintptr_t rsp = (uintptr_t)ep->ContextRecord->Rsp;
    char line[600];
    int len = 0, shown = 0;
    for (int i = 0; i < 96 && shown < 10; i++) {
        uintptr_t v = 0;
        if (!read(rsp + i * 8, v) || v < 0x10000) continue;
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void*)v, &mbi, sizeof(mbi)) || mbi.Type != MEM_IMAGE ||
            !(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
            continue;
        char f[160];
        where(v, f, sizeof(f));
        int k = lstrlenA(f);
        if (len + k + 3 >= (int)sizeof(line)) break;
        if (len) { line[len++] = ','; line[len++] = ' '; }
        memcpy(line + len, f, k);
        len += k;
        shown++;
    }
    line[len] = 0;
    if (len) logf("  pile : %s", line);
    return EXCEPTION_CONTINUE_SEARCH;
}

void install() { AddVectoredExceptionHandler(1, handler); }

}  // namespace crashlog
}  // namespace game
