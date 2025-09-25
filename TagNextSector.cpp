// ==============================
// File: TagNextSector.cpp
// ==============================

#include "stdafx.h"
#include "LOAPlugin.h"
#include <unordered_set>
#include <string>
#include <windows.h>
#include <algorithm>

void RenderNextSectorTagItem(
    EuroScopePlugIn::CFlightPlan flightPlan,
    EuroScopePlugIn::CRadarTarget radarTarget,
    int tagData,
    char sItemString[16],
    int* pColorCode,
    COLORREF* pRGB,
    double* pFontSize)
{
    if (!flightPlan.IsValid()) return;

    // ✅ LOA-relevant states only
    if (!plugin.IsLOARelevantState(flightPlan.GetState())) return;

    // ✅ IFR flight plans only
    const char* planType = flightPlan.GetFlightPlanData().GetPlanType();
    if (_stricmp(planType, "I") != 0) return;

    int clearedAltitude = flightPlan.GetClearedAltitude();   // ft
    int finalAltitude = flightPlan.GetFinalAltitude();       // ft
    std::string origin = flightPlan.GetFlightPlanData().GetOrigin();
    std::string destination = flightPlan.GetFlightPlanData().GetDestination();
    std::string controller = flightPlan.GetTrackingControllerId();

    const auto& onlineControllers = plugin.GetOnlineControllersCached();

    auto route = flightPlan.GetExtractedRoute();
    std::vector<std::string> routePoints;
    for (int i = 0; i < route.GetPointsNumber(); ++i)
        routePoints.emplace_back(route.GetPointName(i));

    auto matches = [&](const LOAEntry& entry) -> bool {
        if (entry.requireNextSectorOnline) {
            bool nextOnline = std::any_of(entry.nextSectors.begin(), entry.nextSectors.end(),
                [&](const std::string& s) {
                    return onlineControllers.count(s) > 0;
                });
            if (!nextOnline) return false;
        }

        bool originMatch = entry.originAirports.empty() || plugin.MatchesAirport(entry.originAirports, origin);
        bool destMatch = entry.destinationAirports.empty() || plugin.MatchesAirport(entry.destinationAirports, destination);

        bool wpMatch = true;
        for (const std::string& wp : entry.waypoints) {
            auto it = std::find_if(routePoints.begin(), routePoints.end(),
                [&](const std::string& r) { return _stricmp(r.c_str(), wp.c_str()) == 0; });
            if (it == routePoints.end()) {
                wpMatch = false;
                break;
            }
        }

        return originMatch && destMatch && wpMatch;
        };

    // ✅ Check Departure LOAs
    for (const auto& entry : departureLoas) {
        if (matches(entry)) {
            if ((clearedAltitude < entry.xfl * 100 && finalAltitude > entry.xfl * 100) ||
                (clearedAltitude > entry.xfl * 100)) {
                if (!entry.nextSectors.empty()) {
                    const std::string& next = entry.nextSectors.front();
                    if (onlineControllers.count(next)) {
                        strncpy_s(sItemString, 16, next.c_str(), _TRUNCATE);
                        return;
                    }
                }
            }
        }
    }

    // ✅ Check Destination LOAs
    for (const auto& entry : destinationLoas) {
        if (matches(entry)) {
            if (clearedAltitude > entry.xfl * 100) {
                if (!entry.nextSectors.empty()) {
                    const std::string& next = entry.nextSectors.front();
                    if (onlineControllers.count(next)) {
                        strncpy_s(sItemString, 16, next.c_str(), _TRUNCATE);
                        return;
                    }
                }
            }
        }
    }

    // ❌ Still nothing useful
    strncpy_s(sItemString, 16, "-", _TRUNCATE);
}