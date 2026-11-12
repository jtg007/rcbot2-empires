#include "bot_empires_barracks_execution.h"
#include "bot.h"
namespace EmpiresBarracksExecution
{
Status ExecuteNative(Attempt& attempt, const EmpiresNativePlacement::ContextIdentity& context,
	const EmpiresSpatial::SearchResult& search, EmpiresNativePlacement::Diagnostics& diagnostics)
{
	bool commander = false;
	const bool resolved = EmpiresNativePlacement::ReadCommanderMode(context, commander, &diagnostics);
	return Execute(attempt, search, resolved && commander && helpers != nullptr,
		[&](const EmpiresSpatial::Point& p) {
			return EmpiresNativePlacement::CanPlaceBarracksNative(context, Vector(p.x,p.y,p.z), &diagnostics);
		}, [&](const char* command) {
			bool stillCommander = false;
			if (!EmpiresNativePlacement::ReadCommanderMode(context, stillCommander, &diagnostics) || !stillCommander) return false;
			edict_t* player = engine->PEntityOfEntIndex(context.index);
			if (player == nullptr || player->IsFree()) return false;
			helpers->ClientCommand(player, command);
			return true;
		});
}
}
