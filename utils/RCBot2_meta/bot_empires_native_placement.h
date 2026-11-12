#ifndef RCBOT_EMPIRES_NATIVE_PLACEMENT_H
#define RCBOT_EMPIRES_NATIVE_PLACEMENT_H

#include <cstdint>
#include <cmath>

class Vector;
class IHandleEntity;
class CResourcePoint;

namespace EmpiresNativePlacement
{
enum class Result
{
	Unavailable,
	Invalid,
	Valid
};

enum class UnavailableReason
{
	None,
	UnsupportedPlatform,
	RuntimeUnavailable,
	WrongMod,
	TraceControlUnavailable,
	TraceVisualizationEnabled,
	BinaryIdentityUnavailable,
	InvalidContext,
	PlayerUnresolved,
	PlayerSerialUnavailable,
	PlayerHandleInvalid,
	PlayerUserIdUnavailable,
	StalePlayer,
	PlayerObjectInvalid,
	TeamInvalid,
	TeamUnresolved,
	TeamObjectInvalid,
	LevelUnavailable,
	CommanderObjectInvalid,
	CommanderIdentityUnavailable,
	CommanderMethodMismatch,
	ContextHandleInvalid,
	ContextEntityUnresolved,
	ContextObjectInvalid,
	ContextOriginDirty,
	ContextOriginUnavailable,
	StaleTeamContext,
	CanBuildAtUnresolved,
	GetAllObstructionsUnresolved,
	OriginInvalid,
};

inline const char* Name(UnavailableReason reason)
{
	switch (reason)
	{
	case UnavailableReason::None: return "None";
	case UnavailableReason::UnsupportedPlatform: return "UnsupportedPlatform";
	case UnavailableReason::RuntimeUnavailable: return "RuntimeUnavailable";
	case UnavailableReason::WrongMod: return "WrongMod";
	case UnavailableReason::TraceControlUnavailable: return "TraceControlUnavailable";
	case UnavailableReason::TraceVisualizationEnabled: return "TraceVisualizationEnabled";
	case UnavailableReason::BinaryIdentityUnavailable: return "BinaryIdentityUnavailable";
	case UnavailableReason::InvalidContext: return "InvalidContext";
	case UnavailableReason::PlayerUnresolved: return "PlayerUnresolved";
	case UnavailableReason::PlayerSerialUnavailable: return "PlayerSerialUnavailable";
	case UnavailableReason::PlayerHandleInvalid: return "PlayerHandleInvalid";
	case UnavailableReason::PlayerUserIdUnavailable: return "PlayerUserIdUnavailable";
	case UnavailableReason::StalePlayer: return "StalePlayer";
	case UnavailableReason::PlayerObjectInvalid: return "PlayerObjectInvalid";
	case UnavailableReason::TeamInvalid: return "TeamInvalid";
	case UnavailableReason::TeamUnresolved: return "TeamUnresolved";
	case UnavailableReason::TeamObjectInvalid: return "TeamObjectInvalid";
	case UnavailableReason::LevelUnavailable: return "LevelUnavailable";
	case UnavailableReason::CommanderObjectInvalid: return "CommanderObjectInvalid";
	case UnavailableReason::CommanderIdentityUnavailable: return "CommanderIdentityUnavailable";
	case UnavailableReason::CommanderMethodMismatch: return "CommanderMethodMismatch";
	case UnavailableReason::ContextHandleInvalid: return "ContextHandleInvalid";
	case UnavailableReason::ContextEntityUnresolved: return "ContextEntityUnresolved";
	case UnavailableReason::ContextObjectInvalid: return "ContextObjectInvalid";
	case UnavailableReason::ContextOriginDirty: return "ContextOriginDirty";
	case UnavailableReason::ContextOriginUnavailable: return "ContextOriginUnavailable";
	case UnavailableReason::StaleTeamContext: return "StaleTeamContext";
	case UnavailableReason::CanBuildAtUnresolved: return "CanBuildAtUnresolved";
	case UnavailableReason::GetAllObstructionsUnresolved: return "GetAllObstructionsUnresolved";
	case UnavailableReason::OriginInvalid: return "OriginInvalid";
	}
	return "Unknown";
}

// Server-thread identity for the live Empires player used to obtain team state.
// Store values, never a cached edict or CBaseEntity pointer.
struct ContextIdentity
{
	int index = -1;
	int serial = 0;
	std::uint32_t handle = 0xffffffffu;
	int userid = 0;

