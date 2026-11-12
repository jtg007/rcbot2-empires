#ifndef RCBOT_EMPIRES_SPATIAL_H
#define RCBOT_EMPIRES_SPATIAL_H

#include "bot_empires_native_placement.h"
#include <algorithm>
#include <vector>

namespace EmpiresSpatial
{
using EmpiresNativePlacement::Point;
using NativeResult = EmpiresNativePlacement::Result;
constexpr float MaxRadius = 2048.0f; // local base-scale search in Source world units
constexpr unsigned MaxCells = 256;
constexpr unsigned MaxEntries = 512;
constexpr unsigned MaxAreas = 128;
constexpr unsigned MaxGenerated = MaxAreas * 6;
constexpr unsigned MaxNativeQueries = 64;
constexpr unsigned MaxAccepted = 8;

enum class Status { Found, NoResult, Unavailable, InvalidRequest };
inline const char* Name(Status status)
{
	switch (status)
	{
	case Status::Found: return "found";
	case Status::NoResult: return "no_result";
	case Status::Unavailable: return "unavailable";
	default: return "invalid_request";
	}
}
struct Request
{
	Point origin;
	float radius = 1024.0f;
	unsigned maxCandidates = MaxNativeQueries;
	int buildingType = 1; // see EmpiresNativePlacement::CanPlaceNative
	// > 0: skip candidates closer than this to an already rejected one, so the query budget
	// reaches beyond a cluster of blocked points (large hulls such as the vehicle factory).
	float invalidSkipRadius = 0.0f;
};
struct Sample { Point origin; unsigned areaId = 0; };
struct Source
{
	bool available = false;
	bool truncated = false;
	unsigned cellsInspected = 0, entriesInspected = 0, areasInspected = 0;
	std::vector<Sample> samples;
};
struct BarracksPlacementCandidate
{
	Point origin;
	unsigned areaId = 0;
	double distanceSquared = 0;
	NativeResult nativeValidity = NativeResult::Unavailable;
};
struct SearchResult
{
	Status status = Status::NoResult;
	bool truncated = false;
	unsigned cellsInspected = 0, entriesInspected = 0, areasInspected = 0;
	unsigned generated = 0, plausible = 0, duplicates = 0;
	unsigned nativeQueries = 0, valid = 0, invalid = 0, unavailable = 0;
	unsigned canBuildAtCalls = 0, canBuildAtPassed = 0, obstructionCalls = 0;
	EmpiresNativePlacement::UnavailableReason unavailableReason = EmpiresNativePlacement::UnavailableReason::None;
	bool hasSelected = false;
	BarracksPlacementCandidate selected;
	std::vector<BarracksPlacementCandidate> accepted;
};
inline bool Finite(const Point& p)
{
	return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}
inline bool ValidRequest(const Request& r)
{
	return Finite(r.origin) && std::isfinite(r.radius) && r.radius > 0 &&
		r.radius <= MaxRadius && r.maxCandidates > 0 && r.maxCandidates <= MaxNativeQueries;
}
inline double DistanceSquared(const Point& a, const Point& b)
{
	const double x = double(a.x) - b.x, y = double(a.y) - b.y, z = double(a.z) - b.z;
	return x*x + y*y + z*z;
}
// Closest in 3D to the request, then area ID and coordinates: stable tie order.
inline bool Closer(const BarracksPlacementCandidate& a, const BarracksPlacementCandidate& b)
{
	if (a.distanceSquared != b.distanceSquared) return a.distanceSquared < b.distanceSquared;
	if (a.areaId != b.areaId) return a.areaId < b.areaId;
	if (a.origin.x != b.origin.x) return a.origin.x < b.origin.x;
	if (a.origin.y != b.origin.y) return a.origin.y < b.origin.y;
	return a.origin.z < b.origin.z;
}

// Engine-free search seam; production supplies nav samples and the native wrapper.
// UNAVAILABLE discards even earlier valid answers. No geometry can substitute for it.
template<class NativeQuery>
SearchResult Select(const Request& request, const Source& source, NativeQuery query)
{
	SearchResult result;
	if (!ValidRequest(request)) { result.status = Status::InvalidRequest; return result; }
	if (!source.available) { result.status = Status::Unavailable; return result; }
	result.truncated = source.truncated || source.samples.size() > MaxGenerated;
	result.cellsInspected = source.cellsInspected;
	result.entriesInspected = source.entriesInspected;
	result.areasInspected = source.areasInspected;
	std::vector<BarracksPlacementCandidate> candidates;
	for (const Sample& sample : source.samples)
	{
		if (result.generated >= MaxGenerated) break;
		++result.generated;
		if (!sample.areaId || !Finite(sample.origin)) continue;
		const double distance = DistanceSquared(sample.origin, request.origin);
		if (distance > double(request.radius)*request.radius) continue;
		candidates.push_back({sample.origin, sample.areaId, distance, NativeResult::Unavailable});
	}
	std::sort(candidates.begin(), candidates.end(), Closer);
	std::vector<BarracksPlacementCandidate> unique;
	for (const auto& candidate : candidates)
	{
		bool duplicate = false;
		for (const auto& prior : unique)
			if (DistanceSquared(candidate.origin, prior.origin) <= 1.0) { duplicate = true; break; }
		if (duplicate) { ++result.duplicates; continue; }
		unique.push_back(candidate);
	}
	result.plausible = unique.size();
	std::vector<Point> rejected;
	for (auto candidate : unique)
	{
		if (request.invalidSkipRadius > 0.0f)
		{
			bool nearRejected = false;
			for (const Point& point : rejected)
				if (DistanceSquared(candidate.origin, point) < double(request.invalidSkipRadius) * request.invalidSkipRadius)
				{ nearRejected = true; break; }
			if (nearRejected) continue;
		}
		if (result.nativeQueries >= request.maxCandidates || result.accepted.size() >= MaxAccepted)
		{ result.truncated = true; break; }
		EmpiresNativePlacement::Diagnostics diagnostics;
		candidate.nativeValidity = query(candidate.origin, diagnostics);
		++result.nativeQueries;
		result.canBuildAtCalls += diagnostics.canBuildAtCalled;
		result.canBuildAtPassed += diagnostics.getAllObstructionsCalled;
		result.obstructionCalls += diagnostics.getAllObstructionsCalled;
		if (candidate.nativeValidity == NativeResult::Unavailable)
		{
			++result.unavailable;
			result.unavailableReason = diagnostics.reason;
			result.accepted.clear(); result.hasSelected = false;
			result.status = Status::Unavailable;
			return result;
		}
		if (candidate.nativeValidity == NativeResult::Invalid) { ++result.invalid; rejected.push_back(candidate.origin); continue; }
		++result.valid;
		result.accepted.push_back(candidate);
		if (!result.hasSelected) { result.selected = candidate; result.hasSelected = true; }
	}
	result.status = result.hasSelected ? Status::Found : Status::NoResult;
	return result;
}

// Synchronous server-thread query. No entity creation or construction execution.
SearchResult SearchBarracksCandidates(const EmpiresNativePlacement::ContextIdentity& context,
	const Request& request);
}
#endif
