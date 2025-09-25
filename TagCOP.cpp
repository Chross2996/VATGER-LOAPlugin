// =========================
// File: TagCOP.cpp
// =========================

#include "stdafx.h"
#include "LOAPlugin.h"
#include <string>
#include <cstring>
#include <windows.h>
#include <algorithm>
#include <climits>  // for INT_MIN
#include <unordered_map>

void RenderCOPTagItem(
    EuroScopePlugIn::CFlightPlan flightPlan,
    EuroScopePlugIn::CRadarTarget radarTarget,
    int tagData,
    char sItemString[16],
    int* pColorCode,
    COLORREF* pRGB,
    double* pFontSize)
{
    if (!radarTarget.IsValid()) return;

    if (!radarTarget.IsValid() || !flightPlan.IsValid() || flightPlan.GetState() == FLIGHT_PLAN_STATE_NON_CONCERNED) {
        plugin.coordinationStates.erase(flightPlan.GetCallsign());
        strncpy_s(sItemString, 16, "COPX", _TRUNCATE);  // ← ensure default shown
        return;
    }

    EuroScopePlugIn::CFlightPlan correlated = radarTarget.GetCorrelatedFlightPlan();
    if (!correlated.IsValid()) return;

    if (!plugin.IsLOARelevantState(flightPlan.GetState())) {
        strncpy_s(sItemString, 16, "COPX", _TRUNCATE);
        return;
    }

    const auto& fpd = flightPlan.GetFlightPlanData();
    const char* planType = fpd.GetPlanType();
    if (_stricmp(planType, "I") != 0) {
        strncpy_s(sItemString, 16, "COPX", _TRUNCATE);
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

    // COORDINATION LOGIC
    std::string coordCOP = flightPlan.GetExitCoordinationPointName();
    int coordState = flightPlan.GetExitCoordinationNameState();

    if ((coordState == COORDINATION_STATE_REQUESTED_BY_ME || coordState == COORDINATION_STATE_REQUESTED_BY_OTHER) && !coordCOP.empty()) {
        plugin.coordinationStates[callsign].exitPoint = coordCOP;
        plugin.coordinationStates[callsign].exitPointState = COORDINATION_STATE_REQUESTED_BY_ME;
    }

    if (coordState == COORDINATION_STATE_NONE) {
        auto& info = plugin.coordinationStates[callsign];
        if (!info.exitPoint.empty() &&
            info.exitPoint == coordCOP &&
            (info.exitPointState == COORDINATION_STATE_REQUESTED_BY_ME || info.exitPointState == COORDINATION_STATE_REQUESTED_BY_OTHER)) {
            info.exitPointState = COORDINATION_STATE_MANUAL_ACCEPTED;
        }
    }

    const auto it = plugin.coordinationStates.find(callsign);
    if (it != plugin.coordinationStates.end()) {
        const auto& info = it->second;
        if (info.exitPointState == COORDINATION_STATE_MANUAL_ACCEPTED && !info.exitPoint.empty()) {
            strncpy_s(sItemString, 16, info.exitPoint.c_str(), _TRUNCATE);
            *pColorCode = TAG_COLOR_ONGOING_REQUEST_ACCEPTED;
            return;
        }
    }

    if (!coordCOP.empty() && coordState == COORDINATION_STATE_REQUESTED_BY_ME) {
        strncpy_s(sItemString, 16, coordCOP.c_str(), _TRUNCATE);
        *pColorCode = TAG_COLOR_ONGOING_REQUEST_FROM_ME;
        return;
    }
    if (!coordCOP.empty() && coordState == COORDINATION_STATE_REQUESTED_BY_OTHER) {
        strncpy_s(sItemString, 16, coordCOP.c_str(), _TRUNCATE);
        *pColorCode = TAG_COLOR_ONGOING_REQUEST_TO_ME;
        return;
    }
    if (!coordCOP.empty() && coordState == COORDINATION_STATE_REFUSED) {
        strncpy_s(sItemString, 16, "COPX", _TRUNCATE);
        *pColorCode = TAG_COLOR_ONGOING_REQUEST_REFUSED;
        return;
    }

    auto shouldMatchLOA = [&](const std::vector<std::string>& nextSectors) -> bool {
        std::string mySector = plugin.ControllerMyself().GetPositionId();
        std::unordered_map<std::string, std::string> controllerCache;

        for (const std::string& next : nextSectors) {
            std::string actualController;
            auto it = controllerCache.find(next);
            if (it != controllerCache.end()) {
                actualController = it->second;
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

    // NEW: Suppress LOAs whose *source* sector is controlled by someone who outranks me
    auto isSourceSectorSuppressed = [&](const LOAEntry& e) -> bool {
        if (e.sectors.empty()) return false;  // older entries may lack source tag
        std::string my = plugin.ControllerMyself().GetPositionId();
        std::unordered_map<std::string, std::string> resolveCache;

        for (const auto& src : e.sectors) {
            std::string actual;
            auto it2 = resolveCache.find(src);
            if (it2 != resolveCache.end()) {
                actual = it2->second;
            }
            else {
                actual = plugin.ResolveControllingSector(src, plugin.currentFrameOnlineControllers);
                resolveCache[src] = actual;
            }

            if (actual.empty()) continue;                              // nobody online → don't suppress
            if (_stricmp(actual.c_str(), my.c_str()) == 0) continue;   // I control it → keep

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

        // Prefer destination LOAs over departure LOAs
        if (!isDepartureList) score += 20;

        // Prefer LOAs whose next sector is actively controlled by someone who outranks me
        std::string mySector = plugin.ControllerMyself().GetPositionId();
        if (!e.nextSectors.empty()) {
            for (const auto& next : e.nextSectors) {
                std::string actual = plugin.ResolveControllingSector(next, plugin.currentFrameOnlineControllers);
                if (!actual.empty()) {
                    if (_stricmp(actual.c_str(), mySector.c_str()) == 0) {
                        // I control it → strongly penalize (should usually be filtered already)
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

        // Small bonus if a COP text is actually present (useful tie-breaker)
        if (!e.copText.empty()) score += 5;

        return score;
        };

    const LOAEntry* bestDep = nullptr;
    int             depScore = INT_MIN;
    for (const auto& e : departureLoas) {
        if (isSourceSectorSuppressed(e)) continue;  // NEW: suppress ML/RL etc. when PAH (higher prio) is online
        // keep original altitude gate for departures
        if (matches(e) && clearedAltitude <= e.xfl * 100) {
            int s = scoreEntry(e, /*isDepartureList=*/true);
            if (s > depScore) { depScore = s; bestDep = &e; }
        }
    }

    const LOAEntry* bestDest = nullptr;
    int             destScore = INT_MIN;
    for (const auto& e : destinationLoas) {
        if (isSourceSectorSuppressed(e)) continue;  // NEW
        // keep original altitude gate for arrivals
        if (matches(e) && clearedAltitude >= e.xfl * 100) {
            int s = scoreEntry(e, /*isDepartureList=*/false);
            // tie-break: prefer the entry that actually has a COP text
            if (s > destScore || (s == destScore && bestDest && !bestDest->copText.empty() && e.copText.empty())) {
                destScore = s; bestDest = &e;
            }
        }
    }

    const LOAEntry* finalMatch = nullptr;
    if (bestDest && (!bestDep || destScore >= depScore)) {
        finalMatch = bestDest;
    }
    else if (bestDep) {
        finalMatch = bestDep;
    }
    // ===== end scoring block =====

    if (finalMatch && !finalMatch->copText.empty()) {
        strncpy_s(sItemString, 16, finalMatch->copText.c_str(), _TRUNCATE);
        return;
    }

    // Only allow fallback LOA in relevant states
    int state = flightPlan.GetState();
    if (state == FLIGHT_PLAN_STATE_NOTIFIED ||
        state == FLIGHT_PLAN_STATE_COORDINATED ||
        state == FLIGHT_PLAN_STATE_TRANSFER_TO_ME_INITIATED ||
        state == FLIGHT_PLAN_STATE_TRANSFER_FROM_ME_INITIATED ||
        state == FLIGHT_PLAN_STATE_ASSUMED)
    {
        for (const auto& e : fallbackLoas) {
            if (isSourceSectorSuppressed(e)) continue;  // NEW: also suppress fallback entries if source sector is taken
            if (clearedAltitude < e.minAltitudeFt) continue;

            if (!e.destinationAirports.empty() &&
                !plugin.MatchesAirport(e.destinationAirportSet, e.destinationAirportPrefixes, destination)) {
                continue;
            }
            if (!e.nextSectors.empty() && !shouldMatchLOA(e.nextSectors)) {
                continue;
            }

            bool wpMatch = std::all_of(e.waypoints.begin(), e.waypoints.end(),
                [&](const std::string& wp) {
                    return std::any_of(routePoints.begin(), routePoints.end(),
                        [&](const std::string& r) { return _stricmp(r.c_str(), wp.c_str()) == 0; });
                });

            if (wpMatch && !e.copText.empty()) {
                strncpy_s(sItemString, 16, e.copText.c_str(), _TRUNCATE);
                return;
            }
        }
    }

    strncpy_s(sItemString, 16, "COPX", _TRUNCATE);
}