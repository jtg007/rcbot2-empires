// This is an open source non-commercial project. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: http://www.viva64.com
#include <string>

#include "icvar.h"
//#include "iconvar.h"
#include "convar.h"

#include "bot_cvars.h"
#include "bot_const.h"

static ICvar* s_pCVar; //TODO: Unused? [APG]RoboCop[CL]

ConVar rcbot_tf2_debug_spies_cloakdisguise("rcbot_tf2_debug_spies_cloakdisguise", "1", 0, "Debug command : allow spy bots to cloak and disguise");
ConVar rcbot_tf2_medic_letgotime("rcbot_tf2_medic_letgotime", "0.5", 0, "Time for medic to let go of medigun to switch players");
ConVar rcbot_tf2_pyro_airblast("rcbot_tf2_pyro_airblast_ammo", "50", 0, "Ammo must be above this to airblast -- if 200 airblast will be disabled");
ConVar rcbot_projectile_tweak("rcbot_projtweak", "0.05", 0, "Tweaks the bots knowledge of projectiles and gravity");
// Session-local kill switch for the bounded Transition Experience penalty.
// Defaults off; explicitly setting it to 1 enables the experimental v1 costs.
ConVar eta_nav_learning("eta_nav_learning", "0", 0,
	"Enable bounded session-local learned navigation edge costs (0 = off)");
// Session-local kill switch for Conservative Active Recovery v1.  Passive
// Recovery Episode telemetry remains available while this is disabled.
ConVar eta_recovery_active("eta_recovery_active", "0", 0,
	"Enable bot-local Conservative Active Recovery v1 for Capture (0 = off)");
// Session-local kill switch for Local Yielding v1. Passive conflict
// observation remains active while this is disabled.
ConVar eta_local_yielding("eta_local_yielding", "0", 0,
	"Enable deterministic WAIT-only local ally yielding (0 = off)");

// Fortress Forever [APG]RoboCop[CL]
ConVar rcbot_ff_grenades("rcbot_ff_grenades", "1", 0, "Fortress Forever bots throw primary (frag) hand grenades at enemies");
ConVar rcbot_ff_grenade_cook("rcbot_ff_grenade_cook", "1.8", 0, "FF bot grenade cook/hold time (s) before throwing; clamped 0.6..3.0 (fuse is 3.81s)");
ConVar rcbot_ff_grenade_chance("rcbot_ff_grenade_chance", "0.5", 0, "Chance 0..1 an FF bot throws a grenade when a valid opportunity arises");
ConVar rcbot_ff_sniper_charge("rcbot_ff_sniper_charge", "2.0", 0, "FF sniper-rifle charge/hold time (s) before the shot is released; clamped 0.5..5.0 (full charge is 5.0s)");

ConVar rcbot_ranged_strafe("rcbot_ranged_strafe", "1", 0, "FF/TF2/HL2DM bots strafe side-to-side during ranged firefights to be harder to hit (0 = stand still). Not applied to Counter-Strike (recoil widens with movement).");
ConVar rcbot_melee_fallback_dist("rcbot_melee_fallback_dist", "150", 0, "Max distance a bot charges to melee when it has no usable ranged weapon (lower = less melee-rushing)");

ConVar bot_cmd_enable_wpt_sounds("rcbot_enable_wpt_sounds", "1", 0, "Enable/disable sound effects when editing waypoints");
//ConVar bot_general_difficulty("rcbot_skill", "0.8", 0, "General difficulty of the bots. 0.5 = stock, < 0.5 easier, > 0.5 = harder");//TODO: Broken! [APG]RoboCop[CL]
ConVar bot_visrevs_clients("rcbot_visrevs_clients", "3", 0, "how many revs the bot searches for visible players and enemies, lower to reduce cpu usage");
ConVar bot_spyknifefov("rcbot_spyknifefov", "80", 0, "the FOV from the enemy that spies must backstab from");
ConVar bot_visrevs("rcbot_visrevs", "6", 0, "how many revs the bot searches for visible monsters, lower to reduce cpu usage min:5");
ConVar bot_pathrevs("rcbot_pathrevs", "30", 0, "how many revs the bot searches for a path each frame, lower to reduce cpu usage, but causes bots to stand still more");
ConVar bot_command("rcbot_cmd", "", 0, "issues a command to all bots");
ConVar bot_attack("rcbot_flipout", "0", 0, "Rcbots all attack");
ConVar bot_scoutdj("rcbot_scoutdj", "0.5", 0, "time scout uses to double jump");
ConVar bot_anglespeed("rcbot_anglespeed", "0.25", 0, "smaller number will make bots turn slower (1 = instant turn but may overshoot)");
ConVar bot_stop("rcbot_stop", "0", 0, "Make bots stop thinking!");
ConVar bot_waypointpathdist("rcbot_wpt_pathdist", "400", 0, "Length for waypoints to automatically add paths at");

