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

#pragma comment(lib, "Gdi32.lib")
#pragma comment(lib, "User32.lib")

using json = nlohmann::json;

static std::string AddArrowPrefix(const std::string& text)
{
    // NOTE: We used to prefix popup items with "> " for readability.
    // Users requested a compact list, so keep menu text unchanged.
    return text;
}

extern "C" IMAGE_DOS_HEADER __ImageBase;

// ============================================================================
// TopSky-style custom sector handoff popup
// - This replaces EuroScope OpenPopupList only for the visual list.
// - Existing behavior is preserved: ASSUME, FREE, and sector handoff.
// ============================================================================

namespace {
    enum class CustomHandoffAction {
        Separator = 0,
        Assume = 1,
        Sector = 2,
        Release = 3,
        // A single row that renders 4 small side-by-side mini-buttons (F T C D).
        // Each button writes its code to flight strip annotation field 8 and releases tracking.
        ReleaseBar = 4
    };

    static const int kReleaseBarCount = 4;
    static const char* kReleaseBarLabels[kReleaseBarCount] = { "F", "T", "C", "D" };
    static const char* kReleaseBarCodes[kReleaseBarCount] = { "/rf/", "/rt/", "/rc/", "/rd/" };

    struct CustomHandoffRow {
        CustomHandoffAction action = CustomHandoffAction::Separator;
        std::string label;
        std::string frequency;
        std::string sectorId;
        std::string targetCallsign;
        std::string callsign;
        bool disabled = false;
    };

    static HWND g_handoffPopupWnd = NULL;
    static std::vector<CustomHandoffRow> g_handoffRows;
    static int g_handoffHoverIndex = -1;
    static int g_handoffHoverSubIndex = -1;  // which mini-button is hovered inside a ReleaseBar row
    static int g_selectedReleaseSubIndex = 0; // which release type is selected (0=F default); -1 = none

    struct CustomHandoffPopupStyle {
        int popupWidth = 172;
        int headerHeight = 42;
        // Each selectable sector row is tall enough for two lines:
        //   Sector ID
        //   Frequency
        int rowHeight = 38;
        int popupBorder = 2;
        int listVerticalPadding = 6;
        int maxVisibleRows = 12;

        std::string fontFace = "Consolas";
        int headerFontSize = 16;
        int sectorFontSize = 16;
        int frequencyFontSize = 13;

        COLORREF backgroundColor = RGB(245, 245, 245);
        COLORREF borderColor = RGB(64, 64, 64);
        COLORREF separatorColor = RGB(128, 128, 128);
        COLORREF headerTextColor = RGB(0, 80, 160);
        COLORREF normalTextColor = RGB(0, 80, 160);
        COLORREF actionTextColor = RGB(0, 0, 0);
        COLORREF actionBackgroundColor = RGB(235, 235, 235);

        // Fill color for normal, non-hover selectable rows.
        // This makes each sector/action row look like a pressable button.
        COLORREF buttonBackgroundColor = RGB(230, 230, 230);

        COLORREF hoverBackgroundColor = RGB(0, 0, 0);
        COLORREF hoverTextColor = RGB(255, 255, 255);
    };

    static CustomHandoffPopupStyle g_popupStyle;
    static bool g_popupStyleLoaded = false;

    // Cached GDI font handles — created once per style load, shared across all WM_PAINT calls.
    static HFONT g_cachedHeaderFont = NULL;
    static HFONT g_cachedRowFont    = NULL;
    static HFONT g_cachedFreqFont   = NULL;

    static void DestroyPopupFonts()
    {
        if (g_cachedHeaderFont) { DeleteObject(g_cachedHeaderFont); g_cachedHeaderFont = NULL; }
        if (g_cachedRowFont)    { DeleteObject(g_cachedRowFont);    g_cachedRowFont    = NULL; }
        if (g_cachedFreqFont)   { DeleteObject(g_cachedFreqFont);   g_cachedFreqFont   = NULL; }
    }

    static int ClampColorByte(int v)
    {
        if (v < 0) return 0;
        if (v > 255) return 255;
        return v;
    }


    static void DestroyCustomHandoffPopup()
    {
        if (g_handoffPopupWnd && IsWindow(g_handoffPopupWnd)) {
            KillTimer(g_handoffPopupWnd, 1001);
            KillTimer(g_handoffPopupWnd, 1002);
            DestroyWindow(g_handoffPopupWnd);
        }

        g_handoffPopupWnd = NULL;
        g_handoffRows.clear();
        g_handoffHoverIndex = -1;
        g_handoffHoverSubIndex = -1;
        g_selectedReleaseSubIndex = 0;
        DestroyPopupFonts();
    }

    static COLORREF ParseHexColor(const std::string& value, COLORREF fallback)
    {
        std::string s = value;
        if (!s.empty() && s[0] == '#') s = s.substr(1);
        if (s.size() != 6) return fallback;
        char* end = nullptr;
        const long rgb = std::strtol(s.c_str(), &end, 16);
        if (!end || *end != '\0') return fallback;
        return RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
    }

    static COLORREF ParseJsonColor(const json& value, COLORREF fallback)
    {
        // Preferred config format:
        //   "backgroundColor": [245, 245, 245]
        // Also still supports old hex format:
        //   "backgroundColor": "#F5F5F5"
        if (value.is_array() && value.size() >= 3 &&
            value[0].is_number_integer() &&
            value[1].is_number_integer() &&
            value[2].is_number_integer())
        {
            const int r = ClampColorByte(value[0].get<int>());
            const int g = ClampColorByte(value[1].get<int>());
            const int b = ClampColorByte(value[2].get<int>());
            return RGB(r, g, b);
        }

        if (value.is_string()) {
            return ParseHexColor(value.get<std::string>(), fallback);
        }

        return fallback;
    }

