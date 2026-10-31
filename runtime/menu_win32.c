/* Menu bar for the game window on Windows (see menu.h). */
#include "menu.h"
#include "controls.h"
#include "i18n.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>                                        /* DROPFILES (a file on the clipboard) */
#include <commdlg.h>                                       /* Open / Save As */
#include "SDL_syswm.h"
#include <string.h>
#include <wchar.h>
#include <stdio.h>
#include <stdlib.h>

static HWND g_hwnd;
static HMENU g_bar;

/* The English text of a menu entry, in the current language, as UTF-16. An entry may end
 * in a tab and its key ("&Pause\tP"): the key is kept, and a translation is looked up for
 * the whole text first, then for the part before the tab. */
static const wchar_t* menu_text(const char* en) {
    static wchar_t buf[8][512];
    static int next;
    wchar_t* w = buf[next++ & 7];
    char joined[600];
    const char* t = tr(en);
    if (t == en && strchr(en, '\t')) {
        const char* tab = strchr(en, '\t');
        char left[300], right[300];
        snprintf(left, sizeof left, "%.*s", (int)(tab - en), en);
        snprintf(right, sizeof right, "%s", tab + 1);
        snprintf(joined, sizeof joined, "%s\t%s", tr(left), tr(right));
        t = joined;
    }
    if (!MultiByteToWideChar(CP_UTF8, 0, t, -1, w, 512)) w[0] = 0;
    return w;
}
/* The same for dialogs: W("Save") */
#define W(en) menu_text(en)

static void item(HMENU m, int id, const char* text) { AppendMenuW(m, MF_STRING, (UINT_PTR)id, menu_text(text)); }
static void sep(HMENU m) { AppendMenuW(m, MF_SEPARATOR, 0, NULL); }
static HMENU sub(HMENU parent, const char* text) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(parent, MF_POPUP, (UINT_PTR)m, menu_text(text));
    return m;
}

/* The names the menu was built with (the console colours), kept for rebuilding it. */
static char g_shell_names[64][64];
static int g_n_shells;
static int g_shown = 1;                       /* the bar is on the window (it is off in fullscreen) */