ConVar bot_rj("rcbot_rj", "0.01", 0, "time for soldier to fire rocket after jumping");
ConVar bot_defrate("rcbot_defrate", "0.2", 0, "rate for bots to defend");
ConVar bot_beliefmulti("rcbot_beliefmulti", "20.0", 0, "multiplier for increasing bot belief"); //Not referenced properly? [APG]RoboCop[CL]
ConVar bot_belief_fade("rcbot_belief_fade", "0.75", 0, "the multiplayer rate bot belief decreases");
ConVar bot_change_class("rcbot_change_classes", "0", 0, "bots change classes at random intervals");
ConVar bot_use_vc_commands("rcbot_voice_cmds", "1", 0, "bots use voice commands e.g. medic/spy etc");
ConVar bot_use_disp_dist("rcbot_disp_dist", "800.0", 0, "distance that bots will go back to use a dispenser");
ConVar bot_max_cc_time("rcbot_max_cc_time", "240", 0, "maximum time for bots to consider changing class <seconds>");
ConVar bot_min_cc_time("rcbot_min_cc_time", "60", 0, "minimum time for bots to consider changing class <seconds>");
ConVar bot_avoid_radius("rcbot_avoid_radius", "80", 0, "radius in units for bots to avoid things");
ConVar bot_avoid_strength("rcbot_avoid_strength", "100", 0, "strength of avoidance (0 = disable)");
ConVar bot_messaround("rcbot_messaround", "1", 0, "bots mess around at start up");
ConVar bot_heavyaimoffset("rcbot_heavyaimoffset", "0.1", 0, "fraction of how much the heavy aims at a diagonal offset");
ConVar bot_aimsmoothing("rcbot_aimsmoothing", "0", 0, "(0 = no smoothing)");//TODO: Broken! [APG]RoboCop[CL]
ConVar bot_bossattackfactor("rcbot_bossattackfactor", "1.0", 0, "the higher the more often the bots will shoot the boss");

ConVar rcbot_enemyshootfov("rcbot_enemyshootfov", "0.97", 0, "the fov dot product before the bot shoots an enemy 0.7 = 45 degrees");
ConVar rcbot_enemyshoot_gravgun_fov("rcbot_enemyshoot_gravgun_fov", "0.98", 0, "the fov dot product before the bot shoots an enemy 0.98 = 11 degrees");
ConVar rcbot_wptplace_width("rcbot_wpt_width", "48", 0, "width of the player, automatic paths won't connect unless there is enough space for a player");
ConVar rcbot_wpt_autoradius("rcbot_wpt_autoradius", "0", 0, "every waypoint added is given this radius, 0 = no radius");
ConVar rcbot_wpt_autotype("rcbot_wpt_autotype", "1", 0, "If 1, types will be automatically added to waypoints when they are added (only for resupply/health/capture/flag etc)\nIf 2: types will autoamtically be added even if the waypoint is cut/paste");
ConVar rcbot_wpt_autotype_detection_range("rcbot_wpt_autotype_detection_range", "80", 0, "Maximum distance between entities and a waypoint for automatic type detection.");
ConVar rcbot_move_sentry_time("rcbot_move_sentry_time", "120", 0, "seconds for bots to start thinking about moving sentries");
ConVar rcbot_move_sentry_kpm("rcbot_move_sentry_kpm", "1", 0, "kpm = kills per minute, if less than this, bots will think about moving the sentry");
ConVar rcbot_smoke_time("rcbot_smoke_time", "10", 0, "seconds a smoke grenade stays active");
ConVar rcbot_move_disp_time("rcbot_move_disp_time", "120", 0, "seconds for bots to start thinking about moving dispensers");
ConVar rcbot_move_disp_healamount("rcbot_move_disp_healamount", "100", 0, "if dispenser heals less than this per minute, bot will move the disp");
ConVar rcbot_demo_runup_dist("rcbot_demo_runup", "99.0", 0, "Distance the demo bot will take to run up for a pipe/sticky bomb jump");
ConVar rcbot_demo_jump("rcbot_enable_pipejump", "1", 0, "Enable experimental pipe jumping at rocket jump waypoints");
ConVar rcbot_move_tele_time("rcbot_move_tele_time", "120", 0, "seconds for bots to start thinking about moving teleporters");
ConVar rcbot_move_tele_tpm("rcbot_move_tele_tpm", "1", 0, "if no of players teleported per minute is less than this, bot will move the teleport");
ConVar rcbot_tf2_protect_cap_time("rcbot_tf2_prot_cap_time", "12.5", 0, "time that the bots will spend more attention to the cap point if attacked");
ConVar rcbot_tf2_protect_cap_percent("rcbot_tf2_protect_cap_percent", "0.25", 0, "the percentage that bots defend the capture point by standing on the point");
ConVar rcbot_tf2_spy_kill_on_cap_dist("rcbot_tf2_spy_kill_on_cap_dist", "200.0", 0, "the distance for spy bots to attack players capturing a point");

