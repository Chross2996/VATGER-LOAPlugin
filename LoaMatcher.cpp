#include "stdafx.h"
#include "windows.h"
#include "LOAPlugin.h"
#include <string>
#include <vector>
#include <cctype>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <algorithm>
#include <climits>

bool EqualsIgnoreCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    }
    return true;
}


static double ToAltFeet(int altOrLevel)
{
    // EuroScope may return altitude in feet or level (FL). If it's small, treat as FL.
    if (altOrLevel > 0 && altOrLevel < 1000) return (double)altOrLevel * 100.0;
    return (double)altOrLevel;
}

struct PredSampleLL {
    double lat;
    double lon;
    double altFt;
};

static void BuildPredSamplesLL(const EuroScopePlugIn::CFlightPlan& fp, std::vector<PredSampleLL>& out)
{
    out.clear();
    EuroScopePlugIn::CFlightPlanPositionPredictions preds = fp.GetPositionPredictions();
    const int n = preds.GetPointsNumber();
    if (n <= 0) return;
    out.reserve((size_t)n);
    for (int i = 0; i < n; ++i) {
        EuroScopePlugIn::CPosition p = preds.GetPosition(i);
        out.push_back(PredSampleLL{ p.m_Latitude, p.m_Longitude, ToAltFeet(preds.GetAltitude(i)) });
    }
}


static bool PointInPolyLL(double lat, double lon, const std::vector<std::pair<double, double>>& poly)
{
    const size_t n = poly.size();
    if (n < 3) return false;
    bool inside = false;
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const double yi = poly[i].first;
        const double xi = poly[i].second;
        const double yj = poly[j].first;
        const double xj = poly[j].second;

        const bool intersect =
            ((yi > lat) != (yj > lat)) &&
            (lon < (xj - xi) * (lat - yi) / ((yj - yi) + 1e-12) + xi);

        if (intersect) inside = !inside;
    }
    return inside;
}

static double Cross2D(double ax, double ay, double bx, double by, double cx, double cy)
{
    // Cross product of AB x AC
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
}

static bool OnSegment2D(double ax, double ay, double bx, double by, double cx, double cy)
{
    double minx = (ax < bx ? ax : bx);
    double maxx = (ax > bx ? ax : bx);
    double miny = (ay < by ? ay : by);
    double maxy = (ay > by ? ay : by);
    const double eps = 1e-12;
    return (minx - eps <= cx && cx <= maxx + eps) && (miny - eps <= cy && cy <= maxy + eps);
}

static int Orient2D(double ax, double ay, double bx, double by, double cx, double cy)
{
    const double v = Cross2D(ax, ay, bx, by, cx, cy);
    const double eps = 1e-12;
    if (v > eps) return 1;
    if (v < -eps) return -1;
    return 0;
}

static bool SegmentsIntersect2D(double ax, double ay, double bx, double by,
    double cx, double cy, double dx, double dy)
{
    const int o1 = Orient2D(ax, ay, bx, by, cx, cy);
    const int o2 = Orient2D(ax, ay, bx, by, dx, dy);
    const int o3 = Orient2D(cx, cy, dx, dy, ax, ay);
    const int o4 = Orient2D(cx, cy, dx, dy, bx, by);

    if (o1 != o2 && o3 != o4) return true;

    // Collinear cases
    if (o1 == 0 && OnSegment2D(ax, ay, bx, by, cx, cy)) return true;
    if (o2 == 0 && OnSegment2D(ax, ay, bx, by, dx, dy)) return true;
    if (o3 == 0 && OnSegment2D(cx, cy, dx, dy, ax, ay)) return true;
    if (o4 == 0 && OnSegment2D(cx, cy, dx, dy, bx, by)) return true;

    return false;
}

static bool SegmentIntersectsPolygonLL(double aLat, double aLon, double bLat, double bLon,
    const std::vector<std::pair<double, double>>& poly)
{
    const size_t n = poly.size();
    if (n < 2) return false;
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const double cLat = poly[j].first, cLon = poly[j].second;
        const double dLat = poly[i].first, dLon = poly[i].second;
        if (SegmentsIntersect2D(aLon, aLat, bLon, bLat, cLon, cLat, dLon, dLat))
            return true;
    }
    return false;
}