static void build_menu(void) {
    g_bar = CreateMenu();

    HMENU game = sub(g_bar, "&Game");
    item(game, MI_PAUSE, "&Pause\tP");
    item(game, MI_TURBO, "&Fast forward\tTab");
    HMENU spd = sub(game, "Spee&d of the fast forward");
    item(spd, MI_SPEED0, "2x");
    item(spd, MI_SPEED0 + 1, "4x");
    item(spd, MI_SPEED0 + 2, "&Unlimited");
    item(game, MI_RESET, "&Reset...\tCtrl+R");
    sep(game);
    item(game, MI_VOLUME, "&Volume...\t- / =");
    item(game, MI_MUTE, "&Mute");
    item(game, MI_CONTROLS_EDIT, "&Controls...");
    HMENU lang = sub(game, "&Language (European version)");      /* (here, not on the bar: a wider bar wraps in a small window) */
    AppendMenuW(lang, MF_STRING, (UINT_PTR)(MI_LANG1), L"&English");
    AppendMenuW(lang, MF_STRING, (UINT_PTR)(MI_LANG1 + 1), L"&Fran\u00e7ais");
    AppendMenuW(lang, MF_STRING, (UINT_PTR)(MI_LANG1 + 2), L"&Deutsch");
    AppendMenuW(lang, MF_STRING, (UINT_PTR)(MI_LANG1 + 3), L"&Italiano");
    AppendMenuW(lang, MF_STRING, (UINT_PTR)(MI_LANG1 + 4), L"E&spa\u00f1ol");
    sep(game);
    HMENU sf = sub(game, "Save &file");
    item(sf, MI_SAVE_EXPORT, "&Export your save (a copy)...");
    item(sf, MI_SAVE_IMPORT, "&Import a save (from another copy or an emulator)...");
    sep(game);
    item(game, MI_SHOT, "Save &screenshot\tF12");
    item(game, MI_REPORT, "Save &bug report\tF10");
    sep(game);
    item(game, MI_EXIT, "E&xit\tEsc");

    HMENU gfx = sub(g_bar, "G&raphics");
    item(gfx, MI_CLASSIC, "&Classic: exactly like the DS\tF5");
    item(gfx, MI_ENHANCED, "&Enhanced: sharper 3D\tF5");
    sep(gfx);
    HMENU res = sub(gfx, "3D &resolution (Enhanced)\tF4");
    item(res, MI_RES1, "1x (DS)");
    item(res, MI_RES2, "2x");
    item(res, MI_RES3, "3x");
    item(res, MI_RES4, "4x");
    item(res, MI_RES6, "6x");
    item(res, MI_RES8, "8x");
    sep(res);
    item(res, MI_RES_AUTO, "&Auto: as sharp as your screen (up to 12x)");
    item(gfx, MI_SMOOTH, "&Smooth textures (Enhanced)");
    HMENU ol = sub(gfx, "Character &outlines (Enhanced)");
    item(ol, MI_OUTLINE0, "&Thin (clean, like emulators at a high resolution)");
    item(ol, MI_OUTLINE0 + 1, "&Medium");
    item(ol, MI_OUTLINE0 + 2, "Like the &DS (thicker, follows the polygons)");
    item(gfx, MI_HD_TEXT, "&HD text: the letters at full resolution (Enhanced, new)\tF1");
    item(gfx, MI_SOFT_SHADOWS, "Soft s&hadows and glows (Enhanced)");

    HMENU scr = sub(g_bar, "&Screen");
    item(scr, MI_LCD, "&DS Lite screen\tF6");
    sep(scr);
    item(scr, MI_LCDCOL, "LCD &colours");
    item(scr, MI_GRID, "&Pixel grid (at 3x and larger)");
    item(scr, MI_GHOST, "&Motion blur");
    HMENU bl = sub(scr, "&Backlight");
    item(bl, MI_BL1, "&Low");
    item(bl, MI_BL2, "&Medium");
    item(bl, MI_BL3, "&High");
    item(bl, MI_BL4, "Ma&x");

    HMENU view = sub(g_bar, "&View");
    item(view, MI_SHELL, "Show the DS &Lite\tF7");
    HMENU col = sub(view, "Console &colour");
    for (int i = 0; i < g_n_shells; i++) item(col, MI_SHELLC0 + i, g_shell_names[i]);
    item(view, MI_GAP, "&Gap between the screens");
    item(view, MI_ACTUAL, "&Actual size (as big as a real DS Lite)\tF8");
    sep(view);
    item(view, MI_STACKED, "S&tacked\tF2");
    item(view, MI_SIDE, "&Side by side\tF2");
    item(view, MI_CORNER, "Top screen in the &corner\tF2");
    item(view, MI_WIDE, "&Basic Widescreen (the action wide, the other screen in a corner)\tF2");
    item(view, MI_WIDE_DYN, "D&ynamic Widescreen (Experimental): the map stays in the corner, the game's windows move right\tF2");
    item(view, MI_TWO, "Two wi&ndows (one screen in each, for two monitors)\tF2");
    sep(view);
    item(view, MI_SINGLE, "Single scr&een (widescreen: hide the small screen)\tU");
    item(view, MI_SWAP, "Swa&p the screens (widescreen: hold to see the other)\tF9");
    HMENU sm = sub(view, "Widescreen: the small scr&een");
    item(sm, MI_CORNER0, "Top left\tK");
    item(sm, MI_CORNER0 + 1, "Top right\tK");
    item(sm, MI_CORNER0 + 2, "Bottom right\tK");
    item(sm, MI_CORNER0 + 3, "Bottom left\tK");
    sep(sm);
    item(sm, MI_SSIZE0, "Small\tI");
    item(sm, MI_SSIZE0 + 1, "Medium\tI");
    item(sm, MI_SSIZE0 + 2, "Large\tI");
    sep(sm);
    item(sm, MI_OPACITY1, "100% (solid)\tO");
    item(sm, MI_OPACITY1 + 1, "75%\tO");
    item(sm, MI_OPACITY1 + 2, "50%\tO");
    item(sm, MI_OPACITY1 + 3, "25%\tO");
    sep(sm);
    item(sm, MI_POPOUT, "In its own &window (a clean field)");
    sep(view);
    HMENU size = sub(view, "&Window size\tF3");
    item(size, MI_SIZE1, "1x");
    item(size, MI_SIZE2, "2x");
    item(size, MI_SIZE3, "3x");
    item(size, MI_SIZE4, "4x");
    item(view, MI_INTEGER, "&Integer scaling");
    item(view, MI_FULL, "&Fullscreen\tF11");

    HMENU wl = sub(g_bar, "&Wireless");
    item(wl, MI_NET, "&Connect (Nintendo Wi-Fi Connection replacement)");
    HMENU srv = sub(wl, "&Server");
    item(srv, MI_SRV_BUILTIN, "&Built-in (offline: DQIX's preserved downloads)");
    sep(srv);
    item(srv, MI_SRV_ALTWFC, "&AltWFC (172.104.88.237)");
    item(srv, MI_SRV_WIIMMFI, "&Wiimmfi (164.132.44.106): not for DQIX");
    item(srv, MI_SRV_CUSTOM, "&Custom (wfc_server in settings.ini)");
    sep(wl);
    HMENU friends = sub(wl, "&Play with friends far away (tag mode, visiting)");
    item(friends, MI_SES_HOST, "&Host: your friends join you...");
    item(friends, MI_SES_JOIN, "&Join a friend...");
    item(friends, MI_SES_START, "&Start now (everyone is here)\tEnter");
    item(friends, MI_SES_LEAVE, "&Leave and play alone");
    sep(friends);
    item(friends, MI_SES_NAME, "Your &name...");
    item(friends, MI_SES_HELP, "&How does this work?");
    sep(wl);
    item(wl, MI_DLC, "&Unlock all downloadable content now (without Sellma)");
    sep(wl);
    item(wl, MI_NET_HELP, "&How does this work?");

    HMENU modern = sub(g_bar, "&Modern");
    item(modern, MI_ANALOG, "Walk in any &direction with the left stick");
    item(modern, MI_WIDE, "&Basic Widescreen: the action wide, the other screen in a corner");
    item(modern, MI_WIDE_DYN, "D&ynamic Widescreen (Experimental)");
    item(modern, MI_CORNER, "Top screen in the &corner (the field as on the DS)");
    item(modern, MI_POPOUT, "Widescreen: the small screen in its own &window (a clean field)");
    item(modern, MI_SCENE_BARS, "Widescreen: &black sides in this place (remembered)");
    item(modern, MI_HUD, "Widescreen: battle &HUD over the 3D (experimental: the party's panels in the bottom left)");
    sep(modern);
    item(modern, MI_MODERN_HELP, "&What is this?");
    HMENU mods = sub(g_bar, "M&ods");
    item(mods, MI_TEXPACK, "&Texture packs (the pictures in the textures folder)");
    item(mods, MI_TEXUP, "&Upscale the 3D textures (xBR: smooth slopes and curves)");
    item(mods, MI_TEXRELOAD, "&Reload the texture packs");
    item(mods, MI_TEXFOLDER, "&Open the textures folder");
    sep(mods);
    item(mods, MI_TEXDUMP, "&Dump textures (to make a pack: every texture shown goes to textures\\dump)");
    sep(mods);
    item(mods, MI_MUSPACK, "&Music packs (the recordings in the music folder)");
    item(mods, MI_MUSRELOAD, "R&eload the music");
    item(mods, MI_MUSFOLDER, "O&pen the music folder (and its list of the songs)");
    item(mods, MI_MUSNAMES, "&Show each song's name as it starts");
    item(mods, MI_MUSEXPORT, "E&xport the game's songs (WAV files with their loops, to music\\export: about 650 MB)");
    sep(mods);
    item(mods, MI_MODS_HELP, "&What is this?");
    HMENU ui = sub(g_bar, "I&nterface language / Idioma");
    for (int i = 0; i < i18n_count(); i++) {
        wchar_t w[64];
        MultiByteToWideChar(CP_UTF8, 0, i18n_name(i), -1, w, 64);
        AppendMenuW(ui, MF_STRING, (UINT_PTR)(MI_UILANG0 + i), w);
    }
    HMENU help = sub(g_bar, "&Help");
    item(help, MI_CONTROLS, "&Controls");
    item(help, MI_UPDATE, "Check for &updates...");
    item(help, MI_UPDATE_AUTO, "Check for updates at &start");
    sep(help);
    item(help, MI_SEND_REPORT, "&Send a bug report...");
    item(help, MI_ABOUT, "&About");

}

void menu_init(SDL_Window* win, const char* const* shell_names, int n_shells) {
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(win, &info) || info.subsystem != SDL_SYSWM_WINDOWS) return;
    g_hwnd = info.info.win.window;
    g_n_shells = n_shells < 64 ? n_shells : 64;
    for (int i = 0; i < g_n_shells; i++) snprintf(g_shell_names[i], sizeof g_shell_names[i], "%s", shell_names[i]);
    build_menu();
    SetMenu(g_hwnd, g_bar);
    SDL_EventState(SDL_SYSWMEVENT, SDL_ENABLE);
}

void menu_show(int on) {
    g_shown = on;
    if (!g_hwnd) return;
    if (on) menu_strip(0);                                       /* a menu belongs to one window at a time */
    SetMenu(g_hwnd, on ? g_bar : NULL);
}

static HWND g_strip;
static int g_strip_menu;                                         /* one of its menus is open */
/* The strip takes the keyboard while it shows (a window that never activates
 * can't open its menus, and Windows draws their names greyed), and gives it
 * back to the game when it hides or a menu closes. */
static void strip_give_back(void) { if (g_hwnd) SetForegroundWindow(g_hwnd); }
static LRESULT CALLBACK strip_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:                                             /* the game's window handles the menu's commands */
        if (g_hwnd && HIWORD(wp) == 0) PostMessageW(g_hwnd, WM_COMMAND, wp, lp);
        return 0;
    case WM_ENTERMENULOOP: g_strip_menu = 1; break;
    case WM_EXITMENULOOP: {
        g_strip_menu = 0;
        POINT pt; RECT r;                                        /* the mouse went down into the game: the strip goes */
        if (GetCursorPos(&pt) && GetWindowRect(h, &r) && pt.y > r.bottom + 40) { ShowWindow(h, SW_HIDE); SetMenu(h, NULL); }
        strip_give_back();
        break;
    }
    case WM_CLOSE: return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
