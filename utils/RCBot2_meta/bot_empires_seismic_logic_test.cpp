// Offline unit tests for bot_empires_seismic_logic.h (TODO 5.31.1 seismic targets & running throw).
#include "bot_empires_seismic_logic.h"

#include <cstdio>

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

void TestTargetClasses()
{
	Expect(EmpiresSeismic::IsSeismicTargetClass("emp_building_nf_barracks"), "barracks eligible");
	Expect(EmpiresSeismic::IsSeismicTargetClass("emp_building_mgturret"), "MG turret eligible");
	Expect(EmpiresSeismic::IsSeismicTargetClass("emp_building_mlturret"), "ML turret eligible");
	Expect(EmpiresSeismic::IsSeismicTargetClass("emp_eng_camera"), "camera eligible");
	Expect(EmpiresSeismic::IsSeismicTargetClass("emp_eng_radar"), "radar eligible");
	Expect(EmpiresSeismic::IsSeismicTargetClass("emp_eng_walls"), "engineer wall eligible");
	Expect(EmpiresSeismic::IsSeismicTargetClass("emp_eng_map_model"), "map obstacle model eligible");
	Expect(!EmpiresSeismic::IsSeismicTargetClass("emp_building_vehicle_physbox"), "physbox excluded");
	Expect(!EmpiresSeismic::IsSeismicTargetClass("emp_building_panel"), "panel excluded");
	Expect(!EmpiresSeismic::IsSeismicTargetClass("emp_eng_map_brush"), "map brush excluded");
	Expect(!EmpiresSeismic::IsSeismicTargetClass("player"), "player excluded");
}

void TestPreferences()
{
	Expect(EmpiresSeismic::TargetPreference("emp_building_mgturret") <
			EmpiresSeismic::TargetPreference("emp_eng_camera"),
		"turret preferred over camera");
	Expect(EmpiresSeismic::TargetPreference("emp_eng_camera") <
			EmpiresSeismic::TargetPreference("emp_eng_map_model"),
		"camera preferred over map obstacle");
	Expect(EmpiresSeismic::TargetPreference("emp_eng_map_model") <
			EmpiresSeismic::TargetPreference("emp_building_nf_barracks"),
		"map obstacle preferred over large building");
	Expect(EmpiresSeismic::TargetPreference("emp_building_nf_barracks") <
			EmpiresSeismic::TargetPreference("emp_eng_walls"),
		"building preferred over wall");
}

void TestStandingAndRunningThrows()
{
	// 920 HU flat (Crossroads barracks test): standing throw succeeds.
	const auto stand = EmpiresSeismic::PlanThrow(920.0f, -60.0f);
	Expect(stand.valid && !stand.running, "920 HU flat uses standing throw");

	// 2050 HU flat (just outside MG turret 2000 HU firing range): standing fails, running succeeds!
	float p = 0, e = 0, s = 0;
	Expect(!EmpiresSeismic::SolveThrowPitch(2050.0f, -60.0f, 0.0f, p, e, s),
		"2050 HU exceeds standing flat range");
	const auto run = EmpiresSeismic::PlanThrow(2050.0f, -60.0f);
	Expect(run.valid && run.running, "2050 HU (outside turret range) uses running throw");

	// Too close (< 170) or too far (> 2250): invalid.
	Expect(!EmpiresSeismic::PlanThrow(120.0f, -60.0f).valid, "120 HU rejected as too close");
	Expect(!EmpiresSeismic::PlanThrow(2400.0f, -60.0f).valid, "2400 HU rejected as too far");
}
} // namespace

int main()
{
	TestTargetClasses();
	TestPreferences();
	TestStandingAndRunningThrows();
	if (g_failures > 0)
	{
		std::fprintf(stderr, "bot_empires_seismic_logic_test: %d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("bot_empires_seismic_logic_test: PASS\n");
	return 0;
}
