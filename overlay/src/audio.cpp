// =====================================================================================
//  MEC Coop - sons des autres joueurs (pas, mains, atterrissages, respiration, efforts, coups)
//
//  Méthode, adresses et listes de sons du mod MECMP (licence MIT, © 2026 MECMP contributors,
//  src/game/player_audio.cpp, native_vocals.cpp, src/net/player_audio.h), réécrites ici sans la
//  bibliothèque C++ standard.
//
//  Chez moi : le gestionnaire d'événements de SoundEntity est remplacé (case de sa vtable) ; quand un
//  son de MON personnage reçoit un événement (pied posé, atterrissage...), on note l'événement et les
//  valeurs du son à cet instant (vitesse, matière du sol...) et coop.exe l'envoie aux autres.
//  Chez les autres : le même son du jeu est créé (SoundSystem), placé sur mon personnage 3D et reçoit
//  les mêmes valeurs puis le même événement, au même moment que la pose (même délai).
//  Seuls des numéros de son, des empreintes d'événements et des nombres voyagent ; un événement ou une
//  valeur que le son du jeu ne déclare pas est refusé.
// =====================================================================================
#include "game.h"

namespace game {
namespace audio {

static const uintptr_t kSoundEntityVtable = 0x141ad0338, kSoundEvent = 0x14301a170;
static const uintptr_t kSoundSystem = 0x1423dd580, kSoundSystemVtable = 0x141abd460;
static const uintptr_t kInstanceVtable = 0x141acfad0;
static const uintptr_t kSoundEntityDataType = 0x1427c7c80;
static const uintptr_t kNativePropertyValue = 0x142a424b0, kFloatType = 0x1427b0cb8;
static const uintptr_t kVocalEntityVtable = 0x141b2e078, kVocalEvent = 0x1432bc760;
static const uintptr_t kVocalDataType = 0x142810e60, kSoundUpdater = 0x1423e2f20;
static const uintptr_t kPrefabRefDataType = 0x1427e7a90;
// voix (VoiceOver) : native_vocals.cpp de MECMP
static const uintptr_t kVoSystem = 0x1423e3150, kBusLevel = 0x14314c5e0;
static const uintptr_t kVoTrigger = 0x1430477f0, kVoPronunciation = 0x143046760;
static const uintptr_t kVoTakeControls = 0x143046940, kVoSources = 0x1430467f0, kVoCancel = 0x143042720;
static const uint32_t kVocalTrigger = 0x9b59cebd, kVocalCancel = 0xa1e48be3;

static const char* const kSounds[] = {
    "ME_Sound/Character/Faith/Footsteps/Faith_Footplant",
    "ME_Sound/Character/Faith/Handsteps/Faith_Handplant", "ME_Sound/Character/Faith/Movement",
    "ME_Sound/Character/Faith/Faith_LandIntoHang", "ME_Sound/Character/Faith/Handsteps/PipeSlide",
    "ME_Sound/Character/Faith/Faith_HeadAgainstCeiling", "ME_Sound/Character/Faith/Footsteps/TarpaulinLanding",
    "ME_Sound/Character/Faith/Faith_LandOnPipe", "ME_Sound/Character/Faith/Handsteps/PipeRelease",
    "ME_Sound/Character/Faith/Faith_LandIntoLadder", "ME_Sound/Character/Faith/Faith_LandIntoPipe",
    "ME_Sound/Character/Faith/Faith_HandsAgainstWall", "ME_Sound/Character/Faith/Faith_CharacterAgainstWall",
    "ME_Sound/Character/Faith/Footsteps/Metal/MetalGantryCreaks",
    "ME_Sound/Character/Faith/Handsteps/MonkeyBar_Swing", "ME_Sound/Character/Faith/Faith_LandIntoBar",
    "ME_Sound/Character/Faith/Handsteps/LadderClimb",
    "ME_Sound/Character/Faith/Vocal/Emotes/Faith_Breathing_Emotes",
    "ME_Sound/Character/Faith/Melee/Faith_Combat_Melee",
    "ME_Sound/Character/AI/AI_Mario_Impact",
    "ME_Sound/Character/Faith/Melee/Faith_Damage_Impacts_Combat",
    "ME_Sound/Weapons/FlyBys/Faith_Nearmiss_Flyby",
    "ME_Sound/Character/Faith/FX/FaithCriticalHealth",
    "ME_Sound/Character/Faith/Melee/Impacts/Faith_Nearmiss_Damage",
    "ME_Sound/Character/Faith/Melee/Faith_Combat_DroneKick"};
static const char* const kVocals[] = {
    "ME_Sound/Character/Faith/Vocal/ME_Faith_Bump",
    "ME_Sound/Character/Faith/Vocal/ME_FaithLedgeStopExhale",
    "ME_Sound/Character/Faith/Vocal/ME_Faith_Scrape",
    "ME_Sound/Character/Faith/Vocal/ME_FaithJumpLight",
    "ME_Sound/Character/Faith/Vocal/Breath/ME_FaithDeathFallBreathing_Stereo",
    "ME_Sound/Character/Faith/Vocal/ME_Faith_DamageLite",
    "ME_Sound/Character/Faith/Vocal/ME_Faith_Damage",
    "ME_Sound/Character/Faith/Vocal/ME_Faith_DamageInjuredBreathTail",
    "ME_Sound/Character/Faith/Vocal/ME_Faith_StrainLite",
    "ME_Sound/Character/Faith/Vocal/ME_Faith_PunchJab",
    "ME_Sound/Character/Faith/Vocal/ME_Faith_PunchFull",
    "ME_Sound/Character/Faith/Vocal/ME_Faith_LandSoft",
    "ME_Sound/Character/Faith/Vocal/ME_Faith_LandHard",
    "ME_Sound/Character/Faith/Vocal/ME_FaithBreathMouthFastIn_Plus",
    "ME_Sound/Character/Faith/Vocal/ME_FaithHurtBreathing"};
static const size_t kSoundCount = sizeof(kSounds) / sizeof(kSounds[0]);
static const size_t kVocalCount = sizeof(kVocals) / sizeof(kVocals[0]);
static const size_t kSources = kSoundCount + kVocalCount;
static inline bool vocalSource(uint32_t s) { return s >= kSoundCount && s < kSources; }

static const float kDistance = 80.f;          // au-delà, on n'entend plus l'autre
static const size_t kMaxPeers = 8;

typedef void (*EventFn)(uintptr_t, const void*);
static EventFn g_original = NULL, g_originalVocal = NULL;
static volatile bool g_installed = false, g_vocalsInstalled = false;
static Lock g_m;

struct Source {
    uintptr_t data, asset, entity;
    Sample previous;
    DWORD sentAt, durationMs;
};
static Source g_src[kSources];
static uintptr_t g_localData[kSources];       // les données de son de MON personnage (index = source)
static uint16_t g_voiceRealm = 0;
static bool g_haveRealm = false;
static Sample g_captured[64];
static DWORD g_capturedAt[64];
static int g_capHead = 0, g_capFill = 0;
static volatile LONG g_captures = 0, g_played = 0;
static volatile bool g_enabled = true;

// ------------------------------------------------------------------- aides
static uint32_t timestamp() {
    static LARGE_INTEGER f = {{0, 0}};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (uint32_t)((double)c.QuadPart * 1000.0 / (double)f.QuadPart);
}
static int sourceIndex(uintptr_t asset) {
    char name[192];
    if (!asset || !readCString(readOr<uintptr_t>(asset + Asset_Name), name, sizeof(name))) return -1;
    for (size_t i = 0; i < kSoundCount; ++i) if (strcmp(name, kSounds[i]) == 0) return (int)i;
    return -1;
}
// seules les entrées déclarées par le graphe du son peuvent être pilotées
static bool graphKey(uintptr_t asset, size_t listOffset, uint32_t key) {
    uintptr_t graph = readOr<uintptr_t>(asset + 0x38);
    uintptr_t list = graph ? readOr<uintptr_t>(graph + listOffset) : 0;
    uint32_t count = list ? readOr<uint32_t>(list - 4) : 0;
    if (count > 256) return false;
    for (uint32_t i = 0; i < count; ++i) {
        uintptr_t e = readOr<uintptr_t>(list + i * 8);
        if (e && readOr<uint32_t>(e + 0x14) == key) return true;
    }
    return false;
}
static void snapshot(uintptr_t entity, uint32_t source, uint32_t event, Sample& s) {
    memset(&s, 0, sizeof(s));
    s.source = (uint8_t)source;
    s.event = event;
    s.timeMs = timestamp();
    uintptr_t begin = 0, end = 0;
    if (!read(entity + 0x48, begin) || !read(entity + 0x50, end) || end < begin || (end - begin) % 0x28 ||
        end - begin > 128 * 0x28)
        return;
    typedef uintptr_t (*ValueFn)(uintptr_t, uintptr_t);
    ValueFn value = (ValueFn)rva(kNativePropertyValue);
    for (uintptr_t p = begin; p < end && s.count < kMaxAudioParams; p += 0x28) {
        // table des propriétés du son : genre 0 = nombre, 1 = position, 2 = son ; on n'envoie que des nombres
        if (readOr<uint32_t>(p + 0x24, ~0u) != 0) continue;
        uint32_t key = readOr<uint32_t>(p + 0x20);
        if (!key) continue;
        uintptr_t v = value(p, rva(kFloatType));
        float f = 0;
        if (!v || !read(v, f) || !finite(f) || fabsf(f) > 100000.f) continue;
        s.key[s.count] = key;
        s.value[s.count] = f;
        s.count++;
    }
}
static bool sameInputs(const Sample& a, const Sample& b) {
    if (a.count != b.count) return false;
    for (uint8_t i = 0; i < a.count; ++i) if (a.key[i] != b.key[i] || a.value[i] != b.value[i]) return false;
    return true;
}
static int localIndex(uintptr_t data) {   // sous g_m
    if (!data) return -1;
    for (size_t i = 0; i < kSources; i++) if (g_localData[i] == data) return (int)i;
    return -1;
}
static void pushCaptured(const Sample& s) {   // sous g_m
    g_captured[g_capHead] = s;
    g_capturedAt[g_capHead] = GetTickCount();
    g_capHead = (g_capHead + 1) % 64;
    if (g_capFill < 64) g_capFill++;
    InterlockedIncrement(&g_captures);
}
static uint16_t realmForBus(uintptr_t bus) {
    if (!bus) return 0;
    uintptr_t level = ((uintptr_t (*)(uintptr_t))rva(kBusLevel))(bus);
    return level ? readOr<uint16_t>(level + 0x90) : 0;
}
static void rememberRealm(uintptr_t entity) {   // sous g_m
    if (g_haveRealm) return;
    uintptr_t bus = readOr<uintptr_t>(entity + 0x20);
    if (bus) { g_voiceRealm = realmForBus(bus); g_haveRealm = true; }
}

// ------------------------------------------------------------------- capture (chez moi)
static void eventHook(uintptr_t entity, const void* event) {
    g_original(entity, event);              // le son du jeu d'abord, intact
    if (!g_enabled) return;
    DWORD err = GetLastError();
    // entité vivante (le jeu l'appelle) : +0x28 = ses données ; on ne garde que les sons de MON personnage
    uintptr_t data = *(volatile uintptr_t*)(entity + 0x28);
    if (data) {
        Guard g(g_m);
        int index = localIndex(data);
        if (index >= 0 && (size_t)index < kSoundCount) {
            Source& src = g_src[index];
            uint32_t hash = 0;
            if (event && read((uintptr_t)event + 8, hash) && src.asset && graphKey(src.asset, 0x80, hash)) {
                rememberRealm(entity);
                Sample s;
                snapshot(entity, (uint32_t)index, hash, s);
                src.entity = entity;
                src.previous = s;
                src.sentAt = GetTickCount();
                pushCaptured(s);
            }
        }
    }
    SetLastError(err);
}
static void vocalEventHook(uintptr_t entity, const void* event) {
    DWORD before = GetLastError();
    uint32_t hash = 0;
    if (event) read((uintptr_t)event + 8, hash);
    uint32_t flags = readOr<uint32_t>(entity + 0x3c);
    SetLastError(before);
    g_originalVocal(entity, event);
    DWORD after = GetLastError();
    if (g_enabled && (hash == kVocalCancel || (hash == kVocalTrigger && !(flags & 2)))) {
        uintptr_t data = readOr<uintptr_t>(entity + 0x28);
        Guard g(g_m);
        int index = localIndex(data);
        if (index >= 0 && vocalSource((uint32_t)index)) {
            rememberRealm(entity);
            Sample s;
            memset(&s, 0, sizeof(s));
            s.source = (uint8_t)index;
            s.event = hash;
            s.timeMs = timestamp();
            pushCaptured(s);
        }
    }
    SetLastError(after);
}

bool install() {
    if (g_installed) return true;
    g_m.init();
    uintptr_t slot = rva(kSoundEntityVtable) + 0x20;
    if (readOr<uintptr_t>(slot) != rva(kSoundEvent)) {
        logf("sons : gestionnaire de SoundEntity inattendu, sons des joueurs désactivés");
        return false;
    }
    g_original = (EventFn)rva(kSoundEvent);
    if (!patchPointer(slot, (uintptr_t)&eventHook, NULL)) return false;
    g_installed = true;
    uintptr_t vslot = rva(kVocalEntityVtable) + 0x20;
    if (readOr<uintptr_t>(vslot) == rva(kVocalEvent)) {
        g_originalVocal = (EventFn)rva(kVocalEvent);
        g_vocalsInstalled = patchPointer(vslot, (uintptr_t)&vocalEventHook, NULL);
    }
    logf("sons : capture installée (mouvements, combat, respiration) ; voix %s", g_vocalsInstalled ? "installées" : "indisponibles");
    return true;
}

// les sons de MON personnage (ses composants, et ceux des prefabs de sons de Faith)
static void catalogue(uintptr_t character) {
    uintptr_t data = readOr<uintptr_t>(character + Character_Data);
    uintptr_t list = data ? readOr<uintptr_t>(data + 0x60) : 0;
    uint32_t count = list ? readOr<uint32_t>(list - 4) : 0;
    if (count > 256) return;
    Guard g(g_m);
    struct Item { uintptr_t c; int depth; };
    static Item stack[1024];
    static uintptr_t visited[2048];
    int sp = 0, nv = 0;
    for (uint32_t i = 0; i < count && sp < 1024; ++i) stack[sp++] = {readOr<uintptr_t>(list + i * 8), 0};
    while (sp > 0) {
        Item it = stack[--sp];
        uintptr_t c = it.c;
        if (!c || it.depth > 3) continue;
        bool seen = false;
        for (int k = 0; k < nv && !seen; k++) seen = visited[k] == c;
        if (seen || nv >= 2048) continue;
        visited[nv++] = c;
        if (isKindOf((void*)c, kSoundEntityDataType)) {
            uintptr_t asset = readOr<uintptr_t>(c + 0x18);
            int index = sourceIndex(asset);
            if (index >= 0) { g_src[index].data = c; g_src[index].asset = asset; g_localData[index] = c; }
        } else if (isKindOf((void*)c, kVocalDataType)) {
            uintptr_t tracks = readOr<uintptr_t>(c + 0x20), groups = readOr<uintptr_t>(c + 0x18);
            if (!tracks || !groups || readOr<uint32_t>(tracks - 4) != 1 || readOr<uint32_t>(groups - 4) != 1) continue;
            uintptr_t track = readOr<uintptr_t>(tracks);
            uintptr_t clips = track ? readOr<uintptr_t>(track + 0x58) : 0;
            if (!clips || readOr<uint32_t>(clips - 4) != 1) continue;
            uintptr_t clip = readOr<uintptr_t>(clips);
            uintptr_t takes = clip ? readOr<uintptr_t>(clip + 0x18) : 0;
            if (!takes || readOr<uint32_t>(takes - 4) != 1) continue;
            uintptr_t wave = readOr<uintptr_t>(takes);
            char name[192];
            if (!wave || !readCString(readOr<uintptr_t>(wave + Asset_Name), name, sizeof(name))) continue;
            for (size_t i = 0; i < kVocalCount; ++i)
                if (strcmp(name, kVocals[i]) == 0) {
                    size_t index = kSoundCount + i;
                    g_src[index].data = c;
                    g_src[index].asset = wave;
                    float d = readOr<float>(wave + 0x68, 2.f);
                    if (!finite(d)) d = 2.f;
                    d = d < .1f ? .1f : (d > 15.f ? 15.f : d);
                    g_src[index].durationMs = (DWORD)(d * 1000.f) + 500;
                    g_localData[index] = c;
                    break;
                }
        }
        if (!isKindOf((void*)c, kPrefabRefDataType)) continue;
        uintptr_t bp = readOr<uintptr_t>(c + 0x18);
        char name[128];
        if (!bp || !readCString(readOr<uintptr_t>(bp + Asset_Name), name, sizeof(name))) continue;
        if (strcmp(name, "Sound/Logic/Faith_Movement_Audio") && strcmp(name, "Sound/Logic/Faith_Combat_Audio") &&
            strcmp(name, "ME_Sound/Character/Faith_Emotes"))
            continue;
        uintptr_t objs = readOr<uintptr_t>(bp + 0x40);
        uint32_t n = objs ? readOr<uint32_t>(objs - 4) : 0;
        if (n > 256) continue;
        for (uint32_t j = 0; j < n && sp < 1024; ++j) stack[sp++] = {readOr<uintptr_t>(objs + j * 8), it.depth + 1};
    }
    int graphs = 0, vocals = 0;
    for (size_t i = 0; i < kSources; i++) if (g_src[i].data) { if (i < kSoundCount) graphs++; else vocals++; }
    logf("sons : %d sons et %d voix de Faith repérés", graphs, vocals);
}

// la respiration en boucle peut avoir commencé avant le catalogue : la liste des sons actifs du jeu
static void discoverActive() {
    uintptr_t up = readOr<uintptr_t>(rva(kSoundUpdater));
    uintptr_t begin = up ? readOr<uintptr_t>(up + 0x28) : 0, end = up ? readOr<uintptr_t>(up + 0x30) : 0;
    if (!begin || end < begin || (end - begin) % 8 || end - begin > 8192 * 8) return;
    Guard g(g_m);
    for (uintptr_t p = begin; p < end; p += 8) {
        uintptr_t sub = readOr<uintptr_t>(p);
        if (sub < 0x10030) continue;
        uintptr_t e = sub - 0x30;
        if (readOr<uintptr_t>(e) != rva(kSoundEntityVtable)) continue;
        int index = localIndex(readOr<uintptr_t>(e + 0x28));
        if (index < 0 || (size_t)index >= kSoundCount) continue;
        g_src[index].entity = e;
        rememberRealm(e);
    }
}

// ------------------------------------------------------------------- lecture (chez les autres)
struct Vocal {                       // gardé à une adresse fixe tant que le jeu s'en sert
    alignas(8) uint8_t conversation[48];
    uintptr_t system;
    uint32_t handle;
    DWORD unreferencedAt;
    bool used, retiring;
};
static Vocal g_vocals[160];
struct Playing { uintptr_t instance; DWORD heardAt, lifetimeMs; Vocal* vocal; };
struct Peer { uint32_t id; bool used; Playing s[kSources]; DWORD window; int burst; };
static Peer g_peers[kMaxPeers];
struct Pending { uint32_t peer; DWORD receivedAt; double playAt; Sample s; };
static Pending g_pending[256];
static int g_pendingCount = 0;

static bool vocalStart(Vocal& v, uintptr_t data, uintptr_t character, uint16_t realm) {
    v.system = readOr<uintptr_t>(rva(kVoSystem));
    if (!v.system || !character || readOr<uintptr_t>(character) != rva(Vtbl_PamClientCharacterEntity) ||
        !read(data + 0x18, v.conversation, sizeof(v.conversation)))
        return false;
    uintptr_t info = (uintptr_t)v.conversation;
    for (size_t off = 0; off <= 8; off += 8) {
        uintptr_t list = readOr<uintptr_t>(info + off);
        if (!list || readOr<uint32_t>(list - 4) != 1) return false;
    }
    v.handle = 0;
    ((void (*)(uintptr_t, uintptr_t, uint16_t, uint32_t*))rva(kVoTrigger))(v.system, info, realm, &v.handle);
    if (!v.handle) return false;
    int32_t pron = readOr<int32_t>(data + 0x58, -1);
    ((void (*)(uintptr_t, uintptr_t, int32_t))rva(kVoPronunciation))(v.system, info, pron);
    const float take = 0.f;
    ((void (*)(uintptr_t, uintptr_t, const float*, uint32_t, uint32_t))rva(kVoTakeControls))(v.system, info, &take, 0, 1);
    // la voix suit le personnage 3D de l'autre (pas ma voix à moi)
    ((void (*)(uintptr_t, uintptr_t, const uintptr_t*, uint32_t, uint32_t))rva(kVoSources))(v.system, info, &character, 0, 1);
    return true;
}
static void vocalCancel(const Vocal& v) {
    if (v.handle && v.system == readOr<uintptr_t>(rva(kVoSystem)))
        ((void (*)(uintptr_t, uint32_t))rva(kVoCancel))(v.system, v.handle);
}
static bool vocalReferenced(const Vocal& v) {
    if (!v.handle || v.system != readOr<uintptr_t>(rva(kVoSystem))) return false;
    uintptr_t begin = 0, end = 0;
    if (!read(v.system + 0x17df0, begin) || !read(v.system + 0x17df8, end) || end < begin || (end - begin) % 0x20 ||
        end - begin > 4096 * 0x20)
        return true;
    for (uintptr_t e = begin; e < end; e += 0x20) {
        uint32_t h = 0;
        if (!read(e + 0x10, h)) return true;
        if (h == v.handle) return true;
    }
    uintptr_t node = 0;
    if (!read(v.system + 0x15e20, node)) return true;
    for (int depth = 0; node && depth < 64; ++depth) {
        uint32_t h = 0;
        if (!read(node + 0x20, h)) return true;
        if (h == v.handle) return true;
        if (!read(node + (v.handle < h ? 8 : 0), node)) return true;
    }
    return node != 0;
}
static void release(Playing& p) {
    if (p.vocal) { vocalCancel(*p.vocal); p.vocal->retiring = true; p.vocal = NULL; }
    if (p.instance && readOr<uintptr_t>(p.instance) == rva(kInstanceVtable)) {
        uintptr_t stop = gameVirtual((void*)p.instance, 0x40), dispose = gameVirtual((void*)p.instance, 0x48);
        if (stop) ((void (*)(uintptr_t, int))stop)(p.instance, 1);
        if (dispose) ((void (*)(uintptr_t))dispose)(p.instance);
    }
    memset(&p, 0, sizeof(p));
}
static void retireVocals() {
    DWORD now = GetTickCount();
    for (size_t i = 0; i < 160; i++) {
        Vocal& v = g_vocals[i];
        if (!v.used || !v.retiring) continue;
        if (!vocalReferenced(v)) {
            if (!v.unreferencedAt) v.unreferencedAt = now;
            if (now - v.unreferencedAt >= 1000) { memset(&v, 0, sizeof(v)); continue; }
        } else v.unreferencedAt = 0;
        vocalCancel(v);
    }
}
static void clearSounds() {
    for (size_t i = 0; i < kMaxPeers; i++) {
        if (!g_peers[i].used) continue;
        for (size_t k = 0; k < kSources; k++) release(g_peers[i].s[k]);
        g_peers[i].used = false;
    }
    g_pendingCount = 0;
}
static void place(uintptr_t instance, const PlayerState& st) {
    alignas(16) LinearTransform t = transformFromYaw(st.pos, st.yaw);
    uintptr_t fn = gameVirtual((void*)instance, 0x68);
    if (fn) ((void (*)(uintptr_t, const LinearTransform*))fn)(instance, &t);
    alignas(16) const float zero[4] = {0, 0, 0, 0};       // vitesse nulle : pas d'effet Doppler aux téléportations
    fn = gameVirtual((void*)instance, 0x78);
    if (fn) ((void (*)(uintptr_t, const float*))fn)(instance, zero);
}
static Peer* peerFor(uint32_t id, bool create) {
    for (size_t i = 0; i < kMaxPeers; i++) if (g_peers[i].used && g_peers[i].id == id) return &g_peers[i];
    if (!create) return NULL;
    for (size_t i = 0; i < kMaxPeers; i++)
        if (!g_peers[i].used) {
            memset(&g_peers[i], 0, sizeof(Peer));
            g_peers[i].used = true;
            g_peers[i].id = id;
            return &g_peers[i];
        }
    return NULL;
}

static bool validSample(const Sample& s) {
    if (s.source >= kSources || s.count > kMaxAudioParams) return false;
    if (vocalSource(s.source)) return !s.count && (s.event == kVocalTrigger || s.event == kVocalCancel);
    for (uint8_t i = 0; i < s.count; ++i) {
        if (!s.key[i] || !finite(s.value[i]) || fabsf(s.value[i]) > 100000.f) return false;
        for (uint8_t j = 0; j < i; ++j) if (s.key[j] == s.key[i]) return false;
    }
    return true;
}

static float g_myPos[3] = {0, 0, 0};
static volatile DWORD g_myPosAt = 0;
void setLocalPos(const float* p) { memcpy(g_myPos, p, 12); g_myPosAt = GetTickCount(); }
void setEnabled(bool on) { g_enabled = on; }

static bool nearby(uint32_t peer, PlayerState& st) {
    uint32_t age = 0;
    if (!remote::state(peer, st, age) || age > 1000) return false;
    if (GetTickCount() - g_myPosAt > 1500) return false;
    float d = 0;
    for (int i = 0; i < 3; i++) d += (st.pos[i] - g_myPos[i]) * (st.pos[i] - g_myPos[i]);
    return d <= kDistance * kDistance;
}

static double nowMs() {
    static LARGE_INTEGER f = {{0, 0}};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

// thread client, à chaque mise à jour des replays (même moment que les poses)
void tick() {
    if (!g_installed) return;
    static DWORD lastTick = 0;
    DWORD now = GetTickCount();
    if (now == lastTick) return;          // deux passes de mise à jour dans la même image
    lastTick = now;
    retireVocals();
    static uintptr_t lastWorld = 0, lastChar = 0;
    static DWORD discoverAt = 0;
    uintptr_t world = level::serverWorld();
    uintptr_t character = world && !level::inFrontEnd() ? (uintptr_t)((void* (*)())rva(Fn_LocalClientCharacter))() : 0;
    if (world != lastWorld || character != lastChar) {
        clearSounds();
        {
            Guard g(g_m);
            memset(g_src, 0, sizeof(g_src));
            memset(g_localData, 0, sizeof(g_localData));
            g_capFill = 0;
            g_haveRealm = false;
        }
        lastWorld = world;
        lastChar = character;
        discoverAt = now - 1000;
        if (character) catalogue(character);
    }
    if (!world || !character) { clearSounds(); return; }
    if (now - discoverAt >= 1000) { discoverActive(); discoverAt = now; }

    // 1) mes sons -> coop.exe
    Sample out[64];
    int nout = 0;
    static Source src[kSources];
    uint16_t realm = 0;
    bool haveRealm = false;
    {
        Guard g(g_m);
        for (int k = g_capFill; k > 0; k--) {
            int idx = (g_capHead - k + 64) % 64;
            if (now - g_capturedAt[idx] < 500) out[nout++] = g_captured[idx];
        }
        g_capFill = 0;
        memcpy(src, g_src, sizeof(src));
        realm = g_voiceRealm;
        haveRealm = g_haveRealm;
    }
    for (int i = 0; i < nout; i++) publish(out[i]);
    // valeurs qui changent (vitesse, matière) et signe de vie des sons actifs
    for (size_t i = 0; i < kSoundCount; ++i) {
        Source& s = src[i];
        if (!s.entity || now - s.sentAt < 200 || readOr<uintptr_t>(s.entity) != rva(kSoundEntityVtable) ||
            readOr<uintptr_t>(s.entity + 0x28) != s.data || !readOr<uintptr_t>(s.entity + 0x88))
            continue;
        Sample smp;
        snapshot(s.entity, (uint32_t)i, 0, smp);
        if (sameInputs(smp, s.previous) && now - s.sentAt < 750) continue;
        publish(smp);
        Guard g(g_m);
        g_src[i].previous = smp;
        g_src[i].sentAt = now;
    }

    // 2) les sons des autres
    if (!g_enabled) { clearSounds(); return; }
    for (size_t i = 0; i < kMaxPeers; i++) {
        Peer& p = g_peers[i];
        if (!p.used) continue;
        PlayerState st;
        if (!nearby(p.id, st)) {
            for (size_t k = 0; k < kSources; k++) release(p.s[k]);
            p.used = false;
            continue;
        }
        for (size_t k = 0; k < kSources; k++) {
            Playing& s = p.s[k];
            if (!s.instance && !s.vocal) continue;
            if (now - s.heardAt > s.lifetimeMs) release(s);
            else if (s.instance) place(s.instance, st);
        }
    }
    // reçus : mis en attente jusqu'au moment où la pose correspondante est montrée
    InSample in[64];
    int nin = take(in, 64);
    double tnow = nowMs();
    for (int i = 0; i < nin; i++) {
        double localMs = 0;
        if (!remote::playTime(in[i].peer, in[i].s.timeMs, localMs)) continue;
        double until = localMs - tnow;
        if (until < -500.0 || until > 500.0) continue;
        if (g_pendingCount >= 256) { memmove(g_pending, g_pending + 1, sizeof(Pending) * 255); g_pendingCount = 255; }
        Pending& pd = g_pending[g_pendingCount++];
        pd.peer = in[i].peer;
        pd.receivedAt = now;
        pd.playAt = localMs;
        pd.s = in[i].s;
    }
    int keep = 0;
    for (int i = 0; i < g_pendingCount; i++) {
        Pending pd = g_pending[i];
        if (now - pd.receivedAt > 750) continue;
        if (pd.playAt > tnow) { g_pending[keep++] = pd; continue; }
        const Sample& smp = pd.s;
        if (!validSample(smp)) continue;
        PlayerState st;
        if (!nearby(pd.peer, st)) continue;
        Peer* peer = peerFor(pd.peer, true);
        if (!peer) continue;
        if (now - peer->window >= 1000) { peer->window = now; peer->burst = 0; }
        if (++peer->burst > 64) continue;            // au plus 64 sons par seconde et par joueur
        const Source& source = src[smp.source];
        bool vocal = vocalSource(smp.source);
        if (!source.asset) continue;
        if (!vocal) {
            if (sourceIndex(source.asset) != (int)smp.source || (smp.event && !graphKey(source.asset, 0x80, smp.event))) continue;
            bool ok = true;
            for (uint8_t k = 0; k < smp.count && ok; ++k) ok = graphKey(source.asset, 0x70, smp.key[k]);
            if (!ok) continue;
        }
        Playing& sound = peer->s[smp.source];
        if (vocal) {
            if (smp.event == kVocalCancel) { release(sound); continue; }
            if (!g_vocalsInstalled || !haveRealm) continue;
            uintptr_t actor = (uintptr_t)ghosts::clientCharacter(pd.peer);
            if (!actor) continue;
            release(sound);
            Vocal* v = NULL;
            for (size_t k = 0; k < 160 && !v; k++) if (!g_vocals[k].used) v = &g_vocals[k];
            if (!v) continue;
            memset(v, 0, sizeof(*v));
            v->used = true;
            if (!vocalStart(*v, source.data, actor, realm)) { v->retiring = true; continue; }
            sound.vocal = v;
            sound.heardAt = now;
            sound.lifetimeMs = source.durationMs ? source.durationMs : 2000;
            InterlockedIncrement(&g_played);
            continue;
        }
        if (!sound.instance) {
            uintptr_t system = readOr<uintptr_t>(rva(kSoundSystem));
            if (!system || readOr<uintptr_t>(system) != rva(kSoundSystemVtable)) continue;
            uintptr_t create = gameVirtual((void*)system, 0x40);
            if (!create) continue;
            sound.instance = ((uintptr_t (*)(uintptr_t, uintptr_t))create)(system, source.asset);
            if (!sound.instance || readOr<uintptr_t>(sound.instance) != rva(kInstanceVtable)) { sound.instance = 0; continue; }
            place(sound.instance, st);
            uintptr_t start = gameVirtual((void*)sound.instance, 0x40);   // comme SoundEntity::onInit
            if (start) ((void (*)(uintptr_t, int))start)(sound.instance, 0);
        }
        sound.heardAt = now;
        sound.lifetimeMs = 2000;
        place(sound.instance, st);
        uintptr_t setInput = gameVirtual((void*)sound.instance, 0x18), fire = gameVirtual((void*)sound.instance, 0);
        if (setInput)
            for (uint8_t k = 0; k < smp.count; ++k) ((void (*)(uintptr_t, uint32_t, float))setInput)(sound.instance, smp.key[k], smp.value[k]);
        if (smp.event && fire) ((void (*)(uintptr_t, uint32_t))fire)(sound.instance, smp.event);
        InterlockedIncrement(&g_played);
    }
    g_pendingCount = keep;
}

void status(char* out, size_t cap) {
    if (!g_installed) { lstrcpynA(out, "sons : indisponibles", (int)cap); return; }
    wsprintfA(out, "sons : %ld envoyés, %ld joués", g_captures, g_played);
}

}  // namespace audio
}  // namespace game
