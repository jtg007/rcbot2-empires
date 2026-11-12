#include "bot_empires_combat_telemetry.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace
{

void Require(const bool condition, const char* message)
{
	if (!condition)
	{
		std::cerr << "FAIL: " << message << std::endl;
		std::exit(1);
	}
}

void TestCorrelationIdentity()
{
	using namespace RCBotEmpiresCombatTelemetry;
	Correlation first;
	first.curtime = 42.125;
	first.lifeGeneration = 7;
	first.captureGeneration = 19;
	first.captureAttempt = 3;
	first.ownerUserId = 101;
	first.ownerTeam = 2;
	first.capturePoint = 4;
	first.targetIndex = 12;
	first.targetUserId = 202;
	first.targetTeam = 3;
	const std::string firstText = FormatCorrelation(first);
	Require(firstText.find("curtime=42.125") != std::string::npos,
		"correlation includes game curtime");
	Require(firstText.find("life_generation=7;capture_generation=19;") !=
		std::string::npos, "correlation includes episode generations");
	Require(firstText.find("capture_attempt=3;owner_userid=101;owner_team=2;") !=
		std::string::npos, "correlation includes owner and route attempt");
	Require(firstText.find("capture_point=4;target_index=12;target_userid=202;target_team=3") !=
		std::string::npos, "correlation includes point and target identity");

	Correlation second = first;
	second.captureAttempt = 4;
	Require(FormatCorrelation(first) != FormatCorrelation(second),
		"new route attempt has a distinct correlation identity");
	second.ownerUserId = 102;
	Require(FormatCorrelation(first) != FormatCorrelation(second),
		"two bots on one point remain distinguishable");
	std::cout << "OBS-CORRELATION PASS\n";
}

void TestIssueDropResumeSequence()
{
	using namespace RCBotEmpiresCombatTelemetry;
	Correlation issue;
	issue.lifeGeneration = 9;
	issue.captureGeneration = 22;
	issue.captureAttempt = 6;
	issue.ownerUserId = 301;
	issue.ownerTeam = 2;
	issue.capturePoint = 5;
	issue.targetIndex = 17;
	issue.targetUserId = 401;
	issue.targetTeam = 3;
	const std::string issueText = FormatCorrelation(issue);
	const std::string dropText = FormatDecision("CONTINUE", "DROP",
		"enemy-left-relevant-area", "PURSUIT_TERMINATED") + ";" +
		FormatCorrelation(issue);
	Correlation resumed = issue;
	resumed.captureAttempt = 7;
	const std::string resumeText = FormatDecision("CONTINUE", "RESUME",
		"schedule-complete-before-arrival", "CAPTURE_PRESERVED") + ";" +
		FormatCorrelation(resumed);
	Require(dropText.find("capture_generation=22;capture_attempt=6") !=
		std::string::npos, "drop retains the issued objective and attempt");
	Require(resumeText.find("capture_generation=22;capture_attempt=7") !=
		std::string::npos, "resume retains objective but advances route attempt");
	Require(issueText.find("capture_point=5;target_index=17;target_userid=401") !=
		std::string::npos, "issue identity is available to the sequence");
	Require(dropText.find("capture_point=5;target_index=17;target_userid=401") !=
		std::string::npos, "drop identity remains correlated");
	std::cout << "OBS-SEQUENCE PASS\n";
}

void TestDistinctTargetsAndReset()
{
	using namespace RCBotEmpiresCombatTelemetry;
	Correlation first;
	first.ownerUserId = 301;
	first.capturePoint = 5;
	first.targetIndex = 17;
	first.targetUserId = 401;
	first.targetTeam = 3;
	Correlation second = first;
	second.targetIndex = 18;
	second.targetUserId = 402;
	Require(FormatCorrelation(first) != FormatCorrelation(second),
		"two enemies for one bot remain distinguishable");
	Correlation reset;
	const std::string resetText = FormatCorrelation(reset);
	Require(resetText.find("capture_attempt=0;owner_userid=-1;owner_team=-1;") !=
		std::string::npos, "reset correlation has neutral owner and attempt state");
	Require(resetText.find("capture_point=-1") != std::string::npos,
		"reset correlation has no active capture point");
	std::cout << "OBS-TARGETS-RESET PASS\n";
}

void TestDecisionPhases()
{
	using namespace RCBotEmpiresCombatTelemetry;
	const std::string defer = FormatDecision("ACQUIRE", "DEFER",
		"enemy-irrelevant-to-capture", "CAPTURE_PRESERVED");
	const std::string drop = FormatDecision("CONTINUE", "DROP",
		"enemy-left-relevant-area", "PURSUIT_TERMINATED");
	Require(defer.find("decision_phase=ACQUIRE;decision=DEFER") == 0,
		"defer records acquire phase and decision");
	Require(defer.find("caller_outcome=CAPTURE_PRESERVED") != std::string::npos,
		"defer records caller outcome");
	Require(drop.find("decision_phase=CONTINUE;decision=DROP") == 0,
		"drop records continue phase and decision");
	Require(drop.find("caller_outcome=PURSUIT_TERMINATED") != std::string::npos,
		"drop records caller outcome");
	Require(defer != drop, "defer and drop are unambiguous event records");
	std::cout << "OBS-PHASE PASS\n";
}

void TestRetryAccounting()
{
	using namespace RCBotEmpiresCombatTelemetry;
	const std::string resume = FormatRetryAccounting(true, true, 1, 1, true, false);
	const std::string consume = FormatRetryAccounting(false, true, 1, 2, false, true);
	Require(resume.find("combat_latch_before=1;capture_valid_before=1;") == 0,
		"resume records latch and objective validity before consume");
	Require(resume.find("retry_before=1;retry_after=1;combat_resume=1;consume_retry=0") !=
		std::string::npos, "resume records no-burn retry accounting");
	Require(consume.find("retry_before=1;retry_after=2;combat_resume=0;consume_retry=1") !=
		std::string::npos, "genuine retry records budget consumption");
	std::cout << "OBS-RETRY PASS\n";
}

void TestBoundedFormatting()
{
	using namespace RCBotEmpiresCombatTelemetry;
	Correlation value;
	const std::string text = FormatCorrelation(value);
	Require(text.size() < 512, "correlation formatting remains bounded");
	Require(text.find("0x") == std::string::npos,
		"correlation uses stable identities instead of pointers");
	std::cout << "OBS-BOUNDED PASS\n";
}

} // namespace

int main()
{
	TestCorrelationIdentity();
	TestIssueDropResumeSequence();
	TestDistinctTargetsAndReset();
	TestDecisionPhases();
	TestRetryAccounting();
	TestBoundedFormatting();
	std::cout << "OBS-TELEMETRY PASS;behavior_changes=0\n";
	return 0;
}
