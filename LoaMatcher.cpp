#include "stdafx.h"
#include "windows.h"
#include "LOAPlugin.h"
#include <string>
#include <vector>
#include <cctype>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <algorithm>
#include <climits>

static bool EqualsIgnoreCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    }
    return true;
}

const LOAEntry* MatchLoaEntry(const EuroScopePlugIn::CFlightPlan& fp,
    const std::unordered_set<std::string>& /*onlineControllers*/)
{
    if (!fp.IsValid() || !plugin.IsLOARelevantState(fp.GetState())) return nullptr;

    const char* planType = fp.GetFlightPlanData().GetPlanType();
    if (_stricmp(planType, "I") != 0) return nullptr;

    const std::string callsign = fp.GetCallsign();
    DWORD now = GetTickCount64();

    // 5s cache + sectorControlVersion
    auto tsIt = plugin.matchTimestamps.find(callsign);
    auto verIt = plugin.matchVersions.find(callsign);
    if (tsIt != plugin.matchTimestamps.end() &&
        verIt != plugin.matchVersions.end() &&
        now - tsIt->second < 5000 &&
        verIt->second == plugin.sectorControlVersion)
    {
        auto matchIt = plugin.matchedLOACache.find(callsign);
        if (matchIt != plugin.matchedLOACache.end() && matchIt->second != nullptr) {
            return matchIt->second;
        }
    }

    std::string origin = fp.GetFlightPlanData().GetOrigin();
    std::string destination = fp.GetFlightPlanData().GetDestination();
    const auto& routePoints = plugin.GetCachedRoutePoints(fp);

    // Next-sector ownership/priority gate (same as before)
    auto shouldMatchLOA = [&](const std::vector<std::string>& nextSectors) -> bool {
        std::string mySector = plugin.ControllerMyself().GetPositionId();
        std::unordered_map<std::string, std::string> controllerCache;

        for (const std::string& next : nextSectors) {
            std::string actualController;
            auto cached = controllerCache.find(next);
            if (cached != controllerCache.end()) {
                actualController = cached->second;
            }
            else {
                actualController = plugin.ResolveControllingSector(next, plugin.currentFrameOnlineControllers);
                controllerCache[next] = actualController;
            }

            const auto& owned = plugin.sectorOwnership[mySector];
            bool iOwnNext = std::find(owned.begin(), owned.end(), next) != owned.end();

            if (actualController.empty()) {
                if (iOwnNext) return false;  // I own it but it's offline → skip
                else continue;               // external/offline → allow
            }

            if (_stricmp(actualController.c_str(), mySector.c_str()) == 0) {
                return false; // I'm controlling → skip
            }

            auto prioListIt = plugin.sectorPriority.find(next);
            if (prioListIt != plugin.sectorPriority.end()) {
                const auto& prioList = prioListIt->second;
                auto myPrio = std::find(prioList.begin(), prioList.end(), mySector);
                auto otherPrio = std::find(prioList.begin(), prioList.end(), actualController);
                if (otherPrio != prioList.end() && (myPrio == prioList.end() || otherPrio < myPrio)) {
                    return true;  // higher-priority neighbor online
                }
                return false;
            }
            return false;
        }
        return false;
        };

    // Classic, fast matcher (no ETA, no extracted-route minutes)
    auto matchInIndexed = [&](const std::unordered_map<std::string, std::vector<const LOAEntry*>>& index) -> const LOAEntry* {
        std::unordered_set<const LOAEntry*> uniqueCandidates;
        for (const std::string& wp : plugin.GetCachedRoutePoints(fp)) {
            auto it = index.find(wp);
            if (it != index.end()) {
                uniqueCandidates.insert(it->second.begin(), it->second.end());
            }
        }

        const LOAEntry* best = nullptr;
        int highestScore = -2;
        std::string mySector = plugin.ControllerMyself().GetPositionId();

        for (const LOAEntry* entry : uniqueCandidates) {
            // Airport filters
            if (!entry->originAirports.empty() &&
                !plugin.MatchesAirport(entry->originAirportSet, entry->originAirportPrefixes, origin)) continue;

            if (!entry->destinationAirports.empty() &&
                !plugin.MatchesAirport(entry->destinationAirportSet, entry->destinationAirportPrefixes, destination)) continue;

            // Waypoint presence: require all
            bool wpMatch = std::all_of(entry->waypoints.begin(), entry->waypoints.end(),
                [&](const std::string& wp) {
                    return std::any_of(routePoints.begin(), routePoints.end(),
                        [&](const std::string& r) { return EqualsIgnoreCase(r, wp); });
                });
            if (!wpMatch) continue;

            // Next-sector gate
            if (!entry->nextSectors.empty() && !shouldMatchLOA(entry->nextSectors)) continue;

            // Original ownership/online scoring
            int score = 0;
            for (const std::string& next : entry->nextSectors) {
                const auto& owned = plugin.sectorOwnership[mySector];
                bool iOwned = std::find(owned.begin(), owned.end(), next) != owned.end();
                std::string actualCtrl = plugin.ResolveControllingSector(next, plugin.currentFrameOnlineControllers);
                bool someoneElseOnline = !actualCtrl.empty() && _stricmp(actualCtrl.c_str(), mySector.c_str()) != 0;

                if (iOwned && someoneElseOnline) {
                    if (score < 2) score = 2;
                }
                else if (!iOwned && someoneElseOnline) {
                    if (score < 1) score = 1;
                }
            }

            if (score == 0) {
                for (const std::string& next : entry->nextSectors) {
                    std::string actualCtrl = plugin.ResolveControllingSector(next, plugin.currentFrameOnlineControllers);
                    const auto& owned = plugin.sectorOwnership[mySector];
                    bool iOwnedNext = std::find(owned.begin(), owned.end(), next) != owned.end();
                    bool nextIsDefined = plugin.sectorOwnership.find(next) != plugin.sectorOwnership.end();

                    if (actualCtrl.empty() && !iOwnedNext && !nextIsDefined) {
                        score = -1; // external & undefined offline neighbor
                        break;
                    }
                }
            }

            // Pick higher score; tie-break on higher XFL
            if (!best || score > highestScore || (score == highestScore && entry->xfl > best->xfl)) {
                best = entry;
                highestScore = score;
            }
        }

        return best;
        };

    // One pass over your waypoint index
    const LOAEntry* result = matchInIndexed(plugin.indexByWaypoint);
    if (result) {
        plugin.matchedLOACache[callsign] = result;
        plugin.matchTimestamps[callsign] = now;
        plugin.matchVersions[callsign] = plugin.sectorControlVersion;
        return result;
    }

    // Fallback LOAs (unchanged; note DOT access for entry)
    int clearedAltitude = fp.GetClearedAltitude();
    for (const auto& entry : fallbackLoas) {
        if (clearedAltitude < entry.minAltitudeFt) continue;

        if (!entry.destinationAirports.empty() &&
            !plugin.MatchesAirport(entry.destinationAirportSet, entry.destinationAirportPrefixes, destination)) continue;

        if (!entry.nextSectors.empty() && !shouldMatchLOA(entry.nextSectors)) continue;

        bool wpMatch = std::all_of(entry.waypoints.begin(), entry.waypoints.end(),
            [&](const std::string& wp) {
                return std::any_of(routePoints.begin(), routePoints.end(),
                    [&](const std::string& r) { return EqualsIgnoreCase(r, wp); });
            });

        if (wpMatch) {
            plugin.matchedLOACache[callsign] = &entry;
            plugin.matchTimestamps[callsign] = now;
            plugin.matchVersions[callsign] = plugin.sectorControlVersion;
            return &entry;
        }
    }

    plugin.matchedLOACache[callsign] = nullptr;
    plugin.matchTimestamps[callsign] = now;
    plugin.matchVersions[callsign] = plugin.sectorControlVersion;
    return nullptr;
}