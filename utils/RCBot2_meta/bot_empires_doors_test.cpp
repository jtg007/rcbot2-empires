// Offline unit tests for bot_empires_doors.h (door-button parsing, timed gate
// detection, duplicate button origin matching, and directional clip/brush rules).
#include "bot_empires_doors.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace
{
int g_failures = 0;

void Expect(bool cond, const char* msg)
{
	if (!cond)
	{
		std::fprintf(stderr, "FAIL: %s\n", msg);
		++g_failures;
	}
}

void TestEscortButtons()
{
	const char* ents =
		"{\n"
		"\"classname\" \"func_button\"\n"
		"\"targetname\" \"control1\"\n"
		"\"origin\" \"-527.72 6012.5 -141.72\"\n"
		"\"OnPressed\" \"door1,Toggle,,0,-1\"\n"
		"}\n"
		"{\n"
		"\"classname\" \"func_button\"\n"
		"\"targetname\" \"control pillar tunnel\"\n"
		"\"origin\" \"4829.5 3920 -141.72\"\n"
		"\"OnPressed\" \"controlfirst_filter,TestActivator,,0,-1\"\n"
		"}\n"
		"{\n"
		"\"classname\" \"filter_activator_team\"\n"
		"\"targetname\" \"controlfirst_filter\"\n"
		"\"filterteam\" \"3\"\n"
		"\"Negated\" \"Allow entities that match criteria\"\n"
		"\"OnPass\" \"pillartunnel,Open,,0,-1\"\n"
		"}\n";
	const auto links = EmpiresDoors::ParseDoorButtonLinks(ents);
	Expect(links.size() == 2, "as_escort yields 2 links (control1 + pillar tunnel)");
	if (links.size() == 2)
	{
		Expect(links[0].buttonName == "control1" && links[0].doorName == "door1" && links[0].team == 0 && !links[0].timed,
			"control1 opens door1 for any team, not timed");
		Expect(links[0].hasOrigin && std::fabs(links[0].origin[0] + 527.72f) < 0.1f,
			"control1 origin parsed");
		Expect(links[1].buttonName == "control pillar tunnel" && links[1].doorName == "pillartunnel" &&
				links[1].team == 3 && !links[1].timed,
			"control pillar tunnel opens pillartunnel for BE (team 3), not timed");
	}
}

void TestCrossroadsTimedGate()
{
	const char* ents =
		"{\n"
		"\"classname\" \"func_button\"\n"
		"\"origin\" \"3521.5 4984 -172.51\"\n"
		"\"wait\" \"15\"\n"
		"\"OnIn\" \"be_gate1_motor,Open,,0,-1\"\n"
		"\"OnOut\" \"be_gate1_motor,Close,,0,-1\"\n"
		"}\n";
	const auto links = EmpiresDoors::ParseDoorButtonLinks(ents);
	Expect(links.size() == 1, "crossroads unnamed gate button parsed via origin");
	if (links.size() == 1)
	{
		Expect(links[0].buttonName.empty(), "crossroads button has empty targetname");
		Expect(links[0].doorName == "be_gate1_motor", "crossroads button targets be_gate1_motor");
		Expect(links[0].timed, "crossroads gate button flagged as timed (OnOut -> Close)");
		Expect(links[0].hasOrigin && std::fabs(links[0].origin[0] - 3521.5f) < 0.1f, "crossroads button origin parsed");
	}
}

void TestMvalleyDuplicateButtonNames()
{
	const char* ents =
		"{\n"
		"\"classname\" \"func_button\"\n"
		"\"targetname\" \"button\"\n"
		"\"origin\" \"437.27 -13531 -4684.01\"\n"
		"\"OnPressed\" \"door2,StartForward,,0,-1\"\n"
		"}\n"
		"{\n"
		"\"classname\" \"func_button\"\n"
		"\"targetname\" \"button\"\n"
		"\"origin\" \"161.95 -12465 -4692.26\"\n"
		"\"OnPressed\" \"dam_building_roof_door_east,Toggle,,0,-1\"\n"
		"\"OnPressed\" \"dam_building_roof_door_west,Toggle,,0,-1\"\n"
		"}\n";
	const auto links = EmpiresDoors::ParseDoorButtonLinks(ents);
	Expect(links.size() == 2, "mvalley parses only the roof-door button (2 doors)");
	if (links.size() == 2)
	{
		Expect(links[0].hasOrigin && std::fabs(links[0].origin[0] - 161.95f) < 0.1f,
			"mvalley link carries exact button origin to disambiguate duplicate targetname");
		Expect(links[1].doorName == "dam_building_roof_door_west" && !links[1].timed,
			"mvalley second roof door parsed as untimed");
	}
}

void TestDirectionalClipAndBrushPolicies()
{
	// Crossroads built one-way ramp exit face: gentle 13-degree slope (normal.z = 0.97)
	Expect(!EmpiresDoors::VehicleClipHitBlocks(true, false, false, 0.97f),
		"vehicle clip allows gentle exit ramp (normal.z=0.97)");
	Expect(!EmpiresDoors::BrushStepHitBlocks(true, false, false, 0.97f),
		"brush step allows gentle exit ramp (normal.z=0.97)");
	// Crossroads built ramp entry face or Mvalley vertical dam clip (normal.z = 0.0)
	Expect(EmpiresDoors::VehicleClipHitBlocks(true, false, false, 0.0f),
		"vehicle clip blocks vertical entry face (normal.z=0.0)");
	Expect(EmpiresDoors::BrushStepHitBlocks(true, false, false, 0.0f),
		"brush step blocks vertical entry face (normal.z=0.0)");
	// 45-degree face: too steep for vehicles (0.71 < 0.8), passable for infantry (0.71 >= 0.7)
	Expect(EmpiresDoors::VehicleClipHitBlocks(true, false, false, 0.71f),
		"vehicle clip blocks 45-degree face");
	Expect(!EmpiresDoors::BrushStepHitBlocks(true, false, false, 0.71f),
		"brush step allows walkable 45-degree slope");
	// Exiting from inside a wedge (startsolid && !allsolid) stays allowed; allsolid blocks.
	Expect(!EmpiresDoors::VehicleClipHitBlocks(true, true, false, 0.0f),
		"vehicle clip allows moving out from startsolid");
	Expect(EmpiresDoors::VehicleClipHitBlocks(true, true, true, 0.97f),
		"vehicle clip blocks allsolid");
	Expect(!EmpiresDoors::BrushStepHitBlocks(true, true, false, 0.0f),
		"brush step allows moving out from startsolid");
	Expect(EmpiresDoors::BrushStepHitBlocks(true, true, true, 0.97f),
		"brush step blocks allsolid");
	Expect(!EmpiresDoors::VehicleClipHitBlocks(false, false, false, 0.0f),
		"vehicle clip miss does not block");
}

void TestUnbuiltBrushSupportPolicy()
{
	// Mvalley unbuilt dam ramp: area at -4268, real ground at -4409 (141 HU gap -> unsupported).
	Expect(EmpiresDoors::UnbuiltBrushAreaUnsupported(-4268.0f, true, false, -4409.0f),
		"mvalley unbuilt dam ramp area is unsupported");
	// Unbuilt bridge over chasm (no ground hit within trace depth -> unsupported).
	Expect(EmpiresDoors::UnbuiltBrushAreaUnsupported(-200.0f, false, false, 0.0f),
		"unbuilt bridge with no ground hit is unsupported");
	// Crossroads unbuilt gate ramp sits flush on road (gap 4 HU -> supported).
	Expect(!EmpiresDoors::UnbuiltBrushAreaUnsupported(-100.0f, true, false, -104.0f),
		"crossroads unbuilt ramp on flat road stays supported");
	// Start-solid in world geometry means solid floor/wall is present -> not floating.
	Expect(!EmpiresDoors::UnbuiltBrushAreaUnsupported(-100.0f, true, true, -100.0f),
		"startsolid is not treated as floating");
	// Escape exits from an unsupported ramp area: bottom exit (-4378) allowed, top (-4160) blocked.
	Expect(EmpiresDoors::UnbuiltBrushExitAllowed(true, -4409.0f, -4378.0f),
		"mvalley ramp bottom exit stays open as escape");
	Expect(!EmpiresDoors::UnbuiltBrushExitAllowed(true, -4409.0f, -4160.0f),
		"mvalley ramp top exit is blocked while unbuilt");
	Expect(!EmpiresDoors::UnbuiltBrushExitAllowed(false, 0.0f, -4378.0f),
		"no exit allowed when there is no ground underneath");
}
} // namespace

int main()
{
	TestEscortButtons();
	TestCrossroadsTimedGate();
	TestMvalleyDuplicateButtonNames();
	TestDirectionalClipAndBrushPolicies();
	TestUnbuiltBrushSupportPolicy();
	if (g_failures > 0)
	{
		std::fprintf(stderr, "bot_empires_doors_test: %d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("bot_empires_doors_test: PASS\n");
	return 0;
}
