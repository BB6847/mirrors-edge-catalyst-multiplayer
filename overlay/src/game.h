// =====================================================================================
//  MEC Coop - personnages 3D des autres joueurs (fantômes de replay)
//
//  Adresses, structures et méthode reprises du mod MECMP (licence MIT) :
//    Copyright (c) 2026 MECMP contributors - https://github.com/CodeNameMeteor/mecmp
//  Réécrit ici sans la bibliothèque C++ standard (tableaux fixes, sections critiques).
// =====================================================================================
#pragma once
#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>

namespace game {

// ---------------------------------------------------------------- mémoire (lectures sûres)
inline uintptr_t moduleBase() { return (uintptr_t)GetModuleHandleW(NULL); }
inline uintptr_t rva(uintptr_t va) { return moduleBase() + (va - 0x140000000ull); }

inline bool read(uintptr_t addr, void* out, size_t size) {
    if (addr < 0x10000) return false;
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), (LPCVOID)addr, out, size, &got) && got == size;
}
template <class T> inline bool read(uintptr_t addr, T& out) { return read(addr, &out, sizeof(T)); }
template <class T> inline T readOr(uintptr_t addr, T fallback = T()) {
    T v;
    return read(addr, v) ? v : fallback;
}
bool write(uintptr_t addr, const void* src, size_t size);
template <class T> inline void poke(uintptr_t addr, T v) { write(addr, &v, sizeof(T)); }
inline void poke(uintptr_t addr, const void* src, size_t n) { write(addr, src, n); }
// lit une chaîne ASCII (faux si illisible ou pas du texte)
bool readCString(uintptr_t addr, char* out, size_t cap);
bool patchPointer(uintptr_t addr, uintptr_t value, uintptr_t* old);

// petit verrou
struct Lock {
    CRITICAL_SECTION cs;
    void init() { InitializeCriticalSection(&cs); }
};
struct Guard {
    Lock& l;
    explicit Guard(Lock& x) : l(x) { EnterCriticalSection(&l.cs); }
    ~Guard() { LeaveCriticalSection(&l.cs); }
};

void logf(const char* fmt, ...);   // journal : <dossier du jeu>\_mod_coop\log_overlay.txt

inline bool finite(float v) { return __builtin_isfinite(v); }

// ---------------------------------------------------------------- adresses du jeu (build PC EA app)
constexpr uintptr_t kTypeInfoListHead = 0x14215d028;
constexpr uintptr_t kServerGameWorld = 0x1425469e0;
constexpr size_t World_Level = 0x20, Level_Data = 0x108, Asset_Name = 0x10;

constexpr uintptr_t TI_ServerCharacterEntity = 0x1427fe1e0;
constexpr uintptr_t TI_ObjectBlueprint = 0x1427e7eb0;
constexpr uintptr_t TI_PamReplayPlayerComponentData = 0x14286b890;
constexpr uintptr_t TI_PamServerReplayPlayerComponent = 0x142858f00;
constexpr uintptr_t TI_PamClientReplayRecordingComponent = 0x1428567b0;

constexpr uintptr_t Slot_ServerCharacterSpawnFactory = 0x141b4f720, Fn_ServerCharacterSpawnFactory = 0x1433820c0;
constexpr uintptr_t Slot_PamServerCharacterSpawnFactory = 0x141c4b5c0, Fn_PamServerCharacterSpawnFactory = 0x143957610;
constexpr uintptr_t Slot_ClientReplayPlayerFactory = 0x141c49158, Fn_ClientReplayPlayerFactory = 0x143949050;
constexpr uintptr_t Slot_ClientCharacterFactory = 0x141c3cd80, Fn_ClientCharacterFactory = 0x143910d20;
constexpr uintptr_t Slot_ServerWorldPostUpdate = 0x141b42d28, Fn_ServerWorldPostUpdate = 0x143341ee0;
constexpr uintptr_t Vtbl_ClientReplayUpdater = 0x141c49190, Fn_ClientReplayUpdateAll = 0x14394da40;
constexpr size_t Vtbl_ClientReplayUpdateAll = 0x10;

constexpr uintptr_t Vtbl_PamServerCharacterSpawnEntity = 0x141c4b388;
constexpr uintptr_t Vtbl_ServerCharacterSpawnEntity = 0x141b4e0c8;
constexpr uintptr_t Vtbl_PamClientReplayPlayerComponent = 0x141c48a28;
constexpr uintptr_t Vtbl_PamClientCharacterEntity = 0x141c3c760;
constexpr uintptr_t Vtbl_PamServerCharacterEntity = 0x141c3fcb0;

