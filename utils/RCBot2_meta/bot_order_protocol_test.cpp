#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
#include <vector>

#include "bot_order_protocol.h"

using namespace RCBotOrder;

static OrderSpec MakeSpec(const OrderId orderId = 1, const Generation generation = 1)
{
	return {
		orderId,
		generation,
		10,
		OrderRecipient::Bot(20),
		OrderType::MOVE,
		{ 100.0f, 200.0f, 300.0f },
		100.0,
		200.0
	};
}

static OrderKey Key(const OrderSpec& spec)
{
	return { spec.orderId, spec.generation };
}

static void AssertInvariant(const UpdateResult& result)
{
	if (!result.hasOrder)
		return;

	assert(result.snapshot.valid);
	if (result.snapshot.state == OrderState::TERMINAL)
		assert(result.snapshot.outcome != OrderOutcome::NONE);
	else
		assert(result.snapshot.outcome == OrderOutcome::NONE);
}

static void AssertUnchanged(const OrderRecord& before, const OrderRecord& after)
{
	assert(before.valid == after.valid);
	assert(before.orderId == after.orderId);
	assert(before.generation == after.generation);
	assert(before.issuerId == after.issuerId);
	assert(before.recipient == after.recipient);
	assert(before.type == after.type);
	assert(before.target.x == after.target.x);
	assert(before.target.y == after.target.y);
	assert(before.target.z == after.target.z);
	assert(before.state == after.state);
	assert(before.outcome == after.outcome);
	assert(before.detailReason == after.detailReason);
	assert(before.issuedAt == after.issuedAt);
	assert(before.deadline == after.deadline);
}

static bool SameRecord(const OrderRecord& left, const OrderRecord& right)
{
	return left.valid == right.valid && left.orderId == right.orderId &&
		left.generation == right.generation && left.issuerId == right.issuerId &&
		left.recipient == right.recipient && left.type == right.type &&
		left.target.x == right.target.x && left.target.y == right.target.y &&
		left.target.z == right.target.z && left.state == right.state &&
		left.outcome == right.outcome && left.detailReason == right.detailReason &&
		left.issuedAt == right.issuedAt && left.deadline == right.deadline;
}

static bool SameResult(const UpdateResult& left, const UpdateResult& right)
{
	const OrderTransition& leftTransition = left.transition;
	const OrderTransition& rightTransition = right.transition;
	const bool sameTransition =
		leftTransition.valid == rightTransition.valid &&
		leftTransition.orderId == rightTransition.orderId &&
		leftTransition.generation == rightTransition.generation &&
		leftTransition.issuerId == rightTransition.issuerId &&
		leftTransition.recipient == rightTransition.recipient &&
		leftTransition.orderType == rightTransition.orderType &&
		leftTransition.target.x == rightTransition.target.x &&
		leftTransition.target.y == rightTransition.target.y &&
		leftTransition.target.z == rightTransition.target.z &&
		leftTransition.operation == rightTransition.operation &&
		leftTransition.hasUpdateKey == rightTransition.hasUpdateKey &&
		leftTransition.updateKey.orderId == rightTransition.updateKey.orderId &&
		leftTransition.updateKey.generation == rightTransition.updateKey.generation &&
		leftTransition.previousHasOrder == rightTransition.previousHasOrder &&
		leftTransition.previousState == rightTransition.previousState &&
		leftTransition.previousOutcome == rightTransition.previousOutcome &&
		leftTransition.newHasOrder == rightTransition.newHasOrder &&
		leftTransition.newState == rightTransition.newState &&
		leftTransition.newOutcome == rightTransition.newOutcome &&
		leftTransition.detailReason == rightTransition.detailReason &&
		leftTransition.updateResult == rightTransition.updateResult &&
		leftTransition.mutationApplied == rightTransition.mutationApplied &&
		leftTransition.timestamp == rightTransition.timestamp;
	return left.status == right.status && left.changed == right.changed &&
		left.hasOrder == right.hasOrder && left.rejectionReason == right.rejectionReason &&
		SameRecord(left.snapshot, right.snapshot) && sameTransition;
}

static void PrintPass(const char* name)
{
	std::cout << name << " PASS" << std::endl;
}

