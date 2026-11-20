#include "bot_empires_spatial.h"
#include "convar.h"
#include "bot_cvars.h"
#include "bot_navmesh_api.h"

namespace EmpiresSpatial
{
SearchResult SearchBarracksCandidates(const EmpiresNativePlacement::ContextIdentity& context,
	const Request& request)
{
	if (!ValidRequest(request))
	{
		SearchResult result;
		result.status = Status::InvalidRequest;
		return result;
	}
	const Vector origin(request.origin.x, request.origin.y, request.origin.z);
	const auto ground = RCBotNavMesh::CollectLocalGroundSamples(origin, request.radius,
		MaxCells, MaxEntries, MaxAreas);
	Source source;
	source.available = ground.available;
	source.truncated = ground.truncated;
	source.cellsInspected = ground.cellsInspected;
	source.entriesInspected = ground.entriesInspected;
	source.areasInspected = ground.areasInspected;
	for (const auto& sample : ground.samples)
		source.samples.push_back({{sample.origin.x, sample.origin.y, sample.origin.z}, sample.areaId});
	const int buildingType = request.buildingType;
	return Select(request, source, [&context, buildingType](const Point& point, EmpiresNativePlacement::Diagnostics& diagnostics)
	{
		const Vector nativeOrigin(point.x, point.y, point.z);
		const auto result = EmpiresNativePlacement::CanPlaceNative(context, nativeOrigin, buildingType, &diagnostics);
		if (buildingType == 5)
			RCBOT_DIAG("[RCBOT2] commander-econ factory-candidate team=%d pos=(%.1f %.1f %.1f) result=%s can_build=%s obstruction_checked=%d mask=0x%x\n",
				diagnostics.team, point.x, point.y, point.z, EmpiresNativePlacement::Name(result),
				diagnostics.getAllObstructionsCalled ? "pass" : (diagnostics.canBuildAtCalled ? "fail" : "unchecked"),
				diagnostics.getAllObstructionsCalled, unsigned(diagnostics.obstructionMask));
		return result;
	});
}
}
