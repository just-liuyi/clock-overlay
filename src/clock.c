/* ============================================================================
 *  ClockOverlay —— 游戏悬浮时钟
 *
 *  C + 纯 Win32，单文件。设计依据：clock-overlay-design.md
 *
 *  安全边界（第 6 节）：只读系统时间 + 显示独立窗口。
 *    不注入、不 Hook 显卡、不读写任何进程内存、不装驱动、不截屏、
 *    不模拟输入、不注册任何键盘热键或键盘钩子、不读取进程名或窗口标题。
 *    “仅全屏时显示”只比较前台窗口矩形是否铺满显示器
 *    （另读窗口类名仅用于排除桌面本身）。
 *
 *  三个窗口：
 *    host     隐藏窗口：托盘、定时器、广播消息
 *    overlay  时钟：分层（逐像素 alpha）、不可激活、不进任务栏
 *    panel    设置面板：打开期间时钟处于编辑模式（可拖动、滚轮缩放）
 *
 *  打开设置面板：点击托盘图标 / 再次运行 exe / 双击时钟（未穿透时）
 * ========================================================================= */

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#define WINVER       0x0A00
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <shellapi.h>
#include <commdlg.h>
#include <commctrl.h>
#include <stdarg.h>
#include <math.h>

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

/* ---------------------------------------------------------------- 常量 -- */

#define APP_NAME     L"ClockOverlay"
#define APP_TITLE    L"悬浮时钟"
#define APP_VERSION  L"1.1.0"
#define CFG_VERSION  2                 /* v2：去掉热键项 */
#define CFG_SECTION  L"ClockOverlay"
#define CFG_FILE     L"ClockOverlay.ini"
#define FRAC         10000             /* 位置与尺寸：显示器的万分比 */

#define HOST_CLASS   L"ClockOverlay.Host"
#define OVL_CLASS    L"ClockOverlay.Overlay"
#define PANEL_CLASS  L"ClockOverlay.Panel"
#define MUTEX_NAME   L"Local\\ClockOverlay.SingleInstance.v1"
#define EV_RECOVER   L"Local\\ClockOverlay.Recover.v1"
#define EV_EXIT      L"Local\\ClockOverlay.Exit.v1"
#define RUN_KEY      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define FALLBACK_FONT L"Microsoft YaHei UI"
#define UI_FONT      L"Microsoft YaHei UI"

#define WM_TRAY       (WM_APP + 1)
#define WM_CO_RECOVER (WM_APP + 2)     /* 来自第二实例 */
#define WM_CO_EXIT    (WM_APP + 3)

#define MIN_W        40
#define MIN_H        18
#define DEF_OFFX     100               /* 1%  */
#define DEF_OFFY     180               /* 1.8% */
#define DATE_HEIGHT_RATIO 145          /* 显示日期时高度 ×1.45 */

enum { IDT_TICK = 1, IDT_TOPMOST, IDT_FSPOLL, IDT_RELAYOUT, IDT_SAVE };

/* 设置面板控件 ID */
enum {
    IDC_LABEL = 900, IDC_HEADER, IDC_HINT,
    IDC_VISIBLE = 1001, IDC_TOPMOST, IDC_CLICKTHRU, IDC_LOCK, IDC_SHOWMODE,
    IDC_TIMEFMT, IDC_TIMEPREV, IDC_SHOWDATE, IDC_DATEFMT, IDC_DATEPREV,
    IDC_AUTOSTART, IDC_OPENCFG, IDC_RELOAD,
    IDC_FONT, IDC_TEXTCOLOR, IDC_OUTLINE, IDC_OUTLINECOLOR, IDC_BGCOLOR,
    IDC_BGOP, IDC_BGOP_VAL, IDC_CORNER, IDC_CORNER_VAL, IDC_OPACITY, IDC_OPACITY_VAL,
    IDC_POS_TL, IDC_POS_TR, IDC_POS_BL, IDC_POS_BR, IDC_POS_TC,
    IDC_SIZESLD, IDC_SIZE_VAL, IDC_FITW, IDC_RESETPOS,
    IDC_DEFAULTS, IDC_EXIT
};

static const WCHAR *kTimePresets[] = {
    L"HH:mm:ss", L"HH:mm", L"h:mm:ss tt", L"h:mm tt", L"tt h:mm:ss", L"tt h:mm"
};
#define N_TIME_PRESETS 6

static const WCHAR *kDatePresets[] = {
    L"yyyy-MM-dd ddd", L"yyyy-MM-dd", L"M月d日 dddd", L"MM/dd ddd", L"ddd", L"dddd"
};
#define N_DATE_PRESETS 6

/* ---------------------------------------------------------------- 配置 -- */

typedef struct {
    int      version;
    WCHAR    monitor[CCHDEVICENAME];   /* 显示器设备名，空 = 主屏 */
    int      anchor;                   /* bit0 靠右，bit1 靠下 */
    int      offX, offY;               /* 距锚定边：显示器宽 / 高的万分比 */
    int      sizeW, sizeH;             /* 宽、高：显示器高度的万分比 */
    int      topmost, clickThrough, locked, visible;
    int      showMode;                 /* 0 始终  1 仅全屏程序在前台时 */
    int      opacity;                  /* 整窗 20..100 */
    WCHAR    font[LF_FACESIZE];
    int      bold, italic;
    COLORREF textColor, bgColor, outlineColor;
    int      bgOpacity;                /* 0..100 */
    int      outline;
    int      corner;                   /* 圆角：高度百分比 0..50 */
    WCHAR    timeFmt[64], dateFmt[64];
    int      showDate;
    int      autostart;
} Config;

static Config g_cfg;

/* ---------------------------------------------------------------- 全局 -- */

static HINSTANCE g_inst;
static HWND      g_host, g_overlay, g_panel;
static HICON     g_iconSmall, g_iconBig;
static NOTIFYICONDATAW g_nid;
static BOOL      g_trayAdded;
static UINT      g_msgTaskbarCreated;
static HWINEVENTHOOK g_hook;
static HANDLE    g_evRecover, g_evExit;

static BOOL      g_edit;
static BOOL      g_dragging;
static POINT     g_dragOrigin;
static RECT      g_dragRect;

static WCHAR     g_exePath[MAX_PATH];
static WCHAR     g_cfgPath[MAX_PATH];
static WCHAR     g_cfgTmp[MAX_PATH + 8];
static BOOL      g_portable;
static COLORREF  g_custColors[16];

/* 面板 */
static UINT      g_panelDpi = 96;
static HFONT     g_uiFont, g_uiBold;
static BOOL      g_syncing;
static BOOL      g_timeFmtBad, g_dateFmtBad;

/* ---------------------------------------------------------------- 工具 -- */

static int IMin(int a, int b) { return a < b ? a : b; }
static int IMax(int a, int b) { return a > b ? a : b; }
static int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static BOOL IsDigitW(WCHAR c) { return c >= L'0' && c <= L'9'; }

static void Trim(WCHAR *s)
{
    WCHAR *p = s;
    while (*p == L' ' || *p == L'\t') p++;
    if (p != s) MoveMemory(s, p, (lstrlenW(p) + 1) * sizeof(WCHAR));
    int n = lstrlenW(s);
    while (n > 0 && (s[n-1] == L' ' || s[n-1] == L'\t' || s[n-1] == L'\r' || s[n-1] == L'\n'))
        s[--n] = 0;
}

static BOOL ParseInt(const WCHAR *s, int *out)
{
    int sign = 1, nd = 0;
    long long v = 0;
    while (*s == L' ') s++;
    if (*s == L'-') { sign = -1; s++; } else if (*s == L'+') s++;
    while (IsDigitW(*s)) {
        v = v * 10 + (*s - L'0');
        if (++nd > 9) return FALSE;
        s++;
    }
    while (*s == L' ') s++;
    if (nd == 0 || *s) return FALSE;
    *out = (int)(v * sign);
    return TRUE;
}

static int HexVal(WCHAR c)
{
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}

static BOOL ParseColor(const WCHAR *s, COLORREF *out)
{
    int v[6];
    while (*s == L' ') s++;
    if (*s == L'#') s++;
    for (int i = 0; i < 6; i++) {
        v[i] = HexVal(s[i]);
        if (v[i] < 0) return FALSE;
    }
    s += 6;
    while (*s == L' ') s++;
    if (*s) return FALSE;
    *out = RGB(v[0] * 16 + v[1], v[2] * 16 + v[3], v[4] * 16 + v[5]);
    return TRUE;
}

static BOOL FileExists(const WCHAR *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static BOOL DirWritable(const WCHAR *dir)
{
    WCHAR p[MAX_PATH + 32];
    wsprintfW(p, L"%s\\.clockoverlay-w.tmp", dir);
    HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    CloseHandle(h);
    return TRUE;
}

/* --------------------------------------------------------- 时间格式化 -- */

static BOOL FormatTimeStr(const SYSTEMTIME *st, const WCHAR *fmt, WCHAR *out, int cap)
{
    if (!fmt[0]) return FALSE;
    return GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, 0, st, fmt, out, cap) > 1;
}

static BOOL FormatDateStr(const SYSTEMTIME *st, const WCHAR *fmt, WCHAR *out, int cap)
{
    if (!fmt[0]) return FALSE;
    return GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, st, fmt, out, cap, NULL) > 1;
}

static BOOL FmtHasSeconds(const WCHAR *f)
{
    BOOL q = FALSE;
    for (; *f; f++) {
        if (*f == L'\'') q = !q;
        else if (!q && *f == L's') return TRUE;
    }
    return FALSE;
}

static BOOL TimeFmtValid(const WCHAR *f)
{
    SYSTEMTIME st; WCHAR b[128];
    GetLocalTime(&st);
    return FormatTimeStr(&st, f, b, 128) && lstrlenW(b) < 63;
}

static BOOL DateFmtValid(const WCHAR *f)
{
    SYSTEMTIME st; WCHAR b[128];
    GetLocalTime(&st);
    return FormatDateStr(&st, f, b, 128) && lstrlenW(b) < 63;
}

/* ---------------------------------------------------------------- 配置 -- */

static void SetDefaults(Config *c)
{
    ZeroMemory(c, sizeof(*c));
    c->version      = CFG_VERSION;
    c->anchor       = 1;                       /* 右上 */
    c->offX         = DEF_OFFX;
    c->offY         = DEF_OFFY;
    c->sizeH        = 600;                     /* 1080p 下约 65px */
    c->sizeW        = 1900;
    c->topmost      = 1;
    c->clickThrough = 1;
    c->visible      = 1;
    c->opacity      = 100;
    lstrcpyW(c->font, L"Consolas");
    c->bold         = 1;
    c->textColor    = RGB(0xF2, 0xF5, 0xF8);
    c->bgColor      = RGB(0x0A, 0x0D, 0x12);
    c->outlineColor = RGB(0, 0, 0);
    c->bgOpacity    = 55;
    c->outline      = 1;
    c->corner       = 20;
    lstrcpyW(c->timeFmt, L"HH:mm:ss");
    lstrcpyW(c->dateFmt, L"yyyy-MM-dd ddd");
}