static int FirstEnterMinuteVolumeFromSamplesLL(const std::vector<PredSampleLL>& samples, const CustomVolume& vol)
{
    const int n = (int)samples.size();
    if (n <= 0) return INT_MAX;

    // Check points + segment crossings
    for (int i = 0; i < n; ++i) {
        const PredSampleLL& s = samples[(size_t)i];
        if (s.altFt >= vol.lowerFt && s.altFt <= vol.upperFt) {
            if (PointInPolyLL(s.lat, s.lon, vol.polygon)) return i;
        }
        if (i + 1 < n) {
            const PredSampleLL& s2 = samples[(size_t)i + 1];
            // Require altitude band overlap at endpoints (simple and stable)
            if ((s.altFt >= vol.lowerFt && s.altFt <= vol.upperFt) ||
                (s2.altFt >= vol.lowerFt && s2.altFt <= vol.upperFt))
            {
                // If segment crosses polygon or enters between points, count as entry at i+1
                if (SegmentIntersectsPolygonLL(s.lat, s.lon, s2.lat, s2.lon, vol.polygon))
                    return i + 1;
            }
        }
    }
    return INT_MAX;
}

static int FirstEnterMinuteVolume(const EuroScopePlugIn::CFlightPlan& fp, const CustomVolume& vol)
{
    std::vector<PredSampleLL> samples;
    BuildPredSamplesLL(fp, samples);
    return FirstEnterMinuteVolumeFromSamplesLL(samples, vol);
}

static bool LastPredictedPointInsideVolumeLL(const std::vector<PredSampleLL>& samples, const CustomVolume& vol)
{
    if (samples.empty()) return false;

    const PredSampleLL& last = samples.back();
    if (last.altFt < vol.lowerFt || last.altFt > vol.upperFt)
        return false;

    return PointInPolyLL(last.lat, last.lon, vol.polygon);
}

