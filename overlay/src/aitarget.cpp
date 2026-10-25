// =====================================================================================
//  MEC Coop - les gardes voient les autres joueurs (ESSAI 2, seulement en « Test solo »)
//
//  Les gardes ne ciblent que les personnages qui ont un JOUEUR (ServerPlayer, personnage +0x250) :
//  AIPlayerEnableAsTargetMessage (+0x30 joueur, +0x38 oui) est envoyé par 0x143382600 pour ces personnages.
//  Les fantômes n'en ont pas. Le moteur sait en créer (comme pour un « bot », 0x143387960) :
//    0x14334beb0(gestionnaire des joueurs = [contexte serveur +0x68], 0, 0xff, -1, 0) -> nouveau ServerPlayer
//      (refus si plus de place : gestionnaire +0x10 = places, +0x910 = places réservées, +0x268.. = joueurs)
//    0x143349ce0(gestionnaire, joueur, nom)                 -> enregistré, nommé
//    personnage case 0x2d0 (0x143377c00)(personnage, joueur) -> personnage +0x250 = joueur
//    0x14334a4e0(joueur, personnage)                         -> le joueur contrôle ce personnage
//  puis le message, envoyé en direct comme le jeu (0x142a39770, message sur la pile, 0x144674e80 / 0x142d4e9a0).
// =====================================================================================
#include "game.h"

namespace game {
namespace aitarget {

static const uintptr_t kServerGameContext = 0x14253d670;
static const uintptr_t Fn_CreatePlayer = 0x14334beb0, Fn_RegisterPlayer = 0x143349ce0, Fn_PlayerSetCharacter = 0x14334a4e0;
static const uintptr_t Fn_SendNow = 0x142a39770, Fn_EnableTargetCtor = 0x144674e80, Fn_MsgDtor = 0x142d4e9a0;
static const size_t Slot_SetPlayer = 0x2d0;

static void* g_doneFor = NULL;
static DWORD g_seenAt = 0;
static uintptr_t g_player = 0;

static void enableAsTarget(uintptr_t player, bool on) {
    uintptr_t ctx = readOr<uintptr_t>(rva(kServerGameContext));
    if (!ctx || !player) return;
    alignas(16) uint8_t msg[0x60];
    memset(msg, 0, sizeof(msg));
    ((void (*)(void*))rva(Fn_EnableTargetCtor))(msg);
    *(uintptr_t*)(msg + 0x30) = player;
    msg[0x38] = on ? 1 : 0;
    ((void (*)(uintptr_t, void*))rva(Fn_SendNow))(ctx + 0x10, msg);
    ((void (*)(void*))rva(Fn_MsgDtor))(msg);
}

static void attach(void* ch) {
    uintptr_t ctx = readOr<uintptr_t>(rva(kServerGameContext));
    uintptr_t mgr = ctx ? readOr<uintptr_t>(ctx + 0x68) : 0;
    if (!mgr) { logf("gardes : ESSAI, pas de gestionnaire des joueurs"); return; }
    int places = readOr<int>(mgr + 0x10);
    uint32_t reserved = readOr<uint32_t>(mgr + 0x910);
    uintptr_t h0 = readOr<uintptr_t>(mgr + 0x268), h1 = readOr<uintptr_t>(mgr + 0x270);
    logf("gardes : ESSAI, joueurs : %d places, %u réservées, %d joueurs", places, reserved, (int)((h1 - h0) / 8));
    if (readOr<uintptr_t>((uintptr_t)ch + Character_Player)) { logf("gardes : le fantôme a déjà un joueur"); return; }
    typedef uintptr_t (*CreateFn)(uintptr_t, uint8_t, int, uint32_t, uint8_t);
    uintptr_t player = ((CreateFn)rva(Fn_CreatePlayer))(mgr, 0, 0xff, 0xffffffffu, 0);
    if (!player) {
        // plus de place (solo : sans doute 1) : une place de plus, en mémoire seulement
        poke<int>(mgr + 0x10, places + 1);
        player = ((CreateFn)rva(Fn_CreatePlayer))(mgr, 0, 0xff, 0xffffffffu, 0);
        logf("gardes : 1re création refusée (plus de place), avec une place de plus : %s", player ? "OK" : "refusée encore");
        if (!player) return;
    }
    logf("gardes : joueur créé %llx", (unsigned long long)player);
    static const char name[] = "Ami";
    ((void (*)(uintptr_t, uintptr_t, const char*))rva(Fn_RegisterPlayer))(mgr, player, name);
    logf("gardes : joueur enregistré");
    typedef void (*SetPlayerFn)(void*, uintptr_t);
    SetPlayerFn sp = (SetPlayerFn)gameVirtual(ch, Slot_SetPlayer);
    if (!sp) { logf("gardes : personnage sans case 0x2d0"); return; }
    sp(ch, player);
    logf("gardes : personnage relié au joueur");
    ((void (*)(uintptr_t, void*))rva(Fn_PlayerSetCharacter))(player, ch);
    logf("gardes : le joueur contrôle le personnage");
    enableAsTarget(player, true);
    g_player = player;
    logf("gardes : ESSAI, le fantôme est signalé comme cible aux gardes");
}

// thread serveur, à chaque tour ; role : 3 = test solo (seul cas essayé pour l'instant)
void tick(int role) {
    if (role != 3) return;
    void* ch = ghosts::firstServerCharacter();
    if (!ch || ch == g_doneFor) return;
    if (!g_seenAt) { g_seenAt = GetTickCount(); return; }
    if (GetTickCount() - g_seenAt < 4000) return;     // le personnage du fantôme doit être bien en place
    g_seenAt = 0;
    g_doneFor = ch;
    attach(ch);
}

void levelChanged() { g_doneFor = NULL; g_seenAt = 0; g_player = 0; }

}  // namespace aitarget
}  // namespace game
