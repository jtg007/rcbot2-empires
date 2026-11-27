// Context steering for driven vehicles (rcbot_empires_vehicle_context_steer), SDK-free.
// The driver looks at kSlots directions around its hull: interest pulls toward the route (and, in
// front of a tight passage, toward an entry point on the passage axis), danger comes from short
// traces in vehicle width along each direction. The best safe forward direction wins; when the
// route direction itself is safe it is returned unchanged, so open-field driving stays as before.
// All yaws in degrees, world frame (0 = +x, 90 = +y).
#pragma once

#include <cmath>

namespace EmpiresContextSteer
{
constexpr int kSlots = 16;
constexpr float kSlotWidth = 360.0f / kSlots;
constexpr float kBlocked = 0.85f;       // danger at or above: slot is not driven into
constexpr float kForwardLimit = 100.0f; // slots further off the hull axis are left to reversing
constexpr float kDesiredSafe = 0.35f;   // route direction kept as is below this danger
constexpr float kSmoothWeight = 0.15f;  // pull toward the last chosen yaw (no steering flutter)

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
} // namespace EmpiresContextSteer
