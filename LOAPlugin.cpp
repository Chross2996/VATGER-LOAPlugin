// =========================
// File: LOAPlugin.cpp
// =========================

#include "stdafx.h"
#include "LOAPlugin.h"
#define NOMINMAX
#include <windows.h>
#include <fstream>
#include <shlwapi.h>
#include <unordered_set>
#include <json.hpp>
#include <algorithm>
#include <cctype>   // for std::toupper
#include <cmath>
#include <cstdlib>

using json = nlohmann::json;

static std::string AddArrowPrefix(const std::string& text)
{
    // NOTE: We used to prefix popup items with "> " for readability.
    // Users requested a compact list, so keep menu text unchanged.
    return text;
}

extern "C" IMAGE_DOS_HEADER __ImageBase;

std::vector<LOAEntry> destinationLoas;
std::vector<LOAEntry> departureLoas;
std::vector<LOAEntry> destinationFallbackLoas;
std::vector<LOAEntry> departureFallbackLoas;
std::map<std::string, std::vector<LOAEntry>> loadedSectorLoas;

std::string NormalizeRunway(const std::string& in)
{
    return std::string();
}

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
        RegisterTagItemFunction("LOA Next Sector Handoff", FunctionIds::NEXT_SECTOR_HANDOFF_MENU);
        registered = true;
    }

    LoadSectorOwnership();

    // Load optional custom volumes (volumes.json) from the same folder as sector_ownership.json
    {
        char dllPath[MAX_PATH];
        GetModuleFileNameA(HINSTANCE(&__ImageBase), dllPath, sizeof(dllPath));
        std::string basePath(dllPath);
        size_t lastSlash = basePath.find_last_of("\\/");
        basePath = (lastSlash != std::string::npos) ? basePath.substr(0, lastSlash) : ".";
        std::string volumesPath = basePath + "\\loa_configs_json\\volumes.json";
        LoadVolumesFromJSON(volumesPath);
    }

    std::string sector = ControllerMyself().GetPositionId();
    if (!sector.empty()) {
        LoadLOAsFromJSON();

        // Prime controller snapshot early; helps avoid a cold-start thrash
        currentFrameOnlineControllers = GetOnlineControllersCached();
        if (GetTickCount64() >= coldStartUntil) {
            coldStartActive = false;
        }
    }

    // Prime active runway caches once at startup (and whenever ES notifies changes)
    UpdateActiveRunwaysFromSectorFile();
}

