@AGENTS.md

# Claude Code only

Two project agents in `.claude/agents/`:

- **`tester`** (small model): every test run. The task names the exact command and
  which lines/numbers to report. Never run long matches in the main session.
- **`expert`** (strongest model): root-cause work when a cause is unclear after 1–2
  attempts, a fix shows no effect, or the game binary, navmesh scans or engine
  behaviour must be interpreted. It does not know the conversation; the task must
  include symptom, numbers, run folder/log lines, previous attempts and the question.