ConVar rcbot_move_dist("rcbot_move_dist", "800", 0, "minimum distance to move objects to");
ConVar rcbot_shoot_breakables("rcbot_shoot_breakables", "1", 0, "if 1, bots will shoot breakable objects");
ConVar rcbot_shoot_breakable_dist("rcbot_shoot_breakable_dist", "128.0", 0, "The distance bots will shoot breakables at");
ConVar rcbot_shoot_breakable_cos("rcbot_shoot_breakable_cos", "0.9", 0, "The cosine of the angle bots should worry about breaking objects at (default = 0.9) ~= 25 degrees");
ConVar rcbot_move_obj("rcbot_move_obj", "1", 0, "if 1 rcbot engineers will move objects around");
ConVar rcbot_taunt("rcbot_taunt", "0", 0, "enable/disable bots taunting");
ConVar rcbot_notarget("rcbot_notarget", "0", 0, "bots don't shoot the host!");
ConVar rcbot_nocapturing("rcbot_dontcapture", "0", 0, "bots don't capture flags in DOD:S");
ConVar rcbot_jump_obst_dist("rcbot_jump_obst_dist", "80", 0, "the distance from an obstacle the bot will jump");
//ConVar rcbot_jump_obst_speed("rcbot_jump_obst_speed", "100", 0, "the speed of the bot for the bot to jump an obstacle");
ConVar rcbot_speed_boost("rcbot_speed_boost", "1", 0, "multiplier for bots speed");
ConVar rcbot_melee_only("rcbot_melee_only", "0", 0, "if 1 bots will only use melee weapons");
ConVar rcbot_debug_iglev("rcbot_debug_iglev", "0", 0, "bot think ignores functions to test cpu speed"); //Not referenced properly? [APG]RoboCop[CL]
ConVar rcbot_dont_move("rcbot_dont_move", "0", 0, "if 1 , bots will all move forward");