int menu_strip_shown(void) { return g_strip && IsWindowVisible(g_strip); }
void menu_strip(int on) {
    if (!g_hwnd || !g_bar) return;
    if (!on) {
        if (g_strip_menu) return;                                /* not while one of its menus is open */
        if (g_strip && IsWindowVisible(g_strip)) { ShowWindow(g_strip, SW_HIDE); SetMenu(g_strip, NULL); strip_give_back(); }
        return;
    }
    if (!g_strip) {
        WNDCLASSW wc; memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = strip_proc; wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = L"DQIXMenuStrip"; wc.hbrBackground = (HBRUSH)(COLOR_MENUBAR + 1);
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        RegisterClassW(&wc);
        g_strip = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"", WS_POPUP, 0, 0, 10, 10, g_hwnd, NULL, wc.hInstance, NULL);
        if (!g_strip) return;
    }
    SetMenu(g_hwnd, NULL);
    SetMenu(g_strip, g_bar);
    HMONITOR mon = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi; mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(mon, &mi)) return;
    int w = mi.rcMonitor.right - mi.rcMonitor.left;
    /* as tall as the menu bar (at this monitor's scale) plus a pixel */
    SetWindowPos(g_strip, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, w, 40, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    MENUBARINFO mb; mb.cbSize = sizeof mb;
    if (GetMenuBarInfo(g_strip, OBJID_MENU, 0, &mb)) {
        RECT wr; GetWindowRect(g_strip, &wr);
        int h = mb.rcBar.bottom - wr.top + 1;
        if (h > 4 && h < 200) SetWindowPos(g_strip, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, w, h, SWP_NOACTIVATE);
    }
    DrawMenuBar(g_strip);
    SetForegroundWindow(g_strip);                                /* its menus open, and aren't drawn greyed */
}

int file_dialog(SDL_Window* win, int save, const char* title, const char* filter, const char* default_name, char* out, size_t n) {
    SDL_SysWMinfo info; SDL_VERSION(&info.version);
    HWND owner = win && SDL_GetWindowWMInfo(win, &info) && info.subsystem == SDL_SYSWM_WINDOWS ? info.info.win.window : NULL;
    wchar_t wtitle[200], wfilter[400], file[MAX_PATH] = L"";
    MultiByteToWideChar(CP_UTF8, 0, title, -1, wtitle, 200);
    int fl = MultiByteToWideChar(CP_UTF8, 0, filter, -1, wfilter, 398);
    if (fl <= 0) return 0;
    for (int i = 0; i < fl; i++) if (wfilter[i] == L'|') wfilter[i] = 0;           /* "a|b|c|d" -> "a\0b\0c\0d\0\0" */
    wfilter[fl] = 0;
    if (default_name) MultiByteToWideChar(CP_UTF8, 0, default_name, -1, file, MAX_PATH);
    OPENFILENAMEW o; memset(&o, 0, sizeof o);
    o.lStructSize = sizeof o; o.hwndOwner = owner; o.lpstrFilter = wfilter; o.lpstrFile = file; o.nMaxFile = MAX_PATH;
    o.lpstrTitle = wtitle; o.lpstrDefExt = L"sav";
    o.Flags = OFN_NOCHANGEDIR | (save ? OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST : OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST);
    if (!(save ? GetSaveFileNameW(&o) : GetOpenFileNameW(&o))) return 0;
    return WideCharToMultiByte(CP_UTF8, 0, file, -1, out, (int)n, NULL, NULL) > 0;
}

int clipboard_set_file(const char* path) {
    wchar_t w[MAX_PATH], full[MAX_PATH];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, w, MAX_PATH)) return 0;
    for (wchar_t* p = w; *p; p++) if (*p == L'/') *p = L'\\';
    DWORD n = GetFullPathNameW(w, MAX_PATH, full, NULL);
    if (!n || n >= MAX_PATH) return 0;
    size_t bytes = sizeof(DROPFILES) + (n + 2) * sizeof(wchar_t);  /* the list ends with two NULs */
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (!g) return 0;
    DROPFILES* df = (DROPFILES*)GlobalLock(g);
    df->pFiles = sizeof(DROPFILES); df->fWide = TRUE;
    memcpy((char*)df + sizeof(DROPFILES), full, n * sizeof(wchar_t));
    GlobalUnlock(g);
    if (!OpenClipboard(g_hwnd)) { GlobalFree(g); return 0; }
    EmptyClipboard();
    int ok = SetClipboardData(CF_HDROP, g) != NULL;
    if (!ok) GlobalFree(g);
    UINT fmt = RegisterClipboardFormatW(L"Preferred DropEffect");      /* a copy, not a move */
    HGLOBAL e = fmt ? GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD)) : NULL;
    if (e) { *(DWORD*)GlobalLock(e) = 1 /* DROPEFFECT_COPY */; GlobalUnlock(e); if (!SetClipboardData(fmt, e)) GlobalFree(e); }
    CloseClipboard();
    return ok;
}

