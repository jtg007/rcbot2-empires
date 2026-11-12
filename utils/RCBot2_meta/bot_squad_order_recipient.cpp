#include "bot_squad_order_recipient.h"

#include <algorithm>

namespace RCBotSquadOrder
{
namespace
{
std::vector<RecipientId> CanonicalMembers(
	const std::vector<RecipientId>& members)
{
	std::vector<RecipientId> result;
	for (const RecipientId member : members)
	{
		if (member != 0)
			result.push_back(member);
	}
	std::sort(result.begin(), result.end());
	result.erase(std::unique(result.begin(), result.end()), result.end());
	return result;
}

bool SameMembers(const std::vector<RecipientId>& left,
	const std::vector<RecipientId>& right)
{
	return left == right;
}

bool IsFailure(const OrderOutcome outcome)
{
	return outcome != OrderOutcome::NONE &&
		outcome != OrderOutcome::COMPLETE &&
		outcome != OrderOutcome::ABORTED;
}
}

SquadMembershipObservation SquadIdentityTracker::Observe(
	const RecipientId squadId, const std::vector<RecipientId>& members)
{
	SquadMembershipObservation observation;
	observation.members = CanonicalMembers(members);
	if (squadId == 0)
		return observation;

	auto entry = m_entries.find(squadId);
	if (entry == m_entries.end())
	{
		Generation& next = m_nextGeneration[squadId];
		if (next == 0)
			next = 1;
		entry = m_entries.emplace(squadId,
			Entry{ next, observation.members }).first;
		observation.membershipChanged = true;
	}
	else
	{
		observation.membershipChanged =
			!SameMembers(entry->second.members, observation.members);
		entry->second.members = observation.members;
	}

	observation.recipient = OrderRecipient::Squad(squadId,
		entry->second.generation);
	return observation;
}

bool SquadIdentityTracker::Forget(const RecipientId squadId)
{
	const bool removed = m_entries.erase(squadId) != 0;
	if (removed)
		++m_nextGeneration[squadId];
	return removed;
}

bool SquadIdentityTracker::Current(const RecipientId squadId,
	SquadMembershipObservation& observation) const
{
	const auto entry = m_entries.find(squadId);
	if (entry == m_entries.end())
		return false;
	observation.recipient = OrderRecipient::Squad(squadId,
		entry->second.generation);
	observation.members = entry->second.members;
	observation.membershipChanged = false;
	return true;
}

bool SquadMembershipSnapshot::IsValid() const
{
	if (recipient.kind != RCBotOrder::RecipientKind::SQUAD ||
		!recipient.IsValid())
		return false;
	const std::vector<RecipientId> canonical = CanonicalMembers(members);
	return !canonical.empty() && canonical.size() == members.size();
}

bool SquadMembershipSnapshot::Contains(const RecipientId memberId) const
{
	return std::find(members.begin(), members.end(), memberId) != members.end();
}

SquadMoveRecipient::SquadMoveRecipient(
	const NavigationIntentStart start, const NavigationIntentCancel cancel,
	const TelemetrySink telemetry, void* const context)
	: m_navigationStart(start), m_navigationCancel(cancel),
	  m_telemetry(telemetry), m_context(context)
{
}

void SquadMoveRecipient::SetNavigationSinks(
	const NavigationIntentStart start, const NavigationIntentCancel cancel)
{
	m_navigationStart = start;
	m_navigationCancel = cancel;
}

void SquadMoveRecipient::SetTelemetrySink(const TelemetrySink sink,
	void* const context)
{
	m_telemetry = sink;
	m_context = context;
}

void SquadMoveRecipient::Reset()
{
	m_parent = RCBotOrder::OrderProtocol();
	m_parentKey = OrderKey();
	m_squadRecipient = OrderRecipient();
	m_target = TargetPosition();
	m_active = false;
	m_children.clear();
	m_retiredChildren.clear();
	m_aggregate = AggregateSnapshot();
	m_nextChildId = 1;
	m_nextChildGeneration = 1;
	m_nextNavigationIntentId = 1;
}

bool SquadMoveRecipient::IssueMove(const SquadMoveSpec& spec)
{
	if (!BeginMove(spec))
		return false;
	std::vector<ChildMovementTarget> targets;
	targets.reserve(m_memberSnapshot.size());
	std::uint64_t slotId = 1;
	for (const RecipientId memberId : m_memberSnapshot)
	{
		ChildMovementTarget target;
		target.memberId = memberId;
		target.hasTarget = true;
		target.target = spec.target;
		target.slotId = slotId;
		target.squadGeneration = m_squadRecipient.generation;
		target.decisionRevision = 1;
		target.formationGeneration = 1;
		target.claimResourceKind = 3;
		target.claimResourceId = slotId;
		target.claimResourceGeneration = 1;
		targets.push_back(target);
		++slotId;
	}
	return DispatchChildren(m_parentKey, targets, spec.issuedAt);
}

bool SquadMoveRecipient::BeginMove(const SquadMoveSpec& spec)
{
	if (!spec.snapshot.IsValid())
		return false;
	if (m_active)
	{
		const RCBotOrder::UpdateResult replaced =
			m_parent.Replace(m_parentKey, spec.issuedAt);
		Emit(replaced);
		RetireActiveChildren();
		ClearActive();
	}
	// OrderProtocol instances are immutable after terminalization.  A new
	// parent therefore always starts in a fresh protocol instance.
	m_parent = RCBotOrder::OrderProtocol();

	const RCBotOrder::OrderSpec parentSpec = {
		spec.orderId,
		spec.generation,
		spec.issuerId,
		spec.snapshot.recipient,
		RCBotOrder::OrderType::MOVE,
		spec.target,
		spec.issuedAt,
		spec.deadline
	};
	const RCBotOrder::UpdateResult issued = m_parent.Issue(parentSpec);
	Emit(issued);
	if (issued.status != RCBotOrder::UpdateStatus::APPLIED ||
		!issued.snapshot.valid ||
		RCBotOrder::IsTerminal(issued.snapshot.state))
	{
		m_aggregate.terminal = true;
		m_aggregate.outcome = issued.snapshot.outcome;
		return false;
	}

	m_parentKey = { issued.snapshot.orderId, issued.snapshot.generation };
	m_squadRecipient = issued.snapshot.recipient;
	m_target = spec.target;
	m_active = true;
	m_children.clear();
	m_memberSnapshot = CanonicalMembers(spec.snapshot.members);
	m_dispatched = false;
	m_aggregate = AggregateSnapshot();

	const RCBotOrder::UpdateResult acknowledged =
		m_parent.Acknowledge(m_parentKey, spec.issuedAt);
	Emit(acknowledged);
	const RCBotOrder::UpdateResult planning =
		m_parent.BeginPlanning(m_parentKey, spec.issuedAt);
	Emit(planning);
	const RCBotOrder::UpdateResult moving =
		m_parent.BeginMoving(m_parentKey, spec.issuedAt);
	Emit(moving);
	const RCBotOrder::UpdateResult executing =
		m_parent.BeginExecuting(m_parentKey, spec.issuedAt);
	Emit(executing);
	return true;
}


bool SquadMoveRecipient::DispatchChildren(const OrderKey& parentOrder,
	const std::vector<ChildMovementTarget>& targets, const double now)
{
	if (!IsCurrentParent(parentOrder) || m_dispatched ||
		targets.size() != m_memberSnapshot.size())
		return false;
	std::vector<ChildMovementTarget> canonical;
	canonical.reserve(targets.size());
	for (const RecipientId memberId : m_memberSnapshot)
	{
		const auto it = std::find_if(targets.begin(), targets.end(),
			[memberId](const ChildMovementTarget& target) {
				return target.memberId == memberId;
			});
		if (it == targets.end() ||
			std::count_if(targets.begin(), targets.end(),
				[memberId](const ChildMovementTarget& target) {
					return target.memberId == memberId;
				}) != 1)
			return false;
		if (it->hasTarget && (!it->target.IsFinite() || it->slotId == 0 ||
			it->squadGeneration == 0 || it->decisionRevision == 0 ||
			it->formationGeneration == 0 || it->claimResourceKind != 3 ||
			it->claimResourceId != it->slotId ||
			it->claimResourceGeneration != it->formationGeneration))
			return false;
		canonical.push_back(*it);
	}
	for (const ChildMovementTarget& target : targets)
		if (!std::binary_search(m_memberSnapshot.begin(), m_memberSnapshot.end(),
			target.memberId))
			return false;

	m_dispatched = true;
	for (const ChildMovementTarget& target : canonical)
	{
		ChildExecution child;
		if (!CreateChild(parentOrder, target, now, child))
			continue;
		m_children.push_back(child);
	}

	RefreshAggregate();
	AggregateIfReady(now);
	return true;
}

bool SquadMoveRecipient::CreateChild(const OrderKey& parentOrder,
	const ChildMovementTarget& target, const double now, ChildExecution& child)
{
	const RCBotOrder::OrderRecord parentRecord = m_parent.Snapshot();
	child.parentOrder = parentOrder;
	child.memberId = target.memberId;
	child.key = { AllocateChildId(), AllocateChildGeneration() };
	child.hasMovementTarget = target.hasTarget;
	child.movementTarget = target.target;
	child.slotId = target.slotId;
	child.squadGeneration = target.squadGeneration;
	child.decisionRevision = target.decisionRevision;
	child.formationGeneration = target.formationGeneration;
	child.claimResourceKind = target.claimResourceKind;
	child.claimResourceId = target.claimResourceId;
	child.claimResourceGeneration = target.claimResourceGeneration;
	const TargetPosition childTarget = target.hasTarget ? target.target : m_target;
	const RCBotOrder::OrderSpec childSpec = {
		child.key.orderId,
		child.key.generation,
		parentRecord.issuerId,
		OrderRecipient::Bot(target.memberId),
		RCBotOrder::OrderType::MOVE,
		childTarget,
		now,
		parentRecord.deadline
	};
	const RCBotOrder::UpdateResult issued = child.protocol.Issue(childSpec);
	child.record = issued.snapshot;
	EmitChild(child, issued);
	if (issued.status != RCBotOrder::UpdateStatus::APPLIED ||
		!issued.snapshot.valid || RCBotOrder::IsTerminal(issued.snapshot.state))
		return false;
	const RCBotOrder::UpdateResult acknowledged =
		child.protocol.Acknowledge(child.key, now);
	child.record = acknowledged.snapshot;
	EmitChild(child, acknowledged);
	const RCBotOrder::UpdateResult planning =
		child.protocol.BeginPlanning(child.key, now);
	child.record = planning.snapshot;
	EmitChild(child, planning);
	const RCBotOrder::UpdateResult moving =
		child.protocol.BeginMoving(child.key, now);
	child.record = moving.snapshot;
	EmitChild(child, moving);
	if (!target.hasTarget)
	{
		const RCBotOrder::UpdateResult excluded = child.protocol.Abort(
			child.key, RCBotOrder::DetailReason::DEATH_OR_LIFECYCLE, now);
		child.record = excluded.snapshot;
		EmitChild(child, excluded);
		return true;
	}
	(void)StartNavigation(child, now);
	return true;
}

bool SquadMoveRecipient::StartNavigation(ChildExecution& child,
	const double now)
{
	NavigationIntentRequest request;
	request.parentOrder = child.parentOrder;
	request.childExecution = child.key;
	request.memberId = child.memberId;
	request.target = child.movementTarget;
	request.slotId = child.slotId;
	request.squadGeneration = child.squadGeneration;
	request.decisionRevision = child.decisionRevision;
	request.formationGeneration = child.formationGeneration;
	request.claimResourceKind = child.claimResourceKind;
	request.claimResourceId = child.claimResourceId;
	request.claimResourceGeneration = child.claimResourceGeneration;
	std::uint64_t navigationIntentId = 0;
	bool started = true;
	if (m_navigationStart != nullptr)
		started = m_navigationStart(m_context, request, &navigationIntentId);
	else
		navigationIntentId = m_nextNavigationIntentId++;
	if (!started || navigationIntentId == 0)
	{
		const RCBotOrder::UpdateResult failed = child.protocol.Fail(child.key,
			OrderOutcome::BLOCKED,
			RCBotOrder::DetailReason::FAILURE_REPORTED, now);
		child.record = failed.snapshot;
		EmitChild(child, failed);
		return false;
	}
	child.navigationRequestToken = navigationIntentId;
	child.navigationIntentId = navigationIntentId;
	child.navigationIntentBound = m_navigationStart == nullptr;
	child.navigationRequested = true;
	const RCBotOrder::UpdateResult executing =
		child.protocol.BeginExecuting(child.key, now);
	child.record = executing.snapshot;
	EmitChild(child, executing);
	return executing.status == RCBotOrder::UpdateStatus::APPLIED;
}

bool SquadMoveRecipient::SameMovementTarget(const ChildExecution& child,
	const ChildMovementTarget& target)
{
	return child.hasMovementTarget == target.hasTarget &&
		(!target.hasTarget || (child.movementTarget.x == target.target.x &&
		child.movementTarget.y == target.target.y &&
		child.movementTarget.z == target.target.z &&
		child.slotId == target.slotId &&
		child.squadGeneration == target.squadGeneration &&
		child.decisionRevision == target.decisionRevision &&
		child.formationGeneration == target.formationGeneration &&
		child.claimResourceKind == target.claimResourceKind &&
		child.claimResourceId == target.claimResourceId &&
		child.claimResourceGeneration == target.claimResourceGeneration));
}

bool SquadMoveRecipient::ReplaceChildMovementTarget(
	const OrderKey& parentOrder, const OrderKey& childExecution,
	const RecipientId memberId, const ChildMovementTarget& target,
	const double now)
{
	if (!IsCurrentParent(parentOrder) || !target.hasTarget ||
		target.memberId != memberId || !target.target.IsFinite() ||
		target.slotId == 0 || target.squadGeneration == 0 ||
		target.decisionRevision == 0 || target.formationGeneration == 0 ||
		target.claimResourceKind != 3 || target.claimResourceId != target.slotId ||
		target.claimResourceGeneration != target.formationGeneration)
		return false;
	for (std::size_t index = 0; index < m_children.size(); ++index)
	{
		ChildExecution& active = m_children[index];
		if (active.key.orderId != childExecution.orderId ||
			active.key.generation != childExecution.generation ||
			active.memberId != memberId ||
			RCBotOrder::IsTerminal(active.record.state))
			continue;
		if (SameMovementTarget(active, target))
			return true;
		CancelNavigation(active);
		const RCBotOrder::UpdateResult cancelled = active.protocol.Abort(
			active.key, RCBotOrder::DetailReason::CANCELLED, now);
		active.record = cancelled.snapshot;
		EmitChild(active, cancelled);
		if (m_retiredChildren.size() == 64)
			m_retiredChildren.erase(m_retiredChildren.begin());
		m_retiredChildren.push_back(active);
		ChildExecution replacement;
		if (!CreateChild(parentOrder, target, now, replacement))
			return false;
		m_children[index] = replacement;
		RefreshAggregate();
		AggregateIfReady(now);
		return true;
	}
	return false;
}

bool SquadMoveRecipient::ObserveNavigationStarted(const OrderKey& parentOrder,
	const OrderKey& childExecution, const RecipientId memberId,
	const std::uint64_t requestToken, const std::uint64_t navigationIntentId)
{
	if (!IsCurrentParent(parentOrder) || navigationIntentId == 0)
		return false;
	ChildExecution* child = FindActiveChild(childExecution, memberId);
	if (child == nullptr || !child->navigationRequested ||
		child->navigationIntentBound ||
		child->navigationRequestToken != requestToken)
		return false;
	child->navigationIntentId = navigationIntentId;
	child->navigationRequestToken = 0;
	child->navigationIntentBound = true;
	return true;
}

bool SquadMoveRecipient::HasMemberInActiveSnapshot(
	const RecipientId memberId) const
{
	return m_active && std::find(m_memberSnapshot.begin(), m_memberSnapshot.end(),
		memberId) != m_memberSnapshot.end();
}

bool SquadMoveRecipient::HasActiveMovementForMember(
	const RecipientId memberId) const
{
	if (!HasMemberInActiveSnapshot(memberId))
		return false;
	// Until the first canonical spatial plan is dispatched, the parent owns
	// this member's movement window so legacy squad tasks cannot race dispatch.
	if (!m_dispatched)
		return true;
	return std::any_of(m_children.begin(), m_children.end(),
		[memberId](const ChildExecution& child) {
			return child.memberId == memberId && child.hasMovementTarget &&
				child.navigationRequested &&
				!RCBotOrder::IsTerminal(child.record.state);
		});
}

bool SquadMoveRecipient::CancelActiveParent(
	const RCBotOrder::DetailReason reason, const double now)
{
	if (!m_active)
		return false;
	for (ChildExecution& child : m_children)
	{
		if (RCBotOrder::IsTerminal(child.record.state))
			continue;
		CancelNavigation(child);
		const RCBotOrder::UpdateResult aborted = child.protocol.Abort(
			child.key, reason, now);
		child.record = aborted.snapshot;
		EmitChild(child, aborted);
	}
	const RCBotOrder::UpdateResult aborted = m_parent.Abort(m_parentKey,
		reason, now);
	Emit(aborted);
	ClearActive();
	return aborted.status == RCBotOrder::UpdateStatus::APPLIED;
}

bool SquadMoveRecipient::HasActiveParent() const
{
	return m_active;
}

OrderKey SquadMoveRecipient::ActiveParentKey() const
{
	return m_parentKey;
}

RCBotOrder::OrderRecord SquadMoveRecipient::ParentSnapshot() const
{
	return m_parent.Snapshot();
}

AggregateSnapshot SquadMoveRecipient::ParentAggregate() const
{
	return m_aggregate;
}

std::vector<ChildExecution> SquadMoveRecipient::ActiveChildren() const
{
	return m_children;
}

std::vector<ChildExecution> SquadMoveRecipient::RetiredChildren() const
{
	return m_retiredChildren;
}

std::vector<RecipientId> SquadMoveRecipient::ActiveMemberSnapshot() const
{
	return m_memberSnapshot;
}

bool SquadMoveRecipient::ChildrenDispatched() const
{
	return m_dispatched;
}

bool SquadMoveRecipient::IsCurrentParent(const OrderKey& key) const
{
	return m_active && key.orderId == m_parentKey.orderId &&
		key.generation == m_parentKey.generation;
}

ChildExecution* SquadMoveRecipient::FindActiveChild(const OrderKey& key,
	const RecipientId memberId)
{
	for (ChildExecution& child : m_children)
	{
		if (child.memberId == memberId && child.key.orderId == key.orderId &&
			child.key.generation == key.generation &&
			!RCBotOrder::IsTerminal(child.record.state))
			return &child;
	}
	return nullptr;
}

bool SquadMoveRecipient::ObserveNavigationTerminal(
	const OrderKey& parentOrder, const OrderKey& childExecution,
	const RecipientId memberId, const std::uint64_t navigationIntentId,
	const RCBotOrderTelemetry::NavigationTerminalResult result,
	const double now)
{
	if (!IsCurrentParent(parentOrder))
		return false;
	ChildExecution* child = FindActiveChild(childExecution, memberId);
	if (child == nullptr || !child->navigationRequested ||
		(child->navigationIntentBound ?
			child->navigationIntentId != navigationIntentId :
			child->navigationRequestToken != navigationIntentId))
		return false;

	child->navigationRequested = false;
	RCBotOrderTelemetry::RuntimeContext context;
	context.hasNavigationIntentId = true;
	context.navigationIntentId = navigationIntentId;
	context.hasNavigationTerminalResult = true;
	context.navigationTerminalResult = result;

	RCBotOrder::UpdateResult update;
	switch (result)
	{
	case RCBotOrderTelemetry::NavigationTerminalResult::COMPLETE:
		update = child->protocol.Complete(child->key, now);
		break;
	case RCBotOrderTelemetry::NavigationTerminalResult::PATH_STUCK:
		update = child->protocol.Fail(child->key, OrderOutcome::STUCK,
			RCBotOrder::DetailReason::FAILURE_REPORTED, now);
		break;
	case RCBotOrderTelemetry::NavigationTerminalResult::UNREACHABLE:
		update = child->protocol.Fail(child->key, OrderOutcome::UNREACHABLE,
			RCBotOrder::DetailReason::FAILURE_REPORTED, now);
		break;
	case RCBotOrderTelemetry::NavigationTerminalResult::PATH_FAILED:
		update = child->protocol.Fail(child->key, OrderOutcome::BLOCKED,
			RCBotOrder::DetailReason::FAILURE_REPORTED, now);
		break;
	case RCBotOrderTelemetry::NavigationTerminalResult::CANCELLED:
		update = child->protocol.Abort(child->key,
			RCBotOrder::DetailReason::CANCELLED, now);
		break;
	case RCBotOrderTelemetry::NavigationTerminalResult::NONE:
		return false;
	}
	child->record = update.snapshot;
	EmitChild(*child, update, context);
	RefreshAggregate();
	return AggregateIfReady(now) || update.status == RCBotOrder::UpdateStatus::APPLIED;
}

bool SquadMoveRecipient::ObserveMemberLifecycle(
	const OrderKey& parentOrder, const OrderRecipient& currentSquad,
	const RecipientId memberId, const RCBotOrder::DetailReason reason,
	const double now)
{
	if (!IsCurrentParent(parentOrder) || currentSquad != m_squadRecipient)
		return false;
	ChildExecution* child = nullptr;
	for (ChildExecution& candidate : m_children)
	{
		if (candidate.memberId == memberId &&
			!RCBotOrder::IsTerminal(candidate.record.state))
		{
			child = &candidate;
			break;
		}
	}
	if (child == nullptr)
		return false;
	CancelNavigation(*child);
	const RCBotOrder::UpdateResult update = child->protocol.Abort(child->key,
		reason == RCBotOrder::DetailReason::NONE ?
			RCBotOrder::DetailReason::DEATH_OR_LIFECYCLE : reason, now);
	child->record = update.snapshot;
	EmitChild(*child, update);
	RefreshAggregate();
	AggregateIfReady(now);
	return update.status == RCBotOrder::UpdateStatus::APPLIED;
}

bool SquadMoveRecipient::ObserveMembership(
	const OrderKey& parentOrder, const SquadMembershipSnapshot& current,
	const double now)
{
	if (!IsCurrentParent(parentOrder) || !current.IsValid() ||
		current.recipient != m_squadRecipient)
		return false;

	bool changed = false;
	for (ChildExecution& child : m_children)
	{
		if (!RCBotOrder::IsTerminal(child.record.state) &&
			!current.Contains(child.memberId))
		{
			CancelNavigation(child);
			const RCBotOrder::UpdateResult update = child.protocol.Abort(
				child.key, RCBotOrder::DetailReason::DEATH_OR_LIFECYCLE, now);
			child.record = update.snapshot;
			EmitChild(child, update);
			changed = update.status == RCBotOrder::UpdateStatus::APPLIED || changed;
		}
	}
	if (changed)
	{
		RefreshAggregate();
		AggregateIfReady(now);
	}
	return changed;
}

bool SquadMoveRecipient::Update(const double now)
{
	if (!m_active)
		return false;
	const RCBotOrder::UpdateResult update = m_parent.Tick(m_parentKey, now);
	Emit(update);
	if (update.status != RCBotOrder::UpdateStatus::APPLIED ||
		!update.changed || !RCBotOrder::IsTerminal(update.snapshot.state))
		return false;

	for (ChildExecution& child : m_children)
	{
		if (RCBotOrder::IsTerminal(child.record.state))
			continue;
		CancelNavigation(child);
		const RCBotOrder::UpdateResult cancelled = child.protocol.Abort(
			child.key, RCBotOrder::DetailReason::DEADLINE_EXPIRED, now);
		child.record = cancelled.snapshot;
		EmitChild(child, cancelled);
	}
	RefreshAggregate();
	ClearActive();
	return true;
}

const char* SquadMoveRecipient::ParentTerminalRule()
{
	return "Parent COMPLETE iff every snapshot child COMPLETE; otherwise wait "
		"for all snapshot children, then use PARTIAL_FAILURE when any child "
		"failed or at least one child completed, and ABORTED/CHILDREN_EXHAUSTED "
		"when every child was cancelled without a completion.";
}

void SquadMoveRecipient::Emit(const RCBotOrder::UpdateResult& result,
	const RCBotOrderTelemetry::OrderCorrelation& correlation)
{
	if (m_telemetry == nullptr)
		return;
	RCBotOrderTelemetry::TelemetryEvent event =
		RCBotOrderTelemetry::BuildEvent(result);
	event.correlation = correlation;
	if (event.type != RCBotOrderTelemetry::EventType::NONE)
		m_telemetry(m_context, event);
}

void SquadMoveRecipient::EmitChild(const ChildExecution& child,
	const RCBotOrder::UpdateResult& result,
	const RCBotOrderTelemetry::RuntimeContext& context)
{
	if (m_telemetry == nullptr)
		return;
	RCBotOrderTelemetry::TelemetryEvent event =
		RCBotOrderTelemetry::BuildEvent(result, context);
	event.correlation.hasParentOrder = true;
	event.correlation.parentOrder = child.parentOrder;
	event.correlation.hasMemberId = true;
	event.correlation.memberId = child.memberId;
	event.correlation.hasChildExecution = true;
	event.correlation.childExecution = child.key;
	if (child.navigationIntentBound && child.navigationIntentId != 0)
	{
		event.context.hasNavigationIntentId = true;
		event.context.navigationIntentId = child.navigationIntentId;
	}
	if (event.type != RCBotOrderTelemetry::EventType::NONE)
		m_telemetry(m_context, event);
}

void SquadMoveRecipient::CancelNavigation(ChildExecution& child)
{
	if (!child.navigationRequested)
		return;
	child.navigationRequested = false;
	if (m_navigationCancel != nullptr)
	{
		NavigationIntentRequest request;
		request.parentOrder = child.parentOrder;
		request.childExecution = child.key;
		request.memberId = child.memberId;
		request.target = child.movementTarget;
		request.slotId = child.slotId;
		request.squadGeneration = child.squadGeneration;
		request.decisionRevision = child.decisionRevision;
		request.formationGeneration = child.formationGeneration;
		request.claimResourceKind = child.claimResourceKind;
		request.claimResourceId = child.claimResourceId;
		request.claimResourceGeneration = child.claimResourceGeneration;
		m_navigationCancel(m_context, request,
			child.navigationIntentBound ? child.navigationIntentId :
				child.navigationRequestToken);
	}
}

bool SquadMoveRecipient::TerminalizeChild(ChildExecution& child,
	const RCBotOrder::UpdateResult& result)
{
	child.record = result.snapshot;
	return result.status == RCBotOrder::UpdateStatus::APPLIED &&
		RCBotOrder::IsTerminal(result.snapshot.state);
}

bool SquadMoveRecipient::AggregateIfReady(const double now)
{
	RefreshAggregate();
	if (m_aggregate.pendingCount != 0 || !m_active)
		return false;

	RCBotOrder::UpdateResult update;
	if (m_aggregate.completeCount == m_children.size())
	{
		update = m_parent.Complete(m_parentKey, now);
		m_aggregate.outcome = OrderOutcome::COMPLETE;
	}
	else if (m_aggregate.failureCount != 0 ||
		m_aggregate.completeCount != 0)
	{
		update = m_parent.Fail(m_parentKey, OrderOutcome::PARTIAL_FAILURE,
			RCBotOrder::DetailReason::CHILDREN_AGGREGATED, now);
		m_aggregate.outcome = OrderOutcome::PARTIAL_FAILURE;
	}
	else
	{
		update = m_parent.Abort(m_parentKey,
			RCBotOrder::DetailReason::CHILDREN_EXHAUSTED, now);
		m_aggregate.outcome = OrderOutcome::ABORTED;
	}
	RCBotOrderTelemetry::OrderCorrelation correlation;
	correlation.hasAggregateOutcome = true;
	correlation.aggregateOutcome = m_aggregate.outcome;
	Emit(update, correlation);
	m_aggregate.terminal = update.status == RCBotOrder::UpdateStatus::APPLIED;
	m_active = !m_aggregate.terminal;
	return m_aggregate.terminal;
}

void SquadMoveRecipient::RefreshAggregate()
{
	m_aggregate.completeCount = 0;
	m_aggregate.failureCount = 0;
	m_aggregate.cancelledCount = 0;
	m_aggregate.pendingCount = 0;
	for (const ChildExecution& child : m_children)
	{
		if (!RCBotOrder::IsTerminal(child.record.state))
		{
			++m_aggregate.pendingCount;
			continue;
		}
		if (child.record.outcome == OrderOutcome::COMPLETE)
			++m_aggregate.completeCount;
		else if (child.record.outcome == OrderOutcome::ABORTED)
			++m_aggregate.cancelledCount;
		else if (IsFailure(child.record.outcome))
			++m_aggregate.failureCount;
	}
}

void SquadMoveRecipient::RetireActiveChildren()
{
	for (ChildExecution& child : m_children)
	{
		if (!RCBotOrder::IsTerminal(child.record.state))
		{
			CancelNavigation(child);
			const RCBotOrder::UpdateResult replaced = child.protocol.Abort(
				child.key, RCBotOrder::DetailReason::REPLACED,
				child.record.issuedAt);
			child.record = replaced.snapshot;
			EmitChild(child, replaced);
		}
		if (m_retiredChildren.size() == 64)
			m_retiredChildren.erase(m_retiredChildren.begin());
		m_retiredChildren.push_back(child);
	}
}

std::uint64_t SquadMoveRecipient::AllocateChildId()
{
	return m_nextChildId++;
}

Generation SquadMoveRecipient::AllocateChildGeneration()
{
	return m_nextChildGeneration++;
}

void SquadMoveRecipient::ClearActive()
{
	m_active = false;
	m_parentKey = OrderKey();
	m_squadRecipient = OrderRecipient();
	m_memberSnapshot.clear();
	m_dispatched = false;
}
}