static BOOL IniGet(const WCHAR *key, WCHAR *buf, DWORD cap)
{
    buf[0] = 0;
    GetPrivateProfileStringW(CFG_SECTION, key, L"\x01", buf, cap, g_cfgPath);
    if (buf[0] == 1 && buf[1] == 0) return FALSE;
    Trim(buf);
    return TRUE;
}

/* 越界 = 非法，保留默认值（只丢这一项） */
static void IniInt(const WCHAR *k, int *dst, int lo, int hi)
{
    WCHAR b[64]; int v;
    if (IniGet(k, b, 64) && ParseInt(b, &v) && v >= lo && v <= hi) *dst = v;
}

/* 越界时钳制到范围内（用于不透明度这类“有下限保护”的项） */
static void IniIntClamp(const WCHAR *k, int *dst, int lo, int hi)
{
    WCHAR b[64]; int v;
    if (IniGet(k, b, 64) && ParseInt(b, &v)) *dst = Clamp(v, lo, hi);
}

static void IniBool(const WCHAR *k, int *dst)
{
    WCHAR b[16];
    if (!IniGet(k, b, 16)) return;
    if (!lstrcmpiW(b, L"1") || !lstrcmpiW(b, L"true") || !lstrcmpiW(b, L"yes") || !lstrcmpiW(b, L"on"))
        *dst = 1;
    else if (!lstrcmpiW(b, L"0") || !lstrcmpiW(b, L"false") || !lstrcmpiW(b, L"no") || !lstrcmpiW(b, L"off"))
        *dst = 0;
}

static void IniColor(const WCHAR *k, COLORREF *dst)
{
    WCHAR b[32]; COLORREF c;
    if (IniGet(k, b, 32) && ParseColor(b, &c)) *dst = c;
}

/* 超长 = 损坏，整项丢弃 */
static BOOL IniStr(const WCHAR *k, WCHAR *dst, int cap)
{
    WCHAR b[256];
    if (!IniGet(k, b, 256)) return FALSE;
    int n = lstrlenW(b);
    if (n >= cap || n >= 255) return FALSE;
    lstrcpyW(dst, b);
    return TRUE;
}

static int ColorDist(COLORREF a, COLORREF b)
{
    int dr = (int)GetRValue(a) - (int)GetRValue(b);
    int dg = (int)GetGValue(a) - (int)GetGValue(b);
    int db = (int)GetBValue(a) - (int)GetBValue(b);
    return (dr < 0 ? -dr : dr) + (dg < 0 ? -dg : dg) + (db < 0 ? -db : db);
}

static void LoadConfig(void)
{
    Config d;
    SetDefaults(&d);
    g_cfg = d;
    if (!FileExists(g_cfgPath)) return;

    IniInt(L"version", &g_cfg.version, 0, 1000000);
    /* 版本迁移：v1 → v2 只去掉了热键项，旧键直接忽略 */

    WCHAR mon[CCHDEVICENAME];
    if (IniStr(L"monitor", mon, CCHDEVICENAME)) lstrcpyW(g_cfg.monitor, mon);
    IniInt(L"anchor",  &g_cfg.anchor, 0, 3);
    IniInt(L"offsetX", &g_cfg.offX, 0, FRAC - 100);
    IniInt(L"offsetY", &g_cfg.offY, 0, FRAC - 100);
    IniInt(L"width",   &g_cfg.sizeW, 150, FRAC * 2);
    IniInt(L"height",  &g_cfg.sizeH, 120, FRAC / 2);

    IniBool(L"topmost",      &g_cfg.topmost);
    IniBool(L"clickThrough", &g_cfg.clickThrough);
    IniBool(L"locked",       &g_cfg.locked);
    IniBool(L"visible",      &g_cfg.visible);
    IniInt (L"showMode",     &g_cfg.showMode, 0, 1);
    IniIntClamp(L"opacity",  &g_cfg.opacity, 20, 100);     /* A3：下限 20% */

    WCHAR f[LF_FACESIZE];
    if (IniStr(L"font", f, LF_FACESIZE) && f[0]) lstrcpyW(g_cfg.font, f);
    IniBool(L"bold",   &g_cfg.bold);
    IniBool(L"italic", &g_cfg.italic);

    IniColor(L"textColor",    &g_cfg.textColor);
    IniColor(L"bgColor",      &g_cfg.bgColor);
    IniColor(L"outlineColor", &g_cfg.outlineColor);
    IniInt  (L"bgOpacity",    &g_cfg.bgOpacity, 0, 100);
    IniBool (L"outline",      &g_cfg.outline);
    IniInt  (L"corner",       &g_cfg.corner, 0, 50);

    WCHAR fmt[64];
    if (IniStr(L"timeFormat", fmt, 64) && TimeFmtValid(fmt)) lstrcpyW(g_cfg.timeFmt, fmt);
    if (IniStr(L"dateFormat", fmt, 64) && DateFmtValid(fmt)) lstrcpyW(g_cfg.dateFmt, fmt);
    IniBool(L"showDate", &g_cfg.showDate);
    IniBool(L"autostart", &g_cfg.autostart);

    /* A3：文字色与底板色几乎相同且无描边 → 文字不可见，修正文字色 */
    if (!g_cfg.outline && g_cfg.bgOpacity > 30 && ColorDist(g_cfg.textColor, g_cfg.bgColor) < 90) {
        int lum = (GetRValue(g_cfg.bgColor) * 3 + GetGValue(g_cfg.bgColor) * 6 + GetBValue(g_cfg.bgColor)) / 10;
        g_cfg.textColor = lum > 128 ? RGB(0, 0, 0) : RGB(0xF2, 0xF5, 0xF8);
    }
}

static WCHAR g_wbuf[6144];
static int   g_wlen;

static void Out(const WCHAR *fmt, ...)
{
    WCHAR line[1100];
    va_list ap;
    va_start(ap, fmt);
    int n = wvsprintfW(line, fmt, ap);
    va_end(ap);
    if (n <= 0 || g_wlen + n >= (int)(sizeof(g_wbuf) / sizeof(WCHAR)) - 1) return;
    CopyMemory(g_wbuf + g_wlen, line, n * sizeof(WCHAR));
    g_wlen += n;
    g_wbuf[g_wlen] = 0;
}

static void OutColor(const WCHAR *k, COLORREF c)
{
    Out(L"%s=#%02X%02X%02X\r\n", k, GetRValue(c), GetGValue(c), GetBValue(c));
}

/* 原子写入：临时文件 → 刷盘 → 替换。写到一半断电不会损坏正式文件 */
static BOOL WriteFileAtomic(const void *data, DWORD bytes)
{
    HANDLE h = CreateFileW(g_cfgTmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    DWORD wr = 0;
    BOOL ok = WriteFile(h, data, bytes, &wr, NULL) && wr == bytes;
    if (ok) ok = FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok) { DeleteFileW(g_cfgTmp); return FALSE; }
    for (int i = 0; i < 10; i++) {
        if (MoveFileExW(g_cfgTmp, g_cfgPath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return TRUE;
        Sleep(30);                     /* 目标被占用（杀软 / 编辑器）时重试 */
    }
    DeleteFileW(g_cfgTmp);
    return FALSE;
}

static BOOL SaveConfig(void)
{
    if (g_host) KillTimer(g_host, IDT_SAVE);
    g_wlen = 0;
    g_wbuf[g_wlen++] = 0xFEFF;         /* UTF-16 LE BOM（B7） */
    g_wbuf[g_wlen] = 0;

    Out(L"; 悬浮时钟 ClockOverlay 配置文件（UTF-16）\r\n");
    Out(L"; 建议在设置面板中修改。手改后在面板里点“重新加载”。非法值只回落该项为默认值。\r\n");
    Out(L"[%s]\r\n", CFG_SECTION);
    Out(L"version=%d\r\n\r\n", CFG_VERSION);

    Out(L"; ---- 位置与大小（显示器万分比，换分辨率后按比例还原）----\r\n");
    Out(L"; monitor 为空 = 主屏；anchor 0 左上 1 右上 2 左下 3 右下\r\n");
    Out(L"monitor=%s\r\n", g_cfg.monitor);
    Out(L"anchor=%d\r\n", g_cfg.anchor);
    Out(L"offsetX=%d\r\noffsetY=%d\r\n", g_cfg.offX, g_cfg.offY);
    Out(L"; 宽、高均为显示器高度的万分比\r\n");
    Out(L"width=%d\r\nheight=%d\r\n\r\n", g_cfg.sizeW, g_cfg.sizeH);

    Out(L"; ---- 行为 ----\r\n");
    Out(L"topmost=%d\r\n", g_cfg.topmost);
    Out(L"; 游戏中鼠标穿透\r\n");
    Out(L"clickThrough=%d\r\n", g_cfg.clickThrough);
    Out(L"locked=%d\r\n", g_cfg.locked);
    Out(L"visible=%d\r\n", g_cfg.visible);
    Out(L"; showMode 0 始终显示 1 仅全屏程序在前台时显示\r\n");
    Out(L"showMode=%d\r\n", g_cfg.showMode);
    Out(L"autostart=%d\r\n\r\n", g_cfg.autostart);

    Out(L"; ---- 外观 ----\r\n");
    Out(L"; opacity 整体不透明度 20..100；bgOpacity 底板不透明度 0..100\r\n");
    Out(L"opacity=%d\r\n", g_cfg.opacity);
    Out(L"font=%s\r\n", g_cfg.font);
    Out(L"bold=%d\r\nitalic=%d\r\n", g_cfg.bold, g_cfg.italic);
    OutColor(L"textColor", g_cfg.textColor);
    Out(L"outline=%d\r\n", g_cfg.outline);
    OutColor(L"outlineColor", g_cfg.outlineColor);
    OutColor(L"bgColor", g_cfg.bgColor);
    Out(L"bgOpacity=%d\r\n", g_cfg.bgOpacity);
    Out(L"; corner 圆角，高度的百分比 0..50\r\n");
    Out(L"corner=%d\r\n\r\n", g_cfg.corner);

    Out(L"; ---- 格式（Windows 时间/日期格式串）----\r\n");
    Out(L"; 时间：HH 24时 h 12时 mm 分 ss 秒 tt 上午/下午；日期：yyyy MM dd ddd(周五) dddd(星期五)\r\n");
    Out(L"timeFormat=%s\r\n", g_cfg.timeFmt);
    Out(L"dateFormat=%s\r\n", g_cfg.dateFmt);
    Out(L"showDate=%d\r\n", g_cfg.showDate);

    return WriteFileAtomic(g_wbuf, (DWORD)(g_wlen * sizeof(WCHAR)));
}

static void SaveLater(void)
{
    SetTimer(g_host, IDT_SAVE, 800, NULL);
}

static void ResolveConfigPath(void)
{
    WCHAR dir[MAX_PATH], cand[MAX_PATH + 32], marker[MAX_PATH + 32];
    GetModuleFileNameW(NULL, g_exePath, MAX_PATH);
    lstrcpynW(dir, g_exePath, MAX_PATH);
    for (int i = lstrlenW(dir) - 1; i >= 0; i--)
        if (dir[i] == L'\\') { dir[i] = 0; break; }

    /* 便携模式：exe 同目录已有配置文件或 portable 标记，且目录可写 */
    wsprintfW(cand, L"%s\\%s", dir, CFG_FILE);
    wsprintfW(marker, L"%s\\portable", dir);
    if ((FileExists(cand) || FileExists(marker)) && DirWritable(dir)) {
        lstrcpynW(g_cfgPath, cand, MAX_PATH);
        g_portable = TRUE;
    } else {
        WCHAR app[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"APPDATA", app, MAX_PATH);
        if (n == 0 || n >= MAX_PATH - 40) lstrcpynW(app, dir, MAX_PATH);
        WCHAR d2[MAX_PATH + 32];
        wsprintfW(d2, L"%s\\ClockOverlay", app);
        CreateDirectoryW(d2, NULL);
        wsprintfW(cand, L"%s\\%s", d2, CFG_FILE);
        lstrcpynW(g_cfgPath, cand, MAX_PATH);
    }
    wsprintfW(g_cfgTmp, L"%s.tmp", g_cfgPath);
}

/* ------------------------------------------------------------ 开机自启 -- */

static BOOL AutostartGet(void)
{
    DWORD sz = 0;
    return RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, APP_NAME, RRF_RT_REG_SZ, NULL, NULL, &sz) == ERROR_SUCCESS;
}

static void AutostartApply(void)
{
    if (g_cfg.autostart) {
        WCHAR v[MAX_PATH + 4];
        wsprintfW(v, L"\"%s\"", g_exePath);
        RegSetKeyValueW(HKEY_CURRENT_USER, RUN_KEY, APP_NAME, REG_SZ, v, (DWORD)((lstrlenW(v) + 1) * sizeof(WCHAR)));
    } else if (AutostartGet()) {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, RUN_KEY, APP_NAME);
    }
}

