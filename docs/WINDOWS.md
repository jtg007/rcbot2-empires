# Running and porting the Empires bots on Windows

Status (2026-11-15): the plugin **builds** with MSVC (32-bit) in GitHub Actions. It is **not tested in
game on Windows yet**. All game-specific reverse engineering was done on the Linux server binary, so a
part of the bot falls back to simpler behaviour on a Windows server. This page lists what that part
is and how to port it, step by step.

## 1. Install and first test

1. Install Metamod:Source on the Windows Empires dedicated server.
2. Download `rcbot2-empires-windows-x86.zip` from the [Releases](../../../releases) page (`latest` is
   the newest build of `main`) and unzip it into the server's `empires\` folder, so that `addons\`
   merges with the existing one. It contains `addons\metamod\rcbot2.vdf` and `addons\rcbot2\...`.
3. Every map needs a navmesh (`empires\maps\<map>.nav`). If one is missing: load the map,
   `sv_cheats 1; p_nav_generate` (up to a minute), then `sv_cheats 0`.
4. Start the server, then in its console:

   ```
   meta list                       // RCBot2 must be listed
   rcbot_empires_debug_log 1       // detailed diagnostics
   rcbotd addbot 1 2 TestBot       // class 1 Rifleman, team 2 NF
   ```

5. Look for `[RCBOT2] empires binary size=0 crc=00000000 trusted=0` in the log. That line is expected
   on Windows (see section 2). Then watch whether bots spawn, walk, shoot and capture.

**Do not set `rcbot_empires_force_offsets 1` on Windows.** It forces the Linux memory offsets onto the
Windows game objects, which can read garbage or crash the server.

If the plugin does not load at all, `meta list` shows the error. Typical causes: Metamod version
mismatch (see README, Build) or a missing MSVC runtime (install the current *Visual C++
Redistributable x86*).

## 2. Why some features are off on Windows

Several features read or write fields of game objects that no SendProp or datamap describes, at
fixed byte offsets found in the Linux `server_srv.so`. A wrong offset would silently corrupt the game,
so `bot_empires_binary.cpp` checks the binary first: `EmpiresBinaryOffsetsTrusted()` reads
`empires/bin/server_srv.so`, compares size and CRC with `kKnownBinaries`, and only then allows those
reads. On Windows that file does not exist, so the check says "untrusted" and every guarded place uses
its fallback. The Windows `server.dll` is built with MSVC; class layouts and function addresses differ,
so the Linux numbers cannot simply be reused.

### Works on Windows (no fixed offsets)

Navigation on the navmesh, combat, classes, captures, commander seat and CV driving, orders through
the game's own console commands, buildings through `emp_build`, everything read through SendProps,
datamaps or entity key values.

### Off or reduced on Windows

| Feature | File | Fixed data used (Linux) | Fallback on Windows |
|---|---|---|---|
| Revive | `bot_empires_revive.cpp` | player +0x1e34 ragdoll handle, +0x1e3c/+0x1e45 flags, +0x121f revive skill, entity +0x294 abs origin | no revives |
| Team resources (read/give) | `bot_empires_commander_economy.cpp` | CEmpTeam +0x3a8 team, +0x3b4 ledger -> +0x30 float | economy decisions without the exact team pool |
| Research state | `bot_empires_commander_economy.cpp` | CEmpTeam +0x3d0/+0x3d4/+0x3d8/+0x3d9, +0x3e4+i | research not tracked |
| Resource points / refinery spots | `bot_empires_commander_economy.cpp` | entity +0x118 flags, +0x4a8 built, +0x4b0 resource index | fewer refinery placements |
| EngiBot purchase | `bot_empires_engibot.cpp` | team ledger as above | purchase refused |
| Engineer skills by rank | `bot_empires.cpp` (~6480) | player +0x12c0 rank points, +0x11f0.. active, +0x1200.. wanted, +0x121d..+0x121f | default skills only |
| Vehicle armor per side, ammo | `bot_empires_vehicle.cpp` | tank +0xfe4 armor float[4], +0x1004, +0x1024, +0x90c/+0x92c ammo | no side/armor-aware retreat |
| Wages hand-over | `bot_empires_vehicle.cpp` | player +0x12c8 wages | only through a chat/give plugin |
| Spawn point enabled | `bot_empires.cpp` (~13590) | CEmpSpawn +0x354 | every spawn treated as enabled |
| Native build placement | `bot_empires_native_placement.cpp` | calls `CanBuildAt`, `GetAllObstructions`, vtables, by address; needs `dladdr` and ELF headers | compiled out (`#if __linux__`), RCBot's own placement checks are used |
| Placement diagnosis `empires_commander_build obstruct` | `bot_commands.cpp` | calls a game function by address | compiled out |

Each offset has a comment with the Linux function address it was read from (for example
`CEmpTank::SetArmorSide 0x68e8f0`). More background is in `reverse-engineering/notes/`.

## 3. How to port a feature

