#include "stdafx.h"
#include "LOAPlugin.h"
#include <string>
#include <algorithm>
#include <climits>      // for INT_MIN
#include <unordered_map>
#include <unordered_set>

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
    // Validity only
    if (!flightPlan.IsValid()) {
        sItemString[0] = '\0';
        return;
    }

    // ✅ Normal XFL tag only when ASSUMED
    if (flightPlan.GetState() != FLIGHT_PLAN_STATE_ASSUMED) {
        sItemString[0] = '\0';
        return;
    }

    const auto& fpd = flightPlan.GetFlightPlanData();
    if (_stricmp(fpd.GetPlanType(), "I") != 0) { // IFR only
        sItemString[0] = '\0';
        return;
    }

    // Use per-frame cached data prepared in LOAPlugin::OnGetTagItem
    const auto& data = plugin.lastTagData;
    const LOAEntry* matched = plugin.currentFrameMatchedEntry;
    const std::string& callsign = data.callsign;
    int clearedAltitude = data.clearedAltitude;
    int finalAltitude = data.finalAltitude;

    // If destination is in AOR but none of the AOR host sectors (e.g., HAM/HAMW) are online,
   // hide the normal XFL tag. If HAM is online, do NOT hide (show normal LOA behavior).
    if (plugin.IsAORDestination(plugin.lastTagData.destination) &&
        !plugin.IsAnyAORHostOnline(plugin.currentFrameOnlineControllers)) {
        sItemString[0] = '\0';
        return;
    }

    // ---------------- Coordination overrides (unchanged) ----------------
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
                _snprintf_s(sItemString, 16, _TRUNCATE, "%03d", coordXFL / 100);
                return;
            }
        }
    }
    if (coordXFL >= 500 && coordState == COORDINATION_STATE_REQUESTED_BY_ME) {
        _snprintf_s(sItemString, 16, _TRUNCATE, "%03d", coordXFL / 100);
        if (pColorCode) *pColorCode = TAG_COLOR_ONGOING_REQUEST_FROM_ME;
        return;
    }
    if (coordXFL >= 500 && coordState == COORDINATION_STATE_REQUESTED_BY_OTHER) {
        _snprintf_s(sItemString, 16, _TRUNCATE, "%03d", coordXFL / 100);
        if (pColorCode) *pColorCode = TAG_COLOR_ONGOING_REQUEST_TO_ME;
        return;
    }
    if (coordXFL >= 500 && coordState == COORDINATION_STATE_REFUSED) {
        _snprintf_s(sItemString, 16, _TRUNCATE, "%03d", coordXFL / 100);
        if (pColorCode) *pColorCode = TAG_COLOR_ONGOING_REQUEST_REFUSED;
        return;
    }
    // --------------------------------------------------------------------

    // ---------- Simple XFL tag policy using the cached match only ----------
    if (matched) {
        const bool isDeparture = !matched->originAirports.empty();
        const bool isArrival = !matched->destinationAirports.empty();
        const int  xflFeet = matched->xfl * 100;

        if (isArrival) {
            // Arrival (simple tag):
            // - if CFL <= XFL → blank
            // - if CFL >  XFL → show XFL value
            if (clearedAltitude <= xflFeet) {
                sItemString[0] = '\0';
                return;
            }
            else {
                _snprintf_s(sItemString, 16, _TRUNCATE, "%d", matched->xfl);
                return;
            }
        }

        // Departure (simple tag):
        // - if CFL < XFL and FNL > XFL → show XFL
        // - if CFL == XFL → blank
        // - if CFL > XFL  → show final altitude
        if (isDeparture) {
            if (clearedAltitude < xflFeet && finalAltitude > xflFeet) {
                _snprintf_s(sItemString, 16, _TRUNCATE, "%d", matched->xfl);
                return;
            }
            if (clearedAltitude == xflFeet) {
                sItemString[0] = '\0';
                return;
            }
            if (clearedAltitude > xflFeet) {
                if (clearedAltitude == finalAltitude) {
                    sItemString[0] = '\0';
                }
                else {
                    _snprintf_s(sItemString, 16, _TRUNCATE, "%d", finalAltitude / 100);
                }
                return;
            }
        }

        // Generic fall-through for departure when above did not return:
        if (clearedAltitude == finalAltitude) {
            sItemString[0] = '\0';
            return;
        }
        _snprintf_s(sItemString, 16, _TRUNCATE, "%d", finalAltitude / 100);
        return;
    }
    // -----------------------------------------------------------------------

    // No LOA match: default to final altitude display policy
    if (clearedAltitude == finalAltitude) {
        sItemString[0] = '\0';
    }
    else {
        _snprintf_s(sItemString, 16, _TRUNCATE, "%d", finalAltitude / 100);
    }
}

