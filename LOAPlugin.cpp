// =========================
// File: LOAPlugin.cpp
// =========================

#include "stdafx.h"
#include "LOAPlugin.h"
#include <windows.h>
#include <fstream>
#include <shlwapi.h>
#include <unordered_set>
#include <json.hpp>
#include <algorithm>

using json = nlohmann::json;

extern "C" IMAGE_DOS_HEADER __ImageBase;

std::vector<LOAEntry> destinationLoas;
std::vector<LOAEntry> departureLoas;
std::vector<LOAEntry> destinationFallbackLoas;
std::vector<LOAEntry> departureFallbackLoas;
std::map<std::string, std::vector<LOAEntry>> loadedSectorLoas;

int nextFunctionId = 1000;

LOAPlugin::LOAPlugin()
    : CPlugIn(COMPATIBILITY_CODE, "LOA Plugin", "1.1", "Author", "LOA Plugin")
{
    startTick = GetTickCount64();
    coldStartUntil = startTick + 5000;   // 5s warm-up window
    coldStartActive = true;

    static bool registered = false;
    if (!registered) {
        RegisterTagItemType("LOA XFL", 1996);
        RegisterTagItemType("LOA XFL Detailed", 2000);
        RegisterTagItemType("Next Sector Ctrl", 2001);
        RegisterTagItemType("COP", 1997);
        registered = true;
    }

    LoadSectorOwnership();

    std::string sector = ControllerMyself().GetPositionId();
    if (!sector.empty()) {
        LoadLOAsFromJSON();

        // Prime controller snapshot early; helps avoid a cold-start thrash
        currentFrameOnlineControllers = GetOnlineControllersCached();
        if (GetTickCount64() >= coldStartUntil) {
            coldStartActive = false;
        }
    }
}

void LOAPlugin::OnControllerPositionUpdate(EuroScopePlugIn::CController controller)
{
    // Reload LOAs when my controller position changes, but FIRST invalidate caches holding LOAEntry*
    std::string sector = controller.GetPositionId();
    if (!sector.empty() && sector != this->loadedSector) {
        // Invalidate everything that can hold dangling LOAEntry* pointers
        sectorControlVersion++;
        matchedLOACache.clear();
        matchTimestamps.clear();
        matchVersions.clear();
        routeCache.clear();
        routeSetCache.clear();
        routeCacheTime.clear();
        routeSetCacheTime.clear();
        coordinationStates.clear();

        currentFrameMatchedEntry = nullptr;
        indexByWaypoint.clear();
        indexByNextSector.clear();

        // Load sector-specific LOAs (this also rebuilds indices)
        LoadLOAsFromJSON();

        // 🔄 Immediately refresh online-controllers snapshot for the new sector
        currentFrameOnlineControllers = GetOnlineControllersCached();

        // 🚫 Disable cold-start delay after a manual sector switch
        coldStartActive = false;
        coldStartUntil = 0;

        // Proactively clean per-flight caches (forces re-render paths)
        for (EuroScopePlugIn::CFlightPlan fp = FlightPlanSelectFirst(); fp.IsValid(); fp = FlightPlanSelectNext(fp)) {
            CleanupCache(fp.GetCallsign());
        }

        reloading = false;  // end guard
    }
}

LOAPlugin::~LOAPlugin() {}


