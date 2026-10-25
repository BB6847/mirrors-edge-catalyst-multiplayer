// =====================================================================================
//  MEC Coop - interactions partagées (v1) : « appuie sur F » (perturbateur sur un ventilateur, et les autres
//  zones où le jeu demande une touche). Ce que l'un déclenche est déclenché au même endroit chez l'autre.
//
//  Ce que le jeu fait (enregistrement GridNode du 23/10 + lecture de son code) :
//    PamServerSpatialEntryInputTriggerEntity (vtable 0x141c78c10) : une zone placée dans la ville.
//      +0xd0 : son déclencheur ; +0x1b0 : maillon de la liste de toutes les zones (tête : voir firstTrigger)
//    PamServerEntityInteractionComponent (celui du joueur) choisit la zone où il est, puis 0x143a6f4a0
//      (déclencheur, joueur) lit les touches et, quand il le faut, envoie les signaux de la zone :
//      événement construit par 0x143395700(évt, joueur, n° du signal), envoyé par la case 0x88 du déclencheur.
//      Ex. ventilateur : signal 1f51d15d -> la logique du niveau arrête le ventilateur (son, effet, animation).
//  Chez moi : la case 0x88 est remplacée par la nôtre, qui note (zone, signal) quand l'appel vient de 0x143a6f4a0.
//  Chez l'autre : la zone la plus proche de cette position (2 m) reçoit le même signal, envoyé de la même façon.
// =====================================================================================
#include "game.h"
#include <stdio.h>

