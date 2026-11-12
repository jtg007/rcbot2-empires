#ifndef __RCBOT_EMPIRES_LEADER_ACTUATION_H__
#define __RCBOT_EMPIRES_LEADER_ACTUATION_H__

#include <cstdint>

namespace RCBotEmpiresLeadActuation
{
	// Request lifecycle. Command submission is never success; only a later
	// public-state readback can confirm. A silent native no-op (for example a
	// rate-limited emp_make_lead) stays distinguishable as FAILED, never
	// silently CONFIRMED.
	enum class LeadActuationStatus : std::uint8_t
	{
		NOT_REQUESTED = 0,
		REQUESTED,
		CONFIRMED,
		FAILED,
		INVALIDATED
	};

	inline const char* LeadActuationStatusName(const LeadActuationStatus status)
	{
		switch (status)
		{
		case LeadActuationStatus::NOT_REQUESTED: return "NOT_REQUESTED";
		case LeadActuationStatus::REQUESTED: return "REQUESTED";
		case LeadActuationStatus::CONFIRMED: return "CONFIRMED";
		case LeadActuationStatus::FAILED: return "FAILED";
		case LeadActuationStatus::INVALIDATED: return "INVALIDATED";
		}
		return "INVALIDATED";
	}

	// Why a request was rejected before submission. Distinct codes keep the
	// userid/entity-index confusion class explicitly testable.
	enum class LeadActuationRejectReason : std::uint8_t
	{
		NONE = 0,
		BAD_ISSUER_IDENTITY,
		BAD_TARGET_IDENTITY,
		SELF_PROMOTION,
		BAD_TEAM,
		TEAM_MISMATCH,
		BAD_SQUAD,
		SQUAD_MISMATCH,
		ISSUER_NOT_LEADER_POSITION
	};

	inline const char* LeadActuationRejectReasonName(
		const LeadActuationRejectReason reason)
	{
		switch (reason)
		{
		case LeadActuationRejectReason::NONE: return "NONE";
		case LeadActuationRejectReason::BAD_ISSUER_IDENTITY:
			return "BAD_ISSUER_IDENTITY";
		case LeadActuationRejectReason::BAD_TARGET_IDENTITY:
			return "BAD_TARGET_IDENTITY";
		case LeadActuationRejectReason::SELF_PROMOTION:
			return "SELF_PROMOTION";
		case LeadActuationRejectReason::BAD_TEAM: return "BAD_TEAM";
		case LeadActuationRejectReason::TEAM_MISMATCH: return "TEAM_MISMATCH";
		case LeadActuationRejectReason::BAD_SQUAD: return "BAD_SQUAD";
		case LeadActuationRejectReason::SQUAD_MISMATCH:
			return "SQUAD_MISMATCH";
		case LeadActuationRejectReason::ISSUER_NOT_LEADER_POSITION:
			return "ISSUER_NOT_LEADER_POSITION";
		}
		return "BAD_ISSUER_IDENTITY";
	}

	// Team ids mirror EMP_TEAM_NF/EMP_TEAM_BE without depending on the
	// engine-bound bot headers, so this unit stays headless.
	static const int kEmpiresTeamNf = 2;
	static const int kEmpiresTeamBe = 3;

	// One side of a leader change. userid identifies the player for humans;
	// entIndex is the server entity index emp_make_lead requires. Both must
	// be positive; they are never interchangeable.
	struct LeadActuationIdentity
	{
		int userId = -1;
		int entIndex = -1;

		bool IsValid() const
		{
			return userId > 0 && entIndex > 0;
		}
	};

	// Observed public state of one party. Liveness is deliberately absent:
	// dead-leader issuance is a required supported case, so alive never
	// gates. Only connected/team/squad/position/xp are tracked.
	struct LeadActuationParty
	{
		LeadActuationIdentity identity;
		bool connected = false;
		int team = -1;
		int squad = -1;
		int position = -1;
		bool positionKnown = false;
		int squadXp = -1;
		bool squadXpKnown = false;
	};

	struct LeadActuationRequest
	{
		LeadActuationParty issuer;
		LeadActuationParty target;
		// Caller-supplied context token (for example the squad-goal
		// generation). Zero means untracked: staleness then rests only on
		// connection/team/squad observation.
		int contextGeneration = 0;
		LeadActuationStatus status = LeadActuationStatus::NOT_REQUESTED;
	};

	inline bool IsEmpiresPlayTeam(const int team)
	{
		return team == kEmpiresTeamNf || team == kEmpiresTeamBe;
	}