bool LOAPlugin::IsPointInsidePolygon(const CPosition& p,
    const std::vector<CPosition>& poly) const
{
    const size_t n = poly.size();
    if (n < 3) return false;

    bool inside = false;
    const double x = p.m_Longitude;
    const double y = p.m_Latitude;

    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const double xi = poly[i].m_Longitude;
        const double yi = poly[i].m_Latitude;
        const double xj = poly[j].m_Longitude;
        const double yj = poly[j].m_Latitude;

        const bool intersect =
            ((yi > y) != (yj > y)) &&
            (x < (xj - xi) * (y - yi) / ((yj - yi) + 1e-9) + xi);

        if (intersect)
            inside = !inside;
    }
    return inside;
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
        coordinationStates.clear();
        routeCacheTime.clear();
        routeSetCacheTime.clear();

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
// Load custom volumes from a separate JSON file (optional).
// Path is usually: <plugin folder>\loa_configs_json\volumes.json
void LOAPlugin::LoadVolumesFromJSON(const std::string& volumesPath)
{
    // Load volumes only once per plugin session. Volumes are global/static configuration and
    // reloading them during sector switches is redundant and can be crash-prone due to
    // EuroScope callback re-entrancy.
    if (volumesLoadAttempted) {
        return;
    }
    volumesLoadAttempted = true;
    volumesLoadedPath = volumesPath;

    customVolumes.clear();

    std::ifstream f(volumesPath.c_str(), std::ios::in);
    if (!f.good()) {
        std::string msg = std::string("volumes.json NOT found (optional): ") + volumesPath;
        DisplayUserMessage("LOA Plugin", "Volumes", msg.c_str(), true, true, false, false, false);
        volumesLoadedOk = false;
        return;
    }

    json j;
    try {
        f >> j;
    }
    catch (...) {
        std::string msg = std::string("Failed to parse volumes.json: ") + volumesPath;
        DisplayUserMessage("LOA Plugin", "Volumes", msg.c_str(), true, true, false, false, false);
        volumesLoadedOk = false;
        return;
    }

    if (!j.is_object() || !j.contains("volumes") || !j["volumes"].is_array()) {
        std::string msg = std::string("volumes.json missing 'volumes' array: ") + volumesPath;
        DisplayUserMessage("LOA Plugin", "Volumes", msg.c_str(), true, true, false, false, false);
        volumesLoadedOk = false;
        return;
    }

    for (const auto& v : j["volumes"]) {
        if (!v.is_object()) continue;
        if (!v.contains("id") || !v["id"].is_string()) continue;

        CustomVolume cv;
        cv.id = v["id"].get<std::string>();

        // Altitude band: prefer feet, else FL*100
        if (v.contains("lowerFt") && v["lowerFt"].is_number() && v.contains("upperFt") && v["upperFt"].is_number()) {
            cv.lowerFt = v["lowerFt"].get<double>();
            cv.upperFt = v["upperFt"].get<double>();
        }
        else {
            int loFL = 0;
            int hiFL = 999;
            if (v.contains("lowerFL") && v["lowerFL"].is_number_integer()) loFL = v["lowerFL"].get<int>();
            if (v.contains("upperFL") && v["upperFL"].is_number_integer()) hiFL = v["upperFL"].get<int>();
            cv.lowerFt = (double)loFL * 100.0;
            cv.upperFt = (double)hiFL * 100.0;
        }

        cv.polygon.clear();
        if (v.contains("polygon") && v["polygon"].is_array()) {
            for (const auto& pt : v["polygon"]) {
                if (!pt.is_array() || pt.size() < 2) continue;
                if (!pt[0].is_number() || !pt[1].is_number()) continue;
                cv.polygon.push_back(std::make_pair(pt[0].get<double>(), pt[1].get<double>()));
            }
        }

        if (cv.polygon.size() >= 3) {
            customVolumes[cv.id] = cv;
        }
    }

    char buf[256];
    sprintf_s(buf, sizeof(buf), "volumes.json loaded successfully (%d volumes)", (int)customVolumes.size());
    std::string msg = std::string(buf) + ": " + volumesPath;
    DisplayUserMessage("LOA Plugin", "Volumes", msg.c_str(), true, true, false, false, false);

    volumesLoadedOk = true;
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
    // volumes.json is loaded once at plugin startup; do NOT reload here.
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
        std::vector<std::string>& prefixes)
        {
            for (std::string a : list) {
                // Treat trailing '*' as "wildcard" → use the part before '*' as prefix
                if (!a.empty() && a.back() == '*') {
                    a.pop_back();  // remove '*', e.g. "EDL*" → "EDL", "EH*" → "EH"
                }

                // 4-letter = exact ICAO, shorter = prefix ("EDL", "EH", "ED", etc.)
                if (a.length() == 4) {
                    exact.insert(a);
                }
                else if (!a.empty()) {
                    prefixes.push_back(a);
                }
                // empty after stripping '*' → ignore
            }
        };

    auto parseLOAList = [&](const json& array, const std::string& sector, LOAListKind kind) {
        std::vector<LOAEntry> result;
        result.reserve(array.size());
        for (const auto& item : array) {
            LOAEntry loa;
            loa.sectors.push_back(sector);

            loa.listKind = kind;
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

            // --- Runway constraints (optional) ---
            // Supported keys:
            //  - "runways": ["05","23"]  (applies based on list kind: dep uses DEP active runways at origin; dest uses ARR active runways at destination)
            //  - "depRunways": [...]     (only used for Departure / DepartureFallback lists)
            //  - "arrRunways": [...]     (only used for Destination / DestinationFallback lists)
            auto readRunways = [&](const json& j)->std::vector<std::string> {
                std::vector<std::string> out;

                try {
                    out = j.get<std::vector<std::string>>();
                }
                catch (...) {
                    return {};
                }

                // Normalize once at load: trim + uppercase
                const char* ws = " \t\r\n";
                for (auto& r : out)
                {
                    const size_t start = r.find_first_not_of(ws);
                    if (start == std::string::npos) { r.clear(); continue; }
                    const size_t end = r.find_last_not_of(ws);
                    if (start != 0 || end + 1 != r.size())
                        r = r.substr(start, end - start + 1);

                    std::transform(r.begin(), r.end(), r.begin(),
                        [](unsigned char c) { return (char)std::toupper(c); });
                }

                // Remove empties
                out.erase(std::remove_if(out.begin(), out.end(),
                    [](const std::string& s) { return s.empty(); }), out.end());

                return out;
                };

            // Prefer side-specific keys if present for the given list kind
            if ((kind == LOAListKind::Departure || kind == LOAListKind::DepartureFallback) && item.contains("depRunways")) {
                loa.runways = readRunways(item["depRunways"]);
            }
            else if ((kind == LOAListKind::Destination || kind == LOAListKind::DestinationFallback) && item.contains("arrRunways")) {
                loa.runways = readRunways(item["arrRunways"]);
            }
            else if (item.contains("runways")) {
                loa.runways = readRunways(item["runways"]);
            }

            if (item.contains("waypoints"))
                loa.waypoints = item["waypoints"].get<std::vector<std::string>>();
            for (auto& __w : loa.waypoints) {
                std::transform(__w.begin(), __w.end(), __w.begin(), ::tolower);
            }

            // --- NOT VIA waypoints (optional) ---
            // If any of these waypoints are present in the route, this LOA will NOT match.
            // Supported keys:
            //  - "notViaWaypoints": ["ELSOB","OSTOR"]
            //  - "notVia": ["ELSOB","OSTOR"] (alias)
            if (item.contains("notViaWaypoints"))
                loa.notViaWaypoints = item["notViaWaypoints"].get<std::vector<std::string>>();
            else if (item.contains("notVia"))
                loa.notViaWaypoints = item["notVia"].get<std::vector<std::string>>();
            for (auto& __w : loa.notViaWaypoints) {
                std::transform(__w.begin(), __w.end(), __w.begin(), ::tolower);
            }

            // --- Custom volume prediction constraints (volumes.json) ---
            if (item.contains("predictedEnterVolumes"))
                loa.predictedEnterVolumes = item["predictedEnterVolumes"].get<std::vector<std::string>>();
            if (item.contains("predictedFromVolumes"))
                loa.predictedFromVolumes = item["predictedFromVolumes"].get<std::vector<std::string>>();
            if (item.contains("predictedToVolumes"))
                loa.predictedToVolumes = item["predictedToVolumes"].get<std::vector<std::string>>();
            if (item.contains("predictedEndVolumes"))
                loa.predictedEndVolumes = item["predictedEndVolumes"].get<std::vector<std::string>>();
            else if (item.contains("predictedEndsInVolumes"))
                loa.predictedEndVolumes = item["predictedEndsInVolumes"].get<std::vector<std::string>>();
            else if (item.contains("predictedEndVolume"))
                loa.predictedEndVolumes = item["predictedEndVolume"].get<std::vector<std::string>>();

            if (item.contains("nextSectors"))
                loa.nextSectors = item["nextSectors"].get<std::vector<std::string>>();
            if (item.contains("copText"))
                loa.copText = item["copText"].get<std::string>();
            // --- XFL parsing (numeric FL) + optional xflText (string) ---
            // Supports:
            //  - "xfl": 250
            //  - "xfl": "250"   (string numeric)
            //  - "xfltext"/"xflText": "23R" / "230-"  (text only)
            //  - legacy: "xfl": "23R"  (will be treated as xflText to avoid breaking load)
            if (item.contains("xfltext"))
                loa.xflText = item["xfltext"].get<std::string>();
            if (item.contains("xflText"))
                loa.xflText = item["xflText"].get<std::string>();

            if (item.contains("xfl")) {
                try {
                    if (item["xfl"].is_number_integer()) {
                        loa.xfl = item["xfl"].get<int>();
                    }
                    else if (item["xfl"].is_string()) {
                        const std::string xs = item["xfl"].get<std::string>();
                        // If it's purely numeric, treat as FL; otherwise treat as text (legacy-safe)
                        bool allDigits = !xs.empty() && std::all_of(xs.begin(), xs.end(),
                            [](unsigned char c) { return std::isdigit(c) != 0; });
                        if (allDigits) {
                            loa.xfl = std::atoi(xs.c_str());
                        }
                        else if (loa.xflText.empty()) {
                            loa.xflText = xs;
                        }
                    }
                }
                catch (...) {
                    // Never abort loading for a bad XFL value; just keep defaults.
                }
            }
            if (item.contains("minAltitudeFt"))
                loa.minAltitudeFt = item["minAltitudeFt"].get<int>();

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
            auto list = parseLOAList(sectorConfig["destinationLoas"], sector, LOAListKind::Destination);
            destinationLoas.insert(destinationLoas.end(), list.begin(), list.end());
        }
        if (sectorConfig.contains("departureLoas")) {
            auto list = parseLOAList(sectorConfig["departureLoas"], sector, LOAListKind::Departure);
            departureLoas.insert(departureLoas.end(), list.begin(), list.end());
        }
        if (sectorConfig.contains("destinationFallbackLoas")) {
            auto list = parseLOAList(sectorConfig["destinationFallbackLoas"], sector, LOAListKind::DestinationFallback);
            destinationFallbackLoas.insert(destinationFallbackLoas.end(), list.begin(), list.end());
        }
        if (sectorConfig.contains("departureFallbackLoas")) {
            auto list = parseLOAList(sectorConfig["departureFallbackLoas"], sector, LOAListKind::DepartureFallback);
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

// ---------------- Active Airports/Runways (sector file) ----------------


void LOAPlugin::OnAirportRunwayActivityChanged(void)
{
    // Some EuroScope builds fire this reliably, some don't. If it fires, refresh immediately.
    UpdateActiveRunwaysFromSectorFile();

    // Force next poll to accept the new state (and invalidate caches now)
    activeRunwayFingerprint = 0ULL;
    lastRunwayPollMs = 0;
    InvalidateLoaCachesForRunwayChange();
}


static inline std::string _TrimWS(std::string s)
{
    // Fast trim without repeated erase(begin()) O(n^2)
    const char* ws = " \t\r\n";
    const size_t start = s.find_first_not_of(ws);
    if (start == std::string::npos) return std::string();
    const size_t end = s.find_last_not_of(ws);
    if (start == 0 && end + 1 == s.size()) return s;
    return s.substr(start, end - start + 1);
}

static inline std::string _ToUpper(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::toupper(c); });
    return s;
}