void window_fix_frame(SDL_Window* win) {
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!win || !SDL_GetWindowWMInfo(win, &info) || info.subsystem != SDL_SYSWM_WINDOWS) return;
    HWND h = info.info.win.window;
    LONG_PTR st = GetWindowLongPtrW(h, GWL_STYLE);
    LONG_PTR want = (st & ~(LONG_PTR)WS_POPUP) | WS_OVERLAPPEDWINDOW;
    if (st != want) {
        SetWindowLongPtrW(h, GWL_STYLE, want);
        SetWindowPos(h, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

/* The second window (two windows): a bar with one command, fullscreen. */
static HWND g_hwnd2;
static HMENU g_bar2;
static int g_shown2 = 1;
static HMENU make_bar2(void) { HMENU m = CreateMenu(); item(m, MI_FULL2, "&Fullscreen (F11)"); return m; }
void menu_init_second(SDL_Window* win) {
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    g_hwnd2 = NULL;
    if (!win || !SDL_GetWindowWMInfo(win, &info) || info.subsystem != SDL_SYSWM_WINDOWS) return;
    g_hwnd2 = info.info.win.window;
    if (!g_bar2) g_bar2 = make_bar2();
    SetMenu(g_hwnd2, g_bar2);
    g_shown2 = 1;
    SDL_EventState(SDL_SYSWMEVENT, SDL_ENABLE);
}
void menu_show_second(int on) {
    g_shown2 = on;
    if (g_hwnd2 && IsWindow(g_hwnd2)) SetMenu(g_hwnd2, on ? g_bar2 : NULL);
}

/* The language changed: build the bars again with the new texts. The frontend then calls menu_sync()
 * to put the check marks back. The bar may be on the window or on the fullscreen strip. */
void menu_rebuild(void) {
    if (g_hwnd && g_bar) {
        HMENU old = g_bar;
        build_menu();
        if (g_strip && IsWindowVisible(g_strip)) { SetMenu(g_strip, g_bar); DrawMenuBar(g_strip); }
        else if (g_shown) { SetMenu(g_hwnd, g_bar); DrawMenuBar(g_hwnd); }
        DestroyMenu(old);
    }
    if (g_bar2) {
        HMENU old = g_bar2;
        g_bar2 = make_bar2();
        if (g_hwnd2 && IsWindow(g_hwnd2) && g_shown2) { SetMenu(g_hwnd2, g_bar2); DrawMenuBar(g_hwnd2); }
        DestroyMenu(old);
    }
}

void menu_check(int id, int on) {
    if (g_bar) CheckMenuItem(g_bar, (UINT)id, MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
}

void menu_enable(int id, int on) {
    if (g_bar) EnableMenuItem(g_bar, (UINT)id, MF_BYCOMMAND | (on ? MF_ENABLED : MF_GRAYED));
}

int menu_command(const SDL_Event* e) {
    if (e->type != SDL_SYSWMEVENT || (!g_bar && !g_bar2)) return 0;
    const SDL_SysWMmsg* m = e->syswm.msg;
    if (m->subsystem != SDL_SYSWM_WINDOWS || m->msg.win.msg != WM_COMMAND || HIWORD(m->msg.win.wParam) != 0) return 0;
    return LOWORD(m->msg.win.wParam);
}

/* The monitor's EDID (in the registry under its device id) holds its
 * physical size: the first detailed timing block in mm, else the basic
 * size in cm. Divided by the monitor's width in pixels. */
float display_mm_per_pixel(SDL_Window* win) {
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(win, &info) || info.subsystem != SDL_SYSWM_WINDOWS) return 0;
    HMONITOR mon = MonitorFromWindow(info.info.win.window, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW mi; mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(mon, (MONITORINFO*)&mi)) return 0;
    DEVMODEW dm; memset(&dm, 0, sizeof dm); dm.dmSize = sizeof dm;
    if (!EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) || !dm.dmPelsWidth) return 0;
    int px_w = (int)dm.dmPelsWidth, px_h = (int)dm.dmPelsHeight;
    DISPLAY_DEVICEW dd; dd.cb = sizeof dd;
    for (DWORD k = 0; EnumDisplayDevicesW(mi.szDevice, k, &dd, EDD_GET_DEVICE_INTERFACE_NAME); k++) {
        if (!(dd.StateFlags & DISPLAY_DEVICE_ACTIVE)) continue;
        /* \\?\DISPLAY#<model>#<instance>#{guid} -> SYSTEM\CurrentControlSet\Enum\DISPLAY\<model>\<instance> */
        wchar_t id[256]; wcsncpy(id, dd.DeviceID, 255); id[255] = 0;
        wchar_t* parts[4] = { 0 }; int n = 0;
        for (wchar_t *ctx = NULL, *t = wcstok(id, L"#", &ctx); t && n < 4; t = wcstok(NULL, L"#", &ctx)) parts[n++] = t;
        if (n < 3) continue;
        wchar_t key[512];
        _snwprintf(key, 511, L"SYSTEM\\CurrentControlSet\\Enum\\DISPLAY\\%ls\\%ls\\Device Parameters", parts[1], parts[2]);
        key[511] = 0;
        BYTE edid[256]; DWORD len = sizeof edid;
        if (RegGetValueW(HKEY_LOCAL_MACHINE, key, L"EDID", RRF_RT_REG_BINARY, NULL, edid, &len) != ERROR_SUCCESS || len < 128) continue;
        if (edid[0] != 0 || edid[1] != 0xFF || edid[7] != 0) continue;
        int w_mm = 0, h_mm = 0;
        if (edid[54] | edid[55]) { w_mm = edid[66] | ((edid[68] & 0xF0) << 4); h_mm = edid[67] | ((edid[68] & 0x0F) << 8); }
        if (w_mm < 100 || h_mm < 50) { w_mm = edid[21] * 10; h_mm = edid[22] * 10; }    /* cm */
        if (w_mm < 100 || h_mm < 50) continue;
        if ((px_w < px_h) != (w_mm < h_mm)) { int t = w_mm; w_mm = h_mm; h_mm = t; } /* rotated monitor */
        float mm = (float)w_mm / (float)px_w;
        if (mm > 0.05f && mm < 1.5f) return mm;
    }
    return 0;
}

/* ---------------------------------------------------------------- text box (bug descriptions)
 * A dialog built in memory: a prompt, a large multi-line edit box, and
 * Save / Skip buttons. */
#define ID_TEXT 1001
static wchar_t* g_text_out;

static INT_PTR CALLBACK text_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG:
        SendDlgItemMessageW(h, ID_TEXT, EM_SETLIMITTEXT, 1 << 20, 0);
        SetFocus(GetDlgItem(h, ID_TEXT));
        return FALSE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            int n = GetWindowTextLengthW(GetDlgItem(h, ID_TEXT));
            g_text_out = calloc((size_t)n + 1, sizeof(wchar_t));
            if (g_text_out) GetDlgItemTextW(h, ID_TEXT, g_text_out, n + 1);
            EndDialog(h, 1);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(h, 0); return TRUE; }
        break;
    }
    return FALSE;
}

static WORD* align4(WORD* p) { return (WORD*)(((ULONG_PTR)p + 3) & ~(ULONG_PTR)3); }
static WORD* put_str(WORD* p, const wchar_t* s) { do { *p++ = (WORD)*s; } while (*s++); return p; }
static WORD* put_item(WORD* p, DWORD style, short x, short y, short cx, short cy, WORD id, WORD cls, const wchar_t* text) {
    p = align4(p);
    DLGITEMTEMPLATE* it = (DLGITEMTEMPLATE*)p;
    it->style = style | WS_CHILD | WS_VISIBLE; it->dwExtendedStyle = 0;
    it->x = x; it->y = y; it->cx = cx; it->cy = cy; it->id = id;
    p = (WORD*)(it + 1);
    *p++ = 0xFFFF; *p++ = cls;                                   /* 0x80 button, 0x81 edit, 0x82 static */
    p = put_str(p, text);
    *p++ = 0;                                                    /* no creation data */
    return p;
}


/* ---------------------------------------------------------------- a message with buttons that fit */
#define CB_TEXT 2000
#define CB_ICON 2001
#define CB_CHECK 2002
#define CB_BUTTON 3000
typedef struct { int n, def, cancel, has_check, checked; } ChoiceInfo;

