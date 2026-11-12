#ifndef __RCBOT_EMPIRES_VACANCY_REVIVE_H__
#define __RCBOT_EMPIRES_VACANCY_REVIVE_H__

#include <cstdint>
#include <vector>

namespace RCBotEmpiresVacancyRevive
{
	// Verified Mass Revive cost table, indexed by eligible dead count.
	// Lookup clamps at 4+ targets. Mirrors the server table {4,6,8,10}.
	inline int ReviveCostForTargets(const int deadCount)
	{
		if (deadCount <= 1)
			return 4;
		if (deadCount == 2)
			return 6;
		if (deadCount == 3)
			return 8;
		return 10;
	}

	// One squad member as seen through public state. Liveness is data, not
	// a gate: the dead position-0 holder is the vacancy signal.
	struct VacancyMember
	{
		int userId = -1;
		int team = -1;
		int squad = -1;
		int position = -1;
		bool positionKnown = false;
		bool connected = false;
		bool alive = false;
		bool isEngineer = false;
		// Controlled means an RCBot we drive (as opposed to human or
		// uncontrolled). Only controlled members participate.
		bool controlled = false;
	};

	enum class VacancyEligibility : std::uint8_t
	{
		ELIGIBLE = 0,
		LIVING_LEADER,
		NO_POSITION_ZERO,
		LEADER_ONLY_DEAD,
		INSUFFICIENT_BALANCE,
		BALANCE_UNKNOWN,
		NO_ENGINEER,
		UNCONTROLLED_LEADER
	};

	inline const char* VacancyEligibilityName(const VacancyEligibility value)
	{
		switch (value)
		{
		case VacancyEligibility::ELIGIBLE: return "ELIGIBLE";
		case VacancyEligibility::LIVING_LEADER: return "LIVING_LEADER";
		case VacancyEligibility::NO_POSITION_ZERO:
			return "NO_POSITION_ZERO";
		case VacancyEligibility::LEADER_ONLY_DEAD:
			return "LEADER_ONLY_DEAD";
		case VacancyEligibility::INSUFFICIENT_BALANCE:
			return "INSUFFICIENT_BALANCE";
		case VacancyEligibility::BALANCE_UNKNOWN:
			return "BALANCE_UNKNOWN";
		case VacancyEligibility::NO_ENGINEER: return "NO_ENGINEER";
		case VacancyEligibility::UNCONTROLLED_LEADER:
			return "UNCONTROLLED_LEADER";
		}
		return "NO_ENGINEER";
	}

	struct VacancyEvaluation
	{
		VacancyEligibility eligibility =
			VacancyEligibility::LIVING_LEADER;
		int originalLeaderUserId = -1;
		int engineerUserId = -1;
		int engineerPosition = -1;
		// Expected post-promotion Revive targets: the displaced dead
		// leader (whose flag promotion clears) plus other eligible dead
		// members. The Engineer candidate is never a target.
		int expectedReviveCount = 0;
		int requiredCost = 0;
		int balanceBefore = -1;
	};

