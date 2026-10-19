// =====================================================================================
//  MEC Coop - les autres joueurs en personnages 3D ("fantômes" des contre-la-montre)
//
//  Chaque joueur distant reçoit un ProjectionCharacter créé par le spawner de fantômes de la
//  ville. À chaque tour du serveur, sa pose reçue est écrite dans le composant de replay du
//  fantôme (le moteur la transmet au client comme un vrai replay) ; côté client, le fantôme est
//  gardé visible et placé à la position du joueur, et sa pose est réécrite à chaque image.
//
//  Méthode reprise du mod MECMP (licence MIT) :
//    Copyright (c) 2026 MECMP contributors - https://github.com/CodeNameMeteor/mecmp
// =====================================================================================
#include "game.h"
#include <stdio.h>

namespace game {
namespace ghosts {

static const uint32_t kFreshMs = 1500;
static const uint32_t kHideAfterMs = 3000;
static const DWORD kSpawnGapMs = 2000, kMinSpawnGapMs = 200, kClaimWindowMs = 1500;
static const uint64_t kSettleTicks = 90, kPlayerSettleTicks = 45;
static const size_t kMaxGhosts = 15;
static const float kParkDepth = 1000.f;
static const DWORD kParkMs = 1500, kReplicateMs = 2000;

struct Ghost {
    uint32_t peer;
    char name[32];
    void* character;      // PamServerCharacterEntity (serveur)
    uintptr_t charData;
    void* spawner;
    void* clientChar;     // PamClientCharacterEntity (client)
    void* clientComp;     // PamClientReplayPlayerComponent (client)
    DWORD spawnedAt, clientCharAt;
    int visibleCalls;
    bool hidden, posing, ghostShader;
    DWORD posingSince;
    float lastPos[3];
    DWORD parkUntil;
    int lookApplied, lookWanted;   // apparence posée / demandée (1 Faith, 2 fantôme ; 0 = aucune)
    DWORD lookTriedAt;
};

static Lock g_m;
static Ghost g_ghosts[kMaxGhosts];
static size_t g_count = 0;
static volatile LONG g_pendingPeer = 0;
static volatile DWORD g_pendingAt = 0;
static volatile uintptr_t g_ghostCharData = 0;
static volatile uintptr_t g_knownCharData[4] = {0, 0, 0, 0};
static volatile uintptr_t g_tiCharData = 0;
static DWORD g_lastSpawnAt = 0;
static bool g_lastSpawnFailed = false;
static volatile bool g_enabled = true;
static volatile LONG g_updaterCalls = 0;

void init() { g_m.init(); }

static bool knownCharData(uintptr_t d) {
    if (!d) return false;
    if (d == g_ghostCharData) return true;
    for (int i = 0; i < 4; i++) if (g_knownCharData[i] == d) return true;
    return false;
}
static void rememberCharData(uintptr_t d) {
    if (!d || knownCharData(d)) return;
    for (int i = 0; i < 4; i++)
        if (InterlockedCompareExchange64((volatile LONG64*)&g_knownCharData[i], (LONG64)d, 0) == 0) return;
    for (int i = 0; i < 3; i++) g_knownCharData[i] = g_knownCharData[i + 1];
    g_knownCharData[3] = d;
}

static bool clientCharIsGhost(void* ent) {
    uintptr_t e = (uintptr_t)ent, vt = 0, data = 0;
    return e && read(e, vt) && vt == rva(Vtbl_PamClientCharacterEntity) && read(e + Character_Data, data) &&
           (!g_ghostCharData || knownCharData(data));
}
static uintptr_t componentOwner(uintptr_t comp) {
    uintptr_t coll = readOr<uintptr_t>(comp + Component_Collection);
    return coll ? readOr<uintptr_t>(coll) : 0;
}
static bool setClientVisible(void* ent, bool visible) {
    if (!clientCharIsGhost(ent)) return false;
    uintptr_t vt = 0, fn = 0;
    if (!read((uintptr_t)ent, vt) || !read(vt + Vtbl_ClientSetVisible, fn) || !inImage(fn)) return false;
    ((void (*)(void*, uint64_t))fn)(ent, visible ? 1 : 0);
    return true;
}

// les champs du composant client utilisés à chaque image, lus d'un coup
struct ClientCompView {
    uint8_t raw[CRP_SkipRoot + 1];
    template <class T> T at(size_t off) const { T v; memcpy(&v, raw + off, sizeof(T)); return v; }
    uintptr_t pose() const { return at<uintptr_t>(CRP_ReceivedPose); }
    uint32_t bones() const { return at<uint32_t>(CRP_ExpectedBones); }
    uintptr_t boneIndices() const { return at<uintptr_t>(CRP_Setup + 0x10); }
};
static bool readClientComp(void* comp, void* owner, ClientCompView& v) {
    uintptr_t c = (uintptr_t)comp;
    if (!c || !read(c, v.raw, sizeof(v.raw)) || v.at<uintptr_t>(0) != rva(Vtbl_PamClientReplayPlayerComponent)) return false;
    if (!owner) return true;
    uintptr_t coll = v.at<uintptr_t>(Component_Collection);
    return coll && readOr<uintptr_t>(coll) == (uintptr_t)owner;
}

static bool isCharacterData(uintptr_t data) {
    uintptr_t ti = g_tiCharData, vt = 0, getType = 0;
    if (!ti || !data || !read(data, vt) || !inImage(vt) || !read(vt, getType) || !inImage(getType)) return false;
    return isKindOfRuntime((void*)data, ti);
}
static uintptr_t blueprintCharacterData(uintptr_t bp) {
    uintptr_t c[3] = {readOr<uintptr_t>(bp + ObjectBlueprint_Object), readOr<uintptr_t>(bp + ObjectBlueprint_Object + 8),
                      bp + 0x60};
    for (int i = 0; i < 3; i++) if (isCharacterData(c[i])) return c[i];
    return 0;
}

static void startHiding(Ghost& g) {
    if (g.clientChar) setClientVisible(g.clientChar, false);
    g.hidden = true;
    g.parkUntil = GetTickCount() + kParkMs;
    g.posing = false;
    g.posingSince = 0;
}
static bool parking(const Ghost& g) { return g.hidden && g.parkUntil && (int32_t)(GetTickCount() - g.parkUntil) < 0; }

// donne au composant de replay serveur un tampon de pose avec la même disposition que les vrais fantômes
static bool preparePose(uint8_t* c) {
    if (*(void**)(c + SRP_Pose)) return true;
    level::BoneSetup ref;
    if (!level::referenceSetup(ref)) return false;
    uint32_t N = ref.count;
    uint8_t* S = c + SRP_Setup;
    if (!*(void**)(S + 0x10)) *(void**)(S + 0x08) = NULL;
    if (!*(void**)(S + 0x78)) *(void**)(S + 0x90) = NULL;
    uint32_t* idx = (uint32_t*)engineAlloc((void**)(S + 0x08), N * 4, 4);
    uint32_t flagCap = (N + 15) & ~15u;
    uint8_t* flags = (uint8_t*)engineAlloc((void**)(S + 0x90), flagCap, 4);
    float* pose = (float*)engineAlloc((void**)(c + SRP_PoseArena), N * 0x20, 16);
    if (!idx || !flags || !pose) return false;
    for (uint32_t i = 0; i < N; ++i) { idx[i] = ref.indices[i]; flags[i] = ref.flags[i]; }
    memset(pose, 0, N * 0x20);
    *(uint32_t**)(S + 0x10) = idx;
    *(uint32_t*)(S + 0x18) = N;
    *(uint8_t**)(S + 0x78) = flags;
    *(uint8_t**)(S + 0x80) = flags + N;
    *(uint8_t**)(S + 0x88) = flags + flagCap;
    *(float**)(c + SRP_Pose) = pose;
    *(float**)(c + SRP_PoseEnd) = pose + N * 8;
    *(float**)(c + SRP_PoseCap) = pose + N * 8;
    *(uint32_t*)S = N;
    return true;
}
static int trajectorySlot(uint8_t* c, uint32_t n) {
    const uint32_t* idx = *(uint32_t**)(c + SRP_Setup + 0x10);
    for (uint32_t i = 0; idx && i < n; ++i) if ((idx[i] & 0xff) == 1) return (int)i;   // AITrajectory
    return -1;
}
static void placeAnchor(float* pose, int traj, const float pos[3]) {
    float* t = pose + traj * 8;
    if (traj == 1) foldBone0(pose, t, pos);
    else { t[0] = pos[0]; t[1] = pos[1]; t[2] = pos[2]; t[3] = 1.f; }
}

// serveur : la pose reçue dans le composant de replay du fantôme (transmise au client)
static void drive(Ghost& g, const PlayerState& input) {
    PlayerState st = input;
    if (!limitPose(st)) return;
    uint8_t* c = (uint8_t*)getComponent(g.character, TI_PamServerReplayPlayerComponent);
    if (!c || !preparePose(c)) return;
    uint32_t N = *(uint32_t*)(c + SRP_Setup);
    float* pose = *(float**)(c + SRP_Pose);
    if (!pose || N == 0 || N > kMaxBones) return;
    *(uint32_t*)(c + SRP_PlaybackState) = 0;
    int traj = trajectorySlot(c, N);
    if (st.boneCount == N) {
        memcpy(pose, st.bones, N * 0x20);
        if (!g.posing) {
            g.posing = true;
            g.posingSince = GetTickCount();
            logf("fantôme : %s animé (%u os)", g.name, N);
        }
    }
    if (traj >= 0) {
        float* t = pose + traj * 8;
        if (st.boneCount != N) { t[4] = 0.f; t[5] = sinf(st.yaw * 0.5f); t[6] = 0.f; t[7] = cosf(st.yaw * 0.5f); }
        placeAnchor(pose, traj, st.pos);
    }
    bool clientDriven = g.posing && g.clientComp && GetTickCount() - g.posingSince > kReplicateMs;
    if (!clientDriven) setNetDirty(c + SRP_NetState, 2 | 4);
}

// client : arrêter son propre replay et garder l'ancre du corps sur la position du joueur
static void fixClient(Ghost& g, const PlayerState& st) {
    ClientCompView v;
    if (!readClientComp(g.clientComp, g.clientChar, v)) return;
    uintptr_t c = (uintptr_t)g.clientComp;
    if (v.at<uint32_t>(CRP_PlaybackState) != 0) poke<uint32_t>(c + CRP_PlaybackState, 0);
    if (v.at<uint8_t>(CRP_PlaybackForced) != 0) poke<uint8_t>(c + CRP_PlaybackForced, 0);
    if (v.at<uint8_t>(CRP_SkipRoot) != 0) poke<uint8_t>(c + CRP_SkipRoot, 0);
    if (g.posing && GetTickCount() - g.posingSince > kReplicateMs) return;   // fait à chaque image (renderFrame)
    uintptr_t pose = v.pose();
    uint32_t n = v.bones();
    uint32_t idx[kMaxBones];
    uintptr_t idxPtr = v.boneIndices();
    if (!pose || !n || n > kMaxBones || !idxPtr || !read(idxPtr, idx, n * 4)) return;
    for (uint32_t i = 0; i < n; ++i) {
        if ((idx[i] & 0xff) != 1) continue;
        float bones[2][8];
        if (i != 1 || !read(pose, bones, sizeof(bones))) { poke(pose + i * 0x20, st.pos, sizeof(st.pos)); break; }
        foldBone0(bones[0], bones[1], st.pos);
        poke(pose, bones, sizeof(bones));
        break;
    }
}

static void setGhostShader(Ghost& ghost, bool enabled) {
    ghost.ghostShader = enabled;
    uintptr_t client = (uintptr_t)ghost.clientChar;
    uint8_t head[ClientChar_GhostShader + 1];
    if (!client || !read(client, head, sizeof(head))) return;
    uintptr_t vt = 0, data = 0;
    memcpy(&vt, head, 8);
    memcpy(&data, head + Character_Data, 8);
    if (vt != rva(Vtbl_PamClientCharacterEntity) || !knownCharData(data) || head[ClientChar_GhostShader] == (uint8_t)enabled)
        return;
    poke<uint8_t>(client + ClientChar_GhostShader, enabled ? 1 : 0);
}

// le jeu demande au spawner son DERNIER personnage (aperçu « Ton écho » du menu) : on garde le sien en dernier
static void keepGameCharacterLast(void* spawner) {
    uintptr_t list[2] = {0, 0}, sp = (uintptr_t)spawner;
    if (!sp || !read(sp + Spawner_Spawned, list, sizeof(list)) || list[1] <= list[0] || (list[1] - list[0]) / 8 > 64) return;
    size_t n = (list[1] - list[0]) / 8;
    uintptr_t items[64], order[64];
    if (!read(list[0], items, n * 8)) return;
    size_t k = 0, nOurs = 0;
    {
        Guard lk(g_m);
        for (size_t pass = 0; pass < 2; pass++)
            for (size_t i = 0; i < n; i++) {
                bool mine = false;
                for (size_t j = 0; j < g_count; j++) mine = mine || (uintptr_t)g_ghosts[j].character == items[i];
                if (mine == (pass == 0)) { order[k++] = items[i]; nOurs += mine; }
            }
    }
    if (!nOurs || nOurs == n) return;
    if (memcmp(order, items, n * 8) != 0) poke(list[0], order, n * 8);
}

static void* spawnGhost(uint32_t peer, const char* name, const PlayerState& st) {
    void* spawner = level::ghostSpawner();
    if (!level::spawnerAlive(spawner)) { logf("fantôme : pas de spawner pour %s", name); return NULL; }
    uintptr_t dataBefore = g_ghostCharData;
    uintptr_t sd = readOr<uintptr_t>((uintptr_t)spawner + Spawner_Data);
    uintptr_t sbp = sd ? readOr<uintptr_t>(sd + ReferenceObjectData_Blueprint) : 0;
    uintptr_t d = sbp ? blueprintCharacterData(sbp) : 0;
    if (d) g_ghostCharData = d;
    else if (sbp && !g_ghostCharData) g_ghostCharData = sbp + 0x60;
    SpawnInfo info;
    memset(&info, 0, sizeof(info));
    info.transform = transformFromYaw(st.pos, st.yaw);
    info.unk58 = -1;
    info.busId = -1;
    info.unk60 = 1;
    info.useTransform = 1;
    uint8_t* enabled = (uint8_t*)spawner + Spawner_Enabled;
    uint8_t oldEnabled = *enabled;
    *enabled = 1;
    {
        Guard lk(g_m);
        if (g_count >= kMaxGhosts) { *enabled = oldEnabled; return NULL; }
        Ghost& g = g_ghosts[g_count++];
        memset(&g, 0, sizeof(g));
        g.peer = peer;
        lstrcpynA(g.name, name, 32);
        g.spawner = spawner;
        g.spawnedAt = GetTickCount();
        g.ghostShader = true;
    }
    g_pendingPeer = (LONG)peer;
    g_pendingAt = GetTickCount();
    typedef void* (*SpawnFn)(void*, SpawnInfo*);
    void* ch = (*(SpawnFn**)spawner)[Vtbl_SpawnerSpawn / 8](spawner, &info);
    *enabled = oldEnabled;
    {
        Guard lk(g_m);
        for (size_t i = 0; i < g_count; ++i)
            if (g_ghosts[i].peer == peer && !g_ghosts[i].character) {
                if (ch) g_ghosts[i].character = ch;
                else { g_ghosts[i] = g_ghosts[--g_count]; }
                break;
            }
    }
    level::spawnResult(spawner, ch != NULL);
    if (!ch) {
        g_ghostCharData = dataBefore;
        logf("fantôme : le spawner n'a pas donné de personnage pour %s", name);
        return NULL;
    }
    uintptr_t cd = readOr<uintptr_t>((uintptr_t)ch + Character_Data);
    if (cd) {
        g_ghostCharData = cd;
        rememberCharData(cd);
        Guard lk(g_m);
        for (size_t i = 0; i < g_count; i++) if (g_ghosts[i].character == ch) g_ghosts[i].charData = cd;
    }
    setTransform(ch, info.transform);
    keepGameCharacterLast(spawner);
    logf("fantôme : créé pour %s (personnage %p, spawner %p)", name, ch, spawner);
    return ch;
}

static void logLostGhost(const Ghost& g, uintptr_t expected) {
    uintptr_t ch = (uintptr_t)g.character, vt = 0, data = 0;
    bool readable = read(ch, vt);
    if (readable) read(ch + Character_Data, data);
    logf("fantôme : celui de %s a disparu %.1f s après sa création (%s)", g.name, (GetTickCount() - g.spawnedAt) / 1000.0,
         !readable ? "mémoire illisible" : vt == rva(Vtbl_PamServerCharacterEntity) ? "données changées" :
         inImage(vt) ? "remplacé par un autre objet" : "détruit");
    (void)expected;
}

static bool claimWindowOpen() { DWORD at = g_pendingAt; return at && GetTickCount() - at < kClaimWindowMs; }
static void closeClaimIfDone(const Ghost& g) {
    if (g.peer != (uint32_t)g_pendingPeer || !g.clientChar || !g.clientComp || !g_pendingAt) return;
    g_pendingAt = 0;
}
static bool pendingClaimedUnlocked() {
    uint32_t peer = (uint32_t)g_pendingPeer;
    for (size_t i = 0; i < g_count; i++)
        if (g_ghosts[i].peer == peer && g_ghosts[i].character) return g_ghosts[i].clientChar && g_ghosts[i].clientComp;
    return true;
}

bool onClientReplayComp(void* comp) {
    if (!comp || !claimWindowOpen()) return false;
    uintptr_t owner = componentOwner((uintptr_t)comp);
    if (owner && !clientCharIsGhost((void*)owner)) return false;
    uint32_t peer = (uint32_t)g_pendingPeer;
    Guard lk(g_m);
    for (size_t i = 0; i < g_count; i++) {
        Ghost& g = g_ghosts[i];
        if (g.peer == peer && !g.clientComp) {
            if (owner && g.clientChar && (uintptr_t)g.clientChar != owner) continue;
            g.clientComp = comp;
            closeClaimIfDone(g);
            return true;
        }
    }
    return false;
}

void onClientCharacter(void* ent) {
    if (!ent || !claimWindowOpen()) return;
    uintptr_t data = readOr<uintptr_t>((uintptr_t)ent + Character_Data);
    if (!data || data != g_ghostCharData) return;
    uint32_t peer = (uint32_t)g_pendingPeer;
    {
        Guard lk(g_m);
        for (size_t i = 0; i < g_count; i++) {
            Ghost& g = g_ghosts[i];
            if (g.peer == peer && !g.clientChar) {
                g.clientChar = ent;
                g.clientCharAt = GetTickCount();
                setGhostShader(g, g.ghostShader);
                closeClaimIfDone(g);
                break;
            }
        }
    }
    setClientVisible(ent, true);
}

void setCharacterDataType(uintptr_t ti) { g_tiCharData = ti; }
void setEnabled(bool on) { g_enabled = on; }

void beforeClientReplayUpdate() {
    InterlockedIncrement(&g_updaterCalls);
    renderFrame();
}

// à chaque image : la pose interpolée pour cet instant, écrite côté client (fluide à 144/180 i/s)
void renderFrame() {
    if (!level::serverWorld() || level::inFrontEnd()) return;
    struct Target { uint32_t peer; void* comp; void* ch; };
    Target targets[kMaxGhosts];
    size_t n = 0;
    {
        Guard lk(g_m);
        for (size_t i = 0; i < g_count; i++) {
            const Ghost& g = g_ghosts[i];
            if (n < kMaxGhosts && g.posing && !g.hidden && g.clientComp && GetTickCount() - g.posingSince > kReplicateMs) {
                targets[n].peer = g.peer;
                targets[n].comp = g.clientComp;
                targets[n].ch = g.clientChar;
                n++;
            }
        }
    }
    for (size_t i = 0; i < n; ++i) {
        PlayerState st;
        uint32_t age = 0;
        if (!remote::state(targets[i].peer, st, age) || age >= kHideAfterMs || !limitPose(st)) continue;
        ClientCompView v;
        if (!readClientComp(targets[i].comp, targets[i].ch, v)) continue;
        uintptr_t pose = v.pose();
        uint32_t bones = v.bones();
        uintptr_t idxPtr = v.boneIndices();
        uint32_t idx[kMaxBones];
        if (!pose || bones != st.boneCount || !bones || bones > kMaxBones || !idxPtr || !read(idxPtr, idx, bones * 4)) continue;
        float buf[kMaxBones][8];
        memcpy(buf, st.bones, bones * 0x20);
        for (uint32_t b = 0; b < bones; ++b)
            if ((idx[b] & 0xff) == 1) placeAnchor(&buf[0][0], (int)b, st.pos);
        poke(pose, buf, bones * 0x20);
    }
}

static bool isPresent(const remote::Info* p, size_t np, uint32_t id) {
    for (size_t i = 0; i < np; i++) if (p[i].id == id) return true;
    return false;
}

// un fantôme libre (son joueur est parti) est donné au nouveau ; de préférence celui qu'il avait (même nom)
static bool reuseFreeGhost(const remote::Info& p, const remote::Info* present, size_t np) {
    Ghost* pick = NULL;
    for (size_t i = 0; i < g_count; i++) {
        Ghost& g = g_ghosts[i];
        if (!g.character || isPresent(present, np, g.peer)) continue;
        if (!pick || (p.name[0] && strcmp(g.name, p.name) == 0 && strcmp(pick->name, p.name) != 0)) pick = &g;
    }
    if (!pick) return false;
    logf("fantôme : %s reprend le fantôme de %s", p.name, pick->name);
    pick->peer = p.id;
    lstrcpynA(pick->name, p.name, 32);
    pick->posing = false;
    pick->posingSince = 0;
    pick->visibleCalls = 0;
    pick->hidden = true;
    pick->clientCharAt = GetTickCount();
    pick->lookApplied = pick->lookWanted = 0;
    pick->lookTriedAt = 0;
    return true;
}

void tick(uint64_t ticksInLevel) {
    // joueurs présents : un paquet reçu il y a moins de kHideAfterMs (coop fermé ou planté : plus rien n'arrive)
    remote::Info all[8], players[8];
    size_t na = remote::list(all, 8), np = 0;
    for (size_t i = 0; i < na; i++) if (all[i].ageMs < kHideAfterMs) players[np++] = all[i];
    {   // fantômes détruits par le jeu
        Guard lk(g_m);
        uintptr_t data = g_ghostCharData;
        for (size_t i = 0; i < g_count;) {
            uintptr_t own = g_ghosts[i].charData ? g_ghosts[i].charData : data;
            if (!g_ghosts[i].character || level::characterExists(g_ghosts[i].character, own)) { ++i; continue; }
            logLostGhost(g_ghosts[i], own);
            g_ghosts[i] = g_ghosts[--g_count];
        }
    }
    // le jeu supprime un fantôme créé juste après l'apparition de mon personnage : on attend un peu
    static void* lastPlayer = NULL;
    static uint64_t playerSince = 0;
    void* playerChar = level::playerCharacter();
    if (playerChar != lastPlayer) { lastPlayer = playerChar; playerSince = ticksInLevel; }
    bool playerSettled = playerChar && ticksInLevel - playerSince >= kPlayerSettleTicks;
    if (playerSettled) {
        void* ours[kMaxGhosts];
        size_t k = 0;
        {
            Guard lk(g_m);
            for (size_t i = 0; i < g_count; i++) if (g_ghosts[i].character) ours[k++] = g_ghosts[i].character;
        }
        level::chooseGhostSpawner(ours, k);
    }
    bool canSpawn = g_enabled && ticksInLevel >= kSettleTicks && !level::inFrontEnd() &&
                    level::spawnerAlive(level::ghostSpawner()) && playerSettled;
    if (canSpawn) {
        for (size_t pi = 0; pi < np; pi++) {
            const remote::Info& p = players[pi];
            if (p.ageMs > kFreshMs) continue;
            size_t count;
            bool claimed;
            {
                Guard lk(g_m);
                bool has = false;
                for (size_t i = 0; i < g_count; i++) has = has || g_ghosts[i].peer == p.id;
                if (has || reuseFreeGhost(p, players, np)) continue;
                count = g_count;
                claimed = pendingClaimedUnlocked();
            }
            if (count >= kMaxGhosts) continue;
            DWORD since = GetTickCount() - g_lastSpawnAt;
            if (since < kMinSpawnGapMs || (since < kSpawnGapMs && (g_lastSpawnFailed || !claimed))) continue;
            g_lastSpawnAt = GetTickCount();
            g_lastSpawnFailed = true;
            level::BoneSetup ref;
            if (!level::referenceSetup(ref)) continue;
            PlayerState st;
            uint32_t age = 0;
            if (!remote::state(p.id, st, age)) continue;
            g_lastSpawnFailed = spawnGhost(p.id, p.name, st) == NULL;
        }
    }
    // piloter les fantômes des joueurs présents ; cacher les autres
    Ghost ghosts[kMaxGhosts];
    size_t n;
    {
        Guard lk(g_m);
        n = g_count;
        memcpy(ghosts, g_ghosts, n * sizeof(Ghost));
    }
    static PlayerState st, park;
    for (size_t i = 0; i < n; ++i) {
        Ghost& g = ghosts[i];
        if (!g.character) continue;
        bool present = isPresent(players, np, g.peer);
        if (!present && g.hidden && !parking(g)) continue;
        uint32_t age = 0;
        bool have = present && remote::state(g.peer, st, age);
        bool visibilityChanged = false;
        if (!present) {
            if (!g.hidden) { startHiding(g); visibilityChanged = true; }
            memset(&park, 0, sizeof(park));
            park.pos[0] = g.lastPos[0];
            park.pos[1] = g.lastPos[1] - kParkDepth;   // caché ET rangé sous le sol (sinon ses cheveux restent)
            park.pos[2] = g.lastPos[2];
            drive(g, park);
            fixClient(g, park);
        } else if (g.clientChar) {
            DWORD since = GetTickCount() - g.clientCharAt;
            static const DWORD kAt[] = {300, 1000, 2500, 5000};
            DWORD due = g.visibleCalls < 4 ? kAt[g.visibleCalls] : 5000u + 3000u * (DWORD)(g.visibleCalls - 3);
            if (g.hidden || since >= due) {
                setClientVisible(g.clientChar, true);
                ++g.visibleCalls;
                g.hidden = false;
                visibilityChanged = true;
            }
        }
        // l'apparence choisie par le joueur ; une seule par tour (ça reconstruit le maillage), réessai après 2 s
        bool lookChanged = false;
        static DWORD lastLookApply = 0;
        if (present) {
            int want = 0;
            for (size_t pi = 0; pi < np; pi++) if (players[pi].id == g.peer) want = (int)players[pi].look;
            DWORD nowT = GetTickCount();
            bool due = want != g.lookWanted || nowT - g.lookTriedAt > 2000;
            if (want && want != g.lookApplied && due && nowT != lastLookApply) {
                lastLookApply = nowT;
                g.lookTriedAt = nowT;
                g.lookWanted = want;
                if (models::apply(g.character, want)) {
                    g.lookApplied = want;
                    g.ghostShader = want == 2;      // Faith : shader normal ; fantôme : shader lumineux
                    logf("fantôme : %s a maintenant l'apparence %s", g.name, want == 1 ? "Faith" : "fantôme");
                }
                lookChanged = true;
            }
        }
        if (have) {
            if (g.clientChar) setGhostShader(g, g.ghostShader);
            drive(g, st);
            fixClient(g, st);
            memcpy(g.lastPos, st.pos, sizeof(g.lastPos));
            g.parkUntil = 0;
        }
        Guard lk(g_m);
        Ghost* live = i < g_count && g_ghosts[i].character == g.character ? &g_ghosts[i] : NULL;
        for (size_t k = 0; !live && k < g_count; ++k) if (g_ghosts[k].character == g.character) live = &g_ghosts[k];
        if (live && live->peer == g.peer) {
            live->posing = g.posing;
            live->posingSince = g.posingSince;
            if (lookChanged) {
                live->lookApplied = g.lookApplied;
                live->lookWanted = g.lookWanted;
                live->lookTriedAt = g.lookTriedAt;
                live->ghostShader = g.ghostShader;
            }
            memcpy(live->lastPos, g.lastPos, sizeof(live->lastPos));
            if (visibilityChanged) { live->hidden = g.hidden; live->visibleCalls = g.visibleCalls; live->parkUntil = g.parkUntil; }
            else if (have) live->parkUntil = 0;
        }
    }
}

void levelChanged() {
    Guard lk(g_m);
    g_count = 0;
    g_pendingAt = 0;
    g_lastSpawnAt = 0;
    g_lastSpawnFailed = false;
    g_ghostCharData = 0;
    for (int i = 0; i < 4; i++) g_knownCharData[i] = 0;
}

bool isGhostCharacter(void* ch) {
    Guard lk(g_m);
    for (size_t i = 0; i < g_count; i++) if (g_ghosts[i].character == ch) return true;
    return false;
}

void* clientCharacter(uint32_t peer) {
    Guard lk(g_m);
    for (size_t i = 0; i < g_count; i++) {
        const Ghost& g = g_ghosts[i];
        if (g.peer == peer && !g.hidden && g.posing && clientCharIsGhost(g.clientChar)) return g.clientChar;
    }
    return NULL;
}

bool shownPosition(uint32_t id, float out[3]) {
    bool shown = false;
    {
        Guard lk(g_m);
        for (size_t i = 0; i < g_count; i++) {
            const Ghost& g = g_ghosts[i];
            if (g.peer == id && g.character && g.clientChar && !g.hidden && g.posing) shown = true;
        }
    }
    if (!shown) return false;
    PlayerState st;
    uint32_t age = 0;
    if (!remote::state(id, st, age) || age >= kHideAfterMs) return false;
    memcpy(out, st.pos, 12);
    return true;
}

void status(char* out, size_t cap) {
    Guard lk(g_m);
    int len = _snprintf(out, cap, "%u fantôme(s)", (unsigned)g_count);
    for (size_t i = 0; i < g_count && len > 0 && (size_t)len < cap; i++) {
        const Ghost& g = g_ghosts[i];
        int dly = remote::delayMs(g.peer);
        len += _snprintf(out + len, cap - len, " | %s%s%s", g.name, !g.clientChar ? " (sans client)" : g.hidden ? " (caché)" :
                         g.posing ? " (animé)" : "", g.lookApplied == 1 ? " Faith" : g.lookApplied == 2 ? " fantôme" : "");
        if (dly > 0 && len > 0 && (size_t)len < cap) len += _snprintf(out + len, cap - len, ", %d ms", dly);
    }
    if (!level::spawnerAlive(level::ghostSpawner()) && level::serverWorld())
        _snprintf(out + (len > 0 ? len : 0), cap - (len > 0 ? len : 0), " | spawner de fantômes pas encore trouvé");
    out[cap - 1] = 0;
}

}  // namespace ghosts
}  // namespace game
