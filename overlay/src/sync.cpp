// =====================================================================================
//  MEC Coop - synchronisation : heure du jour (celle de l'hôte) et téléportation vers un joueur
//
//  Méthode et adresses reprises du mod MECMP (licence MIT) :
//    Copyright (c) 2026 MECMP contributors - https://github.com/CodeNameMeteor/mecmp
// =====================================================================================
#include "game.h"
#include <stdio.h>

namespace game {

// ---------------------------------------------------------------- rayon (pour trouver le sol)
namespace raycast {
static const uintptr_t Fn_RaycastQuery = 0x143a24890;
static const size_t World_PhysicsManager = 0x50, PhysManager_Holder = 0xf0, PhysHolder_Physics = 0x18;
static const size_t Phys_HavokWorld = 0x10, HavokWorld_FilterOwner = 0x150, FilterOwner_Filter = 0x4d0;
static volatile int g_available = -1;

struct alignas(16) Query { float from[4]; float to[4]; float radius; uint8_t flags[3]; uint8_t pad[9]; };
struct alignas(16) Result {
    float position[4]; float normal[4]; uint32_t id; float fraction; float distance; uint8_t hit;
    uint8_t pad2d[3]; uint16_t tail; uint8_t pad[0x0e];
};
static_assert(sizeof(Query) == 0x30 && offsetof(Query, radius) == 0x20, "Query");
static_assert(sizeof(Result) == 0x40 && offsetof(Result, id) == 0x20 && offsetof(Result, hit) == 0x2c, "Result");

static bool available() {
    if (g_available >= 0) return g_available == 1;
    static const uint8_t kExpect[] = {0x48, 0x89, 0xE0, 0x55, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D,
                                      0xA8, 0x08, 0xFF, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0xD0, 0x01, 0x00, 0x00};
    uint8_t got[sizeof(kExpect)];
    bool ok = read(rva(Fn_RaycastQuery), got, sizeof(got)) && memcmp(got, kExpect, sizeof(got)) == 0;
    if (!ok) logf("rayon : fonction différente dans cette version du jeu (téléportation sans recherche du sol)");
    g_available = ok ? 1 : 0;
    return ok;
}
static uintptr_t physics(uintptr_t world) {
    uintptr_t manager = readOr<uintptr_t>(world + World_PhysicsManager);
    uintptr_t holder = manager ? readOr<uintptr_t>(manager + PhysManager_Holder) : 0;
    uintptr_t inner = holder ? readOr<uintptr_t>(holder) : 0;
    uintptr_t phys = inner ? readOr<uintptr_t>(inner + PhysHolder_Physics) : 0;
    if (!phys || !inImage(readOr<uintptr_t>(phys))) return 0;
    uintptr_t havok = readOr<uintptr_t>(phys + Phys_HavokWorld);
    uintptr_t owner = havok ? readOr<uintptr_t>(havok + HavokWorld_FilterOwner) : 0;
    if (!owner || !readOr<uintptr_t>(owner + FilterOwner_Filter)) return 0;
    return phys;
}
// lance un rayon ; vrai si il touche quelque chose (position du point touché)
static bool cast(const float from[3], const float to[3], float hit[3]) {
    if (!available()) return false;
    uintptr_t world = level::serverWorld();
    uintptr_t phys = world ? physics(world) : 0;
    if (!phys) return false;
    Query q;
    Result r;
    memset(&q, 0, sizeof(q));
    memset(&r, 0, sizeof(r));
    memcpy(q.from, from, 12);
    memcpy(q.to, to, 12);
    ((void (*)(const Query*, void*, void*, Result*))rva(Fn_RaycastQuery))(&q, (void*)world, (void*)phys, &r);
    if (r.hit != 1 || !finite(r.position[0]) || !finite(r.position[1]) || !finite(r.position[2])) return false;
    memcpy(hit, r.position, 12);
    return true;
}
}  // namespace raycast

// ---------------------------------------------------------------- heure du jour
namespace tod {
static const uintptr_t kClockUpdaters = 0x142598ac0, Vtbl_ClockUpdater = 0x141cd82a0;
static const size_t Updater_Clock = 0x28, Clock_Time = 0x30;
static const float kDay = 86400.f, kTolerance = 5.f, kMaxRate = 120.f;
static const DWORD kBackOffMs = 5 * 60 * 1000;

struct Watch { float last; DWORD lastAt; bool running; DWORD gameSetAt; };
static Watch g_w = {-1.f, 0, false, 0};
static volatile int g_state = 0;   // 0 rien, 1 hôte, 2 suit l'hôte, 3 désactivé, 4 le jeu règle l'heure (mission)

static uintptr_t timeField(int realm) {
    uintptr_t updater = readOr<uintptr_t>(rva(kClockUpdaters) + (uintptr_t)realm * 8);
    if (!updater || readOr<uintptr_t>(updater) != rva(Vtbl_ClockUpdater)) return 0;
    uintptr_t clock = readOr<uintptr_t>(updater + Updater_Clock);
    float t = -1.f;
    if (!clock || !read(clock + Clock_Time, t) || !finite(t) || t < 0.f || t > kDay) return 0;
    return clock + Clock_Time;
}
static bool current(float& s) {
    uintptr_t f = timeField(1);
    if (!f) f = timeField(0);
    return f && read(f, s);
}
static float apart(float a, float b) { float d = fabsf(a - b); return d > kDay * 0.5f ? kDay - d : d; }

static void watch(float now, DWORD tick) {
    if (g_w.last >= 0.f && g_w.lastAt) {
        float d = now - g_w.last;
        if (d < -kDay * 0.5f) d += kDay;
        float elapsed = (float)(tick - g_w.lastAt) / 1000.f;
        g_w.running = d > 0.01f && d <= kMaxRate * elapsed + 1.f;
        if (fabsf(d) > kMaxRate * elapsed + 1.f) g_w.gameSetAt = tick | 1;   // le jeu a sauté l'heure lui-même
    }
    g_w.last = now;
    g_w.lastAt = tick;
}

// une fois par seconde. role : 1 hôte, 2 rejoint. Renvoie mon heure à envoyer (-1 = rien).
float tick(int role, bool follow, float hostTime) {
    static DWORD lastAt = 0;
    static float lastSend = -1.f;
    DWORD now = GetTickCount();
    if (now - lastAt < 1000) return lastSend;
    lastAt = now;
    bool inLevel = level::serverWorld() && !level::inFrontEnd();
    float mine = -1.f;
    bool have = inLevel && current(mine);
    if (have) watch(mine, now);
    else { g_w.last = -1.f; g_w.lastAt = 0; g_w.running = false; g_w.gameSetAt = 0; }
    bool gameSets = have && (!g_w.running || (g_w.gameSetAt && now - g_w.gameSetAt < kBackOffMs));
    if (role == 1) {
        g_state = !have ? 0 : gameSets ? 4 : 1;
        lastSend = have && !gameSets ? mine : -1.f;
        return lastSend;
    }
    lastSend = -1.f;
    if (role != 2 || !have || hostTime < 0.f) { g_state = 0; return -1.f; }
    if (!follow) { g_state = 3; return -1.f; }
    if (gameSets) { g_state = 4; return -1.f; }
    g_state = 2;
    if (apart(hostTime, mine) <= kTolerance) return -1.f;
    bool wrote = false;
    for (int realm = 1; realm >= 0; realm--) {
        uintptr_t f = timeField(realm);
        if (f) wrote = write(f, &hostTime, 4) || wrote;
    }
    if (wrote) {
        g_w.last = hostTime;
        g_w.lastAt = now;
        if (apart(hostTime, mine) > 60.f)
            logf("heure : réglée sur celle de l'hôte %02d:%02d (était %02d:%02d)", (int)(hostTime / 3600), (int)(fmodf(hostTime, 3600) / 60),
                 (int)(mine / 3600), (int)(fmodf(mine, 3600) / 60));
    }
    return -1.f;
}
const char* status() {
    switch (g_state) {
    case 1: return "tu héberges : les autres ont ton heure";
    case 2: return "même heure que l'hôte";
    case 3: return "ton heure à toi (synchro désactivée)";
    case 4: return "en pause : le jeu règle l'heure (mission ?)";
    default: return "";
    }
}
}  // namespace tod

// ---------------------------------------------------------------- téléportation
namespace teleport {
static const uintptr_t Fn_ServerCharacterEntity_teleport = 0x143378830;
static const DWORD kFadeMs = 350, kHoldMs = 1200, kSettleMs = 400, kMaxHoldMs = 8000, kStuckMs = 4000;
static const float kProbeUp = 1.0f, kProbeDown = 40.f, kProbeAccept = 0.6f, kFarGround = 2.f, kLift = 0.05f;
static const float kFallMargin = 0.5f, kNearTarget = 30.f;
static const int kProbeCasts = 3, kMaxRetries = 10;

enum { Idle, FadeOut, Hold, FadeIn };
static volatile LONG g_phase = Idle;
static volatile DWORD g_phaseAt = 0;
static volatile int g_available = -1;
static uint32_t g_peer = 0;
static float g_fixed[3];
static bool g_useFixed = false;
static LinearTransform g_target, g_from;
static bool g_haveFrom = false;
static void* g_character = NULL;
static int g_retries = 0;
static DWORD g_groundAt = 0, g_steadyAt = 0;
static volatile LONG g_result = 0;   // 1 réussi, 2 échoué

static void setPhase(LONG p) { g_phaseAt = GetTickCount(); g_phase = p; }

static bool available() {
    if (g_available >= 0) return g_available == 1;
    static const uint8_t kExpect[] = {0x48, 0x89, 0xCF, 0x48, 0x8D, 0x99, 0x10, 0x04, 0x00, 0x00};
    uint8_t got[sizeof(kExpect)];
    bool ok = read(rva(Fn_ServerCharacterEntity_teleport) + 24, got, sizeof(got)) && memcmp(got, kExpect, sizeof(got)) == 0;
    if (!ok) logf("téléportation : fonction différente dans cette version du jeu, désactivée");
    g_available = ok ? 1 : 0;
    return ok;
}
static void teleportTo(void* player, const LinearTransform& t) {
    alignas(16) LinearTransform x = t;
    ((void (*)(void*, const LinearTransform*))rva(Fn_ServerCharacterEntity_teleport))(player, &x);
}
static bool groundBelow(const LinearTransform& t, float& ground) {
    float from[3] = {t.trans[0], t.trans[1] + kProbeUp, t.trans[2]};
    float to[3] = {t.trans[0], t.trans[1] - kProbeDown, t.trans[2]};
    for (int i = 0; i < kProbeCasts; ++i) {
        float hit[3];
        if (!raycast::cast(from, to, hit)) return false;
        if (hit[1] <= t.trans[1] + kProbeAccept) { ground = hit[1]; return true; }
        from[1] = hit[1] - 0.05f;   // au-dessus de la cible (plafond) : on regarde en dessous
        if (from[1] <= to[1]) return false;
    }
    return false;
}

bool request(uint32_t peer, const float* fixedPos) {
    if (g_phase != Idle || !available()) return false;
    g_peer = peer;
    g_useFixed = fixedPos != NULL;
    if (fixedPos) memcpy(g_fixed, fixedPos, 12);
    g_result = 0;
    setPhase(FadeOut);
    return true;
}
LONG result() { return g_result; }

float fadeAlpha() {
    LONG p = g_phase;
    if (p == Idle) return 0.f;
    DWORD since = GetTickCount() - g_phaseAt;
    DWORD planned = p == Hold ? kMaxHoldMs : kFadeMs;
    if (since > planned + kStuckMs) {   // le jeu ne tourne plus : on rend l'écran
        InterlockedCompareExchange(&g_phase, Idle, p);
        return 0.f;
    }
    float t = (float)since;
    if (p == FadeOut) return t >= kFadeMs ? 1.f : t / kFadeMs;
    if (p == Hold) return 1.f;
    return t >= kFadeMs ? 0.f : 1.f - t / kFadeMs;
}

void levelChanged() { g_phase = Idle; }

// thread du jeu (serveur), à chaque tour
void tick() {
    LONG p = g_phase;
    if (p == Idle) return;
    DWORD since = GetTickCount() - g_phaseAt;
    void* player = level::playerCharacter();
    if (p == FadeOut) {
        if (since < kFadeMs) return;
        if (!player || !available()) { logf("téléportation : personnage indisponible, annulée"); g_result = 2; setPhase(FadeIn); return; }
        // sur la position exacte du joueur (pas à côté : un mur, un vide...) ; sinon la position donnée par coop
        PlayerState st;
        uint32_t age = 0;
        if (g_peer && remote::state(g_peer, st, age) && age <= 2000 && finite(st.pos[0]) && finite(st.pos[1]) && finite(st.pos[2]) &&
            fabsf(st.pos[0]) + fabsf(st.pos[1]) + fabsf(st.pos[2]) > 1.f) {   // 0,0,0 = position inconnue (vide)
            g_target = transformFromYaw(st.pos, st.yaw);
        } else if (g_useFixed && finite(g_fixed[0]) && finite(g_fixed[1]) && finite(g_fixed[2]) &&
                   fabsf(g_fixed[0]) + fabsf(g_fixed[1]) + fabsf(g_fixed[2]) > 0.01f) {
            g_target = transformFromYaw(g_fixed, 0.f);
        } else {
            logf("téléportation : joueur indisponible, annulée");
            g_result = 2;
            setPhase(FadeIn);
            return;
        }
        logf("téléportation : vers %.1f %.1f %.1f", g_target.trans[0], g_target.trans[1], g_target.trans[2]);
        g_retries = 0;
        g_character = player;
        g_haveFrom = readEntityTransform(player, g_from);
        g_groundAt = g_steadyAt = 0;
        teleportTo(player, g_target);
        setPhase(Hold);
        return;
    }
    if (p == Hold) {
        if (player != g_character) { g_result = 2; setPhase(FadeIn); return; }   // mort / réapparition : le jeu décide
        LinearTransform now;
        bool have = player && readEntityTransform(player, now);
        float dx = now.trans[0] - g_target.trans[0], dz = now.trans[2] - g_target.trans[2];
        if (have && dx * dx + dz * dz >= kNearTarget * kNearTarget) { g_result = 2; setPhase(FadeIn); return; }
        DWORD t = GetTickCount();
        if (!g_groundAt) {
            float ground = 0;
            if (!raycast::available()) {
                g_groundAt = t | 1;
            } else if (groundBelow(g_target, ground) && (ground >= g_target.trans[1] - kFarGround || since >= kHoldMs)) {
                g_target.trans[1] = ground + kLift;
                g_groundAt = t | 1;
                teleportTo(player, g_target);
            } else if (since < kMaxHoldMs) {
                teleportTo(player, g_target);   // on l'y maintient pendant que la ville se charge
            } else {
                if (g_haveFrom) teleportTo(player, g_from);
                logf("téléportation : pas de sol à l'arrivée, retour au départ");
                g_result = 2;
                setPhase(FadeIn);
            }
            return;
        }
        if (have && now.trans[1] < g_target.trans[1] - kFallMargin) {
            g_steadyAt = 0;
            if (g_retries < kMaxRetries) { ++g_retries; teleportTo(player, g_target); }
        } else if (have && !g_steadyAt) {
            g_steadyAt = t | 1;
        }
        bool settled = g_steadyAt && t - g_steadyAt >= kSettleMs;
        if ((since >= kHoldMs && settled) || since >= kMaxHoldMs) {
            g_result = 1;
            setPhase(FadeIn);
        }
        return;
    }
    if (p == FadeIn && since >= kFadeMs) g_phase = Idle;
}
}  // namespace teleport

}  // namespace game
