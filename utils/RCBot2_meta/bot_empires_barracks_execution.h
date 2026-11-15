#ifndef RCBOT_EMPIRES_BARRACKS_EXECUTION_H
#define RCBOT_EMPIRES_BARRACKS_EXECUTION_H
#include "bot_empires_spatial.h"
#include <cstdio>
namespace EmpiresBarracksExecution
{
enum class Status { SpatialFailure, CommandContextFailure, NativeBuildRejected, Requested, AlreadyAttempted };
inline const char* Name(Status s)
{
	switch (s) {
	case Status::SpatialFailure: return "SPATIAL_FAILURE";
	case Status::CommandContextFailure: return "COMMAND_CONTEXT_FAILURE";
	case Status::NativeBuildRejected: return "NATIVE_BUILD_REJECTED";
	case Status::Requested: return "REQUESTED";
	default: return "ALREADY_ATTEMPTED";
	}
}
struct Attempt { bool consumed = false; unsigned requests = 0; Status status = Status::SpatialFailure; };
// Six-argument dispatcher form: emp_build type x y z yaw. Pitch/roll default
// to zero; zero yaw is accepted by the parser, not validated by Spatial v1.
// Submission is not success: callers must observe the resulting native entity.
template<class Revalidate, class Sender>
Status Execute(Attempt& attempt, const EmpiresSpatial::SearchResult& search,
	bool commanderContext, Revalidate revalidate, Sender send)
{
	if (attempt.consumed) return Status::AlreadyAttempted;
	attempt.consumed = true;
	if (search.status != EmpiresSpatial::Status::Found || !search.hasSelected ||
		search.selected.nativeValidity != EmpiresNativePlacement::Result::Valid || !EmpiresSpatial::Finite(search.selected.origin))
		return attempt.status = Status::SpatialFailure;
	if (!commanderContext) return attempt.status = Status::CommandContextFailure;
	const auto& origin = search.selected.origin;
	const auto validity = revalidate(origin);
	if (validity == EmpiresNativePlacement::Result::Unavailable) return attempt.status = Status::CommandContextFailure;
	if (validity != EmpiresNativePlacement::Result::Valid) return attempt.status = Status::NativeBuildRejected;
	char command[160];
	const int size = snprintf(command, sizeof(command), "emp_build 1 %.9g %.9g %.9g 0\n", double(origin.x), double(origin.y), double(origin.z));
	if (size <= 0 || static_cast<unsigned>(size) >= sizeof(command)) return attempt.status = Status::CommandContextFailure;
	if (!send(command)) return attempt.status = Status::CommandContextFailure;
	++attempt.requests;
	return attempt.status = Status::Requested;
}
// Forward only the single synchronously observed creation, never a nearest
// building guess. The bridge receives the native entity incarnation.
template<class Assign>
const char* AssignCreatedBarracks(Status status, int newCount, int index, int serial, int team, Assign assign)
{
 if (status != Status::Requested || newCount != 1 || index <= 0 || serial < 0)
  return "BUILDING_REFERENCE_FAILURE";
 return assign(index, serial, team);
}
Status ExecuteNative(Attempt& attempt, const EmpiresNativePlacement::ContextIdentity& context,
	const EmpiresSpatial::SearchResult& search, EmpiresNativePlacement::Diagnostics& diagnostics);
}
#endif
