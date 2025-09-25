#include "stdafx.h"
#include "LOAPlugin.h"
#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <unordered_set>

extern EuroScopePlugIn::CPlugIn* plugin;

// Case-insensitive compare
bool EqualsIgnoreCase(const std::string& a, const std::string& b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(),
        [](char a, char b) { return tolower(a) == tolower(b); });
}
// Make sure 'plugin' is your instance of CPlugIn-derived class
std::unordered_set<std::string> GetOnlineControllers()
{
    std::unordered_set<std::string> result;

    // Start with the first controller
    EuroScopePlugIn::CController controller = plugin.ControllerSelectFirst();

    while (controller.IsValid())
    {
        std::string id = controller.GetPositionId();  // This is the controller ID (e.g., "ALR", "HAMW")
        result.insert(id);

        controller = plugin.ControllerSelectNext(controller);
    }

    return result;
}
const LOAEntry* MatchLoaEntry(const EuroScopePlugIn::CFlightPlan& fp, const std::unordered_set<std::string>& onlineControllers)
{
    if (!fp.IsValid()) return nullptr;

    std::string origin = fp.GetFlightPlanData().GetOrigin();
    std::string destination = fp.GetFlightPlanData().GetDestination();
    std::string controller = fp.GetTrackingControllerId();

    auto route = fp.GetExtractedRoute();
    int numPoints = route.GetPointsNumber();
    std::vector<std::string> routePoints;
    for (int i = 0; i < numPoints; ++i) {
        routePoints.emplace_back(route.GetPointName(i));
    }

    auto matchIn = [&](const std::vector<LOAEntry>& entries) -> const LOAEntry* {
        for (const auto& entry : entries) {
            bool originMatch = entry.originAirports.empty() || std::any_of(entry.originAirports.begin(), entry.originAirports.end(),
                [&](const std::string& o) { return EqualsIgnoreCase(o, origin); });

            bool destMatch = entry.destinationAirports.empty() || std::any_of(entry.destinationAirports.begin(), entry.destinationAirports.end(),
                [&](const std::string& d) { return EqualsIgnoreCase(d, destination); });

            bool sectorMatch = entry.sectors.empty() || std::any_of(entry.sectors.begin(), entry.sectors.end(),
                [&](const std::string& s) { return EqualsIgnoreCase(s, controller); });

            bool wpMatch = true;
            for (const std::string& wp : entry.waypoints) {
                auto it = std::find_if(routePoints.begin(), routePoints.end(),
                    [&](const std::string& r) { return EqualsIgnoreCase(r, wp); });
                if (it == routePoints.end()) {
                    wpMatch = false;
                    break;
                }
            }

            // ✅ NEW: match nextSectors if defined
            bool nextSectorMatch = entry.nextSectors.empty() || std::any_of(entry.nextSectors.begin(), entry.nextSectors.end(),
                [&](const std::string& ns) { return EqualsIgnoreCase(ns, controller); });

            // ✅ New: requireNextSectorOnline logic
            bool nextSectorOnlineOk = true;
            if (entry.requireNextSectorOnline && !entry.nextSectors.empty()) {
                nextSectorOnlineOk = std::any_of(entry.nextSectors.begin(), entry.nextSectors.end(),
                    [&](const std::string& ns) {
                        return onlineControllers.count(ns) > 0;
                    });
            }

            // ✅ New: requiresSectorOnline logic
            bool sectorOnlineOk = true;
            if (entry.requireNextSectorOnline && !entry.sectors.empty()) {
                sectorOnlineOk = std::any_of(entry.sectors.begin(), entry.sectors.end(),
                    [&](const std::string& s) {
                        return onlineControllers.count(s) > 0;
                    });
            }

            if (originMatch && destMatch && sectorMatch && wpMatch && nextSectorMatch && nextSectorOnlineOk && sectorOnlineOk)
                return &entry;
        }
        return nullptr;
        };

    if (const LOAEntry* match = matchIn(destinationLoas)) return match;
    if (const LOAEntry* match = matchIn(departureLoas)) return match;
    if (const LOAEntry* match = matchIn(lorArrivals))     return match;
    if (const LOAEntry* match = matchIn(lorDepartures))   return match;

    return nullptr;
}