static INT_PTR CALLBACK choice_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    ChoiceInfo* ci = (ChoiceInfo*)GetWindowLongPtrW(h, DWLP_USER);
    switch (msg) {
    case WM_INITDIALOG: {
        ci = (ChoiceInfo*)lp;
        SetWindowLongPtrW(h, DWLP_USER, (LONG_PTR)ci);
        SendDlgItemMessageW(h, CB_ICON, STM_SETICON, (WPARAM)LoadIconW(NULL, (LPCWSTR)IDI_INFORMATION), 0);
        if (ci->has_check) CheckDlgButton(h, CB_CHECK, ci->checked ? BST_CHECKED : BST_UNCHECKED);
        /* sizes in dialog units -> pixels for this font and monitor */
        RECT u = { 7, 7, 280, 14 }; MapDialogRect(h, &u);
        int margin = u.left, textw = u.right, btnh = u.bottom;
        RECT g = { 4, 50, 0, 0 }; MapDialogRect(h, &g);
        int gap = g.left, minbtn = g.top, icon = GetSystemMetrics(SM_CXICON);
        HDC dc = GetDC(h);
        HFONT f = (HFONT)SendMessageW(h, WM_GETFONT, 0, 0);
        HGDIOBJ old = SelectObject(dc, f);
        wchar_t t[4096];
        GetDlgItemTextW(h, CB_TEXT, t, 4096);
        RECT tr = { 0, 0, textw, 0 };
        DrawTextW(dc, t, -1, &tr, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        int bw[8], total = 0;
        for (int i = 0; i < ci->n; i++) {
            wchar_t b[200]; SIZE sz;
            GetDlgItemTextW(h, CB_BUTTON + i, b, 200);
            GetTextExtentPoint32W(dc, b, (int)wcslen(b), &sz);
            bw[i] = sz.cx + 2 * margin + gap; if (bw[i] < minbtn) bw[i] = minbtn;
            total += bw[i] + (i ? gap : 0);
        }
        int checkh = 0, checkw = 0;
        if (ci->has_check) {
            wchar_t c[300]; SIZE sz; GetDlgItemTextW(h, CB_CHECK, c, 300);
            GetTextExtentPoint32W(dc, c, (int)wcslen(c), &sz);
            checkw = sz.cx + GetSystemMetrics(SM_CXMENUCHECK) + 3 * gap; checkh = btnh;
        }
        SelectObject(dc, old);
        ReleaseDC(h, dc);
        int tx = margin + icon + 2 * gap, th = tr.bottom;
        int cw = tx + tr.right + margin;
        if (cw < total + 2 * margin) cw = total + 2 * margin;
        if (ci->has_check && cw < tx + checkw + margin) cw = tx + checkw + margin;
        int y = margin;
        SetWindowPos(GetDlgItem(h, CB_ICON), NULL, margin, y, icon, icon, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(h, CB_TEXT), NULL, tx, y, cw - tx - margin, th, SWP_NOZORDER);
        y += (th > icon ? th : icon) + margin;
        if (ci->has_check) { SetWindowPos(GetDlgItem(h, CB_CHECK), NULL, tx, y, cw - tx - margin, checkh, SWP_NOZORDER); y += checkh + margin; }
        int bx = cw - margin - total;
        for (int i = 0; i < ci->n; i++) { SetWindowPos(GetDlgItem(h, CB_BUTTON + i), NULL, bx, y, bw[i], btnh, SWP_NOZORDER); bx += bw[i] + gap; }
        int ch = y + btnh + margin;
        RECT wr = { 0, 0, cw, ch };
        AdjustWindowRectEx(&wr, (DWORD)GetWindowLongW(h, GWL_STYLE), FALSE, (DWORD)GetWindowLongW(h, GWL_EXSTYLE));
        HWND parent = GetParent(h); RECT pr;
        if (!parent || !GetWindowRect(parent, &pr)) SystemParametersInfoW(SPI_GETWORKAREA, 0, &pr, 0);
        int w = wr.right - wr.left, hh = wr.bottom - wr.top;
        SetWindowPos(h, NULL, pr.left + (pr.right - pr.left - w) / 2, pr.top + (pr.bottom - pr.top - hh) / 2, w, hh, SWP_NOZORDER);
        SetFocus(GetDlgItem(h, CB_BUTTON + ci->def));
        return FALSE;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id >= CB_BUTTON && id < CB_BUTTON + ci->n) {
            if (ci->has_check) ci->checked = IsDlgButtonChecked(h, CB_CHECK) == BST_CHECKED;
            EndDialog(h, id - CB_BUTTON); return TRUE;
        }
        if (id == IDCANCEL) { if (ci->has_check) ci->checked = IsDlgButtonChecked(h, CB_CHECK) == BST_CHECKED; EndDialog(h, ci->cancel); return TRUE; }
        if (id == IDOK) { EndDialog(h, ci->def); return TRUE; }
        break;
    }
    }
    return FALSE;
}

int choice_box(SDL_Window* win, const char* title, const char* text, const char* const* buttons, int n, int def, int cancel,
               const char* check, int* checked) {
    if (n < 1 || n > 8) return -1;
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    HWND parent = NULL;
    if (win && SDL_GetWindowWMInfo(win, &info) && info.subsystem == SDL_SYSWM_WINDOWS) parent = info.info.win.window;
    static WORD buf[16384];
    memset(buf, 0, sizeof buf);
    DLGTEMPLATE* d = (DLGTEMPLATE*)align4(buf);
    d->style = DS_MODALFRAME | DS_SETFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    d->cdit = (WORD)(2 + n + (check ? 1 : 0)); d->cx = 200; d->cy = 100;
    WORD* p = (WORD*)(d + 1);
    *p++ = 0; *p++ = 0;
    static wchar_t w[8192];
    MultiByteToWideChar(CP_UTF8, 0, title, -1, w, 8192);
    p = put_str(p, w);
    *p++ = 9; p = put_str(p, L"Segoe UI");
    p = put_item(p, SS_ICON, 0, 0, 0, 0, CB_ICON, 0x82, L"");
    /* the text: \n -> \r\n for the static control */
    static wchar_t t[8192]; int k = 0;
    MultiByteToWideChar(CP_UTF8, 0, text, -1, w, 8192);
    for (int i = 0; w[i] && k < 8190; i++) { if (w[i] == L'\n') t[k++] = L'\r'; t[k++] = w[i]; }
    t[k] = 0;
    p = put_item(p, SS_NOPREFIX, 0, 0, 10, 10, CB_TEXT, 0x82, t);
    if (check) { MultiByteToWideChar(CP_UTF8, 0, check, -1, w, 8192); p = put_item(p, WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 10, 10, CB_CHECK, 0x80, w); }
    for (int i = 0; i < n; i++) {
        MultiByteToWideChar(CP_UTF8, 0, buttons[i], -1, w, 8192);
        p = put_item(p, WS_TABSTOP | (i == def ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON), 0, 0, 10, 10, (WORD)(CB_BUTTON + i), 0x80, w);
    }
    ChoiceInfo ci = { n, def, cancel, check != NULL, checked ? *checked : 0 };
    INT_PTR r = DialogBoxIndirectParamW(GetModuleHandleW(NULL), d, parent, choice_proc, (LPARAM)&ci);
    if (r < 0) return -1;
    if (checked) *checked = ci.checked;
    return (int)r;
}

char* ask_text(SDL_Window* win, const char* title, const char* prompt) {
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(win, &info) || info.subsystem != SDL_SYSWM_WINDOWS) return NULL;
    wchar_t wtitle[200], wprompt[1000];
    MultiByteToWideChar(CP_UTF8, 0, title, -1, wtitle, 200);
    MultiByteToWideChar(CP_UTF8, 0, prompt, -1, wprompt, 1000);
    static WORD buf[4096];
    memset(buf, 0, sizeof buf);
    DLGTEMPLATE* d = (DLGTEMPLATE*)align4(buf);
    d->style = DS_MODALFRAME | DS_CENTER | DS_SETFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    d->cdit = 4; d->x = 0; d->y = 0; d->cx = 340; d->cy = 230;
    WORD* p = (WORD*)(d + 1);
    *p++ = 0; *p++ = 0;                                          /* no menu, default class */
    p = put_str(p, wtitle);
    *p++ = 9; p = put_str(p, L"Segoe UI");                       /* font */
    p = put_item(p, 0, 7, 7, 326, 34, 0xFFFF, 0x82, wprompt);
    p = put_item(p, WS_BORDER | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN, 7, 44, 326, 158, ID_TEXT, 0x81, L"");
    p = put_item(p, WS_TABSTOP | BS_DEFPUSHBUTTON, 213, 208, 60, 15, IDOK, 0x80, W("Save"));
    p = put_item(p, WS_TABSTOP | BS_PUSHBUTTON, 277, 208, 56, 15, IDCANCEL, 0x80, W("Skip"));
    g_text_out = NULL;
    INT_PTR r = DialogBoxIndirectParamW(GetModuleHandleW(NULL), d, info.info.win.window, text_proc, 0);
    if (r != 1 || !g_text_out) { free(g_text_out); g_text_out = NULL; return NULL; }
    int n = WideCharToMultiByte(CP_UTF8, 0, g_text_out, -1, NULL, 0, NULL, NULL);
    char* out = malloc((size_t)(n > 0 ? n : 1));
    if (out) { out[0] = 0; if (n > 0) WideCharToMultiByte(CP_UTF8, 0, g_text_out, -1, out, n, NULL, NULL); }
    free(g_text_out); g_text_out = NULL;
    return out;
}