	// Evaluates one team's one squad snapshot. Never starts anything; the
	// caller owns vacancy-identity consumption and transaction lifetime.
	// Collection order is irrelevant: selection is deterministic.
	inline VacancyEvaluation EvaluateVacancyRevive(
		const std::vector<VacancyMember>& members, const int team,
		const int squad, const bool balanceKnown, const int balance)
	{
		VacancyEvaluation result;
		const VacancyMember* leader = nullptr;
		for (std::vector<VacancyMember>::const_iterator it =
			members.begin(); it != members.end(); ++it)
		{
			if (!it->connected || it->team != team ||
				it->squad != squad || !it->positionKnown ||
				it->position != 0)
				continue;
			if (leader == nullptr || it->userId < leader->userId)
				leader = &(*it);
		}
		if (leader == nullptr)
		{
			result.eligibility = VacancyEligibility::NO_POSITION_ZERO;
			return result;
		}
		if (leader->alive)
		{
			result.eligibility = VacancyEligibility::LIVING_LEADER;
			return result;
		}
		if (!leader->controlled)
		{
			result.eligibility =
				VacancyEligibility::UNCONTROLLED_LEADER;
			return result;
		}
		result.originalLeaderUserId = leader->userId;

		// Engineer candidate: living, controlled, connected, same squad,
		// known position, not position 0. Lowest position wins; userid
		// breaks ties. No utility scoring.
		const VacancyMember* engineer = nullptr;
		for (std::vector<VacancyMember>::const_iterator it =
			members.begin(); it != members.end(); ++it)
		{
			if (!it->connected || !it->controlled || !it->alive ||
				!it->isEngineer || it->team != team ||
				it->squad != squad || !it->positionKnown ||
				it->position == 0)
				continue;
			if (engineer == nullptr ||
				it->position < engineer->position ||
				(it->position == engineer->position &&
				it->userId < engineer->userId))
				engineer = &(*it);
		}
		if (engineer == nullptr)
		{
			result.eligibility = VacancyEligibility::NO_ENGINEER;
			return result;
		}
		result.engineerUserId = engineer->userId;
		result.engineerPosition = engineer->position;

		// Other eligible dead members: connected, same squad, dead,
		// neither the leader nor the Engineer candidate.
		int otherDead = 0;
		for (std::vector<VacancyMember>::const_iterator it =
			members.begin(); it != members.end(); ++it)
		{
			if (!it->connected || it->alive || it->team != team ||
				it->squad != squad)
				continue;
			if (it->userId == leader->userId ||
				it->userId == engineer->userId)
				continue;
			++otherDead;
		}
		if (otherDead < 1)
		{
			result.eligibility =
				VacancyEligibility::LEADER_ONLY_DEAD;
			return result;
		}
		result.expectedReviveCount = 1 + otherDead;
		result.requiredCost =
			ReviveCostForTargets(result.expectedReviveCount);
		if (!balanceKnown)
		{
			result.eligibility = VacancyEligibility::BALANCE_UNKNOWN;
			return result;
		}
		result.balanceBefore = balance;
		if (balance < result.requiredCost)
		{
			result.eligibility =
				VacancyEligibility::INSUFFICIENT_BALANCE;
			return result;
		}
		result.eligibility = VacancyEligibility::ELIGIBLE;
		return result;
	}

	// Deterministic driver election: exactly the lowest userid among
	// living connected controlled members of the squad drives. Every
	// observer computes the same driver without communication.
	inline bool IsVacancyReviveDriver(const int ownUserId,
		const int lowestLivingControlledMemberUserId)
	{
		return ownUserId > 0 && ownUserId ==
			lowestLivingControlledMemberUserId;
	}

	// Leader-vacancy identity: one intervention per distinct leader
	// death epoch, NOT per squad-goal generation. The epoch is the dead
	// original leader's own CBot recovery life generation
	// (CBot::getRecoveryLifeGeneration, bumped in spawnInit): it is
	// constant while the leader stays dead, advances exactly when the
	// leader returns to life, and never moves for RCBot-side elected
	// leader changes or squad-goal state changes. That is why two
	// genuinely separate deaths stay independent even when the
	// squad-goal generation never moves (notably the steady -1 of
	// goal-less squads), while repeated evaluation of one standing
	// death stays consumed. Epoch 0 means the leader never spawned,
	// hence no vacancy epoch exists.
	struct VacancyIdentity
	{
		int team = -1;
		int squad = -1;
		int originalLeaderUserId = -1;
		unsigned long long leaderLifeEpoch = 0;
	};

	inline bool ValidVacancyIdentity(const VacancyIdentity& identity)
	{
		return identity.team > 0 && identity.squad > 0 &&
			identity.originalLeaderUserId > 0 &&
			identity.leaderLifeEpoch != 0;
	}

	inline bool SameVacancyIdentity(const VacancyIdentity& a,
		const VacancyIdentity& b)
	{
		return a.team == b.team && a.squad == b.squad &&
			a.originalLeaderUserId == b.originalLeaderUserId &&
			a.leaderLifeEpoch == b.leaderLifeEpoch;
	}

