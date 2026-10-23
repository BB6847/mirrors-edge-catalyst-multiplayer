// =====================================================================================
//  MEC Coop - combats partagés (v1) : quand l'un frappe un ennemi, l'ennemi correspondant chez
//  l'autre perd la même santé ; mis K.O. chez l'un = K.O. chez l'autre.
//
//  Ce que le jeu fait (lu dans son code) :
//    PamServerHealthComponent (vtable 0x141c401d0)
//      case 0x100 = applyDamage(composant, résultat*, PamServerDamageInfo*) : dégâts à +0x88 de l'info
//      case 0xf0  = setHealth(composant, float) : écrit la santé (+0x20), et si <= 0 appelle la mort (case 0x130)
//      +0x20 santé, +0x24 santé max ; le personnage : [[composant+0x08]]
//  Chez moi : la case 0x100 est remplacée par la nôtre, qui appelle celle du jeu puis note le coup
//  (position de l'ennemi, santé restante). Chez l'autre : l'ennemi le plus proche de cette position reçoit
//  la même santé par setHealth (la mort du jeu fait le reste). Santé = la plus basse des deux : pas d'écho.
// =====================================================================================
#include "game.h"
#include <stdio.h>

namespace game {
namespace combat {

static const uintptr_t Vtbl_PamServerHealthComponent = 0x141c401d0, Fn_PamApplyDamage = 0x143923a90;
static const uintptr_t TI_PamServerHealthComponent = 0x142853f60;
// les ennemis ont leur propre santé (PamServerAIHealthComponent, test du 19/10 : aucun coup vu avec la seule
// santé de base) ; sa case 0x100 (0x143995fa0) prévient l'IA puis appelle directement celle de base.
static const uintptr_t Vtbl_PamServerAIHealthComponent = 0x141c56398, Fn_PamAIApplyDamage = 0x143995fa0;
static const uintptr_t TI_PamServerAIHealthComponent = 0x14285adc0;
static const size_t Slot_ApplyDamage = 0x100, Slot_SetHealth = 0xf0;
static const size_t Health_Value = 0x20, Health_Max = 0x24, DamageInfo_Amount = 0x88;
static const float kMatchRadius = 10.f;
static const DWORD kPendingMs = 5000, kScanEveryMs = 3000;

typedef void* (*ApplyFn)(void* comp, void* out, void* info);
static ApplyFn g_orig = NULL, g_origAI = NULL;
static volatile bool g_installed = false;
static Lock g_m;

static Event g_out[32];
static int g_outHead = 0, g_outFill = 0;
static volatile LONG g_sent = 0, g_applied = 0, g_missed = 0;

// les spawners d'ennemis du niveau (pas Faith, pas les fantômes, pas les passants)
static void* g_spawners[256];
static int g_nsp = 0;

void init() { g_m.init(); }

void noteSpawner(const char* blueprint, void* spawner) {
    if (!strncmp(blueprint, kPlayerBlueprint, 64) || !strcmp(blueprint, kGhostBlueprint)) return;
    if (strstr(blueprint, "AINpc") || strstr(blueprint, "AICitizen") || strstr(blueprint, "Projection")) return;
    Guard g(g_m);
    for (int i = 0; i < g_nsp; i++) if (g_spawners[i] == spawner) return;
    if (g_nsp >= 256) { memmove(g_spawners, g_spawners + 1, 255 * sizeof(void*)); g_nsp = 255; }
    g_spawners[g_nsp++] = spawner;
}

void levelChanged() {
    // les spawners du nouveau niveau sont souvent créés AVANT le premier tick du niveau : on ne vide pas
    // la liste (on la perdrait), on retire seulement ceux qui n'existent plus
    Guard g(g_m);
    int k = 0;
    for (int i = 0; i < g_nsp; i++)
        if (level::spawnerAlive(g_spawners[i])) g_spawners[k++] = g_spawners[i];
    g_nsp = k;
    g_outFill = 0;
}

static uintptr_t ownerOf(uintptr_t comp) {
    uintptr_t coll = readOr<uintptr_t>(comp + Component_Collection);
    return coll ? readOr<uintptr_t>(coll) : 0;
}

static void note(void* comp, void* info, float before, bool ai);
static void* hook(void* comp, void* out, void* info) {
    float before = readOr<float>((uintptr_t)comp + Health_Value, -1.f);
    void* r = g_orig(comp, out, info);
    note(comp, info, before, false);
    return r;
}
static void* hookAI(void* comp, void* out, void* info) {
    float before = readOr<float>((uintptr_t)comp + Health_Value, -1.f);
    void* r = g_origAI(comp, out, info);
    note(comp, info, before, true);
    return r;
}
static bool isHealthComp(uintptr_t comp) {
    uintptr_t vt = readOr<uintptr_t>(comp);
    return vt == rva(Vtbl_PamServerAIHealthComponent) || vt == rva(Vtbl_PamServerHealthComponent);
}

static volatile LONG g_seen = 0;
static void note(void* comp, void* info, float before, bool ai) {
    DWORD err = GetLastError();
    uintptr_t owner = ownerOf((uintptr_t)comp);
    float after = readOr<float>((uintptr_t)comp + Health_Value, -1.f);
    if (InterlockedIncrement(&g_seen) <= 25)   // diagnostic : tous les coups, ennemis ou non
        logf("combat : dégâts (%s) sur vt=%llx joueur=%d santé %.1f -> %.1f, dégâts lus %.1f", ai ? "ennemi" : "base",
             (unsigned long long)(owner ? readOr<uintptr_t>(owner) - moduleBase() + 0x140000000ull : 0),
             owner ? (readOr<uintptr_t>(owner + Character_Player) != 0) : -1, before, after,
             info ? readOr<float>((uintptr_t)info + DamageInfo_Amount, 0.f) : 0.f);
    if (owner && readOr<uintptr_t>(owner) == rva(Vtbl_PamServerCharacterEntity) &&
        !readOr<uintptr_t>(owner + Character_Player) && !ghosts::isGhostCharacter((void*)owner)) {
        LinearTransform xf;
        Event e;
        memset(&e, 0, sizeof(e));
        e.health = readOr<float>((uintptr_t)comp + Health_Value, -1.f);
        e.maxHealth = readOr<float>((uintptr_t)comp + Health_Max, 0.f);
        e.damage = info ? readOr<float>((uintptr_t)info + DamageInfo_Amount, 0.f) : 0.f;
        if (readEntityTransform((void*)owner, xf) && finite(e.health) && finite(before) && e.health < before) {
            e.damage = before - e.health;
            memcpy(e.pos, xf.trans, 12);
            Guard g(g_m);
            g_out[g_outHead] = e;
            g_outHead = (g_outHead + 1) % 32;
            if (g_outFill < 32) g_outFill++;
        }
    }
    SetLastError(err);
}

// ---- gardes introuvables par leurs spawners (créés avant l'overlay, rencontres, renforts) : 23/10, chez
// l'autre 0 à 2 ennemis connus pendant un combat. Un fil à part parcourt la mémoire du jeu (lecture seule)
// à la recherche des personnages du serveur ; le tour du jeu ne fait que vérifier la liste qu'il rend.
static void* g_found[512];
static int g_nfound = 0;
static volatile LONG g_scanWanted = 0;
static volatile DWORD g_scanDoneAt = 0;
static volatile LONG g_scans = 0;

static DWORD WINAPI ScanThread(void*) {
    const size_t kBuf = 4 << 20;
    uint8_t* buf = (uint8_t*)VirtualAlloc(NULL, kBuf, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    static void* found[512];
    if (!buf) return 0;
    for (;;) {
        if (!InterlockedExchange(&g_scanWanted, 0)) { Sleep(100); continue; }
        const uintptr_t v = rva(Vtbl_PamServerCharacterEntity);
        DWORD t0 = GetTickCount();
        int n = 0;
        MEMORY_BASIC_INFORMATION mbi;
        for (uintptr_t addr = 0x10000; n < 512 && VirtualQuery((void*)addr, &mbi, sizeof(mbi)) == sizeof(mbi);
             addr = (uintptr_t)mbi.BaseAddress + mbi.RegionSize) {
            if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE || mbi.Protect != PAGE_READWRITE) continue;
            uintptr_t base = (uintptr_t)mbi.BaseAddress;
            if ((uint8_t*)base == buf) continue;
            for (size_t off = 0; off < mbi.RegionSize && n < 512; off += kBuf) {
                size_t len = mbi.RegionSize - off < kBuf ? mbi.RegionSize - off : kBuf;
                if (!read(base + off, buf, len)) continue;
                const uintptr_t* q = (const uintptr_t*)buf;
                for (size_t i = 0; i < len / 8 && n < 512; i += 2)
                    if (q[i] == v) found[n++] = (void*)(base + off + i * 8);
            }
        }
        {
            Guard g(g_m);
            memcpy(g_found, found, n * sizeof(void*));
            g_nfound = n;
        }
        g_scanDoneAt = GetTickCount() | 1;
        if (InterlockedIncrement(&g_scans) <= 10)
            logf("combat : recherche des gardes : %d personnages trouvés en %lu ms", n, GetTickCount() - t0);
        Sleep(kScanEveryMs);
    }
}

bool install() {
    if (g_installed) return true;
    uintptr_t slot = rva(Vtbl_PamServerHealthComponent) + Slot_ApplyDamage;
    if (readOr<uintptr_t>(slot) != rva(Fn_PamApplyDamage)) {
        logf("combat : fonction de dégâts inattendue, combats partagés désactivés");
        return false;
    }
    uintptr_t slotAI = rva(Vtbl_PamServerAIHealthComponent) + Slot_ApplyDamage;
    if (readOr<uintptr_t>(slotAI) != rva(Fn_PamAIApplyDamage)) {
        logf("combat : fonction de dégâts des ennemis inattendue, combats partagés désactivés");
        return false;
    }
    g_orig = (ApplyFn)rva(Fn_PamApplyDamage);
    g_origAI = (ApplyFn)rva(Fn_PamAIApplyDamage);
    if (!patchPointer(slotAI, (uintptr_t)&hookAI, NULL)) return false;
    patchPointer(slot, (uintptr_t)&hook, NULL);
    g_installed = true;
    HANDLE th = CreateThread(NULL, 0, ScanThread, NULL, 0, NULL);
    if (th) CloseHandle(th);
    logf("combat : coups sur les ennemis suivis (santé des ennemis et santé de base)");
    return true;
}

// les ennemis vivants du niveau (personnages des spawners d'ennemis)
static int enemies(void** out, int cap) {
    void* sps[256];
    int n;
    {
        Guard g(g_m);
        n = g_nsp;
        memcpy(sps, g_spawners, n * sizeof(void*));
    }
    int k = 0;
    for (int i = 0; i < n && k < cap; i++) {
        if (!level::spawnerAlive(sps[i])) continue;
        uintptr_t head[Spawner_Spawned / 8 + 2];
        if (!read((uintptr_t)sps[i], head, sizeof(head))) continue;
        uintptr_t b = head[Spawner_Spawned / 8], e = head[Spawner_Spawned / 8 + 1];
        if (!b || e <= b || (e - b) / 8 > 64) continue;
        uintptr_t items[64];
        size_t cnt = (e - b) / 8;
        if (!read(b, items, cnt * 8)) continue;
        for (size_t j = 0; j < cnt && k < cap; j++)
            if (items[j] && readOr<uintptr_t>(items[j]) == rva(Vtbl_PamServerCharacterEntity)) out[k++] = (void*)items[j];
    }
    void* found[512];
    int nf;
    {
        Guard g(g_m);
        nf = g_nfound;
        memcpy(found, g_found, nf * sizeof(void*));
    }
    for (int i = 0; i < nf && k < cap; i++) {
        uintptr_t c = (uintptr_t)found[i];
        if (readOr<uintptr_t>(c) != rva(Vtbl_PamServerCharacterEntity) || readOr<uintptr_t>(c + Character_Player) ||
            ghosts::isGhostCharacter(found[i]))
            continue;
        bool dup = false;
        for (int j = 0; j < k && !dup; j++) dup = out[j] == found[i];
        if (!dup) out[k++] = found[i];
    }
    return k;
}

struct Pending { Event e; DWORD at; };
static Pending g_pending[32];
static int g_npending = 0;

// thread du jeu (serveur) : mes coups -> coop ; les coups des autres -> mes ennemis
void tick() {
    if (!g_installed) return;
    Event out[32];
    int n = 0;
    {
        Guard g(g_m);
        for (int k = g_outFill; k > 0; k--) out[n++] = g_out[(g_outHead - k + 32) % 32];
        g_outFill = 0;
    }
    for (int i = 0; i < n; i++) {
        publish(out[i]);
        LONG c = InterlockedIncrement(&g_sent);
        if (c <= 20)
            logf("combat : coup sur un ennemi à %.1f %.1f %.1f (dégâts %.1f, santé %.1f / %.1f)", out[i].pos[0], out[i].pos[1],
                 out[i].pos[2], out[i].damage, out[i].health, out[i].maxHealth);
    }
    Event got[32];
    int ng = take(got, 32);
    if (!ng && !g_npending) return;
    DWORD now = GetTickCount();
    // les coups pas encore placés (on attend la recherche des gardes), puis les nouveaux
    Pending work[64];
    int m = 0;
    for (int i = 0; i < g_npending; i++) work[m++] = g_pending[i];
    for (int i = 0; i < ng && m < 64; i++) { work[m].e = got[i]; work[m].at = now; m++; }
    g_npending = 0;
    static void* chars[640];
    int nc = enemies(chars, 640);
    for (int i = 0; i < m; i++) {
        const Event& e = work[i].e;
        if (!finite(e.health) || !finite(e.pos[0]) || !finite(e.pos[1]) || !finite(e.pos[2])) continue;
        void* best = NULL;
        uintptr_t bestComp = 0;
        float bestD = kMatchRadius * kMatchRadius;
        for (int j = 0; j < nc; j++) {
            LinearTransform xf;
            if (!readEntityTransform(chars[j], xf)) continue;
            float d = 0;
            for (int a = 0; a < 3; a++) d += (xf.trans[a] - e.pos[a]) * (xf.trans[a] - e.pos[a]);
            if (d >= bestD) continue;
            uintptr_t comp = (uintptr_t)getComponent(chars[j], TI_PamServerAIHealthComponent);
            if (!comp || !isHealthComp(comp)) comp = (uintptr_t)getComponent(chars[j], TI_PamServerHealthComponent);
            if (!comp || !isHealthComp(comp)) continue;
            // (un ennemi déjà à terre reste candidat : sinon un coup reçu deux fois, ou deux joueurs qui
            // frappent le même ennemi, mettraient K.O. un AUTRE ennemi proche)
            // santé max différente : possible (deux jeux), mais un garde de même santé max est préféré
            if (e.maxHealth > 0 && fabsf(readOr<float>(comp + Health_Max, 0.f) - e.maxHealth) > 0.5f) d += 36.f;
            if (d >= bestD) continue;
            best = chars[j];
            bestComp = comp;
            bestD = d;
        }
        if (!best) {
            DWORD done = g_scanDoneAt;
            bool fresh = done && (LONG)(done - work[i].at) > 0;    // une recherche finie après ce coup
            if (!fresh && now - work[i].at < kPendingMs && g_npending < 32) {
                g_pending[g_npending++] = work[i];
                InterlockedExchange(&g_scanWanted, 1);
                continue;
            }
            LONG c = InterlockedIncrement(&g_missed);
            if (c <= 20) logf("combat : coup reçu à %.1f %.1f %.1f, aucun ennemi correspondant (%d ennemis connus)", e.pos[0],
                              e.pos[1], e.pos[2], nc);
            continue;
        }
        float cur = readOr<float>(bestComp + Health_Value, 0.f);
        float want = e.health < 0.f ? 0.f : e.health;
        if (cur <= want + 0.01f) continue;                             // déjà aussi bas (ou plus bas) chez moi
        typedef void (*SetHealthFn)(void*, float);
        SetHealthFn set = (SetHealthFn)gameVirtual((void*)bestComp, Slot_SetHealth);
        if (!set) continue;
        set((void*)bestComp, want);
        LONG c = InterlockedIncrement(&g_applied);
        if (c <= 30)
            logf("combat : coup reçu appliqué à l'ennemi %p (à %.1f m) : santé %.1f -> %.1f%s", best, sqrtf(bestD), cur, want,
                 want <= 0 ? " (K.O.)" : "");
    }
}

void status(char* out, size_t cap) {
    if (!g_installed) { lstrcpynA(out, "combats : indisponibles", (int)cap); return; }
    _snprintf(out, cap, "combats : %ld coups envoyés, %ld reçus appliqués", g_sent, g_applied);
}

}  // namespace combat
}  // namespace game
