#include "stdafx.h"
#include "LOAPlugin.h"
#include <string>
#include <algorithm>

//Tagged/Untagged XFL Tag Item 
void RenderXFLTagItem(
    EuroScopePlugIn::CFlightPlan flightPlan,
    EuroScopePlugIn::CRadarTarget radarTarget,
    int tagData,
    char sItemString[16],
    int* pColorCode,
    COLORREF* pRGB,
    double* pFontSize)
{
    if (!flightPlan.IsValid()) return;

    int clearedAltitude = flightPlan.GetClearedAltitude();   // in feet
    int finalAltitude = flightPlan.GetFinalAltitude();       // in feet

    std::string origin = flightPlan.GetFlightPlanData().GetOrigin();
    std::string destination = flightPlan.GetFlightPlanData().GetDestination();
    std::string controller = flightPlan.GetTrackingControllerId();

    auto route = flightPlan.GetExtractedRoute();
    int numPoints = route.GetPointsNumber();
    std::vector<std::string> routePoints;
    for (int i = 0; i < numPoints; ++i)
        routePoints.emplace_back(route.GetPointName(i));

    auto matches = [&](const LOAEntry& entry) -> bool {
        if (entry.requireNextSectorOnline && !entry.sectors.empty()) {
            bool anyOnline = std::any_of(entry.sectors.begin(), entry.sectors.end(), [&](const std::string& s) {
                return onlineControllers.count(s) > 0;
                });
            if (!anyOnline) return false;
        }

        if (entry.requireNextSectorOnline && !entry.nextSectors.empty()) {
            bool anyOnline = std::any_of(entry.nextSectors.begin(), entry.nextSectors.end(), [&](const std::string& s) {
                return onlineControllers.count(s) > 0;
                });
            if (!anyOnline) return false;
        }

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

    bool departureLoaMatched = false;
    bool destinationLoaMatched = false;

    for (const auto& entry : departureLoas) {
        if (matches(entry)) {
            departureLoaMatched = true;

            if (clearedAltitude < entry.xfl * 100 && finalAltitude > entry.xfl * 100) {
                std::string xflStr = std::to_string(entry.xfl);
                strncpy_s(sItemString, 16, xflStr.c_str(), _TRUNCATE);
                return;
            }
            else if (clearedAltitude > entry.xfl * 100) {
                int finalFL = finalAltitude / 100;
                std::string finalStr = std::to_string(finalFL);
                strncpy_s(sItemString, 16, finalStr.c_str(), _TRUNCATE);
                return;
            }

            return;
        }
    }

    for (const auto& entry : destinationLoas) {
        if (matches(entry)) {
            destinationLoaMatched = true;

            if (clearedAltitude > entry.xfl * 100) {
                std::string xflStr = std::to_string(entry.xfl);
                strncpy_s(sItemString, 16, xflStr.c_str(), _TRUNCATE);
                return;
            }

            return;
        }
    }

    if (!departureLoaMatched && !destinationLoaMatched && clearedAltitude != finalAltitude) {
        int finalFL = finalAltitude / 100;
        std::string fallbackStr = std::to_string(finalFL);
        strncpy_s(sItemString, 16, fallbackStr.c_str(), _TRUNCATE);
        return;
    }

    sItemString[0] = '\0';
}

void RenderXFLDetailedTagItem(
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

    auto onlineControllers = GetOnlineControllers();

    auto route = flightPlan.GetExtractedRoute();
    int numPoints = route.GetPointsNumber();
    std::vector<std::string> routePoints;
    for (int i = 0; i < numPoints; ++i)
        routePoints.emplace_back(route.GetPointName(i));

    auto matches = [&](const LOAEntry& entry) -> bool {
        if (entry.requireNextSectorOnline && !entry.sectors.empty()) {
            bool anyOnline = std::any_of(entry.sectors.begin(), entry.sectors.end(), [&](const std::string& s) {
                return onlineControllers.count(s) > 0;
                });
            if (!anyOnline) return false;
        }

        if (entry.requireNextSectorOnline && !entry.nextSectors.empty()) {
            bool anyOnline = std::any_of(entry.nextSectors.begin(), entry.nextSectors.end(), [&](const std::string& s) {
                return onlineControllers.count(s) > 0;
                });
            if (!anyOnline) return false;
        }

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

    for (const auto& entry : departureLoas) {
        if (matches(entry)) {
            if (clearedAltitude < entry.xfl * 100 && finalAltitude > entry.xfl * 100) {
                std::string xflStr = std::to_string(entry.xfl);
                strncpy_s(sItemString, 16, xflStr.c_str(), _TRUNCATE);
                return;
            }
            else if (clearedAltitude > entry.xfl * 100) {
                int finalFL = finalAltitude / 100;
                std::string finalStr = std::to_string(finalFL);
                strncpy_s(sItemString, 16, finalStr.c_str(), _TRUNCATE);
                return;
            }

            int finalFL = finalAltitude / 100;
            std::string fallbackStr = std::to_string(finalFL);
            strncpy_s(sItemString, 16, fallbackStr.c_str(), _TRUNCATE);
            return;
        }
    }

    for (const auto& entry : destinationLoas) {
        if (matches(entry)) {
            if (clearedAltitude < entry.xfl * 100) {
                strncpy_s(sItemString, 16, "XFL", _TRUNCATE);
                return;
            }
            else {
                std::string xflStr = std::to_string(entry.xfl);
                strncpy_s(sItemString, 16, xflStr.c_str(), _TRUNCATE);
                return;
            }
        }
    }

    int finalFL = finalAltitude / 100;
    std::string fallbackStr = std::to_string(finalFL);
    strncpy_s(sItemString, 16, fallbackStr.c_str(), _TRUNCATE);
}