# Progression & missions — research notes / notes de recherche

Game version 1.0.3.47248 (image base 0x140000000). Found from GridNode recordings of two missions
and the game code. / Trouvé à partir d'enregistrements GridNode de deux missions et du code du jeu.

## Story progression = flags

The story is a set of named integer flags (`PamProgressionFlag`, field `NameHash` at +0x10,
`MissionIndex` +0x14, `MaxValue` +0x20, `Clamp` +0x38). Each mission (`PamProgressionMission`)
has an `AvailableFlag` (+0x50) and a `CompletedFlag` (+0x48).

| What | Address | Notes |
|---|---|---|
| Progression manager | `*(0x14257c9d8)` | hash table of flag records: buckets at +0x20, bucket count at +0x28 |
| Flag record | — | +0x00 hash, +0x10 `PamProgressionFlag*`, +0x18 current value, +0x28 next |
| `findFlag(mgr, uint32 hash)` | `0x1439da170` | returns `PamProgressionFlag*` or null |
| `getFlagValue(mgr, flag)` | `0x1439da880` | returns the record's value |
| `setFlag(mgr, flag, int value, bool add, bool notify, bool checkMission)` | `0x1439e3d90` | updates the record (clamped), handles children/missions, then posts `PamProgressionFlagUpdatedMessage` on the server message manager |

`PamProgressionFlagUpdatedMessage` (category PamProgression): +0x48 flag hash (uint32), +0x4c new value (int32).
It is posted twice per change: once by `setFlag` (server), once forwarded with a `ClientConnection` at +0x38.

Game callers use `setFlag(mgr, flag, value, add, notify=0, checkMission=1)`.

## A mission, as recorded / Une mission, telle qu'enregistrée

| Moment | Messages |
|---|---|
| Start | ~20 flag updates (e.g. `2563f4f5` = mission number: 46, 47…; `ed6f924f`, `440bc24b` → 0), `PamTelemetryMissionStartedMessage`, `PamProgressionMissionUpdatedMessage` |
| Checkpoint | flags `a80478f0`, `e705ff9b`, autosave (`CoreWriteSaveGameDoneMessage`), `PamTelemetryStoryCheckpointMessage` |
| During | `PamTelemetryAIKilledMessage`, projectile fire/impact, `ServerStaticModelDestroyedPartMessage`, city alert messages, `UISubtitleMessage`, `UINetworkPlayVideoMessage` / `VideoDone` |
| End | `ServerRoundOverMessage`, ~70 flag updates (`ed6f924f`, `440bc24b` → 1, `2563f4f5` → 0), `PamProgressionMissionUpdatedMessage` ×N, `PamTelemetryMissionEndedMessage`, autosave |

Flag meanings above are guesses from the recordings, not confirmed.

## In the mod (2.1.0) / Dans le mod (2.1.0)

Implemented in `overlay/src/progression.cpp` and `coop.py` (packets `"pg"`). Since 2.2.0, a joining player also asks for
the host's whole flag table (`"pgreq"`): the host walks its table (buckets at manager+0x20, count at +0x28, records
chained by +0x28) and sends every (hash, value) in batches, so the world matches even when the save file can't be loaded.
When a save is loaded, the game restores flags directly from the file (only a few `FlagUpdated` messages appear), which is
why the table has to be read rather than listened to.

Applying flags updates progression in memory, but mission start points in the city are only built when the level loads.
`setFlag` itself schedules a save 1.5 s after a change when no mission is running (`mgr+0x2b4` = time + 1.5, checked by
the update at `0x1439d8680`, which calls the save at `0x1439e0e40`). So since 2.4.0, once the host's table is applied,
coop waits for that save and then reloads the game (`ResumeSaveGame`) so the city is rebuilt with the host's progression. Only the host's flags are relayed, and a
joining player applies them only while their own save is backed up (marker present). Still experimental.

## Plan for co-op / Plan pour le coop

- The host listens to `PamProgressionFlagUpdatedMessage` and sends (hash, value) to the others.
- The others apply it on the server thread: `setFlag(mgr, findFlag(mgr, hash), value, false, false, false)`, ignoring the
  updates they produce themselves while applying (no echo).
- Initial sync: walk the host's flag table and send every (hash, value).
- This could replace swapping save files. To test carefully: setting flags alone may not start a mission's scripted
  content on the other side; that part may still need the mission invite.

## Important: reloading does not reload flags / Le rechargement ne relit pas les drapeaux

Reloading the save in game (`ResumeSaveGame`) rebuilds the level from the file, but the progression flags stay as they are
in memory (the progression manager is not reset). So putting a save file back and reloading does NOT give your progression
back (it also explains being stuck in another player's story earlier). Since 2.5.0, when a player joins, coop first reads
their own flag table from the game and stores it next to their save backup (`drapeaux.json`); when they leave, those flags
are applied back in memory before the file is restored and the game reloads. Restarting the game also reads the file.

Tested 22/10: even with the flags applied back, items unlocked by a mission (the grapple) did not come back after an
in-game reload. 2.6.0 required closing the game to get your progression back. 2.7.0 tries again in game: own flags back in memory,
then a check (flags read back from the game), the save file, a reload, and a second check, all written to the log.