    static void LoadCustomHandoffPopupStyle()
    {
        if (g_popupStyleLoaded) return;
        g_popupStyleLoaded = true;

        char dllPath[MAX_PATH];
        GetModuleFileNameA(HINSTANCE(&__ImageBase), dllPath, sizeof(dllPath));
        std::string basePath(dllPath);
        size_t lastSlash = basePath.find_last_of("\\/");
        basePath = (lastSlash != std::string::npos) ? basePath.substr(0, lastSlash) : ".";

        const std::string filePath = basePath + "\\loa_configs_json\\custom_handoff_popup.json";
        std::ifstream in(filePath.c_str());
        if (!in.is_open()) {
            // Optional file. Defaults are used if it is missing.
            return;
        }

        try {
            json j;
            in >> j;

            auto readInt = [&](const char* key, int& out) {
                if (j.contains(key) && j[key].is_number_integer()) out = j[key].get<int>();
                };
            auto readString = [&](const char* key, std::string& out) {
                if (j.contains(key) && j[key].is_string()) out = j[key].get<std::string>();
                };
            auto readColor = [&](const char* key, COLORREF& out) {
                if (j.contains(key)) out = ParseJsonColor(j[key], out);
                };

            readInt("popupWidth", g_popupStyle.popupWidth);
            readInt("headerHeight", g_popupStyle.headerHeight);
            readInt("rowHeight", g_popupStyle.rowHeight);
            readInt("popupBorder", g_popupStyle.popupBorder);
            readInt("listVerticalPadding", g_popupStyle.listVerticalPadding);
            readInt("maxVisibleRows", g_popupStyle.maxVisibleRows);

            readString("fontFace", g_popupStyle.fontFace);
            readInt("headerFontSize", g_popupStyle.headerFontSize);
            readInt("sectorFontSize", g_popupStyle.sectorFontSize);
            readInt("frequencyFontSize", g_popupStyle.frequencyFontSize);

            readColor("backgroundColor", g_popupStyle.backgroundColor);
            readColor("borderColor", g_popupStyle.borderColor);
            readColor("separatorColor", g_popupStyle.separatorColor);
            readColor("headerTextColor", g_popupStyle.headerTextColor);
            readColor("normalTextColor", g_popupStyle.normalTextColor);
            readColor("actionTextColor", g_popupStyle.actionTextColor);
            readColor("actionBackgroundColor", g_popupStyle.actionBackgroundColor);
            readColor("buttonBackgroundColor", g_popupStyle.buttonBackgroundColor);
            readColor("hoverBackgroundColor", g_popupStyle.hoverBackgroundColor);
            readColor("hoverTextColor", g_popupStyle.hoverTextColor);

            if (g_popupStyle.popupWidth < 80) g_popupStyle.popupWidth = 80;
            if (g_popupStyle.headerHeight < 20) g_popupStyle.headerHeight = 20;
            if (g_popupStyle.rowHeight < 18) g_popupStyle.rowHeight = 18;
            if (g_popupStyle.maxVisibleRows < 1) g_popupStyle.maxVisibleRows = 1;
        }
        catch (...) {
            // Invalid config should never break the plugin. Defaults remain active.
        }
    }

    static void EnsurePopupFonts()
    {
        if (g_cachedHeaderFont) return;
        g_cachedHeaderFont = CreateFontA(
            g_popupStyle.headerFontSize, 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE,
            ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
            FIXED_PITCH | FF_MODERN, g_popupStyle.fontFace.c_str());
        g_cachedRowFont = CreateFontA(
            g_popupStyle.sectorFontSize, 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE,
            ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
            FIXED_PITCH | FF_MODERN, g_popupStyle.fontFace.c_str());
        g_cachedFreqFont = CreateFontA(
            g_popupStyle.frequencyFontSize, 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE,
            ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
            FIXED_PITCH | FF_MODERN, g_popupStyle.fontFace.c_str());
    }

    static EuroScopePlugIn::CFlightPlan FindFlightPlanByCallsign(const std::string& callsign)
    {
        for (EuroScopePlugIn::CFlightPlan fp = plugin.FlightPlanSelectFirst();
            fp.IsValid();
            fp = plugin.FlightPlanSelectNext(fp))
        {
            if (_stricmp(fp.GetCallsign(), callsign.c_str()) == 0)
                return fp;
        }

        return EuroScopePlugIn::CFlightPlan();
    }

    static void HideCustomHandoffPopup()
    {
        if (g_handoffPopupWnd) {
            ShowWindow(g_handoffPopupWnd, SW_HIDE);
        }
        g_handoffHoverIndex = -1;
        g_handoffHoverSubIndex = -1;
        g_selectedReleaseSubIndex = 0;
    }

    // Clicking a ReleaseBar mini-button only updates the selection.
    // The annotation is written (and the handoff fired) when the user subsequently clicks a Sector row.
    static void ExecuteReleaseBarButton(int subIndex)
    {
        if (subIndex < 0 || subIndex >= kReleaseBarCount) return;
        g_selectedReleaseSubIndex = subIndex;
        // Do NOT close the popup — the user still needs to pick a sector.
    }

    static void ExecuteCustomHandoffRow(const CustomHandoffRow& row)
    {
        EuroScopePlugIn::CFlightPlan fp = FindFlightPlanByCallsign(row.callsign);
        if (!fp.IsValid())
            return;

        if (row.action == CustomHandoffAction::Assume) {
            const int state = fp.GetState();
            if (state == EuroScopePlugIn::FLIGHT_PLAN_STATE_TRANSFER_TO_ME_INITIATED)
                fp.AcceptHandoff();
            else
                fp.StartTracking();
            return;
        }

        if (row.action == CustomHandoffAction::Release) {
            const char* trackingId = fp.GetTrackingControllerId();
            std::string myPosId = plugin.ControllerMyself().GetPositionId();

            if (trackingId && trackingId[0] &&
                _stricmp(trackingId, myPosId.c_str()) == 0)
            {
                fp.EndTracking();
            }
            return;
        }

        if (row.action == CustomHandoffAction::Sector) {
            if (!row.targetCallsign.empty()) {
                // If a release type is selected via the ReleaseBar, write the annotation first.
                if (g_selectedReleaseSubIndex >= 0 && g_selectedReleaseSubIndex < kReleaseBarCount) {
                    fp.GetControllerAssignedData().SetFlightStripAnnotation(7, kReleaseBarCodes[g_selectedReleaseSubIndex]);
                }

                fp.InitiateHandoff(row.targetCallsign.c_str());

                // Keep the existing Next Sector tag behavior: show the chosen sector
                // while TRANSFER_FROM_ME_INITIATED.
                std::string cs = fp.GetCallsign();
                plugin.activeHandoffTargets[cs] = row.sectorId;

                std::string key = cs + ":" + std::to_string(ItemCodes::TAG_ITEM_NEXT_SECTOR_CTRL);
                plugin.renderCache.erase(key);
            }
            return;
        }

        // ReleaseBar clicks are routed through ExecuteReleaseBarButton with a sub-index;
        // this path is not reached for ReleaseBar rows.
    }


