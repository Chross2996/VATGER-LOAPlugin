#include "stdafx.h"
#include "LOAPlugin.h"
#include <string>
#include <algorithm>
#include <climits>      // for INT_MIN
#include <unordered_map>

#define DEBUG_MSG(title, msg) plugin.DisplayUserMessage("LOA DEBUG", title, msg, true, true, false, false, false);

// Tagged/Untagged XFL Tag Item
void RenderXFLTagItem(
    EuroScopePlugIn::CFlightPlan flightPlan,
    EuroScopePlugIn::CRadarTarget radarTarget,
    int tagData,
    char sItemString[16],
    int* pColorCode,
    COLORREF* pRGB,
    double* pFontSize)
{
    if (!radarTarget.IsValid()) return;
    EuroScopePlugIn::CFlightPlan correlated = radarTarget.GetCorrelatedFlightPlan();
    if (!correlated.IsValid()) return;
    if (flightPlan.GetState() != FLIGHT_PLAN_STATE_ASSUMED) return;

    const auto& fpd = flightPlan.GetFlightPlanData();
    if (_stricmp(fpd.GetPlanType(), "I") != 0) return;

    if (!radarTarget.IsValid() || !flightPlan.IsValid() || flightPlan.GetState() == FLIGHT_PLAN_STATE_NON_CONCERNED) {
        plugin.coordinationStates.erase(flightPlan.GetCallsign());
        return;
    }

    const auto& data = plugin.lastTagData;
    const std::string& origin = data.origin;
    const std::string& destination = data.destination;
    int clearedAltitude = data.clearedAltitude;
    int finalAltitude = data.finalAltitude;

    const auto& onlineControllers = plugin.GetOnlineControllersCached();
    const auto& routePoints = plugin.GetCachedRoutePoints(flightPlan);

    std::string callsign = flightPlan.GetCallsign();
    int coordXFL = flightPlan.GetExitCoordinationAltitude();
    int coordState = flightPlan.GetExitCoordinationAltitudeState();

    if ((coordState == COORDINATION_STATE_REQUESTED_BY_ME || coordState == COORDINATION_STATE_REQUESTED_BY_OTHER) && coordXFL >= 500) {
        plugin.coordinationStates[callsign].exitAltitude = coordXFL;
        plugin.coordinationStates[callsign].exitAltitudeState = COORDINATION_STATE_REQUESTED_BY_ME;
    }
    if (coordState == COORDINATION_STATE_NONE) {
        const auto it = plugin.coordinationStates.find(callsign);
        if (it != plugin.coordinationStates.end()) {
            const auto& info = it->second;
            if (info.exitAltitude >= 500 && info.exitAltitude == coordXFL && info.exitAltitudeState == COORDINATION_STATE_REQUESTED_BY_ME) {
                snprintf(sItemString, 16, "%03d", coordXFL / 100);
                return;
            }
        }
    }
    if (coordXFL >= 500 && coordState == COORDINATION_STATE_REQUESTED_BY_ME) {
        snprintf(sItemString, 16, "%03d", coordXFL / 100);
        *pColorCode = TAG_COLOR_ONGOING_REQUEST_FROM_ME;
        return;
    }
    if (coordXFL >= 500 && coordState == COORDINATION_STATE_REQUESTED_BY_OTHER) {
        snprintf(sItemString, 16, "%03d", coordXFL / 100);
        *pColorCode = TAG_COLOR_ONGOING_REQUEST_TO_ME;
        return;
    }
    if (coordXFL >= 500 && coordState == COORDINATION_STATE_REFUSED) {
        snprintf(sItemString, 16, "%03d", coordXFL / 100);
        *pColorCode = TAG_COLOR_ONGOING_REQUEST_REFUSED;
        return;
    }

    auto shouldMatchLOA = [&](const std::vector<std::string>& nextSectors) -> bool {
        std::string mySector = plugin.ControllerMyself().GetPositionId();
        std::unordered_map<std::string, std::string> controllerCache;

        for (const std::string& next : nextSectors) {
            // Cached lookup for actual controller
            std::string actualController;
            auto cacheIt = controllerCache.find(next);
            if (cacheIt != controllerCache.end()) {
                actualController = cacheIt->second;
            }
            else {
                actualController = plugin.ResolveControllingSector(next, plugin.currentFrameOnlineControllers);
                controllerCache[next] = actualController;
            }

            bool nextIsDefined = plugin.sectorOwnership.find(next) != plugin.sectorOwnership.end();
            const auto& owned = plugin.sectorOwnership[mySector];
            bool iOwnNext = std::find(owned.begin(), owned.end(), next) != owned.end();

            if (_stricmp(actualController.c_str(), mySector.c_str()) == 0)
                return false;  // I control it

            if (nextIsDefined) {
                if (actualController.empty() && iOwnNext)
                    return false;

                const auto& prioList = plugin.sectorPriority[next];
                auto myPrio = std::find(prioList.begin(), prioList.end(), mySector);
                auto actualPrio = std::find(prioList.begin(), prioList.end(), actualController);

                if (myPrio != prioList.end() && actualPrio != prioList.end() && myPrio < actualPrio)
                    return false;

                return true;
            }

            if (!nextIsDefined && actualController.empty()) {
                return true;  // No one online and it's external — allow
            }

            if (!nextIsDefined && !_stricmp(actualController.c_str(), mySector.c_str())) {
                return false;  // I control it → skip
            }

            return true;  // external and someone else online
        }

        return false;
        };

    auto matches = [&](const LOAEntry& entry) -> bool {
        if (!entry.nextSectors.empty() && !shouldMatchLOA(entry.nextSectors)) return false;

        bool originMatch = entry.originAirports.empty() || plugin.MatchesAirport(entry.originAirportSet, entry.originAirportPrefixes, origin);
        bool destMatch = entry.destinationAirports.empty() || plugin.MatchesAirport(entry.destinationAirportSet, entry.destinationAirportPrefixes, destination);

        bool wpMatch = std::all_of(entry.waypoints.begin(), entry.waypoints.end(),
            [&](const std::string& wp) {
                return std::any_of(routePoints.begin(), routePoints.end(),
                    [&](const std::string& r) { return _stricmp(r.c_str(), wp.c_str()) == 0; });
            });

        return originMatch && destMatch && wpMatch;
        };

    // Suppress LOAs whose *source* sector is controlled by someone who outranks me
    auto isSourceSectorSuppressed = [&](const LOAEntry& e) -> bool {
        if (e.sectors.empty()) return false;  // older entries may lack source tag
        std::string my = plugin.ControllerMyself().GetPositionId();
        std::unordered_map<std::string, std::string> resolveCache;

        for (const auto& src : e.sectors) {
            std::string actual;
            auto it = resolveCache.find(src);
            if (it != resolveCache.end()) actual = it->second;
            else {
                actual = plugin.ResolveControllingSector(src, plugin.currentFrameOnlineControllers);
                resolveCache[src] = actual;
            }

            if (actual.empty()) continue;                         // nobody online → don't suppress
            if (_stricmp(actual.c_str(), my.c_str()) == 0) continue; // I control it → keep

            const auto& prio = plugin.sectorPriority[src];
            auto meIt = std::find(prio.begin(), prio.end(), my);
            auto himIt = std::find(prio.begin(), prio.end(), actual);

            // If the actual controller outranks me on that *source* sector, suppress this LOA
            if (meIt != prio.end() && himIt != prio.end() && himIt < meIt) {
                return true;
            }
        }
        return false;
        };

    // ===== Replace first-match-wins with scoring =====
    auto scoreEntry = [&](const LOAEntry& e, bool isDepartureList) -> int {
        int score = 0;

        // Prefer destination LOAs over departures
        if (!isDepartureList) score += 20;

        // Prefer LOAs whose next sector is actively controlled by someone who outranks me
        std::string mySector = plugin.ControllerMyself().GetPositionId();
        if (!e.nextSectors.empty()) {
            for (const auto& next : e.nextSectors) {
                std::string actual = plugin.ResolveControllingSector(next, plugin.currentFrameOnlineControllers);
                if (!actual.empty()) {
                    if (_stricmp(actual.c_str(), mySector.c_str()) == 0) {
                        // I control it → strongly penalize (should usually be filtered)
                        score -= 10000;
                    }
                    else {
                        const auto& prio = plugin.sectorPriority[next];
                        auto meIt = std::find(prio.begin(), prio.end(), mySector);
                        auto himIt = std::find(prio.begin(), prio.end(), actual);
                        if (meIt != prio.end() && himIt != prio.end() && himIt < meIt) {
                            // actual controller outranks me on 'next'
                            score += 50;
                        }
                    }
                    break; // only need one resolvable next sector to score
                }
            }
        }

        return score;
        };

    const LOAEntry* bestDep = nullptr;
    int             depScore = INT_MIN;
    for (const auto& e : departureLoas) {
        if (isSourceSectorSuppressed(e)) continue;            // FIX: use e, not entry
        if (matches(e)) {
            int s = scoreEntry(e, /*isDepartureList=*/true);
            if (s > depScore) { depScore = s; bestDep = &e; }
        }
    }

    const LOAEntry* bestDest = nullptr;
    int             destScore = INT_MIN;
    for (const auto& e : destinationLoas) {
        if (isSourceSectorSuppressed(e)) continue;            // NEW: suppress higher-priority source sectors
        if (matches(e)) {
            int s = scoreEntry(e, /*isDepartureList=*/false);
            // tie-break by higher XFL for arrivals (helps ABAMI ML@340 beat ABAMI RL@280)
            if (s > destScore || (s == destScore && bestDest && e.xfl > bestDest->xfl)) {
                destScore = s; bestDest = &e;
            }
        }
    }

    const LOAEntry* finalMatch = nullptr;
    bool isDepartureSel = false;
    if (bestDest && (!bestDep || destScore >= depScore)) {
        finalMatch = bestDest;
        isDepartureSel = false;
    }
    else if (bestDep) {
        finalMatch = bestDep;
        isDepartureSel = true;
    }
    // ===== end scoring block =====

    if (finalMatch) {
        const bool isArrivalSel = !finalMatch->destinationAirports.empty();
        const bool isDepartureNow = isDepartureSel;

        // Simple XFL tag policy:
        // - Arrival & CFL <= XFL → blank (suppress final altitude)
        if (isArrivalSel && clearedAltitude <= finalMatch->xfl * 100) {
            sItemString[0] = 0;
            return;
        }

        if ((isDepartureNow && clearedAltitude < finalMatch->xfl * 100 && finalAltitude > finalMatch->xfl * 100) ||
            (isArrivalSel && clearedAltitude > finalMatch->xfl * 100)) {
            snprintf(sItemString, 16, "%d", finalMatch->xfl);
            return;
        }
        else if (clearedAltitude == finalMatch->xfl * 100 || clearedAltitude == finalAltitude) {
            sItemString[0] = 0;
            return;
        }
        else {
            snprintf(sItemString, 16, "%d", finalAltitude / 100);
            return;
        }
    }

    // If no match, but destination is covered in my owned sector LOAs → suppress (blank)
    {
        std::string mySector = plugin.ControllerMyself().GetPositionId();
        const auto& ownedSectors = plugin.sectorOwnership[mySector];
        bool suppressDueToUnmatchedDestination = false;

        for (const auto& loa : destinationLoas) {
            if (plugin.MatchesAirport(loa.destinationAirportSet, loa.destinationAirportPrefixes, destination)) {
                for (const auto& next : loa.nextSectors) {
                    if (std::find(ownedSectors.begin(), ownedSectors.end(), next) != ownedSectors.end()) {
                        suppressDueToUnmatchedDestination = true;
                        break;
                    }
                }
            }
            if (suppressDueToUnmatchedDestination) break;
        }

        if (suppressDueToUnmatchedDestination && clearedAltitude < finalAltitude) {
            sItemString[0] = 0;  // blank in simple tag
            return;
        }
    }

    // Default: show final altitude
    if (clearedAltitude == finalAltitude) {
        sItemString[0] = 0;
    }
    else {
        snprintf(sItemString, 16, "%d", finalAltitude / 100);
    }
}