void LOAPlugin::LoadSectorOwnership()
{
    char dllPath[MAX_PATH];
    GetModuleFileNameA(HINSTANCE(&__ImageBase), dllPath, sizeof(dllPath));
    std::string basePath(dllPath);
    size_t lastSlash = basePath.find_last_of("\\/");
    basePath = (lastSlash != std::string::npos) ? basePath.substr(0, lastSlash) : ".";

    std::string filePath = basePath + "\\loa_configs_json\\sector_ownership.json";
    std::ifstream in(filePath);
    if (!in.is_open()) {
        DisplayUserMessage("LOA Plugin", "Sector Ownership", "Failed to open sector_ownership.json", true, true, false, false, false);
        return;
    }

    json j;
    try {
        in >> j;
    }
    catch (...) {
        DisplayUserMessage("LOA Plugin", "Sector Ownership", "Failed to parse sector_ownership.json", true, true, false, false, false);
        return;
    }

    sectorOwnership.clear();
    sectorPriority.clear();

    if (j.contains("ownership")) {
        for (json::iterator it = j["ownership"].begin(); it != j["ownership"].end(); ++it) {
            sectorOwnership[it.key()] = it.value().get<std::vector<std::string>>();
        }
    }
    if (j.contains("priority")) {
        for (json::iterator it = j["priority"].begin(); it != j["priority"].end(); ++it) {
            sectorPriority[it.key()] = it.value().get<std::vector<std::string>>();
        }
    }

    DisplayUserMessage("LOA Plugin", "Sector Ownership", "sector_ownership.json loaded successfully", true, true, false, false, false);
}

