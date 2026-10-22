// =====================================================================================
//  MEC Coop - base du module "personnages 3D" : mémoire, journal, niveau, crochets,
//  capture de ma pose, joueurs distants (mémoire partagée avec coop.exe), démarrage.
//
//  Méthode et adresses reprises du mod MECMP (licence MIT) :
//    Copyright (c) 2026 MECMP contributors - https://github.com/CodeNameMeteor/mecmp
// =====================================================================================
#include "game.h"
#include <stdarg.h>
#include <stdio.h>


namespace game {

// ---------------------------------------------------------------- mémoire
bool write(uintptr_t addr, const void* src, size_t size) {
    if (addr < 0x10000) return false;
    typedef LONG(NTAPI * NtWriteFn)(HANDLE, PVOID, LPCVOID, SIZE_T, SIZE_T*);
    static NtWriteFn ntWrite = (NtWriteFn)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtWriteVirtualMemory");
    SIZE_T done = 0;
    if (ntWrite && ntWrite(GetCurrentProcess(), (PVOID)addr, src, size, &done) >= 0 && done == size) return true;
    done = 0;
    return WriteProcessMemory(GetCurrentProcess(), (LPVOID)addr, src, size, &done) && done == size;
}

bool readCString(uintptr_t addr, char* out, size_t cap) {
    if (!cap) return false;
    size_t n = 0;
    while (n + 1 < cap) {
        char c;
        if (!read(addr + n, c)) return false;
        if (c == 0) { out[n] = 0; return n > 0; }
        if (c < 0x20 || c > 0x7e) return false;
        out[n++] = c;
    }
    return false;
}

bool patchPointer(uintptr_t addr, uintptr_t value, uintptr_t* old) {
    DWORD prot = 0;
    if (!VirtualProtect((void*)addr, sizeof(uintptr_t), PAGE_READWRITE, &prot)) return false;
    if (old) *old = *(volatile uintptr_t*)addr;
    *(volatile uintptr_t*)addr = value;
    DWORD tmp;
    VirtualProtect((void*)addr, sizeof(uintptr_t), prot, &tmp);
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
    return true;
}

// ---------------------------------------------------------------- journal
static Lock g_logLock;
static HANDLE g_log = INVALID_HANDLE_VALUE;

void logf(const char* fmt, ...) {
    char buf[600];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = wsprintfA(buf, "[%02d:%02d:%02d] ", t.wHour, t.wMinute, t.wSecond);
    va_list ap;
    va_start(ap, fmt);
    int m = _vsnprintf(buf + n, sizeof(buf) - n - 3, fmt, ap);
    va_end(ap);
    if (m < 0) m = (int)strlen(buf + n);
    n += m;
    buf[n++] = '\r';
    buf[n++] = '\n';
    Guard g(g_logLock);
    if (g_log == INVALID_HANDLE_VALUE) {
        // dans le dossier du jeu : <jeu>\_mod_coop\log_overlay.txt (là où coop écrit déjà ses journaux)
        char path[MAX_PATH];
        DWORD k = GetModuleFileNameA(NULL, path, MAX_PATH);
        if (!k || k > MAX_PATH - 40) return;
        char* slash = strrchr(path, '\\');
        if (!slash) return;
        slash[1] = 0;
        lstrcatA(path, "_mod_coop");
        CreateDirectoryA(path, NULL);
        lstrcatA(path, "\\log_overlay.txt");
        g_log = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (g_log == INVALID_HANDLE_VALUE) return;
    }
    DWORD w;
    WriteFile(g_log, buf, n, &w, NULL);
}

// ---------------------------------------------------------------- garde-fous sur une pose
bool limitPose(PlayerState& st) {
    const float kReach = 2.f;
    if (st.boneCount > kMaxBones || !finite(st.yaw)) return false;
    for (int i = 0; i < 3; i++) if (!finite(st.pos[i]) || fabsf(st.pos[i]) > 100000.f) return false;
    st.yaw = fmodf(st.yaw, 6.2831853f);
    for (uint32_t b = 0; b < st.boneCount; ++b) {
        float* bone = st.bones[b];
        double norm = 0;
        for (int j = 0; j < 8; ++j) if (!finite(bone[j])) return false;
        for (int j = 4; j < 8; ++j) norm += (double)bone[j] * bone[j];
        if (norm < 1.e-12 || norm > 1.e12) return false;
        float inv = (float)(1. / sqrt(norm));
        for (int j = 4; j < 8; ++j) bone[j] *= inv;
        bone[3] = 1.f;
        if (b == 1) { for (int j = 0; j < 3; ++j) bone[j] = st.pos[j]; continue; }   // ancre du corps
        double len = sqrt((double)bone[0] * bone[0] + (double)bone[1] * bone[1] + (double)bone[2] * bone[2]);
        if (len > kReach) for (int j = 0; j < 3; ++j) bone[j] *= (float)(kReach / len);
    }
    return true;
}

// =====================================================================================
//  Niveau : les "spawners" (créateurs de personnages) du joueur et des fantômes
// =====================================================================================
namespace level {
static Lock g_m;
static void* g_playerSpawner = NULL;
static void* g_playerCands[32];        // tous les spawners de Faith du niveau (une mission peut avoir le sien)
static size_t g_playerCandCount = 0;
static void* g_ghostSpawner = NULL;
static void* g_clientComps[64];
static size_t g_clientCompCount = 0;
static void* g_ghostCands[64];
static size_t g_ghostCandCount = 0;
static bool g_ghostChosen = false;
struct Fail { void* sp; int n; };
static Fail g_fails[16];
static size_t g_failCount = 0;
struct Bad { void* sp; uintptr_t data; };
static Bad g_bad[64];
static size_t g_badCount = 0;
static const int kMaxSpawnFails = 3;

void init() { g_m.init(); }

static bool clientCompAlive(uintptr_t c) {
    uintptr_t vt = 0;
    return read(c, vt) && vt == rva(Vtbl_PamClientReplayPlayerComponent);
}
bool spawnerAlive(void* spawner) {
    uintptr_t vt = 0;
    return spawner && read((uintptr_t)spawner, vt) &&
           (vt == rva(Vtbl_PamServerCharacterSpawnEntity) || vt == rva(Vtbl_ServerCharacterSpawnEntity));
}
static bool readSetup(uintptr_t comp, BoneSetup& out) {
    uintptr_t S = comp + CRP_Setup;
    uint32_t n = 0;
    uintptr_t idx = 0, flags = 0;
    if (!read(S, n) || n == 0 || n > kMaxBones) return false;
    if (!read(S + 0x10, idx) || !idx || !read(S + 0x78, flags) || !flags) return false;
    if (!read(idx, out.indices, n * 4) || !read(flags, out.flags, n)) return false;
    out.count = n;
    return true;
}

static void considerSpawner(void* entity, uintptr_t data, bool fromScan) {
    uintptr_t bp = data ? readOr<uintptr_t>(data + ReferenceObjectData_Blueprint) : 0;
    char name[128];
    if (!bp || !readCString(readOr<uintptr_t>(bp + Asset_Name), name, sizeof(name))) return;
    models::noteTemplate(data);
    research_ai::noteSpawner(name, entity);
    combat::noteSpawner(name, entity);
    Guard g(g_m);
    if (strcmp(name, kPlayerBlueprint) == 0) {
        if (!fromScan || !g_playerSpawner || !spawnerAlive(g_playerSpawner)) g_playerSpawner = entity;
        bool known = false;
        for (size_t i = 0; i < g_playerCandCount; i++) known = known || g_playerCands[i] == entity;
        if (!known) {
            if (g_playerCandCount >= 32) { memmove(g_playerCands, g_playerCands + 1, 31 * sizeof(void*)); g_playerCandCount = 31; }
            g_playerCands[g_playerCandCount++] = entity;
        }
    } else if (strcmp(name, kGhostBlueprint) == 0) {
        bool gone = !g_ghostSpawner || !spawnerAlive(g_ghostSpawner);
        if (gone) g_ghostChosen = false;
        if ((!fromScan && !g_ghostChosen) || gone) g_ghostSpawner = entity;
        bool known = false;
        for (size_t i = 0; i < g_ghostCandCount; i++) known = known || g_ghostCands[i] == entity;
        if (!known && g_ghostCandCount < 64) g_ghostCands[g_ghostCandCount++] = entity;
    }
}

void onSpawnerCreated(void* ci, void* entity) {
    if (!ci || !entity) return;
    considerSpawner(entity, readOr<uintptr_t>((uintptr_t)ci + CI_Data), false);
}

void onForeignClientReplayComp(void* comp) {
    Guard g(g_m);
    if (g_clientCompCount >= 64) {
        memmove(g_clientComps, g_clientComps + 1, 63 * sizeof(void*));
        g_clientCompCount = 63;
    }
    g_clientComps[g_clientCompCount++] = comp;
}

bool complete() {
    if (!playerCharacter()) return false;      // mon personnage vient d'un spawner pas encore vu (mission) : on cherche
    Guard g(g_m);
    if (!spawnerAlive(g_playerSpawner) || !spawnerAlive(g_ghostSpawner)) return false;
    for (size_t i = 0; i < g_clientCompCount; i++)
        if (clientCompAlive((uintptr_t)g_clientComps[i])) return true;
    return false;
}

// Le mod est chargé après le niveau : on retrouve les spawners et les composants dans la mémoire.
void scanForMissing() {
    const uintptr_t vPam = rva(Vtbl_PamServerCharacterSpawnEntity), vSrv = rva(Vtbl_ServerCharacterSpawnEntity),
                    vComp = rva(Vtbl_PamClientReplayPlayerComponent);
    DWORD start = GetTickCount();
    static uintptr_t spawners[4096];
    static uintptr_t comps[64];
    size_t ns = 0, nc = 0;
    const size_t kBuf = 4 << 20;
    uint8_t* buf = (uint8_t*)VirtualAlloc(NULL, kBuf, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buf) return;
    unsigned long long scanned = 0;
    MEMORY_BASIC_INFORMATION mbi;
    for (uintptr_t addr = 0x10000; VirtualQuery((void*)addr, &mbi, sizeof(mbi)) == sizeof(mbi);
         addr = (uintptr_t)mbi.BaseAddress + mbi.RegionSize) {
        if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE || mbi.Protect != PAGE_READWRITE) continue;
        uintptr_t base = (uintptr_t)mbi.BaseAddress;
        if ((uint8_t*)base == buf) continue;
        for (size_t off = 0; off < mbi.RegionSize; off += kBuf) {
            size_t n = mbi.RegionSize - off < kBuf ? mbi.RegionSize - off : kBuf;
            if (!read(base + off, buf, n)) continue;
            scanned += n;
            const uintptr_t* q = (const uintptr_t*)buf;
            for (size_t i = 0; i < n / 8; i += 2) {
                uintptr_t v = q[i];
                if (v == vPam || v == vSrv) { if (ns < 4096) spawners[ns++] = base + off + i * 8; }
                else if (v == vComp) { if (nc < 64) comps[nc++] = base + off + i * 8; }
            }
        }
    }
    VirtualFree(buf, 0, MEM_RELEASE);
    // fantômes : on préfère le spawner dont les données sont uniques (le fantôme de la ville)
    static uintptr_t ge[512], gd[512];
    size_t ng = 0;
    for (size_t i = 0; i < ns; i++) {
        uintptr_t e = spawners[i];
        uintptr_t data = readOr<uintptr_t>(e + Spawner_Data);
        uintptr_t bp = data ? readOr<uintptr_t>(data + ReferenceObjectData_Blueprint) : 0;
        char name[128];
        if (!bp || !readCString(readOr<uintptr_t>(bp + Asset_Name), name, sizeof(name))) continue;
        if (strcmp(name, kGhostBlueprint) == 0) { if (ng < 512) { ge[ng] = e; gd[ng] = data; ng++; } }
        else considerSpawner((void*)e, data, true);
    }
    // tri stable par nombre de spawners partageant les mêmes données (uniques d'abord)
    static size_t shared[512];
    for (size_t i = 0; i < ng; i++) {
        shared[i] = 0;
        for (size_t j = 0; j < ng; j++) shared[i] += gd[j] == gd[i];
    }
    // du moins partagé au plus partagé : en recherche, le premier vivant retenu reste (il n'est pas remplacé)
    for (size_t s = 1; s <= ng; s++)
        for (size_t i = 0; i < ng; i++)
            if (shared[i] == s) considerSpawner((void*)ge[i], gd[i], true);
    for (size_t i = 0; i < nc; i++) onForeignClientReplayComp((void*)comps[i]);
    logf("niveau : %llu Mo parcourus en %.1f s, %u spawners (%u fantômes), %u composants ; joueur %s, fantôme %s",
         scanned >> 20, (GetTickCount() - start) / 1000.0, (unsigned)ns, (unsigned)ng, (unsigned)nc,
         spawnerAlive(playerSpawner()) ? "trouvé" : "absent", spawnerAlive(ghostSpawner()) ? "trouvé" : "absent");
}

void chooseGhostSpawner(void* const* ours, size_t oursCount) {
    static DWORD lastAt = 0;
    DWORD now = GetTickCount();
    if (now - lastAt < 500) return;
    lastAt = now;
    void* cands[64];
    size_t nc = 0;
    void* current;
    {
        Guard g(g_m);
        current = g_ghostSpawner;
        for (size_t i = 0; i < g_ghostCandCount; i++) {
            bool bad = false;
            for (size_t k = 0; k < g_badCount; k++)
                bad = bad || (g_bad[k].sp == g_ghostCands[i] &&
                              readOr<uintptr_t>((uintptr_t)g_bad[k].sp + Spawner_Data) == g_bad[k].data);
            if (!bad) cands[nc++] = g_ghostCands[i];
        }
    }
    struct Info { void* sp; uintptr_t data; size_t holds; };
    Info alive[64];
    size_t na = 0;
    for (size_t i = 0; i < nc; i++) {
        void* c = cands[i];
        if (!spawnerAlive(c)) continue;
        uintptr_t s = (uintptr_t)c, v[2] = {0, 0};
        size_t holds = 0;
        if (read(s + Spawner_Spawned, v, sizeof(v)) && v[1] >= v[0] && v[1] - v[0] <= 0x800) {
            size_t count = (v[1] - v[0]) / 8;
            uintptr_t items[64];
            if (count <= 64 && (count == 0 || read(v[0], items, count * 8))) {
                for (size_t k = 0; k < count; ++k) {
                    bool mine = false;
                    for (size_t j = 0; j < oursCount; ++j) mine = mine || (uintptr_t)ours[j] == items[k];
                    if (!mine) ++holds;
                }
            } else holds = count;
        }
        alive[na].sp = c;
        alive[na].data = readOr<uintptr_t>(s + Spawner_Data);
        alive[na].holds = holds;
        na++;
    }
    if (!na) return;
    void* best = NULL;
    int bestScore = -1, currentScore = -1;
    for (size_t i = 0; i < na; i++) {
        size_t sh = 0;
        for (size_t j = 0; j < na; j++) sh += alive[j].data == alive[i].data;
        bool own = sh == 1;
        int score = own ? (alive[i].holds ? 6 : 3) : (alive[i].holds ? 1 : 0);
        if (alive[i].sp == current) currentScore = score;
        if (score >= bestScore) { best = alive[i].sp; bestScore = score; }
    }
    Guard g(g_m);
    if (best && best != current && bestScore > currentScore) {
        logf("niveau : spawner de fantômes %p choisi parmi %u (score %d)", best, (unsigned)na, bestScore);
        g_ghostSpawner = best;
    }
    g_ghostChosen = g_ghostSpawner != NULL;
}

void spawnResult(void* spawner, bool got) {
    Guard g(g_m);
    size_t i = 0;
    while (i < g_failCount && g_fails[i].sp != spawner) i++;
    if (got) {
        if (i < g_failCount) g_fails[i] = g_fails[--g_failCount];
        return;
    }
    if (i == g_failCount) {
        if (g_failCount >= 16) return;
        g_fails[g_failCount].sp = spawner;
        g_fails[g_failCount].n = 0;
        g_failCount++;
    }
    if (++g_fails[i].n < kMaxSpawnFails) return;
    g_fails[i] = g_fails[--g_failCount];
    if (g_badCount < 64) {
        g_bad[g_badCount].sp = spawner;
        g_bad[g_badCount].data = readOr<uintptr_t>((uintptr_t)spawner + Spawner_Data);
        g_badCount++;
    }
    logf("niveau : le spawner %p n'a rien donné %d fois de suite, on en choisit un autre", spawner, kMaxSpawnFails);
    if (g_ghostSpawner == spawner) { g_ghostSpawner = NULL; g_ghostChosen = false; }
}

void reset() {
    {
        Guard g(g_m);
        g_playerSpawner = g_ghostSpawner = NULL;
        g_clientCompCount = g_ghostCandCount = g_failCount = g_badCount = g_playerCandCount = 0;
        g_ghostChosen = false;
    }
    models::reset();
}

uintptr_t serverWorld() { return *(const volatile uintptr_t*)rva(kServerGameWorld); }

bool inFrontEnd() {
    static volatile uintptr_t cachedWorld = 0;
    static volatile bool cached = false;
    uintptr_t world = serverWorld();
    if (!world) return false;
    if (world != cachedWorld) {
        uintptr_t lvl = readOr<uintptr_t>(world + World_Level);
        uintptr_t data = lvl ? readOr<uintptr_t>(lvl + Level_Data) : 0;
        char name[160];
        if (!data || !readCString(readOr<uintptr_t>(data + Asset_Name), name, sizeof(name))) return false;
        for (char* c = name; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;
        cached = strstr(name, "frontend") != NULL;
        cachedWorld = world;
        logf("niveau : %s", name);
    }
    return cached;
}

void* playerSpawner() { Guard g(g_m); return g_playerSpawner; }
void* ghostSpawner() { Guard g(g_m); return g_ghostSpawner; }

bool characterExists(void* character, uintptr_t expectedData) {
    uintptr_t ch = (uintptr_t)character;
    uintptr_t head[Character_Data / 8 + 1];
    return ch && read(ch, head, sizeof(head)) && head[0] == rva(Vtbl_PamServerCharacterEntity) &&
           (!expectedData || head[Character_Data / 8] == expectedData);
}

static void* characterOf(void* spawner, bool needPlayer);
void* playerCharacter() {
    void* c = characterOf(playerSpawner(), false);
    if (c && readOr<uintptr_t>((uintptr_t)c + Character_Player)) return c;
    // pas (ou plus) le bon spawner : en mission, Faith peut venir d'un autre spawner de Faith
    static DWORD lastSearch = 0;
    DWORD now = GetTickCount();
    if (now - lastSearch >= 500) {
        lastSearch = now;
        void* cands[32];
        size_t n;
        {
            Guard g(g_m);
            n = g_playerCandCount;
            memcpy(cands, g_playerCands, n * sizeof(void*));
        }
        for (size_t i = n; i-- > 0;) {
            if (cands[i] == playerSpawner() || !spawnerAlive(cands[i])) continue;
            void* p = characterOf(cands[i], true);
            if (p) {
                Guard g(g_m);
                if (g_playerSpawner != cands[i]) logf("niveau : mon personnage vient d'un autre spawner (mission)");
                g_playerSpawner = cands[i];
                return p;
            }
        }
    }
    return c;
}

static void* characterOf(void* spawner, bool needPlayer) {
    if (!spawner) return NULL;
    uintptr_t head[Spawner_Spawned / 8 + 2];
    if (!read((uintptr_t)spawner, head, sizeof(head)) ||
        (head[0] != rva(Vtbl_PamServerCharacterSpawnEntity) && head[0] != rva(Vtbl_ServerCharacterSpawnEntity)))
        return NULL;
    uintptr_t begin = head[Spawner_Spawned / 8], end = head[Spawner_Spawned / 8 + 1];
    if (!begin || end <= begin || (end - begin) / 8 > 64) return NULL;
    uintptr_t items[64];
    size_t count = (end - begin) / 8;
    if (!read(begin, items, count * 8)) return NULL;
    void* fallback = NULL;
    for (size_t i = count; i-- > 0;) {
        void* c = (void*)items[i];
        if (!c || !isKindOf(c, TI_ServerCharacterEntity)) continue;
        if (readOr<uintptr_t>((uintptr_t)c + Character_Player)) return c;
        if (!fallback) fallback = c;
    }
    return needPlayer ? NULL : fallback;
}

bool referenceSetup(BoneSetup& out) {
    void* comps[64];
    size_t n;
    {
        Guard g(g_m);
        n = g_clientCompCount;
        memcpy(comps, g_clientComps, n * sizeof(void*));
    }
    for (size_t i = 0; i < n; i++)
        if (clientCompAlive((uintptr_t)comps[i]) && readSetup((uintptr_t)comps[i], out)) return true;
    return false;
}
}  // namespace level

// =====================================================================================
//  Crochets (on remplace des pointeurs de fonctions du jeu, sans modifier son code)
// =====================================================================================
namespace hooks {
typedef void* (*FactoryFn)(void* creator, void* ci, void* a3, void* a4);
enum { kSpawner, kPamSpawner, kClientReplay, kClientCharacter, kCount };
static FactoryFn g_orig[kCount];
static const char* const kNames[kCount] = {"ServerCharacterSpawnEntity", "PamServerCharacterSpawnEntity",
                                           "PamClientReplayPlayerComponent", "PamClientCharacterEntity"};

static void* spawnerFactory(void* c, void* ci, void* a3, void* a4) {
    void* e = g_orig[kSpawner](c, ci, a3, a4);
    level::onSpawnerCreated(ci, e);
    return e;
}
static void* pamSpawnerFactory(void* c, void* ci, void* a3, void* a4) {
    void* e = g_orig[kPamSpawner](c, ci, a3, a4);
    level::onSpawnerCreated(ci, e);
    return e;
}
typedef void (*UpdateAllFn)(void* self, void* info);
static UpdateAllFn g_origUpdateAll = NULL;
static volatile bool g_updaterHooked = false;
static void updateAllHook(void* self, void* info) {
    mission::clientTick();
    menu::clientTick();
    research_ai::clientTick();
    ghosts::beforeClientReplayUpdate();
    g_origUpdateAll(self, info);
    audio::tick();
}
static void hookUpdaters() {
    if (g_updaterHooked) return;
    uintptr_t slot = rva(Vtbl_ClientReplayUpdater) + Vtbl_ClientReplayUpdateAll, cur = 0;
    if (!read(slot, cur) || cur != rva(Fn_ClientReplayUpdateAll)) return;
    g_origUpdateAll = (UpdateAllFn)cur;
    if (patchPointer(slot, (uintptr_t)&updateAllHook, NULL)) {
        g_updaterHooked = true;
        logf("crochet : mise à jour des replays installée");
    }
}
static void* clientReplayFactory(void* c, void* ci, void* a3, void* a4) {
    void* comp = g_orig[kClientReplay](c, ci, a3, a4);
    hookUpdaters();
    if (comp && !ghosts::onClientReplayComp(comp)) level::onForeignClientReplayComp(comp);
    return comp;
}
static void* clientCharacterFactory(void* c, void* ci, void* a3, void* a4) {
    void* ent = g_orig[kClientCharacter](c, ci, a3, a4);
    if (ent) ghosts::onClientCharacter(ent);
    return ent;
}

struct Patch { uintptr_t slot, original; };
static Patch g_undo[8];
static int g_undoN = 0;

static bool installFactory(int f, uintptr_t slotVa, uintptr_t expectedVa, FactoryFn hook) {
    uintptr_t slot = rva(slotVa), cur = 0;
    if (!read(slot, cur) || !cur) { logf("crochet %s : illisible", kNames[f]); return false; }
    if (cur != rva(expectedVa)) {
        MEMORY_BASIC_INFORMATION mbi;
        bool exec = VirtualQuery((void*)cur, &mbi, sizeof(mbi)) &&
                    (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
        if (!exec) { logf("crochet %s : contenu inattendu, pas touché", kNames[f]); return false; }
        logf("crochet %s : déjà détourné par un autre mod, on se met devant", kNames[f]);
    }
    g_orig[f] = (FactoryFn)cur;
    if (!patchPointer(slot, (uintptr_t)hook, NULL)) return false;
    g_undo[g_undoN].slot = slot;
    g_undo[g_undoN].original = cur;
    g_undoN++;
    return true;
}

typedef void (*PostUpdateFn)(void* world, void* updateInfo);
static PostUpdateFn g_origPostUpdate = NULL;
static TickFn g_onTick = NULL;
static void postUpdate(void* world, void* info) {
    g_origPostUpdate(world, info);
    if (world == (void*)level::serverWorld()) {
        float dt = info ? *(float*)((uint8_t*)info + 0x10) : 0.f;
        g_onTick(world, dt);
    }
}

bool clientUpdateHooked() { return g_updaterHooked; }

bool install(TickFn onTick, bool* ghostClients) {
    uintptr_t slot = rva(Slot_ServerWorldPostUpdate), cur = 0;
    if (!read(slot, cur) || cur != rva(Fn_ServerWorldPostUpdate)) {
        logf("crochet tick : contenu inattendu (%p) : version du jeu non reconnue, rien n'est installé", (void*)cur);
        return false;
    }
    g_onTick = onTick;
    g_origPostUpdate = (PostUpdateFn)cur;
    installFactory(kSpawner, Slot_ServerCharacterSpawnFactory, Fn_ServerCharacterSpawnFactory, &spawnerFactory);
    installFactory(kPamSpawner, Slot_PamServerCharacterSpawnFactory, Fn_PamServerCharacterSpawnFactory, &pamSpawnerFactory);
    bool replay = installFactory(kClientReplay, Slot_ClientReplayPlayerFactory, Fn_ClientReplayPlayerFactory, &clientReplayFactory);
    bool chars = installFactory(kClientCharacter, Slot_ClientCharacterFactory, Fn_ClientCharacterFactory, &clientCharacterFactory);
    if (!patchPointer(slot, (uintptr_t)&postUpdate, NULL)) {
        for (int i = g_undoN; i-- > 0;) patchPointer(g_undo[i].slot, g_undo[i].original, NULL);
        logf("crochet tick : échec, tout est remis en place");
        return false;
    }
    hookUpdaters();
    audio::install();
    combat::install();
    *ghostClients = replay && chars;
    logf("crochets : installés%s", *ghostClients ? "" : " (sans les clients de fantômes : pas de personnages 3D)");
    return true;
}
}  // namespace hooks

// =====================================================================================
//  Ma pose : la dernière image de l'enregistreur de replay du jeu (sert aux contre-la-montre)
// =====================================================================================
namespace local {
static bool capturePose(PlayerState& st, float& keyTime) {
    void* ch = ((void* (*)())rva(Fn_LocalClientCharacter))();
    if (!ch) return false;
    void* comp = getComponent(ch, TI_PamClientReplayRecordingComponent);
    if (!comp) return false;
    uintptr_t rec = readOr<uintptr_t>((uintptr_t)comp + 0x48);
    if (!rec) return false;
    uint8_t ring[Rec_Head + 4 - Rec_Stride];
    if (!read(rec + Rec_Stride, ring, sizeof(ring))) return false;
    uint64_t stride = 0;
    uint32_t cap = 0, head = 0;
    uintptr_t base = 0;
    memcpy(&stride, ring, 8);
    memcpy(&cap, ring + (Rec_Capacity - Rec_Stride), 4);
    memcpy(&base, ring + (Rec_Base - Rec_Stride), 8);
    memcpy(&head, ring + (Rec_Head - Rec_Stride), 4);
    if (!base || !cap || stride < 0x30 || (stride - 0x10) % 0x20 != 0) return false;
    uint32_t n = (uint32_t)((stride - 0x10) / 0x20);
    if (n == 0 || n > kMaxBones) return false;
    uint32_t best = head % cap, prev = (head + cap - 1) % cap;
    if (readOr<float>(base + prev * stride) > readOr<float>(base + best * stride)) best = prev;
    uintptr_t slot = base + best * stride;
    float before = 0, after = 0;
    if (!read(slot, before) || !read(slot + 0x10, st.bones, n * 0x20) || !read(slot, after) || before != after)
        return false;   // réécrite pendant la lecture
    keyTime = before;
    st.boneCount = n;
    return true;
}

bool capture(PlayerState& st) {
    void* player = level::playerCharacter();
    LinearTransform xf;
    if (!player || !readEntityTransform(player, xf)) return false;
    st.pos[0] = xf.trans[0]; st.pos[1] = xf.trans[1]; st.pos[2] = xf.trans[2];
    st.yaw = atan2f(xf.forward[0], xf.forward[2]);
    st.boneCount = 0;
    float keyTime = 0;
    bool posed = capturePose(st, keyTime) && st.boneCount > 1;
    static float lastKeyTime = -1.f;
    if (posed && keyTime == lastKeyTime) return false;      // rien de neuf
    if (posed) { st.pos[0] = st.bones[1][0]; st.pos[1] = st.bones[1][1]; st.pos[2] = st.bones[1][2]; }
    for (int i = 0; i < 3; i++) if (!finite(st.pos[i]) || fabsf(st.pos[i]) > 100000.f) return false;
    for (uint32_t b = 0; b < st.boneCount; ++b)
        for (int j = 0; j < 8; j++)
            if (!finite(st.bones[b][j]) || fabsf(st.bones[b][j]) > 100000.f) { st.boneCount = 0; break; }
    static LARGE_INTEGER freq = {{0, 0}};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    uint32_t now = (uint32_t)((double)c.QuadPart * 1000.0 / (double)freq.QuadPart);
    // horodatage : l'heure de l'image de l'enregistreur (espacement exact entre deux poses)
    static uint32_t lastMs = 0;
    static double anchorMs = 0;
    uint32_t ms = now;
    if (posed) {
        float step = keyTime - lastKeyTime;
        if (lastKeyTime < 0.f || step <= 0.f || step > 1.f) anchorMs = (double)now - keyTime * 1000.0;
        ms = (uint32_t)(int64_t)(anchorMs + keyTime * 1000.0);
        lastKeyTime = keyTime;
    }
    if ((int32_t)(ms - lastMs) <= 0) ms = lastMs + 1;
    lastMs = ms;
    st.timeMs = ms;
    return true;
}
}  // namespace local

// =====================================================================================
//  Échange avec coop.exe : ma pose (envoyée par lui), les joueurs distants (reçus par lui)
//  Mémoire partagée "Local\MECCoopPose_v1", créée par coop.exe.
// =====================================================================================
#pragma pack(push, 1)
struct ShmLocal {
    uint32_t seq;          // pair = stable
    uint32_t valid;
    uint32_t timeMs;
    float pos[3];
    float yaw;
    uint32_t bones;
    float b[kMaxBones][8];
};
struct ShmRemote {
    uint32_t id;
    char name[32];
    uint32_t rx;           // +1 à chaque nouveau paquet
    uint32_t look;         // apparence choisie par ce joueur : 1 Faith, 2 fantôme
    uint32_t timeMs;       // horloge de l'expéditeur
    float pos[3];
    float yaw;
    uint32_t bones;
    float b[kMaxBones][8];
};
struct PoseShm {
    uint32_t magic;        // 'MECP'
    uint32_t version;      // 2
    ShmLocal local;
    uint32_t rseq;
    uint32_t rcount;
    ShmRemote r[8];
    uint32_t sseq;
    char status[200];      // état des fantômes (affiché par coop.exe)
    // contrôle (v3)
    uint32_t role;         // coop : 0 rien, 1 hôte, 2 rejoint, 3 test solo
    uint32_t followTod;    // coop : suivre l'heure de l'hôte
    float hostTod;         // coop : heure de l'hôte (-1 = aucune)
    float myTod;           // overlay : mon heure à envoyer (-1 = rien)
    uint32_t tpReq;        // coop : +1 à chaque demande de téléportation
    uint32_t tpPeer;       // coop : vers ce joueur (0 = vers tpPos)
    float tpPos[3];
    uint32_t tpDone;       // overlay : dernière demande prise en compte
    uint32_t tpResult;     // overlay : 1 réussie, 2 échouée
    char todStatus[64];    // overlay
    // missions (v4)
    uint32_t msSeq;        // overlay : +1 à chaque début / fin de MA mission
    uint32_t msActive;     // overlay : 1 = je suis en mission
    uint32_t msHavePos;
    float msPos[3];        // overlay : où j'étais juste avant le début
    char msName[64];       // overlay : nom de la mission (celui du jeu)
    char msKind[32];
    uint32_t ivSeq;        // coop : +1 à chaque nouvelle invitation (un autre a lancé une mission)
    float ivPos[3];        // coop : où aller pour la lancer aussi
    char ivText[96];       // coop : texte affiché dans le jeu (vide = aucune invitation)
    uint32_t ivTaken;      // overlay : = ivSeq quand j'ai accepté (Alt+M)
    // sons (v5)
    uint32_t aEnabled;     // coop : 1 = jouer les sons des autres
    uint32_t aOutHead;     // overlay : nombre total de sons écrits dans aOut
    audio::Sample aOut[64];
    uint32_t aInHead;      // coop : nombre total de sons écrits dans aIn
    audio::InSample aIn[128];
    // combats (v6)
    uint32_t cbOutHead;    // overlay
    combat::Event cbOut[32];
    uint32_t cbInHead;     // coop
    combat::Event cbIn[32];
    // état du jeu + message de coop affiché dans le jeu (v7)
    uint32_t gameState;    // overlay : 0 rien, 1 au menu principal, 2 en partie
    uint32_t noticeSeq;    // coop
    char notice[128];      // coop : texte affiché en haut de l'écran (vide = rien)
    uint32_t menuReq;      // coop : +1 = demande (v8)
    uint32_t menuKind;     // coop : 0 menu principal, 1 recharger la sauvegarde (v9)
    // progression (v10) : drapeaux de l'histoire
    uint32_t pgOutHead;    // overlay : mes changements de drapeaux
    progression::Flag pgOut[256];
    uint32_t pgInHead;     // coop : drapeaux de l'hôte à appliquer
    progression::Flag pgIn[512];
    uint32_t pgApplied;    // overlay : drapeaux appliqués
    uint32_t pgMissing;    // overlay : drapeaux inconnus de mon jeu
    uint32_t pgSnapReq;    // coop : +1 = envoyer toute ma table de drapeaux (quelqu'un vient de rejoindre)
};
#pragma pack(pop)
static_assert(sizeof(ShmLocal) == 1056, "ShmLocal");
static_assert(sizeof(ShmRemote) == 1092, "ShmRemote");
static_assert(sizeof(PoseShm) == 45868, "PoseShm (coop.py : POSE_SIZE)");

static PoseShm* g_shm = NULL;
static PoseShm* shm() {
    static DWORD lastTry = 0;
    if (g_shm) return g_shm;
    if (GetTickCount() - lastTry < 1000) return NULL;
    lastTry = GetTickCount();
    HANDLE h = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, "Local\\MECCoopPose_v11");
    if (!h) return NULL;
    PoseShm* p = (PoseShm*)MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(PoseShm));
    if (!p) { CloseHandle(h); return NULL; }
    g_shm = p;
    return p;
}

