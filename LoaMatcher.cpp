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

const LOAEntry* MatchLoaEntry(const EuroScopePlugIn::CFlightPlan& fp,
    const std::unordered_set<std::string>& /*onlineControllers*/)
{
    if (!fp.IsValid() || !plugin.IsLOARelevantState(fp.GetState())) return nullptr;

    const char* planType = fp.GetFlightPlanData().GetPlanType();
    if (_stricmp(planType, "I") != 0) return nullptr;

    const std::string callsign = fp.GetCallsign();
    ULONGLONG now = GetTickCount64();

    // 10s cache + sectorControlVersion
    auto tsIt = plugin.matchTimestamps.find(callsign);
    auto verIt = plugin.matchVersions.find(callsign);
    if (tsIt != plugin.matchTimestamps.end() &&
        verIt != plugin.matchVersions.end() &&
        now - tsIt->second < 10000 &&
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
        std::string mySector = plugin.ControllerMyself().GetPositionId();
        std::unordered_map<std::string, std::string> controllerCache;

        for (const std::string& next : nextSectors) {
            std::string actualController;
            auto cached = controllerCache.find(next);
            if (cached != controllerCache.end()) actualController = cached->second;
            else {
                actualController = plugin.ResolveControllingSector(next, plugin.currentFrameOnlineControllers);
                controllerCache[next] = actualController;
            }

            bool nextIsDefined = plugin.sectorOwnership.find(next) != plugin.sectorOwnership.end();
            const auto& owned = plugin.sectorOwnership[mySector];
            bool iOwnNext = std::find(owned.begin(), owned.end(), next) != owned.end();

            if (_stricmp(actualController.c_str(), mySector.c_str()) == 0) return false;

            if (nextIsDefined) {
                if (actualController.empty() && iOwnNext) return false;

                const auto& prioList = plugin.sectorPriority[next];
                auto myPrio = std::find(prioList.begin(), prioList.end(), mySector);
                auto otherPrio = std::find(prioList.begin(), prioList.end(), actualController);
                if (myPrio != prioList.end() && otherPrio != prioList.end() && myPrio < otherPrio) return false;

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
        std::string my = plugin.ControllerMyself().GetPositionId();
        std::unordered_map<std::string, std::string> resolveCache;

        for (const auto& src : e.sectors) {
            std::string actual;
            auto it2 = resolveCache.find(src);
            if (it2 != resolveCache.end()) actual = it2->second;
            else {
                actual = plugin.ResolveControllingSector(src, plugin.currentFrameOnlineControllers);
                resolveCache[src] = actual;
            }
            if (actual.empty()) continue;
            if (_stricmp(actual.c_str(), my.c_str()) == 0) continue;

            const auto& prio = plugin.sectorPriority[src];
            auto meIt = std::find(prio.begin(), prio.end(), my);
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

    auto waypointsMatch = [&](const LOAEntry* e)->bool {
        for (const auto& wp : e->waypoints) {
            std::string lwp = wp;
            std::transform(lwp.begin(), lwp.end(), lwp.begin(), ::tolower);
            if (routeSet.count(lwp) == 0) return false;
        }
        return true;
        };

    auto scoreEntry = [&](const LOAEntry* e)->int {
        int score = 0;

        // prefer destination over departure
        if (!e->destinationAirports.empty()) score += 20;

        // priority on next sectors
        std::string mySector = plugin.ControllerMyself().GetPositionId();
        if (!e->nextSectors.empty()) {
            for (const auto& next : e->nextSectors) {
                std::string actual = plugin.ResolveControllingSector(next, plugin.currentFrameOnlineControllers);
                if (!actual.empty()) {
                    if (_stricmp(actual.c_str(), mySector.c_str()) == 0) score -= 10000;
                    else {
                        const auto& prio = plugin.sectorPriority[next];
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
    for (const std::string& wp : plugin.GetCachedRoutePoints(fp)) {
        std::string lwp = wp; std::transform(lwp.begin(), lwp.end(), lwp.begin(), ::tolower);
        auto it = plugin.indexByWaypoint.find(lwp);
        if (it != plugin.indexByWaypoint.end()) {
            candidates.insert(it->second.begin(), it->second.end());
        }
    }
    // Also consider entries with no waypoints (rare)
    // (We skip global scan for perf; those should still have at least one wpt to be indexed.)

    const LOAEntry* best = nullptr;
    int bestScore = INT_MIN;

    for (const LOAEntry* e : candidates) {
        if (!e) continue;

        if (isExcludedDest(*e)) continue;
        if (isSourceSectorSuppressed(*e)) continue;
        if (!e->nextSectors.empty() && !shouldMatchLOA(e->nextSectors)) continue;
        if (!airportMatch(e)) continue;
        if (!waypointsMatch(e)) continue;

        int s = scoreEntry(e);
        if (!best || s > bestScore) {
            best = e;
            bestScore = s;
        }
    }

    // ---- NEW: Slow normal scan (destination then departure) before any fallback ----
    if (!best) {
        auto consider_normal = [&](const std::vector<LOAEntry>& list) {
            for (const auto& e : list) {
                if (isExcludedDest(e) || isExcludedOrigin(e)) continue;
                if (isSourceSectorSuppressed(e)) continue;
                if (!e.nextSectors.empty() && !shouldMatchLOA(e.nextSectors)) continue;
                if (!airportMatch(&e)) continue;
                if (!waypointsMatch(&e)) continue;

                int s = scoreEntry(&e);
                if (!best || s > bestScore) { best = &e; bestScore = s; }
            }
            };

        consider_normal(destinationLoas);
        if (!best) consider_normal(departureLoas);
    }
    // -----------------------------------------------------------------------------

    // -------------------- Fallback pass (only if nothing matched) --------------------
    if (!best) {
        // Reuse airportMatch (no waypoint checks for fallbacks)
        auto scoreFallback = [&](const LOAEntry& e, bool isDepartureList)->int {
            int s = 0;
            if (!isDepartureList) s += 20; // prefer destination fallback slightly

            // ownership/priority tie-breaks (same as main scoring)
            std::string mySector = plugin.ControllerMyself().GetPositionId();
            if (!e.nextSectors.empty()) {
                for (const auto& next : e.nextSectors) {
                    std::string actual = plugin.ResolveControllingSector(next, plugin.currentFrameOnlineControllers);
                    if (!actual.empty()) {
                        if (_stricmp(actual.c_str(), mySector.c_str()) == 0) s -= 10000;
                        else {
                            const auto& prio = plugin.sectorPriority[next];
                            auto meIt = std::find(prio.begin(), prio.end(), mySector);
                            auto himIt = std::find(prio.begin(), prio.end(), actual);
                            if (meIt != prio.end() && himIt != prio.end() && himIt < meIt) s += 50;
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
            int s = scoreFallback(e, /*isDepartureList=*/false);
            if (!bestDestFB || s > bestDestFBScore) { bestDestFB = &e; bestDestFBScore = s; }
        }

        const LOAEntry* bestDepFB = nullptr; int bestDepFBScore = INT_MIN;
        for (const auto& e : departureFallbackLoas) {
            if (isSourceSectorSuppressed(e)) continue;
            if (!e.nextSectors.empty() && !shouldMatchLOA(e.nextSectors)) continue;
            if (!airportMatch(&e)) continue;                             // ONLY airport constraints; no waypoints
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