	bool matches(const ContextIdentity& current) const
	{
		return index > 0 && serial != 0 && handle != 0xffffffffu && userid > 0 &&
			index == current.index && serial == current.serial &&
			handle == current.handle && userid == current.userid;
	}
};

struct Point
{
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
};

struct TeamContext
{
	int team = 0;
	int level = 0;
	Point contextOrigin;
	bool valid = false;
};

// Optional per-call diagnostics. No logging occurs on the production query path.
struct Diagnostics
{
	UnavailableReason reason = UnavailableReason::None;
	ContextIdentity player;
	int team = 0;
	int level = 0;
	const char* contextOriginSource = "unresolved";
	bool canBuildAtAvailable = false;
	bool getAllObstructionsAvailable = false;
	bool canBuildAtCalled = false;
	bool getAllObstructionsCalled = false;
};

inline Result Unavailable(Diagnostics* diagnostics, UnavailableReason reason)
{
	if (diagnostics != nullptr) diagnostics->reason = reason;
	return Result::Unavailable;
}

// The native Barracks placement gates do not consume yaw. This API deliberately
// accepts only an origin and must not be presented as yaw validation.
bool CaptureContext(int playerIndex, ContextIdentity& identity, Diagnostics* diagnostics = nullptr);
// buildingType follows CEmpBuildingParse::GetBuildingType (1 Barracks, 5 VehicleFactory, ...).
// Refineries (type 0) are placed on resource points and are not covered by this origin query.
Result CanPlaceNative(const ContextIdentity& context, const Vector& origin, int buildingType,
	Diagnostics* diagnostics = nullptr);
inline Result CanPlaceBarracksNative(const ContextIdentity& context, const Vector& origin,
	Diagnostics* diagnostics = nullptr)
{
	return CanPlaceNative(context, origin, 1, diagnostics);
}
const char* Name(Result result);
// Read-only native Commander-mode gate before intentional build requests.
bool ReadCommanderMode(const ContextIdentity& context, bool& commander, Diagnostics* diagnostics = nullptr);

// Engine-free ABI seam used by focused tests. GetAllObstructions argument order
// is (level, origin, building type, team, pass entity, context origin, resource point).
using CanBuildAtFn = std::uint8_t (*)(const Vector* origin, int team);
using GetAllObstructionsFn = std::uint32_t (*)(
	int level, const Vector* origin, int buildingType, int team,
	const IHandleEntity* passEntity, const Vector* contextOrigin,
	CResourcePoint* resourcePoint);

template <typename VectorType>
Result Evaluate(bool abiVerified, const TeamContext& context,
	CanBuildAtFn canBuildAt, GetAllObstructionsFn getAllObstructions,
	const VectorType& origin, const VectorType& contextOrigin, Diagnostics* diagnostics = nullptr,
	int buildingType = 1)
{
	if (diagnostics != nullptr) diagnostics->reason = UnavailableReason::None;
	if (!abiVerified) return Unavailable(diagnostics, UnavailableReason::BinaryIdentityUnavailable);
	if (!context.valid) return Unavailable(diagnostics, UnavailableReason::InvalidContext);
	if (context.team != 2 && context.team != 3) return Unavailable(diagnostics, UnavailableReason::TeamInvalid);
	if (canBuildAt == nullptr) return Unavailable(diagnostics, UnavailableReason::CanBuildAtUnresolved);
	if (getAllObstructions == nullptr) return Unavailable(diagnostics, UnavailableReason::GetAllObstructionsUnresolved);
	if (!std::isfinite(origin.x) || !std::isfinite(origin.y) || !std::isfinite(origin.z))
		return Unavailable(diagnostics, UnavailableReason::OriginInvalid);
	if (!std::isfinite(contextOrigin.x) || !std::isfinite(contextOrigin.y) || !std::isfinite(contextOrigin.z))
		return Unavailable(diagnostics, UnavailableReason::ContextOriginUnavailable);
	if (diagnostics != nullptr) diagnostics->canBuildAtCalled = true;

	if (canBuildAt(&origin, context.team) == 0)
		return Result::Invalid;

	if (diagnostics != nullptr) diagnostics->getAllObstructionsCalled = true;
	const std::uint32_t obstructionMask = getAllObstructions(
		context.level, &origin, buildingType, context.team, nullptr, &contextOrigin, nullptr);
	return obstructionMask == 0 ? Result::Valid : Result::Invalid;
}
}

#endif