/* ---------------------------------------------------------------- the session window (menu.h) */
enum { ID_ADDR = 1101, ID_COPY, ID_HINT, ID_LIST, ID_STATUS, ID_START, ID_HEAD };
static HWND g_ses;
static int g_ses_action;
static SessionWindow g_ses_shown;

static void set_text(HWND h, int id, const char* utf8) {
    wchar_t w[1024];
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, 1024);
    w[1023] = 0;
    SetDlgItemTextW(h, id, w);
}

static void copy_address(HWND h) {
    int n = GetWindowTextLengthW(GetDlgItem(h, ID_ADDR));
    if (n <= 0 || !OpenClipboard(h)) return;
    EmptyClipboard();
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, ((size_t)n + 1) * sizeof(wchar_t));
    if (mem) {
        wchar_t* p = GlobalLock(mem);
        GetDlgItemTextW(h, ID_ADDR, p, n + 1);
        GlobalUnlock(mem);
        if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
    }
    CloseClipboard();
    SetDlgItemTextW(h, ID_COPY, W("Copied!"));
    SetTimer(h, 1, 2000, NULL);
}

static INT_PTR CALLBACK ses_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG: return TRUE;
    case WM_TIMER: KillTimer(h, 1); SetDlgItemTextW(h, ID_COPY, W("Copy")); return TRUE;
    case WM_CLOSE: g_ses_action = 2; return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_COPY: copy_address(h); return TRUE;
        case ID_START: if (IsWindowEnabled(GetDlgItem(h, ID_START))) g_ses_action = 1; return TRUE;
        case IDCANCEL: g_ses_action = 2; return TRUE;
        }
        break;
    }
    return FALSE;
}

int session_window_open(SDL_Window* win) {
    if (g_ses) return 1;
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(win, &info) || info.subsystem != SDL_SYSWM_WINDOWS) return 0;
    static WORD buf[4096];
    memset(buf, 0, sizeof buf);
    DLGTEMPLATE* d = (DLGTEMPLATE*)align4(buf);
    d->style = DS_MODALFRAME | DS_CENTER | DS_SETFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE;
    d->cdit = 9; d->x = 0; d->y = 0; d->cx = 270; d->cy = 278;
    WORD* p = (WORD*)(d + 1);
    *p++ = 0; *p++ = 0;
    p = put_str(p, W("Play with friends far away"));
    *p++ = 10; p = put_str(p, L"Segoe UI");
    p = put_item(p, 0, 8, 8, 254, 10, ID_HEAD, 0x82, L"");
    p = put_item(p, WS_BORDER | ES_READONLY | ES_AUTOHSCROLL | ES_CENTER, 8, 21, 196, 16, ID_ADDR, 0x81, L"");
    p = put_item(p, WS_TABSTOP | BS_PUSHBUTTON, 210, 21, 52, 16, ID_COPY, 0x80, W("Copy"));
    p = put_item(p, 0, 8, 42, 254, 92, ID_HINT, 0x82, L"");
    p = put_item(p, 0, 8, 138, 254, 10, 0xFFFF, 0x82, W("Players:"));
    p = put_item(p, WS_BORDER | WS_VSCROLL | LBS_NOSEL, 8, 150, 254, 54, ID_LIST, 0x83, L"");
    p = put_item(p, 0, 8, 210, 254, 34, ID_STATUS, 0x82, L"");
    p = put_item(p, WS_TABSTOP | BS_DEFPUSHBUTTON, 92, 254, 110, 17, ID_START, 0x80, W("Start the game"));
    p = put_item(p, WS_TABSTOP | BS_PUSHBUTTON, 208, 254, 54, 17, IDCANCEL, 0x80, W("Cancel"));
    g_ses = CreateDialogIndirectParamW(GetModuleHandleW(NULL), d, info.info.win.window, ses_proc, 0);
    if (!g_ses) return 0;
    /* a bigger, bolder address */
    HFONT f = CreateFontW(-22, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    if (f) SendDlgItemMessageW(g_ses, ID_ADDR, WM_SETFONT, (WPARAM)f, TRUE);
    memset(&g_ses_shown, 0, sizeof g_ses_shown);
    g_ses_shown.can_start = -1;
    g_ses_action = 0;
    return 1;
}

void session_window_update(const SessionWindow* w) {
    if (!g_ses) return;
    SessionWindow* o = &g_ses_shown;
    if (strcmp(o->heading, w->heading)) set_text(g_ses, ID_HEAD, w->heading);
    if (strcmp(o->address, w->address)) set_text(g_ses, ID_ADDR, w->address);
    if (strcmp(o->hint, w->hint)) set_text(g_ses, ID_HINT, w->hint);
    if (strcmp(o->status, w->status)) set_text(g_ses, ID_STATUS, w->status);
    int list_changed = o->n_players != w->n_players;
    for (int i = 0; i < w->n_players && !list_changed; i++) list_changed = strcmp(o->players[i], w->players[i]) != 0;
    if (list_changed) {
        HWND l = GetDlgItem(g_ses, ID_LIST);
        SendMessageW(l, LB_RESETCONTENT, 0, 0);
        for (int i = 0; i < w->n_players; i++) {
            wchar_t t[128]; MultiByteToWideChar(CP_UTF8, 0, w->players[i], -1, t, 128); t[127] = 0;
            SendMessageW(l, LB_ADDSTRING, 0, (LPARAM)t);
        }
    }
    int start = w->host && w->can_start && !w->failed;
    if (o->can_start != start || o->host != w->host || o->failed != w->failed) {
        EnableWindow(GetDlgItem(g_ses, ID_START), start);
        ShowWindow(GetDlgItem(g_ses, ID_START), w->host && !w->failed ? SW_SHOW : SW_HIDE);
        SetDlgItemTextW(g_ses, IDCANCEL, w->failed ? W("Back") : W("Cancel"));
        EnableWindow(GetDlgItem(g_ses, ID_COPY), w->address[0] != 0);
    }
    *o = *w;
    o->can_start = start;
}


