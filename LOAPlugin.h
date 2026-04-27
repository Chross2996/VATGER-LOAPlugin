#pragma once

#include "EuroScopePlugIn.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <atomic>
#include <array>
#include <cstdint>
#include <utility>

using namespace EuroScopePlugIn;

// =============================
// LOAEntry Struct
// =============================

// LOA list category: used for altitude gating rules
enum class LOAListKind : uint8_t {
	Unknown = 0,
	Destination = 1,
	Departure = 2,
	DestinationFallback = 3,
	DepartureFallback = 4
};

struct LOAEntry {
	std::vector<std::string> sectors;
	std::vector<std::string> waypoints;
	// If ANY of these waypoints are present in the route, this LOA must NOT match.
	// (Waypoints are normalized to lowercase at JSON load, same as "waypoints".)
	std::vector<std::string> notViaWaypoints;
	// Custom volume prediction constraints (volumes.json)
	// - predictedEnterVolumes: match if predicted trajectory enters ANY listed volume
	// - predictedFromVolumes/predictedToVolumes: match only if it enters a FROM volume and later a TO volume
	// - predictedEndVolumes: match if the last predicted point ends inside ANY listed volume
	std::vector<std::string> predictedEnterVolumes;
	std::vector<std::string> predictedFromVolumes;
	std::vector<std::string> predictedToVolumes;
	std::vector<std::string> predictedEndVolumes;
	std::vector<std::string> originAirports;
	std::vector<std::string> destinationAirports;
	std::vector<std::string> nextSectors;
	// Runway constraints (matches EuroScope Active Airports/Runways selection)
	// - For Departure lists: compared against active DEP runways at ORIGIN airport
	// - For Destination lists: compared against active ARR runways at DESTINATION airport
	// If empty: no runway constraint.
	std::vector<std::string> runways;
	int xfl = 0;               // numeric FL (legacy)
	std::string xflText;       // optional text value (e.g. "23R", "230-")
	std::string copText = "COPX";
	bool requireNextSectorOnline = false;
	int minAltitudeFt = 0;  // For fallbackLoas: minimum altitude (e.g. 24500 for FL245)
	LOAListKind listKind = LOAListKind::Unknown;

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

// =============================
// Custom Volume (user-defined sector volume)
// =============================
struct CustomVolume {
	std::string id;
	double lowerFt = 0.0;
	double upperFt = 999999.0;
	// polygon points as [lat, lon] in decimal degrees
	std::vector<std::pair<double, double>> polygon;
};

struct CachedTagData {
	std::string callsign;
	int clearedAltitude = 0;
	int finalAltitude = 0;
	std::string origin;
	std::string destination;
};


struct CoordinationInfo {
	std::string baselineExitPoint;
	std::string pendingExitPoint;
	std::string acceptedExitPoint;
	int baselineExitAltitude = 0;
	int pendingExitAltitude = 0;
	int acceptedExitAltitude = 0;
	bool pointRequestActive = false;
	bool altitudeRequestActive = false;
};


// =============================
// Sector polygons for route-based next sectors
// =============================
struct SectorPolygon {
	std::string sectorId;        // e.g. "ALR", "HAM"
	std::vector<CPosition> pts;  // polygon vertices from sector file
};

// =============================
// Custom Tag Item IDs
// =============================
namespace ItemCodes {
	const int CUSTOM_TAG_ID = 1996;
	const int CUSTOM_TAG_ID_COP = 1997;
	const int CUSTOM_TAG_XFL_DETAILED = 2000;
	const int TAG_ITEM_NEXT_SECTOR_CTRL = 2001;
	const int CUSTOM_TAG_PEL = 2002;
}

// =============================
// Custom Function IDs
// =============================
namespace FunctionIds {
	// Triggered when you click on the Next Sector Ctrl tag
	const int NEXT_SECTOR_HANDOFF_MENU = 3000;

	// Triggered when you click an entry in the popup list
	const int NEXT_SECTOR_HANDOFF_EXEC = 3001;

	// NEW: Triggered when you click the ASSUME item in the popup
	const int NEXT_SECTOR_ASSUME_EXEC = 3002;

	// NEW: Triggered when you click the RELEASE item in the popup
	const int NEXT_SECTOR_RELEASE_EXEC = 3003;
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

	// Active Airports/Runways integration (like vSID):
	// EuroScope toggles runway-end activity flags in sector elements when you use the
	// "Active Airports/Runways" dialog. We cache those flags per airport for fast matching.
	virtual void OnAirportRunwayActivityChanged() override;

	void UpdateActiveRunwaysFromSectorFile();
	bool MatchesActiveRunway(const std::string& airportIcao, bool isDeparture, const std::vector<std::string>& allowedRunways) const;

	// NEW: handle tag functions & popup selections
	virtual void OnFunctionCall(
		int FunctionId,
		const char* sItemString,
		POINT Pt,
		RECT Area) override;

