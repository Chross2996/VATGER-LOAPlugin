#include "stdafx.h"
#include "LOAPlugin.h"
#include <string>
#include <algorithm>
#include <climits>
#include <unordered_map>
#include <unordered_set>

static bool IsAcceptedAltitudeStateXFL(int state)
{
    return state == COORDINATION_STATE_ACCEPTED ||
        state == COORDINATION_STATE_MANUAL_ACCEPTED;
}

static void FormatFL3(char sItemString[16], int altitudeFeet)
{
    int fl = altitudeFeet;
    if (fl > 1000) fl /= 100; // feet -> FL text
    _snprintf_s(sItemString, 16, _TRUNCATE, "%03d", fl);
}

namespace {
    struct XflCoordHeuristicState {
        int baselineValue = 0;
        int pendingValue = 0;
        bool hasBaseline = false;
        bool pendingActive = false;
    };

    static std::unordered_map<std::string, XflCoordHeuristicState> g_xflHeuristic;
}

static bool TryGetLiveCoordAltitude(
    EuroScopePlugIn::CFlightPlan flightPlan,
    int& outAlt,
    int& outState)
{
    const std::string callsign = flightPlan.GetCallsign();
    XflCoordHeuristicState& st = g_xflHeuristic[callsign];

    outAlt = flightPlan.GetExitCoordinationAltitude();
    outState = flightPlan.GetExitCoordinationAltitudeState();

    // Explicit refusal: abandon the pending request and fall back immediately.
    if (outState == COORDINATION_STATE_REFUSED) {
        st.pendingActive = false;
        st.pendingValue = 0;
        if (outAlt >= 500) {
            st.baselineValue = outAlt;
            st.hasBaseline = true;
        }
        return false;
    }

    // Pending request: remember and show the requested altitude.
    if (outState == COORDINATION_STATE_REQUESTED_BY_ME ||
        outState == COORDINATION_STATE_REQUESTED_BY_OTHER)
    {
        if (outAlt >= 500) {
            st.pendingValue = outAlt;
            st.pendingActive = true;
            return true;
        }
        return false;
    }

    // Explicit accepted/manual accepted: show it and keep the same pending value for the later NONE check.
    if (IsAcceptedAltitudeStateXFL(outState) && outAlt >= 500) {
        st.pendingValue = outAlt;
        st.pendingActive = true;
        return true;
    }

    // NONE before any active coordination: learn baseline but do not display it.
    if (outState == COORDINATION_STATE_NONE && !st.pendingActive) {
        if (outAlt >= 500) {
            st.baselineValue = outAlt;
            st.hasBaseline = true;
        }
        return false;
    }

    // NONE after a coordination cycle: compare the lingering value.
    if (outState == COORDINATION_STATE_NONE && st.pendingActive) {
        if (outAlt >= 500 && st.pendingValue >= 500 && outAlt == st.pendingValue) {
            // Request value survived into NONE -> treat as accepted.
            outAlt = st.pendingValue;
            outState = COORDINATION_STATE_ACCEPTED;
            return true;
        }

        const bool revertedToBaseline =
            (outAlt >= 500 && st.hasBaseline && outAlt == st.baselineValue);

        // Empty, baseline, or any unexpected value -> stop showing coordination and fall back.
        if (outAlt < 500 || revertedToBaseline || outAlt != st.pendingValue) {
            st.pendingActive = false;
            st.pendingValue = 0;
            if (outAlt >= 500) {
                st.baselineValue = outAlt;
                st.hasBaseline = true;
            }
            return false;
        }
    }

    return false;
}

static void ApplyPendingColorOnly(
    EuroScopePlugIn::CRadarTarget radarTarget,
    int state,
    int* pColorCode)
{
    const bool isListContext = !radarTarget.IsValid();
    if (isListContext || !pColorCode) return;

    if (state == COORDINATION_STATE_REQUESTED_BY_ME) {
        *pColorCode = TAG_COLOR_ONGOING_REQUEST_FROM_ME;
    }
    else if (state == COORDINATION_STATE_REQUESTED_BY_OTHER) {
        *pColorCode = TAG_COLOR_ONGOING_REQUEST_TO_ME;
    }
    else {
        *pColorCode = TAG_COLOR_DEFAULT;
    }
}