/* -------------------------------------------------------------- 显示器 -- */

typedef struct { const WCHAR *name; HMONITOR found; } FindMon;

static BOOL CALLBACK EnumMonProc(HMONITOR m, HDC dc, LPRECT r, LPARAM lp)
{
    (void)dc; (void)r;
    FindMon *f = (FindMon *)lp;
    MONITORINFOEXW mi;
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(m, (MONITORINFO *)&mi) && !lstrcmpiW(mi.szDevice, f->name)) {
        f->found = m;
        return FALSE;
    }
    return TRUE;
}

/* 该屏不存在时回落到主屏（7.5） */
static HMONITOR MonitorByName(const WCHAR *name)
{
    if (name[0]) {
        FindMon f = { name, NULL };
        EnumDisplayMonitors(NULL, NULL, EnumMonProc, (LPARAM)&f);
        if (f.found) return f.found;
    }
    POINT p = { 0, 0 };
    return MonitorFromPoint(p, MONITOR_DEFAULTTOPRIMARY);
}

static RECT MonitorRect(HMONITOR m)
{
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(m, &mi);
    return mi.rcMonitor;
}

static void ComputeRectFromConfig(RECT *out)
{
    RECT mr = MonitorRect(MonitorByName(g_cfg.monitor));
    int mw = mr.right - mr.left, mh = mr.bottom - mr.top;
    int w = MulDiv(g_cfg.sizeW, mh, FRAC);
    int h = MulDiv(g_cfg.sizeH, mh, FRAC);
    w = Clamp(w, MIN_W, mw);
    h = Clamp(h, MIN_H, mh / 2);
    int ox = MulDiv(g_cfg.offX, mw, FRAC);
    int oy = MulDiv(g_cfg.offY, mh, FRAC);
    int x = (g_cfg.anchor & 1) ? mr.right - ox - w : mr.left + ox;
    int y = (g_cfg.anchor & 2) ? mr.bottom - oy - h : mr.top + oy;
    /* 启动校验：整个窗口必须落在显示器内（A1） */
    x = Clamp(x, mr.left, mr.right - w);
    y = Clamp(y, mr.top, mr.bottom - h);
    SetRect(out, x, y, x + w, y + h);
}