ConVar rcbot_runplayercmd_dods("rcbot_runplayer_cmd_dods", "417", 0, "offset of the DOD:S PlayerRunCommand function");
ConVar rcbot_ladder_offs("rcbot_ladder_offs", "42", 0, "difference in height for bot to think it has touched the ladder waypoint");
ConVar rcbot_ffa("rcbot_ffa", "0", 0, "Free for all mode -- bots shoot everyone");
ConVar rcbot_prone_enemy_only("rcbot_prone_enemy_only", "1", 0, "if 1 bots only prone in DOD:S when they have an enemy");
ConVar rcbot_menu_update_time1("rcbot_menu_update_time1", "0.04", 0, "time to update menus [displaying message]");
ConVar rcbot_menu_update_time2("rcbot_menu_update_time2", "0.2", 0, "time to update menus [interval]");
ConVar rcbot_autowaypoint_dist("rcbot_autowpt_dist", "150.0", 0, "distance for autowaypoints to be placed");
ConVar rcbot_stats_inrange_dist("rcbot_stats_inrange_dist", "320.0", 0, "distance for bots to realise they have other players in range (for particular radio commands in DOD:S)");
ConVar rcbot_squad_idle_time("rcbot_squad_idle_time", "3.0", 0, "time for bots to do other things if squad leader is idle for a short time");
ConVar rcbot_bots_form_squads("rcbot_bots_form_squads", "1", 0, "if 1, bots will form their own squads via voice commands");
ConVar rcbot_empires_auto_commander("rcbot_empires_auto_commander", "1", 0, "if 1, one bot per team takes the Commander seat and builds the base (refineries, barracks, factory, armory, repair) and assigns Engineers; it leaves the seat to a human who sits in or uses the Commander vehicle");
ConVar rcbot_empires_commander_first("rcbot_empires_commander_first", "-1", 0, "test: building type the auto Commander builds first (0 refinery, 1 barracks, 2 radar, 3 armory, 4 repair, 5 vehicle factory), -1 normal order");
ConVar rcbot_empires_vehicles("rcbot_empires_vehicles", "1", 0, "if 1, bots buy vehicles at their team's vehicle factory (or anywhere with emp_sv_debug_build_vehicles 1), drive them, fight and run over enemies");
ConVar rcbot_empires_debug_log("rcbot_empires_debug_log", "0", 0, "if 1, print the bots' diagnostic [RCBOT2] lines to the server console (on in the dev harness)");
ConVar rcbot_empires_engibot("rcbot_empires_engibot", "1", 0, "1 = the commander can buy base engineer bots with /buildengibot in chat");
ConVar rcbot_empires_engibot_cost("rcbot_empires_engibot_cost", "100", 0, "team resources one /buildengibot costs");
ConVar rcbot_empires_skirmish("rcbot_empires_skirmish", "1", 0, "infantry in a fight keeps an effective distance to its weapon, side-steps away from the enemy aim and backs off when too close, reloading or hurt, instead of running straight at the enemy (2 = NF only, 3 = BE only, for comparisons)");
ConVar rcbot_empires_fight_odds("rcbot_empires_fight_odds", "1", 0, "in a fight, infantry compares visible enemies with teammates nearby: outnumbered it gives ground, ahead it presses in");
ConVar rcbot_empires_navmesh_hurt_cover("rcbot_empires_navmesh_hurt_cover", "1", 0, "3.25: with a navmesh, damage outside a skirmish takes a short navmesh cover (at most 6 s, 2-3.5 s wait) instead of the legacy CGotoHideSpotSched, and the bot keeps firing at a visible attacker unless reloading; 0 = legacy hide spot with fire held");
ConVar rcbot_empires_fair_intel("rcbot_empires_fair_intel", "1", 0, "3.20: team director, commander, objective, tank/APC goals and Lawliet know only fair enemy contacts (teammate sight, game spotting, own cameras/radars, bot sight reports; memory 10-180 s) plus the initial enemy base; 0 = off (full entity list), 1 = both teams, 2 = NF only, 3 = BE only");
ConVar rcbot_empires_health_poll_hurt("rcbot_empires_health_poll_hurt", "1", 0, "3.25b: Empires sends no player_hurt; compare the bot's health each think and call hurt() on a drop (>= 3 HP, not world/fall damage, not in a vehicle). Attacker = current enemy if seen in the last 1.5 s. 0 = hurt() never runs (as before)");
ConVar rcbot_empires_fight_cover("rcbot_empires_fight_cover", "1", 0, "in a fight, infantry that is reloading, badly hurt or outnumbered steps out of the enemy's sight to a close safe spot and comes back afterwards");
ConVar rcbot_empires_join_fight("rcbot_empires_join_fight", "1", 0, "free infantry walks to a fight of a teammate within 2000 units (max 2 helpers per fight)");
ConVar rcbot_empires_callouts("rcbot_empires_callouts", "1", 0, "infantry calls out (spots) enemies it starts fighting, like a player (only a group of enemies or one within 800 units; once per team per minute, per bot every 3 minutes)");
ConVar rcbot_empires_scout_flank("rcbot_empires_scout_flank", "1", 0, "a scout whose teammate fights the same enemy circles to the other side of the enemy instead of attacking from the same direction (up to 1200 units)");
ConVar rcbot_empires_scout_binoculars("rcbot_empires_scout_binoculars", "1", 0, "a scout that sees an unspotted enemy 1000-4000 units away stops and spots him with the binoculars for the team (per bot every 45 s, per team every 20 s)");
ConVar rcbot_empires_grenadier_rpg("rcbot_empires_grenadier_rpg", "1", 0, "a grenadier with rockets keeps 550-1300 units from an enemy vehicle and fires the RPG from there instead of running at it; against soldiers it keeps at least 400 units (rocket splash)");
ConVar rcbot_empires_engineer_rear("rcbot_empires_engineer_rear", "1", 0, "an engineer in a fight stays behind the teammate closest to the enemy instead of pushing in front (up to 1600 units)");
ConVar rcbot_empires_calm_look("rcbot_empires_calm_look", "1", 0, "a bot standing without a route looks toward the last enemy it saw (20 s) or calmly around its current facing (+-30 degrees, level, every 3-5 s) instead of at random points around its head every 2-3 s");
ConVar rcbot_empires_humans_first("rcbot_empires_humans_first", "1", 0, "TODO 8.3: humans come first: engineers prefer hurt humans (and leave a bot patient for a hurt human in reach), serve ammo calls of humans before bots, and a bot repair call does not take over the request for a human-driven vehicle, while a damaged human vehicle nearby replaces a bot call");
ConVar rcbot_empires_test_human_userid("rcbot_empires_test_human_userid", "0", 0, "test only: the bot with this userid counts as a human for rcbot_empires_humans_first and rcbot_empires_buddy_repair (0 = off)");
ConVar rcbot_empires_tank_escort("rcbot_empires_tank_escort", "1", 0, "TODO 8.4: one free engineer per team follows an own driven tank within 3000 HU about 350 HU behind it and repairs it there when it stands damaged");
ConVar rcbot_empires_buddy_repair("rcbot_empires_buddy_repair", "1", 0, "TODO 8.3: a free engineer bot repairs a damaged, standing own vehicle within 1200 HU unasked when a human drives it or it is parked empty (hull below 90 percent or an armor side below 85 percent); 2 = also bot-driven vehicles (test)");
ConVar rcbot_empires_self_preservation("rcbot_empires_self_preservation", "1", 0, "TODO 8.2: two teammates killed by the enemy within 8 s and 600 HU of a bot make it take cover or fall back 300-450 HU from the killer for 5 s instead of walking on (20 s cooldown); in a fight it backs off and breaks line of sight like when outnumbered");
ConVar rcbot_empires_grenadier_rpg_vehicle_priority("rcbot_empires_grenadier_rpg_vehicle_priority", "1", 0, "if 1, a grenadier with rockets breaks off any task (commander orders included) for an RPG shot at an enemy vehicle within 300-2600 units in sight, and only an enemy within 500 units interrupts that shot; 0 = old order (orders first, any visible enemy interrupts)");
ConVar rcbot_empires_lean_defense("rcbot_empires_lean_defense", "1", 0, "if 1, the team director keeps a standing defender at the base outside an alert only from 5 free infantry bots (floor 20 %, minima from 8/12 team bots stay); 0 = round 20 % (3 free bots: push/hold/defense)");
ConVar rcbot_empires_defend_patrol("rcbot_empires_defend_patrol", "1", 0, "if 1, a bot holding a defend or sector spot moves on to the next spot around it after 12 s instead of standing 30 s, and has to walk to within 100 of the spot (0 = 30 s hold, 250 arrive radius)");
ConVar rcbot_empires_hot_zones("rcbot_empires_hot_zones", "1", 0, "TODO 8.1: 3 deaths of a team on foot within 30 s and 700 HU make the spot hot for 90 s (each further death there renews it); routes of that team pay 1500 to enter and 3x per unit inside, so the next wave detours");
ConVar rcbot_empires_kill_spawn("rcbot_empires_kill_spawn", "1", 0, "if 1, a defender on a base alert or a far vehicle buyer may respawn (kill) at a spawn at least 3800 HU closer to its goal (no combat 6 s, tickets > 20 and > 25 %, 90 s per bot)");
ConVar rcbot_empires_commander_orders("rcbot_empires_commander_orders", "1", 0, "the seated bot commander orders its free attacker bots to the next capture point (attack location)");
ConVar rcbot_empires_engibot_limit("rcbot_empires_engibot_limit", "2", 0, "maximum EngiBots per team (0-8)");
ConVar rcbot_empires_engineer_help_human("rcbot_empires_engineer_help_human", "2", 0, "with a human commander, up to this many engineer bots per team build and repair the team's unfinished or damaged buildings near the CV on their own (0 = only on the commander's guard order)");
ConVar rcbot_empires_engibot_leash("rcbot_empires_engibot_leash", "2500", 0, "EngiBots work only within this distance of their team CV (else their first spawn)");
ConVar rcbot_empires_vehicle_drivers("rcbot_empires_vehicle_drivers", "-1", 0, "maximum number of bots per team that use a vehicle at the same time (Bill Nye not counted); -1 = 40% of the team bots, at least 2");
ConVar rcbot_empires_vehicle_debug_mixed("rcbot_empires_vehicle_debug_mixed", "0", 0, "tests only: bought tanks get the MG on the cannon's fire button (mixed loadout, TODO 3.10d)");
ConVar rcbot_empires_vehicle_debug_team("rcbot_empires_vehicle_debug_team", "0", 0, "tests only: 2 or 3 = only bots of this team buy or adopt vehicles (infantry-vs-vehicle tests); 0 = both");
ConVar rcbot_empires_vehicle_debug_buy("rcbot_empires_vehicle_debug_buy", "0", 0, "tests only: with emp_sv_debug_build_vehicles 1 buy in place (also when a vehicle factory exists); 0 = only at a vehicle factory");
ConVar rcbot_empires_billnye("rcbot_empires_billnye", "-1", 0, "Bill Nye mode: one Scout bot of a team always takes a jeep, rams enemy vehicles off the map, stuns them when stuck and runs over infantry. -1 = on maps named *rainbowroad* for NF, 0 = off, 2 = NF, 3 = BE");
ConVar rcbot_empires_commander_engineer("rcbot_empires_commander_engineer", "1", 0, "the auto Commander seat goes to the team's first Engineer bot (Repair role); he leaves the CV to finish buildings near it when no other Engineer is close and idle");
ConVar rcbot_empires_commander_drive_from("rcbot_empires_commander_drive_from", "1000", 0, "Commander Engineer drives the CV toward a building farther than this before leaving it to build");
ConVar rcbot_empires_force_offsets("rcbot_empires_force_offsets", "0", 0, "Use the fixed game-object offsets even when the game binary (server_srv.so) is not a known build: 0 off, 1 on");
ConVar rcbot_empires_commander_boomtank("rcbot_empires_commander_boomtank", "1", 0, "Commander bot drives the CV over enemy infantry inside the base zone: 0 off, 1 on except rainbowroad maps, 2 on everywhere");
ConVar rcbot_empires_engineer_turrets("rcbot_empires_engineer_turrets", "1", 0, "Engineer bots build an MG turret on the front side of own refineries");
ConVar rcbot_empires_engineer_deployables("rcbot_empires_engineer_deployables", "3", 0, "Engineer deployables at own refineries (bits): 1 = radar and camera, 2 = three-part wall on the enemy side");
ConVar rcbot_empires_vacancy_revive("rcbot_empires_vacancy_revive", "1", 0, "if 1, bots may run the death-vacancy Engineer Mass Revive transaction (promote/revive/restore) on a consumed per-vacancy-epoch basis");
ConVar rcbot_empires_lawliet("rcbot_empires_lawliet", "1", 0, "Lawliet mode (desperation APC rush on the enemy CV): 0 = off, 1 = auto when losing in research & map control, 2 = force NF, 3 = force BE");
ConVar rcbot_listen_dist("rcbot_listen_dist", "512", 0, "the distance for bots to hear things");
ConVar rcbot_footstep_speed("rcbot_footstep_speed", "250", 0, "the speed players can go when you first hear them make footsteps");
ConVar rcbot_bot_squads_percent("rcbot_bot_squads_percent", "50", 0, "the percentage of time bots make squads with other bots");
ConVar rcbot_tooltips("rcbot_tooltips", "1", 0, "Enables/disables help tooltips");