	// Pre-submission gate. Checks identities, distinctness, teams, squads,
	// and that the issuer currently reads as position 0 (the expected
	// native-leader context). Contains no tactical policy.
	inline bool ValidateLeadActuationParties(const LeadActuationParty& issuer,
		const LeadActuationParty& target, LeadActuationRejectReason& reason)
	{
		if (!issuer.identity.IsValid())
		{
			reason = LeadActuationRejectReason::BAD_ISSUER_IDENTITY;
			return false;
		}
		if (!target.identity.IsValid())
		{
			reason = LeadActuationRejectReason::BAD_TARGET_IDENTITY;
			return false;
		}
		if (issuer.identity.userId == target.identity.userId ||
			issuer.identity.entIndex == target.identity.entIndex)
		{
			reason = LeadActuationRejectReason::SELF_PROMOTION;
			return false;
		}
		if (!IsEmpiresPlayTeam(issuer.team) ||
			!IsEmpiresPlayTeam(target.team))
		{
			reason = LeadActuationRejectReason::BAD_TEAM;
			return false;
		}
		if (issuer.team != target.team)
		{
			reason = LeadActuationRejectReason::TEAM_MISMATCH;
			return false;
		}
		if (issuer.squad <= 0 || target.squad <= 0)
		{
			reason = LeadActuationRejectReason::BAD_SQUAD;
			return false;
		}
		if (issuer.squad != target.squad)
		{
			reason = LeadActuationRejectReason::SQUAD_MISMATCH;
			return false;
		}
		if (!issuer.positionKnown || issuer.position != 0)
		{
			reason =
				LeadActuationRejectReason::ISSUER_NOT_LEADER_POSITION;
			return false;
		}
		reason = LeadActuationRejectReason::NONE;
		return true;
	}

	// A pending request dies (INVALIDATED, never retried silently) when the
	// observed context no longer matches the request: disconnect, team
	// change, or a genuine squad leave/change. Transient unreadable
	// fields (squad < 0, unknown positions) stay pending instead, so a
	// single missed readback can never kill or fail a request.
	// Wall-clock heuristics are deliberately not used.
	inline bool IsLeadActuationRequestStale(const LeadActuationRequest& request,
		const LeadActuationParty& issuerNow,
		const LeadActuationParty& targetNow, const int currentGeneration)
	{
		if (!issuerNow.connected || !targetNow.connected)
			return true;
		if (issuerNow.team != request.issuer.team ||
			targetNow.team != request.target.team)
			return true;
		if (issuerNow.squad >= 0 && targetNow.squad >= 0 &&
			(issuerNow.squad != request.issuer.squad ||
			targetNow.squad != request.target.squad))
			return true;
		if (request.contextGeneration != 0 &&
			currentGeneration != request.contextGeneration)
			return true;
		return false;
	}

	// Poll settle window in seconds. The native command needs on the
	// order of a second to execute after helpers->ClientCommand queues
	// it, while readback polls can burst many times per second, so a
	// poll-count window would be frame-rate fragile. Fully known but
	// unmoved positions inside the window mean "not yet processed",
	// never "rejected". Only a settled unmoved readback is a silent
	// native no-op (FAILED). This bounds an async round trip; all
	// policy correctness (one-shot attempts, generation tokens) stays
	// state-based, and true no-ops still resolve FAILED after expiry.
	static const float kLeadActuationSettleSeconds = 5.0f;

	// Staged readback poll. CONFIRMED requires the exact native swap
	// observable through public state: target at position 0, issuer at the
	// target's previous position, squads unchanged, XP unchanged. Returns
	// REQUESTED while any field is transiently unreadable or the settle
	// window has not elapsed; FAILED only for a settled mismatch.
	inline LeadActuationStatus PollLeadActuationRequest(
		const LeadActuationRequest& request,
		const LeadActuationParty& issuerNow,
		const LeadActuationParty& targetNow,
		const float elapsedSeconds)
	{
		if (!issuerNow.connected || !targetNow.connected)
			return LeadActuationStatus::INVALIDATED;
		if (issuerNow.squad < 0 || targetNow.squad < 0)
			return LeadActuationStatus::REQUESTED;
		if (issuerNow.squad != request.issuer.squad ||
			targetNow.squad != request.target.squad)
			return LeadActuationStatus::INVALIDATED;
		if (!issuerNow.positionKnown || !targetNow.positionKnown)
			return LeadActuationStatus::REQUESTED;
		if (targetNow.position == 0 &&
			issuerNow.position == request.target.position)
		{
			if (request.issuer.squadXpKnown ||
				request.target.squadXpKnown)
			{
				if (!issuerNow.squadXpKnown ||
					!targetNow.squadXpKnown)
					return LeadActuationStatus::REQUESTED;
				if (issuerNow.squadXp != request.issuer.squadXp ||
					targetNow.squadXp !=
					request.target.squadXp)
					return LeadActuationStatus::FAILED;
			}
			return LeadActuationStatus::CONFIRMED;
		}
		if (elapsedSeconds < kLeadActuationSettleSeconds)
			return LeadActuationStatus::REQUESTED;
		return LeadActuationStatus::FAILED;
	}

	// Strict final interpretation of a fully observed readback. Unknown
	// fields stay REQUESTED (pending), never guessed.
	inline LeadActuationStatus InterpretLeadActuationReadback(
		const LeadActuationRequest& request,
		const LeadActuationParty& issuerNow,
		const LeadActuationParty& targetNow)
	{
		return PollLeadActuationRequest(request, issuerNow, targetNow,
			kLeadActuationSettleSeconds);
	}
}

#endif // __RCBOT_EMPIRES_LEADER_ACTUATION_H__
