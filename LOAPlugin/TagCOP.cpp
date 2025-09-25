// =========================
// File: TagCOP.cpp
// =========================

#include "stdafx.h"
#include "LOAPlugin.h"
#include <string>
#include <cstring>
#include <windows.h>
#include <algorithm>

void RenderCOPTagItem(
    EuroScopePlugIn::CFlightPlan flightPlan,
    EuroScopePlugIn::CRadarTarget radarTarget,
    int tagData,
    char sItemString[16],
    int* pColorCode,
    COLORREF* pRGB,
    double* pFontSize)
{
    if (!flightPlan.IsValid()) return;

    int clearedAltitude = flightPlan.GetClearedAltitude();
    int finalAltitude = flightPlan.GetFinalAltitude();

    std::string origin = flightPlan.GetFlightPlanData().GetOrigin();
    std::string destination = flightPlan.GetFlightPlanData().GetDestination();
    std::string controller = flightPlan.GetTrackingControllerId();

    auto route = flightPlan.GetExtractedRoute();
    int numPoints = route.GetPointsNumber();
    std::vector<std::string> routePoints;
    for (int i = 0; i < numPoints; ++i)
        routePoints.emplace_back(route.GetPointName(i));

    auto matches = [&](const LOAEntry& entry) -> bool {
        bool sectorMatch = entry.sectors.empty() || std::any_of(entry.sectors.begin(), entry.sectors.end(),
            [&](const std::string& s) { return _stricmp(s.c_str(), controller.c_str()) == 0; });

        bool originMatch = entry.originAirports.empty() || std::any_of(entry.originAirports.begin(), entry.originAirports.end(),
            [&](const std::string& o) { return _stricmp(o.c_str(), origin.c_str()) == 0; });

        bool destMatch = entry.destinationAirports.empty() || std::any_of(entry.destinationAirports.begin(), entry.destinationAirports.end(),
            [&](const std::string& d) { return _stricmp(d.c_str(), destination.c_str()) == 0; });

        bool wpMatch = true;
        for (const std::string& wp : entry.waypoints) {
            auto it = std::find_if(routePoints.begin(), routePoints.end(),
                [&](const std::string& r) { return _stricmp(r.c_str(), wp.c_str()) == 0; });
            if (it == routePoints.end()) {
                wpMatch = false;
                break;
            }
        }

        return sectorMatch && originMatch && destMatch && wpMatch;
        };

    // ✅ Check departure LOAs
    for (const auto& entry : departureLoas) {
        if (matches(entry)) {
            if (!entry.copText.empty() &&
                clearedAltitude < entry.xfl * 100 &&
                finalAltitude > entry.xfl * 100)
            {
                strncpy_s(sItemString, 16, entry.copText.c_str(), _TRUNCATE);
                return;
            }
            else {
                strncpy_s(sItemString, 16, "COPX", _TRUNCATE);
                return;
            }
        }
    }

    // ✅ Check destination LOAs
    for (const auto& entry : destinationLoas) {
        if (matches(entry)) {
            if (!entry.copText.empty() &&
                clearedAltitude > entry.xfl * 100)
            {
                strncpy_s(sItemString, 16, entry.copText.c_str(), _TRUNCATE);
                return;
            }
            else {
                strncpy_s(sItemString, 16, "COPX", _TRUNCATE);
                return;
            }
        }
    }

    // ❌ No LOA match
    strncpy_s(sItemString, 16, "COPX", _TRUNCATE);
}





