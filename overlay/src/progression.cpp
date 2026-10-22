// =====================================================================================
//  MEC Coop - progression de l'histoire partagée
//
//  Le jeu garde l'histoire sous forme de « drapeaux » nommés (PamProgressionFlag : une valeur entière par
//  drapeau ; chaque mission a un drapeau « disponible » et un « terminée »). Lu dans le code du jeu :
//    gestionnaire de progression : *(0x14257c9d8) ; table des drapeaux : seaux à +0x20, nombre à +0x28 ;
//      un enregistrement : +0x00 empreinte, +0x10 PamProgressionFlag*, +0x18 valeur, +0x28 suivant
//    findFlag(gestionnaire, empreinte) 0x1439da170 -> PamProgressionFlag*
//    getFlagValue(gestionnaire, drapeau) 0x1439da880
//    setFlag(gestionnaire, drapeau, valeur, ajouter, prévenir, vérifierMission) 0x1439e3d90 : ce que le jeu
//      appelle lui-même ; il poste ensuite PamProgressionFlagUpdatedMessage (+0x48 empreinte, +0x4c valeur)
//      sur le gestionnaire de messages du serveur.
//  Chez moi : j'écoute ce message et j'envoie chaque changement à coop (qui ne le transmet que si j'héberge).
//  Chez l'autre : coop me donne les drapeaux de l'hôte, je les applique avec setFlag (thread serveur), sans
//  renvoyer les changements que je provoque ainsi.
// =====================================================================================
#include "game.h"
#include <stdio.h>

namespace game {
namespace progression {

static const uintptr_t kServerGameContext = 0x14253d670, Fn_RegisterListener = 0x142a3ad40;
static const uintptr_t kManagerGlobal = 0x14257c9d8;
static const uintptr_t Fn_FindFlag = 0x1439da170, Fn_GetFlagValue = 0x1439da880, Fn_SetFlag = 0x1439e3d90;
static const uint32_t kCatProgression = 0xae19b9ac;    // "PamProgression"
static const uint32_t kFlagUpdated = 0x241d89fe;       // "PamProgressionFlagUpdatedMessage"

struct Listener { void* const* vtbl; };
static void onMessage(Listener* self, const uint8_t* m);
static uint16_t priority(Listener*) { return 0; }
static void* const g_vtbl[2] = {(void*)&onMessage, (void*)&priority};
static Listener g_server = {g_vtbl};
static volatile uintptr_t g_serverDone = 0;
static volatile bool g_applying = false;   // je suis en train d'appliquer un drapeau de l'hôte (pas d'écho)
static volatile LONG g_mine = 0, g_applied = 0, g_missing = 0;

void init() {}

static void onMessage(Listener*, const uint8_t* m) {
    uint32_t type = 0;
    if (!read((uintptr_t)m + 12, type) || type != kFlagUpdated || g_applying) return;
    Flag f;
    if (!read((uintptr_t)m + 0x48, f.hash) || !read((uintptr_t)m + 0x4c, f.value) || !f.hash) return;
    publish(f);
    if (InterlockedIncrement(&g_mine) <= 10) logf("progression : drapeau %08x = %d", f.hash, f.value);
}

static void registerListener() {
    uintptr_t ctx = readOr<uintptr_t>(rva(kServerGameContext));
    if (!ctx || g_serverDone == ctx + 0x10) return;
    typedef char (*RegisterFn)(uintptr_t, uint32_t, Listener*, uint32_t);
    ((RegisterFn)rva(Fn_RegisterListener))(ctx + 0x10, kCatProgression, &g_server, 0xff);
    g_serverDone = ctx + 0x10;
    logf("progression : écoute des drapeaux de l'histoire");
}

// Toute ma table de drapeaux, envoyée par morceaux (quelqu'un vient de me rejoindre) : seaux à +0x20 (nombre à
// +0x28), chaque seau est une liste d'enregistrements (+0x00 empreinte, +0x18 valeur, +0x28 suivant).
static uint32_t g_snapSeen = 0;
static bool g_snapFirst = true, g_snapActive = false;
static uint32_t g_snapBucket = 0, g_snapCount = 0;
static uintptr_t g_snapNode = 0;
static void snapshotTick(uintptr_t mgr) {
    uint32_t req = snapshotRequest();
    if (g_snapFirst) { g_snapSeen = req; g_snapFirst = false; }
    if (req != g_snapSeen) {
        g_snapSeen = req;
        g_snapActive = true;
        g_snapBucket = 0;
        g_snapNode = 0;
        g_snapCount = 0;
        logf("progression : envoi de toute ma table de drapeaux demandé");
    }
    if (!g_snapActive) return;
    uintptr_t buckets = readOr<uintptr_t>(mgr + 0x20);
    uint32_t nb = readOr<uint32_t>(mgr + 0x28);
    if (!buckets || !nb || nb > 1000000) { g_snapActive = false; return; }
    int sent = 0, steps = 0;
    while (sent < 128 && steps < 4096) {
        steps++;
        if (!g_snapNode) {
            if (g_snapBucket >= nb) {
                g_snapActive = false;
                logf("progression : table envoyée (%u drapeaux)", g_snapCount);
                return;
            }
            g_snapNode = readOr<uintptr_t>(buckets + 8ull * g_snapBucket++);
            continue;
        }
        Flag f;
        if (!read(g_snapNode, f.hash) || !read(g_snapNode + 0x18, f.value)) { g_snapNode = 0; continue; }
        if (f.hash) { publish(f); sent++; g_snapCount++; }
        g_snapNode = readOr<uintptr_t>(g_snapNode + 0x28);
    }
}

// thread serveur : écoute, table demandée, et drapeaux de l'hôte à appliquer
void serverTick() {
    static DWORD lastTry = 0;
    if (GetTickCount() - lastTry > 1000) { lastTry = GetTickCount(); registerListener(); }
    uintptr_t mgr = readOr<uintptr_t>(rva(kManagerGlobal));
    if (!mgr || !readOr<uintptr_t>(mgr + 0x20) || level::inFrontEnd()) return;   // pas de partie : on garde pour plus tard
    snapshotTick(mgr);
    static Flag in[256];
    int n = take(in, 256, true);
    if (!n) return;
    typedef void* (*FindFn)(uintptr_t, uint32_t);
    typedef int (*GetFn)(uintptr_t, void*);
    typedef char (*SetFn)(uintptr_t, void*, int, char, char, char);
    FindFn find = (FindFn)rva(Fn_FindFlag);
    GetFn get = (GetFn)rva(Fn_GetFlagValue);
    SetFn set = (SetFn)rva(Fn_SetFlag);
    for (int i = 0; i < n; i++) {
        void* flag = find(mgr, in[i].hash);
        if (!flag) {
            if (InterlockedIncrement(&g_missing) <= 10) logf("progression : drapeau %08x inconnu ici", in[i].hash);
            continue;
        }
        if (get(mgr, flag) == in[i].value) continue;
        g_applying = true;
        set(mgr, flag, in[i].value, 0, 0, 0);
        g_applying = false;
        if (InterlockedIncrement(&g_applied) <= 20) logf("progression : drapeau de l'hôte %08x = %d appliqué", in[i].hash, in[i].value);
    }
    consumed(n);
    setStats((uint32_t)g_applied, (uint32_t)g_missing);
}

void status(char* out, size_t cap) {
    if (g_applied || g_missing) _snprintf(out, cap, "progression : %ld drapeaux de l'hôte", g_applied);
    else out[0] = 0;
}

}  // namespace progression
}  // namespace game
