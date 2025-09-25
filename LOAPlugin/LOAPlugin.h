#pragma once

#include "EuroScopePlugIn.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

using namespace EuroScopePlugIn;

// =============================
// LOAEntry Struct
// =============================
struct LOAEntry {
    std::vector<std::string> sectors;
    std::vector<std::string> waypoints;
    std::vector<std::string> originAirports;
    std::vector<std::string> destinationAirports;
    std::vector<std::string> nextSectors;
    int xfl = 0;
    std::string copText = "COPX";
    bool requireNextSectorOnline = false;
};

// =============================
// Custom Tag Item IDs
// =============================
namespace ItemCodes {

const int CUSTOM_TAG_ID = 1996;
const int CUSTOM_TAG_ID_COP = 1997;
const int CUSTOM_TAG_ID_NEXT_SECTOR = 1998;
const int CUSTOM_TAG_MENU_SECTOR = 1999;
const int CUSTOM_TAG_XFL_DETAILED = 2000;
const int CUSTOM_TAG_DO_HANDOFF = 2001;

}

// =============================
// Global LOA Containers
// =============================
extern std::vector<LOAEntry> destinationLoas;
extern std::vector<LOAEntry> departureLoas;
extern std::vector<LOAEntry> lorArrivals;
extern std::vector<LOAEntry> lorDepartures;

extern std::unordered_map<std::string, std::string> controllerFrequencies;
extern std::unordered_map<int, std::pair<std::string, EuroScopePlugIn::CFlightPlan>> handoffTargets;

// =============================
// Match Function
// =============================
std::unordered_set<std::string> GetOnlineControllers();
std::string controllerId = controller.GetCallsign();
const LOAEntry* MatchLoaEntry(const EuroScopePlugIn::CFlightPlan& fp, const std::unordered_set<std::string>& onlineControllers);


// =============================
// Tag Render Functions
// =============================
void RenderXFLTagItem(
    EuroScopePlugIn::CFlightPlan flightPlan,
    EuroScopePlugIn::CRadarTarget radarTarget,
    int tagData,
    char sItemString[16],
    int* pColorCode,
    COLORREF* pRGB,
    double* pFontSize);

void RenderXFLDetailedTagItem(
    EuroScopePlugIn::CFlightPlan flightPlan,
    EuroScopePlugIn::CRadarTarget radarTarget,
    int tagData,
    char sItemString[16],
    int* pColorCode,
    COLORREF* pRGB,
    double* pFontSize);

void RenderCOPTagItem(
    EuroScopePlugIn::CFlightPlan flightPlan,
    EuroScopePlugIn::CRadarTarget radarTarget,
    int tagData,
    char sItemString[16],
    int* pColorCode,
    COLORREF* pRGB,
    double* pFontSize);

void RenderNextSectorTagItem(
    EuroScopePlugIn::CFlightPlan flightPlan,
    EuroScopePlugIn::CRadarTarget radarTarget,
    int tagData,
    char sItemString[16],
    int* pColorCode,
    COLORREF* pRGB,
    double* pFontSize);

// =============================
// LOAPlugin Class
// =============================
class LOAPlugin : public EuroScopePlugIn::CPlugIn {
public:
    LOAPlugin();
    virtual ~LOAPlugin();

    void LoadLOAsFromJSON();

    void OnFunctionCall(int FunctionId,
        const char* sItemString,
        POINT Pt,
        RECT Area);

    virtual void OnGetTagItem(
        EuroScopePlugIn::CFlightPlan flightPlan,
        EuroScopePlugIn::CRadarTarget radarTarget,
        int itemCode,
        int tagData,
        char sItemString[16],
        int* pColorCode,
        COLORREF* pRGB,
        double* pFontSize);

    bool IsControllerOnlineCached(const std::string& controllerId, const std::unordered_set<std::string>& onlineControllers);
    bool MatchesAirport(const std::vector<std::string>& list, const std::string& airport);
    bool RouteContainsAllWaypoints(const EuroScopePlugIn::CFlightPlanExtractedRoute& route, const std::vector<std::string>& waypoints);

};

// =============================
// Plugin Instance
// =============================
extern LOAPlugin plugin;