	// Bounded consumed-vacancy set. Pure logic, no globals: the live
	// driver owns one instance. Oldest entries fall out once full;
	// capacity only bounds memory, never the identity contract.
	class VacancyConsumedIdentities
	{
	public:
		static const int kCapacity = 8;

		bool IsConsumed(const VacancyIdentity& identity) const
		{
			if (!ValidVacancyIdentity(identity))
				return false;
			for (int i = 0; i < m_used; ++i)
			{
				if (SameVacancyIdentity(m_slots[i], identity))
					return true;
			}
			return false;
		}

		void Consume(const VacancyIdentity& identity)
		{
			if (!ValidVacancyIdentity(identity) ||
				IsConsumed(identity))
				return;
			m_slots[m_next] = identity;
			m_next = (m_next + 1) % kCapacity;
			if (m_used < kCapacity)
				++m_used;
		}

		int Size() const
		{
			return m_used;
		}

	private:
		VacancyIdentity m_slots[kCapacity];
		int m_next = 0;
		int m_used = 0;
	};

	enum class VacancyPhase : std::uint8_t
	{
		IDLE = 0,
		PROMOTING,
		REVIVING,
		RESTORING,
		COMPLETE,
		ABORTED
	};

	inline const char* VacancyPhaseName(const VacancyPhase phase)
	{
		switch (phase)
		{
		case VacancyPhase::IDLE: return "IDLE";
		case VacancyPhase::PROMOTING: return "PROMOTING";
		case VacancyPhase::REVIVING: return "REVIVING";
		case VacancyPhase::RESTORING: return "RESTORING";
		case VacancyPhase::COMPLETE: return "COMPLETE";
		case VacancyPhase::ABORTED: return "ABORTED";
		}
		return "ABORTED";
	}

	// Bounded Revive verification window in driver passes. Revive applies
	// server-side synchronously, so a handful of passes is ample; expiry
	// concludes failure without any wall-clock.
	static const int kVacancyReviveVerifyPasses = 10;

	// Native per-player client-command flood gate, verified in
	// server_srv.so: ClientCommand entry drops any command while
	// curtime < [player+0x1534], and every processed command re-arms
	// [player+0x1534] = curtime + 2.0 (rodata float at 0x950348).
	// The Revive activation therefore closes the Engineer's gate, and a
	// restore issued by the Engineer inside the window is silently
	// eaten. The driver honors the verified 2.0s constant plus margin
	// before requesting restore. This bounds an async native round
	// trip; attempt uniqueness stays state-based.
	static const float kVacancyCommandFloodSettleSeconds = 3.0f;

	// True when a restore may be submitted now. reviveSettledTime is the
	// time Revive spend was OBSERVED (so execution provably happened at
	// or before it); negative means no Revive was issued, hence no gate.
	inline bool RestoreGateOpen(const float now,
		const float reviveSettledTime)
	{
		if (reviveSettledTime < 0.0f)
			return true;
		return now - reviveSettledTime >=
			kVacancyCommandFloodSettleSeconds;
	}

	// Pure phase-step helpers keep the live machine honest and testable.
	// Promotion outcome comes from the actuation primitive's readback.
	inline VacancyPhase PhaseAfterPromotion(const bool confirmed)
	{
		return confirmed ? VacancyPhase::REVIVING :
			VacancyPhase::ABORTED;
	}

	// Spend gate evaluated immediately before activation with ACTUAL
	// current numbers, never the pre-promotion expectation.
	inline bool ShouldSpendRevive(const bool engineerAlive,
		const bool engineerAtZero, const int actualDeadTargets,
		const bool balanceKnown, const int balance)
	{
		if (!engineerAlive || !engineerAtZero)
			return false;
		if (actualDeadTargets < 2)
			return false;
		if (!balanceKnown)
			return false;
		return balance >= ReviveCostForTargets(actualDeadTargets);
	}

	// After the single Revive attempt: restore the original leader only
	// when they are alive; never hand leadership to a corpse while a
	// living leader exists.
	inline bool ShouldRestoreLeader(const bool originalAlive)
	{
		return originalAlive;
	}
}

#endif // __RCBOT_EMPIRES_VACANCY_REVIVE_H__
