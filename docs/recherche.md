# Notes de recherche (code du jeu, 1.0.3.47248)

Lu avec Ghidra 12.1.4 (sans interface) sur l'image mémoire du jeu remise en exe (`_mod_coop/mec_dump_pe.exe` : l'exe du
dossier du jeu est chiffré sur le disque), avec les types de GridNode (`gridnode-reflected.json`) et ses enregistrements.
Adresses en VA (base 0x140000000).

## Zones « touche F » (perturbateur, départs de mission…)

- `PamServerSpatialEntryInputTriggerEntity` (vtable 0x141c78c10, 1088 octets) : zone placée dans la ville.
  `+0xd0` déclencheur, `+0x1b0` maillon de la liste de toutes les zones ; tête de liste :
  `*(TI 0x142863740 + (*( *(0x1425469e0) + 0x28) + 6) * 16)`.
- Données : `PamSpatialEntryInputTriggerEntityData` (Enabled, DisableOnTrigger/Release/HoldDown, Triggers,
  LookAtRadius…), chaque `PamEntryInputTriggerInfo` (EntryInputAction, SetTriggerDescriptionOn/OffHash…).
- Le composant d'interaction du joueur (`PamServerEntityInteractionComponent`, vtable 0x141c78e40) choisit la zone
  (0x143a6fc50), puis `0x143a6f4a0(déclencheur, ServerPlayer)` lit les touches de chaque entrée : appui
  (+0x10) → case 0x68 puis signal `[données+0x24]` ; maintien (+0x25) → case 0x70 puis `[+0x28]` ; relâché (+0x12)
  → case 0x78 puis `[+0x2c]`. Le signal : `0x143395700(évt, joueur, n°)` (40 octets, vtable 0x141b51ce8,
  référence au joueur en +0x20), envoyé par la case **0x88** du déclencheur, puis libéré (référence −1,
  `0x142a3a9e0` à zéro ; `0x142d4e9a0`).
- Position monde d'une entité spatiale : `0x143a68a80(entité, LinearTransform*)` (tient compte du parent +0x420).
- Ventilateur + perturbateur : signal `1f51d15d` → `SchematicChannelEntity` → animation « arrêt » (ANT bool
  f218e074), effet, son et invite coupés ; puis `26c98945`.
- Les départs de mission semblent passer par ces zones : ne rejouer un signal reçu que si on est près (overlay : 40 m).

## Gardes : qui peut être pris pour cible