const LOAEntry* MatchLoaEntry(const EuroScopePlugIn::CFlightPlan& fp,
    const std::unordered_set<std::string>& /*onlineControllers*/)
{
    if (!fp.IsValid() || !plugin.IsLOARelevantState(fp.GetState())) return nullptr;

    // Ensure active runway selections are up-to-date even when EuroScope doesn't fire the callback.
    plugin.PollActiveRunwaysIfNeeded();

    const char* planType = fp.GetFlightPlanData().GetPlanType();
    if (_stricmp(planType, "I") != 0) return nullptr;

    const std::string callsign = fp.GetCallsign();
    ULONGLONG now = GetTickCount64();

    // 10s cache + sectorControlVersion
    auto tsIt = plugin.matchTimestamps.find(callsign);
    auto verIt = plugin.matchVersions.find(callsign);
    if (tsIt != plugin.matchTimestamps.end() &&
        verIt != plugin.matchVersions.end() &&
        now - tsIt->second < 5000 &&
        verIt->second == plugin.sectorControlVersion)
    {
        auto matchIt = plugin.matchedLOACache.find(callsign);
        if (matchIt != plugin.matchedLOACache.end()) {
            return matchIt->second;
        }
    }

    const std::string origin = fp.GetFlightPlanData().GetOrigin();
    const std::string destination = fp.GetFlightPlanData().GetDestination();
    const auto& routeSet = plugin.GetCachedRouteSet(fp);

    const std::string mySector = plugin.ControllerMyself().GetPositionId();

    // Shared caches for this match call (avoid repeated lookups)
    std::unordered_map<std::string, std::string> resolveCache; // sectorId -> controlling sectorId

    // Owned sectors as a set for fast membership checks
    std::unordered_set<std::string> ownedSet;
    auto ownItGlobal = plugin.sectorOwnership.find(mySector);
    if (ownItGlobal != plugin.sectorOwnership.end()) {
        ownedSet.reserve(ownItGlobal->second.size() * 2);
        ownedSet.insert(ownItGlobal->second.begin(), ownItGlobal->second.end());
    }

    auto resolveController = [&](const std::string& sectorId) -> std::string {
        auto it = resolveCache.find(sectorId);
        if (it != resolveCache.end()) return it->second;
        std::string actual = plugin.ResolveControllingSector(sectorId, plugin.currentFrameOnlineControllers);
        resolveCache.emplace(sectorId, actual);
        return actual;
        };

    // Exclusion: skip LOAs that explicitly exclude this destination
    auto isExcludedDest = [&](const LOAEntry& e) -> bool {
        if (!e.excludeDestinationAirports.empty() ||
            !e.excludeDestinationAirportSet.empty() ||
            !e.excludeDestinationAirportPrefixes.empty()) {
            if (e.excludeDestinationAirportSet.count(destination) > 0) return true;
            for (const auto& pre : e.excludeDestinationAirportPrefixes) {
                if (destination.compare(0, pre.length(), pre) == 0) return true;
            }
        }
        return false;
        };
    auto isExcludedOrigin = [&](const LOAEntry& e) -> bool {
        if (!e.excludeOriginAirports.empty() ||
            !e.excludeOriginAirportSet.empty() ||
            !e.excludeOriginAirportPrefixes.empty()) {
            if (e.excludeOriginAirportSet.count(origin) > 0) return true;
            for (const auto& pre : e.excludeOriginAirportPrefixes) {
                if (origin.compare(0, pre.length(), pre) == 0) return true;
            }
        }
        return false;
        };

    // Gate by next-sector control/priority
    auto shouldMatchLOA = [&](const std::vector<std::string>& nextSectors) -> bool {
        for (const std::string& next : nextSectors) {
            std::string actualController = resolveController(next);

            bool nextIsDefined = plugin.sectorOwnership.find(next) != plugin.sectorOwnership.end();
            const bool iOwnNext = (ownedSet.count(next) > 0);

            if (_stricmp(actualController.c_str(), mySector.c_str()) == 0) return false;

            if (nextIsDefined) {
                if (actualController.empty() && iOwnNext) return false;

                auto prIt = plugin.sectorPriority.find(next);
                if (prIt != plugin.sectorPriority.end()) {
                    const auto& prioList = prIt->second;
                    auto myPrio = std::find(prioList.begin(), prioList.end(), mySector);
                    auto otherPrio = std::find(prioList.begin(), prioList.end(), actualController);
                    if (myPrio != prioList.end() && otherPrio != prioList.end() && myPrio < otherPrio) return false;
                }

                return true;
            }

            if (!nextIsDefined && actualController.empty()) return true; // external offline
            if (!nextIsDefined && !_stricmp(actualController.c_str(), mySector.c_str())) return false; // I control
            return true; // external and someone else online
        }
        return false;
        };

    // Suppress LOAs whose *source* sector is controlled by someone who outranks me
    auto isSourceSectorSuppressed = [&](const LOAEntry& e) -> bool {
        if (e.sectors.empty()) return false;
        for (const auto& src : e.sectors) {
            std::string actual = resolveController(src);
            if (actual.empty()) continue;
            if (_stricmp(actual.c_str(), mySector.c_str()) == 0) continue;

            auto prIt = plugin.sectorPriority.find(src);
            if (prIt == plugin.sectorPriority.end()) continue;
            const auto& prio = prIt->second;
            auto meIt = std::find(prio.begin(), prio.end(), mySector);
            auto himIt = std::find(prio.begin(), prio.end(), actual);
            if (meIt != prio.end() && himIt != prio.end() && himIt < meIt) return true;
        }
        return false;
        };

    auto airportMatch = [&](const LOAEntry* e)->bool {
        if (!e->originAirports.empty() &&
            !plugin.MatchesAirport(e->originAirportSet, e->originAirportPrefixes, origin)) return false;
        if (!e->destinationAirports.empty() &&
            !plugin.MatchesAirport(e->destinationAirportSet, e->destinationAirportPrefixes, destination)) return false;
        return true;
        };

    auto runwayMatch = [&](const LOAEntry* e)->bool {
        if (!e) return false;
        if (e->runways.empty()) return true; // no runway constraint

        // Decide which airport + which active runway set to compare against
        switch (e->listKind) {
        case LOAListKind::Departure:
        case LOAListKind::DepartureFallback:
            // Departure lists compare against active DEP runways at ORIGIN airport
            return plugin.MatchesActiveRunway(origin, /*isDeparture=*/true, e->runways);

        case LOAListKind::Destination:
        case LOAListKind::DestinationFallback:
            // Destination lists compare against active ARR runways at DESTINATION airport
            return plugin.MatchesActiveRunway(destination, /*isDeparture=*/false, e->runways);

        default:
            // Unknown kind: only apply if entry clearly constrains one side
            if (!e->destinationAirports.empty()) {
                return plugin.MatchesActiveRunway(destination, /*isDeparture=*/false, e->runways);
            }
            if (!e->originAirports.empty()) {
                return plugin.MatchesActiveRunway(origin, /*isDeparture=*/true, e->runways);
            }
            // Sector-style entry: don't block on runways
            return true;
        }
        };

    auto waypointsMatch = [&](const LOAEntry* e)->bool {
        // LOA waypoints are normalized to lowercase at JSON load; routeSet uses lowercase keys
        for (const auto& wp : e->waypoints) {
            if (routeSet.count(wp) == 0) return false;
        }
        return true;
        };

    auto notViaMatch = [&](const LOAEntry* e)->bool {
        // NOT VIA waypoints are normalized to lowercase at JSON load; routeSet uses lowercase keys
        for (const auto& wp : e->notViaWaypoints) {
            if (routeSet.count(wp) != 0) return false; // forbidden waypoint present
        }
        return true;
        };
    // Altitude gate (simplified):
    // Only apply to Departure/Destination-style LOAs (those that constrain origin and/or destination)
    // and only when a numeric XFL is defined.
    auto passesFinalAltitudeGate = [&](const LOAEntry* e) -> bool {
        if (!e) return false;
        if (e->xfl <= 0) return true; // no numeric XFL -> no altitude gate
        const int xflFeet = e->xfl * 100;
        const int finalAlt = fp.GetFinalAltitude();

        switch (e->listKind) {
        case LOAListKind::Destination:
        case LOAListKind::DestinationFallback:
            // Destination LOAs: match when final altitude is SAME or ABOVE XFL
            return (finalAlt >= xflFeet);
        case LOAListKind::Departure:
        case LOAListKind::DepartureFallback:
            // Departure LOAs: match only when final altitude is ABOVE XFL
            return (finalAlt > xflFeet);
        default:
            // Sector-style / unknown: no altitude gating here
            return true;
        }
        };


    // Custom volume prediction gate (volumes.json):
    // - If predictedEnterVolumes is set: require entering ANY of those volumes
    // - If predictedFromVolumes/predictedToVolumes are set:
    //     * from+to: require entering a FROM volume and later a TO volume
    //     * only from: require entering any FROM volume
    //     * only to: require entering any TO volume
    // - If predictedEndVolumes is set: require the last predicted point to end inside ANY listed volume

    // ---------------- Volume prediction caching (performance) ----------------
    // Evaluating volume entry can be expensive (position predictions + geometry).
    // Cache entry minute per volume-id for the duration of this MatchLoaEntry call.
    const auto& _volsAll = plugin.GetCustomVolumes();
    std::unordered_map<std::string, int> _volEnterMinuteCache;
    std::vector<PredSampleLL> _predSamplesLL;
    bool _predSamplesReady = false;

    auto _ensurePredSamples = [&]() {
        if (_predSamplesReady) return;
        BuildPredSamplesLL(fp, _predSamplesLL);
        _predSamplesReady = true;
        };

    auto _getVolPtr = [&](const std::string& id) -> const CustomVolume* {
        auto it = _volsAll.find(id);
        if (it == _volsAll.end()) return nullptr;
        return &it->second;
        };

    auto _enterMinuteCached = [&](const std::string& id, const CustomVolume& v) -> int {
        auto it = _volEnterMinuteCache.find(id);
        if (it != _volEnterMinuteCache.end()) return it->second;
        _ensurePredSamples();
        int m = FirstEnterMinuteVolumeFromSamplesLL(_predSamplesLL, v);
        _volEnterMinuteCache.emplace(id, m);
        return m;
        };

    auto volumesMatch = [&](const LOAEntry* e) -> bool {
        if (!e) return false;
        const bool hasEnter = !e->predictedEnterVolumes.empty();
        const bool hasFrom = !e->predictedFromVolumes.empty();
        const bool hasTo = !e->predictedToVolumes.empty();
        const bool hasEnd = !e->predictedEndVolumes.empty();
        if (!hasEnter && !hasFrom && !hasTo && !hasEnd) return true; // no constraint
        auto entersAny = [&](const std::vector<std::string>& ids) -> int {
            int bestMinute = INT_MAX;
            for (const auto& id : ids) {
                const CustomVolume* v = _getVolPtr(id);
                if (!v) return INT_MAX; // referenced volume missing -> do NOT match
                int m = _enterMinuteCached(id, *v);
                if (m < bestMinute) bestMinute = m;
            }
            return bestMinute;
            };

        auto endsInAny = [&]() -> bool {
            if (!hasEnd) return true;
            _ensurePredSamples();
            for (const auto& id : e->predictedEndVolumes) {
                const CustomVolume* v = _getVolPtr(id);
                if (!v) return false; // referenced volume missing -> do NOT match
                if (LastPredictedPointInsideVolumeLL(_predSamplesLL, *v)) return true;
            }
            return false;
            };

        if (!endsInAny()) return false;

        // Only predictedEndVolumes was defined, and it passed.
        if (!hasEnter && !hasFrom && !hasTo) return true;

        if (hasEnter) {
            // Any enter volume hit is enough
            for (const auto& id : e->predictedEnterVolumes) {
                const CustomVolume* v = _getVolPtr(id);
                if (!v) return false; // missing volume -> no match
                if (_enterMinuteCached(id, *v) != INT_MAX) return true;
            }
            return false;
        }

        if (hasFrom && !hasTo) {
            return entersAny(e->predictedFromVolumes) != INT_MAX;
        }
        if (!hasFrom && hasTo) {
            return entersAny(e->predictedToVolumes) != INT_MAX;
        }

        // from + to transition
        // We require there exists a pair where enter(to) > enter(from).
        // Compute entry minutes for each referenced volume; if any referenced volume is missing, fail.
        std::vector<int> fromTimes;
        fromTimes.reserve(e->predictedFromVolumes.size());
        for (const auto& id : e->predictedFromVolumes) {
            const CustomVolume* v = _getVolPtr(id);
            if (!v) return false;
            fromTimes.push_back(_enterMinuteCached(id, *v));
        }
        std::vector<int> toTimes;
        toTimes.reserve(e->predictedToVolumes.size());
        for (const auto& id : e->predictedToVolumes) {
            const CustomVolume* v = _getVolPtr(id);
            if (!v) return false;
            toTimes.push_back(_enterMinuteCached(id, *v));
        }
        for (size_t i = 0; i < fromTimes.size(); ++i) {
            if (fromTimes[i] == INT_MAX) continue;
            for (size_t j = 0; j < toTimes.size(); ++j) {
                if (toTimes[j] == INT_MAX) continue;
                if (toTimes[j] > fromTimes[i]) return true;
            }
        }
        return false;
        };



    auto isVolumeLoaEntry = [&](const LOAEntry* e) -> bool {
        if (!e) return false;
        return !e->predictedEnterVolumes.empty()
            || !e->predictedFromVolumes.empty()
            || !e->predictedToVolumes.empty()
            || !e->predictedEndVolumes.empty();
        };

    auto isDestinationKind = [&](const LOAEntry* e) -> bool {
        if (!e) return false;
        return (e->listKind == LOAListKind::Destination || e->listKind == LOAListKind::DestinationFallback);
        };
    auto isDepartureKind = [&](const LOAEntry* e) -> bool {
        if (!e) return false;
        return (e->listKind == LOAListKind::Departure || e->listKind == LOAListKind::DepartureFallback);
        };
    auto scoreEntry = [&](const LOAEntry* e)->int {
        int score = 0;

        // prefer destination over departure
        if (!e->destinationAirports.empty()) score += 20;

        // priority on next sectors
        // mySector is computed once per match call
        if (!e->nextSectors.empty()) {
            for (const auto& next : e->nextSectors) {
                std::string actual = resolveController(next);
                if (!actual.empty()) {
                    if (_stricmp(actual.c_str(), mySector.c_str()) == 0) score -= 10000;
                    else {
                        auto prIt = plugin.sectorPriority.find(next);
                        if (prIt == plugin.sectorPriority.end()) break;
                        const auto& prio = prIt->second;
                        auto meIt = std::find(prio.begin(), prio.end(), mySector);
                        auto himIt = std::find(prio.begin(), prio.end(), actual);
                        if (meIt != prio.end() && himIt != prio.end() && himIt < meIt) score += 50;
                    }
                    break;
                }
            }
        }

        // small bonus if COP text present (tie-break)
        if (!e->copText.empty()) score += 5;

        // tie-break on higher XFL last
        score += e->xfl;

        return score;
        };

    // Build candidate set from waypoint index (already includes dest/dep/LOR)
    std::unordered_set<const LOAEntry*> candidates;
    candidates.reserve(256);
    // routeSet already contains lowercased fixes
    for (const auto& lwp : routeSet) {
        auto it = plugin.indexByWaypoint.find(lwp);

        if (it != plugin.indexByWaypoint.end()) {
            candidates.insert(it->second.begin(), it->second.end());
        }
    }
    // Also consider entries with no waypoints (rare)
    // (We skip global scan for perf; those should still have at least one wpt to be indexed.)

    const LOAEntry* best = nullptr;
    int bestScore = INT_MIN;

    auto considerCandidate = [&](const LOAEntry* e) {
        if (!e) return;
        if (isExcludedDest(*e) || isExcludedOrigin(*e)) return;
        if (isSourceSectorSuppressed(*e)) return;
        if (!e->nextSectors.empty() && !shouldMatchLOA(e->nextSectors)) return;
        if (!airportMatch(e)) return;
        if (!runwayMatch(e)) return;
        if (!passesFinalAltitudeGate(e)) return;
        if (!volumesMatch(e)) return;
        if (!notViaMatch(e)) return;
        if (!waypointsMatch(e)) return;

        int s = scoreEntry(e);
        if (!best || s > bestScore) {
            best = e;
            bestScore = s;
        }
        };

    // Priority:
    // 1) Destination LOAs (non-volume)
    for (const LOAEntry* e : candidates) {
        if (!e) continue;
        if (!isDestinationKind(e)) continue;
        if (isVolumeLoaEntry(e)) continue;
        considerCandidate(e);
    }

    // 2) Departure LOAs (non-volume)
    if (!best) {
        for (const LOAEntry* e : candidates) {
            if (!e) continue;
            if (!isDepartureKind(e)) continue;
            if (isVolumeLoaEntry(e)) continue;
            considerCandidate(e);
        }
    }

    // 2b) Other non-volume (sector-style etc.)
    if (!best) {
        for (const LOAEntry* e : candidates) {
            if (!e) continue;
            if (isDestinationKind(e) || isDepartureKind(e)) continue;
            if (isVolumeLoaEntry(e)) continue;
            considerCandidate(e);
        }
    }

    // 3) Volume LOAs (enter / from-to), regardless of list kind
    if (!best) {
        for (const LOAEntry* e : candidates) {
            if (!e) continue;
            if (!isVolumeLoaEntry(e)) continue;
            considerCandidate(e);
        }
    }

    // ---- NEW: Slow normal scan (destination then departure) before any fallback ----
    if (!best) {
        auto consider_nonvolume = [&](const std::vector<LOAEntry>& list) {
            for (const auto& e : list) {
                if (isVolumeLoaEntry(&e)) continue; // volume LOAs are handled in phase 3
                if (isExcludedDest(e) || isExcludedOrigin(e)) continue;
                if (isSourceSectorSuppressed(e)) continue;
                if (!e.nextSectors.empty() && !shouldMatchLOA(e.nextSectors)) continue;
                if (!airportMatch(&e)) continue;
                if (!runwayMatch(&e)) continue;
                if (!passesFinalAltitudeGate(&e)) continue;
                if (!volumesMatch(&e)) continue;
                if (!notViaMatch(&e)) continue;
                if (!waypointsMatch(&e)) continue;

                int s = scoreEntry(&e);
                if (!best || s > bestScore) { best = &e; bestScore = s; }
            }
            };

        auto consider_volume = [&](const std::vector<LOAEntry>& list) {
            for (const auto& e : list) {
                if (!isVolumeLoaEntry(&e)) continue;
                if (isExcludedDest(e) || isExcludedOrigin(e)) continue;
                if (isSourceSectorSuppressed(e)) continue;
                if (!e.nextSectors.empty() && !shouldMatchLOA(e.nextSectors)) continue;
                if (!airportMatch(&e)) continue;
                if (!runwayMatch(&e)) continue;
                if (!passesFinalAltitudeGate(&e)) continue;
                if (!volumesMatch(&e)) continue;
                if (!notViaMatch(&e)) continue;
                if (!waypointsMatch(&e)) continue;

                int s = scoreEntry(&e);
                if (!best || s > bestScore) { best = &e; bestScore = s; }
            }
            };

        // Priority: Destination -> Departure -> Volume
        consider_nonvolume(destinationLoas);
        if (!best) consider_nonvolume(departureLoas);
        if (!best) {
            consider_volume(destinationLoas);
            if (!best) consider_volume(departureLoas);
        }
    }
    // -----------------------------------------------------------------------------

    // -------------------- Fallback pass (only if nothing matched) --------------------
    if (!best) {
        // Reuse airportMatch (no waypoint checks for fallbacks)
        auto scoreFallback = [&](const LOAEntry& e, bool isDepartureList)->int {
            int s = 0;
            if (!isDepartureList) s += 20; // prefer destination fallback slightly

            // ownership/priority tie-breaks (same as main scoring)
            // mySector is computed once per match call
            if (!e.nextSectors.empty()) {
                for (const auto& next : e.nextSectors) {
                    std::string actual = resolveController(next);
                    if (!actual.empty()) {
                        if (_stricmp(actual.c_str(), mySector.c_str()) == 0) s -= 10000;
                        else {
                            auto prIt = plugin.sectorPriority.find(next);
                            if (prIt != plugin.sectorPriority.end()) {
                                const auto& prio = prIt->second;
                                auto meIt = std::find(prio.begin(), prio.end(), mySector);
                                auto himIt = std::find(prio.begin(), prio.end(), actual);
                                if (meIt != prio.end() && himIt != prio.end() && himIt < meIt) s += 50;
                            }
                        }
                        break;
                    }
                }
            }

            if (!e.copText.empty()) s += 5;
            s += e.xfl; // higher XFL slight tie-break
            return s;
            };

        const LOAEntry* bestDestFB = nullptr; int bestDestFBScore = INT_MIN;
        for (const auto& e : destinationFallbackLoas) {
            if (isExcludedDest(e) || isExcludedOrigin(e)) continue;
            if (isSourceSectorSuppressed(e)) continue;                   // ownership suppression
            if (!e.nextSectors.empty() && !shouldMatchLOA(e.nextSectors)) continue;
            if (!airportMatch(&e)) continue;                             // ONLY airport constraints; no waypoints
            if (!runwayMatch(&e)) continue;
            if (!passesFinalAltitudeGate(&e)) continue;
            if (!notViaMatch(&e)) continue;
            int s = scoreFallback(e, /*isDepartureList=*/false);
            if (!bestDestFB || s > bestDestFBScore) { bestDestFB = &e; bestDestFBScore = s; }
        }

        const LOAEntry* bestDepFB = nullptr; int bestDepFBScore = INT_MIN;
        for (const auto& e : departureFallbackLoas) {
            if (isSourceSectorSuppressed(e)) continue;
            if (!e.nextSectors.empty() && !shouldMatchLOA(e.nextSectors)) continue;
            if (!airportMatch(&e)) continue;                             // ONLY airport constraints; no waypoints
            if (!notViaMatch(&e)) continue;
            int s = scoreFallback(e, /*isDepartureList=*/true);
            if (!bestDepFB || s > bestDepFBScore) { bestDepFB = &e; bestDepFBScore = s; }
        }

        // Strict priority: destination fallback before departure fallback
        if (bestDestFB) best = bestDestFB;
        else if (bestDepFB) best = bestDepFB;
    }
    // -------------------------------------------------------------------------------

    plugin.matchedLOACache[callsign] = best;
    plugin.matchTimestamps[callsign] = now;
    plugin.matchVersions[callsign] = plugin.sectorControlVersion;
    return best;
}