	bool IsLOARelevantState(int state);
	bool IsControllerOnlineCached(const std::string& controllerId, const std::unordered_set<std::string>& onlineControllers);
	bool MatchesAirport(const std::unordered_set<std::string>& exactSet,
		const std::vector<std::string>& prefixes,
		const std::string& airport);

	// Centralized suppression/gating helpers (shared by matcher + tags)
	bool ShouldAllowNextSectors(
		const std::vector<std::string>& nextSectors,
		const std::unordered_set<std::string>& onlineControllers);

	bool IsSourceSectorSuppressed(
		const LOAEntry& e,
		const std::unordered_set<std::string>& onlineControllers);
	// Returns controlling station ID for `nextSector` if someone online owns it via ownership/priority.
	std::string GetIndicatedNextSectorStation(const std::string& nextSector);

	// Cached active runways per airport (from sector file runway elements)
	// Key: airport ICAO (e.g. "EDDH"), Values: runway ends (e.g. "05", "23", "09L")
	std::unordered_map<std::string, std::unordered_set<std::string>> activeDepRunwaysByAirport;
	std::unordered_map<std::string, std::unordered_set<std::string>> activeArrRunwaysByAirport;
	ULONGLONG lastActiveRunwayRefreshMs = 0;

	// Polling support: some EuroScope builds don't reliably call OnAirportRunwayActivityChanged.
	// We therefore poll runway activity flags periodically and invalidate caches when they change.
	ULONGLONG lastRunwayPollMs = 0;
	unsigned long long activeRunwayFingerprint = 0ULL;

	void PollActiveRunwaysIfNeeded();
	void InvalidateLoaCachesForRunwayChange();

	std::string GetPredictedNextController(const EuroScopePlugIn::CFlightPlan& fp);

	// LOAPlugin.h
	std::vector<std::string> BuildHybridPredictedSectorList(
		const EuroScopePlugIn::CFlightPlan& fp,
		const std::unordered_set<std::string>& onlineControllers,
		size_t* outLoaCount = nullptr);

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

	// Track which sector we initiated a handoff to (per callsign)
	std::unordered_map<std::string, std::string> activeHandoffTargets;

	// Coordination heuristic cache for COP/XFL tag rendering
	std::unordered_map<std::string, CoordinationInfo> coordinationStates;

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


	// ✅ Sector Ownership Logic
	void LoadSectorOwnership();
	std::string ResolveControllingSector(const std::string& sector, const std::unordered_set<std::string>& onlineControllers);
	std::unordered_map<std::string, std::vector<std::string>> sectorOwnership; // e.g., "ALR": ["HEI", "EID"]
	std::unordered_map<std::string, std::vector<std::string>> sectorPriority;  // e.g., "FRI": ["EID", "ALR"]

	// Sectors that contributed AOR destinations (e.g., HAM, HAMW)
	std::unordered_set<std::string> aorHostSectors;

	// Returns true if I currently control at least one sector that defines AOR destinations
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

	// ---------------- Custom Volumes (volumes.json) ----------------
	void LoadVolumesFromJSON(const std::string& volumesPath);
	const std::unordered_map<std::string, CustomVolume>& GetCustomVolumes() const { return customVolumes; }
private:
	std::string loadedSector;
	void LoadLOAsFromJSON();

	void OnControllerDisconnect(const EuroScopePlugIn::CController& controller);

	void OnGetControllerList();

	bool IsPointInsidePolygon(const CPosition& p,
		const std::vector<CPosition>& poly) const;

	std::unordered_map<std::string, CustomVolume> customVolumes;

	// volumes.json is global/static configuration.
	// Load only once at startup; do NOT reload on sector switches.
	bool volumesLoadAttempted = false;
	bool volumesLoadedOk = false;
	std::string volumesLoadedPath;

	// --- Debug: watch TopSky strip annotations ---
	bool debugWatchStripAnnotations = true; // set false to disable
	std::unordered_map<std::string, std::array<std::string, 9>> lastStripAnn;
	ULONGLONG lastStripScanTick = 0;

	std::unordered_set<std::string> cachedOnlineControllers;

	// Online-controller snapshot generation (bumps when cached online set refreshes)
	uint64_t onlineControllersVersion = 0;

	// Per-generation cache for ResolveControllingSector (sector -> controlling station)
	std::unordered_map<std::string, std::string> controllingSectorCache;
	uint64_t controllingSectorCacheVersion = 0;
	// ✅ Cached online controllers

	std::unordered_map<std::string, unsigned long long> routeSignature;  // hash of origin|dest|route to detect FP edits

	ULONGLONG lastOwnershipRecheckTime = 0;
	ULONGLONG lastOnlineFetchTime = 0;
};

// =============================
// Plugin Instance
// =============================
extern LOAPlugin plugin;