void LOAPlugin::UpdateActiveRunwaysFromSectorFile()
{
    activeDepRunwaysByAirport.clear();
    activeArrRunwaysByAirport.clear();

    for (EuroScopePlugIn::CSectorElement sfe = SectorFileElementSelectFirst(EuroScopePlugIn::SECTOR_ELEMENT_RUNWAY);
        sfe.IsValid();
        sfe = SectorFileElementSelectNext(sfe, EuroScopePlugIn::SECTOR_ELEMENT_RUNWAY))
    {
        std::string apt = _ToUpper(_TrimWS(std::string(sfe.GetAirportName())));
        if (apt.empty()) continue;

        for (int endIdx = 0; endIdx < 2; ++endIdx) {
            std::string rw = _ToUpper(_TrimWS(std::string(sfe.GetRunwayName(endIdx))));
            if (rw.empty()) continue;

            if (sfe.IsElementActive(true, endIdx)) {
                activeDepRunwaysByAirport[apt].insert(rw);
            }
            if (sfe.IsElementActive(false, endIdx)) {
                activeArrRunwaysByAirport[apt].insert(rw);
            }
        }
    }

    lastActiveRunwayRefreshMs = GetTickCount64();
}

void LOAPlugin::InvalidateLoaCachesForRunwayChange()
{
    ++sectorControlVersion;

    currentFrameMatchedEntry = nullptr;
    currentFrameCallsign.clear();

    matchedLOACache.clear();
    matchTimestamps.clear();
    matchVersions.clear();
    renderCache.clear();
}

void LOAPlugin::PollActiveRunwaysIfNeeded()
{
    // Throttle: every 5 seconds (matches your LOA match cadence)
    const ULONGLONG nowMs = GetTickCount64();
    if (nowMs - lastRunwayPollMs < 5000ULL)
        return;
    lastRunwayPollMs = nowMs;

    // Scan sector runway elements and build a fingerprint of active runway selections (DEP/ARR).
    std::unordered_map<std::string, std::unordered_set<std::string>> tmpDep;
    std::unordered_map<std::string, std::unordered_set<std::string>> tmpArr;

    unsigned long long h = 1469598103934665603ULL; // FNV offset basis (64-bit)
    auto hashMix = [&](const std::string& s)
        {
            for (size_t i = 0; i < s.size(); ++i) {
                h ^= (unsigned char)s[i];
                h *= 1099511628211ULL; // FNV prime
            }
        };

    auto hashMixChar = [&](char c)
        {
            h ^= (unsigned char)c;
            h *= 1099511628211ULL;
        };


    for (EuroScopePlugIn::CSectorElement sfe = SectorFileElementSelectFirst(EuroScopePlugIn::SECTOR_ELEMENT_RUNWAY);
        sfe.IsValid();
        sfe = SectorFileElementSelectNext(sfe, EuroScopePlugIn::SECTOR_ELEMENT_RUNWAY))
    {
        std::string apt = _ToUpper(_TrimWS(std::string(sfe.GetAirportName())));
        if (apt.empty()) continue;

        for (int endIdx = 0; endIdx < 2; ++endIdx)
        {
            std::string rw = _ToUpper(_TrimWS(std::string(sfe.GetRunwayName(endIdx))));
            if (rw.empty()) continue;

            if (sfe.IsElementActive(true, endIdx)) {
                tmpDep[apt].insert(rw);
                hashMixChar('D'); hashMixChar(':'); hashMix(apt); hashMixChar(':'); hashMix(rw);
            }
            if (sfe.IsElementActive(false, endIdx)) {
                tmpArr[apt].insert(rw);
                hashMixChar('A'); hashMixChar(':'); hashMix(apt); hashMixChar(':'); hashMix(rw);
            }
        }
    }

    if (h == activeRunwayFingerprint)
        return;

    // Runway selection changed -> swap in new caches and invalidate LOA/tag caches.
    activeRunwayFingerprint = h;
    activeDepRunwaysByAirport.swap(tmpDep);
    activeArrRunwaysByAirport.swap(tmpArr);
    lastActiveRunwayRefreshMs = nowMs;

    InvalidateLoaCachesForRunwayChange();
}


