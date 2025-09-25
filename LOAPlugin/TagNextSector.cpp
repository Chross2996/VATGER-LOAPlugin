// ==============================
// File: TagNextSector.cpp
// ==============================

#include "stdafx.h"
#include "LOAPlugin.h"
#include <unordered_set>
#include <string>
#include <windows.h>
#include <algorithm>

// Access the plugin instance
extern LOAPlugin plugin;

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

    int clearedAltitude = flightPlan.GetClearedAltitude();   // ft
    int finalAltitude = flightPlan.GetFinalAltitude();       // ft
    std::string origin = flightPlan.GetFlightPlanData().GetOrigin();
    std::string destination = flightPlan.GetFlightPlanData().GetDestination();
    std::string controller = flightPlan.GetTrackingControllerId();

    // Build list of online controllers
    std::unordered_set<std::string> onlineControllers;
    for (EuroScopePlugIn::CController c = plugin.ControllerSelectFirst(); c.IsValid(); c = plugin.ControllerSelectNext(c)) {
        onlineControllers.insert(c.GetPositionId());
    }

    auto route = flightPlan.GetExtractedRoute();
    std::vector<std::string> routePoints;
    for (int i = 0; i < route.GetPointsNumber(); ++i)
        routePoints.emplace_back(route.GetPointName(i));

    auto matches = [&](const LOAEntry& entry) -> bool {
        bool sectorMatch = entry.sectors.empty() || std::any_of(entry.sectors.begin(), entry.sectors.end(),
            [&](const std::string& s) { return _stricmp(s.c_str(), controller.c_str()) == 0; });

        bool originMatch = entry.originAirports.empty() || std::any_of(entry.originAirports.begin(), entry.originAirports.end(),
            [&](const std::string& o) { return _stricmp(o.c_str(), origin.c_str()) == 0; });

        bool destMatch = entry.destinationAirports.empty() || std::any_of(entry.destinationAirports.begin(), entry.destinationAirports.end(),
            [&](const std::string& d) { return _stricmp(d.c_str(), destination.c_str()) == 0; });

        bool wpMatch = std::all_of(entry.waypoints.begin(), entry.waypoints.end(), [&](const std::string& wp) {
            return std::any_of(routePoints.begin(), routePoints.end(), [&](const std::string& r) {
                return _stricmp(r.c_str(), wp.c_str()) == 0;
                });
            });

        return sectorMatch && originMatch && destMatch && wpMatch;
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

    // 🔁 Fallback to EuroScope's predicted next sector (skip current)
    auto predictions = flightPlan.GetPositionPredictions();
    for (int i = 0; i < predictions.GetPointsNumber(); ++i) {
        const char* predictedId = predictions.GetControllerId(i);
        if (predictedId && strlen(predictedId) > 0 && _stricmp(predictedId, controller.c_str()) != 0) {
            strncpy_s(sItemString, 16, predictedId, _TRUNCATE);
            return;
        }
    }

    // ❌ Still nothing useful
    strncpy_s(sItemString, 16, "-", _TRUNCATE);
}

void LOAPlugin::OnFunctionCall(int FunctionId, const char* sItemString, POINT Pt, RECT Area)
{
    EuroScopePlugIn::CFlightPlan fp = FlightPlanSelectASEL();
    if (!fp.IsValid() || fp.GetSimulated() || !fp.GetTrackingControllerIsMe())
        return;

    if (FunctionId == ItemCodes::CUSTOM_TAG_MENU_SECTOR) {
        std::string nextSector = "";
        std::string frequency = "";

        std::unordered_set<std::string> onlineControllers;
        for (EuroScopePlugIn::CController c = plugin.ControllerSelectFirst(); c.IsValid(); c = plugin.ControllerSelectNext(c)) {
            onlineControllers.insert(c.GetPositionId());
        }

        const LOAEntry* entry = MatchLoaEntry(fp, onlineControllers);
        int clearedAltitude = fp.GetClearedAltitude();
        int finalAltitude = fp.GetFinalAltitude();

        std::unordered_map<std::string, std::string> online;
        for (EuroScopePlugIn::CController c = plugin.ControllerSelectFirst(); c.IsValid(); c = plugin.ControllerSelectNext(c)) {
            char freqStr[16];
            snprintf(freqStr, sizeof(freqStr), "%.3f", c.GetPrimaryFrequency());
            online[c.GetPositionId()] = freqStr;
        }

        if (entry && !entry->nextSectors.empty()) {
            const std::string& candidate = entry->nextSectors.front();
            if (online.find(candidate) != online.end()) {
                nextSector = candidate;
                frequency = online[candidate];
            }
        }

        if (nextSector.empty()) {
            auto predictions = fp.GetPositionPredictions();
            for (int i = 0; i < predictions.GetPointsNumber(); ++i) {
                const char* pred = predictions.GetControllerId(i);
                if (pred && strlen(pred) > 0 && online.find(pred) != online.end()) {
                    nextSector = pred;
                    frequency = online[pred];
                    break;
                }
            }
        }

        if (!nextSector.empty()) {
            OpenPopupList(Area, "Next Sector", 1);
            std::string label = nextSector + " " + frequency;
            AddPopupListElement(label.c_str(), nextSector.c_str(), ItemCodes::CUSTOM_TAG_DO_HANDOFF);
        }

        return;
    }

    if (FunctionId == ItemCodes::CUSTOM_TAG_DO_HANDOFF) {
        // ⛳ Handoff target controller is in sItemString
        if (strlen(sItemString) > 0) {
            fp.InitiateHandoff(sItemString);
        }
        return;
    }
}