/* ---------------------------------------------------------------- volume (Game > Volume...) */
#define ID_VOL_BAR 4000
#define ID_VOL_TEXT 4001
#define ID_VOL_MUTE 4002
static HWND g_vol;
static int* g_vol_val; static int* g_vol_mute;
static void vol_show(HWND h) {
    wchar_t t[32]; swprintf(t, 32, L"%d%%", *g_vol_val); SetDlgItemTextW(h, ID_VOL_TEXT, t);
    CheckDlgButton(h, ID_VOL_MUTE, *g_vol_mute ? BST_CHECKED : BST_UNCHECKED);
}
static INT_PTR CALLBACK vol_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        SCROLLINFO si = { sizeof si, SIF_RANGE | SIF_POS | SIF_PAGE, 0, 100 + 9, 10, *g_vol_val, 0 };
        SetScrollInfo(GetDlgItem(h, ID_VOL_BAR), SB_CTL, &si, TRUE);
        vol_show(h);
        return TRUE;
    }
    case WM_HSCROLL: {
        HWND bar = GetDlgItem(h, ID_VOL_BAR);
        int v = *g_vol_val;
        switch (LOWORD(wp)) {
        case SB_LINELEFT: v -= 1; break; case SB_LINERIGHT: v += 1; break;
        case SB_PAGELEFT: v -= 10; break; case SB_PAGERIGHT: v += 10; break;
        case SB_LEFT: v = 0; break; case SB_RIGHT: v = 100; break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: v = HIWORD(wp); break;
        default: return TRUE;
        }
        v = v < 0 ? 0 : v > 100 ? 100 : v;
        *g_vol_val = v; if (v > 0) *g_vol_mute = 0;
        SetScrollPos(bar, SB_CTL, v, TRUE);
        vol_show(h);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == ID_VOL_MUTE) { *g_vol_mute = IsDlgButtonChecked(h, ID_VOL_MUTE) == BST_CHECKED; return TRUE; }
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) { DestroyWindow(h); g_vol = NULL; return TRUE; }
        break;
    case WM_CLOSE: DestroyWindow(h); g_vol = NULL; return TRUE;
    }
    (void)lp;
    return FALSE;
}
void volume_window_open(SDL_Window* win, int* volume, int* mute) {
    g_vol_val = volume; g_vol_mute = mute;
    if (g_vol) { vol_show(g_vol); SetForegroundWindow(g_vol); return; }
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(win, &info) || info.subsystem != SDL_SYSWM_WINDOWS) return;
    static WORD buf[1024];
    memset(buf, 0, sizeof buf);
    DLGTEMPLATE* d = (DLGTEMPLATE*)align4(buf);
    d->style = DS_MODALFRAME | DS_CENTER | DS_SETFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE;
    d->cdit = 4; d->cx = 220; d->cy = 70;
    WORD* p = (WORD*)(d + 1);
    *p++ = 0; *p++ = 0;
    p = put_str(p, W("Volume"));
    *p++ = 10; p = put_str(p, L"Segoe UI");
    p = put_item(p, WS_TABSTOP | SBS_HORZ, 8, 10, 168, 12, ID_VOL_BAR, 0x84, L"");
    p = put_item(p, 0, 182, 11, 32, 10, ID_VOL_TEXT, 0x82, L"");
    p = put_item(p, WS_TABSTOP | BS_AUTOCHECKBOX, 8, 30, 120, 12, ID_VOL_MUTE, 0x80, W("Mute"));
    p = put_item(p, WS_TABSTOP | BS_DEFPUSHBUTTON, 160, 48, 54, 15, IDOK, 0x80, W("Close"));
    g_vol = CreateDialogIndirectParamW(GetModuleHandleW(NULL), d, info.info.win.window, vol_proc, 0);
}
void volume_window_refresh(void) { if (g_vol) { SetScrollPos(GetDlgItem(g_vol, ID_VOL_BAR), SB_CTL, *g_vol_val, TRUE); vol_show(g_vol); } }

