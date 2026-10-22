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

## Plan for co-op / Plan pour le coop

- The host listens to `PamProgressionFlagUpdatedMessage` and sends (hash, value) to the others.
- The others apply it on the server thread: `setFlag(mgr, findFlag(mgr, hash), value, false, false, false)`, ignoring the
  updates they produce themselves while applying (no echo).
- Initial sync: walk the host's flag table and send every (hash, value).
- This could replace swapping save files. To test carefully: setting flags alone may not start a mission's scripted
  content on the other side; that part may still need the mission invite.