// Tagged/Untagged XFL Tag Item
void RenderXFLTagItem(
    EuroScopePlugIn::CFlightPlan flightPlan,
    EuroScopePlugIn::CRadarTarget radarTarget,
    int tagData,
    char sItemString[16],
    int* pColorCode,
    COLORREF* pRGB,
    double* pFontSize,
    const PerAircraftFrameData& ctx)
{
    if (!flightPlan.IsValid()) {
        sItemString[0] = '\0';
        return;
    }

    switch (flightPlan.GetState()) {
    case FLIGHT_PLAN_STATE_ASSUMED:
    case FLIGHT_PLAN_STATE_NOTIFIED:
    case FLIGHT_PLAN_STATE_COORDINATED:
    case FLIGHT_PLAN_STATE_TRANSFER_TO_ME_INITIATED:
        break;
    default:
        sItemString[0] = '\0';
        return;
    }

    const auto& fpd = flightPlan.GetFlightPlanData();
    if (_stricmp(fpd.GetPlanType(), "I") != 0) {
        sItemString[0] = '\0';
        return;
    }

    const LOAEntry* matched = plugin.currentFrameMatchedEntry;
    if (matched && !plugin.IsLoaEntryPointerValid(matched)) matched = nullptr;
    int clearedAltitude = ctx.clearedAltitude;
    int finalAltitude = ctx.finalAltitude;

    if (plugin.IsAORDestination(ctx.destination) &&
        plugin.IsAnyAORHostOnline(plugin.currentFrameOnlineControllers)) {
        sItemString[0] = '\0';
        return;
    }

    // 1) Active live coordination altitude shows directly.
    int coordAlt = 0, coordState = COORDINATION_STATE_NONE;
    if (TryGetLiveCoordAltitude(flightPlan, coordAlt, coordState)) {
        ApplyPendingColorOnly(radarTarget, coordState, pColorCode);

        // Only hide when coord altitude equals cleared altitude
        if (coordAlt == clearedAltitude) {
            sItemString[0] = '\0';
            return;
        }

        FormatFL3(sItemString, coordAlt);
        return;
    }

    // 2) LOA match path.
    if (matched) {
        if (!matched->xflText.empty()) {
            strncpy_s(sItemString, 16, matched->xflText.c_str(), _TRUNCATE);
            return;
        }

        if (matched->xfl == 0) {
            sItemString[0] = '\0';
            return;
        }

        const int loaXflFeet = matched->xfl * 100;

        // If a LOA matched, only hide when the cleared altitude equals the matched LOA value.
        if (clearedAltitude == loaXflFeet) {
            sItemString[0] = '\0';
            return;
        }

        _snprintf_s(sItemString, 16, _TRUNCATE, "%03d", matched->xfl);
        return;
    }

    // 3) No LOA match -> final altitude fallback.
    // Only in the no-match case do we hide when cleared altitude equals final altitude.
    if (clearedAltitude == finalAltitude) {
        sItemString[0] = '\0';
        return;
    }

    FormatFL3(sItemString, finalAltitude);
}

// Detailed XFL tag — always show something.
void RenderXFLDetailedTagItem(
    EuroScopePlugIn::CFlightPlan flightPlan,
    EuroScopePlugIn::CRadarTarget radarTarget,
    int tagData,
    char sItemString[16],
    int* pColorCode,
    COLORREF* pRGB,
    double* pFontSize,
    const PerAircraftFrameData& ctx)
{
    if (!flightPlan.IsValid() || !plugin.IsLOARelevantState(flightPlan.GetState())) {
        strncpy_s(sItemString, 16, "XFL", _TRUNCATE);
        return;
    }

    if (flightPlan.GetState() == FLIGHT_PLAN_STATE_NON_CONCERNED) {
        strncpy_s(sItemString, 16, "XFL", _TRUNCATE);
        return;
    }

    const auto& fpd = flightPlan.GetFlightPlanData();
    if (_stricmp(fpd.GetPlanType(), "I") != 0) {
        strncpy_s(sItemString, 16, "XFL", _TRUNCATE);
        return;
    }

    int finalAltitude = ctx.finalAltitude;

    if (plugin.IsAORDestination(ctx.destination) &&
        plugin.IsAnyAORHostOnline(plugin.currentFrameOnlineControllers)) {
        strncpy_s(sItemString, 16, "XFL", _TRUNCATE);
        return;
    }

    int coordAlt = 0, coordState = COORDINATION_STATE_NONE;
    if (TryGetLiveCoordAltitude(flightPlan, coordAlt, coordState)) {
        ApplyPendingColorOnly(radarTarget, coordState, pColorCode);
        FormatFL3(sItemString, coordAlt);
        return;
    }

    const LOAEntry* finalMatch = plugin.currentFrameMatchedEntry;
    if (finalMatch && !plugin.IsLoaEntryPointerValid(finalMatch)) finalMatch = nullptr;

    if (finalMatch) {
        if (!finalMatch->xflText.empty()) {
            strncpy_s(sItemString, 16, finalMatch->xflText.c_str(), _TRUNCATE);
            return;
        }
        if (finalMatch->xfl == 0) {
            strncpy_s(sItemString, 16, "XFL", _TRUNCATE);
            return;
        }
        _snprintf_s(sItemString, 16, _TRUNCATE, "%03d", finalMatch->xfl);
        return;
    }

    FormatFL3(sItemString, finalAltitude);
}