static void PlaceFromConfig(void)
{
    RECT r;
    ComputeRectFromConfig(&r);
    SetWindowPos(g_overlay, NULL, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

/* 把当前窗口位置换算成“显示器 + 锚点 + 比例偏移”（7.4） */
static void CaptureToConfig(void)
{
    RECT r;
    GetWindowRect(g_overlay, &r);
    HMONITOR m = MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW mi;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(m, (MONITORINFO *)&mi)) return;
    RECT mr = mi.rcMonitor;
    int mw = mr.right - mr.left, mh = mr.bottom - mr.top;
    if (mw <= 0 || mh <= 0) return;

    int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    int anchor = (cx > (mr.left + mr.right) / 2 ? 1 : 0) | (cy > (mr.top + mr.bottom) / 2 ? 2 : 0);
    int ox = (anchor & 1) ? mr.right - r.right : r.left - mr.left;
    int oy = (anchor & 2) ? mr.bottom - r.bottom : r.top - mr.top;

    MONITORINFO pmi; pmi.cbSize = sizeof(pmi);
    POINT p0 = { 0, 0 };
    GetMonitorInfoW(MonitorFromPoint(p0, MONITOR_DEFAULTTOPRIMARY), &pmi);
    BOOL isPrimary = EqualRect(&pmi.rcMonitor, &mr);

    lstrcpynW(g_cfg.monitor, isPrimary ? L"" : mi.szDevice, CCHDEVICENAME);
    g_cfg.anchor = anchor;
    g_cfg.offX   = Clamp(MulDiv(IMax(ox, 0), FRAC, mw), 0, FRAC - 100);
    g_cfg.offY   = Clamp(MulDiv(IMax(oy, 0), FRAC, mh), 0, FRAC - 100);
    g_cfg.sizeW  = Clamp(MulDiv(r.right - r.left, FRAC, mh), 150, FRAC * 2);
    g_cfg.sizeH  = Clamp(MulDiv(r.bottom - r.top, FRAC, mh), 120, FRAC / 2);
}

/* ---------------------------------------------------------------- 渲染 -- */

typedef struct {
    HFONT font, fb;                    /* 主字体 + 缺字回退字体（中文星期等） */
    int   px, digitW;
    int   above, below, boxH;          /* 基线上 / 下的可视高度 */
} LineFont;

static HDC      g_memDC;
static HGDIOBJ  g_memOldBmp;
static HBITMAP  g_bmOut, g_bmText, g_bmOutl;
static BYTE    *g_pxOut, *g_pxText, *g_pxOutl;
static int      g_bmW, g_bmH;

static LineFont g_lfTime, g_lfDate;
static int      g_padX, g_padY, g_gap;
static BOOL     g_layoutDirty = TRUE;
static int      g_layoutW, g_layoutH;

#define MAX_SMP 96
static WCHAR    g_smpTime[4][64];
static int      g_nSmpTime;
static WCHAR    g_smpDate[MAX_SMP][64];
static int      g_nSmpDate;

static HFONT MakeFont(const WCHAR *face, int px)
{
    return CreateFontW(-px, 0, 0, 0, g_cfg.bold ? FW_BOLD : FW_NORMAL, g_cfg.italic, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                       ANTIALIASED_QUALITY, DEFAULT_PITCH, face);
}

static void FreeLineFont(LineFont *lf)
{
    if (lf->font) DeleteObject(lf->font);
    if (lf->fb)   DeleteObject(lf->fb);
    ZeroMemory(lf, sizeof(*lf));
}

/* 选中合适字体并返回该字符宽度；主字体缺字时换回退字体 */
static int CharWidth(HDC dc, const LineFont *lf, WCHAR c, BOOL *useFb)
{
    WORD gi = 0;
    SelectObject(dc, lf->font);
    BOOL fb = FALSE;
    if (c > 0x7F && GetGlyphIndicesW(dc, &c, 1, &gi, GGI_MARK_NONEXISTING_GLYPHS) != GDI_ERROR && gi == 0xFFFF) {
        fb = TRUE;
        SelectObject(dc, lf->fb);
    }
    SIZE s;
    GetTextExtentPoint32W(dc, &c, 1, &s);
    if (useFb) *useFb = fb;
    return s.cx;
}

/* 数字统一按“最宽数字”占位，避免比例字体下每秒宽度抖动（B10） */
static int StableWidth(HDC dc, const LineFont *lf, const WCHAR *s, BOOL *anyFb)
{
    int t = 0;
    for (; *s; s++) {
        BOOL fb;
        if (IsDigitW(*s)) t += lf->digitW;
        else {
            t += CharWidth(dc, lf, *s, &fb);
            if (fb && anyFb) *anyFb = TRUE;
        }
    }
    return t;
}

static void DrawStable(HDC dc, const LineFont *lf, int x, int baseline, const WCHAR *s)
{
    for (; *s; s++) {
        int cw = CharWidth(dc, lf, *s, NULL);
        int slot = IsDigitW(*s) ? lf->digitW : cw;
        ExtTextOutW(dc, x + (slot - cw) / 2, baseline, 0, NULL, s, 1, NULL);
        x += slot;
    }
}

static void NormalizeDigits(WCHAR *s)
{
    for (; *s; s++) if (IsDigitW(*s)) *s = L'0';
}

static void AddSample(WCHAR (*arr)[64], int *n, int cap, const WCHAR *s)
{
    WCHAR t[64];
    lstrcpynW(t, s, 64);
    NormalizeDigits(t);
    for (int i = 0; i < *n; i++) if (!lstrcmpW(arr[i], t)) return;
    if (*n < cap) lstrcpyW(arr[(*n)++], t);
}

/* “最宽样例串”：两种上下午 × 两位小时；12 个月 × 7 个星期 */
static void BuildSamples(void)
{
    SYSTEMTIME st;
    WCHAR b[64];
    ZeroMemory(&st, sizeof(st));
    st.wYear = 2026; st.wMonth = 10; st.wDay = 28; st.wMinute = 58; st.wSecond = 58;
    g_nSmpTime = 0;
    WORD hrs[2] = { 10, 22 };
    for (int i = 0; i < 2; i++) {
        st.wHour = hrs[i];
        if (FormatTimeStr(&st, g_cfg.timeFmt, b, 64)) AddSample(g_smpTime, &g_nSmpTime, 4, b);
    }
    g_nSmpDate = 0;
    if (g_cfg.showDate) {
        for (WORD m = 1; m <= 12; m++)
            for (WORD d = 22; d <= 28; d++) {
                st.wMonth = m; st.wDay = d;
                if (FormatDateStr(&st, g_cfg.dateFmt, b, 64)) AddSample(g_smpDate, &g_nSmpDate, MAX_SMP, b);
            }
    }
}

static void MeasureLine(HDC dc, int px, WCHAR (*smp)[64], int n, LineFont *out, int *width)
{
    ZeroMemory(out, sizeof(*out));
    out->font = MakeFont(g_cfg.font, px);
    out->fb   = MakeFont(FALLBACK_FONT, px);
    out->px   = px;

    TEXTMETRICW tm, tmf;
    SelectObject(dc, out->fb);
    GetTextMetricsW(dc, &tmf);
    SelectObject(dc, out->font);
    GetTextMetricsW(dc, &tm);

    int dw = 0;
    for (WCHAR c = L'0'; c <= L'9'; c++) {
        SIZE s;
        GetTextExtentPoint32W(dc, &c, 1, &s);
        if (s.cx > dw) dw = s.cx;
    }
    out->digitW = dw;

    BOOL anyFb = FALSE;
    int mx = 0;
    for (int i = 0; i < n; i++) mx = IMax(mx, StableWidth(dc, out, smp[i], &anyFb));

    out->above = tm.tmAscent - tm.tmInternalLeading;
    out->below = tm.tmDescent / 2;
    if (anyFb) {
        out->above = IMax(out->above, tmf.tmAscent - tmf.tmInternalLeading);
        out->below = IMax(out->below, tmf.tmDescent / 3);
    }
    out->boxH = out->above + out->below;
    *width = mx;
}

static void FitLine(HDC dc, WCHAR (*smp)[64], int n, int maxW, int maxH, LineFont *out, int *outW)
{
    int lo = 6, hi = IMax(7, maxH * 2 + 2), best = 6;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, w;
        LineFont t;
        MeasureLine(dc, mid, smp, n, &t, &w);
        int boxH = t.boxH;
        FreeLineFont(&t);
        if (w <= maxW && boxH <= maxH) { best = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    MeasureLine(dc, best, smp, n, out, outW);
}

static void EnsureMemDC(void)
{
    if (!g_memDC) {
        g_memDC = CreateCompatibleDC(NULL);
        SetBkMode(g_memDC, TRANSPARENT);
        SetTextColor(g_memDC, RGB(255, 255, 255));
        SetTextAlign(g_memDC, TA_BASELINE | TA_LEFT | TA_NOUPDATECP);
    }
}

/* 字号按窗口自适应（只在尺寸 / 格式 / 字体变化时重算）。
   unlimitedW = 只按高度排版，用于“宽度贴合文字”，返回所需宽度 */
static int ComputeLayout(int W, int H, BOOL unlimitedW)
{
    EnsureMemDC();
    FreeLineFont(&g_lfTime);
    FreeLineFont(&g_lfDate);
    BuildSamples();

    g_padY = IMax(2, H * 8 / 100);
    g_padX = IMax(3, H * 14 / 100);
    int availW = unlimitedW ? (1 << 20) : IMax(8, W - 2 * g_padX);
    int availH = IMax(8, H - 2 * g_padY);
    int wT = 0, wD = 0;

    if (g_cfg.showDate && g_nSmpDate) {
        g_gap = IMax(1, availH * 4 / 100);
        FitLine(g_memDC, g_smpTime, g_nSmpTime, availW, availH * 60 / 100, &g_lfTime, &wT);
        int dMax = IMin(availH - g_lfTime.boxH - g_gap, g_lfTime.boxH * 62 / 100);
        FitLine(g_memDC, g_smpDate, g_nSmpDate, availW, IMax(6, dMax), &g_lfDate, &wD);
    } else {
        g_gap = 0;
        FitLine(g_memDC, g_smpTime, g_nSmpTime, availW, availH, &g_lfTime, &wT);
    }
    g_layoutW = W;
    g_layoutH = H;
    g_layoutDirty = unlimitedW;        /* 只量宽度时，真正排版之后再做 */
    return IMax(wT, wD) + 2 * g_padX + 2;
}

static void FreeSurfaces(void)
{
    if (g_memDC && g_memOldBmp) { SelectObject(g_memDC, g_memOldBmp); g_memOldBmp = NULL; }
    if (g_bmOut)  DeleteObject(g_bmOut);
    if (g_bmText) DeleteObject(g_bmText);
    if (g_bmOutl) DeleteObject(g_bmOutl);
    g_bmOut = g_bmText = g_bmOutl = NULL;
    g_pxOut = g_pxText = g_pxOutl = NULL;
    g_bmW = g_bmH = 0;
}

static HBITMAP MakeDib(int w, int h, BYTE **bits)
{
    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;        /* 自上而下 */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *p = NULL;
    HBITMAP b = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &p, NULL, 0);
    *bits = (BYTE *)p;
    return b;
}

static BOOL EnsureSurfaces(int w, int h)
{
    if (g_bmOut && w == g_bmW && h == g_bmH) return TRUE;
    EnsureMemDC();
    FreeSurfaces();
    g_bmOut  = MakeDib(w, h, &g_pxOut);
    g_bmText = MakeDib(w, h, &g_pxText);
    g_bmOutl = MakeDib(w, h, &g_pxOutl);
    if (!g_bmOut || !g_bmText || !g_bmOutl) { FreeSurfaces(); return FALSE; }
    g_memOldBmp = SelectObject(g_memDC, g_bmOut);
    g_bmW = w; g_bmH = h;
    return TRUE;
}

/* 圆角矩形覆盖率（0..255），带 1px 抗锯齿 */
static int RoundCov(int x, int y, int W, int H, float inset, float rad)
{
    float px = x + 0.5f, py = y + 0.5f;
    float l = inset, t = inset, r = W - inset, b = H - inset;
    if (px < l || px > r || py < t || py > b) return 0;
    if (rad <= 0.5f) return 255;
    float cx = 0, cy = 0;
    int corner = 0;
    if (px < l + rad)      { cx = l + rad; corner |= 1; }
    else if (px > r - rad) { cx = r - rad; corner |= 1; }
    if (py < t + rad)      { cy = t + rad; corner |= 2; }
    else if (py > b - rad) { cy = b - rad; corner |= 2; }
    if (corner != 3) return 255;
    float dx = px - cx, dy = py - cy;
    float c = rad - sqrtf(dx * dx + dy * dy) + 0.5f;
    if (c <= 0) return 0;
    if (c >= 1) return 255;
    return (int)(c * 255);
}

static void DrawLineTo(HBITMAP bm, const LineFont *lf, const WCHAR *s, int cx, int baseline, int rad)
{
    SelectObject(g_memDC, bm);
    BOOL dummy = FALSE;
    int w = StableWidth(g_memDC, lf, s, &dummy);
    int x = cx - w / 2;
    if (rad <= 0) {
        DrawStable(g_memDC, lf, x, baseline, s);
        return;
    }
    for (int dy = -rad; dy <= rad; dy++)
        for (int dx = -rad; dx <= rad; dx++)
            if (dx * dx + dy * dy <= rad * rad + rad)
                DrawStable(g_memDC, lf, x + dx, baseline + dy, s);
}

static void Render(void)
{
    if (!g_overlay) return;
    RECT r;
    GetWindowRect(g_overlay, &r);
    int W = r.right - r.left, H = r.bottom - r.top;
    if (W <= 0 || H <= 0 || !EnsureSurfaces(W, H)) return;
    if (g_layoutDirty || W != g_layoutW || H != g_layoutH) ComputeLayout(W, H, FALSE);

    SYSTEMTIME st;
    GetLocalTime(&st);
    WCHAR tb[64], db[64];
    if (!FormatTimeStr(&st, g_cfg.timeFmt, tb, 64)) lstrcpyW(tb, L"--:--");
    BOOL date = g_cfg.showDate && g_lfDate.font && FormatDateStr(&st, g_cfg.dateFmt, db, 64);

    int total = g_lfTime.boxH + (date ? g_gap + g_lfDate.boxH : 0);
    int top = (H - total) / 2;
    int baseT = top + g_lfTime.above;
    int baseD = top + g_lfTime.boxH + g_gap + g_lfDate.above;
    int cx = W / 2;

    size_t bytes = (size_t)W * H * 4;
    ZeroMemory(g_pxText, bytes);
    ZeroMemory(g_pxOutl, bytes);

    DrawLineTo(g_bmText, &g_lfTime, tb, cx, baseT, 0);
    if (date) DrawLineTo(g_bmText, &g_lfDate, db, cx, baseD, 0);
    if (g_cfg.outline) {
        DrawLineTo(g_bmOutl, &g_lfTime, tb, cx, baseT, IMax(1, (g_lfTime.boxH + 12) / 24));
        if (date) DrawLineTo(g_bmOutl, &g_lfDate, db, cx, baseD, IMax(1, (g_lfDate.boxH + 12) / 24));
    }
    SelectObject(g_memDC, g_bmOut);
    GdiFlush();

    /* 合成：底板 → 描边 → 文字 → 编辑边框（预乘 alpha） */
    int bgA = g_cfg.bgOpacity * 255 / 100;
    if (g_edit) bgA = IMax(bgA, 110);          /* 编辑模式保证整块可抓 */
    int rad = IMin(g_cfg.corner * H / 100, IMin(W, H) / 2);
    int bt = IMax(2, H / 28);
    COLORREF ec = g_cfg.locked ? RGB(255, 165, 0) : RGB(0, 175, 255);

    int bR = GetRValue(g_cfg.bgColor), bG = GetGValue(g_cfg.bgColor), bB = GetBValue(g_cfg.bgColor);
    int oR = GetRValue(g_cfg.outlineColor), oG = GetGValue(g_cfg.outlineColor), oB = GetBValue(g_cfg.outlineColor);
    int tR = GetRValue(g_cfg.textColor), tG = GetGValue(g_cfg.textColor), tB = GetBValue(g_cfg.textColor);
    int eR = GetRValue(ec), eG = GetGValue(ec), eB = GetBValue(ec);

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            size_t i = ((size_t)y * W + x) * 4;
            int pc = RoundCov(x, y, W, H, 0, (float)rad);
            int a  = pc * bgA / 255;
            int cr = bR * a / 255, cg = bG * a / 255, cb = bB * a / 255;
            if (g_cfg.outline) {
                int oa = g_pxOutl[i + 1];
                if (oa) {
                    cr = (oR * oa + cr * (255 - oa)) / 255;
                    cg = (oG * oa + cg * (255 - oa)) / 255;
                    cb = (oB * oa + cb * (255 - oa)) / 255;
                    a  = oa + a * (255 - oa) / 255;
                }
            }
            int ta = g_pxText[i + 1];
            if (ta) {
                cr = (tR * ta + cr * (255 - ta)) / 255;
                cg = (tG * ta + cg * (255 - ta)) / 255;
                cb = (tB * ta + cb * (255 - ta)) / 255;
                a  = ta + a * (255 - ta) / 255;
            }
            if (g_edit) {
                int ea = pc - RoundCov(x, y, W, H, (float)bt, (float)(rad - bt));
                if (ea > 0) {
                    cr = (eR * ea + cr * (255 - ea)) / 255;
                    cg = (eG * ea + cg * (255 - ea)) / 255;
                    cb = (eB * ea + cb * (255 - ea)) / 255;
                    a  = ea + a * (255 - ea) / 255;
                }
            }
            g_pxOut[i + 0] = (BYTE)cb;
            g_pxOut[i + 1] = (BYTE)cg;
            g_pxOut[i + 2] = (BYTE)cr;
            g_pxOut[i + 3] = (BYTE)a;
        }
    }

    HDC scr = GetDC(NULL);
    POINT src = { 0, 0 };
    SIZE sz = { W, H };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, (BYTE)(Clamp(g_cfg.opacity, 20, 100) * 255 / 100), AC_SRC_ALPHA };
    UpdateLayeredWindow(g_overlay, scr, NULL, &sz, g_memDC, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(NULL, scr);
}

