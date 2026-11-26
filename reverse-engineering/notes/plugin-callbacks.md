# Work done from engine/plugin callbacks (2026-11-15)

Community crash: a human commander types `!buildengibot` on a server with SourceMod 1.12 and
several plugins (SourceBans++, UMC, DraftPick, NCEV); `segfault at 0 ip 0` in the same second.
The old code created the fake client (`CBots::createBot` -> `CreateFakeClient`) and sent the
HintText/SayText reply from the `player_say` handler.

Call context of that handler (RCBot hooks `IGameEventManager2::FireEvent` as a pre hook):

- Human speaker: net packet of that client -> `ExecuteStringCommand("say ...")` -> SourceMod
  say/chat-trigger hooks and plugin command listeners -> `Host_Say` -> `FireEvent(player_say)`
  -> RCBot pre hook. Creating a client here runs `ClientConnect`/`ClientPutInServer` and all
  plugin forwards (and nested `player_connect` events) while that chain is still on the stack.
- Bot speaker in local tests (`helpers->ClientCommand(bot, "say ...")`): no net channel and most
  plugins skip fake clients, so the local test does not reproduce the human context.

Not confirmed (no crash dump from the server). Rule used since: chat/event handlers only queue
the request; client creation and user messages run in `GameFrame`
(`EmpiresEngibotFrame`, `bot_empires_engibot.cpp`). Confirming it needs a real human client
saying `!buildengibot` on a server with SourceMod and the same plugins.

## player_hurt is not delivered (2026-11-25)

- `resource/modevents.res` of Empires declares no `player_hurt` (it has `player_death`, `emp_killfeed`,
  `point_captured`, ...). The string exists in `server_srv.so`, but a counting diagnostic in
  `CPlayerHurtEvent::execute` saw 0 events in a 4-minute 6-bot Crossroads run with hits and a kill
  (`20261125-193429`).
- Consequence: `CBot::hurt` / `CBotEmpires::hurt` never run in Empires. Everything hanging on them is
  dead code there: the hurt hide schedule (`CGotoHideSpotSched`, and the 3.25 navmesh hurt cover),
  `noteEmpiresTurretThreat` from damage, the engineer-support damage interrupt, `m_vHurtOrigin` /
  `LOOK_HURT_ORIGIN`, `m_fCurrentDanger` from damage.
- Damage has to be detected by polling the bot's own health (`m_iHealth`) per think if any of these
  are wanted.
- Done in 3.25b: `CBotEmpires::pollEmpiresHurt` (modThink) compares `GetHealth()` with the last think and calls
  `hurt()`; `m_DmgOrigin` at the world origin marks fall/world damage. Cvar `rcbot_empires_health_poll_hurt`.
