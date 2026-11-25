// Pure-C++ helpers for Empires map doors, buttons, vehicle clips, and buildable
// engineer map brushes (SDK-free so they can be unit-tested offline).
#ifndef __BOT_EMPIRES_DOORS_H__
#define __BOT_EMPIRES_DOORS_H__

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace EmpiresDoors
{
struct MapOutput
{
	std::string target;
	std::string input;
};

struct MapEntity
{
	std::string classname;
	std::string targetname;
	std::string filterteam;
	std::string negated;
	bool hasOrigin = false;
	float origin[3] = {0.0f, 0.0f, 0.0f};
	std::vector<std::pair<std::string, MapOutput>> outputs;
};

struct DoorButtonLink
{
	std::string buttonName;
	std::string doorName;
	int team = 0; // 0 = any team, 2 = NF, 3 = BE
	bool hasOrigin = false;
	float origin[3] = {0.0f, 0.0f, 0.0f};
	// True when the button closes the door again on release (OnOut -> Close),
	// e.g. emp_crossroads 15-second timed base gates. Bots must not proactively
	// walk over and press timed gates that shut again right afterwards.
	bool timed = false;
};

// "OnPressed" "door1,Toggle,,0,-1" (older maps separate with ESC 0x1b).
inline MapOutput ParseOutput(const std::string& value)
{
	MapOutput out;
	const char sep = value.find('\x1b') != std::string::npos ? '\x1b' : ',';
	const std::size_t a = value.find(sep);
	out.target = value.substr(0, a);
	if (a != std::string::npos)
	{
		const std::size_t b = value.find(sep, a + 1);
		out.input = value.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
	}
	return out;
}

inline bool ParseOriginString(const std::string& value, float out[3])
{
	const char* p = value.c_str();
	char* end = nullptr;
	for (int i = 0; i < 3; ++i)
	{
		out[i] = static_cast<float>(std::strtod(p, &end));
		if (end == p)
			return false;
		p = end;
	}
	return true;
}

inline std::vector<MapEntity> ParseMapEntities(const char* text)
{
	std::vector<MapEntity> entities;
	if (text == nullptr)
		return entities;
	MapEntity current;
	bool inside = false;
	std::vector<std::string> tokens;
	for (const char* p = text; *p != '\0'; ++p)
	{
		if (*p == '{')
		{
			current = MapEntity();
			tokens.clear();
			inside = true;
		}
		else if (*p == '}' && inside)
		{
			for (std::size_t i = 0; i + 1 < tokens.size(); i += 2)
			{
				const std::string& key = tokens[i];
				const std::string& value = tokens[i + 1];
				if (key == "classname")
					current.classname = value;
				else if (key == "targetname")
					current.targetname = value;
				else if (key == "filterteam")
					current.filterteam = value;
				else if (key == "Negated")
					current.negated = value;
				else if (key == "origin")
					current.hasOrigin = ParseOriginString(value, current.origin);
				else if (key.size() > 2 && key[0] == 'O' && key[1] == 'n')
					current.outputs.emplace_back(key, ParseOutput(value));
			}
			entities.push_back(current);
			inside = false;
		}
		else if (*p == '"' && inside)
		{
			const char* end = std::strchr(p + 1, '"');
			if (end == nullptr)
				break;
			tokens.emplace_back(p + 1, end);
			p = end;
		}
	}
	return entities;
}

inline bool OpensDoorInput(const std::string& input)
{
	return input == "Toggle" || input == "Open";
}

inline bool ButtonClosesDoorOnOut(const MapEntity& button, const std::string& doorName)
{
	for (const auto& output : button.outputs)
		if (output.first == "OnOut" && output.second.target == doorName && output.second.input == "Close")
			return true;
	return false;
}

inline std::vector<DoorButtonLink> ParseDoorButtonLinks(const char* entitiesText)
{
	std::vector<DoorButtonLink> links;
	const std::vector<MapEntity> entities = ParseMapEntities(entitiesText);
	auto addLink = [&](const MapEntity& button, const std::string& doorName, int team) {
		if (doorName.empty())
			return;
		DoorButtonLink link;
		link.buttonName = button.targetname;
		link.doorName = doorName;
		link.team = team;
		link.hasOrigin = button.hasOrigin;
		link.origin[0] = button.origin[0];
		link.origin[1] = button.origin[1];
		link.origin[2] = button.origin[2];
		link.timed = ButtonClosesDoorOnOut(button, doorName);
		links.push_back(link);
	};
	for (const MapEntity& button : entities)
	{
		if (button.classname != "func_button")
			continue;
		if (button.targetname.empty() && !button.hasOrigin)
			continue;
		for (const auto& output : button.outputs)
		{
			const bool pressOutput = output.first == "OnPressed" || output.first == "OnIn";
			if (!pressOutput)
				continue;
			if (OpensDoorInput(output.second.input))
			{
				addLink(button, output.second.target, 0);
				continue;
			}
			if (output.second.input != "TestActivator")
				continue;
			// Team filter: OnPass runs for the filter team (or every other team if negated).
			for (const MapEntity& filter : entities)
			{
				if (filter.classname != "filter_activator_team" || filter.targetname != output.second.target)
					continue;
				const int filterTeam = std::atoi(filter.filterteam.c_str());
				if (filterTeam < 2 || filterTeam > 3)
					continue;
				const int team = std::atoi(filter.negated.c_str()) != 0 ? 5 - filterTeam : filterTeam;
				for (const auto& pass : filter.outputs)
					if (pass.first == "OnPass" && OpensDoorInput(pass.second.input))
						addLink(button, pass.second.target, team);
			}
		}
	}
	return links;
}

// Directional func_vehicleclip check: a gentle upward ramp face (e.g. built
// Crossroads one-way exit ramps, normal.z ~ 0.97) is drivable in the exit
// direction, while vertical walls (entry face or Mvalley dam clips, normal.z ~ 0)
// block vehicles.
inline bool VehicleClipHitBlocks(bool didHit, bool startSolid, bool allSolid, float normalZ)
{
	if (allSolid)
		return true;
	if (!didHit || startSolid)
		return false;
	return normalZ < 0.8f;
}

// Directional built emp_eng_map_brush check for infantry edges: gentle upward
// ramps and walkable bridges (normal.z >= 0.7) stay open; steep vertical faces
// (such as the 60-HU outer wall of a built Crossroads gate ramp) block entry.
inline bool BrushStepHitBlocks(bool didHit, bool startSolid, bool allSolid, float normalZ)
{
	if (allSolid)
		return true;
	if (!didHit || startSolid)
		return false;
	return normalZ < 0.7f;
}

// True when a navmesh area overlapping an UNBUILT emp_eng_map_brush has no
// real solid floor within maxGap (40 HU = navmesh max climb) underneath it —
// e.g. emp_mvalley unbuilt dam ramps or as_escort/emp_isle unbuilt bridges.
// Flat ground under an unbuilt Crossroads gate ramp stays supported (false).
inline bool UnbuiltBrushAreaUnsupported(float areaZ, bool groundHit, bool startSolid, float groundZ,
	float maxGap = 40.0f)
{
	if (startSolid)
		return false;
	if (!groundHit)
		return true;
	return (areaZ - groundZ) > maxGap;
}

// Outgoing escape links from an unsupported unbuilt-brush area stay open only
// when the destination lies near or below the real ground under that area.
inline bool UnbuiltBrushExitAllowed(bool groundHit, float groundZ, float destZ, float maxClimb = 40.0f)
{
	return groundHit && destZ <= groundZ + maxClimb;
}
} // namespace EmpiresDoors

#endif