void LOAPlugin::LoadLOAsFromJSON() {
    std::string mySector = ControllerMyself().GetPositionId();
    if (mySector.empty() || mySector == this->loadedSector) return;

    // Hard invalidate before reload (kept from prior patch)
    this->loadedSector = mySector;
    ++sectorControlVersion;
    currentFrameMatchedEntry = nullptr;
    currentFrameCallsign.clear();
    matchedLOACache.clear();
    matchTimestamps.clear();
    matchVersions.clear();
    routeCache.clear();
    routeCacheTime.clear();
    routeSignature.clear();
    coordinationStates.clear();
    currentFrameRoutePoints.clear();
    currentFrameRouteSet.clear();
    currentFrameOnlineControllers.clear();
    lastOnlineFetchTime = 0;
    renderCache.clear();

    char dllPath[MAX_PATH];
    GetModuleFileNameA(HINSTANCE(&__ImageBase), dllPath, sizeof(dllPath));

    std::string basePath(dllPath);
    size_t lastSlash = basePath.find_last_of("\\/");
    basePath = (lastSlash != std::string::npos) ? basePath.substr(0, lastSlash) : ".";
    std::string baseFolder = basePath + "\\loa_configs_json\\";
    std::string filePath = baseFolder + "LOA.json";

    // Load order: my sector, then owned
    std::vector<std::string> sectorsToLoadOrdered;
    sectorsToLoadOrdered.push_back(mySector);
    const auto ownIt = sectorOwnership.find(mySector);
    if (ownIt != sectorOwnership.end()) {
        for (const auto& s : ownIt->second) {
            if (_stricmp(s.c_str(), mySector.c_str()) != 0) sectorsToLoadOrdered.push_back(s);
        }
    }

    // Only keep the two main lists now
    destinationLoas.clear();
    departureLoas.clear();

    destinationFallbackLoas.clear();
    departureFallbackLoas.clear();

    aorDestinationSet.clear();
    aorDestinationPrefixes.clear();
    aorHostSectors.clear();

    auto processAirportList = [](const std::vector<std::string>& list,
        std::unordered_set<std::string>& exact,
        std::vector<std::string>& prefixes) {
            for (const std::string& a : list) {
                if (a.length() == 4) exact.insert(a);
                else prefixes.push_back(a);
            }
        };

    auto parseLOAList = [&](const json& array, const std::string& sector) {
        std::vector<LOAEntry> result;
        result.reserve(array.size());
        for (const auto& item : array) {
            LOAEntry loa;
            loa.sectors.push_back(sector);

            if (item.contains("origins")) {
                loa.originAirports = item["origins"].get<std::vector<std::string>>();
                processAirportList(loa.originAirports, loa.originAirportSet, loa.originAirportPrefixes);
            }
            if (item.contains("destinations")) {
                loa.destinationAirports = item["destinations"].get<std::vector<std::string>>();
                processAirportList(loa.destinationAirports, loa.destinationAirportSet, loa.destinationAirportPrefixes);
            }
            if (item.contains("excludeDestinations")) {
                loa.excludeDestinationAirports = item["excludeDestinations"].get<std::vector<std::string>>();
                processAirportList(loa.excludeDestinationAirports,
                    loa.excludeDestinationAirportSet,
                    loa.excludeDestinationAirportPrefixes);
            }
            if (item.contains("excludeOrigins")) {
                loa.excludeOriginAirports = item["excludeOrigins"].get<std::vector<std::string>>();
                processAirportList(loa.excludeOriginAirports,
                    loa.excludeOriginAirportSet,
                    loa.excludeOriginAirportPrefixes);
            }
            if (item.contains("waypoints")) loa.waypoints = item["waypoints"].get<std::vector<std::string>>();
            for (auto& __w : loa.waypoints) { std::transform(__w.begin(), __w.end(), __w.begin(), ::tolower); }

            if (item.contains("nextSectors")) loa.nextSectors = item["nextSectors"].get<std::vector<std::string>>();
            if (item.contains("copText"))    loa.copText = item["copText"].get<std::string>();
            if (item.contains("xfl"))        loa.xfl = item["xfl"].get<int>();
            if (item.contains("minAltitudeFt")) loa.minAltitudeFt = item["minAltitudeFt"].get<int>();
            result.push_back(std::move(loa));
        }
        return result;
        };

    std::ifstream inFile(filePath);
    if (!inFile.is_open()) {
        DisplayUserMessage("LOA Plugin", "JSON Load Error", ("Missing: " + filePath).c_str(), true, false, false, false, false);
        return;
    }

    json config;
    try {
        inFile >> config;
    }
    catch (const std::exception& e) {
        DisplayUserMessage("LOA Plugin", "JSON Parse Error", e.what(), true, true, true, true, false);
        return;
    }

    for (const std::string& sector : sectorsToLoadOrdered) {
        if (!config.contains(sector)) continue;
        const json& sectorConfig = config[sector];

        if (sectorConfig.contains("destinationLoas")) {
            auto list = parseLOAList(sectorConfig["destinationLoas"], sector);
            destinationLoas.insert(destinationLoas.end(), list.begin(), list.end());
        }
        if (sectorConfig.contains("departureLoas")) {
            auto list = parseLOAList(sectorConfig["departureLoas"], sector);
            departureLoas.insert(departureLoas.end(), list.begin(), list.end());
        }
        if (sectorConfig.contains("destinationFallbackLoas")) {
            auto list = parseLOAList(sectorConfig["destinationFallbackLoas"], sector);
            destinationFallbackLoas.insert(destinationFallbackLoas.end(), list.begin(), list.end());
        }
        if (sectorConfig.contains("departureFallbackLoas")) {
            auto list = parseLOAList(sectorConfig["departureFallbackLoas"], sector);
            departureFallbackLoas.insert(departureFallbackLoas.end(), list.begin(), list.end());
        }
        if (sectorConfig.contains("aorDestinations")) {
            auto aorList = sectorConfig["aorDestinations"].get<std::vector<std::string>>();
            processAirportList(aorList, aorDestinationSet, aorDestinationPrefixes);
            aorHostSectors.insert(sector);
        }

    }

    // Rebuild indices (only over the two active lists)
    indexByWaypoint.clear();
    indexByNextSector.clear();

    auto indexEntries = [&](const std::vector<LOAEntry>& entries) {
        for (const LOAEntry& entry : entries) {
            const LOAEntry* ptr = &entry;
            for (const std::string& wp : entry.waypoints) {
                indexByWaypoint[wp].push_back(ptr);
            }
            for (const std::string& next : entry.nextSectors) {
                indexByNextSector[next].push_back(ptr);
            }
        }
        };

    indexEntries(destinationLoas);
    indexEntries(departureLoas);
    
    currentFrameOnlineControllers = GetOnlineControllersCached();
    if (GetTickCount64() >= coldStartUntil) {
        coldStartActive = false;
    }

    DisplayUserMessage("LOA Plugin", "LOA Load", ("LOAs loaded for: " + mySector).c_str(), true, true, true, true, false);
}