- `AIPlayerEnableAsTargetMessage` (catégorie « AI » 0x0059722d, type 0x63c0f317, vtable 0x141b5d758, 64 octets ;
  constructeur 0x144674e80) : `+0x30` = **personnage +0x250** (son joueur, nul pour l'IA et les fantômes), `+0x38`
  = oui/non.
- Envoyé par `0x143382600(objet, oui/non)` pour chaque personnage de la liste `objet+0x148..+0x150` qui a un joueur,
  en direct (`0x142a39770(gestionnaire serveur, msg)`, message sur la pile). Appelé depuis 0x143387b44 et 0x14338d933.
- Donc les gardes ciblent des **joueurs** (`ServerPlayer`), pas des personnages : un fantôme n'a pas de joueur.
  Piste suivie : lui en créer un (voir « Joueurs supplémentaires »).
- `PamServerCharacterProxyEntity` (doublure de personnage pour la logique des niveaux : données Template,
  UseLocalPlayerCharacter) : sur l'événement 0x1d8fac4e, appelle `0x143382600` (active/désactive le ciblage).
- Gestionnaires d'IA connus : `ServerAIEnemyManager` (0x141a5e720), `ServerAISensingManager` (0x141a5e268),
  `PamAISensingManager` (0x141c568a8 ; case 4 = 0x143992450 : événement 0x467b0e51 → ajoute un stimulus à sa
  liste +0x70, position + type 3). Factions : `FactionId` (Neutral, US, RUS, MEC).

## Joueurs supplémentaires (comme les « bots » de Battlefield)

- Gestionnaire des joueurs du serveur : `[*(0x14253d670) + 0x68]` ; `+0x10` places, `+0x910` places réservées,
  `+0x268..+0x270` joueurs, `+0x4a0..+0x4a8` autre liste (joueur +0x1399), `+0x920` table n° → joueur.
- Création : `0x14334beb0(gestionnaire, 0, 0xff, n° ou -1, 0)` (alloue + `0x143347fe0`), puis `0x143349ce0(gestionnaire,
  joueur, nom)`. Pour une connexion réseau : `0x1433cf3a0` (ServerConnection : +0x2ba0 joueurs, +0x446c raison de refus),
  qui envoie ensuite ServerPlayerCreatedForConnection et ServerPlayerCreate (catégorie 0xf3c93dd3, type 0x3051e83c).
- Un spawner de personnages (`0x143387960`) sait créer un joueur pour son personnage selon le mode lu dans ses données
  (`0x143386a40`, propriété 0x84b9de3f : 0 aucun, 2/3 pas de joueur, 1 et 300 = bot : AISpawnBotMessage ensuite).
  Il fait : personnage case 0x2d0 (`0x143377c00` : personnage +0x250 = joueur, +0x409 = joueur +0x1398) puis
  `0x14334a4e0(joueur, personnage)`, puis `0x143382600` (gardes : peut être pris pour cible).
- Essai 2 (overlay, Test solo) : même suite pour le personnage du fantôme.

## Missions, points de passage, réapparition

- Entités : `PamServerMissionManagerEntity`, `PamServerMissionEntity` (données : Mission, FlagsSetWhenReplaying,
  ReloadFromLastCheckpointOnFail, EnableCityAlertDuringMission, StreamIn/Out…), `PamServerMissionStartPointEntity`,
  `PamServerMissionCheckpointEntity`, `PamServerCheckpointEntity`, `PamServerMissionStatusEntity`.
- Messages : `ServerGameplayCheckpointActivatedMessage`, `ServerGameplayCheckpointTriggeredMessage`,
  `ServerPlayerRespawnMessage`, `ServerMissionObjectiveCompletedMessage`, `PamProgressionActiveMissionMessage`,
  `PamProgressionMissionUpdatedMessage`.
- Liens drapeaux ↔ missions dans les données : `PamProgressionMission` (CompletedFlag, AvailableFlag,
  MissionDescription…), `PamMissionDescription.FlagNameHash` (à lire en jeu : ce sont des données du niveau).

## Progression (drapeaux)

- `setFlag` 0x1439e3d90 : 27 appels, surtout des entités de la logique des niveaux (0x1439d6680, 0x1439d68f0,
  0x1439d9510, 0x1439de2c0 : met plusieurs drapeaux d'un coup) et 0x143a7xxxx. Sauvegarde : 0x1439e0e40, appelée
  seulement par la mise à jour 0x1439d8680. Le chargement depuis le fichier n'est pas encore trouvé (piste pour
  retrouver sa progression en un seul rechargement).

## Outils

- `ghidra/dec.sh sortie.txt 0xADRESSE…` (dans l'espace de travail de Claude) : décompile en ~5 s par appel.
- REA (Claude Desktop) : chaque appel est limité à 60 s et une analyse annulée est perdue ; inutilisable pour
  analyser l'exe entier, utile pour de petits fichiers.

## MECMP 0.9.0-beta.3 (licence MIT) : pistes à reprendre

Lu dans `multi/mecmp-0.9.0-beta.3/src/game/engine.h` et le CHANGELOG (à créditer si on reprend du code) :
- `SpawnInfo +0x40 = player` : le spawn d'un personnage peut recevoir directement un ServerPlayer.
- Retirer un fantôme : spawner case 0x108 `unspawn(spawner, personnage, détruire, 0)` (0x14338e4c0) ; libère aussi
  le joueur créé pour le personnage.
- Marqueurs des joueurs sur la carte : PresenceInfoManager (`assignSlot` 0x143a405e0, vtable 0x141c74be8,
  emplacements 0x50 octets : affiché, priorité, OnlineId, position).
- Pause des menus : PamUISystem (case 0x141cb64f0 → 0x143b16c50), contrôleur de pause UISystem +0x8c8
  (demandes en +0x08 / +0x0c).
- Courses (contre-la-montre, dashs) : créateurs de PamServerNamedChallengeEntity (0x141c7ee00) et
  PamServerChallengeMarkerEntity (0x141c7e9f0).
- Corps entier : 85 des 182 os envoyés (doigts, poignets…) au lieu de 23, en réglant l'enregistreur du joueur
  (PamClientReplayRecordingComponent, créateur 0x141c491c8) avant le chargement du niveau.
- Écran « Ton écho » et carte : garder le personnage du jeu en dernier dans les listes du spawner, côté client aussi ;
  relancer la pose des fantômes après la carte.

## Marqueurs sur la carte (essai abandonné le 25/10)

- Repris de MECMP (PresenceInfoManager, voir plus haut) : rien ne s'affiche, même avec un faux profil au pseudo. Les
  serveurs EA du jeu sont hors ligne et la carte semble exiger un vrai profil en ligne. Coupé dans coop (réglage
  `map_players`, faux par défaut). Piste si on y revient : dessiner nos propres marqueurs par-dessus la carte (overlay).
