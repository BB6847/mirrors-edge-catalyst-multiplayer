# Mirror's Edge Catalyst Multiplayer

[Français](README.fr.md) · **English**

An **unofficial** co-op multiplayer mod for **Mirror's Edge Catalyst** (PC): up to 4 players in the same city.

- **Other players in 3D**: shown as Faith (or as a red ghost), fully animated, at the right spot, with a display delay that adapts to the network.
- **Their sounds**: footsteps, jumps, hits, breathing and voice lines, played where they are.
- **Shared missions**: when a player starts a mission, the others get an in-game invite (`Alt+M` takes you to the start so you can launch it yourself, cutscenes included).
- **Same world**: whoever joins receives the host's progression, loaded by the game without restarting it. Their own progression is set aside and comes back automatically afterwards.
- **Time of day synced** to the host's, plus a **radar** drawn over the game.
- **Shared story progression** *(experimental)*: when the host progresses (missions, checkpoints), the same story flags are applied for the others, live. See [docs/progression.md](docs/progression.md).
- **Shared combat** *(experimental)*: hits landed on an enemy by one player are applied to the same enemy for the others.

> Fan project, not affiliated with EA or DICE. No game files are included: you need to own the game (Steam / EA app).
> Supported game version: **1.0.3.47248**. On any other version the mod disables itself.
>
> The app's interface is in French. The buttons you need are **Héberger** (Host), **Rejoindre** (Join) and **Test solo** (Solo test).

## How to use

1. Put all players on the same network, for example with [Radmin VPN](https://www.radmin-vpn.com/). Port used: **UDP 27777**.
2. The host starts the game and roams the city, then clicks **Héberger** (Host) in coop and shares their IP.
3. The others paste that IP and click **Rejoindre** (Join). Coop fetches the host's progression, installs it (theirs is set aside in `sauvegardes/`) and has the game load it.
4. **Test solo** lets you try everything alone: a clone copies your moves next to you.

In-game hotkeys:

| Key | Action |
|---|---|
| `Alt+M` | go to the mission another player started |
| `Alt+1` / `Alt+2` | radar zoom |
| `Alt+3` | radar size |
| `Alt+4` | radar screen corner |
| `Alt+5` | hide / show the radar |

### Saves

Coop never deletes a save:

- **Before installing someone else's world**, your progression is copied to `sauvegardes/`.
- **A marker file** (`coop_monde_hote.json`, next to the game's saves) records that another world is installed. Your progression comes back as soon as you play alone or quit coop, even if coop crashed in between.

## Running from source

You need Windows and Python 3.10 or newer (numpy and psutil are installed on first launch).

```bat
py coop.py
```

`coop.py` looks for `mec_overlay.dll` next to itself or in `overlay/build/`.

To build an executable (`coop_vX.Y.Z.exe`, with the overlay bundled), run `build_exe.bat`. It needs PyInstaller: `py -m pip install pyinstaller`.

## Building the overlay (`mec_overlay.dll`)

```bash
cd overlay
./build.sh
```

- **On Windows**, you need [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) in your PATH and a bash shell (Git Bash or MSYS2).
- **On Linux**, you need clang and lld, plus either llvm-mingw or `SYSROOT=<mingw-w64 include/ and lib/> ./build.sh`.

## How it works

```
 game (MirrorsEdgeCatalyst.exe)                        coop.py
 ┌───────────────────────────────┐    shared memory    ┌──────────────────────────┐   UDP 27777
 │ mec_overlay.dll               │ <─────────────────> │ window, network,         │ <──────────> other players
 │  • reads my pose / sounds /   │  Local\MECCoopPose  │ saves, missions          │   (host relays)
 │    missions / hits            │                     └──────────────────────────┘
 │  • spawns and animates the    │
 │    other players (ghosts)     │
 │  • ImGui: radar, messages     │
 └───────────────────────────────┘
```

| File | Role |
|---|---|
| `coop.py` | the app: interface, networking (relay server on the host, client on every player), save sharing and restoring, overlay injection |
| `overlay/src/game_core.cpp` | engine hooks, shared memory, remote player interpolation |
| `overlay/src/ghosts.cpp`, `models.cpp` | the other players' characters (spawning, appearance, animation) |
| `overlay/src/audio.cpp` | sound capture and playback |
| `overlay/src/mission.cpp` | missions (start / end), read from the game's messages |
| `overlay/src/combat.cpp` | shared combat |
| `overlay/src/menu.cpp` | reloading the save without quitting the game |
| `overlay/src/overlay.cpp` | ImGui rendering (DirectX 11), radar, invites |
| `tests/faux_pote.py` | a fake second player, for testing alone |

Source code comments are in French.

## Credits

- [**MECMP**](https://github.com/CodeNameMeteor/mecmp) (MIT license): some of the engine methods and addresses, used for ghosts, sounds and listening to game messages, come from this project. The files concerned say so at the top.
- [**Dear ImGui**](https://github.com/ocornut/imgui) (MIT) and [**MinHook**](https://github.com/TsudaKageyu/minhook) (BSD 2-Clause), copied into `overlay/third_party/` with their licenses.

See [THIRD_PARTY.md](THIRD_PARTY.md). Project license: [MIT](LICENSE).