// ✅ Detailed XFL tag — prefer the cached per-frame LOA match; only re-match if none cached
void RenderXFLDetailedTagItem(
    EuroScopePlugIn::CFlightPlan flightPlan,
    EuroScopePlugIn::CRadarTarget radarTarget,
    int tagData,
    char sItemString[16],
    int* pColorCode,
    COLORREF* pRGB,
    double* pFontSize)
{

    if (!flightPlan.IsValid() || !plugin.IsLOARelevantState(flightPlan.GetState())) {
        strncpy_s(sItemString, 16, "XFL", _TRUNCATE);
        return;
    }

    // keep this cleanup for non-concerned frames
    if (flightPlan.GetState() == FLIGHT_PLAN_STATE_NON_CONCERNED) {
        plugin.coordinationStates.erase(flightPlan.GetCallsign());
        return;
    }

    const auto& fpd = flightPlan.GetFlightPlanData();
    if (_stricmp(fpd.GetPlanType(), "I") != 0) {
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

    // If destination is in AOR and no AOR host sector is online, force literal "XFL".
   // If HAM/HAMW is online, skip this override so LOAs render normally.
    if (plugin.IsAORDestination(plugin.lastTagData.destination) &&
        !plugin.IsAnyAORHostOnline(plugin.currentFrameOnlineControllers)) {
        strncpy_s(sItemString, 16, "XFL", _TRUNCATE);
        return;
    }

    // ---------------- Coordination handling (unchanged) ----------------
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
                _snprintf_s(sItemString, 16, _TRUNCATE, "%03d", coordXFL / 100);
                if (pColorCode) *pColorCode = TAG_COLOR_ONGOING_REQUEST_ACCEPTED;
                return;
            }
        }
    }
    if (coordXFL >= 500 && coordState == COORDINATION_STATE_REQUESTED_BY_ME) {
        _snprintf_s(sItemString, 16, _TRUNCATE, "%03d", coordXFL / 100);
        if (pColorCode) *pColorCode = TAG_COLOR_ONGOING_REQUEST_FROM_ME;
        return;
    }
    if (coordXFL >= 500 && coordState == COORDINATION_STATE_REQUESTED_BY_OTHER) {
        _snprintf_s(sItemString, 16, _TRUNCATE, "%03d", coordXFL / 100);
        if (pColorCode) *pColorCode = TAG_COLOR_ONGOING_REQUEST_TO_ME;
        return;
    }
    if (coordXFL >= 500 && coordState == COORDINATION_STATE_REFUSED) {
        _snprintf_s(sItemString, 16, _TRUNCATE, "%03d", coordXFL / 100);
        if (pColorCode) *pColorCode = TAG_COLOR_ONGOING_REQUEST_REFUSED;
        return;
    }
    // -------------------------------------------------------------------

    // === Prefer the cached per-frame match; only re-scan if missing ===
    const LOAEntry* finalMatch = plugin.currentFrameMatchedEntry;
    bool            isDepartureSel = (finalMatch && !finalMatch->originAirports.empty());

    auto shouldMatchLOA = [&](const std::vector<std::string>& nextSectors) -> bool {
        std::string mySector = plugin.ControllerMyself().GetPositionId();

        for (const std::string& next : nextSectors) {
            bool nextIsDefined = plugin.sectorOwnership.find(next) != plugin.sectorOwnership.end();
            const auto& owned = plugin.sectorOwnership[mySector];
            bool iOwnNext = std::find(owned.begin(), owned.end(), next) != owned.end();
            std::string actualController = plugin.ResolveControllingSector(next, onlineControllers);

            if (_stricmp(actualController.c_str(), mySector.c_str()) == 0)
                return false;  // I control it

            if (nextIsDefined) {
                if (actualController.empty() && iOwnNext)
                    return false;

                const auto& prioList = plugin.sectorPriority[next];
                auto myPrio = std::find(prioList.begin(), prioList.end(), mySector);
                auto actualPrio = std::find(prioList.begin(), prioList.end(), actualController);
                if (myPrio != prioList.end() && actualPrio != prioList.end() && actualPrio < myPrio)
                    return false;

                return true;
            }

            // external sector
            if (!nextIsDefined && actualController.empty()) {
                return true;  // no one online there → allow
            }
            if (!nextIsDefined && !_stricmp(actualController.c_str(), mySector.c_str())) {
                return false; // I control it
            }
            return true;      // external and someone else online
        }

        return false;
        };

    auto matches = [&](const LOAEntry& entry) -> bool {
        if (!entry.nextSectors.empty() && !shouldMatchLOA(entry.nextSectors)) return false;

        bool originMatch = entry.originAirports.empty() ||
            plugin.MatchesAirport(entry.originAirportSet, entry.originAirportPrefixes, origin);
        bool destMatch = entry.destinationAirports.empty() ||
            plugin.MatchesAirport(entry.destinationAirportSet, entry.destinationAirportPrefixes, destination);

        bool wpMatch = std::all_of(entry.waypoints.begin(), entry.waypoints.end(),
            [&](const std::string& wp) {
                return std::any_of(routePoints.begin(), routePoints.end(),
                    [&](const std::string& r) { return _stricmp(r.c_str(), wp.c_str()) == 0; });
            });

        return originMatch && destMatch && wpMatch;
        };

    auto isSourceSectorSuppressed = [&](const LOAEntry& e) -> bool {
        if (e.sectors.empty()) return false;
        std::string my = plugin.ControllerMyself().GetPositionId();
        std::unordered_map<std::string, std::string> resolveCache;

        for (const auto& src : e.sectors) {
            std::string actual;
            auto it = resolveCache.find(src);
            if (it != resolveCache.end()) actual = it->second;
            else {
                actual = plugin.ResolveControllingSector(src, onlineControllers);
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

    auto scoreEntry = [&](const LOAEntry& e, bool isDepartureList) -> int {
        int score = 0;
        if (!isDepartureList) score += 20;

        std::string mySector = plugin.ControllerMyself().GetPositionId();
        if (!e.nextSectors.empty()) {
            for (const auto& next : e.nextSectors) {
                std::string actual = plugin.ResolveControllingSector(next, onlineControllers);
                if (!actual.empty()) {
                    if (_stricmp(actual.c_str(), mySector.c_str()) == 0) {
                        score -= 10000; // I control it → strongly deprioritize
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

    // Only re-match if cached is missing
    if (!finalMatch) {
        const LOAEntry* bestDep = nullptr; int depScore = INT_MIN;
        const LOAEntry* bestDest = nullptr; int destScore = INT_MIN;

        for (const auto& e : departureLoas) {
            if (isSourceSectorSuppressed(e)) continue;
            if (matches(e)) {
                int s = scoreEntry(e, /*isDepartureList=*/true);
                if (s > depScore) { depScore = s; bestDep = &e; }
            }
        }
        for (const auto& e : destinationLoas) {
            if (isSourceSectorSuppressed(e)) continue;
            if (matches(e)) {
                int s = scoreEntry(e, /*isDepartureList=*/false);
                if (s > destScore || (s == destScore && bestDest && e.xfl > bestDest->xfl)) {
                    destScore = s; bestDest = &e;
                }
            }
        }

        if (bestDest && (!bestDep || destScore >= depScore)) {
            finalMatch = bestDest;
            isDepartureSel = false;
        }
        else if (bestDep) {
            finalMatch = bestDep;
            isDepartureSel = true;
        }
    }

    // ---- Display using finalMatch (detailed rules) ----
    if (finalMatch) {
        const bool isArrivalSel = !finalMatch->destinationAirports.empty();
        const int  xflFeet = finalMatch->xfl * 100;

        // AFTER: Only below XFL shows "XFL" on arrivals
        if (isArrivalSel && clearedAltitude < xflFeet) {
            strncpy_s(sItemString, 16, "XFL", _TRUNCATE);
            return;
        }

        if ((isDepartureSel && clearedAltitude < xflFeet && finalAltitude > xflFeet) ||
            (isArrivalSel && clearedAltitude > xflFeet)) {
            strncpy_s(sItemString, 16, std::to_string(finalMatch->xfl).c_str(), _TRUNCATE);
            return;
        }
        else if (clearedAltitude == finalAltitude) {
            strncpy_s(sItemString, 16, std::to_string(finalAltitude / 100).c_str(), _TRUNCATE);
            return;
        }
        else if (clearedAltitude == xflFeet) {
            strncpy_s(sItemString, 16, std::to_string(finalMatch->xfl).c_str(), _TRUNCATE);
            return;
        }
    }

    // Default: show RFL
    strncpy_s(sItemString, 16, std::to_string(finalAltitude / 100).c_str(), _TRUNCATE);
}