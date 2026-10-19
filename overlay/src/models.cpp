// =====================================================================================
//  MEC Coop - apparence des personnages des autres joueurs (Faith ou fantôme rouge)
//
//  Une apparence = la liste des pièces (VisualUnlockAsset : corps, tête, cheveux...) d'un
//  personnage. On fabrique une "CustomizeCharacterData" avec ces pièces et on l'applique au
//  fantôme (fonction du jeu vtable+0x340) ; elle est transmise au client comme une vraie.
//
//  Méthode reprise du mod MECMP (licence MIT) :
//    Copyright (c) 2026 MECMP contributors - https://github.com/CodeNameMeteor/mecmp
// =====================================================================================
#include "game.h"
#include <stdlib.h>

namespace game {
namespace models {

static const uintptr_t TI_ServerCharacterCustomizationComponent = 0x1427fe0f0;
static const uintptr_t Vtbl_PamCustomizeCharacterData = 0x141c883e0;
static const size_t PamCustomizeCharacterData_Size = 0x70;
static const uintptr_t Fn_RbTreeIncrement = 0x142a34820;
static const size_t Vtbl_ApplyCustomize = 0x340;
static const size_t SpawnerData_Template = 0x198, Template_Customization = 0x18;

struct Model { char name[160]; uintptr_t ccd; };
static Lock g_m;
static Model g_models[64];
static size_t g_modelCount = 0;
static uintptr_t g_ccdVtable = 0;
// apparences déjà fabriquées (jamais libérées : le jeu peut garder des pointeurs dessus)
struct Built { uintptr_t pieces[64]; size_t n; uintptr_t unlocks; uintptr_t ccd; };
static Built g_built[24];
static size_t g_builtCount = 0;

void init() { g_m.init(); }

static void lower(char* s) { for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s += 32; }

static bool assetName(uintptr_t asset, char* out, size_t cap) {
    return readCString(readOr<uintptr_t>(asset + Asset_Name), out, cap);
}

// appelé quand un spawner est vu : on retient les apparences de Faith et des fantômes
void noteTemplate(uintptr_t spawnerData) {
    uintptr_t tmpl = readOr<uintptr_t>(spawnerData + SpawnerData_Template);
    uintptr_t ccd = tmpl ? readOr<uintptr_t>(tmpl + Template_Customization) : 0;
    char name[160];
    if (!ccd || !assetName(ccd, name, sizeof(name))) return;
    if (strstr(name, "ErrorTemplate")) return;
    lower(name);
    // le fantôme est animé sur le squelette de Faith : seulement ses tenues et les fantômes
    if (!strstr(name, "faith") && !strstr(name, "projection")) return;
    Guard g(g_m);
    if (!g_ccdVtable) {
        uintptr_t vt = readOr<uintptr_t>(ccd);
        if (inImage(vt)) g_ccdVtable = vt;
    }
    for (size_t i = 0; i < g_modelCount; i++) if (g_models[i].ccd == ccd) return;
    if (g_modelCount >= 64) return;
    lstrcpynA(g_models[g_modelCount].name, name, sizeof(g_models[0].name));
    g_models[g_modelCount].ccd = ccd;
    g_modelCount++;
    logf("apparence trouvée : %s", name);
}

void reset() {
    Guard g(g_m);
    g_modelCount = 0;
    g_builtCount = 0;   // les pièces sont des données du niveau : on refabrique au prochain
}

// les pièces portées par un personnage du serveur (ensemble EASTL au composant de personnalisation +0x58)
static size_t readVisualSet(void* character, uintptr_t* out, size_t cap) {
    void* comp = getComponent(character, TI_ServerCharacterCustomizationComponent);
    if (!comp) return 0;
    uintptr_t set = (uintptr_t)comp + 0x58, anchor = set + 8;
    uintptr_t node = readOr<uintptr_t>(set + 0x10);
    typedef uintptr_t (*NextFn)(uintptr_t);
    NextFn next = (NextFn)rva(Fn_RbTreeIncrement);
    size_t n = 0;
    for (int guard = 0; node && node != anchor && guard < 128 && n < cap; ++guard) {
        uintptr_t v = readOr<uintptr_t>(node + 0x20);
        if (v) out[n++] = v;
        node = next(node);
    }
    return n;
}

// la 1re pièce de chaque groupe d'une CustomizeCharacterData (+0x18)
static size_t visualsOf(uintptr_t ccd, uintptr_t* out, size_t cap) {
    uintptr_t groups = readOr<uintptr_t>(ccd + 0x18);
    uint32_t ng = groups ? readOr<uint32_t>(groups - 4) : 0;
    size_t n = 0;
    for (uint32_t i = 0; i < ng && i < 64 && n < cap; ++i) {
        uintptr_t gr = readOr<uintptr_t>(groups + i * 8);
        if (gr && readOr<uint32_t>(gr - 4) > 0) {
            uintptr_t v = readOr<uintptr_t>(gr);
            if (v) out[n++] = v;
        }
    }
    return n;
}

// pièces vue "de l'intérieur" (1re personne) : invisibles de l'extérieur, on les enlève
static bool isFirstPersonPiece(uintptr_t v) {
    char name[200];
    if (!assetName(v, name, sizeof(name))) return false;
    lower(name);
    const char* leaf = strrchr(name, '/');
    leaf = leaf ? leaf + 1 : name;
    size_t len = strlen(leaf);
    return (len >= 2 && leaf[len - 2] == '1' && leaf[len - 1] == 'p') || strstr(leaf, "1p_") != NULL;
}

static uintptr_t makeCustomization(const uintptr_t* visuals, size_t n, uintptr_t vtable, uintptr_t unlockArray,
                                   uintptr_t preset) {
    if (!vtable) return 0;
    uint8_t* ccd = (uint8_t*)calloc(1, PamCustomizeCharacterData_Size);
    if (!ccd) return 0;
    if (preset && vtable == rva(Vtbl_PamCustomizeCharacterData) && !read(preset, ccd, PamCustomizeCharacterData_Size)) {
        free(ccd);
        return 0;
    }
    uint8_t* groups = (uint8_t*)calloc(1, 8 + n * 8);
    uint8_t* unlocks = (uint8_t*)calloc(1, 16);
    if (!groups || !unlocks) return 0;
    *(uint32_t*)(groups + 4) = (uint32_t)n;
    for (size_t i = 0; i < n; ++i) {
        uint8_t* gr = (uint8_t*)calloc(1, 16);
        if (!gr) return 0;
        *(uint32_t*)(gr + 4) = 1;
        *(uintptr_t*)(gr + 8) = visuals[i];
        *(uintptr_t*)(groups + 8 + i * 8) = (uintptr_t)(gr + 8);
    }
    *(uintptr_t*)ccd = vtable;
    static const char kName[] = "MECCoop/GhostModel";
    *(const char**)(ccd + Asset_Name) = kName;
    *(uintptr_t*)(ccd + 0x18) = (uintptr_t)(groups + 8);
    *(uintptr_t*)(ccd + 0x20) = unlockArray ? unlockArray : (uintptr_t)(unlocks + 8);
    ccd[0x31] = 1;   // ClearVisualState
    return (uintptr_t)ccd;
}

static bool applyPieces(void* ghost, uintptr_t* visuals, size_t n, uintptr_t unlockArray, uintptr_t vtableFrom,
                        const char* used) {
    uintptr_t kept[64];
    size_t k = 0;
    for (size_t i = 0; i < n; i++) if (!isFirstPersonPiece(visuals[i])) kept[k++] = visuals[i];
    if (k) { memcpy(visuals, kept, k * 8); n = k; }
    uintptr_t ccd = 0;
    {
        Guard g(g_m);
        uintptr_t vtable = vtableFrom ? readOr<uintptr_t>(vtableFrom) : 0;
        if (!inImage(vtable)) vtable = g_ccdVtable;
        for (size_t i = 0; i < g_builtCount && !ccd; i++)
            if (g_built[i].n == n && g_built[i].unlocks == unlockArray && memcmp(g_built[i].pieces, visuals, n * 8) == 0)
                ccd = g_built[i].ccd;
        if (!ccd) {
            ccd = makeCustomization(visuals, n, vtable, unlockArray, vtableFrom);
            if (!ccd) { logf("apparence : impossible de fabriquer « %s » pour l'instant", used); return false; }
            if (g_builtCount < 24 && n <= 64) {
                Built& b = g_built[g_builtCount++];
                memcpy(b.pieces, visuals, n * 8);
                b.n = n;
                b.unlocks = unlockArray;
                b.ccd = ccd;
            }
        }
    }
    uintptr_t ch = (uintptr_t)ghost, vt = 0, fn = 0;
    if (!read(ch, vt) || !inImage(vt) || !read(vt + Vtbl_ApplyCustomize, fn) || !inImage(fn)) return false;
    struct Args { void* ch; void* player; void* customize; uint8_t skip; uint8_t pad[7]; } args;
    static_assert(offsetof(Args, player) == 0x08 && offsetof(Args, customize) == 0x10 && offsetof(Args, skip) == 0x18,
                  "arguments de personnalisation");
    memset(&args, 0, sizeof(args));
    args.ch = ghost;
    args.customize = (void*)ccd;
    ((void (*)(void*, Args*))fn)(ghost, &args);
    logf("apparence : %s (%u pièces)", used, (unsigned)n);
    return true;
}

// look : 1 = Faith (la tenue que le jeu donne à Faith dans ce niveau), 2 = fantôme rouge (RunVis)
bool apply(void* ghost, int look) {
    uintptr_t visuals[64];
    size_t n = 0;
    uintptr_t vtableFrom = 0, unlocks = 0;
    if (look == 1) {
        void* player = level::playerCharacter();
        if (!player || !(n = readVisualSet(player, visuals, 64))) {
            logf("apparence : tenue de Faith illisible pour l'instant");
            return false;
        }
        return applyPieces(ghost, visuals, n, 0, 0, "Faith");
    }
    {
        Guard g(g_m);
        for (size_t i = 0; i < g_modelCount && !n; i++) {
            if (!strstr(g_models[i].name, "runvis")) continue;
            n = visualsOf(g_models[i].ccd, visuals, 64);
            vtableFrom = g_models[i].ccd;
            unlocks = readOr<uintptr_t>(g_models[i].ccd + 0x20);
        }
    }
    if (!n) return false;
    return applyPieces(ghost, visuals, n, unlocks, vtableFrom, "fantôme");
}

}  // namespace models
}  // namespace game