static void Relayout(void)
{
    g_layoutDirty = TRUE;
    Render();
}

/* ------------------------------------------------------------ 窗口状态 -- */

static void ReassertTopmost(void)
{
    if (g_cfg.topmost && IsWindowVisible(g_overlay))
        SetWindowPos(g_overlay, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

static void ApplyTopmost(void)
{
    SetWindowPos(g_overlay, g_cfg.topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

static void ApplyClickThrough(void)
{
    LONG_PTR ex = GetWindowLongPtrW(g_overlay, GWL_EXSTYLE);
    LONG_PTR nx = (!g_edit && g_cfg.clickThrough) ? (ex | WS_EX_TRANSPARENT) : (ex & ~(LONG_PTR)WS_EX_TRANSPARENT);
    if (nx != ex) SetWindowLongPtrW(g_overlay, GWL_EXSTYLE, nx);
}

/* “仅游戏时显示”：只看前台窗口是否铺满它所在的显示器（6.4） */
static BOOL ForegroundIsFullscreen(void)
{
    HWND fg = GetForegroundWindow();
    if (!fg || fg == g_overlay || fg == g_host || fg == g_panel) return FALSE;
    if (fg == GetShellWindow() || fg == GetDesktopWindow()) return FALSE;
    WCHAR cls[16];
    if (GetClassNameW(fg, cls, 16) && (!lstrcmpW(cls, L"WorkerW") || !lstrcmpW(cls, L"Progman")))
        return FALSE;                  /* 桌面本身 */
    if (!IsWindowVisible(fg) || IsIconic(fg)) return FALSE;
    RECT r;
    if (!GetWindowRect(fg, &r)) return FALSE;
    HMONITOR m = MonitorFromWindow(fg, MONITOR_DEFAULTTONULL);
    if (!m) return FALSE;
    RECT mr = MonitorRect(m);
    return r.left <= mr.left && r.top <= mr.top && r.right >= mr.right && r.bottom >= mr.bottom;
}

static void UpdateVisibility(void)
{
    BOOL fsOk = (g_cfg.showMode == 1) ? ForegroundIsFullscreen() : TRUE;
    BOOL want = g_edit || (g_cfg.visible && fsOk);
    BOOL now  = IsWindowVisible(g_overlay);
    if (want && !now) {
        Render();
        ShowWindow(g_overlay, SW_SHOWNOACTIVATE);
        ApplyTopmost();
    } else if (!want && now) {
        ShowWindow(g_overlay, SW_HIDE);
    }
}

static void ArmTick(void)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    /* 对齐到下一个整秒（不显示秒时对齐整分），加 20ms 余量防早触发（4.5） */
    UINT ms = 1000 - st.wMilliseconds;
    if (!FmtHasSeconds(g_cfg.timeFmt)) ms += (59 - st.wSecond) * 1000;
    ms += 20;
    SetTimer(g_host, IDT_TICK, ms, NULL);
}

static void ResyncClock(void)
{
    KillTimer(g_host, IDT_TICK);
    if (IsWindowVisible(g_overlay)) Render();
    ArmTick();
}

static void ConfigureFsPoll(void)
{
    if (g_cfg.showMode == 1) SetTimer(g_host, IDT_FSPOLL, 1500, NULL);
    else KillTimer(g_host, IDT_FSPOLL);
}

/* ---------------------------------------------------------------- 托盘 -- */

static void TrayAdd(void)
{
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_host;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = g_iconSmall;
    lstrcpynW(g_nid.szTip, L"悬浮时钟 · 点击打开设置", 128);
    g_trayAdded = Shell_NotifyIconW(NIM_ADD, &g_nid);
    if (!g_trayAdded) g_trayAdded = Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void TrayRemove(void)
{
    if (!g_trayAdded) return;
    g_nid.uFlags = 0;
    Shell_NotifyIconW(NIM_DELETE, &g_nid);   /* 不删会留下幽灵图标（B6） */
    g_trayAdded = FALSE;
}

/* ------------------------------------------------------------ 编辑模式 -- */

static void SetEdit(BOOL on)
{
    if (g_edit == on) return;
    g_edit = on;
    if (!on) {
        if (g_dragging) { g_dragging = FALSE; ReleaseCapture(); }
        CaptureToConfig();
        SaveConfig();                  /* 退出编辑模式时保存（4.3） */
    }
    ApplyClickThrough();
    UpdateVisibility();
    Render();
    ReassertTopmost();
}

static void ResetPosition(void)
{
    g_cfg.monitor[0] = 0;
    g_cfg.anchor = 1;
    g_cfg.offX = DEF_OFFX;
    g_cfg.offY = DEF_OFFY;
    PlaceFromConfig();
}

/* 以锚定角为基准改大小（右上锚定时向左下扩展） */
static void ResizeOverlay(int w, int h)
{
    RECT r;
    GetWindowRect(g_overlay, &r);
    RECT mr = MonitorRect(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST));
    w = Clamp(w, MIN_W, mr.right - mr.left);
    h = Clamp(h, MIN_H, (mr.bottom - mr.top) / 2);
    int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    BOOL right  = cx > (mr.left + mr.right) / 2;
    BOOL bottom = cy > (mr.top + mr.bottom) / 2;
    int x = right ? r.right - w : r.left;
    int y = bottom ? r.bottom - h : r.top;
    x = Clamp(x, mr.left, mr.right - w);
    y = Clamp(y, mr.top, mr.bottom - h);
    SetWindowPos(g_overlay, NULL, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    CaptureToConfig();
}

static void FitWidthToText(void)
{
    RECT r;
    GetWindowRect(g_overlay, &r);
    int H = r.bottom - r.top;
    int need = ComputeLayout(r.right - r.left, H, TRUE);
    ResizeOverlay(need, H);
    Relayout();
}

/* 窗口高度设为显示器高度的 frac/10000，宽度贴合文字 */
static void SetHeightFrac(int frac)
{
    RECT r;
    GetWindowRect(g_overlay, &r);
    RECT mr = MonitorRect(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST));
    int h = MulDiv(frac, mr.bottom - mr.top, FRAC);
    ResizeOverlay(r.right - r.left, h);
    FitWidthToText();
}

static void ScaleOverlay(int pct, BOOL widthOnly)
{
    RECT r;
    GetWindowRect(g_overlay, &r);
    int w = r.right - r.left, h = r.bottom - r.top;
    int nw = w * pct / 100, nh = widthOnly ? h : h * pct / 100;
    if (nw == w) nw += pct > 100 ? 1 : -1;
    if (!widthOnly && nh == h) nh += pct > 100 ? 1 : -1;
    ResizeOverlay(nw, nh);
}

/* anchor 0..3 = 四角；-1 = 顶部居中 */
static void SnapTo(int anchor)
{
    CaptureToConfig();                 /* 保留当前所在显示器与大小 */
    g_cfg.offX = DEF_OFFX;
    g_cfg.offY = DEF_OFFY;
    if (anchor >= 0) {
        g_cfg.anchor = anchor;
    } else {
        RECT r, mr;
        GetWindowRect(g_overlay, &r);
        mr = MonitorRect(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST));
        int mw = mr.right - mr.left, w = r.right - r.left;
        g_cfg.anchor = 0;
        g_cfg.offX = MulDiv(IMax(0, (mw - w) / 2), FRAC, mw);
    }
    PlaceFromConfig();
}

static void ApplyAll(void)
{
    AutostartApply();
    PlaceFromConfig();
    ApplyClickThrough();
    ApplyTopmost();
    ConfigureFsPoll();
    UpdateVisibility();
    Relayout();
    ResyncClock();
}

/* ------------------------------------------------------------ 设置面板 -- */

static void PanelSync(void);
static void PanelSyncSize(void);

static int S(int v) { return MulDiv(v, (int)g_panelDpi, 96); }

static BOOL PickColor(COLORREF *c)
{
    CHOOSECOLORW cc;
    ZeroMemory(&cc, sizeof(cc));
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = g_panel;
    cc.rgbResult = *c;
    cc.lpCustColors = g_custColors;
    cc.Flags = CC_RGBINIT | CC_FULLOPEN;
    if (!ChooseColorW(&cc)) return FALSE;
    *c = cc.rgbResult;
    return TRUE;
}

static BOOL PickFont(void)
{
    LOGFONTW lf;
    ZeroMemory(&lf, sizeof(lf));
    lf.lfHeight = -24;
    lf.lfWeight = g_cfg.bold ? FW_BOLD : FW_NORMAL;
    lf.lfItalic = (BYTE)g_cfg.italic;
    lf.lfCharSet = DEFAULT_CHARSET;
    lstrcpynW(lf.lfFaceName, g_cfg.font, LF_FACESIZE);

    CHOOSEFONTW cf;
    ZeroMemory(&cf, sizeof(cf));
    cf.lStructSize = sizeof(cf);
    cf.hwndOwner = g_panel;
    cf.lpLogFont = &lf;
    cf.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_NOVERTFONTS | CF_NOSCRIPTSEL;
    if (!ChooseFontW(&cf)) return FALSE;
    lstrcpynW(g_cfg.font, lf.lfFaceName, LF_FACESIZE);
    g_cfg.bold = lf.lfWeight >= FW_SEMIBOLD;
    g_cfg.italic = lf.lfItalic ? 1 : 0;
    return TRUE;
}

static HWND Ctl(const WCHAR *cls, const WCHAR *text, DWORD style, int x, int y, int w, int h, int id, HFONT f)
{
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                             S(x), S(y), S(w), S(h), g_panel, (HMENU)(INT_PTR)id, g_inst, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)f, FALSE);
    return c;
}

static void Label(int x, int y, int w, const WCHAR *t)
{
    Ctl(L"STATIC", t, SS_LEFT | SS_NOPREFIX, x, y + 5, w, 20, IDC_LABEL, g_uiFont);
}

static void Header(int x, int y, const WCHAR *t)
{
    Ctl(L"STATIC", t, SS_LEFT | SS_NOPREFIX, x, y, 340, 22, IDC_HEADER, g_uiBold);
}

static void Check(int x, int y, int w, const WCHAR *t, int id)
{
    Ctl(L"BUTTON", t, BS_AUTOCHECKBOX | WS_TABSTOP, x, y, w, 24, id, g_uiFont);
}

static void Button(int x, int y, int w, const WCHAR *t, int id)
{
    Ctl(L"BUTTON", t, BS_PUSHBUTTON | WS_TABSTOP, x, y, w, 28, id, g_uiFont);
}

static void Swatch(int x, int y, int id)
{
    Ctl(L"BUTTON", L"", BS_OWNERDRAW | WS_TABSTOP, x, y, 56, 26, id, g_uiFont);
}

