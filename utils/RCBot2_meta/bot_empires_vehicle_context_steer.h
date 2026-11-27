// Context steering for driven vehicles (rcbot_empires_vehicle_context_steer), SDK-free.
// The driver looks at kSlots directions around its hull: interest pulls toward the route (and, in
// front of a tight passage, toward an entry point on the passage axis), danger comes from short
// traces in vehicle width along each direction. The best safe forward direction wins; when the
// route direction itself is safe it is returned unchanged, so open-field driving stays as before.
// All yaws in degrees, world frame (0 = +x, 90 = +y).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace EmpiresContextSteer
{
constexpr int kSlots = 16;
constexpr float kSlotWidth = 360.0f / kSlots;
constexpr float kBlocked = 0.85f;       // danger at or above: slot is not driven into
constexpr float kForwardLimit = 100.0f; // slots further off the hull axis are left to reversing
constexpr float kDesiredSafe = 0.35f;   // route direction kept as is below this danger
constexpr float kSmoothWeight = 0.15f;  // pull toward the last chosen yaw (no steering flutter)
constexpr float kBrakeDecel = 300.0f;   // HU/s^2 conservative braking decel for speed caps

struct Point2D
{
	float x = 0.0f;
	float y = 0.0f;
};

struct TurnProfile
{
	float lateralAccel; // HU/s^2 usable lateral acceleration
	float minRadius;    // HU minimum turning radius at low speed
	float minTurnSpeed; // HU/s floor speed in tight corners
};

// Per-chassis turning profile. NF Light (2) and BE AFV (10) come from turn-test runs
// (20261119-103539/-103623); heavier chassis turn wider and carry more inertia.
inline TurnProfile ProfileForChassis(int chassis)
{
	switch (chassis)
	{
	case 0:
	case 8:
		return {900.0f, 220.0f, 240.0f}; // Jeep
	case 2:
		return {800.0f, 260.0f, 200.0f}; // NF Light Tank
	case 10:
		return {680.0f, 300.0f, 200.0f}; // BE AFV
	case 1:
	case 9:
		return {620.0f, 300.0f, 180.0f}; // APC
	case 3:
	case 4:
	case 11:
	case 12:
		return {540.0f, 310.0f, 170.0f}; // Medium / Artillery
	case 5:
	case 13:
	case 15:
	case 16:
		return {460.0f, 340.0f, 160.0f}; // Heavy / Commander Vehicle
	default:
		return {650.0f, 280.0f, 190.0f};
	}
}

inline float TurnRadiusFor(int chassis, float speed)
{
	const TurnProfile p = ProfileForChassis(chassis);
	const float r = speed * speed / p.lateralAccel;
	return r > p.minRadius ? r : p.minRadius;
}

inline float Normalize(float yaw)
{
	while (yaw > 180.0f)
		yaw -= 360.0f;
	while (yaw <= -180.0f)
		yaw += 360.0f;
	return yaw;
}

inline float Cos(float degrees)
{
	return std::cos(degrees * 3.14159265f / 180.0f);
}

// Slot i points heading + offset(i); offsets run 0, +22.5, ..., 180, ..., -22.5.
inline float SlotOffset(int i)
{
	return Normalize(static_cast<float>(i) * kSlotWidth);
}

inline float SlotYaw(float heading, int i)
{
	return Normalize(heading + SlotOffset(i));
}

// Entry aim at a tight passage: follow the passage axis as a line (pure pursuit). The aim point lies
// `lead` ahead of our projection onto the axis line, so the driver merges onto the axis at a flat angle
// (atan(lateral / lead)) well before the passage and drives straight through it.
// axisYaw = driving direction through the passage; lead >= 200.
inline float EntryAim(float x, float y, float gateX, float gateY, float axisYaw, float lead)
{
	const float ax = std::cos(axisYaw * 3.14159265f / 180.0f);
	const float ay = std::sin(axisYaw * 3.14159265f / 180.0f);
	const float along = (gateX - x) * ax + (gateY - y) * ay; // > 0: passage still ahead along the axis
	if (lead < 200.0f)
		lead = 200.0f;
	const float back = along - lead;                         // aim point = passage - axis * back
	const float tx = gateX - ax * back, ty = gateY - ay * back;
	return std::atan2(ty - y, tx - x) * 180.0f / 3.14159265f;
}

struct Result
{
	float yaw = 0.0f;   // chosen driving yaw
	int slot = -1;      // chosen slot, -1 = route direction kept unchanged
	bool blocked = false; // every forward slot is blocked: leave it to reversing
	float score = 0.0f;
};

// danger[i] in [0, 1] for slot i (1 = wall right at the hull). desiredDanger = danger straight along
// the desired yaw (its own trace). prevYaw used when hasPrev.
inline Result Choose(float heading, float desiredYaw, const float (&danger)[kSlots], float desiredDanger, bool hasPrev,
	float prevYaw)
{
	Result r;
	r.yaw = desiredYaw;
	const float desiredOffset = Normalize(desiredYaw - heading);
	if (desiredDanger < kDesiredSafe && std::fabs(desiredOffset) <= kForwardLimit)
		return r;
	float best = -1.0e9f;
	for (int i = 0; i < kSlots; ++i)
	{
		const float offset = SlotOffset(i);
		if (std::fabs(offset) > kForwardLimit || danger[i] >= kBlocked)
			continue;
		const float yaw = SlotYaw(heading, i);
		const float interest = 0.5f + 0.5f * Cos(yaw - desiredYaw);
		const float free = 1.0f - danger[i];
		float score = interest * free * free;
		if (hasPrev)
			score += kSmoothWeight * (0.5f + 0.5f * Cos(yaw - prevYaw));
		if (score > best)
		{
			best = score;
			r.slot = i;
			r.yaw = yaw;
		}
	}
	if (r.slot < 0)
	{
		r.blocked = true;
		r.yaw = desiredYaw;
		return r;
	}
	r.score = best;
	return r;
}

// Arc-length curvature speed cap along the upcoming polyline (Regulated Pure Pursuit style).
// Resampling at uniform arc-length steps detects multi-segment bends (e.g. a 110 deg turn split
// across 3-4 short navmesh portals before a gate) that single-segment angle checks miss, while
// returning 0 (no cap) on straights and gentle bends.
inline float PolylineCurveSpeed(Point2D origin, const Point2D* route, std::size_t count, std::size_t first,
	TurnProfile profile)
{
	if (route == nullptr || first >= count)
		return 0.0f;
	constexpr float kStep = 100.0f;
	constexpr int kMaxSamples = 17; // 0 .. 1600 HU preview
	Point2D samples[kMaxSamples];
	int n = 0;
	samples[n++] = origin;

	Point2D cur = origin;
	float carry = 0.0f;
	for (std::size_t i = first; i < count && n < kMaxSamples; ++i)
	{
		const Point2D next = route[i];
		const float dx = next.x - cur.x;
		const float dy = next.y - cur.y;
		const float segLen = std::hypot(dx, dy);
		if (segLen < 1.0f)
			continue;
		const float ux = dx / segLen;
		const float uy = dy / segLen;
		float pos = kStep - carry;
		while (pos <= segLen && n < kMaxSamples)
		{
			samples[n++] = {cur.x + ux * pos, cur.y + uy * pos};
			pos += kStep;
		}
		carry = segLen - (pos - kStep);
		cur = next;
	}
	if (n < 3)
		return 0.0f;

	float limitSq = 0.0f;
	const float minSpeedSq = profile.minTurnSpeed * profile.minTurnSpeed;
	auto evalAt = [&](int idx, int span) {
		const float ax = samples[idx].x - samples[idx - span].x;
		const float ay = samples[idx].y - samples[idx - span].y;
		const float bx = samples[idx + span].x - samples[idx].x;
		const float by = samples[idx + span].y - samples[idx].y;
		const float la = std::hypot(ax, ay);
		const float lb = std::hypot(bx, by);
		if (la < 20.0f || lb < 20.0f)
			return;
		float cosine = (ax * bx + ay * by) / (la * lb);
		if (cosine > 1.0f)
			cosine = 1.0f;
		else if (cosine < -0.999f)
			cosine = -0.999f;
		if (cosine >= 0.92f) // gentler than ~23 deg over the chord: full speed
			return;
		// Menger curvature radius of the circle through (A, B, C): exact on multi-segment arcs.
		const float cross = std::fabs(ax * by - ay * bx);
		const float cx = samples[idx + span].x - samples[idx - span].x;
		const float cy = samples[idx + span].y - samples[idx - span].y;
		const float lc = std::hypot(cx, cy);
		const float denom = 2.0f * cross;
		if (denom < 1.0f || lc < 20.0f)
			return;
		const float radius = (la * lb * lc) / denom;
		const float atTurnSq = (std::max)(minSpeedSq, profile.lateralAccel * radius);
		const float dist = static_cast<float>(idx) * kStep;
		// Account for ~0.25 s steering latency (~100 HU) on top of the entry chord.
		const float chord = static_cast<float>(span) * kStep;
		const float brakeDist = (std::max)(0.0f, dist - chord - 60.0f);
		const float allowedSq = atTurnSq + 2.0f * kBrakeDecel * brakeDist;
		limitSq = limitSq == 0.0f ? allowedSq : (std::min)(limitSq, allowedSq);
	};

	// Immediate tight bend right ahead (span 1 = 100 HU chord) plus smoothed bends (span 2 = 200 HU).
	evalAt(1, 1);
	for (int j = 2; j + 2 < n; ++j)
		evalAt(j, 2);
	if (n == 4)
		evalAt(2, 1);
	return limitSq > 0.0f ? std::sqrt(limitSq) : 0.0f;
}

// Speed cap while approaching a tight passage: slows down early enough to swing onto the passage
// axis when arriving off-axis or off-centre, and leaves well-aligned drivers at full speed.
// Returns 0 when no slowdown is needed.
inline float PassageApproachSpeed(float x, float y, float heading, float gateX, float gateY, float axisYaw,
	float gateFree, TurnProfile profile)
{
	const float ax = std::cos(axisYaw * 3.14159265f / 180.0f);
	const float ay = std::sin(axisYaw * 3.14159265f / 180.0f);
	const float dx = gateX - x;
	const float dy = gateY - y;
	const float toGate = std::hypot(dx, dy);
	if (toGate > 1200.0f)
		return 0.0f;
	const float along = dx * ax + dy * ay;
	if (along <= -50.0f)
		return 0.0f;
	const float lateral = std::fabs(-dx * ay + dy * ax);
	const float axisErr = std::fabs(Normalize(axisYaw - heading));
	if (axisErr < 15.0f && lateral < 60.0f && gateFree > 120.0f)
		return 0.0f;

	const float errNeed = (std::min)(1.0f, axisErr / 45.0f);
	const float latNeed = (std::min)(1.0f, lateral / 180.0f);
	const float tightNeed = gateFree < 120.0f ? 0.5f * (std::min)(1.0f, (120.0f - gateFree) / 120.0f) : 0.0f;
	const float need = (std::max)((std::max)(errNeed, latNeed), tightNeed);
	if (need <= 0.15f)
		return 0.0f;

	const float entrySpeed = profile.minTurnSpeed + (1.0f - need) * 260.0f;
	const float brakeDist = (std::max)(0.0f, toGate - 220.0f);
	return std::sqrt(entrySpeed * entrySpeed + 2.0f * kBrakeDecel * brakeDist);
}
} // namespace EmpiresContextSteer