constexpr uintptr_t Fn_SpatialEntity_setTransform = 0x1432eb1f0;
constexpr uintptr_t Fn_NetState_setDirty = 0x14347e420;
constexpr uintptr_t Fn_Arena_allocFromMember = 0x142a647d0;
constexpr uintptr_t Fn_LocalClientCharacter = 0x1439655f0;
constexpr size_t Vtbl_SpawnerSpawn = 0xd0, Vtbl_GetComponent = 0x108, Vtbl_ClientSetVisible = 0x3d8;

constexpr size_t ReferenceObjectData_Blueprint = 0x18, ObjectBlueprint_Object = 0x40;
constexpr size_t Component_Collection = 0x08;
constexpr size_t CI_Data = 0xd0;
constexpr size_t Spawner_Data = 0x30, Spawner_Spawned = 0x148, Spawner_Enabled = 0x2a4;
constexpr size_t Character_Data = 0x30, Character_Player = 0x250;
constexpr size_t ClientChar_GhostShader = 0x334;
constexpr size_t Entity_TransformSpace = 0x38;

constexpr size_t SRP_PlaybackState = 0x38, SRP_Setup = 0xb0, SRP_Pose = 0x178, SRP_PoseEnd = 0x180,
                 SRP_PoseCap = 0x188, SRP_PoseArena = 0x190, SRP_NetState = 0x1a0;
constexpr size_t CRP_PlaybackState = 0x40, CRP_PlaybackForced = 0xb0, CRP_Setup = 0xb8,
                 CRP_ReceivedPose = 0x180, CRP_ExpectedBones = 0x200, CRP_SkipRoot = 0x2e6;
constexpr size_t Rec_Stride = 0x68, Rec_Capacity = 0x70, Rec_Base = 0x78, Rec_Head = 0x88;

#define kGhostBlueprint "Characters/Projection/ProjectionCharacter"
#define kPlayerBlueprint "Characters/FaithCharacter"
constexpr uint32_t kMaxBones = 32;

struct alignas(16) LinearTransform { float right[4], up[4], forward[4], trans[4]; };
static_assert(sizeof(LinearTransform) == 0x40, "LinearTransform");

struct alignas(16) SpawnInfo {
    LinearTransform transform;
    void* player;
    void* existing;
    void* unk50;
    int32_t unk58;
    int32_t busId;
    uint8_t unk60;
    uint8_t useTransform;
    uint8_t pad[0x0e];
};
static_assert(offsetof(SpawnInfo, player) == 0x40 && offsetof(SpawnInfo, busId) == 0x5c &&
              offsetof(SpawnInfo, useTransform) == 0x61, "SpawnInfo layout");

// état d'un joueur (position + pose du corps, format du replay du jeu)
struct PlayerState {
    float pos[3];
    float yaw;
    uint32_t timeMs;
    uint32_t boneCount;
    float bones[kMaxBones][8];   // par os : translation x,y,z,w ; rotation x,y,z,w
};

// ---------------------------------------------------------------- aides
inline bool inImage(uintptr_t a) { uintptr_t b = moduleBase(); return a >= b && a < b + 0x727b000; }
inline uintptr_t gameVirtual(const void* obj, size_t slot) {
    uintptr_t vt = 0, fn = 0;
    if (!obj || !read((uintptr_t)obj, vt) || !inImage(vt)) return 0;
    if (!read(vt + slot, fn) || !inImage(fn)) return 0;
    return fn;
}
inline bool isKindOfRuntime(void* obj, uintptr_t classInfo) {
    typedef const uint8_t* (*GetTypeFn)(void*);
    GetTypeFn getType = (GetTypeFn)gameVirtual(obj, 0);
    if (!getType) return false;
    const uint8_t* ti = getType(obj);
    uint16_t id = 0, range[2] = {0, 0};
    if (!ti || !read((uintptr_t)ti + 0x28, id) || !read(classInfo + 0x28, range, sizeof(range))) return false;
    return (uint32_t)(id - range[0]) <= (uint32_t)(range[1] - range[0]);
}
inline bool isKindOf(void* obj, uintptr_t tiVa) { return isKindOfRuntime(obj, rva(tiVa)); }
inline void* getComponent(void* character, uintptr_t tiVa) {
    typedef void* (*Fn)(void*, const void*);
    Fn fn = (Fn)gameVirtual(character, Vtbl_GetComponent);
    return fn ? fn(character, (const void*)rva(tiVa)) : NULL;
}
inline bool readEntityTransform(const void* entity, LinearTransform& out) {
    uintptr_t ts = readOr<uintptr_t>((uintptr_t)entity + Entity_TransformSpace);
    uint8_t sel[2] = {0, 0};
    if (!ts || !read(ts + 9, sel, 2)) return false;
    size_t idx = (size_t)sel[0] + 2u * sel[1];
    return read(ts + 0x10 + idx * 0x20, &out, sizeof(out));
}
inline void setTransform(void* entity, const LinearTransform& t) {
    ((void (*)(void*, const LinearTransform*, uint8_t))rva(Fn_SpatialEntity_setTransform))(entity, &t, 0);
}
inline void setNetDirty(void* netState, uint64_t bits) {
    ((void (*)(void*, uint64_t))rva(Fn_NetState_setDirty))(netState, bits);
}
inline void* engineAlloc(void** arenaMember, uint64_t size, uint64_t align) {
    return ((void* (*)(void**, uint64_t, uint64_t))rva(Fn_Arena_allocFromMember))(arenaMember, size, align);
}
inline LinearTransform transformFromYaw(const float pos[3], float yaw) {
    LinearTransform x;
    memset(&x, 0, sizeof(x));
    float c = cosf(yaw), s = sinf(yaw);
    x.right[0] = c; x.right[2] = -s;
    x.up[1] = 1;
    x.forward[0] = s; x.forward[2] = c;
    x.trans[0] = pos[0]; x.trans[1] = pos[1]; x.trans[2] = pos[2]; x.trans[3] = 1;
    return x;
}