namespace game {
namespace interact {

static const uintptr_t TI_Trigger = 0x142863740, Glob_Realm = 0x1425469e0;
static const uintptr_t Fn_ReadInput = 0x143a6f4a0, Fn_ReadInputEnd = 0x143a6f710;
static const uintptr_t Fn_EventCtor = 0x143395700, Fn_EventDtor = 0x142d4e9a0, Fn_Release = 0x142a3a9e0;
static const uintptr_t Fn_WorldTransform = 0x143a68a80;
static const size_t Trigger_Inner = 0xd0, Trigger_ListNode = 0x1b0, Slot_Fire = 0x88, Event_Id = 0x08, Event_Sender = 0x20;
static const float kMatchRadius = 2.f, kNearMe = 40.f;

typedef uintptr_t (*FireFn)(void*, void*, void*, void*);
static FireFn g_origFire = NULL;
static bool g_installed = false;
static int g_installTries = 0;
static Lock g_m;

struct Rec { void* trig; uint32_t id; };
static Rec g_rec[32];
static int g_nrec = 0;
static uint32_t g_key = 0;
static volatile LONG g_sent = 0, g_applied = 0, g_missed = 0;

void init() { g_m.init(); g_key = GetTickCount() * 2654435761u; }

static bool inGame(uintptr_t a) {
    uintptr_t b = moduleBase();
    return a >= b + 0x1000 && a < b + 0x5000000;
}

// première zone de la liste du jeu (celle que parcourt le composant d'interaction du joueur)
static uintptr_t firstNode() {
    uintptr_t realm = readOr<uintptr_t>(rva(Glob_Realm));
    if (!realm) return 0;
    int idx = readOr<int>(realm + 0x28);
    if (idx < 0 || idx > 8) return 0;
    return readOr<uintptr_t>(rva(TI_Trigger) + (uintptr_t)(idx + 6) * 16);
}

static bool worldPos(uintptr_t entity, float out[3]) {
    alignas(16) float lt[16];
    memset(lt, 0, sizeof(lt));
    ((void (*)(void*, float*))rva(Fn_WorldTransform))((void*)entity, lt);
    if (!finite(lt[12]) || !finite(lt[13]) || !finite(lt[14])) return false;
    memcpy(out, lt + 12, 12);
    return true;
}

static uintptr_t hookFire(void* trig, void* ev, void* a3, void* a4) {
    uintptr_t ra = (uintptr_t)__builtin_return_address(0);
    if (ev && ra >= rva(Fn_ReadInput) && ra < rva(Fn_ReadInputEnd)) {
        Guard g(g_m);
        if (g_nrec < 32) {
            g_rec[g_nrec].trig = trig;
            g_rec[g_nrec].id = readOr<uint32_t>((uintptr_t)ev + Event_Id);
            g_nrec++;
        }
    }
    return g_origFire(trig, ev, a3, a4);
}

static void install() {
    if (g_installed || g_installTries > 200) return;
    ++g_installTries;
    uintptr_t node = firstNode();
    if (!node) return;
    uintptr_t trig = node - Trigger_ListNode + Trigger_Inner;
    uintptr_t vt = readOr<uintptr_t>(trig);
    if (!inGame(vt)) return;
    uintptr_t slot = vt + Slot_Fire;
    uintptr_t cur = readOr<uintptr_t>(slot);
    if (cur == (uintptr_t)&hookFire) { g_installed = true; return; }
    if (!inGame(cur)) return;
    g_origFire = (FireFn)cur;                  // avant le remplacement : la case peut servir tout de suite
    if (!patchPointer(slot, (uintptr_t)&hookFire, NULL)) return;
    g_installed = true;
    logf("interactions : zones « touche F » suivies (déclencheur vt=%llx)",
         (unsigned long long)(vt - moduleBase() + 0x140000000ull));
}

// envoie le signal à une zone comme le jeu le fait (événement construit puis libéré de la même façon)
static bool fire(uintptr_t trig, uint32_t id) {
    void* me = level::playerCharacter();
    uintptr_t player = me ? readOr<uintptr_t>((uintptr_t)me + Character_Player) : 0;
    if (!player || !g_origFire) return false;
    alignas(16) uint8_t ev[0x40];
    memset(ev, 0, sizeof(ev));
    ((void (*)(void*, uintptr_t, uint32_t))rva(Fn_EventCtor))(ev, player, id);
    g_origFire((void*)trig, ev, NULL, NULL);
    uintptr_t ref = *(uintptr_t*)(ev + Event_Sender);
    if (ref && InterlockedDecrement((volatile LONG*)(ref + 8)) == 0) ((void (*)(uintptr_t))rva(Fn_Release))(ref);
    ((void (*)(void*))rva(Fn_EventDtor))(ev);
    return true;
}

// thread du jeu (serveur), à chaque tour
void tick() {
    install();
    if (!g_installed) return;
    Rec recs[32];
    int n;
    {
        Guard g(g_m);
        n = g_nrec;
        memcpy(recs, g_rec, n * sizeof(Rec));
        g_nrec = 0;
    }
    for (int i = 0; i < n; i++) {
        Event e;
        memset(&e, 0, sizeof(e));
        if (!worldPos((uintptr_t)recs[i].trig - Trigger_Inner, e.pos)) continue;
        e.id = recs[i].id;
        e.key = ++g_key;
        publish(e);
        if (InterlockedIncrement(&g_sent) <= 30)
            logf("interactions : signal %08x à la zone %.1f %.1f %.1f (envoyé aux autres)", e.id, e.pos[0], e.pos[1], e.pos[2]);
    }
    Event in[16];
    int m = take(in, 16);
    for (int i = 0; i < m; i++) {
        const Event& e = in[i];
        if (!finite(e.pos[0]) || !finite(e.pos[1]) || !finite(e.pos[2])) continue;
        uintptr_t best = 0;
        float bestD = kMatchRadius * kMatchRadius;
        int guard = 0;
        for (uintptr_t node = firstNode(); node && guard < 20000; node = readOr<uintptr_t>(node), ++guard) {
            uintptr_t ent = node - Trigger_ListNode;
            float p[3];
            if (!worldPos(ent, p)) continue;
            float d = 0;
            for (int a = 0; a < 3; a++) d += (p[a] - e.pos[a]) * (p[a] - e.pos[a]);
            if (d < bestD) { bestD = d; best = ent; }
        }
        if (!best) {
            if (InterlockedIncrement(&g_missed) <= 20)
                logf("interactions : signal %08x reçu pour %.1f %.1f %.1f, aucune zone à moins de %.0f m (%d zones)", e.id,
                     e.pos[0], e.pos[1], e.pos[2], kMatchRadius, guard);
            continue;
        }
        // seulement si je suis près de la zone : les départs de mission passent aussi par ces zones, et un signal
        // rejoué de loin lancerait la mission chez moi n'importe où (vu en lisant le code le 25/10)
        void* me = level::playerCharacter();
        LinearTransform mx;
        if (!me || !readEntityTransform(me, mx)) continue;
        float dm = 0;
        for (int a = 0; a < 3; a++) dm += (mx.trans[a] - e.pos[a]) * (mx.trans[a] - e.pos[a]);
        if (dm > kNearMe * kNearMe) {
            if (InterlockedIncrement(&g_missed) <= 20)
                logf("interactions : signal %08x reçu, zone à %.0f m de moi : pas rejoué (trop loin)", e.id, sqrtf(dm));
            continue;
        }
        bool ok = fire(best + Trigger_Inner, e.id);
        if (InterlockedIncrement(&g_applied) <= 30)
            logf("interactions : signal %08x reçu, %s à la zone à %.1f m", e.id, ok ? "envoyé" : "PAS envoyé (pas de joueur)",
                 sqrtf(bestD));
    }
}

void status(char* out, size_t cap) {
    if (!g_installed) { lstrcpynA(out, "interactions : en attente", (int)cap); return; }
    _snprintf(out, cap, "interactions : %ld envoyées, %ld reçues", g_sent, g_applied);
}

}  // namespace interact
}  // namespace game
