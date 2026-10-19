// =====================================================================================
//  MEC Coop - retour au menu principal sans quitter le jeu (pour changer de sauvegarde)
//
//  Le jeu sait déjà le faire : quand on recommence l'histoire, il envoie PamplonaRestartGameMessage
//  (catégorie « Pamplona ») ; avec un nom de niveau vide, ses écouteurs arrêtent la partie et chargent
//  « Levels/SP/FrontEnd/FrontEnd » (le menu principal). On envoie ce même message.
// =====================================================================================
#include "game.h"

namespace game {
namespace menu {


static volatile LONG g_request = 0;            // demande en attente (thread client)

// PamplonaRestartGameMessage, construit comme le jeu construit ses messages et posté dans le gestionnaire
// de messages du client : tous les écouteurs du jeu (déroulement de partie, interface...) réagissent comme
// s'il venait du jeu lui-même. (Appeler seulement la fonction du déroulement laissait un écran blanc.)
//   allocation 0x142a770c0(taille, arène 0x142361940) ; constructeur de base 0x142a37120(msg, catégorie,
//   type, 0xff) ; vtable 0x141c8eee0 ; +0x30 nom de niveau = chaîne vide du jeu (0x14215d04c) ;
//   envoi 0x142a3ad30(gestionnaire, msg, délai) — comme le jeu le fait (0x1438fc0c9).
static const uintptr_t Fn_MsgAlloc = 0x142a770c0, Arena_Messages = 0x142361940, Fn_MsgBaseCtor = 0x142a37120;
static const uintptr_t Vtbl_RestartGameMsg = 0x141c8eee0, Str_Empty = 0x14215d04c, Fn_QueueMessage = 0x142a3ad30;
static const uintptr_t kClientGameContext = 0x142401cb0;
static const uint32_t kCatPamplona = 0x0dc84585, kTypeRestartGame = 0x04aea817;
static const uintptr_t Fn_StringAssign = 0x142a471f0;   // (chaîne*, caractères, longueur) : copie dans la mémoire du jeu
// Le jeu envoie lui-même ce message avec « ResumeSaveGame » (0x143a7534e) : le déroulement de partie
// recharge alors le menu en lui demandant de reprendre la sauvegarde (0x1438fc091 : nom copié dans le
// ClientLoadLevelMessage). Avec un nom vide (ce que fait l'interface pour « quitter »), l'interface reste
// sur un écran blanc si on n'est pas passé par elle.
static volatile LONG g_kind = 0;               // 0 : menu principal, 1 : reprendre la sauvegarde

static bool postRestart() {
    uintptr_t ctx = readOr<uintptr_t>(rva(kClientGameContext));
    if (!ctx) return false;
    void* m = ((void* (*)(uint64_t, uintptr_t))rva(Fn_MsgAlloc))(0x38, rva(Arena_Messages));
    if (!m) return false;
    memset(m, 0, 0x38);
    ((void (*)(void*, uint32_t, uint32_t, uint32_t))rva(Fn_MsgBaseCtor))(m, kCatPamplona, kTypeRestartGame, 0xff);
    *(uintptr_t*)m = rva(Vtbl_RestartGameMsg);
    *(uintptr_t*)((uint8_t*)m + 0x30) = rva(Str_Empty);
    if (g_kind == 1) {
        static const char resume[] = "ResumeSaveGame";
        ((void (*)(void*, const char*, uint32_t))rva(Fn_StringAssign))((uint8_t*)m + 0x30, resume, 14);
    }
    ((void (*)(uintptr_t, void*, float))rva(Fn_QueueMessage))(ctx + 0x10, m, 0.f);
    return true;
}

void request(int kind) {
    // un nom vide (genre 0) remet l'histoire à zéro (vu le 19/10) : seul « reprendre la sauvegarde » est permis
    if (kind != 1) { logf("menu : demande de retour au menu refusée (remettrait l'histoire à zéro)"); return; }
    g_kind = kind;
    InterlockedExchange(&g_request, 1);
    logf(kind == 1 ? "menu : rechargement de la sauvegarde demandé" : "menu : retour au menu principal demandé");
}

void monitorTick() {}

// thread client : exécute la demande
void clientTick() {
    // depuis quand Faith est en jeu (suivi en continu)
    static void* lastPlayer = NULL;
    static DWORD playerSince = 0, lastCheck = 0;
    if (GetTickCount() - lastCheck >= 250) {
        lastCheck = GetTickCount();
        void* p = level::playerCharacter();
        if (p != lastPlayer) { lastPlayer = p; playerSince = GetTickCount(); }
    }
    if (!InterlockedExchange(&g_request, 0)) return;
    if (level::inFrontEnd()) { logf("menu : déjà au menu principal"); return; }
    // jamais pendant un chargement ou le démarrage du jeu : sans partie chargée, « reprendre » démarre une
    // NOUVELLE partie (vu le 19/10 : retour à Release). Il faut Faith en jeu depuis au moins 10 s.
    if (!lastPlayer || level::inFrontEnd() || GetTickCount() - playerSince < 10000) {
        logf("menu : refusé, la partie n'est pas encore chargée");
        return;
    }
    bool ok = postRestart();
    logf(ok ? (g_kind == 1 ? "menu : message « redémarrer la partie : ResumeSaveGame » envoyé au jeu"
                           : "menu : message « redémarrer la partie » envoyé au jeu")
            : "menu : impossible d'envoyer le message");
}

}  // namespace menu
}  // namespace game