bool LOAPlugin::MatchesActiveRunway(
    const std::string& airportIcao,
    bool isDeparture,
    const std::vector<std::string>& allowedRunways) const
{
    if (allowedRunways.empty()) return true;

    std::string apt = _ToUpper(_TrimWS(airportIcao));
    if (apt.empty()) return false;

    const auto& mapRef = isDeparture ? activeDepRunwaysByAirport : activeArrRunwaysByAirport;
    auto it = mapRef.find(apt);
    if (it == mapRef.end() || it->second.empty()) return false; // nothing selected in ES dialog

    for (const auto& r : allowedRunways) {
        // runways are normalized at JSON load (trimmed + uppercased)
        if (!r.empty() && it->second.find(r) != it->second.end()) return true;
    }
    return false;
}

// -----------------------------------------------------------------------
std::string LOAPlugin::ResolveControllingSector(const std::string& sector, const std::unordered_set<std::string>& onlineControllers)
{
    // Cache result per onlineControllersVersion (refreshed in GetOnlineControllersCached()).
    // Assumption: callers pass a snapshot derived from GetOnlineControllersCached()/currentFrameOnlineControllers.
    if (controllingSectorCacheVersion == onlineControllersVersion) {
        auto it = controllingSectorCache.find(sector);
        if (it != controllingSectorCache.end()) return it->second;
    }

    auto prIt = sectorPriority.find(sector);
    if (prIt != sectorPriority.end()) {
        const auto& prioList = prIt->second;
        for (const auto& s : prioList) {
            if (onlineControllers.count(s)) {
                if (controllingSectorCacheVersion == onlineControllersVersion)
                    controllingSectorCache[sector] = s;
                return s;
            }
        }
    }

    if (controllingSectorCacheVersion == onlineControllersVersion)
        controllingSectorCache[sector] = std::string();
    return {}; // No one online
}

bool LOAPlugin::ShouldAllowNextSectors(
    const std::vector<std::string>& nextSectors,
    const std::unordered_set<std::string>& onlineControllers)
{
    std::string mySector = ControllerMyself().GetPositionId();

    std::vector<std::string> owned;
    auto ownIt = sectorOwnership.find(mySector);
    if (ownIt != sectorOwnership.end()) owned = ownIt->second;

    for (const std::string& next : nextSectors) {
        const bool nextIsDefined = (sectorOwnership.find(next) != sectorOwnership.end());
        const bool iOwnNext = (std::find_if(owned.begin(), owned.end(),
            [&](const std::string& s) { return _stricmp(s.c_str(), next.c_str()) == 0; }) != owned.end());

        std::string actualController = ResolveControllingSector(next, onlineControllers);

        if (!actualController.empty() && _stricmp(actualController.c_str(), mySector.c_str()) == 0)
            return false;

        if (nextIsDefined) {
            if (iOwnNext && actualController.empty())
                return false;

            auto prIt = sectorPriority.find(next);
            if (prIt != sectorPriority.end()) {
                const auto& prioList = prIt->second;
                auto myPrio = std::find_if(prioList.begin(), prioList.end(),
                    [&](const std::string& s) { return _stricmp(s.c_str(), mySector.c_str()) == 0; });
                auto otherPrio = std::find_if(prioList.begin(), prioList.end(),
                    [&](const std::string& s) { return _stricmp(s.c_str(), actualController.c_str()) == 0; });

                // Owned-sector rule: allow only when other controller outranks me
                if (iOwnNext) {
                    if (myPrio != prioList.end() && otherPrio != prioList.end() && otherPrio < myPrio)
                        return true;
                    return false;
                }

                // Non-owned defined sector: suppress if I outrank the controlling sector
                if (myPrio != prioList.end() && otherPrio != prioList.end() && myPrio < otherPrio)
                    return false;
            }

            return true;
        }

        // External sector
        if (actualController.empty()) return true;
        if (!_stricmp(actualController.c_str(), mySector.c_str())) return false;
        return true;
    }

    return false;
}

bool LOAPlugin::IsSourceSectorSuppressed(
    const LOAEntry& e,
    const std::unordered_set<std::string>& onlineControllers)
{
    if (e.sectors.empty()) return false;

    std::string my = ControllerMyself().GetPositionId();
    std::unordered_map<std::string, std::string> resolveCache;

    for (const auto& src : e.sectors) {
        std::string actual;
        auto it = resolveCache.find(src);
        if (it != resolveCache.end()) actual = it->second;
        else {
            actual = ResolveControllingSector(src, onlineControllers);
            resolveCache[src] = actual;
        }

        if (actual.empty()) continue;
        if (_stricmp(actual.c_str(), my.c_str()) == 0) continue;

        auto prIt = sectorPriority.find(src);
        if (prIt == sectorPriority.end()) continue;

        const auto& prio = prIt->second;
        auto meIt = std::find_if(prio.begin(), prio.end(),
            [&](const std::string& s) { return _stricmp(s.c_str(), my.c_str()) == 0; });
        auto himIt = std::find_if(prio.begin(), prio.end(),
            [&](const std::string& s) { return _stricmp(s.c_str(), actual.c_str()) == 0; });

        if (meIt != prio.end() && himIt != prio.end() && himIt < meIt)
            return true;
    }

    return false;
}


bool LOAPlugin::IsLOARelevantState(int state) {
    // Hot-path: use switch instead of unordered_set lookup
    switch (state) {
    case FLIGHT_PLAN_STATE_NOTIFIED:
    case FLIGHT_PLAN_STATE_COORDINATED:
    case FLIGHT_PLAN_STATE_TRANSFER_TO_ME_INITIATED:
    case FLIGHT_PLAN_STATE_TRANSFER_FROM_ME_INITIATED:
    case FLIGHT_PLAN_STATE_ASSUMED:
    case FLIGHT_PLAN_STATE_REDUNDANT:
        return true;
    default:
        return false;
    }
}

