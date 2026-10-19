# MEC Coop

Mod coopératif **non officiel** pour **Mirror's Edge Catalyst** (PC) : jusqu'à 4 joueurs dans la même ville.

- **Les autres joueurs en 3D** : vus avec l'apparence de Faith (ou en fantôme rouge), animés, au bon endroit, avec un délai d'affichage qui s'adapte au réseau.
- **Leurs sons** : pas, sauts, coups, respiration et voix, joués à leur position.
- **Missions partagées** : quand un joueur lance une mission, les autres reçoivent une invitation dans le jeu (`Alt+M` pour aller au départ et la lancer avec ses cinématiques).
- **Même monde** : celui qui rejoint reçoit la progression de l'hôte, chargée dans le jeu sans le redémarrer. Sa propre progression est mise de côté et revient toute seule ensuite.
- **Heure du jour synchronisée** sur celle de l'hôte, **radar** par-dessus le jeu.
- **Combats partagés** *(en test)* : les coups portés à un ennemi chez l'un sont appliqués au même ennemi chez l'autre.

> Projet de fans, sans lien avec EA ou DICE. Aucun fichier du jeu n'est fourni : il faut posséder le jeu (Steam / EA app).
> Version du jeu prise en charge : **1.0.3.47248**. Sur une autre version, le mod se désactive tout seul.

## Utilisation

1. Tous les joueurs se mettent sur le même réseau, par exemple avec [Radmin VPN](https://www.radmin-vpn.com/). Port utilisé : **UDP 27777**.
2. L'hôte lance le jeu et se balade en ville, puis clique **Héberger** dans coop et donne son IP.
3. Les autres collent cette IP et cliquent **Rejoindre** : coop récupère la progression de l'hôte, l'installe (la leur est mise de côté dans `sauvegardes/`) et la fait charger au jeu.
4. **Test solo** permet de tout essayer sans ami : un clone copie tes mouvements à côté de toi.

Raccourcis dans le jeu :

| Touche | Action |
|---|---|
| `Alt+M` | aller à la mission lancée par un autre joueur |
| `Alt+1` / `Alt+2` | zoom du radar |
| `Alt+3` | taille du radar |
| `Alt+4` | coin de l'écran du radar |
| `Alt+5` | masquer / afficher le radar |

### Sauvegardes

Coop ne supprime jamais une sauvegarde :

- **Avant d'installer le monde d'un autre**, ta progression est copiée dans `sauvegardes/`.
- **Un marqueur** (`coop_monde_hote.json`, à côté des sauvegardes du jeu) indique qu'un autre monde est installé. Ta progression revient dès que tu joues seul ou quittes coop, même si coop a planté entre-temps.

## Lancer depuis les sources

Il faut Windows et Python 3.10 ou plus récent (numpy et psutil sont installés au premier lancement).

```bat
py coop.py
```

`coop.py` cherche `mec_overlay.dll` à côté de lui ou dans `overlay/build/`.

Pour faire un exécutable (`coop_vX.Y.Z.exe`, avec l'overlay dedans) : `build_exe.bat` (installe PyInstaller : `py -m pip install pyinstaller`).

## Compiler l'overlay (`mec_overlay.dll`)

```bash
cd overlay
./build.sh
```

- **Sous Windows**, il faut [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) dans le PATH et un terminal bash (Git Bash ou MSYS2).
- **Sous Linux**, il faut clang et lld, avec llvm-mingw, ou `SYSROOT=<include/ et lib/ de mingw-w64> ./build.sh`.

## Comment ça marche

```
 jeu (MirrorsEdgeCatalyst.exe)                         coop.py
 ┌───────────────────────────────┐   mémoire partagée  ┌──────────────────────────┐   UDP 27777
 │ mec_overlay.dll               │ <─────────────────> │ fenêtre, réseau,         │ <──────────> autres joueurs
 │  • lit ma pose / mes sons /   │  Local\MECCoopPose  │ sauvegardes, missions    │   (l'hôte relaie)
 │    mes missions / mes coups   │                     └──────────────────────────┘
 │  • fait apparaître et anime   │
 │    les autres (fantômes)      │
 │  • ImGui : radar, messages    │
 └───────────────────────────────┘
```

| Fichier | Rôle |
|---|---|
| `coop.py` | l'application : interface, réseau (serveur qui relaie chez l'hôte, client chez chacun), partage et remise des sauvegardes, injection de l'overlay |
| `overlay/src/game_core.cpp` | crochets dans le moteur, mémoire partagée, interpolation des joueurs distants |
| `overlay/src/ghosts.cpp`, `models.cpp` | les personnages des autres joueurs (apparition, apparence, animation) |
| `overlay/src/audio.cpp` | capture et lecture des sons |
| `overlay/src/mission.cpp` | missions (début / fin) écoutées dans les messages du jeu |
| `overlay/src/combat.cpp` | combats partagés |
| `overlay/src/menu.cpp` | rechargement de la sauvegarde sans quitter le jeu |
| `overlay/src/overlay.cpp` | rendu ImGui (DirectX 11), radar, invitations |
| `tests/faux_pote.py` | un faux deuxième joueur, pour tester seul |

## Crédits

- [**MECMP**](https://github.com/CodeNameMeteor/mecmp) (licence MIT) : une partie des méthodes et des adresses du moteur, pour les fantômes, les sons et l'écoute des messages, vient de ce projet. Les fichiers concernés le disent en tête.
- [**Dear ImGui**](https://github.com/ocornut/imgui) (MIT) et [**MinHook**](https://github.com/TsudaKageyu/minhook) (BSD 2-Clause), copiés dans `overlay/third_party/` avec leur licence.

Voir [THIRD_PARTY.md](THIRD_PARTY.md). Licence du projet : [MIT](LICENSE).