std::string LOAPlugin::ResolveControllingSector(const std::string& sector, const std::unordered_set<std::string>& onlineControllers)
{
    const auto& prioList = sectorPriority[sector];
    for (const auto& s : prioList) {
        if (onlineControllers.count(s)) return s;
    }
    return {}; // No one online
}

bool LOAPlugin::IsLOARelevantState(int state) {
    static const std::unordered_set<int> validStates = {
        FLIGHT_PLAN_STATE_NOTIFIED,
        FLIGHT_PLAN_STATE_COORDINATED,
        FLIGHT_PLAN_STATE_TRANSFER_TO_ME_INITIATED,
        FLIGHT_PLAN_STATE_TRANSFER_FROM_ME_INITIATED,
        FLIGHT_PLAN_STATE_ASSUMED,
        FLIGHT_PLAN_STATE_REDUNDANT
    };
    return validStates.count(state) > 0;
}

const std::unordered_set<std::string>& LOAPlugin::GetOnlineControllersCached()
{
    ULONGLONG currentTime = GetTickCount64();
    if (currentTime - lastOnlineFetchTime > 10000 || cachedOnlineControllers.empty()) {
        cachedOnlineControllers.clear();
        for (EuroScopePlugIn::CController c = ControllerSelectFirst(); c.IsValid(); c = ControllerSelectNext(c)) {
            cachedOnlineControllers.insert(c.GetPositionId());
        }
        lastOnlineFetchTime = currentTime;
    }
    return cachedOnlineControllers;
}

std::string LOAPlugin::GetIndicatedNextSectorStation(const std::string& nextSector)
{
    if (nextSector.empty()) return {};

    // Use the cached online controllers snapshot if available.
    const auto& online = !currentFrameOnlineControllers.empty()
        ? currentFrameOnlineControllers
        : GetOnlineControllersCached();

    // Resolve who currently controls the next sector (dynamic ownership logic)
    std::string controlling = ResolveControllingSector(nextSector, online);

    // If nobody is online or it's our own sector, show nothing.
    const std::string me = ControllerMyself().GetPositionId();
    if (controlling.empty() || _stricmp(controlling.c_str(), me.c_str()) == 0)
        return {};

    return controlling; // e.g., "HAM", "EID", etc.
}


bool LOAPlugin::IsControllerOnlineCached(const std::string& controllerId, const std::unordered_set<std::string>& onlineControllers)
{
    return onlineControllers.count(controllerId) > 0;
}

bool LOAPlugin::MatchesAirport(const std::unordered_set<std::string>& exactSet,
    const std::vector<std::string>& prefixes,
    const std::string& airport)
{
    // Fast exact match
    if (exactSet.count(airport) > 0) return true;

    // Prefix match
    for (const auto& prefix : prefixes) {
        if (airport.compare(0, prefix.length(), prefix) == 0)
            return true;
    }
    return false;
}

bool LOAPlugin::IsAnyAORHostOnline(const std::unordered_set<std::string>& onlineControllers) const {
    const std::string myId = ControllerMyself().GetPositionId();
    for (const auto& host : aorHostSectors) {
        // Which station controls this sector right now?
        std::string ctrl = const_cast<LOAPlugin*>(this)->ResolveControllingSector(host, onlineControllers);
        if (!ctrl.empty() && _stricmp(ctrl.c_str(), myId.c_str()) != 0) {
            // Someone else (e.g., HAM) is online and controls it
            return true;
        }
    }
    return false;
}

const std::vector<std::string>& LOAPlugin::GetCachedRoutePoints(const EuroScopePlugIn::CFlightPlan& fp) {
    static std::vector<std::string> empty;

    std::string callsign = fp.GetCallsign();
    ULONGLONG now = GetTickCount64();

    // 10s cache validity
    if (routeCache.count(callsign) && now - routeCacheTime[callsign] < 10000) {
        return routeCache[callsign];
    }

    auto route = fp.GetExtractedRoute();
    std::vector<std::string> routePoints;
    for (int i = 0; i < route.GetPointsNumber(); ++i)
        routePoints.emplace_back(route.GetPointName(i));

    routeCache[callsign] = std::move(routePoints);
    routeCacheTime[callsign] = now;

    return routeCache[callsign];
}

