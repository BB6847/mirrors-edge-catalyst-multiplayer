// =====================================================================================
//  MEC Coop - les autres joueurs sur la carte du jeu, comme des amis (d'après MECMP 0.9.0-beta.3, licence MIT :
//  game/players_map.cpp). Le jeu place ses marqueurs d'amis avec PresenceInfoManager_assignSlot(gestionnaire,
//  OnlineId*, Vec4* position alignée sur 16, priorité) ; l'icône est le Runner Tag du profil EA : chaque joueur
//  partage son profil (OnlineId lu en *0x14257cc68 + 0x30) par coop. Thread client (mise à jour du jeu).
// =====================================================================================
#include "game.h"

namespace game {
namespace playersmap {

static const uintptr_t Slot_PoolUpdate = 0x141c74bc8, Fn_PoolUpdate = 0x143a4bb30;
static const uintptr_t Vtbl_Manager = 0x141c74be8, Fn_AssignSlot = 0x143a405e0, kOwnPresence = 0x14257cc68;
static const size_t Mgr_Slots = 0x50, Slot_Size = 0x50, Slot_Shown = 0x00, Slot_Priority = 0x04, Slot_OnlineId = 0x08,
                    Slot_Pos = 0x30, Pool_Entries = 0x28, Pool_EntrySize = 0x10;
static const int kPriority = 1000;            // au-dessus des amis du jeu (0)
static const uint32_t kShowAfterMs = 5000;

#pragma pack(push, 1)
struct OnlineId { uint64_t id; char name[32]; };
#pragma pack(pop)
static_assert(sizeof(OnlineId) == 0x28, "OnlineId");

typedef void (*PoolUpdateFn)(void*, void*);
static PoolUpdateFn g_orig = NULL;
struct Placed { uintptr_t mgr; uint64_t id; };
static Placed g_placed[32];
static int g_nplaced = 0;

static bool ownOnlineId(OnlineId& out) {
    uintptr_t p = readOr<uintptr_t>(rva(kOwnPresence));
    if (!p || !read(p + 0x30, &out, sizeof(out)) || !out.id) return false;
    out.name[31] = 0;
    return out.name[0] != 0;
}

void publishOwnProfile() {
    static DWORD last = 0;
    if (GetTickCount() - last < 2000) return;
    last = GetTickCount();
    OnlineId own;
    memset(&own, 0, sizeof(own));
    bool ok = ownOnlineId(own);
    if (ok) setOwnProfile(own.id, own.name);
    else setOwnProfile(0, "");
    static int lastState = -1;
    if ((int)ok != lastState) {
        lastState = ok;
        if (ok) logf("carte : mon profil EA : %s", own.name);
        else logf("carte : pas de profil EA trouvé (jeu hors ligne ?) : pas de marqueurs");
    }
}

static bool placed(uintptr_t mgr, uint64_t id) {
    for (int i = 0; i < g_nplaced; i++) if (g_placed[i].mgr == mgr && g_placed[i].id == id) return true;
    return false;
}
static void unplace(uintptr_t mgr, uint64_t id) {
    for (int i = 0; i < g_nplaced; i++)
        if (g_placed[i].mgr == mgr && g_placed[i].id == id) { g_placed[i] = g_placed[--g_nplaced]; return; }
}
static void hideSlot(uintptr_t slot) {
    poke<uint8_t>(slot + Slot_Shown, 0);
    poke<int>(slot + Slot_Priority, 0);
    OnlineId none;
    memset(&none, 0, sizeof(none));
    poke(slot + Slot_OnlineId, &none, sizeof(none));
}

static void updateManager(uintptr_t mgr, bool on) {
    uintptr_t range[2] = {0, 0};
    if (!read(mgr + Mgr_Slots, range, sizeof(range)) || range[1] <= range[0] || (range[1] - range[0]) % Slot_Size ||
        (range[1] - range[0]) / Slot_Size > 64)
        return;
    size_t count = (range[1] - range[0]) / Slot_Size;
    static uint8_t slots[64 * Slot_Size];
    if (!read(range[0], slots, count * Slot_Size)) return;
    struct Want { uint32_t peer; OnlineId oid; };
    Want want[8];
    int nw = 0;
    if (on) {
        remote::Info all[8];
        size_t na = remote::list(all, 8);
        for (size_t i = 0; i < na && nw < 8; i++) {
            if (all[i].ageMs >= kShowAfterMs) continue;
            Want& w = want[nw];
            memset(&w, 0, sizeof(w));
            w.peer = all[i].id;
            if (profileOf(all[i].id, w.oid.id, w.oid.name)) { nw++; continue; }
            // sans profil EA (serveurs du jeu hors ligne, 25/10) : un faux profil à son pseudo. Le jeu n'aura pas
            // de Runner Tag à afficher, mais le marqueur peut quand même apparaître (essai).
            w.oid.id = 0x4d45430000000000ull | all[i].id;        // « MEC » + n° du joueur : jamais un vrai profil
            lstrcpynA(w.oid.name, all[i].name[0] ? all[i].name : "Ami", sizeof(w.oid.name));
            nw++;
        }
    }
    static int lastNw = -1;
    static size_t lastCount = (size_t)-1;
    if (nw != lastNw || count != lastCount) {
        lastNw = nw;
        lastCount = count;
        logf("carte : %d joueur(s) à montrer, %u emplacement(s) d'amis dans ce gestionnaire (montrer : %s)", nw,
             (unsigned)count, on ? "oui" : "non");
    }
    for (size_t i = 0; i < count; i++) {          // ceux qui sont partis : emplacement rendu au jeu
        uint64_t id = 0;
        memcpy(&id, slots + i * Slot_Size + Slot_OnlineId, 8);
        if (!id || !placed(mgr, id)) continue;
        bool keep = false;
        for (int k = 0; k < nw; k++) keep = keep || want[k].oid.id == id;
        if (!keep) { hideSlot(range[0] + i * Slot_Size); unplace(mgr, id); }
    }
    for (int k = 0; k < nw; k++) {
        PlayerState st;
        uint32_t age = 0;
        if (!remote::state(want[k].peer, st, age) || age >= kShowAfterMs) continue;
        uintptr_t slot = 0;
        for (size_t i = 0; i < count && !slot; i++) {
            uint64_t id = 0;
            memcpy(&id, slots + i * Slot_Size + Slot_OnlineId, 8);
            if (id == want[k].oid.id) slot = range[0] + i * Slot_Size;
        }
        alignas(16) float pos[4] = {st.pos[0], st.pos[1], st.pos[2], 0.f};   // MOVAPS dans le jeu : aligné sur 16
        if (!slot || !placed(mgr, want[k].oid.id)) {
            typedef void* (*AssignFn)(void*, const OnlineId*, const float*, int);
            slot = (uintptr_t)((AssignFn)rva(Fn_AssignSlot))((void*)mgr, &want[k].oid, pos, kPriority);
            if (!slot) {
                static DWORD lastNo = 0;
                if (GetTickCount() - lastNo > 10000) { lastNo = GetTickCount(); logf("carte : le jeu refuse un emplacement pour %s", want[k].oid.name); }
                continue;
            }
            if (g_nplaced < 32) g_placed[g_nplaced++] = {mgr, want[k].oid.id};
            logf("carte : %s est sur la carte", want[k].oid.name);
        }
        poke(slot + Slot_Pos, pos, sizeof(pos));
        poke<uint8_t>(slot + Slot_Shown, 1);     // remis à chaque image : la vérification de confidentialité l'efface
    }
}

static void poolUpdate(void* pool, void* info) {
    DWORD err = GetLastError();
    publishOwnProfile();
    static bool first = true;
    if (first) { first = false; logf("carte : le jeu met à jour ses marqueurs d'amis"); }
    uintptr_t pl = (uintptr_t)pool, range[2] = {0, 0};
    if (pl && read(pl + Pool_Entries, range, sizeof(range)) && range[1] > range[0] && (range[1] - range[0]) / Pool_EntrySize <= 16) {
        size_t n = (range[1] - range[0]) / Pool_EntrySize;
        uintptr_t entries[32];
        if (read(range[0], entries, n * Pool_EntrySize)) {
            bool on = enabled();
            for (int i = 0; i < g_nplaced;) {      // nouveau niveau : gestionnaires disparus
                bool still = false;
                for (size_t k = 0; k < n; k++) still = still || entries[k * 2] == g_placed[i].mgr;
                if (still) ++i; else g_placed[i] = g_placed[--g_nplaced];
            }
            for (size_t k = 0; k < n; k++) {
                uintptr_t mgr = entries[k * 2];
                if (mgr && readOr<uintptr_t>(mgr) == rva(Vtbl_Manager)) updateManager(mgr, on);
            }
        }
    }
    SetLastError(err);
    g_orig(pool, info);
}

void install() {
    uintptr_t slot = rva(Slot_PoolUpdate), cur = 0;
    if (!read(slot, cur) || cur != rva(Fn_PoolUpdate)) { logf("carte : case inattendue, pas de joueurs sur la carte"); return; }
    g_orig = (PoolUpdateFn)cur;
    if (patchPointer(slot, (uintptr_t)&poolUpdate, NULL)) logf("carte : les autres joueurs seront montrés sur la carte");
}

}  // namespace playersmap
}  // namespace game