const std::unordered_set<std::string>& LOAPlugin::GetOnlineControllersCached()
{
    ULONGLONG currentTime = GetTickCount64();
    if (currentTime - lastOnlineFetchTime > 5000 || cachedOnlineControllers.empty()) {
        cachedOnlineControllers.clear();
        onlineControllersVersion++;
        controllingSectorCache.clear();
        controllingSectorCacheVersion = onlineControllersVersion;
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

std::string LOAPlugin::GetPredictedNextController(const EuroScopePlugIn::CFlightPlan& fp)
{
    EuroScopePlugIn::CFlightPlanPositionPredictions preds = fp.GetPositionPredictions();
    int n = preds.GetPointsNumber();
    if (n <= 0) return {};

    std::string myId = ControllerMyself().GetPositionId();

    std::string last;

    for (int i = 0; i < n; ++i) {
        const char* id = preds.GetControllerId(i);
        if (!id || !id[0]) continue;

        // Skip repeats
        if (!last.empty() && _stricmp(id, last.c_str()) == 0)
            continue;

        // Skip my own sector
        if (!myId.empty() && _stricmp(id, myId.c_str()) == 0) {
            last = id;
            continue;
        }

        // First non-own, non-repeat controller → this is our predicted next
        return std::string(id);
    }

    // No suitable next controller found
    return {};
}

std::vector<std::string> LOAPlugin::BuildHybridPredictedSectorList(
    const EuroScopePlugIn::CFlightPlan& fp,
    const std::unordered_set<std::string>& onlineControllers,
    size_t* outLoaCount)
{
    std::vector<std::string> result;
    std::unordered_set<std::string> seen;

    size_t loaAdded = 0;
    if (outLoaCount) *outLoaCount = 0;

    std::string myId = ControllerMyself().GetPositionId();

    const LOAEntry* match = MatchLoaEntry(fp, onlineControllers);

    // 1) LOA-based next sectors first (match already includes altitude gating)
    if (match && !match->nextSectors.empty()) {
        for (const std::string& next : match->nextSectors) {
            if (next.empty()) continue;

            std::string controlling = ResolveControllingSector(next, onlineControllers);
            const std::string& key = controlling.empty() ? next : controlling;

            // Skip myself; skip duplicates
            if (!myId.empty() && _stricmp(key.c_str(), myId.c_str()) == 0)
                continue;
            if (!seen.insert(key).second)
                continue;

            result.push_back(key);
            ++loaAdded;
        }
    }

    if (outLoaCount)
        *outLoaCount = loaAdded;

    // 2) ES controller prediction (unchanged) — this always runs,
    // but anything already seeded by LOA is skipped via "seen".
    EuroScopePlugIn::CFlightPlanPositionPredictions preds = fp.GetPositionPredictions();
    int n = preds.GetPointsNumber();
    if (n > 0) {
        std::string last;

        for (int i = 0; i < n; ++i) {
            const char* id = preds.GetControllerId(i);
            if (!id || !id[0]) continue;

            // Skip consecutive repeats
            if (!last.empty() && _stricmp(id, last.c_str()) == 0)
                continue;

            last = id;

            // Skip my own sector
            if (!myId.empty() && _stricmp(id, myId.c_str()) == 0)
                continue;

            // Skip anything already seeded by LOA
            if (!seen.insert(id).second)
                continue;

            result.emplace_back(id);
        }
    }

    return result;
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
    // New semantics (2025-11): this returns true if *I* am currently the
    // controlling station for at least one sector that defines AOR destinations.
    const std::string myId = ControllerMyself().GetPositionId();
    if (myId.empty()) return false;

    for (const auto& host : aorHostSectors) {
        // Which station controls this AOR sector right now?
        std::string ctrl = const_cast<LOAPlugin*>(this)->ResolveControllingSector(host, onlineControllers);
        if (!ctrl.empty() && _stricmp(ctrl.c_str(), myId.c_str()) == 0) {
            // I currently own this AOR sector (directly or via ownership tree)
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
    if (routeCache.count(callsign) && now - routeCacheTime[callsign] < 5000) {
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
        std::string cs = fp.GetCallsign();

        CleanupCache(cs);
        activeHandoffTargets.erase(cs);

        // Also clear rendered Next Sector tag so new color/text appears immediately
        std::string key = cs + ":" + std::to_string(ItemCodes::TAG_ITEM_NEXT_SECTOR_CTRL);
        renderCache.erase(key);
    }
}


void LOAPlugin::OnFlightPlanCoordinationStateChange(CFlightPlan fp, int coordinationType, int newState)
{
    if (!fp.IsValid()) return;

    const std::string callsign = fp.GetCallsign();
    CoordinationInfo& info = coordinationStates[callsign];

    if (coordinationType == EuroScopePlugIn::TAG_ITEM_TYPE_COPN_COPX_NAME) {
        const std::string raw = fp.GetExitCoordinationPointName();

        if (newState == COORDINATION_STATE_REQUESTED_BY_ME || newState == COORDINATION_STATE_REQUESTED_BY_OTHER) {
            if (!info.pointRequestActive) {
                info.baselineExitPoint = raw;
            }
            info.pendingExitPoint = raw;
            info.pointRequestActive = true;
            return;
        }

        if (newState == COORDINATION_STATE_NONE) {
            if (info.pointRequestActive) {
                if (!raw.empty() && !info.pendingExitPoint.empty() && _stricmp(raw.c_str(), info.pendingExitPoint.c_str()) == 0) {
                    info.acceptedExitPoint = raw;
                }
                else {
                    info.acceptedExitPoint.clear();
                }
                info.pointRequestActive = false;
            }
            if (!info.pointRequestActive && info.baselineExitPoint.empty()) {
                info.baselineExitPoint = raw;
            }
            return;
        }

        if (newState == COORDINATION_STATE_ACCEPTED || newState == COORDINATION_STATE_MANUAL_ACCEPTED) {
            if (!raw.empty()) info.acceptedExitPoint = raw;
            return;
        }

        if (newState == COORDINATION_STATE_REFUSED) {
            info.acceptedExitPoint.clear();
            info.pointRequestActive = false;
            return;
        }
        return;
    }

    if (coordinationType == EuroScopePlugIn::TAG_ITEM_TYPE_COPN_COPX_ALTITUDE) {
        const int rawAlt = fp.GetExitCoordinationAltitude();

        if (newState == COORDINATION_STATE_REQUESTED_BY_ME || newState == COORDINATION_STATE_REQUESTED_BY_OTHER) {
            if (!info.altitudeRequestActive) {
                info.baselineExitAltitude = rawAlt;
            }
            info.pendingExitAltitude = rawAlt;
            info.altitudeRequestActive = true;
            return;
        }

        if (newState == COORDINATION_STATE_NONE) {
            if (info.altitudeRequestActive) {
                if (rawAlt > 0 && rawAlt == info.pendingExitAltitude) {
                    info.acceptedExitAltitude = rawAlt;
                }
                else {
                    info.acceptedExitAltitude = 0;
                }
                info.altitudeRequestActive = false;
            }
            if (!info.altitudeRequestActive && info.baselineExitAltitude == 0) {
                info.baselineExitAltitude = rawAlt;
            }
            return;
        }

        if (newState == COORDINATION_STATE_ACCEPTED || newState == COORDINATION_STATE_MANUAL_ACCEPTED) {
            if (rawAlt > 0) info.acceptedExitAltitude = rawAlt;
            return;
        }

        if (newState == COORDINATION_STATE_REFUSED) {
            info.acceptedExitAltitude = 0;
            info.altitudeRequestActive = false;
            return;
        }
        return;
    }
}

void LOAPlugin::CheckForOwnershipChange() {
    std::string mySector = ControllerMyself().GetPositionId();
    if (mySector.empty()) return;

    auto oldControllers = currentFrameOnlineControllers;
    currentFrameOnlineControllers = GetOnlineControllersCached();

    bool changed = false;
    std::vector<std::string> checkSectors;
    auto ownIt = sectorOwnership.find(mySector);
    if (ownIt != sectorOwnership.end()) checkSectors = ownIt->second;
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

void LOAPlugin::OnFunctionCall(
    int FunctionId,
    const char* sItemString,
    POINT Pt,
    RECT Area)
{
    try {
        // -----------------------------------------------------------------
        // 1) Click on the tag -> open the LOA + route next-sector popup
        // -----------------------------------------------------------------
        if (FunctionId == FunctionIds::NEXT_SECTOR_HANDOFF_MENU) {

            // Use ASEL as the clicked aircraft
            EuroScopePlugIn::CFlightPlan fp = FlightPlanSelectASEL();
            if (!fp.IsValid()) return;  // still need a valid FP object to ASSUME/RELEASE

            int state = fp.GetState();

            const auto& online = GetOnlineControllersCached();

            struct Option {
                std::string sectorId;  // controlling / predicted sector ID (e.g. "ALR", "HAM", "EDWW_CTR")
                std::string freqStr;   // "123.450"
                bool isLoa = false;    // true = LOA seeded, false = EuroScope predicted
            };

            std::vector<Option> options;  // unified list (LOA + ES hybrid)

            // ----------------- 0) ASSUME / RELEASE flags -----------------

            // Who is currently tracking this flight?
            const char* trackingId = fp.GetTrackingControllerId();
            std::string myPosId = ControllerMyself().GetPositionId();

            // RELEASE if I am the one tracking the flight
            bool canShowRelease =
                trackingId && trackingId[0] &&
                _stricmp(trackingId, myPosId.c_str()) == 0;

            // ASSUME in these "not yet mine" states
            bool canShowAssume =
                state == EuroScopePlugIn::FLIGHT_PLAN_STATE_NON_CONCERNED ||
                state == EuroScopePlugIn::FLIGHT_PLAN_STATE_NOTIFIED ||
                state == EuroScopePlugIn::FLIGHT_PLAN_STATE_COORDINATED ||
                state == EuroScopePlugIn::FLIGHT_PLAN_STATE_TRANSFER_TO_ME_INITIATED;

            // LOA/next-sector is only relevant in your LOA states
            bool loasRelevant = IsLOARelevantState(state);

            // If the state is not LOA-relevant AND we cannot ASSUME or RELEASE,
            // then there is nothing useful the menu can do.
            if (!loasRelevant && !canShowAssume && !canShowRelease)
                return;

            // ----------------- 1) HYBRID next-sector list -----------------
            // LOA next sectors first, then ES predicted controllers (altitude-aware),
            // skipping myself and duplicates.
            if (loasRelevant) {
                size_t loaCount = 0;
                std::vector<std::string> hybridList = BuildHybridPredictedSectorList(fp, online, &loaCount);

                for (size_t i = 0; i < hybridList.size(); ++i) {
                    const auto& cid = hybridList[i];
                    const bool isLoaSeeded = (i < loaCount);
                    // Only show sectors that are actually online
                    if (online.count(cid) == 0)
                        continue;

                    // Find the controller object for this position ID and read its frequency
                    for (EuroScopePlugIn::CController c = ControllerSelectFirst();
                        c.IsValid();
                        c = ControllerSelectNext(c))
                    {
                        if (!c.IsController()) continue;
                        if (_stricmp(c.GetPositionId(), cid.c_str()) != 0)
                            continue;

                        double freq = c.GetPrimaryFrequency();
                        char buf[16] = {};
                        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%.3f", freq);

                        // Keep it compact: frequency string has no leading padding.
                        std::string freqStr = buf;

                        options.push_back({ cid, freqStr, isLoaSeeded });
                        break;
                    }
                }
            }

            // ----------------- 2) Nothing at all? -----------------
            // If we have neither sectors nor ASSUME nor RELEASE -> no menu
            if (options.empty() && !canShowAssume && !canShowRelease)
            {
                return;
            }

            char title[64] = {};
            _snprintf_s(title, sizeof(title), _TRUNCATE, "%s - Handoff", fp.GetCallsign());

            // 1-column popup: we draw "ID FREQ" ourselves per line
            OpenPopupList(Area, title, 1);

            // ----------------- 3) Prepare lines (fixed minimum width) -----------------
            std::vector<std::string> optionLines;
            optionLines.reserve(options.size());

            size_t maxWidth = 0;
            for (const auto& opt : options) {
                std::string line;
                line.reserve(opt.sectorId.size() + 2 + opt.freqStr.size());
                line += (opt.isLoa ? "*" : "-");
                line += opt.sectorId;
                if (!opt.freqStr.empty()) {
                    line += " ";
                    line += opt.freqStr;
                }
                optionLines.push_back(line);
                if (line.size() > maxWidth) maxWidth = line.size();
            }

            // Force a constant popup width even if current entries are shorter.
            // Longest possible line is e.g. "ZZZZ 118.205" (+ our LOA/ES marker prefix).
            // This keeps the handoff list width stable across different next-sector sets.
            const size_t kMinPopupWidth = std::string("*ZZZZ 118.205").size();
            if (maxWidth < kMinPopupWidth)
                maxWidth = kMinPopupWidth;

            // Account for action labels too
            if (std::string("ASSUME").size() > maxWidth)
                maxWidth = std::string("ASSUME").size();

            if (std::string("FREE").size() > maxWidth)
                maxWidth = std::string("FREE").size();

            auto padRight = [&](const std::string& text) -> std::string {
                if (text.size() >= maxWidth) return text;
                return text + std::string(maxWidth - text.size(), ' ');
                };

            auto centerText = [&](const std::string& text) -> std::string {
                if (text.size() >= maxWidth) return text;
                const size_t leftPad = (maxWidth - text.size()) / 2;
                const size_t rightPad = (maxWidth - text.size()) - leftPad;
                return std::string(leftPad, ' ') + text + std::string(rightPad, ' ');
                };

            const std::string separator(maxWidth, '-');

            // ----------------- 4) ASSUME (TOP) -----------------
            if (canShowAssume) {
                const std::string assumeLine = AddArrowPrefix(centerText("ASSUME"));
                AddPopupListElement(
                    assumeLine.c_str(),
                    "",
                    FunctionIds::NEXT_SECTOR_ASSUME_EXEC,
                    false,
                    POPUP_ELEMENT_NO_CHECKBOX,
                    false,
                    false);

                // Visual divider under Assume
                AddPopupListElement(
                    separator.c_str(),
                    "",
                    0,
                    false,
                    POPUP_ELEMENT_NO_CHECKBOX,
                    true,
                    false);
            }

            // ----------------- 5) Hybrid next-sector options -------------
            for (const auto& lineRaw : optionLines) {
                const std::string line = AddArrowPrefix(padRight(lineRaw));
                AddPopupListElement(
                    line.c_str(),
                    "",
                    FunctionIds::NEXT_SECTOR_HANDOFF_EXEC,
                    false,
                    POPUP_ELEMENT_NO_CHECKBOX,
                    false,
                    false);
            }

            // ----------------- 6) RELEASE (BOTTOM) ----------------
            if (canShowRelease) {
                // Divider above Release (only if we had anything above it)
                if (canShowAssume || !optionLines.empty()) {
                    AddPopupListElement(
                        separator.c_str(),
                        "",
                        0,
                        false,
                        POPUP_ELEMENT_NO_CHECKBOX,
                        true,
                        false);
                }

                const std::string releaseLine = AddArrowPrefix(centerText("FREE"));
                AddPopupListElement(
                    releaseLine.c_str(),
                    "",
                    FunctionIds::NEXT_SECTOR_RELEASE_EXEC,
                    false,
                    POPUP_ELEMENT_NO_CHECKBOX,
                    false,
                    false);
            }

            return;
        }

        // ============================================================
        // 2) ASSUME clicked in the popup
        // ============================================================
        if (FunctionId == FunctionIds::NEXT_SECTOR_ASSUME_EXEC) {

            EuroScopePlugIn::CFlightPlan fp = FlightPlanSelectASEL();
            if (!fp.IsValid())
                return;

            int state = fp.GetState();

            // If there is a handoff in progress to me, accept it;
            // otherwise start tracking the flight.
            if (state == EuroScopePlugIn::FLIGHT_PLAN_STATE_TRANSFER_TO_ME_INITIATED) {
                fp.AcceptHandoff();
            }
            else {
                fp.StartTracking();
            }

            return;
        }

        // ============================================================
        // 3) RELEASE clicked in the popup
        // ============================================================
        if (FunctionId == FunctionIds::NEXT_SECTOR_RELEASE_EXEC) {

            EuroScopePlugIn::CFlightPlan fp = FlightPlanSelectASEL();
            if (!fp.IsValid())
                return;

            const char* trackingId = fp.GetTrackingControllerId();
            std::string myPosId = ControllerMyself().GetPositionId();

            // Only release if I am the one currently tracking
            if (trackingId && trackingId[0] &&
                _stricmp(trackingId, myPosId.c_str()) == 0)
            {
                // EndTracking() = "release (drop) the target" per API docs
                fp.EndTracking();
            }

            return;
        }

        // ============================================================
        // 4) Sector line clicked -> initiate handoff
        // ============================================================
        if (FunctionId == FunctionIds::NEXT_SECTOR_HANDOFF_EXEC) {

            if (!sItemString || !*sItemString)
                return;

            std::string line = sItemString;

            // Remove leading "> " prefix if present
            if (line.rfind("> ", 0) == 0) {
                line = line.substr(2);
            }

            // Trim leading whitespace
            while (!line.empty() && (line[0] == ' ' || line[0] == '\t' || line[0] == '\r' || line[0] == '\n'))
                line.erase(line.begin());

            // Strip LOA/ES marker prefix if present ('*' for LOA, '-' for predicted)
            if (!line.empty() && (line[0] == '*' || line[0] == '-')) {
                line.erase(line.begin());
                while (!line.empty() && (line[0] == ' ' || line[0] == '\t'))
                    line.erase(line.begin());
            }

            // Now parse sector ID
            size_t pos = line.find_first_of(" \t\r\n");
            std::string controlling = (pos == std::string::npos)
                ? line
                : line.substr(0, pos);

            EuroScopePlugIn::CFlightPlan fp = FlightPlanSelectASEL();
            if (!fp.IsValid())
                return;

            for (EuroScopePlugIn::CController c = ControllerSelectFirst();
                c.IsValid();
                c = ControllerSelectNext(c))
            {
                if (!c.IsController()) continue;

                if (_stricmp(c.GetPositionId(), controlling.c_str()) != 0)
                    continue;

                fp.InitiateHandoff(c.GetCallsign());

                // ✅ Remember handoff target for this callsign so the Next Sector tag
                // can show it while TRANSFER_FROM_ME_INITIATED.
                std::string cs = fp.GetCallsign();
                activeHandoffTargets[cs] = controlling;

                // Bust the micro-cache for the Next Sector tag so it updates immediately
                std::string key = cs + ":" + std::to_string(ItemCodes::TAG_ITEM_NEXT_SECTOR_CTRL);
                renderCache.erase(key);

                break;
            }

            return;
        }
    }
    catch (...) {
        // Don't crash EuroScope if something goes wrong
    }
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
    const std::string coordPt = flightPlan.GetExitCoordinationPointName();
    const int coordPtSt = flightPlan.GetExitCoordinationNameState();
    const int coordAlt = flightPlan.GetExitCoordinationAltitude();
    const int coordAltSt = flightPlan.GetExitCoordinationAltitudeState();

    // Hot-path: avoid std::to_string temporary allocations
    std::string renderKey;
    renderKey.reserve(callsign.size() + 8);
    renderKey.append(callsign);
    renderKey.push_back(':');
    char itemBuf[16] = { 0 };
    _itoa_s(itemCode, itemBuf, 10);
    renderKey.append(itemBuf);
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
            // 🚫 Do NOT touch pFontSize here – let EuroScope keep its own font.
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

        // Detect FP edits (origin/dest/route) and invalidate per-callsign caches immediately.
        {
            static std::unordered_map<std::string, std::string> lastDestinationByCallsign;
            auto itDest = lastDestinationByCallsign.find(callsign);
            const bool destinationChanged = (itDest != lastDestinationByCallsign.end() &&
                _stricmp(itDest->second.c_str(), destination.c_str()) != 0);
            lastDestinationByCallsign[callsign] = destination;

            // Incremental FNV-1a hash (avoid building a large concatenated string)
            unsigned long long h = 1469598103934665603ULL;
            auto fnv_feed = [&](const std::string& s) {
                for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
                };
            fnv_feed(origin);
            fnv_feed("|");
            fnv_feed(destination);
            fnv_feed("|");
            for (const auto& rp : plugin.currentFrameRoutePoints) {
                fnv_feed(rp);
                fnv_feed(",");
            }

            auto itSig = plugin.routeSignature.find(callsign);
            if (itSig == plugin.routeSignature.end() || itSig->second != h) {
                plugin.routeSignature[callsign] = h;
                plugin.CleanupCache(callsign); // force recompute match & route caches
                if (destinationChanged) {
                    plugin.coordinationStates.erase(callsign);
                    plugin.activeHandoffTargets.erase(callsign);
                }
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

    case 2001:
    {
        sItemString[0] = '\0';
        if (pColorCode) *pColorCode = TAG_COLOR_DEFAULT;

        int state = flightPlan.GetState();
        const std::string cs = flightPlan.GetCallsign();

        // 0A) While a handoff FROM ME is in progress, pin the chosen target sector
        if (state == EuroScopePlugIn::FLIGHT_PLAN_STATE_TRANSFER_FROM_ME_INITIATED) {

            // Always color this state REDUNDANT
            if (pColorCode) {
                *pColorCode = TAG_COLOR_REDUNDANT;
            }

            auto itH = activeHandoffTargets.find(cs);
            if (itH != activeHandoffTargets.end()) {
                strncpy_s(sItemString, 16, itH->second.c_str(), _TRUNCATE);
                break;  // Don't process LOA logic
            }

            // If no remembered handoff sector exists, still apply color
            // and continue to fallback logic
        }

        // States where we just show "whoever assumed it"
        if (state == EuroScopePlugIn::FLIGHT_PLAN_STATE_NON_CONCERNED ||
            state == EuroScopePlugIn::FLIGHT_PLAN_STATE_NOTIFIED ||
            state == EuroScopePlugIn::FLIGHT_PLAN_STATE_COORDINATED ||
            state == EuroScopePlugIn::FLIGHT_PLAN_STATE_TRANSFER_TO_ME_INITIATED ||
            state == EuroScopePlugIn::FLIGHT_PLAN_STATE_REDUNDANT)
        {
            const char* trackingId = flightPlan.GetTrackingControllerId();
            if (trackingId && trackingId[0]) {
                strncpy_s(sItemString, 16, trackingId, _TRUNCATE);
            }
            else {
                strncpy_s(sItemString, 16, "SI", _TRUNCATE);
            }
            // No color override
            break;
        }

        if (state == EuroScopePlugIn::FLIGHT_PLAN_STATE_TRANSFER_FROM_ME_INITIATED) {
            if (pColorCode) {
                *pColorCode = TAG_COLOR_REDUNDANT;
            }
        }

        bool displayed = false;

        const LOAEntry* match = plugin.currentFrameMatchedEntry;
        const bool hasLoa = (match && !match->nextSectors.empty());

        if (hasLoa) {

            const std::string& next = match->nextSectors.front();

            // Resolve who actually controls that LOA next sector via ownership/priority
            std::string station = plugin.GetIndicatedNextSectorStation(next);

            if (!station.empty()) {
                strncpy_s(sItemString, 16, station.c_str(), _TRUNCATE);
                displayed = true;
            }
            else {
                strncpy_s(sItemString, 16, "SI", _TRUNCATE);
                displayed = true;
            }
            if (displayed)
                break;
        }

        // 2) NO LOA MATCH → ES PREDICTION
        std::string predicted = plugin.GetPredictedNextController(flightPlan);

        if (!predicted.empty()) {
            const auto& online =
                !plugin.currentFrameOnlineControllers.empty()
                ? plugin.currentFrameOnlineControllers
                : plugin.GetOnlineControllersCached();

            std::string myId = plugin.ControllerMyself().GetPositionId();

            if (_stricmp(predicted.c_str(), myId.c_str()) != 0 &&
                online.count(predicted) > 0)
            {
                strncpy_s(sItemString, 16, predicted.c_str(), _TRUNCATE);
                displayed = true;
            }
            else {
                strncpy_s(sItemString, 16, "SI", _TRUNCATE);
                displayed = true;
            }
        }
        else {
            strncpy_s(sItemString, 16, "SI", _TRUNCATE);
            displayed = true;
        }

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
        // 🚫 Do NOT store / use any font size anymore.
        rc.fontSize = 0.0;
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

    const std::string callsign = fp.GetCallsign();
    const ULONGLONG now = GetTickCount64();

    // 5s cache validity
    auto itSet = routeSetCache.find(callsign);
    auto itTs = routeSetCacheTime.find(callsign);
    if (itSet != routeSetCache.end() && itTs != routeSetCacheTime.end()) {
        if (now - itTs->second < 5000) {
            return itSet->second;
        }
    }

    const auto& pts = GetCachedRoutePoints(fp);

    std::unordered_set<std::string> s;
    s.reserve(pts.size() * 2 + 4);

    for (const auto& p0 : pts) {
        std::string p = p0;
        std::transform(p.begin(), p.end(), p.begin(), ::tolower);
        s.insert(std::move(p));
    }

    routeSetCache[callsign] = std::move(s);
    routeSetCacheTime[callsign] = now;

    return routeSetCache.find(callsign)->second;
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