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
static const size_t Slot_SetPlayer = 0x2d0, Player_Team = 0x139c;
static const uintptr_t Fn_PlayerSetTeam = 0x143359070;   // (joueur, équipe) : aussi l'équipe de son personnage

// Coups des gardes sur un JOUEUR (lu le 25/10) : 0x1439242f0(objet, message), appelé par la case 0x141c40670 :
//   message +0x90 = n° du joueur touché, +0x98 = n° de celui qui frappe (table : gestionnaire des joueurs +0x920)
//   -> personnage du joueur (joueur +0x1478) -> son composant de santé -> 0x1434d94e0 (dégâts appliqués).
//   dégâts = message +0xc0 ; type = [message aligné + 0x28] (102 = coup de poing de garde).
//   L'écho n'a pas de composant de santé : ses coups s'arrêtent là. On les note (et plus tard : envoyés à l'autre joueur).
static const uintptr_t Slot_PlayerDamage = 0x141c40670, Fn_PlayerDamage = 0x1439242f0, Glob_AlignAdd = 0x142119008;
typedef void (*PlayerDamageFn)(void*, void*);
static PlayerDamageFn g_origDamage = NULL;
static volatile uintptr_t g_echoPlayer = 0;
static volatile LONG g_echoHits = 0, g_otherHits = 0;

static uintptr_t playerByIndex(uint32_t idx) {
    if (idx == 0xffffffffu) return 0;
    uintptr_t ctx = readOr<uintptr_t>(rva(kServerGameContext));
    uintptr_t mgr = ctx ? readOr<uintptr_t>(ctx + 0x68) : 0;
    uintptr_t tab = mgr ? readOr<uintptr_t>(mgr + 0x920) : 0;
    return tab && idx < 4096 ? readOr<uintptr_t>(tab + (uintptr_t)idx * 8) : 0;
}

static void hookDamage(void* self, void* msg) {
    uint32_t vi = readOr<uint32_t>((uintptr_t)msg + 0x90, 0xffffffffu), ai = readOr<uint32_t>((uintptr_t)msg + 0x98, 0xffffffffu);
    uintptr_t victim = playerByIndex(vi);
    float dmg = readOr<float>((uintptr_t)msg + 0xc0, 0.f);
    uintptr_t al = ((uintptr_t)msg + 0x1f) & ~(uintptr_t)0xf;
    al += readOr<uint32_t>(rva(Glob_AlignAdd));
    int type = readOr<int>(al + 0x28, -1);
    if (victim && victim == g_echoPlayer) {
        if (InterlockedIncrement(&g_echoHits) <= 40)
            logf("gardes : COUP SUR L'ÉCHO : dégâts %.1f, type %d (frappé par le joueur n°%d)", dmg, type, (int)ai);
    } else if (InterlockedIncrement(&g_otherHits) <= 10) {
        logf("gardes : coup sur un joueur (n°%d%s) : dégâts %.1f, type %d", (int)vi, victim ? "" : ", inconnu", dmg, type);
    }
    g_origDamage(self, msg);
}

static void installDamageHook() {
    static bool done = false;
    if (done) return;
    uintptr_t slot = rva(Slot_PlayerDamage);
    if (readOr<uintptr_t>(slot) != rva(Fn_PlayerDamage)) { logf("gardes : case des coups sur les joueurs inattendue"); done = true; return; }
    g_origDamage = (PlayerDamageFn)rva(Fn_PlayerDamage);
    if (patchPointer(slot, (uintptr_t)&hookDamage, NULL)) logf("gardes : coups des gardes sur les joueurs suivis");
    done = true;
}

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