void LOAPlugin::CleanupCache(const std::string& callsign) {
    matchedLOACache.erase(callsign);
    routeCache.erase(callsign);
    routeCacheTime.erase(callsign);
    coordinationStates.erase(callsign);
    matchTimestamps.erase(callsign);
    matchVersions.erase(callsign);
}

void LOAPlugin::OnFlightPlanStateChange(EuroScopePlugIn::CFlightPlan fp) {
    if (!fp.IsValid()) return;

    int state = fp.GetState();
    if (state == FLIGHT_PLAN_STATE_NON_CONCERNED || state == FLIGHT_PLAN_STATE_REDUNDANT) {
        CleanupCache(fp.GetCallsign());
    }
}

void LOAPlugin::OnFlightPlanCoordinationStateChange(CFlightPlan fp, int coordinationType, int newState)
{
    if (!fp.IsValid()) return;

    std::string callsign = fp.GetCallsign();

    // Only handle exit altitude coordination
    if (coordinationType == EuroScopePlugIn::TAG_ITEM_TYPE_COPN_COPX_ALTITUDE) {
        CoordinationInfo& info = coordinationStates[callsign];
        info.exitAltitude = fp.GetExitCoordinationAltitude();
        info.exitAltitudeState = newState;
    }

    // Handle point name coordination if needed
    if (coordinationType == EuroScopePlugIn::TAG_ITEM_TYPE_COPN_COPX_NAME) {
        CoordinationInfo& info = coordinationStates[callsign];
        info.exitPoint = fp.GetExitCoordinationPointName();
        info.exitPointState = newState;
    }
}

void LOAPlugin::CheckForOwnershipChange() {
    std::string mySector = ControllerMyself().GetPositionId();
    if (mySector.empty()) return;

    auto oldControllers = currentFrameOnlineControllers;
    currentFrameOnlineControllers = GetOnlineControllersCached();

    bool changed = false;
    std::vector<std::string> checkSectors = sectorOwnership[mySector];
    checkSectors.push_back(mySector);  // Include your own sector

    for (const auto& s : checkSectors) {
        std::string newCtrl = ResolveControllingSector(s, currentFrameOnlineControllers);
        std::string oldCtrl = ResolveControllingSector(s, oldControllers);
        if (_stricmp(newCtrl.c_str(), oldCtrl.c_str()) != 0) {
            changed = true;
            break;
        }
    }

    // 🚀 Only if something changed do we rebuild LOAs
    if (changed) {
        reloading = true;  // <── Begin reload guard

        plugin.sectorControlVersion++;
        plugin.matchedLOACache.clear();
        plugin.matchTimestamps.clear();
        plugin.routeCache.clear();
        plugin.routeCacheTime.clear();
        plugin.coordinationStates.clear();
        plugin.matchVersions.clear();

        LoadLOAsFromJSON();

        // Force tag redraw immediately
        EuroScopePlugIn::CFlightPlan plan = FlightPlanSelectFirst();
        while (plan.IsValid()) {
            plugin.CleanupCache(plan.GetCallsign());
            plan = FlightPlanSelectNext(plan);
        }

        reloading = false; // ✅ end reload guard safely
    }
}

void LOAPlugin::OnGetControllerList() {
    CheckForOwnershipChange(); // ✅ Detect change and force rematch
}

void LOAPlugin::OnControllerDisconnect(const EuroScopePlugIn::CController& controller) {
    CheckForOwnershipChange();
}