static void Slider(int x, int y, int id, int lo, int hi, int valId)
{
    HWND t = Ctl(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, x, y, 200, 28, id, g_uiFont);
    SendMessageW(t, TBM_SETRANGE, FALSE, MAKELPARAM(lo, hi));
    SendMessageW(t, TBM_SETPAGESIZE, 0, 5);
    Ctl(L"STATIC", L"", SS_LEFT, x + 204, y + 5, 52, 20, valId, g_uiFont);
}

static void Combo(int x, int y, int w, int id, BOOL editable, const WCHAR **items, int n)
{
    HWND c = Ctl(L"COMBOBOX", L"",
                 (editable ? (CBS_DROPDOWN | CBS_AUTOHSCROLL) : CBS_DROPDOWNLIST) | WS_VSCROLL | WS_TABSTOP,
                 x, y, w, 240, id, g_uiFont);
    for (int i = 0; i < n; i++) SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)items[i]);
}

static void BuildPanelControls(void)
{
    if (g_uiFont) DeleteObject(g_uiFont);
    if (g_uiBold) DeleteObject(g_uiBold);
    g_uiFont = CreateFontW(-MulDiv(9, (int)g_panelDpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH, UI_FONT);
    g_uiBold = CreateFontW(-MulDiv(10, (int)g_panelDpi, 72), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH, UI_FONT);

    const int L = 20, R = 400, CX = 96;
    static const WCHAR *modes[] = { L"始终显示", L"仅在全屏程序（游戏）在前台时显示" };

    /* ---- 左列：显示 ---- */
    Header(L, 14, L"显示");
    Check(L, 42, 160, L"显示时钟", IDC_VISIBLE);
    Check(L + 170, 42, 170, L"置顶", IDC_TOPMOST);
    Check(L, 68, 160, L"游戏中鼠标穿透", IDC_CLICKTHRU);
    Check(L + 170, 68, 170, L"锁定位置和大小", IDC_LOCK);
    Label(L, 98, 90, L"显示时机");
    Combo(L + CX, 98, 244, IDC_SHOWMODE, FALSE, modes, 2);

    /* ---- 左列：时间与日期 ---- */
    Header(L, 142, L"时间与日期");
    Label(L, 170, 90, L"时间格式");
    Combo(L + CX, 170, 136, IDC_TIMEFMT, TRUE, kTimePresets, N_TIME_PRESETS);
    Ctl(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, L + CX + 144, 175, 100, 20, IDC_TIMEPREV, g_uiFont);
    Check(L, 202, 92, L"显示日期", IDC_SHOWDATE);
    Combo(L + CX, 202, 136, IDC_DATEFMT, TRUE, kDatePresets, N_DATE_PRESETS);
    Ctl(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, L + CX + 144, 207, 100, 20, IDC_DATEPREV, g_uiFont);
    Ctl(L"STATIC",
        L"格式可直接输入：HH 24时  h 12时  mm 分  ss 秒\r\n"
        L"tt 上午/下午  yyyy 年  MM 月  dd 日  ddd 周五",
        SS_LEFT | SS_NOPREFIX, L, 236, 340, 36, IDC_HINT, g_uiFont);

    /* ---- 左列：其他 ---- */
    Header(L, 284, L"其他");
    Check(L, 312, 100, L"开机自启", IDC_AUTOSTART);
    Button(L + 110, 310, 110, L"打开配置文件", IDC_OPENCFG);
    Button(L + 228, 310, 112, L"重新加载配置", IDC_RELOAD);

    /* ---- 右列：外观 ---- */
    Header(R, 14, L"外观");
    Label(R, 42, 90, L"字体");
    Button(R + CX, 40, 244, L"", IDC_FONT);
    Label(R, 74, 90, L"文字颜色");
    Swatch(R + CX, 74, IDC_TEXTCOLOR);
    Check(R + CX + 70, 75, 62, L"描边", IDC_OUTLINE);
    Swatch(R + CX + 134, 74, IDC_OUTLINECOLOR);
    Label(R, 106, 90, L"底板颜色");
    Swatch(R + CX, 106, IDC_BGCOLOR);
    Label(R, 136, 92, L"底板不透明度");
    Slider(R + 88, 136, IDC_BGOP, 0, 100, IDC_BGOP_VAL);
    Label(R, 166, 90, L"圆角");
    Slider(R + 88, 166, IDC_CORNER, 0, 50, IDC_CORNER_VAL);
    Label(R, 196, 92, L"整体不透明度");
    Slider(R + 88, 196, IDC_OPACITY, 20, 100, IDC_OPACITY_VAL);

    /* ---- 右列：位置与大小 ---- */
    Header(R, 236, L"位置与大小");
    static const WCHAR *pos[] = { L"左上", L"右上", L"左下", L"右下", L"顶部居中" };
    for (int i = 0; i < 5; i++)
        Button(R + i * 69, 262, i == 4 ? 64 : 63, pos[i], IDC_POS_TL + i);
    Label(R, 298, 90, L"大小");
    Slider(R + 88, 298, IDC_SIZESLD, 20, 200, IDC_SIZE_VAL);
    Button(R, 330, 166, L"宽度贴合文字", IDC_FITW);
    Button(R + 174, 330, 166, L"移回主屏右上角", IDC_RESETPOS);

    /* ---- 底部 ---- */
    Ctl(L"STATIC", L"", SS_ETCHEDHORZ, 20, 374, 720, 2, IDC_LABEL, g_uiFont);
    Ctl(L"STATIC", L"面板打开时时钟始终可见：可直接拖动，滚轮缩放（Shift+滚轮只改宽度）。",
        SS_LEFT | SS_NOPREFIX, 20, 392, 430, 20, IDC_HINT, g_uiFont);
    Button(468, 386, 84, L"恢复默认", IDC_DEFAULTS);
    Button(560, 386, 84, L"退出程序", IDC_EXIT);
    Ctl(L"BUTTON", L"完成", BS_DEFPUSHBUTTON | WS_TABSTOP, 652, 386, 88, 28, IDOK, g_uiFont);
}

#define PANEL_CW 760
#define PANEL_CH 430
#define PANEL_STYLE (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX)
#define PANEL_EXSTYLE (WS_EX_CONTROLPARENT)

static SIZE PanelWindowSize(void)
{
    RECT rc = { 0, 0, S(PANEL_CW), S(PANEL_CH) };
    AdjustWindowRectExForDpi(&rc, PANEL_STYLE, FALSE, PANEL_EXSTYLE, g_panelDpi);
    SIZE s = { rc.right - rc.left, rc.bottom - rc.top };
    return s;
}

static void SetCheck(int id, BOOL on) { SendDlgItemMessageW(g_panel, id, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0); }
static BOOL GetCheck(int id) { return SendDlgItemMessageW(g_panel, id, BM_GETCHECK, 0, 0) == BST_CHECKED; }
static void SetTb(int id, int v) { SendDlgItemMessageW(g_panel, id, TBM_SETPOS, TRUE, v); }

static void UpdateValueLabels(void)
{
    WCHAR b[32];
    wsprintfW(b, L"%d%%", g_cfg.bgOpacity); SetDlgItemTextW(g_panel, IDC_BGOP_VAL, b);
    wsprintfW(b, L"%d%%", g_cfg.corner);    SetDlgItemTextW(g_panel, IDC_CORNER_VAL, b);
    wsprintfW(b, L"%d%%", g_cfg.opacity);   SetDlgItemTextW(g_panel, IDC_OPACITY_VAL, b);
}

static void UpdatePreviews(void)
{
    if (!g_panel) return;
    SYSTEMTIME st;
    WCHAR b[64];
    GetLocalTime(&st);
    WCHAR cur[64];
    GetDlgItemTextW(g_panel, IDC_TIMEFMT, cur, 64);
    g_timeFmtBad = !FormatTimeStr(&st, cur, b, 64);
    SetDlgItemTextW(g_panel, IDC_TIMEPREV, g_timeFmtBad ? L"格式无效" : b);
    GetDlgItemTextW(g_panel, IDC_DATEFMT, cur, 64);
    g_dateFmtBad = !FormatDateStr(&st, cur, b, 64);
    SetDlgItemTextW(g_panel, IDC_DATEPREV, g_dateFmtBad ? L"格式无效" : b);
}

static void UpdateFontButton(void)
{
    WCHAR b[96];
    wsprintfW(b, L"%s%s%s", g_cfg.font, g_cfg.bold ? L"，粗体" : L"", g_cfg.italic ? L"，斜体" : L"");
    SetDlgItemTextW(g_panel, IDC_FONT, b);
}

static void UpdateEnables(void)
{
    BOOL movable = !g_cfg.locked;
    static const int ids[] = { IDC_POS_TL, IDC_POS_TR, IDC_POS_BL, IDC_POS_BR, IDC_POS_TC,
                               IDC_SIZESLD, IDC_FITW, IDC_RESETPOS };
    for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); i++)
        EnableWindow(GetDlgItem(g_panel, ids[i]), movable);
    EnableWindow(GetDlgItem(g_panel, IDC_OUTLINECOLOR), g_cfg.outline);
}

/* 大小滑块：窗口高度占显示器高度的千分比（20 = 2.0%） */
static void SetSizeLabel(int permille)
{
    WCHAR b[32];
    wsprintfW(b, L"%d.%d%%", permille / 10, permille % 10);
    SetDlgItemTextW(g_panel, IDC_SIZE_VAL, b);
}

static void PanelSyncSize(void)
{
    if (!g_panel) return;
    int v = Clamp((g_cfg.sizeH + 5) / 10, 20, 200);
    SetTb(IDC_SIZESLD, v);
    SetSizeLabel(v);
}

static void PanelSync(void)
{
    if (!g_panel) return;
    g_syncing = TRUE;
    SetCheck(IDC_VISIBLE, g_cfg.visible);
    SetCheck(IDC_TOPMOST, g_cfg.topmost);
    SetCheck(IDC_CLICKTHRU, g_cfg.clickThrough);
    SetCheck(IDC_LOCK, g_cfg.locked);
    SendDlgItemMessageW(g_panel, IDC_SHOWMODE, CB_SETCURSEL, g_cfg.showMode, 0);
    SetDlgItemTextW(g_panel, IDC_TIMEFMT, g_cfg.timeFmt);
    SetCheck(IDC_SHOWDATE, g_cfg.showDate);
    SetDlgItemTextW(g_panel, IDC_DATEFMT, g_cfg.dateFmt);
    SetCheck(IDC_AUTOSTART, g_cfg.autostart);
    SetCheck(IDC_OUTLINE, g_cfg.outline);
    SetTb(IDC_BGOP, g_cfg.bgOpacity);
    SetTb(IDC_CORNER, g_cfg.corner);
    SetTb(IDC_OPACITY, g_cfg.opacity);
    UpdateValueLabels();
    UpdateFontButton();
    UpdateEnables();
    UpdatePreviews();
    PanelSyncSize();
    InvalidateRect(g_panel, NULL, TRUE);
    g_syncing = FALSE;
}

