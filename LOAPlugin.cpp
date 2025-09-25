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

using json = nlohmann::json;

extern "C" IMAGE_DOS_HEADER __ImageBase;

std::vector<LOAEntry> destinationLoas;
std::vector<LOAEntry> departureLoas;
std::vector<LOAEntry> lorArrivals;
std::vector<LOAEntry> lorDepartures;
std::vector<LOAEntry> fallbackLoas;

std::unordered_map<std::string, std::vector<const LOAEntry*>> indexByWaypoint;
std::unordered_map<std::string, std::vector<const LOAEntry*>> indexByNextSector;

int nextFunctionId = 1000;

LOAPlugin::LOAPlugin()
    : CPlugIn(COMPATIBILITY_CODE, "LOA Plugin", "1.1", "Author", "LOA Plugin")
{
    static bool registered = false;
    if (!registered) {
        RegisterTagItemType("LOA XFL", 1996);
        RegisterTagItemType("LOA XFL Detailed", 2000);
        RegisterTagItemType("PEL", 2001);
        RegisterTagItemType("COP", 1997);
        registered = true;
    }

    LoadSectorOwnership();

    std::string sector = ControllerMyself().GetPositionId();
    if (!sector.empty()) {
        LoadLOAsFromJSON();
    }
}

void LOAPlugin::OnControllerPositionUpdate(EuroScopePlugIn::CController controller)
{
    // Example: reload LOAs when controller position changes
    std::string sector = controller.GetPositionId();
    if (!sector.empty() && sector != this->loadedSector) {
        LoadLOAsFromJSON();
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
    this->loadedSector = mySector;

    char dllPath[MAX_PATH];
    GetModuleFileNameA(HINSTANCE(&__ImageBase), dllPath, sizeof(dllPath));

    std::string basePath(dllPath);
    size_t lastSlash = basePath.find_last_of("\\/");
    basePath = (lastSlash != std::string::npos) ? basePath.substr(0, lastSlash) : ".";
    std::string baseFolder = basePath + "\\loa_configs_json\\";
    std::string filePath = baseFolder + "LOA.json";  // ✅ changed filename

    // === PATCH START: deterministic sector load order ===
    // Collect all sectors to load IN ORDER: my sector first, then owned sectors in listed order
    std::vector<std::string> sectorsToLoadOrdered;
    sectorsToLoadOrdered.push_back(mySector);

    const auto ownIt = sectorOwnership.find(mySector);
    if (ownIt != sectorOwnership.end()) {
        for (const auto& s : ownIt->second) {
            if (_stricmp(s.c_str(), mySector.c_str()) != 0) {
                sectorsToLoadOrdered.push_back(s);
            }
        }
    }
    // === PATCH END ===

    // Clear previous LOAs
    destinationLoas.clear();
    departureLoas.clear();
    lorArrivals.clear();
    lorDepartures.clear();
    fallbackLoas.clear();

    auto processAirportList = [](const std::vector<std::string>& list,
        std::unordered_set<std::string>& exact,
        std::vector<std::string>& prefixes) {
            for (const std::string& a : list) {
                if (a.length() == 4) exact.insert(a);
                else prefixes.push_back(a);
            }
        };

    auto parseLOAList = [&](const json& array, const std::string& sector, bool /*isFallback*/ = false) {
        std::vector<LOAEntry> result;
        for (const auto& item : array) {
            LOAEntry loa;
            loa.sectors.push_back(sector);  // ✅ track source sector

            if (item.contains("origins")) {
                loa.originAirports = item["origins"].get<std::vector<std::string>>();
                processAirportList(loa.originAirports, loa.originAirportSet, loa.originAirportPrefixes);
            }
            if (item.contains("destinations")) {
                loa.destinationAirports = item["destinations"].get<std::vector<std::string>>();
                processAirportList(loa.destinationAirports, loa.destinationAirportSet, loa.destinationAirportPrefixes);
            }
            if (item.contains("waypoints")) loa.waypoints = item["waypoints"].get<std::vector<std::string>>();
            if (item.contains("nextSectors")) loa.nextSectors = item["nextSectors"].get<std::vector<std::string>>();
            if (item.contains("copText")) loa.copText = item["copText"].get<std::string>();
            if (item.contains("xfl")) loa.xfl = item["xfl"].get<int>();
            if (item.contains("minAltitudeFt")) loa.minAltitudeFt = item["minAltitudeFt"].get<int>();
            result.push_back(loa);
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

    // === PATCH: use the ordered list here ===
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
        if (sectorConfig.contains("lorArrivals")) {
            auto list = parseLOAList(sectorConfig["lorArrivals"], sector);
            lorArrivals.insert(lorArrivals.end(), list.begin(), list.end());
        }
        if (sectorConfig.contains("lorDepartures")) {
            auto list = parseLOAList(sectorConfig["lorDepartures"], sector);
            lorDepartures.insert(lorDepartures.end(), list.begin(), list.end());
        }
        if (sectorConfig.contains("fallbackLoas")) {
            auto list = parseLOAList(sectorConfig["fallbackLoas"], sector, true);
            fallbackLoas.insert(fallbackLoas.end(), list.begin(), list.end());
        }
    }

    // Index LOAs by waypoint and nextSector
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
    indexEntries(lorArrivals);
    indexEntries(lorDepartures);
    indexEntries(fallbackLoas);

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
        FLIGHT_PLAN_STATE_ASSUMED
    };
    return validStates.count(state) > 0;
}

const std::unordered_set<std::string>& LOAPlugin::GetOnlineControllersCached()
{
    DWORD currentTime = GetTickCount64();
    if (currentTime - lastOnlineFetchTime > 10000 || cachedOnlineControllers.empty()) {
        cachedOnlineControllers.clear();
        for (EuroScopePlugIn::CController c = ControllerSelectFirst(); c.IsValid(); c = ControllerSelectNext(c)) {
            cachedOnlineControllers.insert(c.GetPositionId());
        }
        lastOnlineFetchTime = currentTime;
    }
    return cachedOnlineControllers;
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

const std::vector<std::string>& LOAPlugin::GetCachedRoutePoints(const EuroScopePlugIn::CFlightPlan& fp) {
    static std::vector<std::string> empty;

    std::string callsign = fp.GetCallsign();
    ULONGLONG now = GetTickCount64();

    // 6s cache validity
    if (routeCache.count(callsign) && now - routeCacheTime[callsign] < 6000) {
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

    if (changed) {
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
            plugin.CleanupCache(plan.GetCallsign());  // ⬅ ensure call to cleanup
            plan = FlightPlanSelectNext(plan);
        }
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

    // Precompute and cache route + controller data per frame
    DWORD now = GetTickCount64();
    if (callsign != plugin.currentFrameCallsign || now - plugin.currentFrameTimestamp > 100) {
        plugin.currentFrameCallsign = callsign;
        plugin.currentFrameTimestamp = now;
        plugin.currentFrameOnlineControllers = plugin.GetOnlineControllersCached();
        plugin.currentFrameRoutePoints = plugin.GetCachedRoutePoints(flightPlan);
    }

    switch (itemCode)
    {
    case 1996:
        RenderXFLTagItem(flightPlan, radarTarget, tagData, sItemString, pColorCode, pRGB, pFontSize);
        break;
    case 2000:
        RenderXFLDetailedTagItem(flightPlan, radarTarget, tagData, sItemString, pColorCode, pRGB, pFontSize);
        break;
    case 2001:
        RenderPELTagItem(flightPlan, radarTarget, tagData, sItemString, pColorCode, pRGB, pFontSize);
        break;
    case 1997:
        RenderCOPTagItem(flightPlan, radarTarget, tagData, sItemString, pColorCode, pRGB, pFontSize);
        break;
    default:
        break;
    }
}

LOAPlugin plugin;