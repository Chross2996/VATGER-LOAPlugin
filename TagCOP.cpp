#include "stdafx.h"
#include "LOAPlugin.h"
#include <string>
#include <cstring>
#include <windows.h>
#include <unordered_map>

namespace {
    struct CopHeuristicState {
        std::string baselineValue;
        std::string pendingValue;
        bool hasBaseline = false;
        bool pendingActive = false;
    };

    static std::unordered_map<std::string, CopHeuristicState> g_copHeuristic;

    static bool EqualsNoCase(const std::string& a, const std::string& b)
    {
        return _stricmp(a.c_str(), b.c_str()) == 0;
    }

    static bool IsAcceptedState(int state)
    {
        return state == COORDINATION_STATE_ACCEPTED ||
            state == COORDINATION_STATE_MANUAL_ACCEPTED;
    }
}

void RenderCOPTagItem(
    EuroScopePlugIn::CFlightPlan flightPlan,
    EuroScopePlugIn::CRadarTarget radarTarget,
    int tagData,
    char sItemString[16],
    int* pColorCode,
    COLORREF* pRGB,
    double* pFontSize,
    const PerAircraftFrameData& ctx)
{
    const bool isListContext = !radarTarget.IsValid();

    if (!flightPlan.IsValid() || !plugin.IsLOARelevantState(flightPlan.GetState())) {
        strncpy_s(sItemString, 16, "COPX", _TRUNCATE);
        return;
    }

    const std::string callsign = flightPlan.GetCallsign();

    if (flightPlan.GetState() == FLIGHT_PLAN_STATE_NON_CONCERNED) {
        g_copHeuristic.erase(callsign);
        strncpy_s(sItemString, 16, "COPX", _TRUNCATE);
        return;
    }

    const auto& fpd = flightPlan.GetFlightPlanData();
    if (_stricmp(fpd.GetPlanType(), "I") != 0) {
        g_copHeuristic.erase(callsign);
        strncpy_s(sItemString, 16, "COPX", _TRUNCATE);
        return;
    }

    const LOAEntry* matched = plugin.currentFrameMatchedEntry;
    if (matched && !plugin.IsLoaEntryPointerValid(matched)) matched = nullptr;

    auto showFallback = [&]() {
        if (matched && !matched->copText.empty() && _stricmp(matched->copText.c_str(), "COPX") != 0) {
            strncpy_s(sItemString, 16, matched->copText.c_str(), _TRUNCATE);
        }
        else {
            strncpy_s(sItemString, 16, "COPX", _TRUNCATE);
        }
        };

    const std::string coordCOP = flightPlan.GetExitCoordinationPointName();
    const int coordState = flightPlan.GetExitCoordinationNameState();
    CopHeuristicState& st = g_copHeuristic[callsign];

    // Explicit refusal: abandon the pending request and fall back immediately.
    if (coordState == COORDINATION_STATE_REFUSED) {
        st.pendingActive = false;
        st.pendingValue.clear();
        if (!coordCOP.empty()) {
            st.baselineValue = coordCOP;
            st.hasBaseline = true;
        }
        showFallback();
        return;
    }

    // Pending request: remember the requested value.
    if (coordState == COORDINATION_STATE_REQUESTED_BY_ME ||
        coordState == COORDINATION_STATE_REQUESTED_BY_OTHER)
    {
        if (!coordCOP.empty()) {
            if (!st.pendingActive && !st.hasBaseline) {
                // No trustworthy baseline learned yet; remember current non-pending NONEs only.
            }
            st.pendingValue = coordCOP;
            st.pendingActive = true;
        }

        if (!coordCOP.empty()) {
            strncpy_s(sItemString, 16, coordCOP.c_str(), _TRUNCATE);
            if (!isListContext && pColorCode) {
                *pColorCode = (coordState == COORDINATION_STATE_REQUESTED_BY_ME)
                    ? TAG_COLOR_ONGOING_REQUEST_FROM_ME
                    : TAG_COLOR_ONGOING_REQUEST_TO_ME;
            }
            return;
        }

        showFallback();
        return;
    }

    // Explicit accepted/manual accepted: show it and keep the same pending value for the later NONE check.
    if (IsAcceptedState(coordState) && !coordCOP.empty()) {
        st.pendingValue = coordCOP;
        st.pendingActive = true;
        strncpy_s(sItemString, 16, coordCOP.c_str(), _TRUNCATE);
        return;
    }

    // NONE before any active coordination: learn baseline but do not display it.
    if (coordState == COORDINATION_STATE_NONE && !st.pendingActive) {
        if (!coordCOP.empty()) {
            st.baselineValue = coordCOP;
            st.hasBaseline = true;
        }
        showFallback();
        return;
    }

    // NONE after a coordination cycle: compare the lingering value.
    if (coordState == COORDINATION_STATE_NONE && st.pendingActive) {
        if (!coordCOP.empty() && !st.pendingValue.empty() && EqualsNoCase(coordCOP, st.pendingValue)) {
            // Request value survived into NONE -> treat as accepted.
            strncpy_s(sItemString, 16, st.pendingValue.c_str(), _TRUNCATE);
            return;
        }

        const bool revertedToBaseline =
            (!coordCOP.empty() && st.hasBaseline && EqualsNoCase(coordCOP, st.baselineValue));

        // Empty, baseline, or any unexpected value -> stop showing coordination and fall back.
        if (coordCOP.empty() || revertedToBaseline || !EqualsNoCase(coordCOP, st.pendingValue)) {
            st.pendingActive = false;
            st.pendingValue.clear();
            if (!coordCOP.empty()) {
                st.baselineValue = coordCOP;
                st.hasBaseline = true;
            }
            showFallback();
            return;
        }
    }

    showFallback();
}