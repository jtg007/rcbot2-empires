# RCBot2 Empires Wiki

Navigable overview of the RCBot2-Empires bot project. It condenses the [README](../README.md), the
contributor notes ([AGENTS.md](../AGENTS.md), [docs/OPEN_WORK.md](../docs/OPEN_WORK.md)) and the code into short pages.

State: 2026-11-23, including the infantry-combat module (`bot_empires_skirmish.cpp`). Each page names the sources it
summarizes so it can be refreshed later. When the wiki and the code disagree, the code wins.

## What this project is

Autonomous bots for the Source mod **Empires** (FPS plus RTS base building), built as an improvement of the
old RCBot2 plugin (Metamod:Source, Source SDK 2013, 32-bit Linux; Windows builds in CI but untested in game).
The bots play full matches: a bot commander takes the command vehicle (CV), builds the base and vehicle factory,
researches, assigns engineers and orders infantry; engineers build, repair, revive, heal and throw seismic
grenades; infantry captures flags and fights; drivers buy and drive vehicles. Navigation uses generated navmeshes,
no hand-made waypoints. The goal is to make the existing bots good and playable step by step (no rewrite, no "AI v2").
Yardstick for every change: a 10-bot match at 1x on a map with flags (for example `emp_crossroads`, `emp_palmbay`).

## Pages

| Page | Read it for |
|---|---|
| [Features](Features.md) | What the bots do per role and class, with status (verified / in validation / open) and item IDs |
| [Cvars](Cvars.md) | Every `rcbot_empires_*` cvar: default, meaning, reading file; plus related cvars |
| [Console commands](Console-Commands.md) | `rcbotd` / `empires_*` diagnostic and test commands |
| [Code map](Code-Map.md) | Source files, responsibilities, entry points, code-level pitfalls |
| [Build and test](Build-and-Test.md) | Build, `dev.sh` variables, scenario scripts, `dev_check`, evaluating runs, recorder |
| [Known facts and pitfalls](Known-Facts-and-Pitfalls.md) | Verified engine/game facts and evaluation traps (do not re-derive) |
| [Reverse engineering](Reverse-Engineering.md) | Index of the notes on the game binary `server_srv.so` |
| [Maps](Maps.md) | Map coverage, navmesh status, per-map issues, offline navmesh tools |
| [Roadmap](Roadmap.md) | Open work with stable item IDs, next steps, infantry plan |
| [Windows](Windows.md) | What works on Windows and how to port the Linux-offset features |
| [Glossary](Glossary.md) | Abbreviations (NF, BE, CV, VF, HU, Lawliet ...) |

## How to navigate

- **New to the project:** this page, [Features](Features.md), then [Build and test](Build-and-Test.md).
- **Changing bot behaviour:** [Code map](Code-Map.md) to find the file, [Cvars](Cvars.md) for switches,
  [Known facts](Known-Facts-and-Pitfalls.md) before touching movement, vehicles, shells or weapons.
- **Debugging a run:** [Build and test](Build-and-Test.md) (summaries, recorder), [Console commands](Console-Commands.md),
  [Maps](Maps.md) (nav overview image first for spatial bugs).
- **Choosing work:** [Roadmap](Roadmap.md); IDs such as `3.25` or `5.31.2` are stable and appear in commits and logs.
- **Game internals:** [Reverse engineering](Reverse-Engineering.md).

## Source documents behind the wiki

| Document | Role |
|---|---|
| [README.md](../README.md) | feature list, settings, known issues |
| [AGENTS.md](../AGENTS.md) (+ [CLAUDE.md](../CLAUDE.md)) | contributor rules, build/test commands, known facts |
| [docs/OPEN_WORK.md](../docs/OPEN_WORK.md) | open work with stable item IDs and priority order |
| [docs/WINDOWS.md](../docs/WINDOWS.md) | Windows porting guide |
| [EMPIRES_ANALYSIS_RECORDER.md](../EMPIRES_ANALYSIS_RECORDER.md) | analysis recorder |
| [reverse-engineering/](../reverse-engineering/README.md) | notes on the game binary |

## Working rules in one paragraph

One agent does the whole loop: change, build, test (in the background, output to a file, filter only hits), evaluate.
A change counts as working only with numbers from a real run; otherwise write "built, not measured". Build and test only on the
isolated test server, never kill processes by name. Time-box each topic to about one session; park it in the open-work list with a
one-sentence finding if there is no measurable gain. After two serious failed attempts ask an expert once (symptom, numbers,
run folder, both attempts, concrete question). Priority goes to what a player notices: bots capture, vehicles are bought and
arrive, no hangs. Every escalation logs `stall-escalate who=... stage=... reason=...`. After changes to bot or test scripts run
`scripts/dev_check.sh`. Keep `README.md` in sync with capability, cvar, script or known-issue changes.
