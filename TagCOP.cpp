// =========================
// File: TagCOP.cpp (patched)
// =========================

#include "stdafx.h"
#include "LOAPlugin.h"
#include <string>
#include <cstring>
#include <windows.h>
#include <algorithm>
#include <climits>  // for INT_MIN
#include <unordered_map>
#include <unordered_set>

void RenderCOPTagItem(
    EuroScopePlugIn::CFlightPlan flightPlan,
    EuroScopePlugIn::CRadarTarget radarTarget,
    int tagData,
    char sItemString[16],
    int* pColorCode,
    COLORREF* pRGB,
    double* pFontSize)
{
    if (!flightPlan.IsValid() || !plugin.IsLOARelevantState(flightPlan.GetState())) {
        strncpy_s(sItemString, 16, "COPX", _TRUNCATE);
        return;
    }

    if (flightPlan.GetState() == FLIGHT_PLAN_STATE_NON_CONCERNED) {
        plugin.coordinationStates.erase(flightPlan.GetCallsign());
        strncpy_s(sItemString, 16, "COPX", _TRUNCATE);
        return;
    }

    const auto& fpd = flightPlan.GetFlightPlanData();
    const char* planType = fpd.GetPlanType();
    if (_stricmp(planType, "I") != 0) {
        strncpy_s(sItemString, 16, "COPX", _TRUNCATE);
        return;
    }

    const std::string callsign = flightPlan.GetCallsign();

    // ---------- Coordination FIRST (mirror XFL behavior) ----------
    std::string coordCOP = flightPlan.GetExitCoordinationPointName();
    int coordState = flightPlan.GetExitCoordinationNameState();

    // Cache ongoing requests so we can still show them briefly when state flips through NONE
    if ((coordState == COORDINATION_STATE_REQUESTED_BY_ME ||
        coordState == COORDINATION_STATE_REQUESTED_BY_OTHER) &&
        !coordCOP.empty())
    {
        plugin.coordinationStates[callsign].exitPoint = coordCOP;
        plugin.coordinationStates[callsign].exitPointState = coordState;
    }

    // If the request is accepted and we have a COP name, render it with the accepted color.
    if (!coordCOP.empty() && coordState == COORDINATION_STATE_ACCEPTED) {
        strncpy_s(sItemString, 16, coordCOP.c_str(), _TRUNCATE);
        if (pColorCode) *pColorCode = TAG_COLOR_ONGOING_REQUEST_ACCEPTED;
        return;
    }

    if (!coordCOP.empty() && coordState == COORDINATION_STATE_REQUESTED_BY_ME) {
        strncpy_s(sItemString, 16, coordCOP.c_str(), _TRUNCATE);
        if (pColorCode) *pColorCode = TAG_COLOR_ONGOING_REQUEST_FROM_ME;
        return;
    }
    if (!coordCOP.empty() && coordState == COORDINATION_STATE_REQUESTED_BY_OTHER) {
        strncpy_s(sItemString, 16, coordCOP.c_str(), _TRUNCATE);
        if (pColorCode) *pColorCode = TAG_COLOR_ONGOING_REQUEST_TO_ME;
        return;
    }
    if (!coordCOP.empty() && coordState == COORDINATION_STATE_REFUSED) {
        strncpy_s(sItemString, 16, "COPX", _TRUNCATE);
        if (pColorCode) *pColorCode = TAG_COLOR_ONGOING_REQUEST_REFUSED;
        return;
    }

    // When API momentarily reports NONE but we have a recent cached in-progress/accepted COP, show it.
    if (coordState == COORDINATION_STATE_NONE) {
        auto it = plugin.coordinationStates.find(callsign);
        if (it != plugin.coordinationStates.end()) {
            const auto& info = it->second;
            if (!info.exitPoint.empty()) {
                strncpy_s(sItemString, 16, info.exitPoint.c_str(), _TRUNCATE);
                // Default to accepted color for the cached render (adjust if you keep finer states)
                if (pColorCode) *pColorCode = TAG_COLOR_ONGOING_REQUEST_ACCEPTED;
                return;
            }
        }
    }
    // -------------------------------------------------------------

    // ---------- Fallback to matched LOA COP AFTER coordination ----------
    if (const LOAEntry* matched = plugin.currentFrameMatchedEntry) {
        if (!matched->copText.empty() &&
            _stricmp(matched->copText.c_str(), "COPX") != 0)
        {
            strncpy_s(sItemString, 16, matched->copText.c_str(), _TRUNCATE);
            return;
        }
    }
    // --------------------------------------------------------------------

    // Default
    strncpy_s(sItemString, 16, "COPX", _TRUNCATE);
}