// ✅ Optimized Detailed Tag — only 1 route extract
void RenderXFLDetailedTagItem(
    EuroScopePlugIn::CFlightPlan flightPlan,
    EuroScopePlugIn::CRadarTarget radarTarget,
    int tagData,
    char sItemString[16],
    int* pColorCode,
    COLORREF* pRGB,
    double* pFontSize)
{
    if (!radarTarget.IsValid()) return;
    EuroScopePlugIn::CFlightPlan correlated = radarTarget.GetCorrelatedFlightPlan();
    if (!correlated.IsValid()) return;
    if (!flightPlan.IsValid() || !plugin.IsLOARelevantState(flightPlan.GetState())) {
        strncpy_s(sItemString, 16, "XFL", _TRUNCATE);
        return;
    }

    if (!radarTarget.IsValid() || !flightPlan.IsValid() || flightPlan.GetState() == FLIGHT_PLAN_STATE_NON_CONCERNED) {
        plugin.coordinationStates.erase(flightPlan.GetCallsign());
        return;
    }

    const auto& fpd = flightPlan.GetFlightPlanData();
    const char* planType = fpd.GetPlanType();
    if (_stricmp(planType, "I") != 0) {
        strncpy_s(sItemString, 16, "XFL", _TRUNCATE);
        return;
    }

    const auto& data = plugin.lastTagData;
    const std::string& callsign = data.callsign;
    int clearedAltitude = data.clearedAltitude;
    int finalAltitude = data.finalAltitude;
    const std::string& origin = data.origin;
    const std::string& destination = data.destination;

    const auto& onlineControllers = plugin.currentFrameOnlineControllers;
    const auto& routePoints = plugin.currentFrameRoutePoints;

    int coordXFL = flightPlan.GetExitCoordinationAltitude();
    int coordState = flightPlan.GetExitCoordinationAltitudeState();

    if ((coordState == COORDINATION_STATE_REQUESTED_BY_ME || coordState == COORDINATION_STATE_REQUESTED_BY_OTHER) && coordXFL >= 500) {
        plugin.coordinationStates[callsign].exitAltitude = coordXFL;
        plugin.coordinationStates[callsign].exitAltitudeState = COORDINATION_STATE_REQUESTED_BY_ME;
    }
    if (coordState == COORDINATION_STATE_NONE) {
        const auto it = plugin.coordinationStates.find(callsign);
        if (it != plugin.coordinationStates.end()) {
            const auto& info = it->second;
            if (info.exitAltitude >= 500 && info.exitAltitude == coordXFL && info.exitAltitudeState == COORDINATION_STATE_REQUESTED_BY_ME) {
                snprintf(sItemString, 16, "%03d", coordXFL / 100);
                *pColorCode = TAG_COLOR_ONGOING_REQUEST_ACCEPTED;
                return;
            }
        }
    }
    if (coordXFL >= 500 && coordState == COORDINATION_STATE_REQUESTED_BY_ME) {
        snprintf(sItemString, 16, "%03d", coordXFL / 100);
        *pColorCode = TAG_COLOR_ONGOING_REQUEST_FROM_ME;
        return;
    }
    if (coordXFL >= 500 && coordState == COORDINATION_STATE_REQUESTED_BY_OTHER) {
        snprintf(sItemString, 16, "%03d", coordXFL / 100);
        *pColorCode = TAG_COLOR_ONGOING_REQUEST_TO_ME;
        return;
    }
    if (coordXFL >= 500 && coordState == COORDINATION_STATE_REFUSED) {
        snprintf(sItemString, 16, "%03d", coordXFL / 100);
        *pColorCode = TAG_COLOR_ONGOING_REQUEST_REFUSED;
        return;
    }

    auto shouldMatchLOA = [&](const std::vector<std::string>& nextSectors) -> bool {
        std::string mySector = plugin.ControllerMyself().GetPositionId();

        for (const std::string& next : nextSectors) {
            bool nextIsDefined = plugin.sectorOwnership.find(next) != plugin.sectorOwnership.end();
            const auto& owned = plugin.sectorOwnership[mySector];
            bool iOwnNext = std::find(owned.begin(), owned.end(), next) != owned.end();
            std::string actualController = plugin.ResolveControllingSector(next, plugin.currentFrameOnlineControllers);

            if (_stricmp(actualController.c_str(), mySector.c_str()) == 0)
                return false;  // I control it

            if (nextIsDefined) {
                // Defined sector logic (ownership + priority)
                if (actualController.empty() && iOwnNext)
                    return false;

                const auto& prioList = plugin.sectorPriority[next];
                auto myPrio = std::find(prioList.begin(), prioList.end(), mySector);
                auto actualPrio = std::find(prioList.begin(), prioList.end(), actualController);

                if (myPrio != prioList.end() && actualPrio != prioList.end() && myPrio < actualPrio)
                    return false;

                return true;
            }

            // Fallback: external sector
            if (!nextIsDefined && actualController.empty()) {
                return true;  // No one online and it's external — allow
            }

            if (!nextIsDefined && !_stricmp(actualController.c_str(), mySector.c_str())) {
                return false;  // I control it → skip
            }

            return true;  // external and someone else online
        }

        return false;
        };

    auto matches = [&](const LOAEntry& entry) -> bool {
        if (!entry.nextSectors.empty() && !shouldMatchLOA(entry.nextSectors)) return false;

        bool originMatch = entry.originAirports.empty() || plugin.MatchesAirport(entry.originAirportSet, entry.originAirportPrefixes, origin);
        bool destMatch = entry.destinationAirports.empty() || plugin.MatchesAirport(entry.destinationAirportSet, entry.destinationAirportPrefixes, destination);

        bool wpMatch = std::all_of(entry.waypoints.begin(), entry.waypoints.end(),
            [&](const std::string& wp) {
                return std::any_of(routePoints.begin(), routePoints.end(),
                    [&](const std::string& r) { return _stricmp(r.c_str(), wp.c_str()) == 0; });
            });

        return originMatch && destMatch && wpMatch;
        };

    // Suppress LOAs whose *source* sector is controlled by someone who outranks me
    auto isSourceSectorSuppressed = [&](const LOAEntry& e) -> bool {
        if (e.sectors.empty()) return false;  // older entries may lack source tag
        std::string my = plugin.ControllerMyself().GetPositionId();
        std::unordered_map<std::string, std::string> resolveCache;

        for (const auto& src : e.sectors) {
            std::string actual;
            auto it = resolveCache.find(src);
            if (it != resolveCache.end()) actual = it->second;
            else {
                actual = plugin.ResolveControllingSector(src, plugin.currentFrameOnlineControllers);
                resolveCache[src] = actual;
            }

            if (actual.empty()) continue;
            if (_stricmp(actual.c_str(), my.c_str()) == 0) continue;

            const auto& prio = plugin.sectorPriority[src];
            auto meIt = std::find(prio.begin(), prio.end(), my);
            auto himIt = std::find(prio.begin(), prio.end(), actual);
            if (meIt != prio.end() && himIt != prio.end() && himIt < meIt) {
                return true;
            }
        }
        return false;
        };

    // ===== scoring (same policy as simple tag) =====
    auto scoreEntry = [&](const LOAEntry& e, bool isDepartureList) -> int {
        int score = 0;
        if (!isDepartureList) score += 20;

        std::string mySector = plugin.ControllerMyself().GetPositionId();
        if (!e.nextSectors.empty()) {
            for (const auto& next : e.nextSectors) {
                std::string actual = plugin.ResolveControllingSector(next, plugin.currentFrameOnlineControllers);
                if (!actual.empty()) {
                    if (_stricmp(actual.c_str(), mySector.c_str()) == 0) {
                        score -= 10000;
                    }
                    else {
                        const auto& prio = plugin.sectorPriority[next];
                        auto meIt = std::find(prio.begin(), prio.end(), mySector);
                        auto himIt = std::find(prio.begin(), prio.end(), actual);
                        if (meIt != prio.end() && himIt != prio.end() && himIt < meIt) {
                            score += 50;
                        }
                    }
                    break;
                }
            }
        }
        return score;
        };

    const LOAEntry* bestDep = nullptr;
    int             depScore = INT_MIN;
    for (const auto& e : departureLoas) {
        if (isSourceSectorSuppressed(e)) continue;
        if (matches(e)) {
            int s = scoreEntry(e, /*isDepartureList=*/true);
            if (s > depScore) { depScore = s; bestDep = &e; }
        }
    }

    const LOAEntry* bestDest = nullptr;
    int             destScore = INT_MIN;
    for (const auto& e : destinationLoas) {
        if (isSourceSectorSuppressed(e)) continue;
        if (matches(e)) {
            int s = scoreEntry(e, /*isDepartureList=*/false);
            if (s > destScore || (s == destScore && bestDest && e.xfl > bestDest->xfl)) {
                destScore = s; bestDest = &e;
            }
        }
    }

    const LOAEntry* finalMatch = nullptr;
    bool isDepartureSel = false;
    if (bestDest && (!bestDep || destScore >= depScore)) {
        finalMatch = bestDest;
        isDepartureSel = false;
    }
    else if (bestDep) {
        finalMatch = bestDep;
        isDepartureSel = true;
    }

    if (finalMatch) {
        const bool isArrivalSel = !finalMatch->destinationAirports.empty();
        const bool isDepartureNow = isDepartureSel;

        // AFTER  ✅ only below XFL shows "XFL"
        if (isArrivalSel && clearedAltitude < finalMatch->xfl * 100) {
            strncpy_s(sItemString, 16, "XFL", _TRUNCATE);
            return;
        }

        if ((isDepartureNow && clearedAltitude < finalMatch->xfl * 100 && finalAltitude > finalMatch->xfl * 100) ||
            (isArrivalSel && clearedAltitude > finalMatch->xfl * 100)) {
            strncpy_s(sItemString, 16, std::to_string(finalMatch->xfl).c_str(), _TRUNCATE);
            return;
        }
        else if (clearedAltitude == finalAltitude) {
            // If CFL equals the final altitude, show final altitude
            strncpy_s(sItemString, 16, std::to_string(finalAltitude / 100).c_str(), _TRUNCATE);
            return;
        }
        else if (clearedAltitude == finalMatch->xfl * 100) {
            // If CFL equals XFL, always show the XFL value
            strncpy_s(sItemString, 16, std::to_string(finalMatch->xfl).c_str(), _TRUNCATE);
            return;
        }
    }

    // LOR passes
    for (const auto& e : lorDepartures) {
        if (isSourceSectorSuppressed(e)) continue;
        if (matches(e)) {
            if (clearedAltitude <= e.xfl * 100 && finalAltitude > e.xfl * 100) {
                strncpy_s(sItemString, 16, std::to_string(e.xfl).c_str(), _TRUNCATE);
                return;
            }
            else {
                strncpy_s(sItemString, 16, std::to_string(finalAltitude / 100).c_str(), _TRUNCATE);
                return;
            }
        }
    }

    for (const auto& e : lorArrivals) {
        if (isSourceSectorSuppressed(e)) continue;
        if (matches(e)) {
            if (clearedAltitude < e.xfl * 100) {
                strncpy_s(sItemString, 16, "XFL", _TRUNCATE);
                return;
            }
            else {
                strncpy_s(sItemString, 16, std::to_string(e.xfl).c_str(), _TRUNCATE);
                return;
            }
        }
    }

    // Fallback LOAs
    if (!fallbackLoas.empty()) {
        for (const auto& e : fallbackLoas) {
            if (isSourceSectorSuppressed(e)) continue;
            if (clearedAltitude < e.minAltitudeFt) continue;
            if (!e.destinationAirports.empty() &&
                !plugin.MatchesAirport(e.destinationAirportSet, e.destinationAirportPrefixes, destination)) {
                continue;
            }

            bool wpMatch = std::all_of(e.waypoints.begin(), e.waypoints.end(),
                [&](const std::string& wp) {
                    return std::any_of(routePoints.begin(), routePoints.end(),
                        [&](const std::string& r) { return _stricmp(r.c_str(), wp.c_str()) == 0; });
                });

            if (wpMatch) {
                strncpy_s(sItemString, 16, std::to_string(finalAltitude / 100).c_str(), _TRUNCATE);
                return;
            }
        }
    }

    strncpy_s(sItemString, 16, std::to_string(finalAltitude / 100).c_str(), _TRUNCATE);
}