ConVar rcbot_debug_notasks("rcbot_debug_notasks", "0", 0, "Debug command, stops bots from doing tasks by themselves");
ConVar rcbot_debug_dont_shoot("rcbot_debug_dont_shoot", "0", 0, "Debug command, stops bots from shooting everyone");
ConVar rcbot_debug_show_route("rcbot_debug_show_route", "0", 0, "Debug command, shows waypoint route to host");
ConVar rcbot_tf2_autoupdate_point_time("rcbot_tf2_autoupdate_point_time", "60", 0, "Time to automatically update points in TF2 for any changes");
ConVar rcbot_tf2_payload_dist_retreat("rcbot_tf2_payload_dist_retreat", "512.0", 0, "Distance for payload bomb to be greater than at cap before defend team retreats");
ConVar rcbot_spy_runaway_health("rcbot_spy_runaway_health", "70", 0, "health which spies run away after attacking");
ConVar rcbot_supermode("rcbot_supermode", "0", 0, "If 1 will make every bot skill and reaction much higher");
ConVar rcbot_addbottime("rcbot_addbottime", "5", 0, "The time in seconds for bots to be added after another");
ConVar rcbot_gamerules_offset("rcbot_gamerules_offset", "5", 0, "offset for gamerules object");
ConVar rcbot_datamap_offset("rcbot_datamap_offset", "0", 0, "offset for datamaps");
ConVar rcbot_bot_quota_interval("rcbot_bot_quota_interval", "10", 0, "Interval between bot quota checks, 0 or lower to disable");
ConVar rcbot_show_welcome_msg("rcbot_show_welcome_msg", "1", 0, "Show welcome message on player connect");//Not referenced properly? [APG]RoboCop[CL]
ConVar rcbot_force_class("rcbot_force_class", "0", 0, "Force bots to choose specified class, kills alive bots on change (1 - 9, set to 0 for none)");
ConVar rcbot_ignore_spectators("rcbot_ignore_spectators", "0", 0, "Ignore spectators when calculating target number of bots");
ConVar rcbot_nonrandom_kicking("rcbot_nonrandom_kicking", "0", 0, "Choose newest bot from largest team when kicking bots");
ConVar rcbot_nonrandom_profile("rcbot_nonrandom_profile", "0", 0, "Choose first free bot from profile list when creating bots");