static void publishLocal(const PlayerState& st) {
    PoseShm* s = shm();
    if (!s || s->magic != 0x5043454D) return;
    volatile uint32_t* seq = &s->local.seq;
    uint32_t v = *seq;
    *seq = v | 1;
    MemoryBarrier();
    s->local.valid = 1;
    s->local.timeMs = st.timeMs;
    memcpy(s->local.pos, st.pos, 12);
    s->local.yaw = st.yaw;
    s->local.bones = st.boneCount;
    memcpy(s->local.b, st.bones, st.boneCount * 32);
    MemoryBarrier();
    *seq = (v | 1) + 1;
}

static void publishStatus(const char* text) {
    PoseShm* s = shm();
    if (!s || s->magic != 0x5043454D) return;
    s->sseq++;
    lstrcpynA(s->status, text, sizeof(s->status));
}

namespace remote {
static const int kSnaps = 16;
static const double kInterpDelayMs = 120.0;
struct Snap { double t; PlayerState s; };
struct Remote {
    uint32_t id;
    char name[32];
    uint32_t rx;
    uint32_t look;
    DWORD lastTick;
    double lastSent;
    // estimation du décalage d'horloge (le plus petit "arrivée - envoi" sur 2 s)
    double at[64], delta[64], sentT[64];
    int dh, df;
    double minDelta;
    double delay;          // délai d'affichage adapté au réseau (ms)
    Snap snaps[kSnaps];
    int head, fill;
    bool used;
};
static Remote g_r[8];
static Lock g_m;
static double g_lastPoll = 0;

static double nowMs() {
    static LARGE_INTEGER f = {{0, 0}};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

// lit les joueurs écrits par coop.exe et range les nouveaux paquets (verrou tenu)
static void poll() {
    double now = nowMs();
    if (now - g_lastPoll < 1.0) return;
    g_lastPoll = now;
    PoseShm* s = shm();
    if (!s || s->magic != 0x5043454D) return;
    static ShmRemote copy[8];
    uint32_t count = 0;
    bool ok = false;
    for (int tries = 0; tries < 4 && !ok; tries++) {
        uint32_t a = *(volatile uint32_t*)&s->rseq;
        if (a & 1) { YieldProcessor(); continue; }
        MemoryBarrier();
        count = s->rcount > 8 ? 8 : s->rcount;
        memcpy(copy, s->r, count * sizeof(ShmRemote));
        MemoryBarrier();
        ok = *(volatile uint32_t*)&s->rseq == a;
    }
    if (!ok) return;
    // un joueur qui n'est plus dans la liste de coop (partie arrêtée, joueur parti) : oublié tout de suite
    for (int k = 0; k < 8; k++) {
        if (!g_r[k].used) continue;
        bool listed = false;
        for (uint32_t i = 0; i < count; i++) listed = listed || copy[i].id == g_r[k].id;
        if (!listed) g_r[k].used = false;
    }
    for (uint32_t i = 0; i < count; i++) {
        const ShmRemote& in = copy[i];
        if (!in.id) continue;
        Remote* r = NULL;
        for (int k = 0; k < 8 && !r; k++) if (g_r[k].used && g_r[k].id == in.id) r = &g_r[k];
        if (!r) {
            for (int k = 0; k < 8 && !r; k++) if (!g_r[k].used) r = &g_r[k];
            if (!r) {   // plein : on remplace le plus ancien
                r = &g_r[0];
                for (int k = 1; k < 8; k++) if (g_r[k].lastTick < r->lastTick) r = &g_r[k];
            }
            memset(r, 0, sizeof(*r));
            r->used = true;
            r->id = in.id;
            r->rx = in.rx - 1;
        }
        lstrcpynA(r->name, in.name, 32);
        r->look = in.look;
        if (in.rx == r->rx) continue;
        r->rx = in.rx;
        double sent = (double)in.timeMs;
        if (r->fill && sent <= r->lastSent) continue;   // en retard ou en double
        // un saut d'horloge (redémarrage du jeu de l'autre) : on repart de zéro
        if (r->fill && (sent - r->lastSent > 10000.0)) { r->fill = 0; r->df = 0; }
        r->lastSent = sent;
        r->lastTick = GetTickCount();
        r->at[r->dh] = now;
        r->delta[r->dh] = now - sent;
        r->sentT[r->dh] = sent;
        r->dh = (r->dh + 1) % 64;
        if (r->df < 64) r->df++;
        double best = 1e300;
        for (int k = 0; k < r->df; k++) if (now - r->at[k] <= 2000.0 && r->delta[k] < best) best = r->delta[k];
        r->minDelta = best;
        // Délai d'affichage : juste assez pour avoir toujours une pose plus récente à rejoindre
        // (plus grand écart entre deux poses + plus grand retard du réseau sur 2 s, + une marge).
        // Réseau régulier : ~50 ms au lieu de 120 ; réseau irrégulier : le délai monte tout seul.
        double late = 0, gap = 33.0;
        for (int k = 1; k < r->df; k++) {          // dans l'ordre d'arrivée (= ordre d'envoi : les retardataires sont jetés)
            int a = (r->dh - k + 64) % 64, b = (r->dh - k - 1 + 64) % 64;
            if (now - r->at[a] > 2000.0) break;
            if (r->delta[a] - best > late) late = r->delta[a] - best;
            double d = r->sentT[a] - r->sentT[b];
            if (d > gap && d < 400.0) gap = d;
        }
        double target = gap + late + 12.0;
        if (target < 45.0) target = 45.0;
        if (target > 200.0) target = 200.0;
        if (r->delay <= 0) r->delay = kInterpDelayMs;
        r->delay += (target - r->delay) * (target > r->delay ? 0.3 : 0.03);   // monte vite, redescend doucement
        Snap& sn = r->snaps[r->head];
        sn.t = sent;
        memcpy(sn.s.pos, in.pos, 12);
        sn.s.yaw = in.yaw;
        sn.s.timeMs = in.timeMs;
        sn.s.boneCount = in.bones > kMaxBones ? 0 : in.bones;
        memcpy(sn.s.bones, in.b, sn.s.boneCount * 32);
        r->head = (r->head + 1) % kSnaps;
        if (r->fill < kSnaps) r->fill++;
    }
}

static void blend(const PlayerState& a, const PlayerState& b, float k, PlayerState& out) {
    out = b;
    for (int i = 0; i < 3; ++i) out.pos[i] = a.pos[i] + (b.pos[i] - a.pos[i]) * k;
    float dy = b.yaw - a.yaw;
    while (dy > 3.14159265f) dy -= 6.2831853f;
    while (dy < -3.14159265f) dy += 6.2831853f;
    out.yaw = a.yaw + dy * k;
    if (a.boneCount != b.boneCount) return;
    for (uint32_t i = 0; i < b.boneCount; ++i) {
        const float* pa = a.bones[i];
        const float* pb = b.bones[i];
        float* po = out.bones[i];
        for (int j = 0; j < 3; ++j) po[j] = pa[j] + (pb[j] - pa[j]) * k;
        float dot = pa[4] * pb[4] + pa[5] * pb[5] + pa[6] * pb[6] + pa[7] * pb[7];
        float sb = dot < 0 ? -1.f : 1.f, q[4], len = 0;
        for (int j = 0; j < 4; ++j) { q[j] = pa[4 + j] + (sb * pb[4 + j] - pa[4 + j]) * k; len += q[j] * q[j]; }
        len = len > 1e-12f ? 1.f / sqrtf(len) : 0.f;
        for (int j = 0; j < 4; ++j) po[4 + j] = len ? q[j] * len : pb[4 + j];
    }
}

size_t list(Info* out, size_t cap) {
    Guard g(g_m);
    poll();
    size_t n = 0;
    DWORD now = GetTickCount();
    for (int k = 0; k < 8 && n < cap; k++) {
        if (!g_r[k].used || !g_r[k].fill) continue;
        out[n].id = g_r[k].id;
        lstrcpynA(out[n].name, g_r[k].name, 32);
        out[n].ageMs = now - g_r[k].lastTick;
        out[n].look = g_r[k].look;
        n++;
    }
    return n;
}

int delayMs(uint32_t id) {
    Guard g(g_m);
    for (int k = 0; k < 8; k++)
        if (g_r[k].used && g_r[k].id == id) return (int)(g_r[k].delay + 0.5);
    return 0;
}

bool playTime(uint32_t id, uint32_t senderMs, double& localMs) {
    Guard g(g_m);
    poll();
    for (int k = 0; k < 8; k++)
        if (g_r[k].used && g_r[k].id == id && g_r[k].fill && g_r[k].minDelta < 1e299) {
            // l'horloge de l'autre est sur 32 bits : on la recolle au dernier paquet de pose
            double sent = g_r[k].lastSent + (double)(int32_t)(senderMs - (uint32_t)(int64_t)g_r[k].lastSent);
            localMs = sent + g_r[k].minDelta + (g_r[k].delay > 0 ? g_r[k].delay : kInterpDelayMs);
            return true;
        }
    return false;
}

bool state(uint32_t id, PlayerState& out, uint32_t& ageMs) {
    Guard g(g_m);
    poll();
    Remote* r = NULL;
    for (int k = 0; k < 8 && !r; k++) if (g_r[k].used && g_r[k].id == id) r = &g_r[k];
    if (!r || !r->fill) return false;
    ageMs = GetTickCount() - r->lastTick;
    // les deux poses autour de (maintenant - délai), sur l'horloge de l'expéditeur
    double t = nowMs() - r->minDelta - (r->delay > 0 ? r->delay : kInterpDelayMs);
    const Snap* newer = NULL;
    for (int k = 0; k < r->fill; ++k) {
        const Snap& sn = r->snaps[(r->head - 1 - k + 2 * kSnaps) % kSnaps];
        if (sn.t <= t) {
            if (!newer) {
                // la pose suivante est en retard (paquet perdu) : on prolonge le déplacement un court instant
                // au lieu de figer le personnage (seulement la position, au plus 100 ms)
                out = sn.s;
                if (r->fill >= 2) {
                    const Snap& pv = r->snaps[(r->head - 2 + 2 * kSnaps) % kSnaps];
                    double dt = sn.t - pv.t, ahead = t - sn.t;
                    if (ahead > 100.0) ahead = 100.0;
                    if (dt > 5.0 && dt < 200.0 && ahead > 0) {
                        float v[3], sp = 0;
                        for (int i = 0; i < 3; i++) { v[i] = (float)((sn.s.pos[i] - pv.s.pos[i]) / dt); sp += v[i] * v[i]; }
                        if (sp < 0.015f * 0.015f) {          // moins de 15 m/s : un vrai déplacement, pas une téléportation
                            for (int i = 0; i < 3; i++) {
                                float d = v[i] * (float)ahead;
                                out.pos[i] += d;
                                if (out.boneCount > 1) out.bones[1][i] += d;
                            }
                        }
                    }
                }
                return true;
            }
            double span = newer->t - sn.t;
            float f = span > 0.001 ? (float)((t - sn.t) / span) : 1.f;
            blend(sn.s, newer->s, f < 0 ? 0 : (f > 1 ? 1 : f), out);
            return true;
        }
        newer = &sn;
    }
    out = newer->s;
    return true;
}
}  // namespace remote

// =====================================================================================
//  Démarrage et boucle de surveillance
// =====================================================================================
static uint64_t g_ticks = 0, g_levelStartTick = 0;
static volatile uint32_t g_inviteAccept = 0;   // n° d'invitation acceptée (Alt+M)

namespace audio {
void publish(const Sample& smp) {
    PoseShm* s = shm();
    if (!s || s->magic != 0x5043454D) return;
    uint32_t h = s->aOutHead;
    s->aOut[h % 64] = smp;
    MemoryBarrier();
    s->aOutHead = h + 1;
}
int take(InSample* out, int cap) {
    PoseShm* s = shm();
    if (!s || s->magic != 0x5043454D) return 0;
    static uint32_t last = 0;
    static bool first = true;
    uint32_t h = *(volatile uint32_t*)&s->aInHead;
    if (first) { last = h; first = false; }
    if (h - last > 128) last = h - 128;
    int n = 0;
    MemoryBarrier();
    while (last != h && n < cap) out[n++] = s->aIn[last++ % 128];
    return n;
}
}  // namespace audio

namespace combat {
void publish(const Event& e) {
    PoseShm* s = shm();
    if (!s || s->magic != 0x5043454D) return;
    uint32_t h = s->cbOutHead;
    s->cbOut[h % 32] = e;
    MemoryBarrier();
    s->cbOutHead = h + 1;
}
int take(Event* out, int cap) {
    PoseShm* s = shm();
    if (!s || s->magic != 0x5043454D) return 0;
    static uint32_t last = 0;
    static bool first = true;
    uint32_t h = *(volatile uint32_t*)&s->cbInHead;
    if (first) { last = h; first = false; }
    if (h - last > 32) last = h - 32;
    int n = 0;
    MemoryBarrier();
    while (last != h && n < cap) out[n++] = s->cbIn[last++ % 32];
    return n;
}
}  // namespace combat

namespace progression {
void publish(const Flag& f) {
    PoseShm* s = shm();
    if (!s || s->magic != 0x5043454D) return;
    uint32_t h = s->pgOutHead;
    s->pgOut[h % 256] = f;
    MemoryBarrier();
    s->pgOutHead = h + 1;
}
static uint32_t g_inLast = 0;
static bool g_inFirst = true;
int take(Flag* out, int cap, bool peek) {
    PoseShm* s = shm();
    if (!s || s->magic != 0x5043454D) return 0;
    uint32_t h = *(volatile uint32_t*)&s->pgInHead;
    if (g_inFirst) { g_inLast = h; g_inFirst = false; }
    if (h - g_inLast > 512) g_inLast = h - 512;
    MemoryBarrier();
    int n = 0;
    for (uint32_t i = g_inLast; i != h && n < cap; i++) out[n++] = s->pgIn[i % 512];
    if (!peek) g_inLast += n;
    return n;
}
void consumed(int n) { g_inLast += n; }
uint32_t snapshotRequest() {
    PoseShm* s = shm();
    return (s && s->magic == 0x5043454D) ? *(volatile uint32_t*)&s->pgSnapReq : 0;
}
void setStats(uint32_t applied, uint32_t missing) {
    PoseShm* s = shm();
    if (s && s->magic == 0x5043454D) { s->pgApplied = applied; s->pgMissing = missing; }
}
}  // namespace progression

namespace notice {
bool text(char* out, size_t cap) {
    PoseShm* s = shm();
    if (!s || s->magic != 0x5043454D || !s->notice[0]) return false;
    lstrcpynA(out, s->notice, (int)cap);
    return true;
}
}  // namespace notice

namespace invite {
bool text(char* out, size_t cap, bool* taken) {
    PoseShm* s = shm();
    if (!s || s->magic != 0x5043454D || !s->ivSeq || !s->ivText[0]) return false;
    lstrcpynA(out, s->ivText, (int)cap);
    if (taken) *taken = s->ivTaken == s->ivSeq || g_inviteAccept == s->ivSeq;
    return true;
}
void accept() {
    PoseShm* s = shm();
    if (s && s->magic == 0x5043454D && s->ivSeq && s->ivText[0]) g_inviteAccept = s->ivSeq;
}
}  // namespace invite
static bool g_alreadyRunning = false;
static volatile bool g_active = false;
bool active() { return g_active; }

static void onTick(void* world, float dt) {
    (void)dt;
    static void* lastWorld = NULL;
    if (world != lastWorld) {
        if (lastWorld) { ghosts::levelChanged(); teleport::levelChanged(); combat::levelChanged(); }
        lastWorld = world;
        g_levelStartTick = g_ticks;
    }
    uint64_t n = ++g_ticks;
    static PlayerState st;
    bool have = local::capture(st);
    if (have) publishLocal(st);
    mission::serverTick((uintptr_t)world, have ? st.pos : NULL);
    if (have) audio::setLocalPos(st.pos);
    research_ai::serverTick();
    combat::tick();
    progression::serverTick();
    ghosts::tick(n - g_levelStartTick);
    // heure du jour et téléportation (demandes de coop.exe)
    PoseShm* s = shm();
    if (s && s->magic == 0x5043454D) {
        s->myTod = tod::tick((int)s->role, s->followTod != 0, s->hostTod);
        lstrcpynA(s->todStatus, tod::status(), sizeof(s->todStatus));
        static uint32_t lastReq = 0;
        static bool first = true;
        uint32_t req = s->tpReq;
        if (first) { lastReq = req; first = false; }       // une vieille demande ne compte pas
        if (req != lastReq) {
            lastReq = req;
            float pos[3] = {s->tpPos[0], s->tpPos[1], s->tpPos[2]};
            if (!teleport::request(s->tpPeer, pos)) s->tpResult = 2;
            else s->tpResult = 0;
            s->tpDone = req;
        }
        LONG r = teleport::result();
        if (r) s->tpResult = (uint32_t)r;
        audio::setEnabled(s->aEnabled != 0);
        // ma mission, pour coop (qui la dit aux autres)
        mission::State ms = mission::current();
        if (ms.seq != s->msSeq) {
            s->msActive = ms.active;
            s->msHavePos = ms.havePos;
            memcpy(s->msPos, ms.pos, 12);
            lstrcpynA(s->msName, ms.name, sizeof(s->msName));
            lstrcpynA(s->msKind, ms.kind, sizeof(s->msKind));
            MemoryBarrier();
            s->msSeq = ms.seq;
        }
        // invitation acceptée (Alt+M, vu par l'overlay) : on va au départ de la mission
        if (g_inviteAccept && g_inviteAccept == s->ivSeq && s->ivTaken != s->ivSeq) {
            s->ivTaken = s->ivSeq;
            float pos[3] = {s->ivPos[0], s->ivPos[1], s->ivPos[2]};
            logf("mission : invitation acceptée, téléportation vers %.1f %.1f %.1f", pos[0], pos[1], pos[2]);
            if (teleport::request(0, pos)) s->tpResult = 0;
        }
    }
    teleport::tick();
}

// cherche une classe du jeu par son nom dans la liste des types du moteur
static uintptr_t findType(const char* want) {
    uintptr_t ti = 0;
    if (!read(rva(kTypeInfoListHead), ti)) return 0;
    for (int n = 0; ti && n < 60000; n++) {
        uintptr_t data = 0, namePtr = 0;
        char name[96];
        if (read(ti, data) && data && read(data, namePtr) && namePtr && readCString(namePtr, name, sizeof(name)) &&
            strcmp(name, want) == 0)
            return ti;
        if (!read(ti + 8, ti)) break;
    }
    return 0;
}

static DWORD WINAPI MonitorThread(LPVOID) {
    logf("MEC Coop : module personnages 3D chargé");
    // version du jeu : quelques types doivent être exactement où on les attend
    struct Check { const char* name; uintptr_t va; } checks[] = {
        {"PamReplayPlayerComponentData", TI_PamReplayPlayerComponentData},
        {"ServerCharacterEntity", TI_ServerCharacterEntity},
        {"ObjectBlueprint", TI_ObjectBlueprint}};
    for (int tries = 0;; tries++) {
        bool ok = true;
        for (auto& c : checks) ok = ok && findType(c.name) == rva(c.va);
        if (ok) break;
        if (tries > 120) { logf("démarrage : version du jeu non reconnue, personnages 3D désactivés"); return 0; }
        Sleep(1000);
    }
    ghosts::setCharacterDataType(findType("PamCharacterEntityData"));
    bool ghostClients = false;
    if (!hooks::install(&onTick, &ghostClients)) return 0;
    ghosts::setEnabled(ghostClients);
    logf("démarrage : version du jeu reconnue, personnages 3D %s", ghostClients ? "actifs" : "désactivés");

    uintptr_t lastWorld = 0;
    DWORD levelSince = GetTickCount(), nextScanAt = 3000;
    int scans = 0;
    DWORD lastStatus = 0;
    for (;;) {
        uintptr_t world = level::serverWorld();
        if (world != lastWorld) {
            logf(world ? "niveau commencé" : "niveau terminé");
            if (!world) ghosts::levelChanged();
            research::levelChanged();
            if (lastWorld) level::reset();
            static bool firstWorld = true;
            lastWorld = world;
            levelSince = GetTickCount();
            // chargé en pleine partie : les spawners existent déjà, on les cherche tout de suite
            nextScanAt = firstWorld ? 2000 : 15000;
            if (world) firstWorld = false;
            scans = 0;
        }
        DWORD inLevel = GetTickCount() - levelSince;
        if (world) research_ai::tick();
        // (recherche du câble du grappin : research::tick(), à rebrancher pour la prochaine étape)
        if (world && scans < 4 && inLevel > nextScanAt && !level::inFrontEnd() && !level::complete()) {
            ++scans;
            level::scanForMissing();
            nextScanAt = inLevel + 20000u * (1u << scans);
        }
        {   // au menu principal ou en partie (coop échange les sauvegardes au menu)
            PoseShm* s = shm();
            if (s && s->magic == 0x5043454D) {
                s->gameState = !world ? 0 : (level::inFrontEnd() ? 1 : 2);
                static uint32_t lastMenu = 0;
                static bool firstMenu = true;
                uint32_t mr = s->menuReq;
                if (firstMenu) { lastMenu = mr; firstMenu = false; }
                if (mr != lastMenu) { lastMenu = mr; if (s->gameState == 2) menu::request((int)s->menuKind); }
            }
        }
        menu::monitorTick();
        {
        }
        if (GetTickCount() - lastStatus > 500) {
            lastStatus = GetTickCount();
            char st[200];
            ghosts::status(st, sizeof(st));
            char au[64], cb[80];
            audio::status(au, sizeof(au));
            combat::status(cb, sizeof(cb));
            size_t n = strlen(st);
            if (n + strlen(au) + 4 < sizeof(st)) { lstrcatA(st, " · "); lstrcatA(st, au); }
            if (strlen(st) + strlen(cb) + 4 < sizeof(st)) { lstrcatA(st, " · "); lstrcatA(st, cb); }
            char pg[64];
            progression::status(pg, sizeof(pg));
            if (pg[0] && strlen(st) + strlen(pg) + 4 < sizeof(st)) { lstrcatA(st, " · "); lstrcatA(st, pg); }
            publishStatus(st);
        }
        Sleep(50);
    }
}

void start() {
    // Un seul module 3D par partie : si une version plus ancienne de l'overlay est déjà dans le jeu
    // (mise à jour sans relancer le jeu), on ne double pas les crochets ni les fantômes.
    char name[64];
    wsprintfA(name, "Local\\MECCoop3D_%lu", GetCurrentProcessId());
    HANDLE m = CreateMutexA(NULL, FALSE, name);
    if (m && GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(m); g_alreadyRunning = true; return; }
    level::init();
    remote::g_m.init();
    ghosts::init();
    models::init();
    mission::init();
    research_ai::init();
    combat::init();
    progression::init();
    g_logLock.init();
    g_active = true;
    HANDLE t = CreateThread(NULL, 0, MonitorThread, NULL, 0, NULL);
    if (t) CloseHandle(t);
}
}  // namespace game