/* ---------------------------------------------------------------- controls (Game > Controls...) */
#define ID_CTL_LIST 4100
#define ID_CTL_K1 4101
#define ID_CTL_K2 4102
#define ID_CTL_PAD 4103
#define ID_CTL_CLEAR 4104
#define ID_CTL_DEFAULTS 4105
#define ID_CTL_STATUS 4106
static HWND g_ctlw, g_ctl_owner;          /* the dialog, and the game's window */
static int g_ctl_req = -1, g_ctl_req_what;
static void ctl_fill(HWND h) {
    HWND l = GetDlgItem(h, ID_CTL_LIST);
    int sel = (int)SendMessageW(l, LB_GETCURSEL, 0, 0), top = (int)SendMessageW(l, LB_GETTOPINDEX, 0, 0);
    SendMessageW(l, LB_RESETCONTENT, 0, 0);
    for (int a = 0; a < CA_N; a++) {
        char k0[64], k1[64], pd[64], row[300];
        controls_key_name(g_ctl_key[a][0], k0, sizeof k0); controls_key_name(g_ctl_key[a][1], k1, sizeof k1);
        controls_pad_name(g_ctl_pad[a], pd, sizeof pd);
        snprintf(row, sizeof row, "%s\t%s\t%s\t%s", tr(g_ctl[a].name), k0, k1, pd);
        wchar_t w[300]; MultiByteToWideChar(CP_UTF8, 0, row, -1, w, 300); w[299] = 0;
        SendMessageW(l, LB_ADDSTRING, 0, (LPARAM)w);
    }
    SendMessageW(l, LB_SETTOPINDEX, (WPARAM)top, 0);
    if (sel >= 0) SendMessageW(l, LB_SETCURSEL, (WPARAM)sel, 0);
}
static INT_PTR CALLBACK ctl_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG: {
        int tabs[3] = { 172, 228, 284 };                          /* dialog units: action | key | key | controller */
        SendDlgItemMessageW(h, ID_CTL_LIST, LB_SETTABSTOPS, 3, (LPARAM)tabs);
        ctl_fill(h);
        SendDlgItemMessageW(h, ID_CTL_LIST, LB_SETCURSEL, 0, 0);
        return TRUE;
    }
    case WM_COMMAND: {
        int sel = (int)SendDlgItemMessageW(h, ID_CTL_LIST, LB_GETCURSEL, 0, 0);
        switch (LOWORD(wp)) {
        case ID_CTL_LIST: if (HIWORD(wp) == LBN_DBLCLK && sel >= 0) { g_ctl_req = sel; g_ctl_req_what = 0; } return TRUE;
        case ID_CTL_K1: if (sel >= 0) { g_ctl_req = sel; g_ctl_req_what = 0; } return TRUE;
        case ID_CTL_K2: if (sel >= 0) { g_ctl_req = sel; g_ctl_req_what = 1; } return TRUE;
        case ID_CTL_PAD: if (sel >= 0) { g_ctl_req = sel; g_ctl_req_what = 2; } return TRUE;
        case ID_CTL_CLEAR:
            if (sel >= 0) {
                if (g_ctl_key[sel][1]) g_ctl_key[sel][1] = 0;
                else if (g_ctl_key[sel][0]) g_ctl_key[sel][0] = 0;
                else g_ctl_pad[sel] = CTL_NONE;
                controls_save(); ctl_fill(h);
            }
            return TRUE;
        case ID_CTL_DEFAULTS:
            if (MessageBoxW(h, W("Put every key and button back to how the game came?"), W("Controls"), MB_YESNO | MB_ICONQUESTION) == IDYES) {
                controls_defaults(); controls_save(); ctl_fill(h);
            }
            return TRUE;
        case IDOK: case IDCANCEL: DestroyWindow(h); g_ctlw = NULL; return TRUE;
        }
        break;
    }
    case WM_CLOSE: DestroyWindow(h); g_ctlw = NULL; return TRUE;
    }
    return FALSE;
}
void controls_window_open(SDL_Window* win) {
    if (g_ctlw) { SetForegroundWindow(g_ctlw); return; }
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(win, &info) || info.subsystem != SDL_SYSWM_WINDOWS) return;
    static WORD buf[4096];
    memset(buf, 0, sizeof buf);
    DLGTEMPLATE* d = (DLGTEMPLATE*)align4(buf);
    d->style = DS_MODALFRAME | DS_CENTER | DS_SETFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE;
    d->cdit = 13; d->cx = 400; d->cy = 290;
    WORD* p = (WORD*)(d + 1);
    *p++ = 0; *p++ = 0;
    p = put_str(p, W("Controls"));
    *p++ = 9; p = put_str(p, L"Segoe UI");
    p = put_item(p, 0, 8, 6, 384, 18, 0xFFFF, 0x82, W("Pick an action, then change one of its keys or its controller button. A key or button that "
                 "another action has moves over to it (they swap). Esc and Alt+Enter, and the sticks, stay as they are."));
    p = put_item(p, 0, 8, 28, 170, 9, 0xFFFF, 0x82, W("Action"));
    p = put_item(p, 0, 182, 28, 52, 9, 0xFFFF, 0x82, W("Key"));
    p = put_item(p, 0, 238, 28, 52, 9, 0xFFFF, 0x82, W("Second key"));
    p = put_item(p, 0, 294, 28, 98, 9, 0xFFFF, 0x82, W("Controller"));
    p = put_item(p, WS_BORDER | WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY | LBS_USETABSTOPS | LBS_NOINTEGRALHEIGHT, 8, 39, 384, 196, ID_CTL_LIST, 0x83, L"");
    p = put_item(p, WS_TABSTOP | BS_PUSHBUTTON, 8, 241, 72, 15, ID_CTL_K1, 0x80, W("Change key..."));
    p = put_item(p, WS_TABSTOP | BS_PUSHBUTTON, 84, 241, 82, 15, ID_CTL_K2, 0x80, W("Second key..."));
    p = put_item(p, WS_TABSTOP | BS_PUSHBUTTON, 170, 241, 100, 15, ID_CTL_PAD, 0x80, W("Controller button..."));
    p = put_item(p, WS_TABSTOP | BS_PUSHBUTTON, 274, 241, 54, 15, ID_CTL_CLEAR, 0x80, W("Clear"));
    p = put_item(p, WS_TABSTOP | BS_PUSHBUTTON, 8, 266, 100, 15, ID_CTL_DEFAULTS, 0x80, W("Restore defaults"));
    p = put_item(p, SS_LEFT, 112, 269, 222, 10, ID_CTL_STATUS, 0x82, L"");
    p = put_item(p, WS_TABSTOP | BS_DEFPUSHBUTTON, 338, 266, 54, 15, IDOK, 0x80, W("Close"));
    g_ctl_owner = info.info.win.window;
    g_ctlw = CreateDialogIndirectParamW(GetModuleHandleW(NULL), d, info.info.win.window, ctl_proc, 0);
}
int controls_window_request(int* action, int* what) {
    if (g_ctl_req < 0) return 0;
    *action = g_ctl_req; *what = g_ctl_req_what; g_ctl_req = -1;
    return 1;
}
/* While the game waits for the new key, the keyboard has to be the game
 * window's: Windows doesn't always let SDL_RaiseWindow take it from this
 * dialog (the key then went to the dialog's list and the game waited for
 * ever). SetFocus on a window of this thread always works. The dialog is
 * disabled meanwhile (no stray clicks) and says what to press. */
void controls_window_capture(int on, const char* prompt) {
    if (!g_ctlw) return;
    if (on) {
        wchar_t w[200]; MultiByteToWideChar(CP_UTF8, 0, prompt ? prompt : "", -1, w, 200); w[199] = 0;
        SetDlgItemTextW(g_ctlw, ID_CTL_STATUS, w);
        EnableWindow(g_ctlw, FALSE);
        if (g_ctl_owner) { SetForegroundWindow(g_ctl_owner); SetActiveWindow(g_ctl_owner); SetFocus(g_ctl_owner); }
    } else {
        SetDlgItemTextW(g_ctlw, ID_CTL_STATUS, L"");
        EnableWindow(g_ctlw, TRUE);
    }
}
void controls_window_refresh(void) {
    if (!g_ctlw) return;
    ctl_fill(g_ctlw);
    if (IsWindowEnabled(g_ctlw)) { SetForegroundWindow(g_ctlw); SetFocus(GetDlgItem(g_ctlw, ID_CTL_LIST)); }
}

int session_window_action(void) { int a = g_ses_action; g_ses_action = 0; return a; }

void session_window_close(void) {
    if (!g_ses) return;
    DestroyWindow(g_ses);
    g_ses = NULL;
}
#else
int choice_box(SDL_Window* win, const char* title, const char* text, const char* const* buttons, int n, int def, int cancel,
               const char* check, int* checked) { (void)win; (void)title; (void)text; (void)buttons; (void)n; (void)def; (void)cancel; (void)check; (void)checked; return -1; }
char* ask_text(SDL_Window* win, const char* title, const char* prompt) { (void)win; (void)title; (void)prompt; return NULL; }
float display_mm_per_pixel(SDL_Window* win) { (void)win; return 0; }
void menu_init(SDL_Window* win, const char* const* shell_names, int n_shells) { (void)win; (void)shell_names; (void)n_shells; }
void menu_rebuild(void) {}
void menu_show(int on) { (void)on; }
void menu_strip(int on) { (void)on; }
int menu_strip_shown(void) { return 0; }
void window_fix_frame(SDL_Window* win) { (void)win; }
int clipboard_set_file(const char* path) { (void)path; return 0; }
int file_dialog(SDL_Window* win, int save, const char* title, const char* filter, const char* default_name, char* out, size_t n) {
    (void)win; (void)save; (void)title; (void)filter; (void)default_name; (void)out; (void)n; return 0; }
void menu_init_second(SDL_Window* win) { (void)win; }
void menu_show_second(int on) { (void)on; }
void menu_check(int id, int on) { (void)id; (void)on; }
void menu_enable(int id, int on) { (void)id; (void)on; }
int menu_command(const SDL_Event* e) { (void)e; return 0; }
int session_window_open(SDL_Window* win) { (void)win; return 0; }
void volume_window_open(SDL_Window* win, int* volume, int* mute) { (void)win; (void)volume; (void)mute; }
void volume_window_refresh(void) {}
void controls_window_open(SDL_Window* win) { (void)win; }
int controls_window_request(int* action, int* what) { (void)action; (void)what; return 0; }
void controls_window_capture(int on, const char* prompt) { (void)on; (void)prompt; }
void controls_window_refresh(void) {}
void session_window_update(const SessionWindow* w) { (void)w; }
int session_window_action(void) { return 0; }
void session_window_close(void) {}
#endif
