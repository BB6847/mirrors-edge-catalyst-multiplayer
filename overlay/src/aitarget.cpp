// =====================================================================================
//  MEC Coop - les gardes voient les autres joueurs (ESSAI, seulement en « Test solo » pour l'instant)
//
//  Ce que le jeu fait : à chaque apparition de Faith il envoie AIPlayerEnableAsTargetMessage (catégorie « AI »
//  0x0059722d, type 0x63c0f317, vtable 0x141b5d758, 64 octets ; constructeur 0x144674e80) au serveur :
//    +0x30 = le spawner du personnage (PamServerCharacterSpawnEntity), +0x38 = 1 (peut être pris pour cible).
//  Les IA ajoutent alors ce personnage à leurs cibles. Essai : le même message, pour le spawner des fantômes
//  (les autres joueurs), construit et envoyé comme menu.cpp le fait pour ses messages.
// =====================================================================================
#include "game.h"

namespace game {
namespace aitarget {

static const uintptr_t Fn_MsgAlloc = 0x142a770c0, Arena_Messages = 0x142361940, Fn_MsgBaseCtor = 0x142a37120;
static const uintptr_t Fn_QueueMessage = 0x142a3ad30, kServerGameContext = 0x14253d670;
static const uintptr_t Vtbl_EnableAsTargetMsg = 0x141b5d758;
static const uint32_t kCatAI = 0x0059722d, kTypeEnableAsTarget = 0x63c0f317;

static void* g_doneFor = NULL;
static DWORD g_since = 0;

static bool post(void* spawner, bool enable) {
    uintptr_t ctx = readOr<uintptr_t>(rva(kServerGameContext));
    if (!ctx || !spawner) return false;
    void* m = ((void* (*)(uint64_t, uintptr_t))rva(Fn_MsgAlloc))(0x40, rva(Arena_Messages));
    if (!m) return false;
    memset(m, 0, 0x40);
    ((void (*)(void*, uint32_t, uint32_t, uint32_t))rva(Fn_MsgBaseCtor))(m, kCatAI, kTypeEnableAsTarget, 0xff);
    *(uintptr_t*)m = rva(Vtbl_EnableAsTargetMsg);
    *(void**)((uint8_t*)m + 0x30) = spawner;
    *((uint8_t*)m + 0x38) = enable ? 1 : 0;
    ((void (*)(uintptr_t, void*, float))rva(Fn_QueueMessage))(ctx + 0x10, m, 0.f);
    return true;
}

// thread serveur, à chaque tour ; role : 3 = test solo (seul cas essayé pour l'instant)
void tick(int role) {
    // DÉSACTIVÉ (25/10) : le jeu met dans ce message l'objet JOUEUR du personnage (personnage +0x250, envoyé
    // par 0x143382600 pour chaque personnage qui en a un, en direct par 0x142a39770), pas son spawner ; les
    // fantômes n'ont pas de joueur. Envoyer un spawner à la place risquait un plantage. Voir docs/recherche.md.
    (void)role;
    return;
    if (role != 3) return;
    void* sp = level::ghostSpawner();
    if (!sp || !level::spawnerAlive(sp) || sp == g_doneFor) return;
    // un peu après l'apparition du fantôme (son personnage doit exister)
    if (!g_since) { g_since = GetTickCount(); return; }
    if (GetTickCount() - g_since < 3000) return;
    g_since = 0;
    g_doneFor = sp;
    bool ok = post(sp, true);
    logf("gardes : ESSAI, les autres joueurs signalés comme cibles (spawner %p) : %s", sp, ok ? "envoyé" : "échec");
}

void levelChanged() { g_doneFor = NULL; g_since = 0; }

}  // namespace aitarget
}  // namespace game