static void PanelCenterOnOverlayMonitor(void)
{
    RECT ov;
    GetWindowRect(g_overlay, &ov);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromRect(&ov, MONITOR_DEFAULTTONEAREST), &mi);
    SIZE s = PanelWindowSize();
    RECT wa = mi.rcWork;
    int x = wa.left + (wa.right - wa.left - s.cx) / 2;
    int y = wa.top + (wa.bottom - wa.top - s.cy) / 2;
    x = Clamp(x, wa.left, IMax(wa.left, wa.right - s.cx));
    y = Clamp(y, wa.top, IMax(wa.top, wa.bottom - s.cy));
    SetWindowPos(g_panel, NULL, x, y, s.cx, s.cy, SWP_NOZORDER | SWP_NOACTIVATE);
}

static void CreatePanel(void)
{
    g_panel = CreateWindowExW(PANEL_EXSTYLE, PANEL_CLASS, L"悬浮时钟 · 设置", PANEL_STYLE,
                              CW_USEDEFAULT, CW_USEDEFAULT, 400, 300, NULL, NULL, g_inst, NULL);
    if (!g_panel) return;
    g_panelDpi = GetDpiForWindow(g_panel);
    if (!g_panelDpi) g_panelDpi = 96;
    PanelCenterOnOverlayMonitor();     /* 可能触发 WM_DPICHANGED，那里会自行重建 */
    if (!GetDlgItem(g_panel, IDOK)) BuildPanelControls();
    PanelSync();
}

static void OpenPanel(void)
{
    if (!g_panel) CreatePanel();
    if (!g_panel) return;
    ShowWindow(g_panel, IsIconic(g_panel) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(g_panel);
    SetEdit(TRUE);
    PanelSync();
}

static void ClosePanel(void)
{
    if (g_panel) {
        HWND p = g_panel;
        g_panel = NULL;
        DestroyWindow(p);
    }
    SetEdit(FALSE);                    /* 回到游戏模式并保存 */
}

/* 找回：重复启动 / 托盘（A2）。显示时钟、拉回屏幕内、打开设置面板 */
static void Recover(BOOL resetPos)
{
    if (!g_trayAdded) TrayAdd();
    if (resetPos) ResetPosition();
    else { CaptureToConfig(); PlaceFromConfig(); }
    g_cfg.visible = 1;
    OpenPanel();
    SaveConfig();
}

static void ApplyTimeFmt(const WCHAR *f)
{
    if (!TimeFmtValid(f)) { UpdatePreviews(); InvalidateRect(GetDlgItem(g_panel, IDC_TIMEPREV), NULL, TRUE); return; }
    lstrcpynW(g_cfg.timeFmt, f, 64);
    FitWidthToText();
    ResyncClock();
    PanelSyncSize();
    UpdatePreviews();
    InvalidateRect(GetDlgItem(g_panel, IDC_TIMEPREV), NULL, TRUE);
    SaveLater();
}

static void ApplyDateFmt(const WCHAR *f)
{
    if (!DateFmtValid(f)) { UpdatePreviews(); InvalidateRect(GetDlgItem(g_panel, IDC_DATEPREV), NULL, TRUE); return; }
    lstrcpynW(g_cfg.dateFmt, f, 64);
    if (g_cfg.showDate) FitWidthToText();
    PanelSyncSize();
    UpdatePreviews();
    InvalidateRect(GetDlgItem(g_panel, IDC_DATEPREV), NULL, TRUE);
    SaveLater();
}

static void ComboText(HWND p, int id, WORD code, WCHAR *out)
{
    out[0] = 0;
    HWND c = GetDlgItem(p, id);
    if (code == CBN_SELCHANGE) {
        LRESULT i = SendMessageW(c, CB_GETCURSEL, 0, 0);
        if (i != CB_ERR && SendMessageW(c, CB_GETLBTEXTLEN, i, 0) < 64)
            SendMessageW(c, CB_GETLBTEXT, i, (LPARAM)out);
    } else {
        GetWindowTextW(c, out, 64);
    }
}

static void PanelCommand(HWND p, int id, WORD code)
{
    WCHAR t[64];
    switch (id) {
    case IDOK:
    case IDCANCEL:
        ClosePanel();
        return;
    case IDC_VISIBLE:   g_cfg.visible = GetCheck(id); UpdateVisibility(); break;
    case IDC_TOPMOST:   g_cfg.topmost = GetCheck(id); ApplyTopmost(); break;
    case IDC_CLICKTHRU: g_cfg.clickThrough = GetCheck(id); ApplyClickThrough(); break;
    case IDC_LOCK:      g_cfg.locked = GetCheck(id); UpdateEnables(); Render(); break;
    case IDC_SHOWMODE:
        if (code != CBN_SELCHANGE) return;
        g_cfg.showMode = (int)SendDlgItemMessageW(p, id, CB_GETCURSEL, 0, 0) == 1;
        ConfigureFsPoll();
        UpdateVisibility();
        break;
    case IDC_TIMEFMT:
        if (code != CBN_SELCHANGE && code != CBN_EDITCHANGE) return;
        ComboText(p, id, code, t);
        ApplyTimeFmt(t);
        return;
    case IDC_DATEFMT:
        if (code != CBN_SELCHANGE && code != CBN_EDITCHANGE) return;
        ComboText(p, id, code, t);
        ApplyDateFmt(t);
        return;
    case IDC_SHOWDATE: {
        RECT r;
        GetWindowRect(g_overlay, &r);
        int h = r.bottom - r.top;
        g_cfg.showDate = GetCheck(id);
        h = g_cfg.showDate ? h * DATE_HEIGHT_RATIO / 100 : h * 100 / DATE_HEIGHT_RATIO;
        ResizeOverlay(r.right - r.left, h);
        FitWidthToText();
        PanelSyncSize();
        break;
    }
    case IDC_AUTOSTART: g_cfg.autostart = GetCheck(id); AutostartApply(); break;
    case IDC_OPENCFG: {
        SaveConfig();
        WCHAR arg[MAX_PATH + 4];
        wsprintfW(arg, L"\"%s\"", g_cfgPath);
        ShellExecuteW(p, L"open", L"notepad.exe", arg, NULL, SW_SHOWNORMAL);
        return;
    }
    case IDC_RELOAD:
        LoadConfig();
        ApplyAll();
        PanelSync();
        return;
    case IDC_FONT:
        if (!PickFont()) return;
        FitWidthToText();
        UpdateFontButton();
        PanelSyncSize();
        break;
    case IDC_TEXTCOLOR:    if (!PickColor(&g_cfg.textColor)) return;    Render(); InvalidateRect(GetDlgItem(p, id), NULL, TRUE); break;
    case IDC_OUTLINECOLOR: if (!PickColor(&g_cfg.outlineColor)) return; Render(); InvalidateRect(GetDlgItem(p, id), NULL, TRUE); break;
    case IDC_BGCOLOR:      if (!PickColor(&g_cfg.bgColor)) return;      Render(); InvalidateRect(GetDlgItem(p, id), NULL, TRUE); break;
    case IDC_OUTLINE:      g_cfg.outline = GetCheck(id); UpdateEnables(); Render(); break;
    case IDC_POS_TL: case IDC_POS_TR: case IDC_POS_BL: case IDC_POS_BR:
        SnapTo(id - IDC_POS_TL);
        break;
    case IDC_POS_TC:  SnapTo(-1); break;
    case IDC_FITW:    FitWidthToText(); PanelSyncSize(); break;
    case IDC_RESETPOS:ResetPosition(); CaptureToConfig(); break;
    case IDC_DEFAULTS:
        if (MessageBoxW(p, L"把所有设置恢复为默认值？", APP_TITLE, MB_YESNO | MB_ICONQUESTION) != IDYES) return;
        {
            int as = g_cfg.autostart;
            SetDefaults(&g_cfg);
            g_cfg.autostart = as;
            ApplyAll();
            FitWidthToText();
            PanelSync();
        }
        break;
    case IDC_EXIT:
        if (MessageBoxW(p, L"退出后时钟将关闭。确定退出？", APP_TITLE, MB_YESNO | MB_ICONQUESTION) != IDYES) return;
        DestroyWindow(g_host);
        return;
    default:
        return;
    }
    SaveLater();
}

static void PanelSlider(HWND tb)
{
    int id = GetDlgCtrlID(tb);
    int v = (int)SendMessageW(tb, TBM_GETPOS, 0, 0);
    switch (id) {
    case IDC_BGOP:    g_cfg.bgOpacity = Clamp(v, 0, 100); Render(); break;
    case IDC_CORNER:  g_cfg.corner = Clamp(v, 0, 50); Render(); break;
    case IDC_OPACITY: g_cfg.opacity = Clamp(v, 20, 100); Render(); break;
    case IDC_SIZESLD:    v = Clamp(v, 20, 200); SetHeightFrac(v * 10); SetSizeLabel(v); break;
    default: return;
    }
    UpdateValueLabels();
    SaveLater();
}

static void DrawSwatch(const DRAWITEMSTRUCT *d)
{
    COLORREF c = d->CtlID == IDC_TEXTCOLOR ? g_cfg.textColor :
                 d->CtlID == IDC_OUTLINECOLOR ? g_cfg.outlineColor : g_cfg.bgColor;
    BOOL disabled = (d->itemState & ODS_DISABLED) != 0;
    RECT r = d->rcItem;
    FillRect(d->hDC, &r, GetSysColorBrush(COLOR_WINDOW));
    HBRUSH frame = CreateSolidBrush((d->itemState & ODS_FOCUS) ? RGB(0, 120, 215) : RGB(160, 160, 160));
    FrameRect(d->hDC, &r, frame);
    DeleteObject(frame);
    InflateRect(&r, -S(3), -S(3));
    HBRUSH b = CreateSolidBrush(disabled ? RGB(220, 220, 220) : c);
    FillRect(d->hDC, &r, b);
    DeleteObject(b);
    if (d->itemState & ODS_SELECTED) {
        InflateRect(&r, 1, 1);
        DrawFocusRect(d->hDC, &r);
    }
}

static LRESULT CALLBACK PanelProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        if (!g_syncing) PanelCommand(h, LOWORD(wp), HIWORD(wp));
        return 0;

    case WM_HSCROLL:
        if (!g_syncing && lp) PanelSlider((HWND)lp);
        return 0;

    case WM_DRAWITEM:
        DrawSwatch((const DRAWITEMSTRUCT *)lp);
        return TRUE;

    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        int id = GetDlgCtrlID((HWND)lp);
        SetBkColor(dc, GetSysColor(COLOR_WINDOW));
        if (id == IDC_HINT) SetTextColor(dc, RGB(110, 110, 110));
        else if (id == IDC_TIMEPREV) SetTextColor(dc, g_timeFmtBad ? RGB(200, 40, 40) : RGB(0, 110, 190));
        else if (id == IDC_DATEPREV) SetTextColor(dc, g_dateFmtBad ? RGB(200, 40, 40) : RGB(0, 110, 190));
        else SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }

    case DM_GETDEFID:
        return MAKELRESULT(IDOK, DC_HASDEFID);

    case WM_DPICHANGED: {
        g_panelDpi = HIWORD(wp);
        HWND c;
        while ((c = GetWindow(h, GW_CHILD)) != NULL) DestroyWindow(c);
        BuildPanelControls();
        const RECT *s = (const RECT *)lp;
        SIZE sz = PanelWindowSize();
        SetWindowPos(h, NULL, s->left, s->top, sz.cx, sz.cy, SWP_NOZORDER | SWP_NOACTIVATE);
        if (g_panel == h) PanelSync();
        return 0;
    }

    case WM_CLOSE:
        ClosePanel();
        return 0;

    case WM_DESTROY:
        if (g_panel == h) g_panel = NULL;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* ------------------------------------------------------------ 窗口过程 -- */

