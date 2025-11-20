#pragma once

#include "EuroScopePlugIn.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <atomic>

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

    // --- Exclusions ---
    std::vector<std::string> excludeDestinationAirports;
    std::unordered_set<std::string> excludeDestinationAirportSet;
    std::vector<std::string> excludeDestinationAirportPrefixes;

    std::vector<std::string> excludeOriginAirports;
    std::unordered_set<std::string> excludeOriginAirportSet;
    std::vector<std::string> excludeOriginAirportPrefixes;

};

// Helper result for LOA matching
struct LoaMatchResult {
    const LOAEntry* entry;   // nullptr if no match
    bool isDeparture;        // true if it matched a departure LOA (origin-based)
    LoaMatchResult() : entry(NULL), isDeparture(false) {}
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
    const int TAG_ITEM_NEXT_SECTOR_CTRL = 2001;
}

// =============================
// Global LOA Containers
// =============================
extern std::vector<LOAEntry> destinationLoas;
extern std::vector<LOAEntry> departureLoas;
extern std::vector<LOAEntry> destinationFallbackLoas;
extern std::vector<LOAEntry> departureFallbackLoas;
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

    // Returns true if a LOA match exists for this flightplan and fills 'out'
    bool TryGetLoaMatch(
        const EuroScopePlugIn::CFlightPlan& fp,
        const std::unordered_set<std::string>& onlineControllers,
        const std::vector<std::string>& routePoints,
        LoaMatchResult& out);
    LOAPlugin();
    virtual ~LOAPlugin();

    virtual void OnControllerPositionUpdate(EuroScopePlugIn::CController Controller);
    virtual void RequestRefreshRadarScreen() {}

    bool IsLOARelevantState(int state);
    bool IsControllerOnlineCached(const std::string& controllerId, const std::unordered_set<std::string>& onlineControllers);
    bool MatchesAirport(const std::unordered_set<std::string>& exactSet,
        const std::vector<std::string>& prefixes,
        const std::string& airport);

    // Returns controlling station ID for `nextSector` if someone online owns it via ownership/priority.
    std::string GetIndicatedNextSectorStation(const std::string& nextSector);

    const std::unordered_set<std::string>& GetOnlineControllersCached();  // ✅ 5-second cache accessor

    // In class LOAPlugin (LOAPlugin.h)
    std::unordered_map<std::string, std::vector<const LOAEntry*>> indexByWaypoint;
    std::unordered_map<std::string, std::vector<const LOAEntry*>> indexByNextSector;

    std::atomic<bool> reloading{ false };

    // In class LOAPlugin:
    ULONGLONG startTick = 0;
    ULONGLONG coldStartUntil = 0;     // e.g., start + 5000 ms
    bool      coldStartActive = true;


    // LOA CACHE
    // --- per-callsign render microcache (500–1000 ms) ---
    struct RenderItemCache {
        ULONGLONG ts = 0;
        char text[16] = { 0 };
        int colorCode = 0;
        COLORREF rgb = 0;
        double fontSize = 0.0;
        // signature of inputs that affect rendering (cheap and small)
        int clearedAlt = 0, finalAlt = 0, coordAlt = 0, coordAltState = 0;
        std::string coordPoint;
        int coordPointState = 0;
        int sectorVersion = 0;
    };

    std::unordered_map<std::string, RenderItemCache> renderCache; // key = callsign + ":" + std::to_string(itemCode)

    CachedTagData lastTagData;
    const std::vector<std::string>& GetCachedRoutePoints(const EuroScopePlugIn::CFlightPlan& fp);
    const std::unordered_set<std::string>& GetCachedRouteSet(const EuroScopePlugIn::CFlightPlan& fp);
    std::unordered_map<std::string, const LOAEntry*> matchedLOACache;
    std::unordered_map<std::string, std::vector<std::string>> routeCache;
    std::unordered_map<std::string, ULONGLONG> routeSetCacheTime;
    std::unordered_map<std::string, ULONGLONG> matchTimestamps;
    std::unordered_map<std::string, int> matchVersions;
    std::unordered_map<std::string, std::unordered_set<std::string>> routeSetCache;
    std::unordered_map<std::string, ULONGLONG> routeCacheTime;
    std::unordered_set<std::string> currentFrameRouteSet;
    int sectorControlVersion = 0;

    std::unordered_set<std::string> currentFrameOnlineControllers;
    std::vector<std::string> currentFrameRoutePoints;
    std::string currentFrameCallsign;
    ULONGLONG currentFrameTimestamp = 0;
    const LOAEntry* currentFrameMatchedEntry = nullptr;


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

    // Sectors that contributed AOR destinations (e.g., HAM, HAMW)
    std::unordered_set<std::string> aorHostSectors;

    // Returns true if any AOR host sector is controlled by someone online (not me)
    bool IsAnyAORHostOnline(const std::unordered_set<std::string>& onlineControllers) const;

    // AOR Aerodromes (destinations inside my sector)
   // Supports exact ICAOs and 2–3 letter prefixes (reuse MatchesAirport)
    std::unordered_set<std::string> aorDestinationSet;
    std::vector<std::string>        aorDestinationPrefixes;

    // Convenience
    inline bool IsAORDestination(const std::string& icao) const {
        // MatchesAirport is already declared in LOAPlugin.h
        return const_cast<LOAPlugin*>(this)->MatchesAirport(
            aorDestinationSet, aorDestinationPrefixes, icao);
    }

private:
    std::string loadedSector;
    void LoadLOAsFromJSON();

    void OnControllerDisconnect(const EuroScopePlugIn::CController& controller);

    void OnGetControllerList();

    std::unordered_set<std::string> cachedOnlineControllers;  // ✅ Cached online controllers

    std::unordered_map<std::string, size_t> routeSignature;  // hash of origin|dest|route to detect FP edits

    ULONGLONG lastOwnershipRecheckTime = 0;
    ULONGLONG lastOnlineFetchTime = 0;
};

// =============================
// Plugin Instance
// =============================
extern LOAPlugin plugin;