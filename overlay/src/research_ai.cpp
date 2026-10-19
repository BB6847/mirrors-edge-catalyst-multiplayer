// =====================================================================================
//  MEC Coop - recherche (temporaire) : les ennemis, les coups, les K.O.
//  Écoute les messages du jeu sur les dégâts et les morts (catégories ServerCharacter, ServerPlayer,
//  PamAI, CharacterToComponents, UI, ClientGameplay) et note leur contenu dans le journal, ainsi que les
//  modèles de tous les personnages que le niveau sait faire apparaître. Lecture seule : rien n'est modifié.
//  (Écoute des messages : méthode du mod MECMP, licence MIT, src/game/runs.cpp.)
// =====================================================================================
#include "game.h"
#include <stdio.h>

namespace game {
namespace research_ai {

static const uintptr_t kClientGameContext = 0x142401cb0, kServerGameContext = 0x14253d670;
static const uintptr_t Fn_RegisterListener = 0x142a3ad40;

struct Cat { uint32_t hash; const char* name; bool server; };
static const Cat kCats[] = {
    {0x1286cf79, "ServerCharacter", true}, {0xf3c93dd3, "ServerPlayer", true}, {0x0dd94531, "PamAI", true},
    {0x6982747b, "CharacterToComponents", true}, {0x005973b9, "UI", false}, {0x508f1eb6, "ClientGameplay", false},
    {0xc6e40ba0, "Server", true}, {0x0059722d, "AI", true}, {0x0059722d, "AI", false}};
struct Name { uint32_t type; const char* name; };
static const Name kTypes[] = {
    {0x8d5dbfa0, "ServerCharacterCharacterDamage"}, {0x6ee49cf1, "ServerCharacterKilled"},
    {0xf1440ad2, "CharacterToComponentsOnKilled"}, {0x1a1457c2, "ClientGameplaySoldierHit"},
    {0x1044dbda, "ClientGameplaySoldierHealthRequest"}, {0x0599f06b, "UIDamageGivenToEnemy"},
    {0x6042cde2, "UIHasSuppressedEnemy"}, {0x8c0ff818, "UISoldierHitUpdated"}, {0xc7abb9db, "ServerPlayerKilled"},
    {0xacf2beec, "PamAIRemoveAICorpse"}, {0x83f6f3c9, "PamAIStunned"}, {0xe74ad5c1, "AIDirectorState"},
    {0x94a37fc4, "AISpawnBot"}, {0x63c0f317, "AIPlayerEnableAsTarget"}, {0xc46769e9, "PamAIFireOnceAtPosition"},
    {0x079f0fef, "NetworkPamRegisterAIKill"}, {0x63c0f317, "AIPlayerEnableAsTarget"}};
static const char* typeName(uint32_t t) {
    for (size_t i = 0; i < sizeof(kTypes) / sizeof(kTypes[0]); i++) if (kTypes[i].type == t) return kTypes[i].name;
    return NULL;
}

struct Listener { void* const* vtbl; };
static void onMessage(Listener* self, const uint8_t* m);
static uint16_t priority(Listener*) { return 0; }
static void* const g_vtbl[2] = {(void*)&onMessage, (void*)&priority};
static Listener g_client = {g_vtbl}, g_server = {g_vtbl};
static volatile uintptr_t g_clientDone = 0, g_serverDone = 0;
static volatile LONG g_lines = 0;
static const LONG kMaxLines = 600;

static void registerOn(uintptr_t ctxGlobal, Listener& l, volatile uintptr_t& done, bool server) {
    uintptr_t ctx = readOr<uintptr_t>(rva(ctxGlobal));
    if (!ctx || done == ctx + 0x10) return;
    typedef char (*RegisterFn)(uintptr_t, uint32_t, Listener*, uint32_t);
    RegisterFn reg = (RegisterFn)rva(Fn_RegisterListener);
    for (size_t i = 0; i < sizeof(kCats) / sizeof(kCats[0]); i++)
        if (kCats[i].server == server) reg(ctx + 0x10, kCats[i].hash, &l, 0xff);
    done = ctx + 0x10;
    logf("recherche ennemis : écoute des messages (%s)", server ? "serveur" : "client");
}
void clientTick() { registerOn(kClientGameContext, g_client, g_clientDone, false); }
void serverTick() {
    static DWORD last = 0;
    if (GetTickCount() - last < 1000) return;
    last = GetTickCount();
    registerOn(kServerGameContext, g_server, g_serverDone, true);
}

static const char* catName(uint32_t c) {
    for (size_t i = 0; i < sizeof(kCats) / sizeof(kCats[0]); i++) if (kCats[i].hash == c) return kCats[i].name;
    return "?";
}

// décrit un pointeur : objet du jeu (vtable), chaîne, ou rien
static void describe(uintptr_t p, char* out, size_t cap) {
    out[0] = 0;
    if (p < 0x10000 || inImage(p)) return;
    uintptr_t vt = 0;
    char s[64];
    if (read(p, vt) && inImage(vt)) {
        uintptr_t data = readOr<uintptr_t>(p + 0x30), bp = 0, nm = 0;
        char name[96] = "";
        // personnage : +0x30 données -> +0x18 blueprint -> +0x10 nom ; sinon données +0x28
        for (size_t off = 0x28; off <= 0x30 && !name[0]; off += 8) {
            data = readOr<uintptr_t>(p + off);
            if (data && read(data + 0x18, bp) && bp && read(bp + 0x10, nm) && nm) readCString(nm, name, sizeof(name));
            if (name[0] && !strchr(name, '/')) name[0] = 0;
        }
        _snprintf(out, cap, "obj vt=%llx%s%s", (unsigned long long)(vt - moduleBase() + 0x140000000ull), name[0] ? " " : "",
                  name);
    } else if (readCString(p, s, sizeof(s)) && s[0]) {
        _snprintf(out, cap, "\"%s\"", s);
    }
}

static void onMessage(Listener* self, const uint8_t* m) {
    uint32_t cat = 0, type = 0;
    if (!read((uintptr_t)m + 8, cat) || !read((uintptr_t)m + 12, type)) return;
    const char* tn = typeName(type);
    // UI et ClientGameplay sont bavards : seulement les messages de coups
    if (!tn && (cat == 0x005973b9 || cat == 0x508f1eb6 || cat == 0xc6e40ba0)) return;
    static uint32_t seen[96];
    static int counts[96];
    int slot = -1;
    for (int i = 0; i < 96 && slot < 0; i++) if (seen[i] == type || seen[i] == 0) { seen[i] = type; slot = i; }
    int limit = tn ? 12 : 0;      // les messages inconnus ont déjà été notés (journal plus léger)
    if (slot < 0 || counts[slot]++ >= limit || g_lines > kMaxLines) return;
    uint8_t raw[0x80];
    if (!read((uintptr_t)m + 0x10, raw, sizeof(raw))) return;
    char hex[0x80 * 3 + 1];
    for (int i = 0; i < 0x80; i++) wsprintfA(hex + i * 3, "%02x ", raw[i]);
    InterlockedIncrement(&g_lines);
    logf("ennemis : %s %s/%s (%08x) %s +0x10: %s", self == &g_server ? "S" : "C", catName(cat), tn ? tn : "?", type,
         (uintptr_t)m >= 0x10000 ? "" : "", hex);
    for (int off = 0x10; off < 0x90; off += 8) {
        uint64_t q;
        memcpy(&q, raw + off - 0x10, 8);
        char d[160];
        describe((uintptr_t)q, d, sizeof(d));
        if (d[0]) { logf("ennemis :    +%x -> %s", off, d); InterlockedIncrement(&g_lines); }
    }
    // floats plausibles (dégâts, santé, positions)
    char fl[400] = "";
    size_t n = 0;
    for (int off = 0x10; off < 0x90 && n < sizeof(fl) - 40; off += 4) {
        float f;
        memcpy(&f, raw + off - 0x10, 4);
        if (finite(f) && fabsf(f) >= 0.01f && fabsf(f) < 100000.f) n += _snprintf(fl + n, sizeof(fl) - n, " +%x=%.2f", off, f);
    }
    if (n) logf("ennemis :    nombres :%s", fl);
}

// spawners d'autres personnages que Faith et les fantômes (les ennemis, a priori)
struct Sp { void* sp; char name[80]; int last; };
static Sp g_sps[256];
static int g_nsp = 0;
static Lock g_spLock;
static bool g_lockInit = false;

void init() { g_spLock.init(); g_lockInit = true; }

void tick() {   // fil de surveillance : qui apparaît, qui disparaît
    static DWORD last = 0;
    if (GetTickCount() - last < 2000 || !g_lockInit) return;
    last = GetTickCount();
    Guard g(g_spLock);
    for (int i = 0; i < g_nsp; i++) {
        Sp& e = g_sps[i];
        if (!level::spawnerAlive(e.sp)) { if (e.last >= 0) e.last = -1; continue; }
        uintptr_t head[Spawner_Spawned / 8 + 2];
        if (!read((uintptr_t)e.sp, head, sizeof(head))) continue;
        uintptr_t b = head[Spawner_Spawned / 8], en = head[Spawner_Spawned / 8 + 1];
        int n = (b && en > b && (en - b) / 8 < 64) ? (int)((en - b) / 8) : 0;
        if (n == e.last || g_lines > kMaxLines) continue;
        e.last = n;
        char where[160] = "";
        if (n) {
            uintptr_t c = readOr<uintptr_t>(b);
            LinearTransform xf;
            uintptr_t hc = c ? readOr<uintptr_t>(c + 0x198) : 0, hvt = hc ? readOr<uintptr_t>(hc) : 0;
            if (c && readEntityTransform((void*)c, xf))
                _snprintf(where, sizeof(where), " ; 1er %p vt=%llx à %.1f %.1f %.1f, +0x198 -> vt=%llx", (void*)c,
                          (unsigned long long)(readOr<uintptr_t>(c) - moduleBase() + 0x140000000ull), xf.trans[0], xf.trans[1],
                          xf.trans[2], (unsigned long long)(hvt ? hvt - moduleBase() + 0x140000000ull : 0));
        }
        logf("ennemis : %s : %d personnage(s)%s", e.name, n, where);
        InterlockedIncrement(&g_lines);
    }
}

// modèles de personnages que le niveau sait faire apparaître (pour repérer les ennemis)
void noteSpawner(const char* blueprint, void* spawner) {
    if (g_lockInit && strcmp(blueprint, kPlayerBlueprint) && strcmp(blueprint, kGhostBlueprint)) {
        Guard g(g_spLock);
        bool known = false;
        for (int i = 0; i < g_nsp && !known; i++) known = g_sps[i].sp == spawner;
        if (!known && g_nsp < 256) {
            g_sps[g_nsp].sp = spawner;
            lstrcpynA(g_sps[g_nsp].name, blueprint, 80);
            g_sps[g_nsp].last = 0;
            g_nsp++;
        }
    }
    static char names[160][80];
    static int count = 0;
    for (int i = 0; i < count; i++) if (strcmp(names[i], blueprint) == 0) return;
    if (count >= 160) return;
    lstrcpynA(names[count++], blueprint, 80);
    logf("ennemis : modèle de personnage du niveau : %s (spawner %p)", blueprint, spawner);
}

}  // namespace research_ai
}  // namespace game