    static bool CustomPopupHasAssumeRow()
    {
        for (size_t i = 0; i < g_handoffRows.size(); ++i) {
            if (g_handoffRows[i].action == CustomHandoffAction::Assume ||
                g_handoffRows[i].action == CustomHandoffAction::ReleaseBar)
                return true;
        }
        return false;
    }

    static int GetEffectiveHeaderHeight()
    {
        // When ASSUME is not available, there is no Section 1 button.
        // Compact the header so Section 2 starts directly below the title area.
        return CustomPopupHasAssumeRow()
            ? g_popupStyle.headerHeight
            : 28;
    }


    static int GetCustomHandoffRowHeight(const CustomHandoffRow& row)
    {
        if (row.action == CustomHandoffAction::Separator)
            return 8;

        return g_popupStyle.rowHeight;
    }

    static int GetCustomHandoffRowsHeight(const std::vector<CustomHandoffRow>& rows, int maxVisibleRows)
    {
        int total = 0;

        for (int i = 0; i < (int)rows.size() && i < maxVisibleRows; ++i) {
            total += GetCustomHandoffRowHeight(rows[i]);
        }

        return total;
    }

    static int GetCustomHandoffRowTop(int rowIndex)
    {
        int y = 0;

        for (int i = 0; i < rowIndex && i < (int)g_handoffRows.size(); ++i) {
            y += GetCustomHandoffRowHeight(g_handoffRows[i]);
        }

        return y;
    }

    static int RowFromPointY(int y)
    {
        const int listTop = GetEffectiveHeaderHeight() + g_popupStyle.listVerticalPadding;

        if (y < listTop)
            return -1;

        int curY = listTop;

        for (int i = 0; i < (int)g_handoffRows.size() && i < g_popupStyle.maxVisibleRows; ++i) {
            const int rowH = GetCustomHandoffRowHeight(g_handoffRows[i]);

            if (y >= curY && y < curY + rowH) {
                if (g_handoffRows[i].action == CustomHandoffAction::Separator)
                    return -1;

                return i;
            }

            curY += rowH;
        }

        return -1;
    }

    // Given a client-area X coordinate and the row rect width, return which mini-button
    // (0-3) is under the cursor inside a ReleaseBar row, or -1 if outside.
    static int ReleaseBarSubIndexFromX(int x, int clientWidth)
    {
        const int margin = g_popupStyle.popupBorder + 4;
        const int barLeft = margin;
        const int barRight = clientWidth - margin;
        const int barWidth = barRight - barLeft;
        if (barWidth <= 0 || x < barLeft || x >= barRight) return -1;

        const int btnWidth = barWidth / kReleaseBarCount;
        int sub = (x - barLeft) / btnWidth;
        if (sub >= kReleaseBarCount) sub = kReleaseBarCount - 1;
        return sub;
    }

