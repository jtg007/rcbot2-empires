/* Minimal Valve-inspired path following for the RCBot2 NavMesh spike. */
#ifndef __RCBOT_NAVMESH_FOLLOWER_H__
#define __RCBOT_NAVMESH_FOLLOWER_H__

#include "bot_navmesh_api.h"

#include <cstddef>

class CBot;

namespace RCBotNavMesh
{
enum class FollowStatus
{
	INACTIVE,
	RUNNING,
	COMPLETE,
	STUCK,
	FAILED,
};

const char *FollowStatusName(FollowStatus status);

class PathFollower
{
public:
	void Reset();
	void SetPath(const Path &path);

	FollowStatus Update(CBot *bot);
	bool IsActive() const { return m_active; }
	std::size_t CurrentPoint() const { return m_goalIndex; }
	std::size_t PointCount() const { return m_path.points.size(); }
	bool IsPathValid() const { return !m_path.points.empty(); }
	FollowStatus LastStatus() const { return m_lastStatus; }
	Vector CurrentTarget() const
	{
		return m_goalIndex < m_path.points.size() ?
			m_path.points[m_goalIndex] : Vector(0.0f, 0.0f, 0.0f);
	}
	Vector PreviousTarget() const
	{
		return m_goalIndex > 0 && m_goalIndex - 1 < m_path.points.size() ?
			m_path.points[m_goalIndex - 1] : Vector(0.0f, 0.0f, 0.0f);
	}

private:
	bool IsAtGoal(CBot *bot) const;
	bool IsPotentiallyTraversable(CBot *bot, const Vector &from,
		const Vector &to, float *fraction = nullptr) const;
	bool HasPotentialGap(CBot *bot, const Vector &from,
		const Vector &to) const;
	Vector Avoid(CBot *bot, const Vector &goal) const;
	bool IsInPathArea(const Vector &point) const;

	Path m_path;
	std::size_t m_goalIndex = 0;
	bool m_active = false;
	Vector m_lastProgressPosition = Vector(0.0f, 0.0f, 0.0f);
	float m_lastProgressTime = 0.0f;
	FollowStatus m_lastStatus = FollowStatus::INACTIVE;
};
}

#endif