void LOAPlugin::OnGetTagItem(
    EuroScopePlugIn::CFlightPlan flightPlan,
    EuroScopePlugIn::CRadarTarget radarTarget,
    int itemCode,
    int tagData,
    char sItemString[16],
    int* pColorCode,
    COLORREF* pRGB,
    double* pFontSize)
{
    const std::string callsign = flightPlan.GetCallsign();
    const auto& fpd = flightPlan.GetFlightPlanData();
    int clearedAltitude = flightPlan.GetClearedAltitude();
    int finalAltitude = flightPlan.GetFinalAltitude();
    std::string origin = fpd.GetOrigin();
    std::string destination = fpd.GetDestination();

    plugin.lastTagData = { callsign, clearedAltitude, finalAltitude, origin, destination };

    // ---------- MICRO-CACHE: reuse last render for this callsign+itemCode (<= 750–1000 ms) ----------
    ULONGLONG now = GetTickCount64();
    const int sectorControlVersion = plugin.sectorControlVersion;
    const int coordAlt = flightPlan.GetExitCoordinationAltitude();
    const int coordAltSt = flightPlan.GetExitCoordinationAltitudeState();
    const std::string coordPt = flightPlan.GetExitCoordinationPointName();
    const int coordPtSt = flightPlan.GetExitCoordinationNameState();

    const std::string renderKey = callsign + ":" + std::to_string(itemCode);
    auto itRC = renderCache.find(renderKey);
    if (itRC != renderCache.end()) {
        const RenderItemCache& rc = itRC->second;
        const bool fresh = (now - rc.ts) <= 2000;
        const bool sameInputs =
            rc.clearedAlt == clearedAltitude &&
            rc.finalAlt == finalAltitude &&
            rc.coordAlt == coordAlt &&
            rc.coordAltState == coordAltSt &&
            rc.coordPoint == coordPt &&
            rc.coordPointState == coordPtSt &&
            rc.sectorVersion == sectorControlVersion;

        if (fresh && sameInputs) {
            strncpy_s(sItemString, 16, rc.text, _TRUNCATE);
            if (pColorCode) *pColorCode = rc.colorCode;
            if (pRGB)       *pRGB = rc.rgb;
            if (pFontSize)  *pFontSize = rc.fontSize;
            return;
        }
    }
    // -----------------------------------------------------------------------------------------------

    // Precompute and cache route + controller data per frame (gate 250 -> 750 ms)
    if (callsign != plugin.currentFrameCallsign || now - plugin.currentFrameTimestamp > 1000) {
        plugin.currentFrameCallsign = callsign;
        plugin.currentFrameTimestamp = now;
        plugin.currentFrameOnlineControllers = plugin.GetOnlineControllersCached();
        plugin.currentFrameRoutePoints = plugin.GetCachedRoutePoints(flightPlan);

        // Detect FP edits (origin/dest/route) and invalidate per-callsign caches immediately
        {
            std::string sigConcat = origin + "|" + destination + "|";
            for (const auto& rp : plugin.currentFrameRoutePoints) {
                sigConcat += rp;
                sigConcat += ",";
            }
            // FNV-1a
            size_t h = 1469598103934665603ull;
            for (unsigned char c : sigConcat) { h ^= c; h *= 1099511628211ull; }
            auto itSig = plugin.routeSignature.find(callsign);
            if (itSig == plugin.routeSignature.end() || itSig->second != h) {
                plugin.routeSignature[callsign] = h;
                plugin.CleanupCache(callsign); // force recompute match & route caches
            }
        }

        plugin.currentFrameRouteSet = plugin.GetCachedRouteSet(flightPlan);
        plugin.currentFrameMatchedEntry = MatchLoaEntry(flightPlan, plugin.currentFrameOnlineControllers);
    }

    // ---- Render selected tag item
    switch (itemCode)
    {
    case 1996:
        RenderXFLTagItem(flightPlan, radarTarget, tagData, sItemString, pColorCode, pRGB, pFontSize);
        break;

    case 2000:
        RenderXFLDetailedTagItem(flightPlan, radarTarget, tagData, sItemString, pColorCode, pRGB, pFontSize);
        break;

    case 1997:
        RenderCOPTagItem(flightPlan, radarTarget, tagData, sItemString, pColorCode, pRGB, pFontSize);
        break;

        // ✅ Next Sector Ctrl tag (old behavior — no ES fallback)
    case 2001:
    {
        // Blank by default
        sItemString[0] = '\0';
        if (pColorCode) *pColorCode = TAG_COLOR_DEFAULT;

        const LOAEntry* match = plugin.currentFrameMatchedEntry;
        if (!match || match->nextSectors.empty()) break;

        // Gate altitude: prefer XFL; fall back to minAltitudeFt (for fallback LOAs)
        int gateFeet = 0;
        if (match->xfl > 0) {
            gateFeet = match->xfl * 100;
        }
        else if (match->minAltitudeFt > 0) {
            gateFeet = match->minAltitudeFt;
        }

        // Use final altitude (RFL) only, as requested previously
        const int rflFeet = flightPlan.GetFinalAltitude();

        // Show ONLY if RFL > gate (strictly above)
        if (gateFeet > 0 && rflFeet <= gateFeet) break;

        // Resolve & display controlling station for the first next sector
        const std::string& next = match->nextSectors.front();
        std::string station = plugin.GetIndicatedNextSectorStation(next);
        if (!station.empty()) {
            strncpy_s(sItemString, 16, station.c_str(), _TRUNCATE);
            if (pColorCode) *pColorCode = TAG_COLOR_DEFAULT;
        }
        // else remain blank
        break;
    }

    default:
        break;
    }

    // ---------- Store result in micro-cache for this callsign+itemCode ----------
    {
        RenderItemCache rc;
        rc.ts = now;
        strncpy_s(rc.text, 16, sItemString ? sItemString : "", _TRUNCATE);
        rc.colorCode = pColorCode ? *pColorCode : 0;
        rc.rgb = pRGB ? *pRGB : 0;
        rc.fontSize = pFontSize ? *pFontSize : 0.0;
        rc.clearedAlt = clearedAltitude;
        rc.finalAlt = finalAltitude;
        rc.coordAlt = coordAlt;
        rc.coordAltState = coordAltSt;
        rc.coordPoint = coordPt;
        rc.coordPointState = coordPtSt;
        rc.sectorVersion = sectorControlVersion;
        renderCache[renderKey] = rc;
    }
}