    static void DrawCenteredText(HDC hdc, const std::string& text, RECT rc, COLORREF color, HFONT font)
    {
        HFONT oldFont = (HFONT)SelectObject(hdc, font);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, color);
        DrawTextA(hdc, text.c_str(), -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, oldFont);
    }

    static LRESULT CALLBACK CustomHandoffPopupProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;

        case WM_ERASEBKGND:
            return 1;

        case WM_MOUSEMOVE:
        {
            KillTimer(hwnd, 1001);

            TRACKMOUSEEVENT tme = {};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);

            const int x = (short)LOWORD(lParam);
            const int y = (short)HIWORD(lParam);
            int idx = RowFromPointY(y);
            if (idx >= 0 && g_handoffRows[idx].disabled)
                idx = -1;

            int subIdx = -1;
            if (idx >= 0 && g_handoffRows[idx].action == CustomHandoffAction::ReleaseBar) {
                RECT rc;
                GetClientRect(hwnd, &rc);
                subIdx = ReleaseBarSubIndexFromX(x, rc.right);
            }

            if (idx != g_handoffHoverIndex || subIdx != g_handoffHoverSubIndex) {
                g_handoffHoverIndex = idx;
                g_handoffHoverSubIndex = subIdx;
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            // Do not close immediately. Give the cursor a small safe zone around the popup.
            SetTimer(hwnd, 1001, 200, NULL);
            return 0;

        case WM_TIMER:
        {
            if (wParam == 1001) {
                POINT pt;
                GetCursorPos(&pt);

                RECT rc;
                GetWindowRect(hwnd, &rc);

                // Hover-safe margin around the popup. This prevents accidental closing
                // when the mouse briefly leaves the exact window border.
                InflateRect(&rc, 16, 16);

                if (!PtInRect(&rc, pt)) {
                    KillTimer(hwnd, 1001);
                    HideCustomHandoffPopup();
                }
            }
            else if (wParam == 1002) {
                KillTimer(hwnd, 1002);
                HideCustomHandoffPopup();
            }
            return 0;
        }

        case WM_KILLFOCUS:
            KillTimer(hwnd, 1001);
            KillTimer(hwnd, 1002);
            HideCustomHandoffPopup();
            return 0;

        case WM_LBUTTONDOWN:
        {
            const int x = (short)LOWORD(lParam);
            const int y = (short)HIWORD(lParam);
            const int idx = RowFromPointY(y);

            if (idx >= 0 && idx < (int)g_handoffRows.size() && !g_handoffRows[idx].disabled) {
                const CustomHandoffRow& row = g_handoffRows[idx];

                if (row.action == CustomHandoffAction::ReleaseBar) {
                    RECT rc;
                    GetClientRect(hwnd, &rc);
                    const int sub = ReleaseBarSubIndexFromX(x, rc.right);
                    if (sub < 0) return 0;  // clicked between buttons

                    // Only update the selection; keep popup open so user can still pick a sector.
                    ExecuteReleaseBarButton(sub);
                    InvalidateRect(hwnd, NULL, FALSE);
                    // Do NOT set the close timer.
                }
                else {
                    CustomHandoffRow rowCopy = row;

                    g_handoffHoverIndex = idx;
                    g_handoffHoverSubIndex = -1;
                    InvalidateRect(hwnd, NULL, FALSE);

                    ExecuteCustomHandoffRow(rowCopy);

                    KillTimer(hwnd, 1001);
                    SetTimer(hwnd, 1002, 150, NULL);
                }
            }
            return 0;
        }

        case WM_DESTROY:
        case WM_NCDESTROY:
            KillTimer(hwnd, 1001);
            KillTimer(hwnd, 1002);
            if (hwnd == g_handoffPopupWnd) {
                g_handoffPopupWnd = NULL;
            }
            return 0;

        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            RECT client = {};
            GetClientRect(hwnd, &client);

            // soft popup shadow
            RECT shadow = client;
            OffsetRect(&shadow, 3, 3);

            HBRUSH shadowBrush = CreateSolidBrush(RGB(0, 0, 255));
            FillRect(hdc, &shadow, shadowBrush);
            DeleteObject(shadowBrush);

            HBRUSH bg = CreateSolidBrush(g_popupStyle.backgroundColor);
            FillRect(hdc, &client, bg);
            DeleteObject(bg);

            HPEN borderPen = CreatePen(PS_SOLID, 1, g_popupStyle.borderColor);
            HGDIOBJ oldPen = SelectObject(hdc, borderPen);
            HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));

            // Draw the border inset by one pixel so it is visually even on all sides.
            // Rectangle() treats right/bottom differently, so avoid using client.right/client.bottom directly.
            RECT borderRect = client;
            borderRect.left += 1;
            borderRect.top += 1;
            borderRect.right -= 1;
            borderRect.bottom -= 1;

            Rectangle(hdc, borderRect.left, borderRect.top, borderRect.right, borderRect.bottom);

            SelectObject(hdc, oldBrush);
            SelectObject(hdc, oldPen);
            DeleteObject(borderPen);

            EnsurePopupFonts();
            HFONT headerFont = g_cachedHeaderFont;
            HFONT rowFont    = g_cachedRowFont;
            HFONT freqFont   = g_cachedFreqFont;

            std::string callsign;
            if (!g_handoffRows.empty())
                callsign = g_handoffRows.front().callsign;

            const int effectiveHeaderHeight = GetEffectiveHeaderHeight();

            RECT r1{ 4, 5, client.right - 4, 24 };

            DrawCenteredText(hdc, callsign, r1, g_popupStyle.headerTextColor, headerFont);

            int currentRowY = effectiveHeaderHeight + g_popupStyle.listVerticalPadding;

            for (int i = 0; i < (int)g_handoffRows.size() && i < g_popupStyle.maxVisibleRows; ++i) {
                const CustomHandoffRow& row = g_handoffRows[i];
                const int rowH = GetCustomHandoffRowHeight(row);

                RECT rr{
                    g_popupStyle.popupBorder + 4,
                    currentRowY,
                    client.right - g_popupStyle.popupBorder - 4,
                    currentRowY + rowH
                };

                currentRowY += rowH;

                if (row.action == CustomHandoffAction::Separator) {
                    HPEN p = CreatePen(PS_SOLID, 1, g_popupStyle.separatorColor);
                    HGDIOBJ op = SelectObject(hdc, p);
                    const int midY = rr.top + (rowH / 2);
                    MoveToEx(hdc, rr.left, midY, NULL);
                    LineTo(hdc, rr.right, midY);
                    SelectObject(hdc, op);
                    DeleteObject(p);
                    continue;
                }

                // ---- ReleaseBar: four small buttons side by side ----
                if (row.action == CustomHandoffAction::ReleaseBar) {
                    // Fill row background
                    HBRUSH rowBg = CreateSolidBrush(g_popupStyle.backgroundColor);
                    FillRect(hdc, &rr, rowBg);
                    DeleteObject(rowBg);

                    const int totalW = rr.right - rr.left;
                    const int btnW = totalW / kReleaseBarCount;
                    const int btnGap = 3;  // horizontal gap between mini-buttons

                    // Active/selected button colour: use hover background with a visible border.
                    const COLORREF selectedBg = g_popupStyle.hoverBackgroundColor;
                    const COLORREF selectedTxt = g_popupStyle.hoverTextColor;

                    for (int b = 0; b < kReleaseBarCount; ++b) {
                        const bool isSelected = (b == g_selectedReleaseSubIndex);
                        const bool btnHover = (i == g_handoffHoverIndex && b == g_handoffHoverSubIndex);

                        COLORREF btnBg = isSelected ? selectedBg
                            : btnHover ? RGB(200, 200, 200)  // lighter hover when not selected
                            : g_popupStyle.buttonBackgroundColor;
                        COLORREF txtCol = isSelected ? selectedTxt
                            : g_popupStyle.normalTextColor;

                        RECT btn;
                        btn.left = rr.left + b * btnW + btnGap;
                        btn.right = rr.left + (b + 1) * btnW - btnGap;
                        btn.top = rr.top + 3;
                        btn.bottom = rr.bottom - 3;

                        HPEN   bPen = CreatePen(PS_SOLID, isSelected ? 2 : 1,
                            isSelected ? RGB(40, 40, 40)
                            : btnHover ? RGB(80, 80, 80)
                            : RGB(120, 120, 120));
                        HBRUSH bBrush = CreateSolidBrush(btnBg);

                        HGDIOBJ oPen = SelectObject(hdc, bPen);
                        HGDIOBJ oBrush = SelectObject(hdc, bBrush);

                        RoundRect(hdc, btn.left, btn.top, btn.right, btn.bottom, 6, 6);

                        SelectObject(hdc, oBrush);
                        SelectObject(hdc, oPen);
                        DeleteObject(bBrush);
                        DeleteObject(bPen);

                        DrawCenteredText(hdc, kReleaseBarLabels[b], btn, txtCol, rowFont);
                    }
                    continue;
                }

                const bool hover = (i == g_handoffHoverIndex);

                // The full row uses the popup background. The inner rounded rectangle is the actual button.
                COLORREF rowBgColor = g_popupStyle.backgroundColor;
                COLORREF buttonBgColor = hover ? g_popupStyle.hoverBackgroundColor : g_popupStyle.buttonBackgroundColor;
                COLORREF textColor = hover ? g_popupStyle.hoverTextColor : g_popupStyle.normalTextColor;

                HBRUSH rowBg = CreateSolidBrush(rowBgColor);
                FillRect(hdc, &rr, rowBg);
                DeleteObject(rowBg);

                // Draw each selectable entry as a rounded button.
                // Separators are skipped earlier, so this applies to ASSUME, sector rows, and FREE.
                RECT box = rr;
                InflateRect(&box, -2, -1);

                HPEN boxPen = CreatePen(PS_SOLID, 1, hover
                    ? RGB(40, 40, 40)
                    : RGB(120, 120, 120));

                HBRUSH boxBrush = CreateSolidBrush(buttonBgColor);

                HGDIOBJ oldBoxPen = SelectObject(hdc, boxPen);
                HGDIOBJ oldBoxBrush = SelectObject(hdc, boxBrush);

                RoundRect(
                    hdc,
                    box.left,
                    box.top,
                    box.right,
                    box.bottom,
                    8,
                    8);

                SelectObject(hdc, oldBoxBrush);
                SelectObject(hdc, oldBoxPen);

                DeleteObject(boxBrush);
                DeleteObject(boxPen);

                if (row.action == CustomHandoffAction::Sector && !row.frequency.empty()) {
                    RECT sectorRect = rr;
                    sectorRect.bottom = rr.top + (g_popupStyle.rowHeight / 2) + 3;

                    RECT freqRect = rr;
                    freqRect.top = rr.top + (g_popupStyle.rowHeight / 2) - 3;

                    DrawCenteredText(hdc, row.label, sectorRect, textColor, rowFont);
                    DrawCenteredText(hdc, row.frequency, freqRect, textColor, freqFont);
                }
                else {
                    DrawCenteredText(hdc, row.label, rr, textColor, rowFont);
                }
            }

            EndPaint(hwnd, &ps);
            return 0;
        }

        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
        }
    }

    static void EnsureCustomHandoffPopupWindow()
    {
        LoadCustomHandoffPopupStyle();

        if (g_handoffPopupWnd && IsWindow(g_handoffPopupWnd))
            return;

        HINSTANCE hInst = HINSTANCE(&__ImageBase);

        static bool classRegistered = false;
        if (!classRegistered) {
            WNDCLASSEXA wc = {};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = CustomHandoffPopupProc;
            wc.hInstance = hInst;
            wc.lpszClassName = "LOAPluginCustomHandoffPopup";
            wc.hCursor = LoadCursor(NULL, IDC_ARROW);
            wc.hbrBackground = NULL;
            RegisterClassExA(&wc);
            classRegistered = true;
        }

        g_handoffPopupWnd = CreateWindowExA(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            "LOAPluginCustomHandoffPopup",
            "LOA Handoff",
            WS_POPUP,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            g_popupStyle.popupWidth,
            200,
            NULL,
            NULL,
            hInst,
            NULL);
    }

    static void ShowCustomHandoffPopup(POINT pt, const std::vector<CustomHandoffRow>& rows)
    {
        POINT mousePt;
        GetCursorPos(&mousePt);
        pt = mousePt;

        LoadCustomHandoffPopupStyle();

        if (rows.empty())
            return;

        EnsureCustomHandoffPopupWindow();
        if (!g_handoffPopupWnd)
            return;

        g_handoffRows = rows;
        g_handoffHoverIndex = -1;
        g_handoffHoverSubIndex = -1;
        g_selectedReleaseSubIndex = 0;  // default: F (Full) pre-selected

        const int visibleRows = (int)std::min<size_t>(g_handoffRows.size(), g_popupStyle.maxVisibleRows);
        // Use a smaller bottom padding so the visual gap above/below the list appears equal.
        const int bottomPadding = 1;
        const int effectiveHeaderHeight = GetEffectiveHeaderHeight();
        const int rowsHeight = GetCustomHandoffRowsHeight(g_handoffRows, g_popupStyle.maxVisibleRows);

        const int height =
            effectiveHeaderHeight +
            g_popupStyle.listVerticalPadding +
            bottomPadding +
            rowsHeight +
            (g_popupStyle.popupBorder * 2);

        // Anchor row: the row that will be centred under the mouse cursor when the popup opens.
        // In ASSUMED state the user most likely wants to hand off immediately, so anchor to the
        // first sector row — it can be clicked without moving the mouse.
        // In all other states prefer ASSUME/ReleaseBar so the most common action is under the cursor.
        int anchorRow = 0;
        bool foundAnchor = false;

        const bool preferSectorAnchor =
            !g_handoffRows.empty() &&
            g_handoffRows.front().action == CustomHandoffAction::ReleaseBar &&
            // Only true when we're in ASSUMED (ReleaseBar present but no ASSUME row after it)
            [&]() {
            for (const auto& r : g_handoffRows)
                if (r.action == CustomHandoffAction::Assume) return false;
            return true;
            }();

        if (preferSectorAnchor) {
            // ASSUMED state: anchor to first sector row.
            for (int i = 0; i < (int)g_handoffRows.size(); ++i) {
                if (g_handoffRows[i].action == CustomHandoffAction::Sector && !g_handoffRows[i].disabled) {
                    anchorRow = i;
                    foundAnchor = true;
                    break;
                }
            }
        }

        // For all other states (or if no sector found above): prefer ASSUME or ReleaseBar.
        if (!foundAnchor) {
            for (int i = 0; i < (int)g_handoffRows.size(); ++i) {
                if ((g_handoffRows[i].action == CustomHandoffAction::Assume ||
                    g_handoffRows[i].action == CustomHandoffAction::ReleaseBar) &&
                    !g_handoffRows[i].disabled) {
                    anchorRow = i;
                    foundAnchor = true;
                    break;
                }
            }
        }

        // Otherwise use first sector.
        if (!foundAnchor) {
            for (int i = 0; i < (int)g_handoffRows.size(); ++i) {
                if (g_handoffRows[i].action == CustomHandoffAction::Sector && !g_handoffRows[i].disabled) {
                    anchorRow = i;
                    foundAnchor = true;
                    break;
                }
            }
        }

        // Otherwise use FREE.
        if (!foundAnchor) {
            for (int i = 0; i < (int)g_handoffRows.size(); ++i) {
                if (g_handoffRows[i].action == CustomHandoffAction::Release && !g_handoffRows[i].disabled) {
                    anchorRow = i;
                    foundAnchor = true;
                    break;
                }
            }
        }

        int x = pt.x - (g_popupStyle.popupWidth / 2);
        int y = pt.y - effectiveHeaderHeight - g_popupStyle.listVerticalPadding - GetCustomHandoffRowTop(anchorRow) - (GetCustomHandoffRowHeight(g_handoffRows[anchorRow]) / 2);

        // Keep the popup on the current monitor.
        HWND esWnd = GetForegroundWindow();
        HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoA(mon, &mi)) {
            if (x < mi.rcWork.left) x = mi.rcWork.left;
            if (y < mi.rcWork.top) y = mi.rcWork.top;
            if (x + g_popupStyle.popupWidth > mi.rcWork.right) x = mi.rcWork.right - g_popupStyle.popupWidth;
            if (y + height > mi.rcWork.bottom) y = mi.rcWork.bottom - height;
        }

        // Make the visual highlight match the row centered under the mouse.
        // Prefer first sector when available; otherwise ASSUME/ReleaseBar/FREE from the anchor logic above.
        g_handoffHoverIndex = anchorRow;
        g_handoffHoverSubIndex = -1;  // no sub-button pre-highlighted

        SetWindowPos(
            g_handoffPopupWnd,
            HWND_TOPMOST,
            x,
            y,
            g_popupStyle.popupWidth,
            height,
            SWP_NOACTIVATE | SWP_SHOWWINDOW);

        InvalidateRect(g_handoffPopupWnd, NULL, TRUE);
        UpdateWindow(g_handoffPopupWnd);
    }
}


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

