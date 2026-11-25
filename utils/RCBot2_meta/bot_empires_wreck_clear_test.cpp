// Offline unit tests for bot_empires_wreck_clear.h (TODO 5.8a / 3.17).
#include "bot_empires_wreck_clear.h"

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

void TestCarcassEvaluation()
{
	// Live vehicle is never a carcass (kit repairs live vehicles, does not deconstruct them).
	Expect(!EmpiresWreckClear::EvaluateCarcass(true, 100, 200.0f, true, 50.0f, true, 300.0f).eligible,
		"live vehicle rejected as carcass");
	// Dead vehicle with 0 carcass health is already gone.
	Expect(!EmpiresWreckClear::EvaluateCarcass(false, 0, 200.0f, true, 50.0f, true, 300.0f).eligible,
		"zero carcass health rejected");

	// Factory bay blocker at 2500 HU beats a closer base carcass at 300 HU.
	const auto bay = EmpiresWreckClear::EvaluateCarcass(false, 100, 2500.0f, true, 120.0f, true, 800.0f);
	const auto local = EmpiresWreckClear::EvaluateCarcass(false, 100, 300.0f, true, 1200.0f, true, 800.0f);
	Expect(bay.eligible && bay.blocksFactory, "carcass in VF bay is eligible and flagged");
	Expect(local.eligible && !local.blocksFactory, "local base carcass is eligible");
	Expect(bay.score < local.score, "factory bay blocker outranks closer loose carcass");

	// Far field carcass (> 900 from eng, > 2000 from CV, > 500 from VF) ignored.
	Expect(!EmpiresWreckClear::EvaluateCarcass(false, 100, 1500.0f, true, 5000.0f, true, 4000.0f).eligible,
		"far field carcass ignored");
}

void TestEnemyFactoryBlocker()
{
	Expect(EmpiresWreckClear::IsEnemyFactoryBlocker(true, 3, 2, true, 140.0f, 1800.0f),
		"live enemy vehicle in NF factory bay flagged for grenadier");
	Expect(!EmpiresWreckClear::IsEnemyFactoryBlocker(true, 2, 2, true, 140.0f, 1800.0f),
		"friendly vehicle in NF factory bay is not an enemy blocker");
	Expect(!EmpiresWreckClear::IsEnemyFactoryBlocker(false, 3, 2, true, 140.0f, 1800.0f),
		"dead enemy carcass handled by engineer, not grenadier");
	Expect(!EmpiresWreckClear::IsEnemyFactoryBlocker(true, 3, 2, true, 900.0f, 1800.0f),
		"enemy vehicle far from factory bay not flagged as bay blocker");
}
} // namespace

int main()
{
	TestCarcassEvaluation();
	TestEnemyFactoryBlocker();
	if (g_failures > 0)
	{
		std::fprintf(stderr, "bot_empires_wreck_clear_test: %d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("bot_empires_wreck_clear_test: PASS\n");
	return 0;
}