// ---------------------------------------------------------------- maths de pose (pose_math.h de MECMP)
inline void quatMul(const float* a, const float* b, float* out) {
    float x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    float y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    float z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    float w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    out[0] = x; out[1] = y; out[2] = z; out[3] = w;
}
// Os 0 replié dans l'os 1 (l'ancre du corps) : les deux chemins de placement du jeu donnent `pos`.
inline void foldBone0(float* bone0, float* bone1, const float* pos) {
    float q[4];
    quatMul(&bone0[4], &bone1[4], q);
    float n = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (n > 1e-6f && finite(n))
        for (int j = 0; j < 4; ++j) bone1[4 + j] = q[j] / n;
    bone1[0] = pos[0]; bone1[1] = pos[1]; bone1[2] = pos[2]; bone1[3] = 1.f;
    static const float identity[8] = {0, 0, 0, 1, 0, 0, 0, 1};
    for (int j = 0; j < 8; ++j) bone0[j] = identity[j];
}

// Garde-fous sur une pose reçue (pose_limits.h de MECMP) : jamais de valeurs folles dans le jeu.
bool limitPose(PlayerState& st);

// ---------------------------------------------------------------- modules
namespace level {
struct BoneSetup { uint32_t count; uint32_t indices[kMaxBones]; uint8_t flags[kMaxBones]; };
void init();
void onSpawnerCreated(void* creationInfo, void* entity);
void onForeignClientReplayComp(void* comp);
bool complete();
void scanForMissing();
void reset();
uintptr_t serverWorld();
bool inFrontEnd();
void* playerSpawner();
void* ghostSpawner();
void chooseGhostSpawner(void* const* ours, size_t oursCount);
void spawnResult(void* spawner, bool gotCharacter);
void* playerCharacter();
bool spawnerAlive(void* spawner);
bool characterExists(void* character, uintptr_t expectedData);
bool referenceSetup(BoneSetup& out);
}  // namespace level

namespace remote {
// joueurs distants (reçus de coop.exe) : état interpolé pour maintenant
struct Info { uint32_t id; char name[32]; uint32_t ageMs; uint32_t look; };   // look : 1 Faith, 2 fantôme
size_t list(Info* out, size_t cap);
bool state(uint32_t id, PlayerState& out, uint32_t& ageMs);
// quand montrer (mon horloge, ms) ce que ce joueur a fait à senderMs (sur son horloge) : même délai que la pose
bool playTime(uint32_t id, uint32_t senderMs, double& localMs);
int delayMs(uint32_t id);   // délai d'affichage actuel de ce joueur (0 = inconnu)
}  // namespace remote

namespace models {
void init();
void noteTemplate(uintptr_t spawnerData);
void reset();
bool apply(void* ghost, int look);   // 1 = Faith, 2 = fantôme rouge
}  // namespace models