LOAPlugin::~LOAPlugin() {
    DestroyCustomHandoffPopup();
}


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


static std::string TrimVolumeCoordText(std::string s)
{
    const char* ws = " \t\r\n";
    const size_t start = s.find_first_not_of(ws);
    if (start == std::string::npos) return std::string();
    const size_t end = s.find_last_not_of(ws);
    s = s.substr(start, end - start + 1);

    // Allow common copied coordinate decorations, e.g. N540230 / E0092429.
    if (!s.empty() && (s[0] == 'N' || s[0] == 'n' || s[0] == 'E' || s[0] == 'e' ||
        s[0] == 'S' || s[0] == 's' || s[0] == 'W' || s[0] == 'w')) {
        s = s.substr(1);
    }
    return s;
}

static bool TryParseCompactDmsCoordinate(std::string text, bool isLongitude, double& outDecimal)
{
    // Supported volumes.json coordinate formats:
    //   latitude:  "DDMMSS"  e.g. "540230"  -> 54°02'30"
    //   longitude: "DDDMMSS" e.g. "0092429" -> 009°24'29"
    // Optional leading +/- is accepted. Optional N/E/S/W prefix is accepted.
    text = TrimVolumeCoordText(text);
    if (text.empty()) return false;

    double sign = 1.0;
    if (text[0] == '+' || text[0] == '-') {
        sign = (text[0] == '-') ? -1.0 : 1.0;
        text = text.substr(1);
    }

    const size_t expectedDigits = isLongitude ? 7u : 6u;
    if (text.size() != expectedDigits) return false;

    for (size_t i = 0; i < text.size(); ++i) {
        if (!std::isdigit((unsigned char)text[i])) return false;
    }

    const size_t degDigits = isLongitude ? 3u : 2u;
    const int deg = std::atoi(text.substr(0, degDigits).c_str());
    const int min = std::atoi(text.substr(degDigits, 2).c_str());
    const int sec = std::atoi(text.substr(degDigits + 2, 2).c_str());

    if (min > 59 || sec > 59) return false;
    if (isLongitude) {
        if (deg > 180) return false;
    }
    else {
        if (deg > 90) return false;
    }

    outDecimal = sign * ((double)deg + ((double)min / 60.0) + ((double)sec / 3600.0));
    return true;
}