// qui écoute les messages « AI » du serveur ? (arbre des catégories du gestionnaire : 0x142a39770)
static void dumpListeners(uint32_t category, const char* what) {
    uintptr_t ctx = readOr<uintptr_t>(rva(kServerGameContext));
    uintptr_t mgr = ctx ? readOr<uintptr_t>(ctx + 0x10) : 0;
    if (!mgr) return;
    uintptr_t head = mgr + 0x88, node = readOr<uintptr_t>(mgr + 0x98), best = head;
    for (int g = 0; node && g < 64; g++) {
        if (readOr<int>(node + 0x20) < (int)category) node = readOr<uintptr_t>(node);
        else { best = node; node = readOr<uintptr_t>(node + 8); }
    }
    if (best == head || (int)category < readOr<int>(best + 0x20)) { logf("gardes : aucun écouteur %s", what); return; }
    typedef uintptr_t (*NextFn)(uintptr_t);
    NextFn next = (NextFn)rva(0x142a34820);
    int n = 0;
    for (uintptr_t e = readOr<uintptr_t>(best + 0x38); e && e != best + 0x30 && n < 40; e = next(e), n++) {
        uintptr_t obj = readOr<uintptr_t>(e + 0x20);
        uintptr_t vt = obj ? readOr<uintptr_t>(obj) : 0;
        uintptr_t fn = vt ? readOr<uintptr_t>(vt) : 0;
        uintptr_t b = moduleBase() - 0x140000000ull;
        logf("gardes : écouteur %s n°%d : objet %llx vt=%llx fonction=%llx filtre=%x", what, n, (unsigned long long)obj,
             (unsigned long long)(vt ? vt - b : 0), (unsigned long long)(fn ? fn - b : 0), readOr<uint32_t>(e + 0x34));
    }
}

// diagnostic : quels composants (santé, physique...) ont l'écho et Faith ? (les coups des gardes ne comptent pas
// sur l'écho : sans composant de santé, rien ne les reçoit)
static void compareComponents(void* ghost) {
    static const char* names[] = {"PamServerHumanHealthComponent", "PamServerHealthComponent", "ServerHealthComponent",
                                  "PamServerAIHealthComponent", "ServerCharacterPhysicsComponent",
                                  "PamServerCharacterPhysicsComponent", "PamServerHitReactionComponent",
                                  "PamServerMeleeTargetComponent", "PamServerCombatComponent", "ServerBoneCollisionComponent",
                                  "PamServerSoldierBodyComponent", "PamServerDamageComponent"};
    void* me = level::playerCharacter();
    typedef void* (*Fn)(void*, const void*);
    uintptr_t b = moduleBase() - 0x140000000ull;
    for (const char* n : names) {
        uintptr_t ti = findTypeByName(n);
        if (!ti) { logf("composants : %s inconnu", n); continue; }
        Fn fg = (Fn)gameVirtual(ghost, Vtbl_GetComponent), fm = me ? (Fn)gameVirtual(me, Vtbl_GetComponent) : NULL;
        void* cg = fg ? fg(ghost, (const void*)ti) : NULL;
        void* cm = fm ? fm(me, (const void*)ti) : NULL;
        logf("composants : %s : écho %p (vt %llx), Faith %p (vt %llx)", n, cg,
             (unsigned long long)(cg ? readOr<uintptr_t>((uintptr_t)cg) - b : 0), cm,
             (unsigned long long)(cm ? readOr<uintptr_t>((uintptr_t)cm) - b : 0));
    }
}

static void attach(void* ch) {
    compareComponents(ch);
    uintptr_t ctx = readOr<uintptr_t>(rva(kServerGameContext));
    uintptr_t mgr = ctx ? readOr<uintptr_t>(ctx + 0x68) : 0;
    if (!mgr) { logf("gardes : ESSAI, pas de gestionnaire des joueurs"); return; }
    dumpListeners(0x0059722du, "AI");
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
    // même équipe que moi : les gardes n'attaquent que l'équipe adverse (essai 2 : équipe 0, ignoré par les gardes)
    void* me = level::playerCharacter();
    uintptr_t myPlayer = me ? readOr<uintptr_t>((uintptr_t)me + Character_Player) : 0;
    int myTeam = myPlayer ? readOr<int>(myPlayer + Player_Team) : 0;
    int before = readOr<int>(player + Player_Team);
    if (myTeam) ((void (*)(uintptr_t, int))rva(Fn_PlayerSetTeam))(player, myTeam);
    logf("gardes : équipe du fantôme %d -> %d (la mienne : %d)", before, readOr<int>(player + Player_Team), myTeam);
    enableAsTarget(player, true);
    g_player = player;
    g_echoPlayer = player;
    installDamageHook();
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

void levelChanged() { g_doneFor = NULL; g_seenAt = 0; g_player = 0; g_echoPlayer = 0; }

}  // namespace aitarget
}  // namespace game