static void CALLBACK OnWinEvent(HWINEVENTHOOK h, DWORD ev, HWND hwnd, LONG idObj, LONG idChild,
                                DWORD thread, DWORD time)
{
    (void)h; (void)ev; (void)hwnd; (void)idObj; (void)idChild; (void)thread; (void)time;
    /* 前台窗口变化：此时重申置顶最有效，又不会频繁抢层级（B5） */
    if (g_cfg.showMode == 1) UpdateVisibility();
    ReassertTopmost();
}

static LRESULT CALLBACK HostProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == g_msgTaskbarCreated && g_msgTaskbarCreated) {
        g_trayAdded = FALSE;           /* 资源管理器重启后重建托盘图标 */
        TrayAdd();
        return 0;
    }
    switch (msg) {
    case WM_TIMER:
        switch (wp) {
        case IDT_TICK:
            KillTimer(h, IDT_TICK);
            if (IsWindowVisible(g_overlay)) Render();
            if (g_panel) UpdatePreviews();
            ArmTick();
            break;
        case IDT_TOPMOST:  ReassertTopmost(); break;
        case IDT_FSPOLL:   UpdateVisibility(); break;
        case IDT_RELAYOUT:
            KillTimer(h, IDT_RELAYOUT);
            if (!g_dragging) PlaceFromConfig();
            Relayout();
            ReassertTopmost();
            break;
        case IDT_SAVE:     SaveConfig(); break;
        }
        return 0;

    case WM_TRAY:                      /* 点击托盘图标（左键或右键）打开设置面板 */
        switch (LOWORD(lp)) {
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
            OpenPanel();
            break;
        }
        return 0;

    case WM_CO_RECOVER: Recover(FALSE); return 0;
    case WM_CO_EXIT:    DestroyWindow(h); return 0;

    case WM_DISPLAYCHANGE:             /* 分辨率 / 显示器变化：稍后按比例重新摆放 */
        SetTimer(h, IDT_RELAYOUT, 400, NULL);
        return 0;

    case WM_SETTINGCHANGE:
        if (lp && !lstrcmpiW((const WCHAR *)lp, L"intl")) { Relayout(); ResyncClock(); }
        return 0;

    case WM_TIMECHANGE:                /* 手动改时间、时区、夏令时 */
        ResyncClock();
        return 0;

    case WM_POWERBROADCAST:
        if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) {
            ResyncClock();
            SetTimer(h, IDT_RELAYOUT, 1000, NULL);
        }
        return TRUE;

    case WM_QUERYENDSESSION:
        return TRUE;

    case WM_ENDSESSION:
        if (wp) { CaptureToConfig(); SaveConfig(); TrayRemove(); }
        return 0;

    case WM_DESTROY:
        if (g_edit) CaptureToConfig();
        SaveConfig();                  /* 正常退出时保存 */
        if (g_hook) UnhookWinEvent(g_hook);
        TrayRemove();
        if (g_panel) { HWND p = g_panel; g_panel = NULL; DestroyWindow(p); }
        if (g_overlay) DestroyWindow(g_overlay);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static LRESULT CALLBACK OverlayProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;          /* 点击也不抢焦点 */

    case WM_SETCURSOR:
        SetCursor(LoadCursorW(NULL, (g_edit && !g_cfg.locked) ? IDC_SIZEALL : IDC_ARROW));
        return TRUE;

    case WM_LBUTTONDOWN:               /* 手动实现拖动（B3） */
        if (g_edit && !g_cfg.locked) {
            g_dragging = TRUE;
            SetCapture(h);
            GetCursorPos(&g_dragOrigin);
            GetWindowRect(h, &g_dragRect);
        }
        return 0;

    case WM_MOUSEMOVE:
        if (g_dragging) {
            POINT p;
            GetCursorPos(&p);
            SetWindowPos(h, NULL, g_dragRect.left + p.x - g_dragOrigin.x,
                         g_dragRect.top + p.y - g_dragOrigin.y, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;

    case WM_LBUTTONUP:
        if (g_dragging) {
            g_dragging = FALSE;
            ReleaseCapture();
            CaptureToConfig();
            PlaceFromConfig();         /* 拖出屏幕的部分拉回来 */
            SaveConfig();              /* 松开时保存，拖动过程中不写盘 */
        }
        return 0;

    case WM_CAPTURECHANGED:
        if (g_dragging) {
            g_dragging = FALSE;
            CaptureToConfig();
            SaveLater();
        }
        return 0;

    case WM_LBUTTONDBLCLK:             /* 双击：编辑中 = 完成；未穿透时 = 打开设置 */
        if (g_edit) ClosePanel();
        else OpenPanel();
        return 0;

    case WM_MOUSEWHEEL:                /* 滚轮缩放（B4）；Shift 只改宽度 */
        if (g_edit && !g_cfg.locked) {
            int d = GET_WHEEL_DELTA_WPARAM(wp);
            BOOL wOnly = (GET_KEYSTATE_WPARAM(wp) & MK_SHIFT) != 0;
            ScaleOverlay(d > 0 ? 105 : 95, wOnly);
            PanelSyncSize();
            SaveLater();
        }
        return 0;

    case WM_SIZE:
        g_layoutDirty = TRUE;
        Render();
        return 0;

    case WM_DPICHANGED:
        if (g_dragging) {
            const RECT *s = (const RECT *)lp;
            SetWindowPos(h, NULL, s->left, s->top, s->right - s->left, s->bottom - s->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        } else {
            PlaceFromConfig();
        }
        Relayout();
        return 0;

    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* ---------------------------------------------------------------- 入口 -- */

/* 等待第二实例的信号，转成消息投递给 host。
   投递的消息在颜色 / 字体对话框等模态循环中也会被处理，找回通道不会被卡住。 */
static DWORD WINAPI SignalThread(LPVOID arg)
{
    (void)arg;
    HANDLE evs[2] = { g_evRecover, g_evExit };
    for (;;) {
        DWORD w = WaitForMultipleObjects(2, evs, FALSE, INFINITE);
        if (w == WAIT_OBJECT_0) PostMessageW(g_host, WM_CO_RECOVER, 0, 0);
        else if (w == WAIT_OBJECT_0 + 1) { PostMessageW(g_host, WM_CO_EXIT, 0, 0); return 0; }
        else return 0;
    }
}

static BOOL HasArg(const WCHAR *cmd, const WCHAR *a1, const WCHAR *a2)
{
    WCHAR b[64];
    if (!cmd) return FALSE;
    lstrcpynW(b, cmd, 64);
    Trim(b);
    return !lstrcmpiW(b, a1) || !lstrcmpiW(b, a2);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmd, int show)
{
    (void)prev; (void)show;
    g_inst = inst;
    BOOL wantExit = HasArg(cmd, L"/exit", L"--exit");

    /* 单实例：已有实例时通知它“显示、回到屏幕内、打开设置面板”，自己退出 */
    HANDLE mtx = CreateMutexW(NULL, FALSE, MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        AllowSetForegroundWindow(ASFW_ANY);   /* 让已有实例能把设置面板带到前台 */
        HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, wantExit ? EV_EXIT : EV_RECOVER);
        if (ev) { SetEvent(ev); CloseHandle(ev); }
        if (mtx) CloseHandle(mtx);
        return 0;
    }
    if (wantExit) { if (mtx) CloseHandle(mtx); return 0; }
    g_evRecover = CreateEventW(NULL, FALSE, FALSE, EV_RECOVER);
    g_evExit    = CreateEventW(NULL, FALSE, FALSE, EV_EXIT);

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    ResolveConfigPath();
    BOOL firstRun = !FileExists(g_cfgPath);
    LoadConfig();

    g_iconSmall = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                    GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    g_iconBig   = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                    GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    if (!g_iconSmall) g_iconSmall = LoadIconW(NULL, IDI_APPLICATION);

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.hInstance = inst;
    wc.hIcon = g_iconBig;
    wc.hIconSm = g_iconSmall;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);

    wc.lpfnWndProc = HostProc;
    wc.lpszClassName = HOST_CLASS;
    if (!RegisterClassExW(&wc)) return 1;

    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = OverlayProc;
    wc.lpszClassName = OVL_CLASS;
    if (!RegisterClassExW(&wc)) return 1;

    wc.style = 0;
    wc.lpfnWndProc = PanelProc;
    wc.lpszClassName = PANEL_CLASS;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    if (!RegisterClassExW(&wc)) return 1;

    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    g_host = CreateWindowExW(WS_EX_TOOLWINDOW, HOST_CLASS, APP_TITLE, WS_POPUP,
                             0, 0, 0, 0, NULL, NULL, inst, NULL);
    if (!g_host) return 2;
    ChangeWindowMessageFilterEx(g_host, g_msgTaskbarCreated, MSGFLT_ALLOW, NULL);

    RECT r;
    ComputeRectFromConfig(&r);
    /* 不进任务栏 / Alt+Tab、不可激活、分层（逐像素 alpha） */
    g_overlay = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | (g_cfg.topmost ? WS_EX_TOPMOST : 0),
        OVL_CLASS, APP_TITLE, WS_POPUP,
        r.left, r.top, r.right - r.left, r.bottom - r.top,
        NULL, NULL, inst, NULL);
    if (!g_overlay) return 3;

    TrayAdd();
    AutostartApply();

    g_hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, NULL, OnWinEvent,
                             0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    SetTimer(g_host, IDT_TOPMOST, 5000, NULL);
    ConfigureFsPoll();

    ApplyClickThrough();
    Render();
    UpdateVisibility();
    ArmTick();

    if (firstRun) {                    /* 第一次运行：直接打开设置面板 */
        FitWidthToText();
        SaveConfig();
        OpenPanel();
    }

    if (g_evRecover && g_evExit) {
        HANDLE th = CreateThread(NULL, 0, SignalThread, NULL, 0, NULL);
        if (th) CloseHandle(th);
    }

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (g_panel && IsDialogMessageW(g_panel, &m)) continue;   /* Tab / Enter / Esc */
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    FreeLineFont(&g_lfTime);
    FreeLineFont(&g_lfDate);
    FreeSurfaces();
    if (g_memDC) DeleteDC(g_memDC);
    if (g_uiFont) DeleteObject(g_uiFont);
    if (g_uiBold) DeleteObject(g_uiBold);
    if (g_evRecover) CloseHandle(g_evRecover);
    if (g_evExit) CloseHandle(g_evExit);
    if (mtx) CloseHandle(mtx);
    return 0;
}