Work feature by feature, smallest first. Recommended order: spawn enabled, rank/skills, team
resources (unlocks economy and EngiBots), revive, vehicles, native placement last.

### Option A, preferred: replace the fixed offset by a name

Many fields have a name in a SendProp table or datamap even if they were read raw so far. A name
lookup works on Linux and Windows alike and survives game updates.

- SendProps: `CClassInterfaceValue("CEmpPlayer", "m_flWages", 0)` (see `bot_getprop.h`), or
  `UTIL_FindSendPropInfo`. List all props with `sm_dump_netprops` (SourceMod) or the game's
  `dt_dump`-style tools.
- Datamaps: `sm_dump_datamaps` lists server-only fields (`m_vecAbsOrigin`, `m_iHealth`, ...).
- Relative offsets: some code reads a neighbour of a known prop (`type[4]` next to `m_iCommandType`).
  That only holds if both compilers lay the members out the same way; verify it on Windows.

If a field is found by name, remove it from the guarded path. Then it works everywhere.

### Option B: find the offset in `server.dll`

The Windows binary has no symbols, so work from the Linux one (which has them) to the Windows one:

1. Open the Linux function named in the code comment (`nm -C --defined-only server_srv.so`,
   `objdump -d -C --start-address=0x...`) and note what makes it recognisable: string references,
   called functions, constants, console command names.
2. In `server.dll` (Ghidra, IDA or x64dbg), search the same string or command name (for example
   `emp_revive`, `emp_give_resources`, `engSelect`) and follow the cross reference to the same
   function.
3. Read the field offset there (`mov eax, [ecx+0x...]`). On MSVC the object pointer (`this`) is in
   `ecx` (`__thiscall`); on Linux it is the first stack argument.
4. Confirm it at runtime: read the field from the plugin with debug logging and compare with what the
   game shows (resources in the HUD, wages, armour).

### Make the offsets per platform

Today the offsets are plain numbers in each file. For Windows:

1. Add a Windows entry to the binary check: in `EmpiresBinaryOffsetsTrusted()` read
   `bin/server.dll` under `#ifdef _WIN32`, print its size and CRC, and add that pair to
   `kKnownBinaries` (or a separate Windows list).
2. Put each ported offset in a small table, for example

   ```cpp
   #ifdef _WIN32
   constexpr unsigned kPlayerWages = 0x....;   // server.dll, found via ...
   #else
   constexpr unsigned kPlayerWages = 0x12c8;   // CEmpPlayer::m_flWages
   #endif
   ```

   and replace the literal in the code. Only mark a feature trusted on Windows once **all** offsets
   it uses are ported; otherwise keep its guard returning false on Windows.
3. Calling game functions by address (native placement): Linux member functions take `this` on the
   stack, MSVC uses `__thiscall`. The function pointer types must change (e.g. declare them
   `__thiscall`, or call through a `__fastcall` wrapper with a dummy `edx`), and the module base comes
   from `GetModuleHandleA("server.dll")` instead of `dladdr`.

### After a game update

Size and CRC change, the check reports `trusted=0`, and the guarded features turn off by themselves.
Re-check the offsets, then add the new size/CRC pair.

## 4. Building on Windows yourself

GitHub Actions (`.github/workflows/build.yml`) builds every push. Locally:

```bat
:: Visual Studio 2022 with C++ tools, Python 3, Git
pip install git+https://github.com/alliedmodders/ambuild
git clone https://github.com/jtg007/rcbot2-empires && cd rcbot2-empires
git submodule update --init --depth 1 hl2sdk-manifests alliedmodders/hl2sdk-sdk2013 alliedmodders/metamod-source alliedmodders/sourcemod
git -C alliedmodders/sourcemod submodule update --init --depth 1 sourcepawn public/amtl
:: in an "x86 Native Tools Command Prompt for VS 2022":
mkdir post && cd post
python ..\configure.py -s sdk2013 --targets x86 --enable-optimize --hl2sdk-root ..\alliedmodders --mms-path ..\alliedmodders\metamod-source --sm-path ..\alliedmodders\sourcemod
ambuild
```

The result is in `post\package\addons\`.

**Code rules that keep the Windows build green:** the SDK defines `min`, `max` and `snprintf` as
macros on Windows. Write `(std::min)(a, b)`, `(std::numeric_limits<T>::max)()` and plain `snprintf(`.
Keep Linux-only APIs (`dlfcn.h`, ELF, `/proc`) under `#ifdef __linux__`. Do not reuse a class name
that already exists in another file (MSVC rejects it with LNK2005; GCC silently keeps one vtable).
Lambdas: MSVC does not see a plain local `constexpr` inside a lambda without capture; make it
`static constexpr`.

## 5. What to report from a Windows test

- The `[RCBOT2] empires binary ...` line and any `WARNING`/`error` lines from the plugin.
- Map, number of bots, and what worked (spawn, walking, shooting, captures, commander, vehicles).
- For a crash: the last 50 lines of the console and the crash dump, if one was written.
