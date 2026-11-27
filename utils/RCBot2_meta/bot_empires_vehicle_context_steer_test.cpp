// Offline unit tests for bot_empires_vehicle_context_steer.h (rcbot_empires_vehicle_context_steer).
#include "bot_empires_vehicle_context_steer.h"

#include <cstdio>

namespace
{
using namespace EmpiresContextSteer;

int g_failures = 0;

void Expect(bool cond, const char* msg)
{
	if (!cond)
	{
		std::fprintf(stderr, "FAIL: %s\n", msg);
		++g_failures;
	}
}

bool Near(float a, float b, float eps = 1.0f)
{
	return std::fabs(Normalize(a - b)) <= eps;
}

void Fill(float (&d)[kSlots], float v)
{
	for (float& x : d)
		x = v;
}

void TestSlots()
{
	Expect(Near(SlotOffset(0), 0.0f), "slot 0 is straight ahead");
	Expect(Near(SlotOffset(4), 90.0f), "slot 4 is left");
	Expect(Near(SlotOffset(12), -90.0f), "slot 12 is right");
	Expect(Near(SlotYaw(170.0f, 1), -167.5f), "slot yaw wraps");
}

void TestOpenFieldKeepsDesired()
{
	float d[kSlots];
	Fill(d, 0.0f);
	const Result r = Choose(0.0f, 37.0f, d, 0.0f, false, 0.0f);
	Expect(r.slot == -1 && Near(r.yaw, 37.0f, 0.01f) && !r.blocked, "free route direction is returned unchanged");
}

void TestAvoidsBlockedDesired()
{
	float d[kSlots];
	Fill(d, 0.0f);
	// Wall ahead and to the right: slots 0, 15, 14, 13 blocked; desired -30 (right).
	d[0] = d[15] = d[14] = d[13] = 1.0f;
	const Result r = Choose(0.0f, -30.0f, d, 1.0f, false, 0.0f);
	Expect(r.slot == 1, "takes the nearest free slot left of the wall");
	Expect(!r.blocked, "not blocked while a forward slot is free");
}

void TestPrefersLessDanger()
{
	float d[kSlots];
	Fill(d, 0.0f);
	d[0] = 0.7f; // grazing straight ahead
	const Result r = Choose(0.0f, 0.0f, d, 0.7f, false, 0.0f);
	Expect(r.slot == 1 || r.slot == 15, "a little off the scraping line beats the scraping line");
}

void TestAllForwardBlocked()
{
	float d[kSlots];
	Fill(d, 1.0f);
	d[8] = 0.0f; // only straight back is free
	const Result r = Choose(0.0f, 10.0f, d, 1.0f, false, 0.0f);
	Expect(r.blocked && r.slot == -1 && Near(r.yaw, 10.0f, 0.01f), "all forward slots blocked hands over to reversing");
}

void TestSmoothing()
{
	float d[kSlots];
	Fill(d, 0.0f);
	d[0] = 1.0f; // symmetric choice between slot 1 and slot 15
	const Result left = Choose(0.0f, 0.0f, d, 1.0f, true, 30.0f);
	const Result right = Choose(0.0f, 0.0f, d, 1.0f, true, -30.0f);
	Expect(left.slot == 1 && right.slot == 15, "ties follow the previous choice");
}

void TestEntryAim()
{
	// Passage at the origin, driven south (-90). From north-east the aim swings onto the axis line.
	const float far = EntryAim(300.0f, 600.0f, 0.0f, 0.0f, -90.0f, 300.0f);
	Expect(far < -120.0f && far > -175.0f, "off-axis approach aims west of the passage line first");
	// On the axis line it drives straight through.
	const float on = EntryAim(0.0f, 400.0f, 0.0f, 0.0f, -90.0f, 300.0f);
	Expect(Near(on, -90.0f), "on the axis line straight through");
	// Beside the axis the merge angle is flat (atan(lateral / lead)).
	const float merge = EntryAim(100.0f, 600.0f, 0.0f, 0.0f, -90.0f, 300.0f);
	Expect(Near(merge, -90.0f - 18.4f, 1.0f), "100 off the axis merges at about 18 degrees");
	// Past the passage it keeps following the axis.
	const float past = EntryAim(100.0f, -50.0f, 0.0f, 0.0f, -90.0f, 300.0f);
	Expect(std::fabs(Normalize(past + 90.0f)) < 45.0f, "past the passage still along the axis");
}

void TestPolylineCurveSpeed()
{
	const TurnProfile apc = ProfileForChassis(1);
	const TurnProfile heavy = ProfileForChassis(5);
	const TurnProfile light = ProfileForChassis(2);

	// Straight route: no cap.
	const Point2D straight[] = {{300, 0}, {700, 0}, {1200, 0}, {1800, 0}};
	Expect(PolylineCurveSpeed({0, 0}, straight, 4, 0, apc) == 0.0f, "straight route has no speed cap");

	// Gentle bend (~14 deg): no cap.
	const Point2D gentle[] = {{500, 0}, {1000, 125}, {1500, 250}};
	Expect(PolylineCurveSpeed({0, 0}, gentle, 3, 0, apc) == 0.0f, "gentle bend stays uncapped");

	// Sharp 90-degree turn 400 HU ahead: caps APC below cruising speed (~620) and Heavy lower than Light.
	const Point2D turn90[] = {{400, 0}, {400, 600}, {400, 1200}};
	const float apc90 = PolylineCurveSpeed({0, 0}, turn90, 3, 0, apc);
	const float heavy90 = PolylineCurveSpeed({0, 0}, turn90, 3, 0, heavy);
	const float light90 = PolylineCurveSpeed({0, 0}, turn90, 3, 0, light);
	Expect(apc90 > 200.0f && apc90 < 520.0f, "90-degree corner 400 HU ahead slows APC");
	Expect(heavy90 < apc90 && apc90 < light90, "heavier chassis gets lower curve cap");

	// 110-degree bend split across 4 short navmesh segments (~27 deg each), apex ~430 HU ahead:
	// old per-segment cosine<0.90 missed this completely; arc-length Menger curvature slows the APC.
	const Point2D splitBend[] = {
		{150, 0}, {280, 50}, {370, 160}, {390, 300}, {360, 500}, {330, 900},
	};
	const float splitCap = PolylineCurveSpeed({0, 0}, splitBend, 6, 0, apc);
	Expect(splitCap > 180.0f && splitCap < 480.0f, "multi-segment 110-degree bend slows APC");

	// Farther turn allows higher current speed than a close turn.
	const Point2D far90[] = {{1100, 0}, {1100, 600}, {1100, 1200}};
	const float farCap = PolylineCurveSpeed({0, 0}, far90, 3, 0, apc);
	Expect(farCap > apc90 + 150.0f, "distant corner allows higher speed until braking zone");
}

void TestPassageApproachSpeed()
{
	const TurnProfile apc = ProfileForChassis(1);
	// Aligned on the gate axis with reasonable clearance: no slowdown.
	Expect(PassageApproachSpeed(0.0f, -500.0f, 90.0f, 0.0f, 0.0f, 90.0f, 200.0f, apc) == 0.0f,
		"aligned driver passes wide enough gate without cap");
	// Approaching 45 deg off-axis 400 HU before the gate: brakes toward entry speed.
	const float offCap = PassageApproachSpeed(-300.0f, -300.0f, 20.0f, 0.0f, 0.0f, 90.0f, 200.0f, apc);
	Expect(offCap > 180.0f && offCap < 420.0f, "off-axis approach slows down before gate");
	// Much closer off-axis slows down even more.
	const float closeCap = PassageApproachSpeed(-150.0f, -150.0f, 20.0f, 0.0f, 0.0f, 90.0f, 200.0f, apc);
	Expect(closeCap < offCap && closeCap <= 220.0f, "close off-axis approach reaches low entry speed");
	// Already past the gate: no cap.
	Expect(PassageApproachSpeed(0.0f, 80.0f, 45.0f, 0.0f, 0.0f, 90.0f, 100.0f, apc) == 0.0f,
		"past the gate releases the cap");
	// Very tight gap (< 120 free) caps modestly even when aligned.
	const float tightCap = PassageApproachSpeed(0.0f, -250.0f, 90.0f, 0.0f, 0.0f, 90.0f, 40.0f, apc);
	Expect(tightCap > 250.0f && tightCap < 420.0f, "very tight passage moderates speed");
}
} // namespace

int main()
{
	TestSlots();
	TestOpenFieldKeepsDesired();
	TestAvoidsBlockedDesired();
	TestPrefersLessDanger();
	TestAllForwardBlocked();
	TestSmoothing();
	TestEntryAim();
	TestPolylineCurveSpeed();
	TestPassageApproachSpeed();
	if (g_failures != 0)
		return 1;
	std::printf("context steer tests passed\n");
	return 0;
}
