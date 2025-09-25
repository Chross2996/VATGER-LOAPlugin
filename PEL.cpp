#include "stdafx.h"
#include "LOAPlugin.h"
#include <string>
#include <algorithm>

void RenderPELTagItem(
    EuroScopePlugIn::CFlightPlan flightPlan,
    EuroScopePlugIn::CRadarTarget radarTarget,
    int tagData,
    char sItemString[16],
    int* pColorCode,
    COLORREF* pRGB,
    double* pFontSize)
{
    if (!flightPlan.IsValid() || !plugin.IsLOARelevantState(flightPlan.GetState())) {
        strncpy_s(sItemString, 16, "PEL", _TRUNCATE);
        return;
    }

    const std::string callsign = flightPlan.GetCallsign();
    int coordXFL = flightPlan.GetExitCoordinationAltitude();
    int coordState = flightPlan.GetExitCoordinationAltitudeState();

    // Store coordination info if a request was made
    if ((coordState == COORDINATION_STATE_REQUESTED_BY_ME || coordState == COORDINATION_STATE_REQUESTED_BY_OTHER) && coordXFL >= 500) {
        plugin.coordinationStates[callsign].exitAltitude = coordXFL;
        plugin.coordinationStates[callsign].exitAltitudeState = coordState;
    }

    // Show accepted coordination from cache (manual accepted)
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

    // Live coordination states
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

    // LOA match logic
    const auto& onlineControllers = plugin.currentFrameOnlineControllers;
    const LOAEntry* match = MatchLoaEntry(flightPlan, onlineControllers);

    if (match) {
        std::string mySector = plugin.ControllerMyself().GetPositionId();
        const auto& owned = plugin.sectorOwnership[mySector];

        for (const std::string& next : match->nextSectors) {
            bool staticallyOwned = std::find(owned.begin(), owned.end(), next) != owned.end();
            std::string actualController = plugin.ResolveControllingSector(next, plugin.currentFrameOnlineControllers);
            bool iControlNext = _stricmp(actualController.c_str(), mySector.c_str()) == 0;

            if (staticallyOwned || iControlNext) {
                snprintf(sItemString, 16, "%03d", match->xfl);
                return;
            }
        }
    }

    // No coordination and no match to owned sector
    strncpy_s(sItemString, 16, "PEL", _TRUNCATE);
}