int main()
{
	// ORDER-01: the only successful lifecycle is strictly forward.
	{
		const OrderSpec spec = MakeSpec();
		const OrderKey key = Key(spec);
		OrderProtocol protocol;
		UpdateResult result = protocol.Issue(spec);
		AssertInvariant(result);
		assert(result.status == UpdateStatus::APPLIED);
		assert(result.snapshot.state == OrderState::ISSUED);
		result = protocol.Acknowledge(key, 101.0);
		AssertInvariant(result);
		assert(result.status == UpdateStatus::APPLIED);
		result = protocol.BeginPlanning(key, 102.0);
		AssertInvariant(result);
		assert(result.status == UpdateStatus::APPLIED);
		result = protocol.BeginMoving(key, 103.0);
		AssertInvariant(result);
		assert(result.status == UpdateStatus::APPLIED);
		result = protocol.BeginExecuting(key, 104.0);
		AssertInvariant(result);
		assert(result.status == UpdateStatus::APPLIED);
		result = protocol.Complete(key, 105.0);
		AssertInvariant(result);
		assert(result.status == UpdateStatus::APPLIED);
		assert(result.snapshot.state == OrderState::TERMINAL);
		assert(result.snapshot.outcome == OrderOutcome::COMPLETE);
		PrintPass("ORDER-01");
	}

	// ORDER-02: an invalid MOVE becomes a terminal INVALID order immediately.
	{
		OrderSpec spec = MakeSpec(2, 1);
		spec.target.x = std::numeric_limits<float>::quiet_NaN();
		OrderProtocol protocol;
		const UpdateResult result = protocol.Issue(spec);
		AssertInvariant(result);
		assert(result.status == UpdateStatus::APPLIED);
		assert(result.snapshot.state == OrderState::TERMINAL);
		assert(result.snapshot.outcome == OrderOutcome::INVALID);
		assert(result.snapshot.detailReason == DetailReason::NON_FINITE_TARGET);

		OrderProtocol invalidIdProtocol;
		const UpdateResult invalidId = invalidIdProtocol.Issue(MakeSpec(0, 1));
		AssertInvariant(invalidId);
		assert(invalidId.snapshot.outcome == OrderOutcome::INVALID);
		assert(invalidId.snapshot.detailReason == DetailReason::INVALID_ORDER_ID);

		OrderProtocol invalidGenerationProtocol;
		const UpdateResult invalidGeneration =
			invalidGenerationProtocol.Issue(MakeSpec(12, 0));
		AssertInvariant(invalidGeneration);
		assert(invalidGeneration.snapshot.outcome == OrderOutcome::INVALID);
		assert(invalidGeneration.snapshot.detailReason == DetailReason::INVALID_GENERATION);

		OrderSpec invalidDeadlineSpec = MakeSpec(13, 1);
		invalidDeadlineSpec.deadline = invalidDeadlineSpec.issuedAt;
		OrderProtocol invalidDeadlineProtocol;
		const UpdateResult invalidDeadline = invalidDeadlineProtocol.Issue(invalidDeadlineSpec);
		AssertInvariant(invalidDeadline);
		assert(invalidDeadline.snapshot.outcome == OrderOutcome::INVALID);
		assert(invalidDeadline.snapshot.detailReason == DetailReason::INVALID_DEADLINE);
		PrintPass("ORDER-02");
	}

	// ORDER-03: every non-abort failure outcome is terminal and non-NONE.
	{
		const std::vector<OrderOutcome> failures = {
			OrderOutcome::BLOCKED,
			OrderOutcome::STUCK,
			OrderOutcome::TARGET_LOST,
			OrderOutcome::INVALID,
			OrderOutcome::NEEDS_SUPPORT,
			OrderOutcome::UNREACHABLE
		};
		OrderId orderId = 30;
		for (const OrderOutcome outcome : failures)
		{
			const OrderSpec spec = MakeSpec(orderId++, 1);
			OrderProtocol protocol;
			const UpdateResult issued = protocol.Issue(spec);
			AssertInvariant(issued);
			const UpdateResult failed = protocol.Fail(Key(spec), outcome,
				DetailReason::FAILURE_REPORTED, 101.0);
			AssertInvariant(failed);
			assert(failed.status == UpdateStatus::APPLIED);
			assert(failed.snapshot.state == OrderState::TERMINAL);
			assert(failed.snapshot.outcome == outcome);
		}
		PrintPass("ORDER-03");
	}

	// ORDER-04: replacement aborts only the old instance; the new one is separate.
	{
		const OrderSpec oldSpec = MakeSpec(4, 1);
		const OrderSpec newSpec = MakeSpec(5, 2);
		OrderProtocol oldProtocol;
		assert(oldProtocol.Issue(oldSpec).status == UpdateStatus::APPLIED);
		const UpdateResult replaced = oldProtocol.Replace(Key(oldSpec), 101.0);
		AssertInvariant(replaced);
		assert(replaced.snapshot.outcome == OrderOutcome::ABORTED);
		assert(replaced.snapshot.detailReason == DetailReason::REPLACED);
		const OrderRecord oldSnapshot = replaced.snapshot;

		OrderProtocol newProtocol;
		const UpdateResult issued = newProtocol.Issue(newSpec);
		AssertInvariant(issued);
		assert(issued.snapshot.orderId != oldSnapshot.orderId);
		assert(issued.snapshot.generation > oldSnapshot.generation);
		AssertUnchanged(oldSnapshot, oldProtocol.Snapshot());
		PrintPass("ORDER-04");
	}

	// ORDER-05: a stale ID/generation is rejected without mutation.
	{
		const OrderSpec spec = MakeSpec(6, 3);
		OrderProtocol protocol;
		assert(protocol.Issue(spec).status == UpdateStatus::APPLIED);
		const OrderRecord before = protocol.Snapshot();
		const UpdateResult stale = protocol.BeginPlanning(
			{ spec.orderId, spec.generation + 1 }, 101.0);
		AssertInvariant(stale);
		assert(stale.status == UpdateStatus::STALE);
		assert(stale.rejectionReason == DetailReason::STALE_CONTEXT);
		AssertUnchanged(before, stale.snapshot);
		PrintPass("ORDER-05");
	}

	// ORDER-06: duplicate terminal updates cannot mutate a completed order.
	{
		const OrderSpec spec = MakeSpec(7, 1);
		OrderProtocol protocol;
		assert(protocol.Issue(spec).status == UpdateStatus::APPLIED);
		assert(protocol.Acknowledge(Key(spec), 101.0).status == UpdateStatus::APPLIED);
		assert(protocol.BeginPlanning(Key(spec), 102.0).status == UpdateStatus::APPLIED);
		assert(protocol.BeginMoving(Key(spec), 103.0).status == UpdateStatus::APPLIED);
		assert(protocol.BeginExecuting(Key(spec), 104.0).status == UpdateStatus::APPLIED);
		const UpdateResult completed = protocol.Complete(Key(spec), 105.0);
		const UpdateResult duplicate = protocol.Complete(Key(spec), 106.0);
		AssertInvariant(duplicate);
		assert(duplicate.status == UpdateStatus::ALREADY_TERMINAL);
		AssertUnchanged(completed.snapshot, duplicate.snapshot);
		PrintPass("ORDER-06");
	}

	// ORDER-07: the bounded lifetime aborts only after the deadline is exceeded.
	{
		OrderSpec spec = MakeSpec(8, 1);
		spec.issuedAt = 10.0;
		spec.deadline = 20.0;
		OrderProtocol protocol;
		assert(protocol.Issue(spec).status == UpdateStatus::APPLIED);
		const UpdateResult atDeadline = protocol.Tick(Key(spec), 20.0);
		AssertInvariant(atDeadline);
		assert(atDeadline.status == UpdateStatus::APPLIED);
		assert(atDeadline.snapshot.state != OrderState::TERMINAL);
		const UpdateResult expired = protocol.Tick(Key(spec), 20.001);
		AssertInvariant(expired);
		assert(expired.status == UpdateStatus::APPLIED);
		assert(expired.snapshot.state == OrderState::TERMINAL);
		assert(expired.snapshot.outcome == OrderOutcome::ABORTED);
		assert(expired.snapshot.detailReason == DetailReason::DEADLINE_EXPIRED);
		PrintPass("ORDER-07");
	}

	// ORDER-08: no transition is accepted after TERMINAL.
	{
		const OrderSpec spec = MakeSpec(9, 1);
		OrderProtocol protocol;
		assert(protocol.Issue(spec).status == UpdateStatus::APPLIED);
		assert(protocol.Abort(Key(spec), DetailReason::EXPLICIT_ABORT, 101.0).status ==
			UpdateStatus::APPLIED);
		const OrderRecord before = protocol.Snapshot();
		const UpdateResult rejected = protocol.BeginPlanning(Key(spec), 102.0);
		AssertInvariant(rejected);
		assert(rejected.status == UpdateStatus::ALREADY_TERMINAL);
		AssertUnchanged(before, rejected.snapshot);
		PrintPass("ORDER-08");
	}

	// ORDER-09: skipping a lifecycle step is rejected without mutation.
	{
		const OrderSpec spec = MakeSpec(10, 1);
		OrderProtocol protocol;
		assert(protocol.Issue(spec).status == UpdateStatus::APPLIED);
		const OrderRecord before = protocol.Snapshot();
		const UpdateResult illegal = protocol.BeginMoving(Key(spec), 101.0);
		AssertInvariant(illegal);
		assert(illegal.status == UpdateStatus::ILLEGAL_TRANSITION);
		AssertUnchanged(before, illegal.snapshot);
		PrintPass("ORDER-09");
	}

	// ORDER-10: equal input and explicit updates produce equal results.
	{
		const OrderSpec spec = MakeSpec(11, 4);
		OrderProtocol left;
		OrderProtocol right;
		const std::vector<UpdateResult> leftResults = {
			left.Issue(spec),
			left.Acknowledge(Key(spec), 101.0),
			left.BeginPlanning(Key(spec), 102.0),
			left.BeginMoving(Key(spec), 103.0),
			left.BeginExecuting(Key(spec), 104.0),
			left.Complete(Key(spec), 105.0)
		};
		const std::vector<UpdateResult> rightResults = {
			right.Issue(spec),
			right.Acknowledge(Key(spec), 101.0),
			right.BeginPlanning(Key(spec), 102.0),
			right.BeginMoving(Key(spec), 103.0),
			right.BeginExecuting(Key(spec), 104.0),
			right.Complete(Key(spec), 105.0)
		};
		assert(leftResults.size() == rightResults.size());
		for (std::size_t i = 0; i < leftResults.size(); ++i)
		{
			AssertInvariant(leftResults[i]);
			AssertInvariant(rightResults[i]);
			assert(SameResult(leftResults[i], rightResults[i]));
		}
		PrintPass("ORDER-10");
	}

	std::cout << "RESULT 10/10 PASS" << std::endl;
	return 0;
}