namespace ghosts {
void init();
bool onClientReplayComp(void* comp);
void onClientCharacter(void* ent);
void setCharacterDataType(uintptr_t ti);
void setEnabled(bool on);
void beforeClientReplayUpdate();
void renderFrame();
void tick(uint64_t ticksInLevel);
void levelChanged();
// pour les pseudos : position affichée du fantôme (pieds) si il est montré
bool shownPosition(uint32_t id, float out[3]);
void* clientCharacter(uint32_t peer);   // personnage client du fantôme montré (0 = aucun)
bool isGhostCharacter(void* serverCharacter);
void status(char* out, size_t cap);
}  // namespace ghosts

namespace local {
bool capture(PlayerState& st);
}

namespace tod {
float tick(int role, bool follow, float hostTime);   // renvoie mon heure à envoyer (-1 = rien)
const char* status();
}
namespace teleport {
bool request(uint32_t peer, const float* fixedPos);  // peer = joueur (0 : position fixedPos)
LONG result();                                       // 1 réussie, 2 échouée
float fadeAlpha();                                   // fondu au noir à dessiner (0..1)
void levelChanged();
void tick();
}

namespace mission {
// ma mission en cours (annoncée par le jeu) ; seq change à chaque début / fin
struct State { uint32_t seq, active, havePos; int32_t outcome; float pos[3]; char name[64]; char kind[32]; };
void init();
void clientTick();                                   // thread client : s'enregistre auprès du jeu
void serverTick(uintptr_t world, const float* pos);  // thread serveur : mes positions récentes
State current();
}
namespace menu {
void request(int kind);   // 0 : retour au menu principal ; 1 : recharger la sauvegarde (sans quitter le jeu)
void monitorTick();
void clientTick();
}
namespace notice {
bool text(char* out, size_t cap);   // message de coop à afficher en haut de l'écran
}
namespace invite {
// invitation reçue d'un autre joueur (texte écrit par coop) ; taken = déjà acceptée
bool text(char* out, size_t cap, bool* taken);
void accept();                                       // Alt+M
}

namespace audio {
constexpr uint32_t kMaxAudioParams = 16;
#pragma pack(push, 1)
struct Sample {                 // un son de mon personnage (même format dans la mémoire partagée)
    uint32_t timeMs;            // mon horloge (celle des poses)
    uint32_t event;             // empreinte de l'événement (0 = mise à jour des valeurs)
    uint8_t source, count;
    uint16_t pad;
    uint32_t key[kMaxAudioParams];
    float value[kMaxAudioParams];
};
struct InSample { uint32_t peer; Sample s; };
#pragma pack(pop)
static_assert(sizeof(Sample) == 140, "audio::Sample (coop.py)");
bool install();
void tick();                                  // thread client
void setLocalPos(const float* p);
void setEnabled(bool on);
void status(char* out, size_t cap);
void publish(const Sample& s);                // -> coop.exe (game_core)
int take(InSample* out, int cap);             // <- coop.exe (game_core)
}

namespace combat {
#pragma pack(push, 1)
struct Event { float pos[3]; float health, maxHealth, damage; uint32_t peer; };   // un coup sur un ennemi
#pragma pack(pop)
static_assert(sizeof(Event) == 28, "combat::Event (coop.py)");
void init();
bool install();
void tick();                                   // thread serveur
void noteSpawner(const char* blueprint, void* spawner);
void levelChanged();
void status(char* out, size_t cap);
void publish(const Event& e);                  // -> coop (game_core)
int take(Event* out, int cap);                 // <- coop (game_core)
}

namespace progression {   // progression de l'histoire (drapeaux) : celle de l'hôte appliquée chez les autres
#pragma pack(push, 1)
struct Flag { uint32_t hash; int32_t value; };
#pragma pack(pop)
static_assert(sizeof(Flag) == 8, "progression::Flag (coop.py)");
void init();
void serverTick();                             // thread serveur
void status(char* out, size_t cap);
void publish(const Flag& f);                   // -> coop (game_core)
int take(Flag* out, int cap, bool peek);       // <- coop (game_core)
void consumed(int n);
uint32_t snapshotRequest();                    // n° de demande de coop (toute ma table)
void setStats(uint32_t applied, uint32_t missing);
}

namespace research_ai {   // temporaire : ennemis, coups, K.O.
void clientTick();
void serverTick();
void noteSpawner(const char* blueprint, void* spawner);
void tick();
void init();
}
namespace research {   // temporaire : le câble du grappin
void tick();
void levelChanged();
}

namespace hooks {
typedef void (*TickFn)(void* world, float dt);
bool install(TickFn onTick, bool* ghostClients);
bool clientUpdateHooked();
}

// démarrage complet (thread du mod) : vérifie la version du jeu, pose les crochets, surveille le niveau
void start();
bool active();   // faux si une autre copie de l'overlay gère déjà les personnages 3D
}  // namespace game
