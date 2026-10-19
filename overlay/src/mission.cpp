// =====================================================================================
//  MEC Coop - missions : savoir quand JE commence / finis une mission, pour que les autres
//  puissent me suivre (même mission = mêmes cinématiques).
//
//  Le jeu annonce le début et la fin d'une mission avec ses messages de télémétrie,
//  PamTelemetryMissionStartedMessage / PamTelemetryMissionEndedMessage (catégorie PamTelemetry,
//  gestionnaire de messages du client). On les écoute avec un objet "listener" à nous, enregistré
//  par la fonction du jeu MessageManager_registerListener : aucune ligne du jeu n'est modifiée.
//  Méthode et adresses de l'écoute : mod MECMP (licence MIT, © 2026 MECMP contributors,
//  src/game/runs.cpp). Contenu des messages (lu dans le code de télémétrie du jeu, 0x143a00973) :
//    début : +0x48 nom de la mission (char*, champ "mnam"), +0x50 type (char*, champ "type")
//    fin   : +0x48 nom, +0x50 type, +0x58 résultat (entier), +0x5c durée (float)
// =====================================================================================
#include "game.h"

namespace game {
namespace mission {

static const uintptr_t kClientGameContext = 0x142401cb0;   // *ptr + 0x10 = gestionnaire de messages du client
static const uintptr_t kServerGameContext = 0x14253d670;   // *ptr + 0x10 = celui du serveur
static const uintptr_t Fn_RegisterListener = 0x142a3ad40;
static const uint32_t kCatTelemetry = 0x258e0996;          // "PamTelemetry" (hash djb2-xor)
static const uint32_t kCatProgression = 0xae19b9ac;        // "PamProgression"
static const uint32_t kMissionStarted = 0xf30a1bd0, kMissionEnded = 0x8bfba07f;

struct Name { uint32_t type; const char* name; };
static const Name kNames[] = {
#include "mission_names.inc"
};
static const char* nameOf(uint32_t t) {
    for (size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); i++)
        if (kNames[i].type == t) return kNames[i].name;
    return "?";
}

struct Listener { void* const* vtbl; };
static void onMessage(Listener* self, const uint8_t* m);
static uint16_t priority(Listener*) { return 0; }
static void* const g_vtbl[2] = {(void*)&onMessage, (void*)&priority};
static Listener g_client = {g_vtbl}, g_server = {g_vtbl};
static volatile uintptr_t g_clientDone = 0, g_serverDone = 0;

static Lock g_m;
static State g_state;            // ma mission (sous g_m)
static DWORD g_lastStart = 0;

// mes positions des dernières secondes : on garde où j'étais AVANT le début (la cinématique peut me déplacer)
struct Sample { DWORD at; uintptr_t world; float pos[3]; };
static Sample g_ring[32];
static int g_head = 0, g_fill = 0;

void init() { g_m.init(); memset(&g_state, 0, sizeof(g_state)); }

static bool registerOn(uintptr_t contextGlobal, Listener& l, volatile uintptr_t& done, const char* who) {
    uintptr_t ctx = readOr<uintptr_t>(rva(contextGlobal));
    if (!ctx) return false;
    uintptr_t manager = ctx + 0x10;
    if (done == manager) return true;
    typedef char (*RegisterFn)(uintptr_t manager, uint32_t category, Listener* listener, uint32_t filter);
    RegisterFn reg = (RegisterFn)rva(Fn_RegisterListener);
    if (&l == &g_client) reg(manager, kCatTelemetry, &l, 0xff);
    done = manager;
    logf("mission : écoute des messages de mission (%s)", who);
    return true;
}

void clientTick() { registerOn(kClientGameContext, g_client, g_clientDone, "client"); }

void serverTick(uintptr_t world, const float* pos) {
    static DWORD lastTry = 0;
    if (GetTickCount() - lastTry > 1000) { lastTry = GetTickCount(); registerOn(kServerGameContext, g_server, g_serverDone, "serveur"); }
    if (!pos) return;
    static DWORD lastSample = 0;
    DWORD now = GetTickCount();
    if (now - lastSample < 250) return;
    lastSample = now;
    Guard g(g_m);
    Sample& s = g_ring[g_head];
    s.at = now;
    s.world = world;
    memcpy(s.pos, pos, 12);
    g_head = (g_head + 1) % 32;
    if (g_fill < 32) g_fill++;
}

// où j'étais ~2 s avant maintenant (sous g_m)
static bool posBefore(DWORD now, float out[3]) {
    const Sample* best = NULL;
    for (int k = 0; k < g_fill; k++) {
        const Sample& s = g_ring[(g_head - 1 - k + 64) % 32];
        best = &s;
        if (now - s.at >= 2000) break;
    }
    if (!best) return false;
    memcpy(out, best->pos, 12);
    return true;
}

static void dump(const uint8_t* m, uint32_t cat, uint32_t type) {
    static uint32_t types[48] = {0};
    static int counts[48] = {0};
    int slot = -1;
    for (int i = 0; i < 48 && slot < 0; i++)
        if (types[i] == type || types[i] == 0) { types[i] = type; slot = i; }
    if (slot < 0 || counts[slot]++ >= 3) return;
    uint8_t raw[0x40];
    if (!read((uintptr_t)m + 0x40, raw, sizeof(raw))) return;
    char hex[0x40 * 3 + 1];
    for (int i = 0; i < 0x40; i++) wsprintfA(hex + i * 3, "%02x ", raw[i]);
    // chaînes pointées par les premiers champs (recherche : que contient le message ?)
    char strs[200] = "";
    for (int off = 0x40; off < 0x80; off += 8) {
        uintptr_t p = 0;
        char s[48];
        if (read((uintptr_t)m + off, p) && p > 0x10000 && readCString(p, s, sizeof(s)) && s[0]) {
            size_t n = strlen(strs);
            if (n < sizeof(strs) - 60) wsprintfA(strs + n, "+%x=\"%s\" ", off, s);
        }
    }
    logf("mission : message %s (%08x/%08x) +0x40: %s %s", nameOf(type), cat, type, hex, strs);
}

static void onMessage(Listener* self, const uint8_t* m) {
    uint32_t cat = 0, type = 0;
    if (!read((uintptr_t)m + 8, cat) || !read((uintptr_t)m + 12, type)) return;
    if (cat != kCatTelemetry && cat != kCatProgression) return;
    dump(m, cat, type);
    if (self != &g_client || cat != kCatTelemetry) return;
    if (type != kMissionStarted && type != kMissionEnded) return;
    char name[64] = "", kind[32] = "";
    uintptr_t pn = 0, pk = 0;
    if (read((uintptr_t)m + 0x48, pn) && pn) readCString(pn, name, sizeof(name));
    if (read((uintptr_t)m + 0x50, pk) && pk) readCString(pk, kind, sizeof(kind));
    DWORD now = GetTickCount();
    Guard g(g_m);
    if (type == kMissionStarted) {
        // les deux gestionnaires peuvent livrer le même message : un début répété tout de suite est le même
        if (g_state.active && strcmp(g_state.name, name) == 0 && now - g_lastStart < 3000) return;
        g_lastStart = now;
        g_state.active = 1;
        lstrcpynA(g_state.name, name, sizeof(g_state.name));
        lstrcpynA(g_state.kind, kind, sizeof(g_state.kind));
        g_state.havePos = posBefore(now, g_state.pos);
        g_state.seq++;
        logf("mission : début de « %s » (type %s), départ %.1f %.1f %.1f", name, kind, g_state.pos[0], g_state.pos[1],
             g_state.pos[2]);
    } else {
        int32_t outcome = readOr<int32_t>((uintptr_t)m + 0x58, -1);
        if (!g_state.active) return;
        g_state.active = 0;
        g_state.outcome = outcome;
        g_state.seq++;
        logf("mission : fin de « %s » (résultat %d)", name, outcome);
    }
}

State current() {
    Guard g(g_m);
    return g_state;
}

}  // namespace mission
}  // namespace game
