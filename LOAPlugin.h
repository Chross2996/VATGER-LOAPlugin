#pragma once

#include "EuroScopePlugIn.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <map>

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
    int minAltitudeFt = 0;  // For fallbackLoas: minimum altitude (e.g. 24500 for FL245)

    // ✅ NEW: Optimized airport matching
    std::unordered_set<std::string> originAirportSet;
    std::vector<std::string> originAirportPrefixes;
    std::unordered_set<std::string> destinationAirportSet;
    std::vector<std::string> destinationAirportPrefixes;
};

struct CachedTagData {
    std::string callsign;
    int clearedAltitude = 0;
    int finalAltitude = 0;
    std::string origin;
    std::string destination;
};

// ✅ NEW: Coordination info for XFL/COP coordination caching
struct CoordinationInfo {
    int exitAltitude = 0;
    int exitAltitudeState = 0;
    std::string exitPoint;
    int exitPointState = 0;
};

// =============================
// Custom Tag Item IDs
// =============================
namespace ItemCodes {
    const int CUSTOM_TAG_ID = 1996;
    const int CUSTOM_TAG_ID_COP = 1997;
    const int CUSTOM_TAG_XFL_DETAILED = 2000;
}

// =============================
// Global LOA Containers
// =============================
extern std::vector<LOAEntry> destinationLoas;
extern std::vector<LOAEntry> departureLoas;
extern std::vector<LOAEntry> lorArrivals;
extern std::vector<LOAEntry> lorDepartures;
extern std::vector<LOAEntry> fallbackLoas;
extern std::map<std::string, std::vector<LOAEntry>> loadedSectorLoas;

extern std::unordered_map<std::string, std::string> controllerFrequencies;
extern std::unordered_map<int, std::pair<std::string, EuroScopePlugIn::CFlightPlan>> handoffTargets;

// =============================
// Match Function
// =============================
bool EqualsIgnoreCase(const std::string& a, const std::string& b);
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

void RenderPELTagItem(
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

    virtual void OnControllerPositionUpdate(EuroScopePlugIn::CController Controller);
    virtual void RequestRefreshRadarScreen() {}

    bool IsLOARelevantState(int state);
    bool IsControllerOnlineCached(const std::string& controllerId, const std::unordered_set<std::string>& onlineControllers);
    bool MatchesAirport(const std::unordered_set<std::string>& exactSet,
        const std::vector<std::string>& prefixes,
        const std::string& airport);

    const std::unordered_set<std::string>& GetOnlineControllersCached();  // ✅ 5-second cache accessor

    // In class LOAPlugin (LOAPlugin.h)
    std::unordered_map<std::string, std::vector<const LOAEntry*>> indexByWaypoint;
    std::unordered_map<std::string, std::vector<const LOAEntry*>> indexByNextSector;

    // LOA CACHE
    CachedTagData lastTagData;
    const std::vector<std::string>& GetCachedRoutePoints(const EuroScopePlugIn::CFlightPlan& fp);
    std::unordered_map<std::string, const LOAEntry*> matchedLOACache;
    std::unordered_map<std::string, std::vector<std::string>> routeCache;
    std::unordered_map<std::string, DWORD> routeCacheTime;
    std::unordered_map<std::string, DWORD> matchTimestamps;
    std::unordered_map<std::string, int> matchVersions;
    int sectorControlVersion = 0;

    std::unordered_set<std::string> currentFrameOnlineControllers;
    std::vector<std::string> currentFrameRoutePoints;
    std::string currentFrameCallsign;
    DWORD currentFrameTimestamp = 0;

    void CleanupCache(const std::string& callsign);
    virtual void OnFlightPlanStateChange(EuroScopePlugIn::CFlightPlan fp);
    virtual void OnFlightPlanCoordinationStateChange(EuroScopePlugIn::CFlightPlan fp, int coordinationType, int newState);

    void CheckForOwnershipChange();

    virtual void OnGetTagItem(
        EuroScopePlugIn::CFlightPlan flightPlan,
        EuroScopePlugIn::CRadarTarget radarTarget,
        int itemCode,
        int tagData,
        char sItemString[16],
        int* pColorCode,
        COLORREF* pRGB,
        double* pFontSize);

    // ✅ Coordination caching (accessible from render functions)
    std::unordered_map<std::string, CoordinationInfo> coordinationStates;

    // ✅ Sector Ownership Logic
    void LoadSectorOwnership();
    std::string ResolveControllingSector(const std::string& sector, const std::unordered_set<std::string>& onlineControllers);
    std::unordered_map<std::string, std::vector<std::string>> sectorOwnership; // e.g., "ALR": ["HEI", "EID"]
    std::unordered_map<std::string, std::vector<std::string>> sectorPriority;  // e.g., "FRI": ["EID", "ALR"]

private:
    std::string loadedSector;
    void LoadLOAsFromJSON();

    void OnControllerDisconnect(const EuroScopePlugIn::CController& controller);

    void OnGetControllerList();

    std::unordered_set<std::string> cachedOnlineControllers;  // ✅ Cached online controllers
    DWORD lastOwnershipRecheckTime = 0;
    DWORD lastOnlineFetchTime = 0;
};

// =============================
// Plugin Instance
// =============================
extern LOAPlugin plugin;