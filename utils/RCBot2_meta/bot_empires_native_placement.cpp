#include "bot_empires_native_placement.h"

#include "bot.h"
#include "bot_globals.h"
#include "bot_mods.h"
#include "iserverunknown.h"

#include <cmath>
#include <cstring>
#include <limits>

#if defined(__linux__) && defined(__i386__)
#include <dlfcn.h>
#include <elf.h>
#endif

extern IServerGameDLL* servergamedll;
extern IServerGameEnts* servergameents;

namespace EmpiresNativePlacement
{
namespace
{
bool Reject(Diagnostics* diagnostics, UnavailableReason reason)
{
	Unavailable(diagnostics, reason);
	return false;
}
}

const char* Name(Result result)
{
	switch (result)
	{
	case Result::Valid: return "valid";
	case Result::Invalid: return "invalid";
	default: return "unavailable";
	}
}

bool CaptureContext(int playerIndex, ContextIdentity& identity, Diagnostics* diagnostics)
{
	identity = ContextIdentity{};
	if (diagnostics != nullptr)
	{
		*diagnostics = Diagnostics{};
		diagnostics->player.index = playerIndex;
	}
	if (engine == nullptr || gpGlobals == nullptr || playerIndex <= 0 ||
		playerIndex > gpGlobals->maxClients)
		return Reject(diagnostics, UnavailableReason::InvalidContext);

	edict_t* edict = engine->PEntityOfEntIndex(playerIndex);
	if (edict == nullptr || edict->IsFree() || edict->GetUnknown() == nullptr)
		return Reject(diagnostics, UnavailableReason::PlayerUnresolved);

	if (diagnostics != nullptr) diagnostics->player.serial = edict->m_NetworkSerialNumber;
	if (edict->m_NetworkSerialNumber == 0)
		return Reject(diagnostics, UnavailableReason::PlayerSerialUnavailable);

	const CBaseHandle& handle = edict->GetUnknown()->GetRefEHandle();
	if (diagnostics != nullptr) diagnostics->player.handle = handle.ToInt();
	if (!handle.IsValid() || handle.GetEntryIndex() != playerIndex)
		return Reject(diagnostics, UnavailableReason::PlayerHandleInvalid);

	const int userid = engine->GetPlayerUserId(edict);
	if (diagnostics != nullptr) diagnostics->player.userid = userid;
	if (userid <= 0)
		return Reject(diagnostics, UnavailableReason::PlayerUserIdUnavailable);

	identity.index = playerIndex;
	identity.serial = edict->m_NetworkSerialNumber;
	identity.handle = handle.ToInt();
	identity.userid = userid;
	if (diagnostics != nullptr) diagnostics->player = identity;
	return true;
}

#if defined(__linux__) && defined(__i386__)
namespace
{
constexpr std::uintptr_t kCanBuildAt = 0x00397af0;
constexpr std::uintptr_t kGetAllObstructions = 0x00387ed0;
constexpr std::uintptr_t kGetTeamNumber = 0x005b17d0;
constexpr std::uintptr_t kGetGlobalEmpTeam = 0x006808c0;
constexpr std::uintptr_t kIsCommander = 0x006080e0;
constexpr std::uintptr_t kEmpPlayerVtable = 0x009d9068;
// Pinned ELF _ZTV8CEmpTeam + 8; CEmpTeam ctor writes this at 0x0068151b.
constexpr std::uintptr_t kEmpTeamVtable = 0x009dcba8;
constexpr std::uintptr_t kTeamCommanderPlayer = 0x474;
constexpr std::uintptr_t kTeamLevelState = 0x3b4;
constexpr std::uintptr_t kLevelValue = 0x30;
constexpr std::uintptr_t kCommanderContextHandle = 0x144c;
constexpr std::uintptr_t kEntityFlags = 0x119;
constexpr std::uintptr_t kAbsOrigin = 0x294;
constexpr unsigned kIsCommanderSlot = 0x72c / sizeof(std::uintptr_t);
const unsigned char kBuildId[20] = {
	0x55, 0x0f, 0x46, 0x68, 0xa1, 0xbb, 0xa1, 0xce, 0x16, 0xfa,
	0xa9, 0x70, 0xd9, 0xed, 0xdc, 0xdf, 0x67, 0x72, 0xd3, 0xa8
};

struct NativePoint
{
	float x;
	float y;
	float z;
};

using GetTeamNumberFn = int (*)(void* entity);
using GetGlobalEmpTeamFn = void* (*)(int team);
using IsCommanderFn = bool (*)(void* player);
using NativeCanBuildAtFn = CanBuildAtFn;
using NativeGetAllObstructionsFn = GetAllObstructionsFn;

bool CurrentImage(std::uintptr_t& base)
{
	if (servergamedll == nullptr)
		return false;

	Dl_info info{};
	if (!dladdr((*reinterpret_cast<void***>(servergamedll))[0], &info) ||
		info.dli_fbase == nullptr || info.dli_fname == nullptr)
		return false;
	const char* filename = std::strrchr(info.dli_fname, '/');
	if (std::strcmp(filename != nullptr ? filename + 1 : info.dli_fname, "server_srv.so") != 0)
		return false;

	base = reinterpret_cast<std::uintptr_t>(info.dli_fbase);
	const auto* elf = reinterpret_cast<const Elf32_Ehdr*>(base);
	if (std::memcmp(elf->e_ident, ELFMAG, SELFMAG) != 0 ||
		elf->e_ident[EI_CLASS] != ELFCLASS32 || elf->e_ident[EI_DATA] != ELFDATA2LSB ||
		elf->e_machine != EM_386 || elf->e_type != ET_DYN ||
		elf->e_phentsize != sizeof(Elf32_Phdr) || elf->e_phnum > 64)
		return false;

	const auto* headers = reinterpret_cast<const Elf32_Phdr*>(base + elf->e_phoff);
	for (unsigned i = 0; i < elf->e_phnum; ++i)
	{
		if (headers[i].p_type != PT_NOTE || headers[i].p_filesz > 4096)
			continue;
		const auto* begin = reinterpret_cast<const unsigned char*>(base + headers[i].p_vaddr);
		unsigned offset = 0;
		while (offset + sizeof(Elf32_Nhdr) <= headers[i].p_filesz)
		{
			const auto* note = reinterpret_cast<const Elf32_Nhdr*>(begin + offset);
			const unsigned nameSize = (note->n_namesz + 3u) & ~3u;
			const unsigned descSize = (note->n_descsz + 3u) & ~3u;
			if (nameSize > 4096 || descSize > 4096 ||
				offset + sizeof(*note) + nameSize + descSize > headers[i].p_filesz)
				return false;
			const auto* name = begin + offset + sizeof(*note);
			const auto* desc = name + nameSize;
			if (note->n_type == NT_GNU_BUILD_ID && note->n_namesz == 4 &&
				std::memcmp(name, "GNU", 4) == 0 && note->n_descsz == sizeof(kBuildId))
				return std::memcmp(desc, kBuildId, sizeof(kBuildId)) == 0;
			offset += sizeof(*note) + nameSize + descSize;
		}
	}
	return false;
}

bool ReadIdentity(const ContextIdentity& expected, edict_t*& edict, void*& entity,
	std::uintptr_t base, Diagnostics* diagnostics)
{
	edict = nullptr;
	entity = nullptr;
	if (engine == nullptr || servergameents == nullptr || gpGlobals == nullptr ||
		expected.index <= 0 || expected.index > gpGlobals->maxClients || expected.userid <= 0)
		return Reject(diagnostics, UnavailableReason::InvalidContext);

	ContextIdentity current;
	if (!CaptureContext(expected.index, current))
		return Reject(diagnostics, UnavailableReason::PlayerUnresolved);
	if (!expected.matches(current))
		return Reject(diagnostics, UnavailableReason::StalePlayer);
	edict = engine->PEntityOfEntIndex(expected.index);
	if (edict == nullptr || edict->IsFree() || edict->GetUnknown() == nullptr)
		return Reject(diagnostics, UnavailableReason::PlayerUnresolved);
	entity = edict->GetUnknown()->GetBaseEntity();
	if (entity == nullptr || servergameents->BaseEntityToEdict(
		static_cast<CBaseEntity*>(entity)) != edict ||
		*reinterpret_cast<std::uintptr_t*>(entity) != base + kEmpPlayerVtable)
		return Reject(diagnostics, UnavailableReason::PlayerObjectInvalid);
	return true;
}

bool ResolveTeamContext(void* player, int teamNumber, std::uintptr_t base,
	GetGlobalEmpTeamFn getGlobalEmpTeam, void*& teamObject, TeamContext& result,
	Diagnostics* diagnostics)
{
	teamObject = nullptr;
	result = TeamContext{};
	if ((teamNumber != 2 && teamNumber != 3) || getGlobalEmpTeam == nullptr)
		return Reject(diagnostics, UnavailableReason::TeamInvalid);
	teamObject = getGlobalEmpTeam(teamNumber);
	if (teamObject == nullptr)
		return Reject(diagnostics, UnavailableReason::TeamUnresolved);
	if (*reinterpret_cast<std::uintptr_t*>(teamObject) != base + kEmpTeamVtable)
		return Reject(diagnostics, UnavailableReason::TeamObjectInvalid);

	auto* levelState = *reinterpret_cast<void**>(
		static_cast<unsigned char*>(teamObject) + kTeamLevelState);
	if (levelState == nullptr)
		return Reject(diagnostics, UnavailableReason::LevelUnavailable);
	const float levelValue = *reinterpret_cast<float*>(
		static_cast<unsigned char*>(levelState) + kLevelValue);
	if (!std::isfinite(levelValue) ||
		static_cast<double>(levelValue) < std::numeric_limits<int>::min() ||
		static_cast<double>(levelValue) > std::numeric_limits<int>::max())
		return Reject(diagnostics, UnavailableReason::LevelUnavailable);

	result.team = teamNumber;
	result.level = static_cast<int>(levelValue);
	result.valid = true;
	if (diagnostics != nullptr)
	{
		diagnostics->team = teamNumber;
		diagnostics->level = result.level;
		diagnostics->contextOriginSource = "native-zero-no-commander";
	}

	void* commanderPlayer = *reinterpret_cast<void**>(
		static_cast<unsigned char*>(teamObject) + kTeamCommanderPlayer);
	if (commanderPlayer == nullptr)
		return true;
	if (*reinterpret_cast<std::uintptr_t*>(commanderPlayer) != base + kEmpPlayerVtable)
		return Reject(diagnostics, UnavailableReason::CommanderObjectInvalid);

	edict_t* commanderEdict = servergameents != nullptr ?
		servergameents->BaseEntityToEdict(static_cast<CBaseEntity*>(commanderPlayer)) : nullptr;
	if (commanderEdict == nullptr || commanderEdict->IsFree() ||
		commanderEdict->GetUnknown() == nullptr ||
		commanderEdict->GetUnknown()->GetBaseEntity() != commanderPlayer ||
		engine == nullptr || engine->IndexOfEdict(commanderEdict) <= 0 ||
		engine->IndexOfEdict(commanderEdict) > gpGlobals->maxClients)
		return Reject(diagnostics, UnavailableReason::CommanderIdentityUnavailable);
	ContextIdentity commanderIdentity;
	if (!CaptureContext(engine->IndexOfEdict(commanderEdict), commanderIdentity))
		return Reject(diagnostics, UnavailableReason::CommanderIdentityUnavailable);

	const auto* commanderVtable = *reinterpret_cast<const std::uintptr_t**>(commanderPlayer);
	if (commanderVtable[kIsCommanderSlot] != base + kIsCommander)
		return Reject(diagnostics, UnavailableReason::CommanderMethodMismatch);
	const auto isCommander = reinterpret_cast<IsCommanderFn>(commanderVtable[kIsCommanderSlot]);
	if (!isCommander(commanderPlayer))
	{
		if (diagnostics != nullptr) diagnostics->contextOriginSource = "native-zero-not-commander";
		return true;
	}

	const std::uint32_t handle = *reinterpret_cast<std::uint32_t*>(
		static_cast<unsigned char*>(commanderPlayer) + kCommanderContextHandle);
	if (handle == 0xffffffffu)
		return Reject(diagnostics, UnavailableReason::ContextHandleInvalid);
	const unsigned entryIndex = handle & 0xfffu;
	if (entryIndex == 0 || entryIndex >= static_cast<unsigned>(gpGlobals->maxEntities))
		return Reject(diagnostics, UnavailableReason::ContextHandleInvalid);
	edict_t* contextEdict = engine->PEntityOfEntIndex(static_cast<int>(entryIndex));
	if (contextEdict == nullptr || contextEdict->IsFree() ||
		contextEdict->GetUnknown() == nullptr ||
		contextEdict->GetUnknown()->GetRefEHandle().ToInt() != handle)
		return Reject(diagnostics, UnavailableReason::ContextEntityUnresolved);
	void* contextEntity = contextEdict->GetUnknown()->GetBaseEntity();
	if (contextEntity == nullptr || servergameents->BaseEntityToEdict(
		static_cast<CBaseEntity*>(contextEntity)) != contextEdict)
		return Reject(diagnostics, UnavailableReason::ContextObjectInvalid);

	const auto* entityBytes = static_cast<const unsigned char*>(contextEntity);
	if ((entityBytes[kEntityFlags] & 0x08u) != 0)
		return Reject(diagnostics, UnavailableReason::ContextOriginDirty); // Do not call CalcAbsolutePosition; it mutates native cache state.
	const auto* origin = reinterpret_cast<const NativePoint*>(entityBytes + kAbsOrigin);
	if (!std::isfinite(origin->x) || !std::isfinite(origin->y) || !std::isfinite(origin->z))
		return Reject(diagnostics, UnavailableReason::ContextOriginUnavailable);
	if (diagnostics != nullptr) diagnostics->contextOriginSource = "commander-entity";
	result.contextOrigin = Point{origin->x, origin->y, origin->z};
	(void)player;
	return true;
}
}
#endif

bool ReadCommanderMode(const ContextIdentity& context, bool& commander, Diagnostics* diagnostics)
{
	commander = false;
	if (diagnostics != nullptr) { *diagnostics = Diagnostics{}; diagnostics->player = context; }
#if defined(__linux__) && defined(__i386__)
	std::uintptr_t base = 0;
	if (!CurrentImage(base)) return Reject(diagnostics, UnavailableReason::BinaryIdentityUnavailable);
	edict_t* edict = nullptr;
	void* player = nullptr;
	if (!ReadIdentity(context, edict, player, base, diagnostics)) return false;
	const auto* vtable = *reinterpret_cast<const std::uintptr_t**>(player);
	if (vtable[kIsCommanderSlot] != base + kIsCommander)
		return Reject(diagnostics, UnavailableReason::CommanderMethodMismatch);
	commander = reinterpret_cast<IsCommanderFn>(vtable[kIsCommanderSlot])(player);
	return true;
#else
	(void)context;
	return Reject(diagnostics, UnavailableReason::UnsupportedPlatform);
#endif
}

Result CanPlaceNative(const ContextIdentity& context, const Vector& origin, int buildingType,
	Diagnostics* diagnostics)
{
	if (diagnostics != nullptr)
	{
		*diagnostics = Diagnostics{};
		diagnostics->player = context;
	}
#if defined(__linux__) && defined(__i386__)
	static_assert(sizeof(void*) == 4 && sizeof(bool) == 1, "Empires native ABI requires x86");
	static_assert(sizeof(Vector) == sizeof(Point), "Empires Vector ABI mismatch");
	if (CBotGlobals::getCurrentMod() == nullptr ||
		CBotGlobals::getCurrentMod()->getBotType() != BOTTYPE_EMPIRES)
		return Unavailable(diagnostics, UnavailableReason::WrongMod);
	if (engine == nullptr || gpGlobals == nullptr || servergamedll == nullptr ||
		servergameents == nullptr || g_pCVar == nullptr)
		return Unavailable(diagnostics, UnavailableReason::RuntimeUnavailable);

	// GetAllObstructions can draw diagnostic traces when this cvar is enabled.
	// Do not change the cvar; refuse the query instead.
	ConVar* visualize = g_pCVar->FindVar("r_visualizetraces");
	if (visualize == nullptr)
		return Unavailable(diagnostics, UnavailableReason::TraceControlUnavailable);
	if (visualize->GetBool())
		return Unavailable(diagnostics, UnavailableReason::TraceVisualizationEnabled);

	std::uintptr_t base = 0;
	if (!CurrentImage(base))
		return Unavailable(diagnostics, UnavailableReason::BinaryIdentityUnavailable);

	edict_t* contextEdict = nullptr;
	void* player = nullptr;
	if (!ReadIdentity(context, contextEdict, player, base, diagnostics))
		return Result::Unavailable;

	const auto getTeamNumber = reinterpret_cast<GetTeamNumberFn>(base + kGetTeamNumber);
	const auto getGlobalEmpTeam = reinterpret_cast<GetGlobalEmpTeamFn>(base + kGetGlobalEmpTeam);
	const auto canBuildAt = reinterpret_cast<NativeCanBuildAtFn>(base + kCanBuildAt);
	const auto getAllObstructions = reinterpret_cast<NativeGetAllObstructionsFn>(
		base + kGetAllObstructions);
	if (diagnostics != nullptr)
	{
		diagnostics->canBuildAtAvailable = canBuildAt != nullptr;
		diagnostics->getAllObstructionsAvailable = getAllObstructions != nullptr;
	}
	const int teamNumber = getTeamNumber(player);
	if (diagnostics != nullptr) diagnostics->team = teamNumber;
	if (teamNumber != 2 && teamNumber != 3)
		return Unavailable(diagnostics, UnavailableReason::TeamInvalid);

	void* teamObject = nullptr;
	TeamContext nativeContext;
	if (!ResolveTeamContext(player, teamNumber, base, getGlobalEmpTeam,
		teamObject, nativeContext, diagnostics))
		return Result::Unavailable;

	const Vector nativeContextOrigin(nativeContext.contextOrigin.x,
		nativeContext.contextOrigin.y, nativeContext.contextOrigin.z);
	const Result result = Evaluate(true, nativeContext,
		canBuildAt, getAllObstructions, origin, nativeContextOrigin, diagnostics, buildingType);

	// Re-read both identities after native calls; never return a classification
	// from a context that changed during the query.
	edict_t* currentEdict = nullptr;
	void* currentPlayer = nullptr;
	if (!ReadIdentity(context, currentEdict, currentPlayer, base, diagnostics))
		return Result::Unavailable;
	if (
		currentEdict != contextEdict || currentPlayer != player ||
		getTeamNumber(currentPlayer) != teamNumber)
		return Unavailable(diagnostics, UnavailableReason::StalePlayer);
	void* currentTeam = nullptr;
	TeamContext currentNativeContext;
	if (!ResolveTeamContext(currentPlayer, teamNumber, base, getGlobalEmpTeam,
		currentTeam, currentNativeContext, diagnostics))
		return Result::Unavailable;
	if (currentTeam != teamObject ||
		currentNativeContext.level != nativeContext.level ||
		currentNativeContext.contextOrigin.x != nativeContext.contextOrigin.x ||
		currentNativeContext.contextOrigin.y != nativeContext.contextOrigin.y ||
		currentNativeContext.contextOrigin.z != nativeContext.contextOrigin.z)
		return Unavailable(diagnostics, UnavailableReason::StaleTeamContext);
	return result;
#else
	(void)context;
	(void)origin;
	(void)buildingType;
	return Unavailable(diagnostics, UnavailableReason::UnsupportedPlatform);
#endif
}
}
