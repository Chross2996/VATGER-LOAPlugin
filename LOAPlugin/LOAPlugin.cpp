// =========================
// File: LOAPlugin.cpp
// =========================

#include "stdafx.h"
#include "LOAPlugin.h"
#include <windows.h>
#include <fstream>
#include <shlwapi.h>
#include <unordered_set>
#include "json.hpp"

using json = nlohmann::json;
extern "C" IMAGE_DOS_HEADER __ImageBase;

// Define global LOA vectors here
std::vector<LOAEntry> destinationLoas;
std::vector<LOAEntry> departureLoas;
std::vector<LOAEntry> lorArrivals;
std::vector<LOAEntry> lorDepartures;

std::unordered_map<std::string, std::string> controllerFrequencies;
std::unordered_map<int, std::pair<std::string, EuroScopePlugIn::CFlightPlan>> handoffTargets;

int nextFunctionId = 1000;

LOAPlugin::LOAPlugin()
	: CPlugIn(COMPATIBILITY_CODE, "LOA Plugin", "1.1", "Author", "LOA Plugin")
{
	RegisterTagItemType("LOA XFL", 1996);
	RegisterTagItemType("LOA XFL Detailed", 2000);
	RegisterTagItemType("Next Sector", 1998);
	RegisterTagItemType("COP", 1997);
	RegisterTagItemFunction("LOA Sector Menu", 1999);
	RegisterTagItemFunction("Initiate Handoff", 2001);

	DisplayUserMessage("LOA Plugin", "Init", "LOA Plugin initialized. Loading LOAs from JSON...", true, true, true, true, false);
	LoadLOAsFromJSON();
}

LOAPlugin::~LOAPlugin() {}

bool LOAPlugin::IsControllerOnlineCached(const std::string& controllerId, const std::unordered_set<std::string>& onlineControllers)
{
	return onlineControllers.count(controllerId) > 0;
}

bool LOAPlugin::MatchesAirport(const std::vector<std::string>& list, const std::string& airport)
{
	for (const auto& code : list) {
		if (airport == code || (airport.length() >= code.length() && airport.substr(0, code.length()) == code)) {
			return true;
		}
	}
	return false;
}

bool LOAPlugin::RouteContainsAllWaypoints(const EuroScopePlugIn::CFlightPlanExtractedRoute& route, const std::vector<std::string>& waypoints)
{
	for (const auto& wp : waypoints) {
		bool found = false;
		for (int i = 0; i < route.GetPointsNumber(); ++i) {
			if (route.GetPointName(i) == wp) {
				found = true;
				break;
			}
		}
		if (!found) return false;
	}
	return true;
}

void LOAPlugin::LoadLOAsFromJSON()
{
	char dllPath[MAX_PATH];
	GetModuleFileNameA(HINSTANCE(&__ImageBase), dllPath, sizeof(dllPath));
	std::string basePath = dllPath;
	basePath = basePath.substr(0, basePath.find_last_of("\\/")) + "\\loa_configs\\";

	WIN32_FIND_DATAA findFileData;
	HANDLE hFind = FindFirstFileA((basePath + "*.json").c_str(), &findFileData);

	if (hFind == INVALID_HANDLE_VALUE) {
		DisplayUserMessage("LOA Plugin", "LOA Load", "No JSON files found in loa_configs folder.", true, true, true, true, false);
		return;
	}

	do {
		std::ifstream f(basePath + findFileData.cFileName);
		if (!f.is_open()) continue;

		try {
			json j;
			f >> j;

			if (j.contains("destinationLoas")) {
				for (const auto& entry : j["destinationLoas"]) {
					LOAEntry loa;
					loa.sectors = entry["sectors"].get<std::vector<std::string>>();
					loa.waypoints = entry["waypoints"].get<std::vector<std::string>>();
					loa.destinationAirports = entry["destinations"].get<std::vector<std::string>>();
					loa.xfl = entry["xfl"];
					loa.nextSectors = entry["nextSectors"].get<std::vector<std::string>>();
					loa.copText = entry["copText"];
					loa.requireNextSectorOnline = entry["requireNextSectorOnline"];
					destinationLoas.push_back(loa);
				}
			}

			if (j.contains("departureLoas")) {
				for (const auto& entry : j["departureLoas"]) {
					LOAEntry loa;
					loa.sectors = entry["sectors"].get<std::vector<std::string>>();
					loa.waypoints = entry["waypoints"].get<std::vector<std::string>>();
					loa.originAirports = entry["origins"].get<std::vector<std::string>>();
					loa.xfl = entry["xfl"];
					loa.nextSectors = entry["nextSectors"].get<std::vector<std::string>>();
					loa.copText = entry["copText"];
					loa.requireNextSectorOnline = entry["requireNextSectorOnline"];
					departureLoas.push_back(loa);
				}
			}

			if (j.contains("lorArrivals")) {
				for (const auto& entry : j["lorArrivals"]) {
					LOAEntry loa;
					loa.sectors = entry["sectors"].get<std::vector<std::string>>();
					loa.destinationAirports = entry["destinations"].get<std::vector<std::string>>();
					loa.xfl = entry["xfl"];
					loa.nextSectors = entry["nextSectors"].get<std::vector<std::string>>();
					loa.requireNextSectorOnline = entry["requireNextSectorOnline"];
					lorArrivals.push_back(loa);
				}
			}

			if (j.contains("lorDepartures")) {
				for (const auto& entry : j["lorDepartures"]) {
					LOAEntry loa;
					loa.sectors = entry["sectors"].get<std::vector<std::string>>();
					loa.originAirports = entry["origins"].get<std::vector<std::string>>();
					loa.xfl = entry["xfl"];
					loa.nextSectors = entry["nextSectors"].get<std::vector<std::string>>();
					loa.requireNextSectorOnline = entry["requireNextSectorOnline"];
					lorDepartures.push_back(loa);
				}
			}
		}
		catch (const std::exception& e) {
			std::string msg = std::string("Error parsing ") + findFileData.cFileName + ": " + e.what();
			DisplayUserMessage("LOA Plugin", "JSON Parse Error", msg.c_str(), true, true, true, true, false);
		}
	} while (FindNextFileA(hFind, &findFileData) != 0);

	FindClose(hFind);
	DisplayUserMessage("LOA Plugin", "Success", "All LOA entries loaded from sector-specific JSON files.", true, true, true, true, false);
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
	case 1998:
		RenderNextSectorTagItem(flightPlan, radarTarget, tagData, sItemString, pColorCode, pRGB, pFontSize);
		break;
	default:
		break;
	}
}
LOAPlugin plugin;