const std::unordered_set<std::string>& LOAPlugin::GetCachedRouteSet(const EuroScopePlugIn::CFlightPlan& fp) {
    static std::unordered_set<std::string> empty;
    if (!fp.IsValid()) return empty;
    std::string callsign = fp.GetCallsign();
    ULONGLONG now = GetTickCount64();
    // 10s cache validity
    if (routeSetCache.count(callsign) && now - routeSetCacheTime[callsign] < 10000) {
        return routeSetCache[callsign];
    }
    const auto& pts = GetCachedRoutePoints(fp);
    std::unordered_set<std::string> s;
    s.reserve(pts.size() * 2 + 4);
    for (auto p : pts) {
        std::transform(p.begin(), p.end(), p.begin(), ::tolower);
        s.insert(std::move(p));
    }
    routeSetCache[callsign] = std::move(s);
    routeSetCacheTime[callsign] = now;
    return routeSetCache[callsign];
}

// =============================
// Helper: TryGetLoaMatch
// =============================
bool LOAPlugin::TryGetLoaMatch(
    const EuroScopePlugIn::CFlightPlan& fp,
    const std::unordered_set<std::string>& onlineControllers,
    const std::vector<std::string>& /*routePoints*/,
    LoaMatchResult& out)
{
    out.entry = NULL;
    out.isDeparture = false;
    if (!fp.IsValid()) return false;
    if (!IsLOARelevantState(fp.GetState())) return false;

    // Use existing matcher (cached internally by callsign + sectorControlVersion)
    const auto& online = onlineControllers.empty() ? currentFrameOnlineControllers : onlineControllers;
    const LOAEntry* m = MatchLoaEntry(fp, online);
    if (m) {
        out.entry = m;
        // Heuristic: if originAirports present -> departure; else destination.
        if (!m->originAirports.empty()) out.isDeparture = true;
        else if (!m->destinationAirports.empty()) out.isDeparture = false;
        else {
            // Fall back: if only waypoints/nextSectors set, consider arrival as default
            out.isDeparture = false;
        }
        return true;
    }
    return false;
}

LOAPlugin plugin;