// Synergy CVars
ConVar rcbot_runplayercmd_syn("rcbot_runplayer_cmd_syn", "424", 0, "offset of the Synergy PlayerRunCommand function");
ConVar rcbot_syn_use_search_range("rcbot_syn_use_search_range", "256", 0, "Sets the maximum button search range.", true, 150.0f, true, 1024.0f);

// Counter-Strike:Source Cvars
ConVar rcbot_css_economy_eco_limit("rcbot_css_economy_eco_limit", "2000", 0, "If the bot money is less than this, it won't purchase anything.");

ConVarRef sv_gravity("sv_gravity");
ConVarRef mp_teamplay("mp_teamplay");
ConVarRef sv_tags("sv_tags");
ConVarRef mp_friendlyfire("mp_friendlyfire");
ConVarRef mp_stalemate_enable("mp_stalemate_enable");
ConVarRef mp_stalemate_meleeonly("mp_stalemate_meleeonly");

// For CS:S
ConVarRef mp_roundtime("mp_roundtime");
ConVarRef mp_c4timer("mp_c4timer");

void RCBOT2_Cvar_setup(ICvar* cvar) //'cvar' hides global declaration from /public/icvar.h [APG]RoboCop[CL]
{
    if (cvar == nullptr)
    {
        return; // Handle null pointer gracefully
    }

    ConVar* svTagsVar = cvar->FindVar("sv_tags");

    if (svTagsVar == nullptr)
    {
        return; // Handle missing variable gracefully
    }

    std::string sv_tags = svTagsVar->GetString();
    if (sv_tags.find("rcbot") == std::string::npos)
    {
        sv_tags += ",";
        sv_tags += "rcbot";
        svTagsVar->SetValue(sv_tags.c_str());
    }
}
