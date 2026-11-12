# Empires gameplay analysis recorder

The recorder logs what the bots do during a match, for later analysis. It is passive and opt-in: it
does not select goals, change tasks, move bots, choose waypoints, alter
cooldowns, or affect combat.

## Commands

```text
rcbot_analysis_start
rcbot_analysis_status
rcbot_analysis_mark <text>
rcbot_analysis_stop
```

The same operations are available through the dedicated RCBot command group:

```text
rcbotd analysis_start
rcbotd analysis_status
rcbotd analysis_mark <text>
rcbotd analysis_stop
```

`start` creates a new session. `mark` records a `USER_MARK` with the supplied
text. `stop` flushes the buffered JSONL files and writes the summary. A
filesystem or write error disables the recorder and logs a clear error; the
bot loop continues.

## Output

Sessions are written below the game directory:

```text
analysis/session-YYYYMMDD-HHMMSS/
  metadata.json
  events.jsonl
  snapshots.jsonl
  summary.json
```

Events are sampled/deduplicated state transitions plus explicit production
events. Each event contains session/game time, bot identity, team/class,
position, goal/task, waypoint/enemy context when available, and event data.
Snapshots are written at most every 1.5 seconds per bot; there is no per-frame
JSON and no A* expansion logging. Writes are buffered until the buffer reaches
32 KiB or the session stops.
