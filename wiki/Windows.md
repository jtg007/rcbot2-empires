# Windows

Sources: [docs/WINDOWS.md](../docs/WINDOWS.md) (status 2026-11-15), [README.md](../README.md) (platform note), [AGENTS.md](../AGENTS.md) (MSVC rules), `utils/RCBot2_meta/bot_empires_binary.cpp`,
work item 6.9. The plugin builds with MSVC (x86) in CI, but it is **not tested in game on Windows**. All reverse engineering was done on the Linux `server_srv.so`.

## First test on Windows

1. Install Metamod:Source; unzip `rcbot2-empires-windows-x86.zip` (from Releases, `latest` tracks `main`) into the server's `empires\` folder.
2. Every map needs `empires\maps\<map>.nav` (`sv_cheats 1; p_nav_generate`, then `sv_cheats 0`).
3. Console: `meta list` (RCBot2 listed), `rcbot_empires_debug_log 1`, `rcbotd addbot 1 2 TestBot`.
4. Expected log line: `[RCBOT2] empires binary size=0 crc=00000000 trusted=0`. **Never set `rcbot_empires_force_offsets 1` on Windows** (garbage reads, crashes).
5. Report: that log line, warnings/errors, map, bot count, what worked, last 50 console lines and dump on a crash. A plugin built for Metamod 2.0 headers is rejected by Metamod 1.12 (build against its `1.12-dev` branch).

## Works without fixed offsets

Navigation on the navmesh, combat, classes, captures, commander seat and CV driving, orders through the game's console commands, buildings through `emp_build`, everything read through
SendProps, datamaps or key values.

## Off or reduced on Windows (fixed Linux offsets, guarded by `EmpiresBinaryOffsetsTrusted()`)

| Feature | File | Fallback |
|---|---|---|
| Revive | `bot_empires_revive.cpp` | no revives |
| Team resources read/give | `bot_empires_commander_economy.cpp` | economy without exact team pool |
| Research state | `bot_empires_commander_economy.cpp` | research not tracked |
| Resource points / refinery spots | `bot_empires_commander_economy.cpp` | fewer refinery placements |
| EngiBot purchase | `bot_empires_engibot.cpp` | purchase refused |
| Engineer skills by rank | `bot_empires.cpp` (`updateEmpiresEngineerSkills`) | default skills only |
| Vehicle armor per side, ammo | `bot_empires_vehicle.cpp` | no side/armor-aware retreat |
| Wages hand-over | `bot_empires_vehicle.cpp` | only via a chat/give plugin |
| Spawn point enabled | `bot_empires.cpp` | every spawn treated as enabled |
| Native build placement, placement diagnosis | `bot_empires_native_placement.cpp`, `bot_commands.cpp` | compiled out (`#if __linux__`); RCBot's own placement checks |

## Porting recipe

1. **Prefer names over offsets**: `CClassInterfaceValue("CEmpPlayer", "m_flWages", 0)` (`bot_getprop.h`), `UTIL_FindSendPropInfo`, `sm_dump_netprops` / `sm_dump_datamaps`
   to find fields; verify relative-offset tricks on MSVC layouts.
2. **Otherwise find the offset in `server.dll`**: open the Linux function named in the code comment, note strings/commands/constants, find the same in `server.dll`, read the field offset
   (MSVC `this` in `ecx`, `__thiscall`), confirm at runtime against the HUD.
3. Make offsets per platform (`#ifdef _WIN32` tables), add `server.dll` size/CRC to `kKnownBinaries`, mark a feature trusted only when **all** its offsets are ported.
4. Native calls by address need `__thiscall` function types and `GetModuleHandleA("server.dll")` instead of `dladdr`.
5. Port order: spawn enabled, rank/skills, team resources (unlocks economy and EngiBots), revive, vehicles, native placement last.
6. After a game update size/CRC change and guarded features switch off by themselves; recheck offsets, then add the new pair.

## Build and code rules

Local MSVC build: Visual Studio 2022 C++ tools, Python 3, AMBuild, submodules, `configure.py -s sdk2013 --targets x86 ...`, output in `post\package\addons\`. Code rules: `(std::min)(a, b)`,
`(std::numeric_limits<T>::max)()`, plain `snprintf(`; Linux APIs (`dlfcn.h`, ELF, `/proc`) under `#ifdef __linux__`; unique class names across files (LNK2005); `static constexpr` in lambdas.
The line numbers quoted in `docs/WINDOWS.md` (`~6480`, `~13590`) have drifted (now about 6550 and 13790).