static bool TryReadVolumePointLL(const json& pt, double& outLat, double& outLon)
{
    if (!pt.is_array() || pt.size() < 2) return false;

    // Old decimal-degree format: [54.041944, 9.408333]
    if (pt[0].is_number() && pt[1].is_number()) {
        outLat = pt[0].get<double>();
        outLon = pt[1].get<double>();
        return true;
    }

    // New compact copy/paste format: ["DDMMSS", "DDDMMSS"]
    if (pt[0].is_string() && pt[1].is_string()) {
        const std::string latText = pt[0].get<std::string>();
        const std::string lonText = pt[1].get<std::string>();
        return TryParseCompactDmsCoordinate(latText, false, outLat) &&
            TryParseCompactDmsCoordinate(lonText, true, outLon);
    }

    return false;
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
        int skippedPoints = 0;
        if (v.contains("polygon") && v["polygon"].is_array()) {
            for (const auto& pt : v["polygon"]) {
                double lat = 0.0;
                double lon = 0.0;
                if (TryReadVolumePointLL(pt, lat, lon)) {
                    // Store internally as decimal degrees. The predicted route logic already uses
                    // decimal lat/lon, so no changes are needed in the matcher.
                    cv.polygon.push_back(std::make_pair(lat, lon));
                }
                else {
                    ++skippedPoints;
                }
            }
        }

        if (skippedPoints > 0) {
            char warn[256];
            sprintf_s(warn, sizeof(warn), "Volume %s skipped %d invalid coordinate point(s)", cv.id.c_str(), skippedPoints);
            DisplayUserMessage("LOA Plugin", "Volumes", warn, true, true, false, false, false);
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
    validLoaEntryPtrs.clear();

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

    // Rebuild O(1) pointer-validity set now that all entry vectors are final
    validLoaEntryPtrs.clear();
    for (const auto& e : destinationLoas)         validLoaEntryPtrs.insert(&e);
    for (const auto& e : departureLoas)           validLoaEntryPtrs.insert(&e);
    for (const auto& e : destinationFallbackLoas) validLoaEntryPtrs.insert(&e);
    for (const auto& e : departureFallbackLoas)   validLoaEntryPtrs.insert(&e);

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

    auto itTime = routeCacheTime.find(callsign);
    if (itTime != routeCacheTime.end() && now - itTime->second < 5000) {
        auto itPts = routeCache.find(callsign);
        if (itPts != routeCache.end()) return itPts->second;
    }

    auto route = fp.GetExtractedRoute();
    std::vector<std::string> routePoints;
    for (int i = 0; i < route.GetPointsNumber(); ++i)
        routePoints.emplace_back(route.GetPointName(i));

    routeCacheTime[callsign] = now;
    auto& resultPts = routeCache[callsign];
    resultPts = std::move(routePoints);
    return resultPts;
}

bool LOAPlugin::IsLoaEntryPointerValid(const LOAEntry* entry) const
{
    return entry && validLoaEntryPtrs.count(entry) > 0;
}

void LOAPlugin::CleanupCache(const std::string& callsign) {
    matchedLOACache.erase(callsign);
    routeCache.erase(callsign);
    routeCacheTime.erase(callsign);
    routeSetCache.erase(callsign);
    routeSetCacheTime.erase(callsign);
    coordinationStates.erase(callsign);
    matchTimestamps.erase(callsign);
    matchVersions.erase(callsign);
    lastDestinationByCallsign.erase(callsign);

    if (_stricmp(currentFrameCallsign.c_str(), callsign.c_str()) == 0) {
        currentFrameMatchedEntry = nullptr;
        currentFrameCallsign.clear();
        currentFrameRoutePoints.clear();
        currentFrameRouteSet.clear();
        currentFrameTimestamp = 0;
    }
}

void LOAPlugin::PrunePerformanceCaches(ULONGLONG nowMs)
{
    // Keep long sessions stable even if many callsigns come and go.
    // These caches are only accelerators; clearing old entries does not remove plugin features.
    const ULONGLONG routeTtlMs = 60000ULL;
    const ULONGLONG matchTtlMs = 60000ULL;

    for (auto it = routeCacheTime.begin(); it != routeCacheTime.end(); ) {
        if (nowMs - it->second > routeTtlMs) {
            const std::string cs = it->first;
            it = routeCacheTime.erase(it);
            routeCache.erase(cs);
            routeSetCache.erase(cs);
            routeSetCacheTime.erase(cs);
            routeSignature.erase(cs);
            lastDestinationByCallsign.erase(cs);
        }
        else {
            ++it;
        }
    }

    for (auto it = matchTimestamps.begin(); it != matchTimestamps.end(); ) {
        if (nowMs - it->second > matchTtlMs) {
            const std::string cs = it->first;
            it = matchTimestamps.erase(it);
            matchedLOACache.erase(cs);
            matchVersions.erase(cs);
        }
        else {
            ++it;
        }
    }

    // Render cache keys are callsign:itemCode. If it grows unexpectedly, clear it;
    // the next render will rebuild values immediately.
    if (renderCache.size() > 2000) {
        renderCache.clear();
    }
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

    std::vector<std::string> checkSectors;
    auto ownIt = sectorOwnership.find(mySector);
    if (ownIt != sectorOwnership.end()) checkSectors = ownIt->second;
    checkSectors.push_back(mySector);

    // Snapshot only the per-sector resolved strings (avoids copying the full online set)
    std::vector<std::string> oldResolved;
    oldResolved.reserve(checkSectors.size());
    for (const auto& s : checkSectors)
        oldResolved.push_back(ResolveControllingSector(s, currentFrameOnlineControllers));

    currentFrameOnlineControllers = GetOnlineControllersCached();

    bool changed = false;
    for (size_t i = 0; i < checkSectors.size(); ++i) {
        const std::string newCtrl = ResolveControllingSector(checkSectors[i], currentFrameOnlineControllers);
        if (_stricmp(newCtrl.c_str(), oldResolved[i].c_str()) != 0) {
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

        // Bulk-clear the caches not already cleared above (replaces per-aircraft CleanupCache loop)
        routeSetCache.clear();
        routeSetCacheTime.clear();
        lastDestinationByCallsign.clear();
        currentFrameMatchedEntry = nullptr;
        currentFrameCallsign.clear();
        currentFrameRoutePoints.clear();
        currentFrameRouteSet.clear();
        currentFrameTimestamp = 0;
        currentFrameRenderData = PerAircraftFrameData{};

        reloading = false;
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
                std::string sectorId;       // displayed sector ID
                std::string targetCallsign; // actual controller callsign used by InitiateHandoff()
                std::string freqStr;        // "123.450"
                bool isLoa = false;         // true = LOA seeded, false = EuroScope predicted
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

                // Single pass over all controllers to build posId -> (callsign, freq) map
                std::unordered_map<std::string, std::pair<std::string, std::string>> controllerInfoByPosId;
                for (EuroScopePlugIn::CController c = ControllerSelectFirst(); c.IsValid(); c = ControllerSelectNext(c)) {
                    if (!c.IsController()) continue;
                    char fbuf[16] = {};
                    _snprintf_s(fbuf, sizeof(fbuf), _TRUNCATE, "%.3f", c.GetPrimaryFrequency());
                    controllerInfoByPosId[c.GetPositionId()] = { c.GetCallsign(), std::string(fbuf) };
                }

                for (size_t i = 0; i < hybridList.size(); ++i) {
                    const auto& cid = hybridList[i];
                    const bool isLoaSeeded = (i < loaCount);
                    if (online.count(cid) == 0)
                        continue;

                    auto ctIt = controllerInfoByPosId.find(cid);
                    if (ctIt != controllerInfoByPosId.end()) {
                        options.push_back({ cid, ctIt->second.first, ctIt->second.second, isLoaSeeded });
                    }
                }
            }

            // ----------------- 2) Nothing at all? -----------------
            // If we have neither sectors nor ASSUME nor RELEASE -> no menu
            if (options.empty() && !canShowAssume && !canShowRelease)
            {
                return;
            }

            // ----------------- 3) Build custom TopSky-style popup rows -----------------
            // Structure:
            //   Section 1: Callsign + Transfer Menu header
            //              + ASSUME  (when not yet tracking)
            //              + ReleaseBar F/T/C/D  (when ASSUMED or TRANSFER_FROM_ME_INITIATED)
            //   Separator
            //   Section 2: Sector ID buttons
            std::vector<CustomHandoffRow> rows;
            const std::string callsign = fp.GetCallsign();

            // Show ReleaseBar (F T C D) when I own the track, ASSUME otherwise.
            const bool showReleaseBar =
                state == EuroScopePlugIn::FLIGHT_PLAN_STATE_ASSUMED ||
                state == EuroScopePlugIn::FLIGHT_PLAN_STATE_TRANSFER_FROM_ME_INITIATED;

            if (showReleaseBar) {
                CustomHandoffRow r;
                r.action = CustomHandoffAction::ReleaseBar;
                r.callsign = callsign;
                rows.push_back(r);
            }

            // In TRANSFER_FROM_ME_INITIATED also show ASSUME so the controller
            // can take the tag back if they handed off to the wrong sector.
            const bool showAssume =
                canShowAssume ||
                state == EuroScopePlugIn::FLIGHT_PLAN_STATE_TRANSFER_FROM_ME_INITIATED;

            if (!showReleaseBar && showAssume) {
                CustomHandoffRow r;
                r.action = CustomHandoffAction::Assume;
                r.label = "ASSUME";
                r.callsign = callsign;
                rows.push_back(r);
            }
            else if (showReleaseBar && state == EuroScopePlugIn::FLIGHT_PLAN_STATE_TRANSFER_FROM_ME_INITIATED) {
                // Separator between ReleaseBar and ASSUME
                CustomHandoffRow sep;
                sep.action = CustomHandoffAction::Separator;
                sep.callsign = callsign;
                sep.disabled = true;
                rows.push_back(sep);

                CustomHandoffRow r;
                r.action = CustomHandoffAction::Assume;
                r.label = "ASSUME";
                r.callsign = callsign;
                rows.push_back(r);
            }

            if (!options.empty()) {
                // Divider between Section 1 and Section 2
                CustomHandoffRow sep;
                sep.action = CustomHandoffAction::Separator;
                sep.callsign = callsign;
                sep.disabled = true;
                rows.push_back(sep);
            }

            // Display only the next 5 sector options.
            const size_t maxSectorRows = 5;
            size_t sectorRowsAdded = 0;

            for (const auto& opt : options) {
                if (sectorRowsAdded >= maxSectorRows)
                    break;

                CustomHandoffRow r;
                r.action = CustomHandoffAction::Sector;
                r.sectorId = opt.sectorId;
                r.targetCallsign = opt.targetCallsign;
                r.callsign = callsign;
                r.frequency = opt.freqStr;

                // Display LOA-matched sectors with a leading asterisk.
                r.label = opt.isLoa
                    ? ("*" + opt.sectorId + "*")
                    : opt.sectorId;

                rows.push_back(r);
                ++sectorRowsAdded;
            }

            if (canShowRelease) {
                // Divider above FREE
                CustomHandoffRow sep;
                sep.action = CustomHandoffAction::Separator;
                sep.callsign = callsign;
                sep.disabled = true;
                rows.push_back(sep);

                CustomHandoffRow r;
                r.action = CustomHandoffAction::Release;
                r.label = "FREE";
                r.callsign = callsign;
                rows.push_back(r);
            }

            ShowCustomHandoffPopup(Pt, rows);
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

            // Strip LOA marker prefix if present ('*' for LOA).
            // Predicted sectors use a leading space only; that was already trimmed above.
            if (!line.empty() && line[0] == '*') {
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
    int clearedAltitude = flightPlan.GetClearedAltitude();
    int finalAltitude = flightPlan.GetFinalAltitude();

    // ---------- MICRO-CACHE: reuse last render for this callsign+itemCode (<= 750–1000 ms) ----------
    ULONGLONG now = GetTickCount64();

    static ULONGLONG lastCachePruneMs = 0;
    if (now - lastCachePruneMs > 30000ULL) {
        plugin.PrunePerformanceCaches(now);
        lastCachePruneMs = now;
    }

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

        // Poll once per current-frame refresh instead of inside every matcher call.
        // The poll remains throttled internally, but keeping it here avoids sector-file scans
        // from being triggered by secondary tag render paths.
        plugin.PollActiveRunwaysIfNeeded();

        plugin.currentFrameRoutePoints = plugin.GetCachedRoutePoints(flightPlan);

        // Detect FP edits (origin/dest/route) and invalidate per-callsign caches immediately.
        {
            const auto& fpd = flightPlan.GetFlightPlanData();
            plugin.currentFrameRenderData.origin      = fpd.GetOrigin();
            plugin.currentFrameRenderData.destination = fpd.GetDestination();

            auto itDest = lastDestinationByCallsign.find(callsign);
            const bool destinationChanged = (itDest != lastDestinationByCallsign.end() &&
                _stricmp(itDest->second.c_str(), plugin.currentFrameRenderData.destination.c_str()) != 0);
            lastDestinationByCallsign[callsign] = plugin.currentFrameRenderData.destination;

            // Incremental FNV-1a hash (avoid building a large concatenated string)
            unsigned long long h = 1469598103934665603ULL;
            auto fnv_feed = [&](const std::string& s) {
                for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
                };
            fnv_feed(plugin.currentFrameRenderData.origin);
            fnv_feed("|");
            fnv_feed(plugin.currentFrameRenderData.destination);
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
        if (plugin.currentFrameMatchedEntry && !plugin.IsLoaEntryPointerValid(plugin.currentFrameMatchedEntry)) {
            plugin.currentFrameMatchedEntry = nullptr;
        }
    }

    plugin.currentFrameRenderData.callsign        = callsign;
    plugin.currentFrameRenderData.clearedAltitude = clearedAltitude;
    plugin.currentFrameRenderData.finalAltitude   = finalAltitude;
    plugin.currentFrameRenderData.matchedEntry    = plugin.currentFrameMatchedEntry;

    // ---- Render selected tag item
    switch (itemCode)
    {
    case 1996:
        RenderXFLTagItem(flightPlan, radarTarget, tagData, sItemString, pColorCode, pRGB, pFontSize, plugin.currentFrameRenderData);
        break;

    case 2000:
        RenderXFLDetailedTagItem(flightPlan, radarTarget, tagData, sItemString, pColorCode, pRGB, pFontSize, plugin.currentFrameRenderData);
        break;

    case 1997:
        RenderCOPTagItem(flightPlan, radarTarget, tagData, sItemString, pColorCode, pRGB, pFontSize, plugin.currentFrameRenderData);
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

    auto itTs = routeSetCacheTime.find(callsign);
    if (itTs != routeSetCacheTime.end() && now - itTs->second < 5000) {
        auto itSet = routeSetCache.find(callsign);
        if (itSet != routeSetCache.end()) return itSet->second;
    }

    const auto& pts = GetCachedRoutePoints(fp);

    std::unordered_set<std::string> s;
    s.reserve(pts.size() * 2 + 4);

    for (const auto& p0 : pts) {
        std::string p = p0;
        std::transform(p.begin(), p.end(), p.begin(), ::tolower);
        s.insert(std::move(p));
    }

    routeSetCacheTime[callsign] = now;
    auto& resultSet = routeSetCache[callsign];
    resultSet = std::move(s);
    return resultSet;
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