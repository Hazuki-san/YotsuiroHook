#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cstdio>
#include <cstdint>
#include <string>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <fstream>
#include <mutex>
#include <vector>
#include <algorithm>
#include <functional>
#include <atomic>
#include <memory>
#include <chrono>
#include <MinHook.h>
#include <discord_rpc.h>
#include "proxy.h"

//=============================================================================
// Function Offsets (ImageBase 0x10000000)
//=============================================================================
namespace Offsets {
    constexpr uintptr_t AdvCharSay       = 0x00039A10; // RetouchAdvCharacter::say()
    constexpr uintptr_t PrintSub         = 0x000499D0; // RetouchPrintManager::printSub()
    constexpr uintptr_t PrintEx          = 0x00049A50; // RetouchPrintManager::printEx()
    constexpr uintptr_t DrawHistory      = 0x0004B380; // RetouchPrintManager::drawHistory()
    constexpr uintptr_t SaveDataTitle    = 0x0004D470; // RetouchSaveDataControl::title()
    constexpr uintptr_t SaveDataIsValid  = 0x0004C210; // RetouchSaveDataControl::isValid()
    constexpr uintptr_t SaveDataGetItem  = 0x0004C2E0; // RetouchSaveDataControl::getItem()
    constexpr uintptr_t PrepareQuestion  = 0x000A5A20; // RetouchSystem::prepareQuestion()
    constexpr uintptr_t CalcIniValue     = 0x000ACCD0; // RetouchSystem::calcIniValue()
    constexpr uintptr_t LiteSetDebugMode = 0x000B20D0; // RetouchSystem::liteSetDebugMode()
    constexpr uintptr_t LiteLoad         = 0x000C11D0; // RetouchSystem::liteLoad()
    constexpr uintptr_t CmdMessage       = 0x000CF370; // RetouchSystem::cmdMessage()
}

//=============================================================================
// Print Context Layout (resident.dll)
//=============================================================================
// The print context stores the message box geometry and font. It is the only
// structure that tells us the usable line width. Reach it from the print
// manager by following two pointers:
//
//   ctx = *(void**)(*(void**)(printMgr + Target) + FCStringData)
//
// The engine lays out text using three values from this context:
//
//   newline position : x = RectLeft + Indent,  y = PenY + LineHeight
//   usable width     : RectRight - (RectLeft + Indent)
//   pen advance      : x += GLYPHMETRICS.gmCellIncX, per character
//
// Note on pen advance: the engine does not use gmCellIncX directly. The glyph
// rasterizer stores gmCellIncX + 4, and the pen-movement routine subtracts 4
// when applying the stored value. The two adjustments cancel, so each
// character advances the pen by exactly gmCellIncX.
//
// gmCellIncX comes from the installed font file, not from the engine. Two
// machines with different builds of MS Gothic or MS PGothic produce different
// pixel widths for the same text. This is why line breaks must be computed
// from pixel measurements, not character counts.
//
// These offsets are specific to this game's resident.dll. To port this hook
// to another ExHIBIT title, re-derive this table; the rest of the WordWrap
// code does not depend on the engine.
namespace PrintMgrOffsets {
    constexpr size_t Target       = 0x38;  // -> FCString-like holder
    constexpr size_t FCStringData = 0x14;  // holder -> print context
}

namespace PrintCtxOffsets {
    constexpr size_t Flags      = 0x004;   // bit 2 set = vertical writing
    constexpr size_t RectLeft   = 0x150;   // FCRect: text box, screen pixels
    constexpr size_t RectTop    = 0x154;
    constexpr size_t RectRight  = 0x158;
    constexpr size_t RectBottom = 0x15C;
    constexpr size_t PenX       = 0x164;   // caret, screen pixels
    constexpr size_t PenY       = 0x168;
    constexpr size_t LineHeight = 0x178;   // added to PenY on newline
    constexpr size_t Indent     = 0x17C;   // added to RectLeft on newline
    constexpr size_t LogFont    = 0x1A4;   // LOGFONTA; this context's font
    constexpr size_t FontHandle = 0x1E0;   // HFONT built from LogFont
    constexpr size_t AntiAlias  = 0x23D;   // byte; selects the GGO_* format
}

//=============================================================================
// Shift-JIS Character Classes (kinsoku)
//=============================================================================
//   0x8141-0x8149  、。，．・：；？！
//   0x815B         ー
//   0x8165-0x8176  bracket pairs; odd trail opens, even trail closes
namespace Sjis {
    constexpr unsigned char PunctLead     = 0x81;
    constexpr unsigned char PunctFirst    = 0x41;  // 、
    constexpr unsigned char PunctLast     = 0x49;  // ！
    constexpr unsigned char ProlongedMark = 0x5B;  // ー
    constexpr unsigned char BracketFirst  = 0x65;
    constexpr unsigned char BracketLast   = 0x76;

    // Small kana, which may not begin a line
    constexpr unsigned short kSmallKana[] = {
        0x829F, 0x82A1, 0x82A3, 0x82A5, 0x82A7,          // ぁぃぅぇぉ
        0x82C1, 0x82E1, 0x82E3, 0x82E5, 0x82EC,          // っゃゅょゎ
        0x8340, 0x8342, 0x8344, 0x8346, 0x8348,          // ァィゥェォ
        0x8362, 0x8383, 0x8385, 0x8387, 0x838E,          // ッャュョヮ
        0x8395, 0x8396,                                  // ヵヶ
    };
}

//=============================================================================
// Constants
//=============================================================================
namespace Constants {
    constexpr int kMaxLabelSuffixSearch = 30;
    constexpr int kMaxSearchResults = 20;
    constexpr DWORD kHotkeyPollIntervalMs = 50;
    constexpr DWORD kFileWatcherDebounceMs = 400;
    constexpr DWORD kWatcherStopTimeoutMs = 2000;
    constexpr int kMaxMissedTextsToShow = 15;
}

//=============================================================================
// Configuration
//=============================================================================
namespace Config {
    // Default file paths
    constexpr const char* kDefaultTranslationFile = ".\\tl\\translation.tsv";
    constexpr const char* kDefaultNamesFile = ".\\tl\\unique_names.tsv";
    constexpr const char* kDefaultCharIdFile = ".\\tl\\char_table.tsv";
    constexpr const char* kDefaultTlAssetsPath = ".\\tl\\assets\\";
    constexpr const char* kDefaultScriptsDir = ".\\tl\\scripts";
    constexpr const char* kDefaultUiFile = ".\\tl\\ui.tsv";
    constexpr const char* kDefaultUntranslatedLog = ".\\tl\\untranslated.tsv";

    // Runtime file paths
    char translationFile[MAX_PATH] = ".\\tl\\translation.tsv";
    char scriptsDir[MAX_PATH] = ".\\tl\\scripts";
    char uiFile[MAX_PATH] = ".\\tl\\ui.tsv";
    char namesFile[MAX_PATH] = ".\\tl\\unique_names.tsv";
    char charIdFile[MAX_PATH] = ".\\tl\\char_table.tsv";
    char configFile[MAX_PATH] = ".\\yotsuiro_tl.ini";
    char untranslatedLog[MAX_PATH] = ".\\tl\\untranslated.tsv";

    // General
    char windowTitle[256] = "";
    bool enableConsole = false;
    std::atomic<bool> enableTextLogging{false};
    char logFile[MAX_PATH] = ".\\tl\\hook.log";   // empty = disable
    bool dumpUntranslated = false;
    bool enableDiscordPresence = true;
    bool enableEditingTools = false;

    // Text
    enum class WrapMode { Off, Chars, Pixel };

    WrapMode wrapMode = WrapMode::Pixel;
    int wrapSafetyPx = 12;
    int wordWrapWidth = 70;   // chars mode, and pixel-mode fallback
    bool warnOnOverflow = true;

    // Hotkeys
    int reloadHotkey = VK_F5;
    int statsHotkey = VK_F6;
    int logToggleHotkey = VK_F7;

    // Font
    // Custom font folder, loaded private to this process
    char fontDir[MAX_PATH] = ".\\tl\\fonts\\";
    char fontName[64] = "";
    char fontNameProportional[64] = "";
    // Used for Japanese glyphs when the custom face has no Shift-JIS charset
    char fontFallbackJp[64] = "";

    // Asset redirection
    bool enableAssetRedirect = true;
    bool logAssetRedirects = false;
    char tlAssetsPath[MAX_PATH] = ".\\tl\\assets\\";

    // Debug
    bool enableDebugMode = false;
    bool enableGameDebugOutput = false;
}

//=============================================================================
// Config Save/Load
//=============================================================================
static void SaveDefaultConfig() {
    FILE* f = nullptr;
    if (fopen_s(&f, Config::configFile, "w") != 0 || !f) return;

    fprintf(f, "; Yotsuiro Passionato Translation Hook Configuration\n");
    fprintf(f, "; Auto-generated - edit as needed\n");
    fprintf(f, "\n");

    fprintf(f, "[General]\n");
    fprintf(f, "; Window title (empty = keep original Japanese)\n");
    fprintf(f, "WindowTitle=\n");
    fprintf(f, "\n");
    fprintf(f, "; Show debug console window\n");
    fprintf(f, "EnableConsole=false\n");
    fprintf(f, "\n");
    fprintf(f, "; Log every translated line to console and log file\n");
    fprintf(f, "EnableTextLogging=false\n");
    fprintf(f, "\n");
    fprintf(f, "; Translator tools: F5 reload hotkey and the TSV file watcher.\n");
    fprintf(f, "; Does not affect whether text is translated.\n");
    fprintf(f, "EnableEditingTools=false\n");
    fprintf(f, "\n");
    fprintf(f, "; Write log to file (empty=disable)\n");
    fprintf(f, "LogFile=.\\tl\\hook.log\n");
    fprintf(f, "\n");
    fprintf(f, "; Dump untranslated text to file\n");
    fprintf(f, "DumpUntranslated=false\n");
    fprintf(f, "\n");
    fprintf(f, "; Enable Discord Rich Presence (shows current chapter/label in Discord status)\n");
    fprintf(f, "EnableDiscordPresence=true\n");
    fprintf(f, "\n");

    fprintf(f, "[Text]\n");
    fprintf(f, "; Wrap mode:\n");
    fprintf(f, ";   pixel = fit to the in-game text box, using the game's font\n");
    fprintf(f, ";   chars = fixed column count, see WordWrapWidth\n");
    fprintf(f, ";   off   = don't wrap\n");
    fprintf(f, "WrapMode=pixel\n");
    fprintf(f, "\n");
    fprintf(f, "; Pixels kept clear of the box edge (raise if words still split)\n");
    fprintf(f, "WrapSafetyPx=12\n");
    fprintf(f, "\n");
    fprintf(f, "; Word wrap width in halfwidth columns (a Japanese character counts 2,\n");
    fprintf(f, "; so 70 fits 70 Latin or 35 Japanese). 0=disable - chars mode only\n");
    fprintf(f, "WordWrapWidth=70\n");
    fprintf(f, "\n");
    fprintf(f, "; Warn when a line needs more lines than the box can show\n");
    fprintf(f, "WarnOnOverflow=true\n");
    fprintf(f, "\n");

    fprintf(f, "[Hotkeys]\n");
    fprintf(f, "; Hotkey VK codes: F5=116, F6=117, F7=118, F8=119\n");
    fprintf(f, "ReloadKey=116\n");
    fprintf(f, "StatsKey=117\n");
    fprintf(f, "LogToggleKey=118\n");
    fprintf(f, "\n");

    fprintf(f, "[Font]\n");
    fprintf(f, "; Custom font folder (empty=disable)\n");
    fprintf(f, "Dir=.\\tl\\fonts\\\n");
    fprintf(f, "\n");
    fprintf(f, "; Custom font name (empty=game default)\n");
    fprintf(f, "Name=\n");
    fprintf(f, "\n");
    fprintf(f, "; Proportional font (empty=same as Name)\n");
    fprintf(f, "NameProportional=\n");
    fprintf(f, "\n");
    fprintf(f, "; Japanese glyphs when the font above is Latin-only\n");
    fprintf(f, "; (untranslated lines, unmapped names). Empty=MS Gothic\n");
    fprintf(f, "FallbackJapanese=\n");
    fprintf(f, "\n");

    fprintf(f, "[Files]\n");
    fprintf(f, "; Translation file paths\n");
    fprintf(f, "; Dev style; for experimenting\n");
    fprintf(f, "TranslationFile=.\\tl\\translation.tsv\n");
    fprintf(f, "; Per-RLD split; used in preference to TranslationFile when present\n");
    fprintf(f, "ScriptsDir=.\\tl\\scripts\n");
    fprintf(f, "; Shared names\n");
    fprintf(f, "NamesFile=.\\tl\\unique_names.tsv\n");
    fprintf(f, "; CharID -> original JP name (used when say() gets a NULL name pointer)\n");
    fprintf(f, "CharIdFile=.\\tl\\char_table.tsv\n");
    fprintf(f, "; Engine UI strings from ExHIBIT.exe dialog resources + .rdata\n");
    fprintf(f, "UiFile=.\\tl\\ui.tsv\n");
    fprintf(f, "; Where DumpUntranslated appends lines the game sent but no TSV covers\n");
    fprintf(f, "UntranslatedLog=.\\tl\\untranslated.tsv\n");
    fprintf(f, "\n");

    fprintf(f, "[Assets]\n");
    fprintf(f, "; Enable asset redirection from tl/assets folder\n");
    fprintf(f, "EnableRedirect=true\n");
    fprintf(f, "\n");
    fprintf(f, "; Log asset redirects to console\n");
    fprintf(f, "LogRedirects=false\n");
    fprintf(f, "\n");
    fprintf(f, "; Path to replacement assets (supports .gyu and .png)\n");
    fprintf(f, "Path=.\\tl\\assets\\\n");
    fprintf(f, "\n");


    fprintf(f, "[Debug]\n");
    fprintf(f, "; Enable game debug mode on startup\n");
    fprintf(f, "EnableDebugMode=false\n");
    fprintf(f, "\n");
    fprintf(f, "; Show game's internal debug output in console\n");
    fprintf(f, "EnableGameDebugOutput=false\n");
    fprintf(f, "\n");

    fclose(f);
}

// Helper functions for INI reading
static bool FileExists(const char* path) {
    DWORD attr = GetFileAttributesA(path);
    return (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
}

static bool ReadBool(const char* section, const char* key, bool defaultVal) {
    char buf[16];
    GetPrivateProfileStringA(section, key, defaultVal ? "true" : "false", buf, sizeof(buf), Config::configFile);
    return (_stricmp(buf, "true") == 0 || _stricmp(buf, "1") == 0 || _stricmp(buf, "yes") == 0);
}

static int ReadInt(const char* section, const char* key, int defaultVal) {
    return GetPrivateProfileIntA(section, key, defaultVal, Config::configFile);
}

static void ReadString(const char* section, const char* key, const char* defaultVal, char* out, size_t outSize) {
    GetPrivateProfileStringA(section, key, defaultVal, out, (DWORD)outSize, Config::configFile);
}

// SaveDefaultConfig runs only when the INI file does not exist, so an
// existing INI never receives keys added in later versions of this hook.
// IniEnsure writes each missing key back to the file so users can find and
// edit it.
static std::vector<std::string> g_iniKeysAdded;
static std::vector<std::string> g_iniKeysFailed;

static bool IniKeyExists(const char* section, const char* key) {
    char buf[8];
    GetPrivateProfileStringA(section, key, "\x01", buf, sizeof(buf), Config::configFile);
    return buf[0] != '\x01';
}

static void IniEnsure(const char* section, const char* key, const char* value) {
    if (IniKeyExists(section, key)) return;
    if (WritePrivateProfileStringA(section, key, value, Config::configFile)) {
        g_iniKeysAdded.push_back(std::string(section) + "/" + key + "=" + value);
    } else {
        g_iniKeysFailed.push_back(std::string(section) + "/" + key +
                                  " (error " + std::to_string(GetLastError()) + ")");
    }
}

static bool ReadBoolEnsure(const char* section, const char* key, bool defaultVal) {
    bool v = ReadBool(section, key, defaultVal);
    IniEnsure(section, key, v ? "true" : "false");
    return v;
}

static int ReadIntEnsure(const char* section, const char* key, int defaultVal) {
    int v = ReadInt(section, key, defaultVal);
    char buf[16];
    _itoa_s(v, buf, 10);
    IniEnsure(section, key, buf);
    return v;
}

static void ReadStringEnsure(const char* section, const char* key, const char* defaultVal,
                             char* out, size_t outSize) {
    ReadString(section, key, defaultVal, out, outSize);
    IniEnsure(section, key, out);
}

// Forward declaration for Log (defined later)
static void Log(const char* fmt, ...);

//=============================================================================
// Debug Scene Jump
//=============================================================================
namespace DebugJump {
    static std::mutex g_mutex;
    static std::string g_pendingScene;
    static bool g_jumpRequested = false;
    static void* g_retouchSystem = nullptr;
    static std::atomic<bool> g_debugModeActive{false};
}

static void LoadConfig() {
    // Auto-generate config if missing
    if (!FileExists(Config::configFile)) {
        Log("[CONFIG] Creating default config: %s\n", Config::configFile);
        SaveDefaultConfig();
    }

    // General
    ReadStringEnsure("General", "WindowTitle", "", Config::windowTitle, sizeof(Config::windowTitle));
    Config::enableConsole = ReadBoolEnsure("General", "EnableConsole", false);
    Config::enableTextLogging = ReadBoolEnsure("General", "EnableTextLogging", false);
    Config::enableEditingTools = ReadBoolEnsure("General", "EnableEditingTools", false);
    ReadStringEnsure("General", "LogFile", ".\\tl\\hook.log", Config::logFile, sizeof(Config::logFile));
    Config::dumpUntranslated = ReadBoolEnsure("General", "DumpUntranslated", false);
    Config::enableDiscordPresence = ReadBoolEnsure("General", "EnableDiscordPresence", true);

    // Text
    {
        char mode[16];
        ReadStringEnsure("Text", "WrapMode", "pixel", mode, sizeof(mode));
        if (_stricmp(mode, "off") == 0)        Config::wrapMode = Config::WrapMode::Off;
        else if (_stricmp(mode, "chars") == 0) Config::wrapMode = Config::WrapMode::Chars;
        else                                   Config::wrapMode = Config::WrapMode::Pixel;
    }
    Config::wrapSafetyPx = ReadIntEnsure("Text", "WrapSafetyPx", 12);
    Config::wordWrapWidth = ReadIntEnsure("Text", "WordWrapWidth", 70);
    Config::warnOnOverflow = ReadBoolEnsure("Text", "WarnOnOverflow", true);

    // Hotkeys
    Config::reloadHotkey = ReadIntEnsure("Hotkeys", "ReloadKey", VK_F5);
    Config::statsHotkey = ReadIntEnsure("Hotkeys", "StatsKey", VK_F6);
    Config::logToggleHotkey = ReadIntEnsure("Hotkeys", "LogToggleKey", VK_F7);

    // Font
    ReadStringEnsure("Font", "Dir", ".\\tl\\fonts\\", Config::fontDir, sizeof(Config::fontDir));
    ReadStringEnsure("Font", "Name", "", Config::fontName, sizeof(Config::fontName));
    ReadStringEnsure("Font", "NameProportional", "", Config::fontNameProportional, sizeof(Config::fontNameProportional));
    ReadStringEnsure("Font", "FallbackJapanese", "", Config::fontFallbackJp, sizeof(Config::fontFallbackJp));

    // Files
    ReadStringEnsure("Files", "TranslationFile", Config::kDefaultTranslationFile, Config::translationFile, sizeof(Config::translationFile));
    ReadStringEnsure("Files", "ScriptsDir", Config::kDefaultScriptsDir, Config::scriptsDir, sizeof(Config::scriptsDir));
    ReadStringEnsure("Files", "NamesFile", Config::kDefaultNamesFile, Config::namesFile, sizeof(Config::namesFile));
    ReadStringEnsure("Files", "UiFile", Config::kDefaultUiFile, Config::uiFile, sizeof(Config::uiFile));
    ReadStringEnsure("Files", "CharIdFile", Config::kDefaultCharIdFile, Config::charIdFile, sizeof(Config::charIdFile));
    ReadStringEnsure("Files", "UntranslatedLog", Config::kDefaultUntranslatedLog, Config::untranslatedLog, sizeof(Config::untranslatedLog));

    // Asset Redirection
    Config::enableAssetRedirect = ReadBoolEnsure("Assets", "EnableRedirect", true);
    Config::logAssetRedirects = ReadBoolEnsure("Assets", "LogRedirects", false);
    ReadStringEnsure("Assets", "Path", Config::kDefaultTlAssetsPath, Config::tlAssetsPath, sizeof(Config::tlAssetsPath));

    // Debug
    Config::enableDebugMode = ReadBoolEnsure("Debug", "EnableDebugMode", false);
    Config::enableGameDebugOutput = ReadBoolEnsure("Debug", "EnableGameDebugOutput", false);

    // Initialize debug state from config
    DebugJump::g_debugModeActive = Config::enableGameDebugOutput;

    // Ensure paths end with backslash
    size_t len = strlen(Config::tlAssetsPath);
    if (len > 0 && Config::tlAssetsPath[len - 1] != '\\') {
        strcat_s(Config::tlAssetsPath, "\\");
    }

    // Log
    Log("[CONFIG] Loaded from %s\n", Config::configFile);
    for (const std::string& added : g_iniKeysAdded) {
        Log("[CONFIG] Added missing key: %s\n", added.c_str());
    }
    for (const std::string& failed : g_iniKeysFailed) {
        Log("[CONFIG] Could not write missing key: %s\n", failed.c_str());
    }
    if (Config::fontName[0] != '\0') {
        Log("[CONFIG] Font: %s\n", Config::fontName);
    }
}

//=============================================================================
// Discord RPC
//=============================================================================

static std::atomic<bool> g_discordRunning{false};
static HANDLE g_discordThread = nullptr;
static std::mutex g_discordMutex;
static std::atomic<int64_t> g_presenceStart{0};
static std::string g_currentChapter = "Loading...";

static const char* DISCORD_CLIENT_ID = "1466328361583251488";

static void OnDiscordReady(const DiscordUser* connectedUser) {
    Log("[Discord] Connected as %s#%s\n", connectedUser->username, connectedUser->discriminator);
}

static void OnDiscordDisconnected(int errcode, const char* message) {
    Log("[Discord] Disconnected (%d): %s\n", errcode, message);
}

static void OnDiscordError(int errcode, const char* message) {
    Log("[Discord] Error (%d): %s\n", errcode, message);
}

static void UpdateDiscordPresence() {
    if (!Config::enableDiscordPresence || !g_discordRunning) return;

    DiscordRichPresence rp = {};
    rp.state          = "";
    std::string chapter;
    {
        std::lock_guard<std::mutex> lock(g_discordMutex);
        chapter = g_currentChapter;
    }
    rp.details        = chapter.c_str();
    rp.largeImageKey  = "icon";
    rp.largeImageText = "";
    int64_t start = g_presenceStart.load(std::memory_order_relaxed);
    if (start == 0) {
        auto now = std::chrono::system_clock::now();
        start = std::chrono::duration_cast<std::chrono::seconds>(
            now.time_since_epoch()).count();
        int64_t expected = 0;
        if (!g_presenceStart.compare_exchange_strong(expected, start))
            start = expected;   // another thread got there first
    }
    rp.startTimestamp = start;

    Discord_UpdatePresence(&rp);
}

static DWORD WINAPI DiscordUpdateThreadProc(LPVOID) {
    while (g_discordRunning) {
        Discord_RunCallbacks();
        UpdateDiscordPresence();
        for (int i = 0; i < 100 && g_discordRunning; i++)
            Sleep(100);
    }
    return 0;
}

static void InitDiscordRPC() {
    DiscordEventHandlers handlers = {};
    handlers.ready      = OnDiscordReady;
    handlers.disconnected = OnDiscordDisconnected;
    handlers.errored    = OnDiscordError;

    Discord_Initialize(DISCORD_CLIENT_ID, &handlers, 1, nullptr);
    g_discordRunning = true;

    g_discordThread = CreateThread(nullptr, 0, DiscordUpdateThreadProc, nullptr, 0, nullptr);
    if (!g_discordThread) {
        // If the update thread cannot start, nothing calls Discord_RunCallbacks.
        // The connection would never complete, and every later presence update
        // would be queued but never delivered. Disable presence instead.
        Log("[Discord] CreateThread failed (error %lu) - presence disabled\n", GetLastError());
        g_discordRunning = false;
        Discord_Shutdown();
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_discordMutex);
        g_currentChapter = "Loading...";
    }
    UpdateDiscordPresence();
}

static void ShutdownDiscordRPC() {
    if (!g_discordRunning) return;
    g_discordRunning = false;
    if (g_discordThread) {
        WaitForSingleObject(g_discordThread, 3000);
        CloseHandle(g_discordThread);
        g_discordThread = nullptr;
    }
    g_presenceStart.store(0, std::memory_order_relaxed);
    Discord_ClearPresence();
    Discord_Shutdown();
}

// Call this whenever chapter/label changes
void UpdateChapterPresence(const std::string& chapterName) {
    if (!Config::enableDiscordPresence) return;
    if (chapterName.empty()) return;

    {
        std::lock_guard<std::mutex> lock(g_discordMutex);
        if (chapterName == g_currentChapter) return;
        g_currentChapter = chapterName;
    }
    Log("[Discord] Updated chapter: %s\n", chapterName.c_str());
    UpdateDiscordPresence();
}

//=============================================================================
// Logging
//=============================================================================
static FILE* g_logFile = nullptr;
static std::mutex g_logMutex;
static std::string g_earlyLog;
static bool g_earlyLogDone = false;

static void InitConsole() {
    if (!Config::enableConsole) {
        std::lock_guard<std::mutex> lock(g_logMutex);
        g_earlyLog.clear();
        g_earlyLogDone = true;
        return;
    }
    AllocConsole();
    std::wstring title = L"Translation Hook";
    if (Config::windowTitle[0]) {
        std::string narrow = Config::windowTitle;
        narrow += " - Translation Hook";
        int len = MultiByteToWideChar(CP_UTF8, 0, narrow.c_str(), -1, nullptr, 0);
        title.resize(len);
        MultiByteToWideChar(CP_UTF8, 0, narrow.c_str(), -1, &title[0], len);
    } else {
        title = L"よついろ★パッショナート！ - Translation Hook";
    }
    SetConsoleTitleW(title.c_str());
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    freopen_s(&g_logFile, "CONOUT$", "w", stdout);
    setvbuf(stdout, nullptr, _IONBF, 0);

    // Flush everything logged before the console existed
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (!g_earlyLog.empty()) {
        fputs(g_earlyLog.c_str(), g_logFile);
        fflush(g_logFile);
        g_earlyLog.clear();
    }
    g_earlyLogDone = true;
}

// Log file on disk. The console is not a reliable record: its scrollback
// buffer is limited, so early output is lost in long sessions, and its
// code-page conversion can corrupt Japanese text. The disk log keeps
// everything as UTF-8.
static FILE* g_diskLog = nullptr;

static void InitDiskLog() {
    if (!Config::logFile[0]) return;

    CreateDirectoryA(".\\tl", nullptr);

    FILE* f = nullptr;
    if (fopen_s(&f, Config::logFile, "wb") != 0 || !f) return;

    static const unsigned char kBom[] = { 0xEF, 0xBB, 0xBF };
    fwrite(kBom, 1, sizeof(kBom), f);   // UTF-8 BOM so editors detect the encoding

    std::lock_guard<std::mutex> lock(g_logMutex);
    g_diskLog = f;
    // Write any messages that were logged before this file was opened.
    if (!g_earlyLog.empty()) {
        fputs(g_earlyLog.c_str(), g_diskLog);
        fflush(g_diskLog);
    }
}

static void Log(const char* fmt, ...) {
    char stackBuf[2048];
    std::string heapBuf;
    const char* text = stackBuf;

    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(stackBuf, sizeof(stackBuf), fmt, args);
    va_end(args);
    if (n < 0) return;

    if ((size_t)n >= sizeof(stackBuf)) {
        heapBuf.resize((size_t)n + 1);
        va_start(args, fmt);
        vsnprintf(&heapBuf[0], heapBuf.size(), fmt, args);
        va_end(args);
        heapBuf.resize((size_t)n);
        text = heapBuf.c_str();
    }

    std::lock_guard<std::mutex> lock(g_logMutex);

    if (g_logFile) {
        fputs(text, g_logFile);
        fflush(g_logFile);
    } else if (!g_earlyLogDone) {
        g_earlyLog.append(text);
    }

    if (g_diskLog) {
        fputs(text, g_diskLog);
        fflush(g_diskLog);
    }
}


//=============================================================================
// Encoding Detection & Conversion
//=============================================================================
namespace Encoding {
    enum class Type {
        Unknown,
        UTF8_BOM,
        UTF8,
        ShiftJIS
    };

    // Detect encoding of a buffer
    Type Detect(const char* data, size_t size) {
        if (size == 0) return Type::Unknown;

        // Check for UTF-8 BOM
        if (size >= 3 &&
            (unsigned char)data[0] == 0xEF &&
            (unsigned char)data[1] == 0xBB &&
            (unsigned char)data[2] == 0xBF) {
            return Type::UTF8_BOM;
        }

        // Try to detect UTF-8 by looking for valid multi-byte sequences
        int utf8Score = 0;
        int sjisScore = 0;

        for (size_t i = 0; i < size && i < 1000; i++) {
            unsigned char c = (unsigned char)data[i];

            // UTF-8 multi-byte detection
            if (c >= 0xC0 && c <= 0xDF && i + 1 < size) {
                unsigned char c2 = (unsigned char)data[i + 1];
                if (c2 >= 0x80 && c2 <= 0xBF) {
                    utf8Score += 2;
                    i++;
                    continue;
                }
            }
            if (c >= 0xE0 && c <= 0xEF && i + 2 < size) {
                unsigned char c2 = (unsigned char)data[i + 1];
                unsigned char c3 = (unsigned char)data[i + 2];
                if (c2 >= 0x80 && c2 <= 0xBF && c3 >= 0x80 && c3 <= 0xBF) {
                    utf8Score += 3;
                    i += 2;
                    continue;
                }
            }

            // Shift-JIS detection (common Japanese ranges)
            if ((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC)) {
                if (i + 1 < size) {
                    unsigned char c2 = (unsigned char)data[i + 1];
                    if ((c2 >= 0x40 && c2 <= 0x7E) || (c2 >= 0x80 && c2 <= 0xFC)) {
                        sjisScore += 2;
                        i++;
                        continue;
                    }
                }
            }
        }

        if (utf8Score > sjisScore * 2) return Type::UTF8;
        if (sjisScore > 0) return Type::ShiftJIS;
        return Type::UTF8;  // Default to UTF-8 for ASCII-only
    }

    // Shift-JIS -> UTF-8
    std::string SjisToUtf8(const char* sjis) {
        if (!sjis || !*sjis) return "";

        int wideLen = MultiByteToWideChar(932, 0, sjis, -1, nullptr, 0);
        if (wideLen <= 0) return "";

        std::wstring wide(wideLen, 0);
        MultiByteToWideChar(932, 0, sjis, -1, &wide[0], wideLen);

        int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (utf8Len <= 0) return "";

        std::string utf8(utf8Len, 0);
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, &utf8[0], utf8Len, nullptr, nullptr);

        if (!utf8.empty() && utf8.back() == 0) utf8.pop_back();
        return utf8;
    }

    // UTF-8 -> Shift-JIS
    std::string Utf8ToSjis(const char* utf8) {
        if (!utf8 || !*utf8) return "";

        int wideLen = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
        if (wideLen <= 0) return "";

        std::wstring wide(wideLen, 0);
        MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &wide[0], wideLen);

        int sjisLen = WideCharToMultiByte(932, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (sjisLen <= 0) return "";

        std::string sjis(sjisLen, 0);
        WideCharToMultiByte(932, 0, wide.c_str(), -1, &sjis[0], sjisLen, nullptr, nullptr);

        if (!sjis.empty() && sjis.back() == 0) sjis.pop_back();
        return sjis;
    }

    // Same as Utf8ToSjis, but also reports characters that CP932 cannot
    // represent (em dashes, accented Latin letters). WideCharToMultiByte
    // replaces those characters with a best-fit substitute and still returns
    // success, so the caller cannot detect the loss from the return value.
    // On return, `lost` maps each byte offset in the output string to the
    // original character that was replaced at that offset.
    std::string Utf8ToSjisTracked(const char* utf8,
                                  std::unordered_map<size_t, wchar_t>& lost) {
        lost.clear();
        if (!utf8 || !*utf8) return "";

        int wideLen = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
        if (wideLen <= 0) return "";

        std::wstring wide(wideLen, 0);
        MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &wide[0], wideLen);
        if (!wide.empty() && wide.back() == 0) wide.pop_back();

        std::string sjis;
        sjis.reserve(wide.size() * 2);

        char buf[8];
        for (size_t i = 0; i < wide.size(); i++) {
            BOOL usedDefault = FALSE;
            int n = WideCharToMultiByte(932, WC_NO_BEST_FIT_CHARS, &wide[i], 1,
                                        buf, sizeof(buf), nullptr, &usedDefault);
            if (n > 0 && !usedDefault) {
                sjis.append(buf, n);        // The character converts cleanly; keep it.
                continue;
            }
            // This character has no CP932 encoding. Emit the best-fit substitute
            // (an unaccented letter, or '?') so the string remains valid Shift-JIS,
            // and record what was lost so the glyph hook can draw the original
            // character later. If that hook is missing or fails, the text still
            // renders with the substitute instead of garbage.
            n = WideCharToMultiByte(932, 0, &wide[i], 1, buf, sizeof(buf), nullptr, nullptr);
            if (n <= 0) { buf[0] = '?'; n = 1; }
            lost[sjis.size()] = wide[i];
            sjis.append(buf, n);
        }
        return sjis;
    }

    // Convert any encoding to UTF-8
    std::string ToUtf8(const std::string& data, Type encoding) {
        switch (encoding) {
        case Type::UTF8_BOM:
            return data.size() >= 3 ? data.substr(3) : data;
        case Type::UTF8:
            return data;
        case Type::ShiftJIS:
            return SjisToUtf8(data.c_str());
        default:
            return data;
        }
    }
}

//=============================================================================
// Asset Redirection
//=============================================================================
namespace AssetRedirect {

    static std::string GetFileName(const std::string& path) {
        size_t pos = path.find_last_of("\\/");
        return (pos != std::string::npos) ? path.substr(pos + 1) : path;
    }

    static std::string GetRelativePath(const std::string& fullPath) {
        // Look for "res\" in the path
        size_t resPos = fullPath.find("res\\");
        if (resPos != std::string::npos) {
            return fullPath.substr(resPos + 4);
        }
        return GetFileName(fullPath);
    }

    static std::string FindReplacement(const std::string& originalPath) {
        if (!Config::enableAssetRedirect) return "";

        std::string relativePath = GetRelativePath(originalPath);
        std::string assetsPath = Config::tlAssetsPath;

        // Try 1: Exact path (tl/assets/g/ev/xxx.gyu)
        std::string tryPath = assetsPath + relativePath;
        if (::FileExists(tryPath.c_str())) {
            return tryPath;
        }

        // Try 2: PNG version of exact path
        size_t extPos = tryPath.rfind(".gyu");
        if (extPos != std::string::npos) {
            std::string pngPath = tryPath.substr(0, extPos) + ".png";
            if (::FileExists(pngPath.c_str())) {
                return pngPath;
            }
        }

        // Try 3: Flat structure (tl/assets/xxx.gyu)
        std::string filename = GetFileName(originalPath);
        tryPath = assetsPath + filename;
        if (::FileExists(tryPath.c_str())) {
            return tryPath;
        }

        // Try 4: Flat PNG (tl/assets/xxx.png)
        extPos = filename.rfind(".gyu");
        if (extPos != std::string::npos) {
            std::string pngName = filename.substr(0, extPos) + ".png";
            tryPath = assetsPath + pngName;
            if (::FileExists(tryPath.c_str())) {
                return tryPath;
            }
        }

        return "";
    }
}

//=============================================================================
// Translation Database
//=============================================================================
// Scene tracking globals
namespace {
    std::mutex g_sceneMutex;
    std::string g_currentFile;
    std::string g_currentLabel;

    // Index of the MESSAGE command currently being dispatched. CmdMessage_Hook
    // sets it; AdvCharSay_Hook uses it to look up the translation by
    // (file, index) instead of by message text. It is -1 when say() is reached
    // from any path other than cmdMessage.
    thread_local int t_cmdIndex = -1;

    // True while the engine redraws the backlog. drawHistory() is called from
    // hitwait(), which runs inside cmdMessage, so t_cmdIndex still holds the
    // live line's index during the redraw; without this flag, every backlog
    // entry would be looked up with that index. The flag also excludes backlog
    // redraws from scene tracking and the miss statistics.
    thread_local bool t_inHistory = false;

    std::string CurrentSceneFile() {
        std::lock_guard<std::mutex> lock(g_sceneMutex);
        return g_currentFile;
    }
}

class TranslationDB {
public:
    struct LoadSummary {
        bool sourceFound = false;
        bool dialogueFound = false;
    };

    LoadSummary Load(const char* tsvPath, const char* namesPath) {
        std::lock_guard<std::mutex> lock(m_dataMutex);

        m_names.clear();
        m_contextualNames.clear();
        m_messages.clear();
        m_uiStrings.clear();
        m_originalNamesByIndex.clear();
        m_labels.clear();
        m_textByFileIndex.clear();
        m_originalByFileIndex.clear();
        m_messageToFile.clear();
        m_messageToIndex.clear();
        m_labelsByFileIndex.clear();

        int globalCount = 0;
        int contextualCount = 0;
        int textCount = 0;
        int choiceCount = 0;
        int labelCount = 0;

        // Load global names from unique_names.tsv
        if (namesPath) {
            globalCount = LoadGlobalNames(namesPath);
        }

        // Load rows
        // Prefer the per-RLD split otherwise fall back to the
        // monolithic translation.tsv when that folder is absent or empty.
        std::map<std::pair<std::string, int>, std::string> namesByIndex;
        std::map<std::pair<std::string, int>, std::string> textsByIndex;

        Encoding::Type encoding = Encoding::Type::Unknown;
        int splitFiles = 0;
        std::string sourceDesc;

        std::vector<std::string> scriptFiles = ListTsvFiles(Config::scriptsDir);
        for (const std::string& path : scriptFiles) {
            std::string utf8;
            Encoding::Type enc;
            if (!ReadFileUtf8(path.c_str(), utf8, enc)) continue;
            if (splitFiles == 0) encoding = enc;

            // FILE column is implicit: it is the filename stem.
            std::string stem = path;
            size_t slash = stem.find_last_of("\\/");
            if (slash != std::string::npos) stem = stem.substr(slash + 1);
            size_t dot = stem.find_last_of('.');
            if (dot != std::string::npos) stem = stem.substr(0, dot);

            ParseTsvContent(utf8, stem, namesByIndex, textsByIndex,
                            textCount, choiceCount, labelCount);
            splitFiles++;
        }

        if (splitFiles > 0) {
            sourceDesc = std::string(Config::scriptsDir) + " (" +
                         std::to_string(splitFiles) + " files)";
        } else {
            std::string utf8;
            if (ReadFileUtf8(tsvPath, utf8, encoding)) {
                ParseTsvContent(utf8, std::string(), namesByIndex, textsByIndex,
                                textCount, choiceCount, labelCount);
                sourceDesc = tsvPath;
            } else {
                Log("[TL] Cannot open: %s\n", tsvPath);
                sourceDesc = "(no script source)";
            }
        }

        // load ui strings
        int uiCount = LoadUiStrings(Config::uiFile);

        for (const auto& [key, translatedName] : namesByIndex) {
            if (translatedName.empty()) continue;

            std::string originalName = m_originalNamesByIndex[key];
            if (originalName.empty()) continue;

            auto textIt = textsByIndex.find(key);
            if (textIt != textsByIndex.end()) {
                // Contextual: "name|message" -> translation
                std::string contextKey = originalName + "|" + textIt->second;
                m_contextualNames[contextKey] = translatedName;
                contextualCount++;
            } else {
                // No text at same index - override global
                m_names[originalName] = translatedName;
                // Don't increment globalCount - it was loaded from unique_names
            }
        }

        const char* encodingName = "Unknown";
        switch (encoding) {
            case Encoding::Type::UTF8_BOM: encodingName = "UTF-8 (BOM)"; break;
            case Encoding::Type::UTF8: encodingName = "UTF-8"; break;
            case Encoding::Type::ShiftJIS: encodingName = "Shift-JIS"; break;
            default: break;
        }

        Log("[TL] Loaded (%s) from %s:\n", encodingName, sourceDesc.c_str());
        Log("[TL]   %d global names (from %s)\n", globalCount, namesPath ? namesPath : "none");
        Log("[TL]   %d contextual names\n", contextualCount);
        Log("[TL]   %d texts\n", textCount);
        Log("[TL]   %d choices\n", choiceCount);
        Log("[TL]   %d labels\n", labelCount);
        Log("[TL]   %d UI strings (from %s)\n", uiCount, Config::uiFile);

        LoadSummary summary;
        // sourceFound reports whether any translation file exists, not whether it
        // parsed: a script file that exists but cannot be read leaves splitFiles
        // at 0 and still counts as found.
        summary.sourceFound = !scriptFiles.empty() ||
                              (tsvPath && FileExists(tsvPath)) ||
                              (namesPath && FileExists(namesPath)) ||
                              FileExists(Config::uiFile);
        // Use textCount, not m_messages.size(): m_messages also contains CHOICE_
        // rows, which would make it look as if dialogue was loaded.
        summary.dialogueFound = textCount > 0;
        return summary;
    }

    void Reload() {
        Log("[TL] Reloading...\n");
        auto started = std::chrono::steady_clock::now();
        Load(Config::translationFile, Config::namesFile);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        Log("[TL] Reload finished in %lld ms\n", (long long)ms);
    }

        void FindInDB(const std::string& searchText) {
        std::lock_guard<std::mutex> lock(m_dataMutex);

        int found = 0;
        Log("\n[SEARCH] Looking for: %s\n", searchText.c_str());

        auto searchMap = [&](const char* tag,
                             const std::unordered_map<std::string, std::string>& m) {
            for (const auto& [key, val] : m) {
                if (found >= Constants::kMaxSearchResults) return;
                if (key.find(searchText) != std::string::npos ||
                    val.find(searchText) != std::string::npos) {
                    Log("  %s [%s] -> [%s]\n", tag,
                        key.substr(0, 40).c_str(),
                        val.substr(0, 40).c_str());
                    found++;
                }
            }
        };

        searchMap("msg", m_messages);
        searchMap("ui ", m_uiStrings);

        if (found >= Constants::kMaxSearchResults) {
            Log("  ... (showing first %d)\n", Constants::kMaxSearchResults);
        }
        if (found == 0) {
            Log("  No matches found.\n");
        }
        Log("\n");
    }

    // All Find* methods return by value. Do not change them to return pointers
    // or references into the maps: Load() clears the maps during hot reload
    // (F5 or the file-watcher thread), which would leave callers holding
    // dangling pointers. An empty string means "no translation". This is
    // unambiguous because rows with an empty TRANSLATION column are dropped at
    // load time, so a real translation is never empty.

    std::string FindNameTranslation(const char* sjisName, const char* sjisMessage) {
        if (!sjisName || !*sjisName) return std::string();

        std::string utf8Name = Encoding::SjisToUtf8(sjisName);
        if (utf8Name.empty()) return std::string();

        {
            std::lock_guard<std::mutex> lock(m_dataMutex);

            // Try contextual lookup first (name + message)
            if (sjisMessage && *sjisMessage) {
                std::string utf8Msg = Encoding::SjisToUtf8(sjisMessage);
                std::string contextKey = utf8Name + "|" + utf8Msg;

                auto it = m_contextualNames.find(contextKey);
                if (it != m_contextualNames.end()) return it->second;
            }

            // Fall back to global name lookup
            auto it = m_names.find(utf8Name);
            if (it != m_names.end()) return it->second;
        }

        LogMissing(utf8Name.c_str(), "NAME");
        return std::string();
    }

    // `file` and `index` identify the exact MESSAGE command being executed.
    // When they match a TSV row, the translation applies to that specific
    // occurrence, which lets duplicate lines have different translations.
    // Otherwise, fall back to a text lookup, which returns the same translation
    // for every occurrence of the text.
    //
    // Pass countMiss=false when this lookup is a second attempt on text that an
    // earlier hook already translated. Those lookups are expected to miss. If
    // they were counted, every already-translated line would be recorded as
    // untranslated and appended to untranslated.tsv, which translators use as
    // the list of lines still to do.
    std::string FindMessageTranslation(const char* sjisMessage,
                                       const std::string& file = std::string(),
                                       int index = -1,
                                       bool countMiss = true) {
        if (!sjisMessage || !*sjisMessage) return std::string();

        std::string utf8Key = Encoding::SjisToUtf8(sjisMessage);
        if (utf8Key.empty()) return std::string();

        std::string result;
        bool found = false;
        std::string sceneFile, sceneLabel;

        {
            std::lock_guard<std::mutex> lock(m_dataMutex);

            // t_cmdIndex is only valid for the MESSAGE command currently being
            // dispatched. Verify that the ORIGINAL text stored at (file, index)
            // matches the text we received; otherwise this call inherited a
            // stale index and a positional lookup would return the wrong line.
            // (The backlog redraw used to trigger exactly that bug.)
            if (!t_inHistory && !file.empty() && index >= 0) {
                auto key = std::make_pair(file, index);
                auto origIt = m_originalByFileIndex.find(key);
                if (origIt != m_originalByFileIndex.end() && origIt->second == utf8Key) {
                    auto posIt = m_textByFileIndex.find(key);
                    if (posIt != m_textByFileIndex.end()) {
                        result = posIt->second;
                        found = true;
                        sceneFile = file;
                        sceneLabel = GetNearestLabel(file, index);
                    }
                }
            }

            if (!found) {
                auto it = m_messages.find(utf8Key);
                if (it != m_messages.end()) {
                    result = it->second;
                    found = true;

                    auto fileIt = m_messageToFile.find(utf8Key);
                    auto indexIt = m_messageToIndex.find(utf8Key);
                    if (fileIt != m_messageToFile.end() && indexIt != m_messageToIndex.end()) {
                        sceneFile = fileIt->second;
                        sceneLabel = GetNearestLabel(sceneFile, indexIt->second);
                    }
                }
            }

            if (found) m_hitCount++;
        }

        if (found) {
            {
                std::lock_guard<std::mutex> slock(m_statsMutex);
                m_usedKeys.insert(utf8Key);
            }

            // Don't update the scene or Discord presence while the user scrolls
            // the backlog; it is not story progress.
            if (!t_inHistory) {
                if (!sceneFile.empty()) {
                    std::lock_guard<std::mutex> sceneLock(g_sceneMutex);
                    if (g_currentFile != sceneFile || g_currentLabel != sceneLabel) {
                        g_currentFile = sceneFile;
                        g_currentLabel = sceneLabel;
                        if (!sceneLabel.empty()) {
                            Log("[SCENE] %s | %s\n", g_currentFile.c_str(), g_currentLabel.c_str());
                        }
                    }
                }

                std::string display = sceneLabel;
                size_t bracket = display.rfind(" [");
                if (bracket != std::string::npos) display.erase(bracket);
                UpdateChapterPresence(display);
            }

            return result;
        }

        // Backlog entries were translated before they were stored, so lookups
        // on them always miss. Don't count those misses.
        if (t_inHistory) return std::string();
        if (!countMiss)  return std::string();

        m_missCount++;
        {
            std::lock_guard<std::mutex> slock(m_statsMutex);
            m_missedTexts.insert(utf8Key);
        }

        LogMissing(utf8Key.c_str(), "TEXT");
        return std::string();
    }

    // UI
    std::string FindUITranslation(const char* sjisText) {
        if (!sjisText || !*sjisText) return std::string();

        std::string utf8Key = Encoding::SjisToUtf8(sjisText);
        if (utf8Key.empty()) return std::string();

        std::lock_guard<std::mutex> lock(m_dataMutex);
        auto it = m_uiStrings.find(utf8Key);
        return (it != m_uiStrings.end()) ? it->second : std::string();
    }

    // Label
    std::string FindLabelTranslation(const char* sjisLabel) {
        if (!sjisLabel || !*sjisLabel) return std::string();

        std::string utf8Key = Encoding::SjisToUtf8(sjisLabel);
        if (utf8Key.empty()) return std::string();

        {
            std::lock_guard<std::mutex> lock(m_dataMutex);

            // Try exact match first
            auto it = m_labels.find(utf8Key);
            if (it != m_labels.end()) return it->second;

            // Save files don't include [X] suffix, but TSV does
            // Try appending " [1]", " [2]", etc.
            for (int i = 1; i <= Constants::kMaxLabelSuffixSearch; i++) {
                std::string withSuffix = utf8Key + " [" + std::to_string(i) + "]";
                it = m_labels.find(withSuffix);
                if (it != m_labels.end()) return it->second;
            }
        }

        LogMissing(utf8Key.c_str(), "LABEL");
        return std::string();
    }

    std::string GetNearestLabel(const std::string& file, int index) {
        auto it = m_labelsByFileIndex.upper_bound(std::make_pair(file, index));
        if (it != m_labelsByFileIndex.begin()) {
            --it;
            if (it->first.first == file) {
                return it->second;
            }
        }
        return "";
    }

    std::atomic<int> m_hitCount{0};
    std::atomic<int> m_missCount{0};
    std::unordered_set<std::string> m_usedKeys;  // Track which translations were used
    std::mutex m_statsMutex;

    void PrintStats() {
        std::lock_guard<std::mutex> lock(m_dataMutex);
        std::lock_guard<std::mutex> slock(m_statsMutex);

        Log("\n========== Translation Stats ==========\n");
        Log("  Loaded: %d messages, %d labels, %d names, %d ui\n",
            (int)m_messages.size(), (int)m_labels.size(), (int)m_names.size(),
            (int)m_uiStrings.size());
        Log("  Hits: %d | Misses: %d\n", m_hitCount.load(), m_missCount.load());
        Log("  Unique texts matched: %d\n", (int)m_usedKeys.size());

        // Show missed texts (game sent but not in TSV)
        if (!m_missedTexts.empty()) {
            Log("\n--- Missed (game sent, not in TSV): ---\n");
            int count = 0;
            for (const auto& text : m_missedTexts) {
                if (count++ < Constants::kMaxMissedTextsToShow) {
                    Log("  %s\n", text.substr(0, 70).c_str());
                }
            }
            if (m_missedTexts.size() > Constants::kMaxMissedTextsToShow) {
                Log("  ... +%d more\n", (int)m_missedTexts.size() - Constants::kMaxMissedTextsToShow);
            }
        } else {
            Log("\n  No missed texts! Everything translated.\n");
        }

        Log("=========================================\n\n");
    }

private:
    void UnescapeString(std::string& s) {
        size_t pos = 0;
        while ((pos = s.find("\\n", pos)) != std::string::npos) {
            s.replace(pos, 2, "\n");
            pos += 1;
        }
        pos = 0;
        while ((pos = s.find("\\t", pos)) != std::string::npos) {
            s.replace(pos, 2, "\t");
            pos += 1;
        }
    }

    void LogMissing(const char* utf8Text, const char* type) {
        if (!Config::dumpUntranslated) return;

        std::lock_guard<std::mutex> lock(m_logMutex);

        std::string key(utf8Text);
        if (m_logged.count(key)) return;
        m_logged.insert(key);

        std::ofstream file(Config::untranslatedLog, std::ios::app | std::ios::binary);
        if (file) {
            std::string escaped = key;
            size_t pos = 0;
            while ((pos = escaped.find('\n', pos)) != std::string::npos) {
                escaped.replace(pos, 1, "\\n");
                pos += 2;
            }
            file << "RUNTIME\t0\t" << type << "\t" << escaped << "\t\r\n";
        }
    }

    int LoadUiStrings(const char* uiPath) {
        if (!uiPath || !*uiPath) return 0;

        std::string utf8;
        Encoding::Type encoding;
        if (!ReadFileUtf8(uiPath, utf8, encoding)) return 0;

        int count = 0;
        std::istringstream iss(utf8);
        std::string line;

        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            if (line.rfind("ORIGINAL", 0) == 0) continue;

            size_t tab = line.find('\t');
            if (tab == std::string::npos) continue;

            std::string original = line.substr(0, tab);
            std::string translated = line.substr(tab + 1);
            size_t tab2 = translated.find('\t');
            if (tab2 != std::string::npos) translated = translated.substr(0, tab2);

            while (!translated.empty() &&
                   (translated.back() == ' ' || translated.back() == '\t')) {
                translated.pop_back();
            }
            if (original.empty() || translated.empty()) continue;

            UnescapeString(original);
            UnescapeString(translated);

            m_uiStrings[original] = translated;
            count++;
        }
        return count;
    }

    int LoadGlobalNames(const char* namesPath) {
        std::ifstream file(namesPath, std::ios::binary | std::ios::ate);
        if (!file) {
            Log("[TL] No global names file: %s\n", namesPath);
            return 0;
        }

        std::streamoff fileSize = file.tellg();
        if (fileSize < 0) {
            Log("[TL] Cannot size: %s\n", namesPath);
            return 0;
        }
        file.seekg(0);

        std::string content((size_t)fileSize, 0);
        if (fileSize > 0) file.read(&content[0], fileSize);

        Encoding::Type encoding = Encoding::Detect(content.c_str(), content.size());
        std::string utf8Content = Encoding::ToUtf8(content, encoding);

        int count = 0;
        std::istringstream iss(utf8Content);
        std::string line;

        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;

            // Skip header line
            if (line.find("ORIGINAL") == 0) continue;

            // Split by tab
            size_t tabPos = line.find('\t');
            if (tabPos == std::string::npos) continue;

            std::string original = line.substr(0, tabPos);
            std::string translated = line.substr(tabPos + 1);

            // Remove trailing tab/count if present
            size_t tabPos2 = translated.find('\t');
            if (tabPos2 != std::string::npos) {
                translated = translated.substr(0, tabPos2);
            }

            // Trim whitespace
            while (!translated.empty() && (translated.back() == ' ' || translated.back() == '\t')) {
                translated.pop_back();
            }

            // Skip if empty (user hasn't filled it in yet)
            if (original.empty() || translated.empty()) continue;

            UnescapeString(original);
            UnescapeString(translated);

            m_names[original] = translated;
            count++;
        }

        return count;
    }

    // Read a file and convert it to UTF-8, auto-detecting the encoding.
    static bool ReadFileUtf8(const char* path, std::string& utf8Out, Encoding::Type& encOut) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) return false;

        std::streamoff size = file.tellg();
        if (size < 0) return false;
        file.seekg(0);

        std::string content((size_t)size, 0);
        if (size > 0) file.read(&content[0], size);

        encOut = Encoding::Detect(content.c_str(), content.size());
        utf8Out = Encoding::ToUtf8(content, encOut);
        return true;
    }

    // Returns the sorted list of *.tsv files in a directory, or an empty vector
    // if the directory does not exist. ScriptsDir must contain only script
    // TSVs: if it is pointed at .\tl, this function also returns
    // unique_names.tsv, ui.tsv, and translation.tsv, and those are currently
    // rejected only because their column layout happens not to parse as a
    // script.
    static std::vector<std::string> ListTsvFiles(const char* dir) {
        std::vector<std::string> out;
        if (!dir || !*dir) return out;

        std::string pattern = std::string(dir) + "\\*.tsv";
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return out;

        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            out.push_back(std::string(dir) + "\\" + fd.cFileName);
        } while (FindNextFileA(h, &fd));
        FindClose(h);

        std::sort(out.begin(), out.end());
        return out;
    }

    // Parse TSV rows into the lookup maps.
    //   fixedFileId empty -> monolithic layout: FILE INDEX TYPE ORIGINAL TRANSLATION
    //   fixedFileId set   -> per-RLD layout:         INDEX TYPE ORIGINAL TRANSLATION
    void ParseTsvContent(const std::string& utf8Content,
                         const std::string& fixedFileId,
                         std::map<std::pair<std::string, int>, std::string>& namesByIndex,
                         std::map<std::pair<std::string, int>, std::string>& textsByIndex,
                         int& textCount, int& choiceCount, int& labelCount) {
        const bool split = !fixedFileId.empty();
        const size_t minCols = split ? 4u : 5u;
        const size_t tlCol = split ? 3u : 4u;

        std::istringstream iss(utf8Content);
        std::string line;

        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;

            std::vector<std::string> parts;
            size_t start = 0, end;
            while ((end = line.find('\t', start)) != std::string::npos) {
                parts.push_back(line.substr(start, end - start));
                start = end + 1;
            }
            parts.push_back(line.substr(start));

            if (parts.size() < minCols || parts[tlCol].empty()) continue;

            std::string fileId = split ? fixedFileId : parts[0];
            const size_t base = split ? 0u : 1u;      // index of the INDEX column
            if (parts[base] == "INDEX" || parts[base] == "FILE") continue;   // header

            int index = std::atoi(parts[base].c_str());
            std::string type = parts[base + 1];
            std::string original = parts[base + 2];
            std::string translated = parts[tlCol];

            UnescapeString(original);
            UnescapeString(translated);

            auto key = std::make_pair(fileId, index);

            // An empty TRANSLATION column means "not translated yet". Never insert
            // it into a lookup map; callers treat an empty result as "no
            // translation".
            const bool hasTl = !translated.empty();

            if (type == "NAME") {
                // namesByIndex and m_originalNamesByIndex are consumed as a pair
                // later in Load(). Store both even when the translation is empty;
                // that consumer checks for empty itself.
                namesByIndex[key] = translated;
                m_originalNamesByIndex[key] = original;
            } else if (type == "TEXT" || type == "MSG") {
                textsByIndex[key] = original;          // scene tracking, all rows
                // Checked against the engine's text before a (file,index)
                // translation is accepted - see FindMessageTranslation.
                m_originalByFileIndex[key] = original;
                if (hasTl) {
                    m_textByFileIndex[key] = translated;   // exact, per-occurrence
                    m_messages[original] = translated;     // fallback, text-keyed
                }
                // Scene/Discord tracking works off the original text, so these
                // two are populated regardless of translation state.
                m_messageToFile[original] = fileId;
                m_messageToIndex[original] = index;
                if (hasTl) textCount++;
            } else if (type == "LABEL") {
                if (hasTl) {
                    m_labels[original] = translated;
                    labelCount++;
                }
                // m_labelsByFileIndex feeds scene tracking (logging and Discord),
                // not display. A readable name is useful even without a
                // translation, so fall back to the original text.
                m_labelsByFileIndex[key] = hasTl ? translated : original;
            } else if (type.rfind("CHOICE_", 0) == 0) {
                if (hasTl) {
                    m_messages[original] = translated;
                    choiceCount++;
                }
            } else if (type == "SAVEDESC") {
                // SAVEDESC rows are autosave slot titles: the AUTOSAVE (0x0C)
                // command passes this string through UxAdvSystem::autoSave to
                // saveEngine(), and RetouchSaveDataControl::title() displays it
                // through the same path as a BLOCK label, so store it with the
                // labels.
                if (hasTl) {
                    m_labels[original] = translated;
                    labelCount++;
                }
            }
        }
    }

    // Storage
    std::unordered_map<std::string, std::string> m_contextualNames;  // "name|message" -> translated_name
    std::unordered_map<std::string, std::string> m_names;            // name -> translated (fallback)
    std::unordered_map<std::string, std::string> m_messages;         // message -> translated
    std::unordered_map<std::string, std::string> m_uiStrings;        // ui.tsv only: engine UI -> translated
    std::unordered_map<std::string, std::string> m_labels;           // label -> translated
    std::map<std::pair<std::string, int>, std::string> m_textByFileIndex;  // (file,index) -> translated
    std::map<std::pair<std::string, int>, std::string> m_originalByFileIndex;  // (file,index) -> original
    std::unordered_map<std::string, std::string> m_messageToFile;    // message -> file
    std::unordered_map<std::string, int> m_messageToIndex;           // message -> index
    std::map<std::pair<std::string, int>, std::string> m_labelsByFileIndex;  // (file,index) -> label
    std::map<std::pair<std::string, int>, std::string> m_originalNamesByIndex;
    std::unordered_set<std::string> m_logged;
    std::unordered_set<std::string> m_missedTexts;
    std::mutex m_dataMutex;
    std::mutex m_logMutex;
};


static TranslationDB g_translationDB;

//=============================================================================
// String Pool
//=============================================================================
class StringPool {
public:
    const char* Store(const std::string& str) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_pool.find(str);
        if (it != m_pool.end()) return it->second.c_str();
        m_pool[str] = str;
        return m_pool[str].c_str();
    }

private:
    std::unordered_map<std::string, std::string> m_pool;
    std::mutex m_mutex;
};

static StringPool g_stringPool;

//=============================================================================
// Word Wrapping
//=============================================================================
typedef HFONT(WINAPI* Fn_CreateFontIndirectA)(const LOGFONTA*);
static Fn_CreateFontIndirectA g_origCreateFontIndirectA = nullptr;

//=============================================================================
// Bundled fonts
//=============================================================================
// Wine quirk: a face that is available only through a FontSubstitutes
// alias renders correctly, but font enumeration does not report it.
// FontProbe below relies on enumeration, so it reports such a face as not
// installed.
static std::vector<std::pair<std::wstring, DWORD>> g_bundledFonts;

static void LoadBundledFonts() {
    if (Config::fontDir[0] == '\0') return;

    wchar_t dir[MAX_PATH];
    if (MultiByteToWideChar(932, 0, Config::fontDir, -1, dir, MAX_PATH) == 0) return;

    std::wstring base(dir);
    if (base.back() != L'\\' && base.back() != L'/') base += L'\\';

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((base + L"*.*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const wchar_t* ext = wcsrchr(fd.cFileName, L'.');
        if (!ext) continue;
        if (_wcsicmp(ext, L".ttf") && _wcsicmp(ext, L".otf") &&
            _wcsicmp(ext, L".ttc") && _wcsicmp(ext, L".fon")) continue;

        std::wstring path = base + fd.cFileName;

        // Older Wine versions do not enumerate fonts added with FR_PRIVATE.
        // If the private load fails, retry as a shared (system-wide) load.
        DWORD flags = FR_PRIVATE;
        int added = AddFontResourceExW(path.c_str(), flags, nullptr);
        if (!added) {
            flags = 0;
            added = AddFontResourceExW(path.c_str(), flags, nullptr);
        }

        Log("[FONT] bundled %ls: %s%s\n", fd.cFileName,
            added ? "loaded" : "FAILED", (added && !flags) ? " (shared)" : "");

        if (added) g_bundledFonts.emplace_back(std::move(path), flags);
    } while (FindNextFileW(h, &fd));

    FindClose(h);
}

static void UnloadBundledFonts() {
    for (const auto& f : g_bundledFonts)
        RemoveFontResourceExW(f.first.c_str(), f.second, nullptr);
    g_bundledFonts.clear();
}

//=============================================================================
// Font capability probe
//=============================================================================
// Detects whether a face is installed and whether it supports Shift-JIS.
//
// CreateFont with SHIFTJIS_CHARSET does not fail for a Latin-only face:
// GDI silently substitutes MS PGothic. GetTextFace cannot detect the
// substitution either, because it can report Japanese faces under their
// localized names, so comparing names produces false negatives.
// EnumFontFamiliesExA gives a reliable answer: one callback per charset the
// face actually supports, and no callbacks at all if it is not installed.
namespace FontProbe {
    struct Info {
        bool installed = false;
        bool sjis      = false;
    };

    static std::mutex g_mutex;
    static std::unordered_map<std::string, Info> g_cache;

    static int CALLBACK EnumCb(const LOGFONTA* lf, const TEXTMETRICA*,
                               DWORD, LPARAM lp) {
        Info* info = (Info*)lp;
        info->installed = true;
        if (lf && lf->lfCharSet == SHIFTJIS_CHARSET) info->sjis = true;
        return 1;
    }

    static Info Query(const char* face) {
        if (!face || !face[0]) return Info{};

        std::string key(face);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_cache.find(key);
            if (it != g_cache.end()) return it->second;
        }

        Info info;
        LOGFONTA lf = {};
        lf.lfCharSet = DEFAULT_CHARSET;   // enumerate every charset of this face
        strncpy_s(lf.lfFaceName, face, _TRUNCATE);

        if (HDC hdc = CreateCompatibleDC(nullptr)) {
            EnumFontFamiliesExA(hdc, &lf, (FONTENUMPROCA)EnumCb, (LPARAM)&info, 0);
            DeleteDC(hdc);
        }

        std::lock_guard<std::mutex> lock(g_mutex);
        g_cache[key] = info;
        return info;
    }

    // Set to true after ApplyFontSubstitution downgrades a request to
    // DEFAULT_CHARSET to keep a Latin-only face. JpFallback activates only
    // when this is set.
    static std::atomic<bool> g_relaxed{ false };

    static void ReportOnce(const char* face, const Info& info) {
        static std::mutex mutex;
        static std::unordered_set<std::string> said;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!said.insert(face).second) return;
        }

        if (!info.installed) {
            Log("[FONT] '%s' is not installed - GDI will substitute a default"
                " face. Check the name in [Font] matches the installed family"
                " exactly.\n", face);
        } else if (!info.sjis) {
            Log("[FONT] '%s' has no Shift-JIS charset; requesting it with"
                " DEFAULT_CHARSET so the face is honored. Japanese glyphs fall"
                " back to a Gothic face.\n", face);
        }
    }
}

static void ApplyFontSubstitution(LOGFONTA& lf) {
    // Japanese font names usually mark the proportional variant with a
    // fullwidth 'P' (SJIS 0x82 0x6F), as in MS PGothic. Known limitation:
    // faces that are proportional without the marker, such as Meiryo
    // (which this game requests), are treated as fixed-pitch.
    bool isProportional = (strstr(lf.lfFaceName, "\x82\x6F") != nullptr);

    const char* newFont;
    bool custom = true;
    if (isProportional) {
        if (Config::fontNameProportional[0] != '\0')  newFont = Config::fontNameProportional;
        else if (Config::fontName[0] != '\0')         newFont = Config::fontName;
        else                                        { newFont = "MS PGothic"; custom = false; }
    } else {
        if (Config::fontName[0] != '\0')              newFont = Config::fontName;
        else                                        { newFont = "MS Gothic";  custom = false; }
    }

    // lfFaceName holds LF_FACESIZE characters; the Config buffers hold
    // twice that. Truncate long names; strcpy_s would invoke the
    // invalid-parameter handler and crash.
    strncpy_s(lf.lfFaceName, newFont, _TRUNCATE);

    // Request a charset the face can actually satisfy. Requesting
    // SHIFTJIS_CHARSET from a Latin-only face is what makes GDI silently
    // substitute a different font.
    FontProbe::Info info = FontProbe::Query(lf.lfFaceName);
    lf.lfCharSet = info.sjis ? SHIFTJIS_CHARSET : DEFAULT_CHARSET;

    FontProbe::ReportOnce(lf.lfFaceName, info);
    if (custom && info.installed && !info.sjis)
        FontProbe::g_relaxed.store(true, std::memory_order_relaxed);
}

// True while WordWrap measures a glyph of its own; see GlyphAdvance.
static thread_local bool g_measuringGlyph = false;

//=============================================================================
// Character restoration
//=============================================================================
// Restores characters that Shift-JIS cannot represent.
//
// Shift-JIS has no encoding for characters such as em dashes and accented
// Latin letters. Utf8ToSjisTracked replaces each one with a best-fit
// substitute and records the original character and its byte offset. When
// the engine draws the substitute, GetGlyphOutlineA_Hook finds the record
// and draws the original character instead (through GetGlyphOutlineW).
//
// Records are keyed by string content, not by pointer, for two reasons: a
// backlog redraw renders the same line from a different buffer, and names
// reach the renderer from a different call site than messages.
// PrintSub_Hook selects the record for the string being printed; the glyph
// hook consumes it.
namespace CharRestore {
    typedef std::unordered_map<size_t, wchar_t> Map;

    static std::mutex                           g_mutex;
    static std::unordered_map<std::string, Map> g_registry;

    // Registry key: the string with every '\n' replaced by ' '. Word
    // wrapping replaces spaces with newlines, so the wrapped and unwrapped
    // forms of a line differ only in those bytes; folding '\n' back to ' '
    // makes both forms map to one entry. The replacement is byte-for-byte,
    // which keeps every recorded offset valid.
    static std::string Key(const std::string& s) {
        std::string k = s;
        for (char& c : k) if (c == '\n') c = ' ';
        return k;
    }

    // Entries are never evicted, because the backlog can redraw a line from
    // any earlier point in the session. Growth is bounded in practice:
    // only lines that actually lost a character are registered.
    static void Register(const std::string& sjis, Map lost) {
        if (sjis.empty() || lost.empty()) return;
        std::lock_guard<std::mutex> lock(g_mutex);
        g_registry[Key(sjis)] = std::move(lost);
    }

    static bool Find(const char* sjis, Map& out) {
        if (!sjis || !*sjis) return false;
        out.clear();

        std::string q = Key(sjis);
        std::lock_guard<std::mutex> lock(g_mutex);

        auto it = g_registry.find(q);
        if (it != g_registry.end()) { out = it->second; return true; }

        // No exact match. drawHistory draws name + "\n" + message as one
        // string, so try splitting at each newline and looking up both
        // halves. The join is not necessarily the first newline, because
        // the message may contain its own wrap newlines. Each half must
        // match a complete registry entry; matching substrings could attach
        // one line's records to a different line containing the same text.
        bool any = false;
        auto merge = [&](const std::string& part, size_t base) {
            auto p = g_registry.find(Key(part));
            if (p == g_registry.end()) return false;
            for (const auto& e : p->second) out[base + e.first] = e.second;
            any = true;
            return true;
        };

        // Iterate over newlines in the raw input; the folded key has none.
        std::string raw(sjis);
        for (size_t nl = raw.find('\n'); nl != std::string::npos; nl = raw.find('\n', nl + 1)) {
            bool l = merge(raw.substr(0, nl), 0);
            bool r = merge(raw.substr(nl + 1), nl + 1);
            if (l || r) break;
        }
        return any;
    }

    // Translates offsets from the pre-wrap string to the post-wrap string.
    // Wrapping performs only two edits: inserting '\n' and deleting ' '.
    // A replaced character is never either of those, so walking both
    // strings with two pointers realigns every offset exactly. If the walk
    // hits any other difference, an assumption is wrong; return an empty
    // map instead of guessing.
    static Map Remap(const std::string& before, const std::string& after, const Map& src) {
        Map out;
        size_t i = 0, j = 0;
        while (i < before.size() && j < after.size()) {
            if (before[i] == after[j]) {
                auto it = src.find(i);
                if (it != src.end()) out[j] = it->second;
                i++; j++;
            }
            else if (after[j] == '\n') j++;
            else if (before[i] == ' ')  i++;
            else return {};
        }
        return out;
    }

    // Active for the duration of one printSub call.
    static thread_local Map         t_map;
    static thread_local const char* t_str      = nullptr;
    static thread_local const void* t_printData = nullptr;

    // Offsets into printSub's UxPrintData argument. The engine initializes
    // an iterator at 0x10171D40: +24 holds the start of the string and +28
    // holds the cursor, which the engine advances with CharNextA. So
    // (cursor - start) is the byte offset of the glyph being drawn.
    constexpr size_t kIterStart  = 24;
    constexpr size_t kIterCursor = 28;

    // 0 when there is nothing to substitute at the current cursor.
    static wchar_t CharAtCursor() {
        if (!t_printData || t_map.empty() || g_measuringGlyph) return 0;

        const char* start  = *(const char* const*)((const char*)t_printData + kIterStart);
        const char* cursor = *(const char* const*)((const char*)t_printData + kIterCursor);
        if (!start || !cursor || cursor < start) return 0;
        if (start != t_str) return 0;   // not the string this record describes

        auto it = t_map.find((size_t)(cursor - start));
        return it == t_map.end() ? 0 : it->second;
    }

    // The textOut renderers do not pass UxPrintData, so there is no cursor
    // structure to read. Instead, track the engine's own string walk: the
    // renderer calls CharNextA on a character and rasterizes it before the
    // next CharNextA call, so the most recent CharNextA argument is the
    // address of the glyph currently being drawn.
    static thread_local bool        t_toArmed = false;
    static thread_local const char* t_toBase  = nullptr;  // fragment start
    static thread_local const char* t_toCur   = nullptr;  // glyph being drawn
    static thread_local Map         t_toMap;              // offsets within it

    static void Disarm() {
        t_toArmed = false;
        t_toBase  = nullptr;
        t_toCur   = nullptr;
        t_toMap.clear();
    }

    // The renderer works on its own copy of the text, so find the matching
    // record by content, not by pointer. If the fragment matches more than
    // one registry entry, or occurs more than once inside an entry, do
    // nothing: a wrong match would draw another line's characters.
    static void FragBegin(const char* frag) {
        t_toMap.clear();
        t_toBase = frag;
        if (!frag || !*frag) return;

        const std::string f = Key(frag);

        std::lock_guard<std::mutex> lock(g_mutex);

        // Check for an exact match first. Without this, a complete line
        // that is also a substring of a longer entry would be rejected as
        // ambiguous.
        auto exact = g_registry.find(f);
        if (exact != g_registry.end()) { t_toMap = exact->second; return; }

        const Map* hit = nullptr;
        size_t     hitOff = 0;
        for (const auto& [key, map] : g_registry) {
            size_t p = key.find(f);
            if (p == std::string::npos) continue;
            if (hit) return;                                        // occurs in two entries: ambiguous
            if (key.find(f, p + 1) != std::string::npos) return;    // occurs twice in one entry: ambiguous
            hit = &map;
            hitOff = p;
        }
        if (!hit) return;

        for (const auto& [off, wc] : *hit) {
            if (off >= hitOff && off < hitOff + f.size())
                t_toMap[off - hitOff] = wc;
        }
    }

    // Verify that the character at the tracked cursor is the one GDI was
    // asked for. If they differ, the CharNextA-tracking assumption does not
    // hold on this build; disable restoration for this path so the best-fit
    // substitute renders instead of a wrong glyph.
    static wchar_t CharAtTextOut(UINT uChar) {
        if (!t_toBase || !t_toCur || t_toMap.empty() || g_measuringGlyph) return 0;
        if (t_toCur < t_toBase) return 0;

        auto it = t_toMap.find((size_t)(t_toCur - t_toBase));
        if (it == t_toMap.end()) return 0;

        unsigned char a = (unsigned char)t_toCur[0];
        UINT here = a;
        if ((a >= 0x81 && a <= 0x9F) || (a >= 0xE0 && a <= 0xFC)) {
            unsigned char b = (unsigned char)t_toCur[1];
            if (b) here = ((UINT)a << 8) | b;
        }
        if (here != uChar) {
            static bool s_said = false;
            if (!s_said) {
                s_said = true;
                Log("[ACCENT] fragment cursor out of step (uChar=0x%04X here=0x%04X)"
                    " - textOut restoration disabled\n", uChar, here);
            }
            return 0;
        }
        return it->second;
    }
}

namespace WordWrap {
    bool IsSjisLead(unsigned char c) {
        return (c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC);
    }

    // Print context read from RetouchPrintManager
    struct PrintCtx {
        LONG rectLeft, rectTop, rectRight, rectBottom;
        LONG lineHeight;
        LONG indent;
        HDC hdc;
        UINT fuFormat;
        bool vertical;
        LOGFONTA logFont;   // this context's font, substitution applied
    };

    // Private memory DC used for measuring. Do not borrow the engine's font
    // DC (context +0x244): it is reference-counted, and its acquire/release
    // path also selects the palette, so touching it from outside the engine
    // could disturb rendering. The engine builds each context's HFONT from
    // the LOGFONTA at +0x1A4 (see 0x10171220); creating the same font in
    // our own DC gives identical metrics.
    //
    // The font is per print context, not global. The name box and the
    // message body use different fonts. Measuring the body with the name
    // font under-reports every advance, so no break point is ever emitted
    // and the engine truncates the line mid-word.
    static std::mutex g_fontMutex;
    static HDC        g_measureDC = nullptr;
    static HFONT      g_measureFont = nullptr;
    static LOGFONTA   g_measureFontLf = {};

    static void InvalidateGlyphsForFont(HFONT hf);

    // Private DC carrying a font equivalent to `lf`. Reused across calls; only
    // rebuilt when the requested LOGFONT actually changes.
    static HDC GetMeasureDC(const LOGFONTA& lf) {
        std::lock_guard<std::mutex> lock(g_fontMutex);

        if (!g_measureDC) {
            g_measureDC = CreateCompatibleDC(nullptr);
            if (!g_measureDC) return nullptr;
        }

        if (!g_measureFont || memcmp(&g_measureFontLf, &lf, sizeof(LOGFONTA)) != 0) {
            // Bypass our own CreateFontIndirectA hook: substitution is already
            // applied, and re-entering it would flush the glyph cache and log.
            HFONT nf = g_origCreateFontIndirectA
                     ? g_origCreateFontIndirectA(&lf)
                     : CreateFontIndirectA(&lf);
            if (!nf) return nullptr;

            SelectObject(g_measureDC, nf);
            if (g_measureFont) {
                // The glyph cache is keyed by HFONT. Remove this font's entries
                // before deleting it: GDI can reissue the same handle value for
                // a future font, which would then read stale advances.
                InvalidateGlyphsForFont(g_measureFont);
                DeleteObject(g_measureFont);
            }
            g_measureFont = nf;
            g_measureFontLf = lf;
        }
        return g_measureDC;
    }

    // Glyph advance cache, keyed on (HFONT, char, format)
    static std::mutex g_glyphCacheMutex;
    static std::unordered_map<uint64_t, int> g_glyphCache;

    // Drop cached advances for a handle that has been reused for another font.
    static void InvalidateGlyphsForFont(HFONT hf) {
        if (!hf) return;
        const uint64_t prefix = (uint64_t)(uintptr_t)hf << 32;

        std::lock_guard<std::mutex> lock(g_glyphCacheMutex);
        if (g_glyphCache.empty()) return;
        for (auto it = g_glyphCache.begin(); it != g_glyphCache.end(); ) {
            if ((it->first & 0xFFFFFFFF00000000ULL) == prefix) it = g_glyphCache.erase(it);
            else ++it;
        }
    }

    // Release the private measuring DC and font (process detach).
    static void ReleaseMeasureDC() {
        std::lock_guard<std::mutex> lock(g_fontMutex);
        // DC first: DeleteObject fails on a font still selected into one.
        if (g_measureDC)   { DeleteDC(g_measureDC);       g_measureDC = nullptr; }
        if (g_measureFont) { DeleteObject(g_measureFont); g_measureFont = nullptr; }
        ZeroMemory(&g_measureFontLf, sizeof(g_measureFontLf));
    }

    // Read the geometry half of the print context (no DC yet).
    // pThis is printEx's "this"; the chain matches the engine's own accessor.
    static bool GetPrintCtx(void* pThis, PrintCtx& out) {
        if (!pThis) return false;

        void* p1 = *(void**)((char*)pThis + PrintMgrOffsets::Target);
        if (!p1) return false;
        void* ctx = *(void**)((char*)p1 + PrintMgrOffsets::FCStringData);
        if (!ctx) return false;

        auto field = [ctx](size_t off) -> LONG {
            return *(LONG*)((char*)ctx + off);
        };

        out.vertical   = (field(PrintCtxOffsets::Flags) & 4) != 0;
        out.rectLeft   = field(PrintCtxOffsets::RectLeft);
        out.rectTop    = field(PrintCtxOffsets::RectTop);
        out.rectRight  = field(PrintCtxOffsets::RectRight);
        out.rectBottom = field(PrintCtxOffsets::RectBottom);
        out.lineHeight = field(PrintCtxOffsets::LineHeight);
        out.indent     = field(PrintCtxOffsets::Indent);
        out.hdc        = nullptr;   // supplied by GetMeasureDC()

        // Same rasterization format the engine asks for: 4 * (aa != 0) + 1,
        // i.e. GGO_GRAY4_BITMAP or GGO_BITMAP.
        unsigned char aaFlag = *((unsigned char*)ctx + PrintCtxOffsets::AntiAlias);
        out.fuFormat = 4u * (aaFlag != 0) + 1u;

        // This context's own font; the engine builds its HFONT from here.
        memcpy(&out.logFont, (char*)ctx + PrintCtxOffsets::LogFont, sizeof(LOGFONTA));

        // A rejected context silently downgrades the feature to character
        // wrap, so log which check failed and the values that were read.
        // Log once per session.
        auto reject = [&](const char* why) -> bool {
            static bool s_said = false;
            if (!s_said) {
                s_said = true;
                Log("[WRAP] context rejected (%s) - using character wrap\n", why);
                Log("[WRAP]   ctx=%p rect=(%d,%d)-(%d,%d) lineHeight=%d indent=%d aa=%u\n",
                    ctx, out.rectLeft, out.rectTop, out.rectRight, out.rectBottom,
                    out.lineHeight, out.indent, (unsigned)aaFlag);
                Log("[WRAP]   font='%s' h=%d weight=%d cs=%d pitch=%u\n",
                    out.logFont.lfFaceName, out.logFont.lfHeight,
                    out.logFont.lfWeight, out.logFont.lfCharSet,
                    (unsigned)out.logFont.lfPitchAndFamily);
            }
            return false;
        };

        if (out.rectRight <= out.rectLeft || out.rectBottom <= out.rectTop)
            return reject("empty rect");
        if (out.lineHeight <= 0 || out.lineHeight > 512)
            return reject("bad lineHeight");
        if (out.rectRight - out.rectLeft > 8192)
            return reject("rect too wide");
        if (out.indent < 0 || out.indent >= out.rectRight - out.rectLeft)
            return reject("bad indent");
        if (out.logFont.lfHeight == 0)
            return reject("font height 0");
        if (out.logFont.lfFaceName[0] == '\0')
            return reject("empty face name");

        // The stored LOGFONT is what the engine asked for; what actually renders
        // is the substituted face, so measure with that.
        ApplyFontSubstitution(out.logFont);

        return true;
    }

    // Returns the pen advance for one glyph. A single call is enough: GDI
    // fills in the same GLYPHMETRICS whether or not a bitmap buffer is
    // supplied. g_measuringGlyph tells GetGlyphOutlineA_Hook not to apply
    // character restoration; the engine's print cursor has nothing to do
    // with the glyph measured here.
    static int GlyphAdvance(HDC hdc, UINT ch, UINT fuFormat) {
        struct MeasureScope {
            MeasureScope()  { g_measuringGlyph = true;  }
            ~MeasureScope() { g_measuringGlyph = false; }
        } scope;
        HFONT hFont = (HFONT)GetCurrentObject(hdc, OBJ_FONT);
        // Disjoint bit fields: fuFormat 0..7, ch 8..23, HFONT 32..63
        uint64_t key = ((uint64_t)(uintptr_t)hFont << 32)
                     | ((uint64_t)(ch & 0xFFFF) << 8)
                     | (uint64_t)(fuFormat & 0xFF);

        {
            std::lock_guard<std::mutex> lock(g_glyphCacheMutex);
            auto it = g_glyphCache.find(key);
            if (it != g_glyphCache.end())
                return it->second;
        }

        MAT2 mat2 = {};
        mat2.eM11.value = 1;
        mat2.eM22.value = 1;

        GLYPHMETRICS gm = {};

        DWORD size = GetGlyphOutlineA(hdc, ch, fuFormat, &gm, 0, nullptr, &mat2);
        if (size == GDI_ERROR)
            return -1;

        int advance = gm.gmCellIncX;

        {
            std::lock_guard<std::mutex> lock(g_glyphCacheMutex);
            g_glyphCache[key] = advance;
        }

        return advance;
    }

    // Kinsoku: characters that may not begin a line -- trailing punctuation,
    // the prolonged sound mark, closing brackets (even trail) and small kana.
    static bool IsJpNoLineStart(unsigned char lead, unsigned char trail) {
        if (lead == Sjis::PunctLead) {
            if (trail >= Sjis::PunctFirst && trail <= Sjis::PunctLast) return true;
            if (trail == Sjis::ProlongedMark) return true;
            if (trail >= Sjis::BracketFirst && trail <= Sjis::BracketLast)
                return (trail % 2) == 0;   // even closes the pair
        }
        unsigned short ch = (unsigned short)((lead << 8) | trail);
        for (unsigned short k : Sjis::kSmallKana) {
            if (k == ch) return true;
        }
        return false;
    }

    // Kinsoku: characters that may not end a line -- opening brackets (odd trail).
    static bool IsJpNoLineEnd(unsigned char lead, unsigned char trail) {
        if (lead != Sjis::PunctLead) return false;
        if (trail < Sjis::BracketFirst || trail > Sjis::BracketLast) return false;
        return (trail % 2) == 1;           // odd opens the pair
    }

    struct BreakPoint {
        size_t pos;        // index in result where the '\n' belongs
        bool   replace;    // true: overwrite a space; false: insert
        int    widthThru;  // line width accumulated through this point
    };

    static std::string WrapPixels(const std::string& sjis, const PrintCtx& ctx, int& linesOut) {
        int budget = (ctx.rectRight - ctx.rectLeft) - ctx.indent - Config::wrapSafetyPx;
        if (budget <= 0) {
            linesOut = 1;
            return sjis;
        }

        std::string result;
        result.reserve(sjis.size() + 64);

        int    lineWidth = 0;
        size_t lineStart = 0;
        int    lines     = 1;
        bool   haveBreak = false;
        BreakPoint br    = {};

        // Emit the newline at the recorded opportunity and re-base the line.
        auto breakAtOpportunity = [&]() {
            if (br.replace) result[br.pos] = '\n';
            else            result.insert(br.pos, 1, '\n');

            lineStart = br.pos + 1;
            lineWidth -= br.widthThru;
            if (lineWidth < 0) lineWidth = 0;
            haveBreak = false;
            lines++;

            // Runs of spaces: the break consumed one, drop any others so the
            // new line does not start indented by leftover whitespace.
            while (lineStart < result.size() && result[lineStart] == ' ') {
                int sp = GlyphAdvance(ctx.hdc, ' ', ctx.fuFormat);
                result.erase(lineStart, 1);
                if (sp > 0) lineWidth -= sp;
                if (lineWidth < 0) lineWidth = 0;
            }
        };

        auto breakHard = [&]() {
            result += '\n';
            lineStart = result.size();
            lineWidth = 0;
            haveBreak = false;
            lines++;
        };

        for (size_t i = 0; i < sjis.size(); ) {
            unsigned char c = (unsigned char)sjis[i];

            // Author's own newline: honour it and reset
            if (c == '\n') {
                result += c;
                lineStart = result.size();
                lineWidth = 0;
                haveBreak = false;
                lines++;
                i++;
                continue;
            }

            bool dbcs = IsSjisLead(c) && (i + 1 < sjis.size());
            unsigned char lead = c;
            unsigned char trail = dbcs ? (unsigned char)sjis[i + 1] : 0;
            UINT ch = dbcs ? (UINT)((lead << 8) | trail) : (UINT)c;

            int adv = GlyphAdvance(ctx.hdc, ch, ctx.fuFormat);
            if (adv < 0) adv = ctx.lineHeight / 2;   // missing glyph: approximate

            // Would this glyph leave the box?
            if (lineWidth + adv > budget && lineWidth > 0) {
                if (haveBreak && br.pos > lineStart) breakAtOpportunity();
                else                                 breakHard();
            }

            result += (char)lead;
            if (dbcs) result += (char)trail;
            lineWidth += adv;

            if (dbcs) {
                // Any boundary between two double-byte chars is a legal break,
                // subject to kinsoku on both sides.
                if (!IsJpNoLineEnd(lead, trail) && i + 3 < sjis.size()) {
                    unsigned char nLead = (unsigned char)sjis[i + 2];
                    unsigned char nTrail = (unsigned char)sjis[i + 3];
                    if (IsSjisLead(nLead) && !IsJpNoLineStart(nLead, nTrail)) {
                        br = { result.size(), false, lineWidth };
                        haveBreak = true;
                    }
                }
                i += 2;
            } else {
                // A space is the break opportunity for English, and it is
                // replaced by the newline rather than left dangling.
                if (c == ' ') {
                    br = { result.size() - 1, true, lineWidth };
                    haveBreak = true;
                }
                i++;
            }
        }

        linesOut = lines;
        return result;
    }

    static int ColumnsFrom(const std::string& s, size_t from) {
        int n = 0;
        for (size_t k = from; k < s.size(); ) {
            bool dbcs = IsSjisLead((unsigned char)s[k]) && k + 1 < s.size();
            n += dbcs ? 2 : 1;
            k += dbcs ? 2 : 1;
        }
        return n;
    }

    // maxWidth counts halfwidth columns, so a fullwidth character costs 2 and
    // a width of 70 holds 70 Latin characters or 35 Japanese ones.
    //
    // No kinsoku here, unlike WrapPixels: a hard break can leave 、or 。at the
    // start of a line. Pixel mode is the default and does apply it.
    static std::string WrapByChars(const std::string& text, int maxWidth) {
        if (text.empty() || maxWidth <= 0) return text;

        std::string result;
        result.reserve(text.size() + 64);

        int lineLen = 0;
        size_t lineStart = 0;
        size_t lastSpace = std::string::npos;

        for (size_t i = 0; i < text.size(); ) {
            unsigned char c = (unsigned char)text[i];

            if (c == '\n') {
                result += c;
                lineLen = 0;
                lineStart = result.size();
                lastSpace = std::string::npos;
                i++;
                continue;
            }

            const bool dbcs = IsSjisLead(c) && i + 1 < text.size();
            const int  cols = dbcs ? 2 : 1;

            if (lineLen + cols > maxWidth && lineLen > 0) {
                if (lastSpace != std::string::npos && lastSpace > lineStart) {
                    result[lastSpace] = '\n';
                    lineStart = lastSpace + 1;
                    lineLen = ColumnsFrom(result, lineStart);
                } else {
                    result += '\n';
                    lineStart = result.size();
                    lineLen = 0;
                }
                lastSpace = std::string::npos;
            }

            if (!dbcs && c == ' ') lastSpace = result.size();

            result += text[i];
            if (dbcs) result += text[i + 1];
            lineLen += cols;
            i += cols;
        }

        return result;
    }

    static std::mutex g_overflowMutex;
    static std::unordered_set<std::string> g_overflowLogged;

    static bool ClaimOverflowReport(const std::string& key) {
        std::lock_guard<std::mutex> lock(g_overflowMutex);
        return g_overflowLogged.insert(key).second;
    }

    static void ReportOverflow(const std::string& file, int index,
                               int needLines, int maxLines, int boxPx,
                               const std::string& sjis) {
        if (!Config::warnOnOverflow) return;

        std::string key = file + ":" + std::to_string(index);
        if (!ClaimOverflowReport(key)) return;

        std::string utf8 = Encoding::SjisToUtf8(sjis.c_str());
        // Collapse the newlines we just inserted so the log stays one line
        for (size_t p = 0; (p = utf8.find('\n', p)) != std::string::npos; )
            utf8.replace(p, 1, " / ");

        Log("[WRAP] OVERFLOW %s  needs %d lines, box holds %d  (box=%dpx)\n",
            key.c_str(), needLines, maxLines, boxPx);
        Log("[WRAP]   \"%s\"\n", utf8.c_str());
    }

    std::string WrapForPrint(void* printMgr, const std::string& sjis) {
        if (sjis.empty()) return sjis;
        if (Config::wrapMode == Config::WrapMode::Off) return sjis;

        if (Config::wrapMode == Config::WrapMode::Pixel) {
            PrintCtx ctx;
            if (GetPrintCtx(printMgr, ctx) && !ctx.vertical) {
                // Our own DC, carrying a copy of *this* context's font.
                HDC hdc = GetMeasureDC(ctx.logFont);

                if (hdc) {
                    ctx.hdc = hdc;

                    int maxLines = (ctx.rectBottom - ctx.rectTop) / ctx.lineHeight;

                    // One-time report so the measured geometry is in the log
                    static bool s_reported = false;
                    if (!s_reported) {
                        s_reported = true;
                        Log("[WRAP] pixel mode active: box=%dpx (%d..%d) lineHeight=%d "
                            "maxLines=%d indent=%d fmt=%u safety=%dpx font=%s h=%d\n",
                            ctx.rectRight - ctx.rectLeft, ctx.rectLeft, ctx.rectRight,
                            ctx.lineHeight, maxLines, ctx.indent, ctx.fuFormat,
                            Config::wrapSafetyPx, ctx.logFont.lfFaceName,
                            ctx.logFont.lfHeight);
                    }

                    int lines = 1;
                    std::string wrapped = WrapPixels(sjis, ctx, lines);

                    if (maxLines > 0 && lines > maxLines) {
                        ReportOverflow(CurrentSceneFile(), t_cmdIndex,
                                       lines, maxLines,
                                       ctx.rectRight - ctx.rectLeft, wrapped);
                    }
                    return wrapped;
                }

                // Expected exactly once, before the engine has drawn any glyph.
                static bool s_warnedNoDc = false;
                if (!s_warnedNoDc) {
                    s_warnedNoDc = true;
                    Log("[WRAP] font not captured yet - character wrap for this line\n");
                }
            }
        }

        return WrapByChars(sjis, Config::wordWrapWidth);
    }
}

//=============================================================================
// Hot Reload Thread
//=============================================================================
static std::atomic<bool> g_running{true};
static HANDLE g_hotkeyThread = nullptr;
static std::atomic<bool> g_modulePinFailed{false};

// GetAsyncKeyState reads keys system-wide. Without a focus check, pressing
// F5 in a text editor would reload the game's translations. Two checks are
// required, and each covers a case the other misses:
//
// 1. Process ID: rejects keys typed into other applications. Not enough by
//    itself, because the classic console window reports this process's PID
//    (verified by testing, not assumed) and the console opens in front of
//    the game, so keys typed into the console would pass.
// 2. Console window: rejects keys typed into our own console. Not enough by
//    itself, because a terminal host that owns its window (for example,
//    Windows Terminal) runs under a different PID, which only check 1
//    rejects.
static bool GameHasFocus() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;

    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid != GetCurrentProcessId()) return false;

    if (HWND console = GetConsoleWindow()) {
        if (fg == console || GetAncestor(fg, GA_ROOT) == console) return false;
    }
    return true;
}

static bool WaitForKeyRelease(int vk) {
    while (g_running && (GetAsyncKeyState(vk) & 0x8000)) Sleep(10);
    return g_running;
}

static DWORD WINAPI HotkeyThreadProc(LPVOID) {
    while (g_running) {
        if (!GameHasFocus()) {
            Sleep(Constants::kHotkeyPollIntervalMs);
            continue;
        }

        // Re-check focus after the key is released: the user can hold the
        // key while alt-tabbing away.
        // Reload translations
        if (GetAsyncKeyState(Config::reloadHotkey) & 0x8000) {
            if (!WaitForKeyRelease(Config::reloadHotkey)) break;
            if (!GameHasFocus()) continue;
            g_translationDB.Reload();
            MessageBeep(MB_OK);
        }

        // Print stats
        if (GetAsyncKeyState(Config::statsHotkey) & 0x8000) {
            if (!WaitForKeyRelease(Config::statsHotkey)) break;
            if (!GameHasFocus()) continue;
            g_translationDB.PrintStats();
            MessageBeep(MB_OK);
        }

        // Toggle logging
        if (GetAsyncKeyState(Config::logToggleHotkey) & 0x8000) {
            if (!WaitForKeyRelease(Config::logToggleHotkey)) break;
            if (!GameHasFocus()) continue;
            bool on = !Config::enableTextLogging.load();
            Config::enableTextLogging.store(on);
            Log("[*] Text logging: %s\n", on ? "ON" : "OFF");
            MessageBeep(MB_OK);
        }

        Sleep(Constants::kHotkeyPollIntervalMs);
    }
    return 0;
}

//=============================================================================
// Character ID -> Original Name Lookup (from char_table.tsv)
//=============================================================================
static std::unordered_map<int, std::string> g_charIdToName;  // ID -> original JP name

static void LoadCharIdTable(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        Log("[TL] No char_table.tsv found\n");
        return;
    }

    std::streamoff size = file.tellg();
    if (size < 0) {
        Log("[TL] Cannot size: %s\n", path);
        return;
    }
    file.seekg(0);
    std::string content((size_t)size, 0);
    if (size > 0) file.read(&content[0], size);

    Encoding::Type enc = Encoding::Detect(content.c_str(), (size_t)size);
    std::string utf8 = Encoding::ToUtf8(content, enc);

    std::istringstream iss(utf8);
    std::string line;
    int count = 0;

    while (std::getline(iss, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (line.back() == '\r') line.pop_back();
        if (line.find("ID") == 0) continue;  // Skip header

        size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;

        int id = std::atoi(line.substr(0, tab).c_str());
        std::string name = line.substr(tab + 1);

        if (id > 0 && !name.empty()) {
            g_charIdToName[id] = name;
            count++;
        }
    }

    Log("[TL] Loaded %d CharID mappings\n", count);
}

//=============================================================================
// INI Checksum Bypass - Make INI Editable
//=============================================================================
typedef unsigned int(__thiscall* Fn_CalcIniValue)(void* pThis);
static Fn_CalcIniValue g_origCalcIniValue = nullptr;

static unsigned int __fastcall CalcIniValue_Hook(void* pThis, void* edx) {
    constexpr unsigned int ORIGINAL_CHECKSUM = 0xFC561D8B;
    return ORIGINAL_CHECKSUM;
}

//=============================================================================
// Hook: RetouchSystem::cmdMessage()
//=============================================================================
// Runs immediately before the say() call that renders the line. Its job is
// to capture the command's index so AdvCharSay_Hook can look up the
// translation by (file, index) instead of by text.
//
// The index is read from the command record at +0xD4. liteExec writes it
// there during dispatch: it computes the record address as
// base + index * 0xD8 and then stores that same index into the record, so
// the field reliably holds the command's position in the loaded script,
// which is exactly what the TSV INDEX column contains.
//
// Record layout (from RetouchSystem::liteExec and liteCommandInfo):
//   +0x04  header dword: opcode | nparams<<16 | nstrings<<24 | flags<<28
//   +0x08  int32 params[]
//   +0x60  string records, 12 bytes each; the char* is at record+20
//          (cmdMessage reads the name from +0x60, the message from +0x6C)
//   +0xD4  command index        liteExec: mov [edi+0D4h], eax
//   0xD8   record stride        liteExec: imul edi, 0D8h
constexpr size_t kCmdDataIndexOffset = 0xD4;

typedef void(__thiscall* Fn_CmdMessage)(void* pThis, void* cmdData, const char* arg);
static Fn_CmdMessage g_origCmdMessage = nullptr;

static void __fastcall CmdMessage_Hook(void* pThis, void* edx, void* cmdData, const char* arg)
{
    int prev = t_cmdIndex;
    t_cmdIndex = cmdData
        ? *(int*)((char*)cmdData + kCmdDataIndexOffset)
        : -1;
    g_origCmdMessage(pThis, cmdData, arg);
    t_cmdIndex = prev;   // Restore: say() is also reachable from paths that
                         // never pass through cmdMessage.
}

//=============================================================================
// Hook: RetouchAdvCharacter::say()
//=============================================================================
typedef void(__thiscall* Fn_AdvCharSay)(
    void* pThis, int voiceId, const char* name, const char* message,
    bool flag, int flags, int p1, int p2, int p3, void* printParam
);
static Fn_AdvCharSay g_origAdvCharSay = nullptr;

static void __fastcall AdvCharSay_Hook(
    void* pThis, void* edx,
    int voiceId, const char* name, const char* message,
    bool flag, int flags, int p1, int p2, int p3, void* printParam)
{
    const char* finalName = name;
    const char* finalMsg = message;

    // If name is NULL, look up by CharID
    if ((!name || !*name) && pThis) {
        int charId = *((int*)((uintptr_t)pThis + 4));

        if (charId > 0) {
            // char -> name
            auto it = g_charIdToName.find(charId);
            if (it != g_charIdToName.end()) {
                const std::string& origName = it->second;

                // name -> translation
                std::string sjisName = Encoding::Utf8ToSjis(origName.c_str());
                std::string tlUtf8 = g_translationDB.FindNameTranslation(sjisName.c_str(), nullptr);

                if (!tlUtf8.empty()) {
                    CharRestore::Map lost;
                    std::string sjis = Encoding::Utf8ToSjisTracked(tlUtf8.c_str(), lost);
                    if (!sjis.empty()) {
                        CharRestore::Register(sjis, std::move(lost));
                        finalName = g_stringPool.Store(sjis);
                        if (Config::enableTextLogging) {
                            Log("[SAY] CharID %d (%s) -> %s\n", charId, origName.c_str(), tlUtf8.c_str());
                        }
                    }
                } else {
                    // no translation, use original name
                    finalName = g_stringPool.Store(sjisName);
                }
            }
        }
    }
    // translate inline name
    else if (name && *name) {
        std::string tlUtf8 = g_translationDB.FindNameTranslation(name, message);
        if (!tlUtf8.empty()) {
            CharRestore::Map lost;
            std::string sjis = Encoding::Utf8ToSjisTracked(tlUtf8.c_str(), lost);
            if (!sjis.empty()) {
                CharRestore::Register(sjis, std::move(lost));
                finalName = g_stringPool.Store(sjis);
            }
        }
    }

    // Translate message. t_cmdIndex is set by CmdMessage_Hook when this say()
    // came from a MESSAGE command, giving an exact (file, index) match instead
    // of a text match -- the only way to disambiguate lines whose text repeats.
    std::string msgAsRendered;   // UTF-8 translation, kept for logging; the
                                 // Shift-JIS copy loses accented characters.
    if (message && *message) {
        std::string tlUtf8 = g_translationDB.FindMessageTranslation(
            message, CurrentSceneFile(), t_cmdIndex);
        if (!tlUtf8.empty()) {
            msgAsRendered = tlUtf8;
            CharRestore::Map lost;
            std::string sjis = Encoding::Utf8ToSjisTracked(tlUtf8.c_str(), lost);
            if (!sjis.empty()) {
                // Translate only; do not wrap here. printEx runs later in
                // this call chain and owns the wrapping, because it is the
                // only hook that can reach the text box geometry and font.
                CharRestore::Register(sjis, std::move(lost));
                finalMsg = g_stringPool.Store(sjis);
            }
        }
    }

    // Log
    if (Config::enableTextLogging) {
        std::string nameUtf8 = name ? Encoding::SjisToUtf8(name) : "(null)";
        std::string msgUtf8 = message ? Encoding::SjisToUtf8(message) : "(null)";

        Log("[SAY] voiceId=%d flags=0x%08X\n", voiceId, flags);
        Log("      name=\"%s\"\n", nameUtf8.c_str());
        Log("      msg=\"%s\"\n", msgUtf8.c_str());

        if (finalName != name || finalMsg != message) {
            std::string tlNameUtf8 = finalName ? Encoding::SjisToUtf8(finalName) : "";
            std::string tlMsgUtf8 = !msgAsRendered.empty()
                                  ? msgAsRendered
                                  : (finalMsg ? Encoding::SjisToUtf8(finalMsg) : "");
            Log("  --> name=\"%s\"\n", tlNameUtf8.c_str());
            Log("  --> msg=\"%s\"\n", tlMsgUtf8.c_str());
        }
    }

    g_origAdvCharSay(pThis, voiceId, finalName, finalMsg, flag, flags, p1, p2, p3, printParam);
}

//=============================================================================
// Hook: RetouchPrintManager::printSub()
//=============================================================================
// Carries the UxPrintData that owns the print cursor.
typedef char(__thiscall* Fn_PrintSub)(void* pThis, const char* str, void* printData,
                                      unsigned long flags);
static Fn_PrintSub g_origPrintSub = nullptr;

static char __fastcall PrintSub_Hook(void* pThis, void* edx, const char* str,
                                     void* printData, unsigned long flags)
{
    // printSub can recurse, so save and restore the thread-local state
    // instead of clearing it.
    const void*      prevData = CharRestore::t_printData;
    const char*      prevStr  = CharRestore::t_str;
    CharRestore::Map prevMap;
    prevMap.swap(CharRestore::t_map);

    CharRestore::t_printData = printData;
    CharRestore::t_str       = str;
    if (!CharRestore::Find(str, CharRestore::t_map)) CharRestore::t_map.clear();

    char r = g_origPrintSub(pThis, str, printData, flags);

    CharRestore::t_printData = prevData;
    CharRestore::t_str       = prevStr;
    CharRestore::t_map.swap(prevMap);
    return r;
}

//=============================================================================
// Hook: RetouchPrintManager::printEx()
//=============================================================================
typedef void(__thiscall* Fn_PrintEx)(
    void* pThis, int charId, int msgId, const char* name, const char* message,
    unsigned long flags, unsigned long linkData
);
static Fn_PrintEx g_origPrintEx = nullptr;

static void __fastcall PrintEx_Hook(
    void* pThis, void* edx,
    int charId, int msgId, const char* name, const char* message,
    unsigned long flags, unsigned long linkData)
{
    const char* finalMsg = message;

    if (message && *message) {
        // By the time printEx runs, say() has usually already replaced the
        // text with English, so the lookup below normally misses. Wrap
        // whatever text arrives, translated or not: this is the only hook
        // with access to the text box and its font.
        std::string sjis;
        CharRestore::Map lost;

        // countMiss=false: say() already counted this line, and the text
        // here is usually already English, so a miss at this point does not
        // mean the line is untranslated.
        std::string tlUtf8 = g_translationDB.FindMessageTranslation(
            message, CurrentSceneFile(), t_cmdIndex, /*countMiss=*/false);
        if (!tlUtf8.empty()) {
            sjis = Encoding::Utf8ToSjisTracked(tlUtf8.c_str(), lost);
            CharRestore::Register(sjis, lost);
        }
        if (sjis.empty()) {
            sjis = message;
            CharRestore::Find(message, lost);   // registered upstream by say()
        }

        std::string wrapped = WordWrap::WrapForPrint(pThis, sjis);
        if (wrapped != message) {
            finalMsg = g_stringPool.Store(wrapped);
        }

        // The recorded offsets refer to the unwrapped string, but the
        // engine draws the wrapped string, so remap the offsets and
        // register the wrapped form too.
        if (!lost.empty() && wrapped != sjis) {
            auto moved = CharRestore::Remap(sjis, wrapped, lost);
            if (moved.size() == lost.size()) {
                CharRestore::Register(wrapped, std::move(moved));
            }
            else {
                Log("[ACCENT] remap lost %zu of %zu entries - disabled for this line\n",
                    lost.size() - moved.size(), lost.size());
            }
        }
    }

    g_origPrintEx(pThis, charId, msgId, name, finalMsg, flags, linkData);
}

//=============================================================================
// Hook: RetouchPrintManager::drawHistory() - backlog repaint
//=============================================================================
// Repaints one backlog entry. The call path matters:
//
//   cmdMessage                 <- CmdMessage_Hook sets t_cmdIndex
//     say()
//       printEngine (virtual)
//         messageEngine
//           printEx            <- the live line
//           hitwait()          <- input wait; backlog UI runs here
//             prevHistory -> drawHistory -> printEx
//
// The final printEx runs while still inside cmdMessage, so t_cmdIndex holds
// the live line's index. Before this hook, opening the backlog displayed
// the current message for every entry.
typedef void(__thiscall* Fn_DrawHistory)(void* pThis, void* container);
static Fn_DrawHistory g_origDrawHistory = nullptr;

static void __fastcall DrawHistory_Hook(void* pThis, void* edx, void* container)
{
    bool prev = t_inHistory;
    t_inHistory = true;
    g_origDrawHistory(pThis, container);
    t_inHistory = prev;
}

//=============================================================================
// Hook: SaveDataTitle (LABEL translation)
//=============================================================================
typedef bool(__thiscall* Fn_SaveDataIsValid)(void* pThis, int slotType, int slotIndex);
typedef void*(__thiscall* Fn_SaveDataGetItem)(void* pThis, int slotType, int slotIndex);

static Fn_SaveDataIsValid g_SaveDataIsValid = nullptr;
static Fn_SaveDataGetItem g_SaveDataGetItem = nullptr;

typedef int(__thiscall* Fn_SaveDataTitle)(
    void* pThis, void* fcString, int slotType, int slotIndex, bool useTemplate, unsigned int* outTime
);
static Fn_SaveDataTitle g_origSaveDataTitle = nullptr;

static int __fastcall SaveDataTitle_Hook(
    void* pThis, void* edx,
    void* fcString, int slotType, int slotIndex, bool useTemplate, unsigned int* outTime)
{
    const bool trace = Config::enableTextLogging;

    if (trace) Log("[SAVE] title() called: type=%d index=%d\n", slotType, slotIndex);

    // Check valid
    if (!g_SaveDataIsValid(pThis, slotType, slotIndex)) {
        if (trace) Log("[SAVE] Invalid slot\n");
        return g_origSaveDataTitle(pThis, fcString, slotType, slotIndex, useTemplate, outTime);
    }

    DWORD* item = (DWORD*)g_SaveDataGetItem(pThis, slotType, slotIndex);

    // Empty slot - call original
    if (!item || item[0] == 0) {
        if (trace) Log("[SAVE] Empty slot\n");
        return g_origSaveDataTitle(pThis, fcString, slotType, slotIndex, useTemplate, outTime);
    }

    // Get label
    DWORD labelFCString = item[2];
    if (!labelFCString) {
        return g_origSaveDataTitle(pThis, fcString, slotType, slotIndex, useTemplate, outTime);
    }
    const char* labelSjis = *(const char**)(labelFCString + 0x14);
    if (trace) {
        Log("[SAVE] Label raw: %p -> \"%s\"\n", labelSjis,
            labelSjis ? Encoding::SjisToUtf8(labelSjis).c_str() : "(null)");
    }

    // Try translate
    const char* finalLabel = labelSjis;
    std::string translatedSjis;

    if (labelSjis && *labelSjis) {
        std::string translated = g_translationDB.FindLabelTranslation(labelSjis);

        if (!translated.empty()) {
            CharRestore::Map lost;
            translatedSjis = Encoding::Utf8ToSjisTracked(translated.c_str(), lost);
            CharRestore::Register(translatedSjis, std::move(lost));
            finalLabel = translatedSjis.c_str();
            if (trace) Log("[SAVE] Found translation: \"%s\"\n", translated.c_str());
        } else if (trace) {
            Log("[SAVE] No translation found!\n");
        }
    }

    // Temporarily replace the label in the item
    const char* originalPtr = *(const char**)(labelFCString + 0x14);
    *(const char**)(labelFCString + 0x14) = finalLabel;

    // Call original
    int result = g_origSaveDataTitle(pThis, fcString, slotType, slotIndex, useTemplate, outTime);

    // Restore original
    *(const char**)(labelFCString + 0x14) = originalPtr;

    return result;
}

//=============================================================================
// Hook: RetouchSystem::prepareQuestion() (CHOICE translation)
//=============================================================================
typedef void(__thiscall* Fn_PrepareQuestion)(void* pThis, int choiceId, const char* text);
static Fn_PrepareQuestion g_origPrepareQuestion = nullptr;

static void __fastcall PrepareQuestion_Hook(void* pThis, void* edx, int choiceId, const char* text)
{
    const char* finalText = text;

    if (text && *text) {
        std::string translated = g_translationDB.FindMessageTranslation(text);
        if (!translated.empty()) {
            CharRestore::Map lost;
            std::string sjis = Encoding::Utf8ToSjisTracked(translated.c_str(), lost);
            if (!sjis.empty()) {
                CharRestore::Register(sjis, std::move(lost));
                finalText = g_stringPool.Store(sjis);

                if (Config::enableTextLogging) {
                    Log("[CHOICE] %d: \"%s\" -> \"%s\"\n",
                        choiceId,
                        Encoding::SjisToUtf8(text).c_str(),
                        translated.c_str());
                }
            }
        }
    }

    g_origPrepareQuestion(pThis, choiceId, finalText);
}

//=============================================================================
// Hook: RetouchSystem::liteLoad() - Scene Tracking
//=============================================================================
typedef void(__thiscall* Fn_LiteSetDebugMode)(void* pThis, unsigned int flags);
static Fn_LiteSetDebugMode g_liteSetDebugMode = nullptr;

typedef char(__thiscall* Fn_LiteLoad)(void* pThis, const char* path, unsigned int flags);
static Fn_LiteLoad g_origLiteLoad = nullptr;

static char __fastcall LiteLoad_Hook(void* pThis, void* edx, const char* path, unsigned int flags)
{
    // Capture RetouchSystem pointer
    {
        std::lock_guard<std::mutex> lock(DebugJump::g_mutex);

        // First capture - enable debug mode if configured
        if (!DebugJump::g_retouchSystem && Config::enableDebugMode && g_liteSetDebugMode) {
            DebugJump::g_retouchSystem = pThis;
            g_liteSetDebugMode(pThis, 0x10001);
            Log("[DEBUG] Auto-enabled debug mode from config\n");
        }

        DebugJump::g_retouchSystem = pThis;
    }

    const char* finalPath = path;
    std::string overridePath;

    // Check for pending debug jump
    {
        std::lock_guard<std::mutex> lock(DebugJump::g_mutex);
        if (DebugJump::g_jumpRequested && !DebugJump::g_pendingScene.empty()) {
            // Replace the path with the jump target. The engine always
            // passes liteLoad a decorated path of the form rld\<name>.rld,
            // so build the same form.
            overridePath = "rld\\" + DebugJump::g_pendingScene + ".rld";
            finalPath = overridePath.c_str();

            Log("\n[DEBUG] =======================================\n");
            Log("[DEBUG] SCENE JUMP ACTIVATED!\n");
            Log("[DEBUG]   Original: %s\n", path);
            Log("[DEBUG]   Jump to:  %s\n", finalPath);
            Log("[DEBUG] =======================================\n\n");

            DebugJump::g_jumpRequested = false;
            DebugJump::g_pendingScene.clear();
        }
    }

    // Track with potentially overridden path
    if (finalPath && *finalPath) {
        std::string pathStr(finalPath);
        std::string filename = pathStr;

        size_t lastSlash = filename.find_last_of("\\/");
        if (lastSlash != std::string::npos) {
            filename = filename.substr(lastSlash + 1);
        }

        size_t lastDot = filename.find_last_of(".");
        if (lastDot != std::string::npos) {
            filename = filename.substr(0, lastDot);
        }

        // Map filename to friendly name for Discord RPC
        std::string display;
        if (filename == "title") {
            display = "Main Menu";
        }
        else if (filename == "CgMode") {
            display = "CG Gallery";
        }
        else if (filename == "Replay") {
            display = "Scene Replay";
        }
        else if (filename == "MusicMode") {
            display = "Music Room";
        }
        else if (filename == "ExtraMode") {
            display = "Extras Menu";
        }

        {
            std::lock_guard<std::mutex> lock(g_sceneMutex);
            g_currentFile = filename;
            g_currentLabel.clear();
        }

        UpdateChapterPresence(display);

        Log("[LOAD] %s\n", filename.c_str());
    }

    return g_origLiteLoad(pThis, finalPath, flags);
}

//=============================================================================
// OutputDebugStringA - Redirect to Console
//=============================================================================
typedef void(WINAPI* Fn_OutputDebugStringA)(LPCSTR lpOutputString);
static Fn_OutputDebugStringA g_origOutputDebugStringA = nullptr;

static void WINAPI OutputDebugStringA_Hook(LPCSTR lpOutputString) {
    if (lpOutputString && *lpOutputString) {
        bool isOverflowWarning =
            (strncmp(lpOutputString, "[WARN] ", 7) == 0) ||
            (strstr(lpOutputString, " in:") != nullptr &&
             strchr(lpOutputString, ')') != nullptr);

        if (isOverflowWarning && Config::warnOnOverflow && !DebugJump::g_debugModeActive) {
            std::string scene = CurrentSceneFile();
            if (scene.empty()) scene = "?";
            // The engine emits this warning on every repaint, and Log()
            // flushes on every call, so report each overflow only once.
            if (WordWrap::ClaimOverflowReport("engine:" + scene + ":" +
                                              std::to_string(t_cmdIndex))) {
                // The engine emits SJIS; the log is UTF-8.
                std::string text = Encoding::SjisToUtf8(lpOutputString);
                while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
                    text.pop_back();
                }
                Log("[WRAP] ENGINE OVERFLOW %s:%d  %s\n",
                    scene.c_str(), t_cmdIndex, text.c_str());
            }
        }

        if (DebugJump::g_debugModeActive) {
            Log("[GAME] %s", lpOutputString);
            size_t len = strlen(lpOutputString);
            if (len > 0 && lpOutputString[len - 1] != '\n') {
                Log("\n");
            }
        }
    }

    // Always call original
    g_origOutputDebugStringA(lpOutputString);
}

//=============================================================================
// textOut renderer detection (both are named gdi32 imports)
//=============================================================================
// Detects the start of a textOut renderer call. Both renderers begin by
// measuring the ASCII space and then the fullwidth space (0x8140); those
// are the only two characters they measure but never rasterize. The pair
// occurs once per renderer call, so arming on it resets fragment tracking
// for each pass over the text (including the outline pass).
typedef BOOL(WINAPI* Fn_GetTextExtentPoint32A)(HDC, LPCSTR, int, LPSIZE);
static Fn_GetTextExtentPoint32A g_origGetTextExtentPoint32A = nullptr;

static BOOL WINAPI GetTextExtentPoint32A_Hook(HDC hdc, LPCSTR lpString, int c, LPSIZE psizl) {
    static thread_local bool s_sawSpace = false;
    if (lpString) {
        if (c == 1 && lpString[0] == ' ') {
            s_sawSpace = true;
        }
        else if (c == 2 && s_sawSpace &&
                 (unsigned char)lpString[0] == 0x81 &&
                 (unsigned char)lpString[1] == 0x40) {
            s_sawSpace = false;
            CharRestore::Disarm();
            CharRestore::t_toArmed = true;
        }
        else {
            s_sawSpace = false;
        }
    }
    return g_origGetTextExtentPoint32A(hdc, lpString, c, psizl);
}

// GetOutlineTextMetricsA is called by the two textOut renderers and by the
// glyph loop's metrics helper. A renderer call arrives with tracking armed
// and no fragment base set yet; any other combination is stale state from
// an earlier call, so clear it.
typedef UINT(WINAPI* Fn_GetOutlineTextMetricsA)(HDC, UINT, LPOUTLINETEXTMETRICA);
static Fn_GetOutlineTextMetricsA g_origGetOutlineTextMetricsA = nullptr;

static UINT WINAPI GetOutlineTextMetricsA_Hook(HDC hdc, UINT cjCopy, LPOUTLINETEXTMETRICA potm) {
    if (!(CharRestore::t_toArmed && !CharRestore::t_toBase)) CharRestore::Disarm();
    return g_origGetOutlineTextMetricsA(hdc, cjCopy, potm);
}

//=============================================================================
// Japanese fallback for a Latin-only custom face
//=============================================================================
// Provides Japanese glyphs when the custom font is Latin-only.
//
// With DEFAULT_CHARSET, GDI honors a Latin-only face, but that face has no
// Japanese glyphs, so untranslated lines would render as .notdef boxes.
// This namespace redirects those glyph requests to a Gothic face with the
// same height, weight, and italic setting.
//
// The fallback cache is keyed on those metrics, not on the engine's HFONT:
// the engine creates and deletes fonts freely, and a handle-keyed table
// would dangle unless every DeleteObject call were also hooked.
namespace JpFallback {
    static std::mutex g_mutex;
    static std::unordered_map<uint64_t, HFONT> g_fonts;
    static std::unordered_map<uint64_t, bool>  g_missing;   // (HFONT, wchar) -> absent

    // The proportional/fixed choice cannot be read from lfPitchAndFamily:
    // the engine hardcodes FIXED_PITCH and encodes proportionality in the
    // face name, which ApplyFontSubstitution has already overwritten.
    static HFONT FontFor(const LOGFONTA& lf) {
        const uint64_t key = ((uint64_t)(uint32_t)lf.lfHeight << 32)
                           | ((uint64_t)(uint16_t)lf.lfWeight << 16)
                           | (uint64_t)(lf.lfItalic != 0);

        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_fonts.find(key);
        if (it != g_fonts.end()) return it->second;

        LOGFONTA jp = lf;
        jp.lfCharSet = SHIFTJIS_CHARSET;
        strncpy_s(jp.lfFaceName,
                  Config::fontFallbackJp[0] ? Config::fontFallbackJp : "MS Gothic",
                  _TRUNCATE);

        // Bypass our own hook, which would substitute this straight back.
        HFONT hf = g_origCreateFontIndirectA ? g_origCreateFontIndirectA(&jp)
                                             : CreateFontIndirectA(&jp);
        Log("[FONT] Japanese fallback -> %s (h=%d weight=%d) %s\n",
            jp.lfFaceName, jp.lfHeight, jp.lfWeight, hf ? "ok" : "FAILED");
        g_fonts[key] = hf;
        return hf;
    }

    // The font to swap in for this request, or null to leave the DC alone.
    static HFONT For(HDC hdc, UINT uChar) {
        if (!FontProbe::g_relaxed.load(std::memory_order_relaxed)) return nullptr;
        if (uChar < 0x80) return nullptr;      // ASCII: the custom face has it

        HFONT cur = (HFONT)GetCurrentObject(hdc, OBJ_FONT);
        if (!cur) return nullptr;

        LOGFONTA lf = {};
        if (!GetObjectA(cur, sizeof(lf), &lf)) return nullptr;
        if (lf.lfCharSet == SHIFTJIS_CHARSET) return nullptr;   // already Japanese

        return FontFor(lf);
    }

    // Fallback for a restored character that the custom face does not
    // contain. Without this check, a face missing a single glyph draws a
    // .notdef box with no diagnostic.
    static HFONT ForMissingGlyph(HDC hdc, wchar_t wc) {
        if (!FontProbe::g_relaxed.load(std::memory_order_relaxed)) return nullptr;

        HFONT cur = (HFONT)GetCurrentObject(hdc, OBJ_FONT);
        if (!cur) return nullptr;

        // Cache both outcomes. A missing character recurs on every redraw,
        // and the textOut renderer repeats each fragment for the outline
        // pass; if only one outcome were cached, the other would query GDI
        // once per glyph per pass.
        const uint64_t key = ((uint64_t)(uintptr_t)cur << 32) | (uint64_t)wc;
        bool absent = false;
        bool known  = false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_missing.find(key);
            if (it != g_missing.end()) { absent = it->second; known = true; }
        }

        if (!known) {
            WORD gi = 0;
            if (GetGlyphIndicesW(hdc, &wc, 1, &gi, GGI_MARK_NONEXISTING_GLYPHS) == GDI_ERROR)
                return nullptr;
            absent = (gi == 0xFFFF);

            std::lock_guard<std::mutex> lock(g_mutex);
            g_missing[key] = absent;
        }
        if (!absent) return nullptr;

        LOGFONTA lf = {};
        if (!GetObjectA(cur, sizeof(lf), &lf)) return nullptr;

        static std::mutex warnMutex;
        static std::unordered_set<unsigned> warned;
        {
            std::lock_guard<std::mutex> lock(warnMutex);
            if (warned.insert((unsigned)wc).second)
                Log("[FONT] '%s' has no glyph for U+%04X - drawing it from the"
                    " fallback face\n", lf.lfFaceName, (unsigned)wc);
        }
        return FontFor(lf);
    }

    // g_missing is keyed on HFONT, and GDI reuses handle values, so a
    // recycled handle would inherit the previous face's answers; clear
    // them. g_fonts is keyed on metrics and holds handles we created
    // ourselves, so it is unaffected.
    static void InvalidateForFont(HFONT hf) {
        if (!hf) return;
        const uint64_t prefix = (uint64_t)(uintptr_t)hf << 32;

        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_missing.empty()) return;
        for (auto it = g_missing.begin(); it != g_missing.end(); ) {
            if ((it->first & 0xFFFFFFFF00000000ULL) == prefix) it = g_missing.erase(it);
            else ++it;
        }
    }

    static void Release() {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto& kv : g_fonts) if (kv.second) DeleteObject(kv.second);
        g_fonts.clear();
        g_missing.clear();
    }
}

//=============================================================================
// GetGlyphOutlineA Font Fix
//=============================================================================
typedef DWORD(WINAPI* Fn_GetGlyphOutlineA)(
    HDC hdc, UINT uChar, UINT fuFormat,
    LPGLYPHMETRICS lpgm, DWORD cjBuffer,
    LPVOID pvBuffer, const MAT2* lpmat2
);
static Fn_GetGlyphOutlineA g_origGetGlyphOutlineA = nullptr;

static DWORD WINAPI GetGlyphOutlineA_Hook(
    HDC hdc, UINT uChar, UINT fuFormat,
    LPGLYPHMETRICS lpgm, DWORD cjBuffer,
    LPVOID pvBuffer, const MAT2* lpmat2)
{
    DWORD result;
    // Choose exactly one source for the substitute character; never try one
    // and fall back to the other. During a textOut renderer walk, the
    // UxPrintData cursor still holds the position where the last glyph loop
    // stopped, so consulting it there would substitute the wrong character.
    wchar_t sub = (CharRestore::t_toArmed && CharRestore::t_toBase)
                ? CharRestore::CharAtTextOut(uChar)
                : CharRestore::CharAtCursor();
    if (wchar_t wc = sub) {
        // Use the DC's current font on purpose: restored characters are the
        // Latin ones Shift-JIS dropped, and the custom face should draw
        // them - unless it has no such glyph, in which case use the
        // fallback face.
        if (HFONT fb = JpFallback::ForMissingGlyph(hdc, wc)) {
            HGDIOBJ prev = SelectObject(hdc, fb);
            result = GetGlyphOutlineW(hdc, (UINT)wc, fuFormat, lpgm, cjBuffer, pvBuffer, lpmat2);
            SelectObject(hdc, prev);
        } else {
            result = GetGlyphOutlineW(hdc, (UINT)wc, fuFormat, lpgm, cjBuffer, pvBuffer, lpmat2);
        }
    }
    else if (HFONT jp = JpFallback::For(hdc, uChar)) {
        HGDIOBJ prev = SelectObject(hdc, jp);
        result = g_origGetGlyphOutlineA(hdc, uChar, fuFormat, lpgm, cjBuffer, pvBuffer, lpmat2);
        SelectObject(hdc, prev);
    }
    else {
        result = g_origGetGlyphOutlineA(hdc, uChar, fuFormat, lpgm, cjBuffer, pvBuffer, lpmat2);
    }

    // Fix negative left-side bearings. The engine draws each glyph at the
    // pen position, so a negative gmptGlyphOrigin.x makes the glyph overlap
    // the previous one. Clamp the origin to 0 and add the difference to
    // gmCellIncX so the total advance is unchanged; clamping alone would
    // move the overlap onto the next glyph. Apply this on every call,
    // including measurement-only calls with no buffer (the metrics path at
    // 0x10175390): gating on the buffer made measured widths disagree with
    // rendered widths.
    if (lpgm && result != GDI_ERROR) {
        if (lpgm->gmptGlyphOrigin.x < 0) {
            int offset = -lpgm->gmptGlyphOrigin.x;
            lpgm->gmptGlyphOrigin.x = 0;
            lpgm->gmCellIncX += offset;
        }
    }

    return result;
}

//=============================================================================
// Font Replacement Hook
//=============================================================================
static HFONT WINAPI CreateFontIndirectA_Hook(const LOGFONTA* lf) {
    if (lf) {
        LOGFONTA modified = *lf;

        // Same substitution the word wrapper measures with -- see
        // ApplyFontSubstitution's note on why it must be shared.
        ApplyFontSubstitution(modified);

        if (Config::enableTextLogging) {
            Log("[FONT] %s (h=%d, cs=%d) -> %s (cs=%d)\n",
                Encoding::SjisToUtf8(lf->lfFaceName).c_str(),
                lf->lfHeight, lf->lfCharSet,
                modified.lfFaceName, modified.lfCharSet);
        }

        HFONT created = g_origCreateFontIndirectA(&modified);

        // The glyph cache is keyed by HFONT, and GDI can return a handle
        // value it used before, so drop any cached entries for this handle.
        // Invalidate only this handle: the engine recreates fonts often,
        // and flushing the whole cache each time would make it useless.
        WordWrap::InvalidateGlyphsForFont(created);
        JpFallback::InvalidateForFont(created);

        return created;
    }
    return g_origCreateFontIndirectA(lf);
}

//=============================================================================
// Hook: CreateFileA - Asset Redirection
//=============================================================================
typedef HANDLE(WINAPI* Fn_CreateFileA)(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
    LPSECURITY_ATTRIBUTES lpSecurity, DWORD dwCreation,
    DWORD dwFlags, HANDLE hTemplate
);
static Fn_CreateFileA g_origCreateFileA = nullptr;

static HANDLE WINAPI CreateFileA_Hook(
    LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
    LPSECURITY_ATTRIBUTES lpSecurity, DWORD dwCreation,
    DWORD dwFlags, HANDLE hTemplate)
{
    if (lpFileName && (dwDesiredAccess & GENERIC_READ)) {
        const char* ext = strrchr(lpFileName, '.');
        if (ext && _stricmp(ext, ".gyu") == 0) {
            std::string replacement = AssetRedirect::FindReplacement(lpFileName);
            if (!replacement.empty()) {
                if (Config::logAssetRedirects) {
                    Log("[ASSET] %s -> %s\n", lpFileName, replacement.c_str());
                }
                return g_origCreateFileA(
                    replacement.c_str(), dwDesiredAccess, dwShareMode,
                    lpSecurity, dwCreation, dwFlags, hTemplate
                );
            }
        }
    }

    return g_origCreateFileA(
        lpFileName, dwDesiredAccess, dwShareMode,
        lpSecurity, dwCreation, dwFlags, hTemplate
    );
}

//=============================================================================
// File Watcher
//=============================================================================
class FileWatcher {
public:
    struct Spec {
        std::string directory;
        bool anyTsv = false;
        std::vector<std::string> exactNames;
        std::vector<std::string> ignoreNames;
    };

    bool Start(const Spec& spec, std::function<void()> onChange) {
        m_spec = spec;
        m_onChange = onChange;
        m_running = true;

        // Create the stop event on this thread, not the watcher thread. If
        // Stop() ran before the watcher thread created the event, SetEvent
        // would have nothing to signal and the watch would wait forever.
        m_stopEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        if (!m_stopEvent) {
            Log("[FileWatcher] CreateEvent failed for %s (error %lu)\n",
                m_spec.directory.c_str(), GetLastError());
            m_running = false;
            return false;
        }

        m_thread = CreateThread(nullptr, 0, WatchThreadProc, this, 0, nullptr);
        if (!m_thread) {
            Log("[FileWatcher] CreateThread failed for %s (error %lu)\n",
                m_spec.directory.c_str(), GetLastError());
            CloseHandle(m_stopEvent);
            m_stopEvent = nullptr;
            m_running = false;
            return false;
        }
        return true;
    }

    // Returns false if the thread did not exit within the timeout. The
    // thread may then still be inside a reload, reading m_spec and
    // m_onChange, so the caller must leak this object rather than destroy
    // it.
    bool Stop() {
        m_running = false;
        if (m_stopEvent) SetEvent(m_stopEvent);

        if (m_thread) {
            if (WaitForSingleObject(m_thread, Constants::kWatcherStopTimeoutMs) != WAIT_OBJECT_0) {
                return false;
            }
            CloseHandle(m_thread);
            m_thread = nullptr;
        }
        if (m_stopEvent) {
            CloseHandle(m_stopEvent);
            m_stopEvent = nullptr;
        }
        return true;
    }

private:
    static DWORD WINAPI WatchThreadProc(LPVOID param) {
        return ((FileWatcher*)param)->WatchThread();
    }

public:
    static bool HasTsvExtension(const char* path) {
        size_t n = strlen(path);
        return n >= 4 && _stricmp(path + n - 4, ".tsv") == 0;
    }

private:

    static const char* BaseName(const char* path) {
        const char* slash = strrchr(path, '\\');
        return slash ? slash + 1 : path;
    }

    struct Fingerprint {
        unsigned long long count = 0;
        unsigned long long hash = 1469598103934665603ULL;

        bool operator!=(const Fingerprint& o) const {
            return count != o.count || hash != o.hash;
        }
    };

    static void Fold(Fingerprint& fp, const char* name,
                     const FILETIME& ft, DWORD sizeHigh, DWORD sizeLow) {
        ULARGE_INTEGER t;
        t.LowPart = ft.dwLowDateTime;
        t.HighPart = ft.dwHighDateTime;

        unsigned long long entry = t.QuadPart ^
                                   (((unsigned long long)sizeHigh << 32) | sizeLow);
        // Do not use tolower(): it is undefined behavior for bytes outside
        // 0..127 (such as an SJIS lead byte) and depends on the C locale.
        // Fold ASCII case manually.
        for (const char* p = name; *p; ++p) {
            unsigned char c = (unsigned char)*p;
            if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
            entry = (entry ^ c) * 1099511628211ULL;
        }
        fp.hash += entry;   // Sum, so the result does not depend on
                            // directory enumeration order.
        fp.count++;
    }

    Fingerprint Snapshot() const {
        Fingerprint fp;

        if (m_spec.anyTsv) {
            WIN32_FIND_DATAA fd;
            std::string pattern = m_spec.directory + "\\*.tsv";
            HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                    if (Ignored(fd.cFileName)) continue;
                    Fold(fp, fd.cFileName, fd.ftLastWriteTime,
                         fd.nFileSizeHigh, fd.nFileSizeLow);
                } while (FindNextFileA(h, &fd));
                FindClose(h);
            }
        }

        for (const std::string& name : m_spec.exactNames) {
            if (Ignored(name.c_str())) continue;
            WIN32_FILE_ATTRIBUTE_DATA data;
            std::string full = m_spec.directory + "\\" + name;
            if (GetFileAttributesExA(full.c_str(), GetFileExInfoStandard, &data)) {
                Fold(fp, name.c_str(), data.ftLastWriteTime,
                     data.nFileSizeHigh, data.nFileSizeLow);
            }
        }
        return fp;
    }

    bool Ignored(const char* name) const {
        for (const std::string& ig : m_spec.ignoreNames) {
            if (_stricmp(name, ig.c_str()) == 0) return true;
        }
        return false;
    }

    bool Matches(const char* relPath) const {
        if (Ignored(BaseName(relPath))) return false;
        if (m_spec.anyTsv && HasTsvExtension(relPath)) return true;
        for (const std::string& name : m_spec.exactNames) {
            if (_stricmp(relPath, name.c_str()) == 0) return true;
        }
        return false;
    }

    DWORD WatchThread() {
        HANDLE hDir = CreateFileA(
            m_spec.directory.c_str(),
            FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
            nullptr
        );

        if (hDir == INVALID_HANDLE_VALUE) {
            Log("[FileWatcher] Failed to open directory: %s\n", m_spec.directory.c_str());
            return 1;
        }

        Log("[FileWatcher] Watching %s\n", m_spec.directory.c_str());
        if (m_spec.anyTsv) Log("[FileWatcher]   - *.tsv\n");
        for (const std::string& f : m_spec.exactNames) {
            Log("[FileWatcher]   - %s\n", f.c_str());
        }

        alignas(DWORD) BYTE buffer[16384];
        OVERLAPPED overlapped = {};
        overlapped.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        if (!overlapped.hEvent) {
            Log("[FileWatcher] CreateEvent failed for %s (error %lu)"
                " - no longer watching\n", m_spec.directory.c_str(), GetLastError());
            CloseHandle(hDir);
            return 1;
        }

        HANDLE waitHandles[2] = { overlapped.hEvent, m_stopEvent };

        const DWORD kFilter = FILE_NOTIFY_CHANGE_LAST_WRITE |
                              FILE_NOTIFY_CHANGE_SIZE |
                              FILE_NOTIFY_CHANGE_FILE_NAME;

        bool armed = false;
        bool pending = false;
        ULONGLONG deadline = 0;
        int namesLogged = 0;
        constexpr int kMaxNamesPerWindow = 5;

        // Only a change to a watched file may move the deadline. If every
        // notification rescheduled the debounce, unrelated activity in the
        // directory could postpone a pending reload indefinitely.
        auto schedule = [&]() {
            pending = true;
            deadline = GetTickCount64() + Constants::kFileWatcherDebounceMs;
        };

        Fingerprint seen = Snapshot();

        bool saidSuppressed = false;

        // Overflow handling must not reload unconditionally: hook.log lives
        // in tl\ and is flushed on every line, so a reload that logs would
        // generate more changes and re-trigger this watcher in a loop.
        // Compare fingerprints and reload only if a watched file actually
        // changed.
        auto scheduleIfChanged = [&]() {
            Fingerprint now = Snapshot();
            if (now != seen) {
                seen = now;
                Log("[FileWatcher] change buffer overflowed on %s"
                    " - watched files differ, reloading\n", m_spec.directory.c_str());
                schedule();
            } else if (!saidSuppressed) {
                saidSuppressed = true;
                Log("[FileWatcher] change buffer overflowed on %s from unrelated"
                    " activity - no watched file changed (further ones silent)\n",
                    m_spec.directory.c_str());
            }
        };

        while (m_running) {
            if (!armed) {
                DWORD ignored = 0;
                ResetEvent(overlapped.hEvent);
                if (!ReadDirectoryChangesW(hDir, buffer, sizeof(buffer),
                                           FALSE, kFilter,
                                           &ignored, &overlapped, nullptr)) {
                    DWORD err = GetLastError();
                    if (err != ERROR_IO_PENDING) {
                        Log("[FileWatcher] ReadDirectoryChangesW failed on %s (error %lu)"
                            " - no longer watching\n", m_spec.directory.c_str(), err);
                        break;
                    }
                }
                armed = true;
            }

            // Read the clock once. With two reads, the deadline could pass
            // between them, and (deadline - now) would underflow to roughly
            // 49 days, or exactly INFINITE.
            ULONGLONG now = GetTickCount64();

            // Fire while the ReadDirectoryChangesW request is still armed,
            // so edits made during the reload accumulate in the kernel's
            // buffer instead of being lost.
            if (pending && now >= deadline) {
                pending = false;
                namesLogged = 0;
                if (!m_running) break;
                seen = Snapshot();
                if (m_onChange) m_onChange();
                continue;
            }

            DWORD timeout = INFINITE;
            if (pending) {
                timeout = (DWORD)std::min<ULONGLONG>(deadline - now, MAXDWORD - 1);
            }

            DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, timeout);

            if (waitResult == WAIT_OBJECT_0) {
                armed = false;
                DWORD bytesReturned = 0;

                if (!GetOverlappedResult(hDir, &overlapped, &bytesReturned, FALSE)) {
                    DWORD err = GetLastError();
                    if (err == ERROR_OPERATION_ABORTED) break;
                    if (err == ERROR_NOTIFY_ENUM_DIR) {
                        scheduleIfChanged();
                        continue;
                    }
                    Log("[FileWatcher] GetOverlappedResult failed on %s (error %lu)"
                        " - no longer watching\n", m_spec.directory.c_str(), err);
                    break;
                }

                if (bytesReturned == 0) {
                    scheduleIfChanged();
                    continue;
                }

                FILE_NOTIFY_INFORMATION* info = (FILE_NOTIFY_INFORMATION*)buffer;
                bool matched = false;
                std::string changed;
                while (true) {
                    std::wstring changedW(info->FileName, info->FileNameLength / sizeof(WCHAR));
                    // Size the buffer from the conversion result, not
                    // MAX_PATH: a name whose CP932 form is longer would
                    // otherwise be dropped silently.
                    int need = WideCharToMultiByte(CP_ACP, 0, changedW.c_str(), -1,
                                                   nullptr, 0, nullptr, nullptr);
                    changed.assign(need > 0 ? (size_t)need : 0, '\0');
                    bool converted = need > 0 &&
                        WideCharToMultiByte(CP_ACP, 0, changedW.c_str(), -1,
                                            &changed[0], need, nullptr, nullptr) > 0;
                    // need counts the terminator; drop it so size() is the name.
                    if (converted && !changed.empty() && changed.back() == '\0') {
                        changed.pop_back();
                    }
                    if (converted && Matches(changed.c_str())) {
                        if (namesLogged < kMaxNamesPerWindow) {
                            Log("[FileWatcher] %s changed\n", changed.c_str());
                            namesLogged++;
                        }
                        matched = true;
                        schedule();
                    }

                    if (info->NextEntryOffset == 0) break;
                    info = (FILE_NOTIFY_INFORMATION*)((char*)info + info->NextEntryOffset);
                }

                // Refresh the fingerprint now. Otherwise an overflow
                // arriving before the deadline would compare against the
                // pre-edit fingerprint, treat this same edit as new, and
                // push the reload out again.
                if (matched) seen = Snapshot();
            } else if (waitResult == WAIT_FAILED) {
                Log("[FileWatcher] WaitForMultipleObjects failed on %s (error %lu)"
                    " - no longer watching\n", m_spec.directory.c_str(), GetLastError());
                break;
            } else if (waitResult != WAIT_TIMEOUT) {
                break;   // stop event
            }
        }

        if (armed) {
            CancelIo(hDir);
            DWORD discarded = 0;
            GetOverlappedResult(hDir, &overlapped, &discarded, TRUE);
        }

        CloseHandle(overlapped.hEvent);
        CloseHandle(hDir);
        return 0;
    }

    Spec m_spec;
    std::function<void()> m_onChange;
    std::atomic<bool> m_running{false};
    HANDLE m_thread = nullptr;
    HANDLE m_stopEvent = nullptr;
};

// char_table.tsv is intentionally not watched: g_charIdToName has no mutex
// and is read from the render path, so reloading it at runtime would be a
// data race.
static std::vector<std::unique_ptr<FileWatcher>> g_fileWatchers;

static std::string DirNameOf(const std::string& path) {
    size_t slash = path.find_last_of("\\/");
    if (slash == std::string::npos) return ".";
    // For "C:\file", return "C:\", not "C:". To Win32 those are different
    // places: "C:\" is the root, "C:" is the drive's current directory.
    if (slash == 2 && path.size() >= 3 && path[1] == ':') return path.substr(0, 3);
    if (slash == 0) return path.substr(0, 1);
    return path.substr(0, slash);
}

static std::string BaseNameOf(const std::string& path) {
    size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

static std::string FullPathOf(const std::string& path) {
    char buf[MAX_PATH];
    DWORD n = GetFullPathNameA(path.c_str(), MAX_PATH, buf, nullptr);
    std::string out = (n > 0 && n < MAX_PATH) ? std::string(buf) : path;

    auto isRoot = [](const std::string& s) {
        return s.size() <= 3 && s.size() >= 2 && s[1] == ':';
    };
    while (out.size() > 1 && !isRoot(out) &&
           (out.back() == '\\' || out.back() == '/')) {
        out.pop_back();
    }
    return out;
}

static size_t StartTranslationWatchers(std::function<void()> onChange) {
    std::vector<FileWatcher::Spec> specs;

    // Exclusions match directory and file name together, not name alone: a
    // file with the same name in a different directory must stay watched.
    // char_table.tsv is listed for the case ScriptsDir=.\tl, where the
    // *.tsv wildcard would otherwise match it.
    struct Exclusion { std::string dir, base; };
    Exclusion exclusions[2];
    {
        std::string a = FullPathOf(Config::untranslatedLog);
        exclusions[0] = { DirNameOf(a), BaseNameOf(a) };
        std::string b = FullPathOf(Config::charIdFile);
        exclusions[1] = { DirNameOf(b), BaseNameOf(b) };
    }

    auto findSpec = [&specs](const std::string& dir) -> FileWatcher::Spec* {
        for (FileWatcher::Spec& s : specs) {
            if (_stricmp(s.directory.c_str(), dir.c_str()) == 0) return &s;
        }
        return nullptr;
    };

    auto addSpec = [&](FileWatcher::Spec s) {
        for (const Exclusion& ex : exclusions) {
            if (_stricmp(s.directory.c_str(), ex.dir.c_str()) == 0) {
                s.ignoreNames.push_back(ex.base);
            }
        }
        specs.push_back(std::move(s));
    };

    // Watch a single directory, never a subtree: TranslationDB's
    // ListTsvFiles scans one level only, so a change in a nested TSV would
    // trigger a reload that cannot see the file.
    if (Config::scriptsDir[0]) {
        FileWatcher::Spec s;
        s.directory = FullPathOf(Config::scriptsDir);
        s.anyTsv = true;
        addSpec(std::move(s));
    }

    for (const char* file : { Config::translationFile, Config::namesFile, Config::uiFile }) {
        if (!file[0]) continue;
        std::string full = FullPathOf(file);
        std::string dir = DirNameOf(full);
        std::string base = BaseNameOf(full);

        if (FileWatcher::Spec* existing = findSpec(dir)) {
            if (!(existing->anyTsv && FileWatcher::HasTsvExtension(base.c_str()))) {
                existing->exactNames.push_back(base);
            }
            continue;
        }
        FileWatcher::Spec s;
        s.directory = dir;
        s.exactNames.push_back(base);
        addSpec(std::move(s));
    }

    for (const FileWatcher::Spec& s : specs) {
        auto watcher = std::make_unique<FileWatcher>();
        if (watcher->Start(s, onChange)) {
            g_fileWatchers.push_back(std::move(watcher));
        }
    }
    return g_fileWatchers.size();
}

static void StopTranslationWatchers() {
    for (auto& watcher : g_fileWatchers) {
        if (!watcher->Stop()) {
            Log("[FileWatcher] thread did not stop in time - leaking the watcher\n");
            watcher.release();
        }
    }
    g_fileWatchers.clear();
}

//=============================================================================
// Locale Independence Hooks
//=============================================================================

static void ForceCRTJapaneseCodepage() {
    // Try to call _setmbcp(932) in all loaded CRTs
    const wchar_t* runtimes[] = {
        L"msvcr80.dll", L"msvcrt.dll", L"ucrtbase.dll"
    };

    typedef int(__cdecl* Fn_setmbcp)(int);

    for (const wchar_t* rt : runtimes) {
        HMODULE hMod = GetModuleHandleW(rt);
        if (hMod) {
            Fn_setmbcp pSetmbcp = (Fn_setmbcp)GetProcAddress(hMod, "_setmbcp");
            if (pSetmbcp) {
                int result = pSetmbcp(932);
                Log("[MBCS] Called _setmbcp(932) in %S -> %d\n", rt, result);
            }
        }
    }
}

// Character Navigation
typedef LPSTR(WINAPI* Fn_CharPrevA)(LPCSTR lpszStart, LPCSTR lpszCurrent);
static Fn_CharPrevA g_origCharPrevA = nullptr;

static LPSTR WINAPI CharPrevA_Hook(LPCSTR lpszStart, LPCSTR lpszCurrent) {
    if (lpszCurrent <= lpszStart)
        return (LPSTR)lpszStart;

    LPCSTR p = lpszCurrent - 1;

    // Check if we're in the middle of a 2-byte SJIS char
    if (p > lpszStart) {
        unsigned char prev = *(unsigned char*)(p - 1);
        unsigned char curr = *(unsigned char*)p;

        // If prev is SJIS lead byte and curr is valid trail byte
        if (((prev >= 0x81 && prev <= 0x9F) || (prev >= 0xE0 && prev <= 0xFC)) &&
            ((curr >= 0x40 && curr <= 0x7E) || (curr >= 0x80 && curr <= 0xFC))) {
            return (LPSTR)(p - 1);
        }
    }

    return (LPSTR)p;
}

typedef LPSTR(WINAPI* Fn_CharNextA)(LPCSTR lpsz);
static Fn_CharNextA g_origCharNextA = nullptr;

static LPSTR WINAPI CharNextA_Hook(LPCSTR lpsz) {
    // The textOut renderers step their fragment with this call and rasterize
    // before advancing, so this argument is the glyph GetGlyphOutlineA is
    // about to be asked for. The first call after the renderer prologue
    // establishes the fragment base.
    if (CharRestore::t_toArmed && lpsz) {
        CharRestore::t_toCur = lpsz;
        if (!CharRestore::t_toBase) CharRestore::FragBegin(lpsz);
    }

    if (!lpsz || !*lpsz)
        return (LPSTR)lpsz;

    unsigned char c = *(unsigned char*)lpsz;

    // SJIS lead byte - skip 2 bytes
    if ((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC)) {
        if (lpsz[1])
            return (LPSTR)(lpsz + 2);
    }

    return (LPSTR)(lpsz + 1);
}

// System Codepage
typedef UINT(WINAPI* Fn_GetACP)();
static Fn_GetACP g_origGetACP = nullptr;

static UINT WINAPI GetACP_Hook() {
    return 932;   // Japanese Shift-JIS
}

typedef UINT(WINAPI* Fn_GetOEMCP)();
static Fn_GetOEMCP g_origGetOEMCP = nullptr;

static UINT WINAPI GetOEMCP_Hook() {
    return 932;
}

//=============================================================================
// Debug Console Commands
//=============================================================================
static void ProcessDebugCommand(const std::string& cmd) {
    std::istringstream iss(cmd);
    std::string verb;
    iss >> verb;

    if (verb == "help") {
        Log("\n=== Debug Commands ===\n");
        Log("  debug on/off - Toggle game debug mode\n");
        Log("  stats        - Show translation stats\n");
        Log("  reload       - Reload translations\n");
        Log("  scene        - Show current scene\n");
        Log("  find <text>  - Search for text in DB\n");
        Log("  log on/off   - Toggle logging\n");
        Log("  goto <file>  - Jump to scene\n");
        Log("  list         - List common scenes\n");
        Log("========================\n\n");
    } else if (verb == "debug") {
        std::string state;
        iss >> state;

        std::lock_guard<std::mutex> lock(DebugJump::g_mutex);

        if (!DebugJump::g_retouchSystem) {
            Log("[DEBUG] RetouchSystem not captured yet. Start game first!\n");
            return;
        }

        if (!g_liteSetDebugMode) {
            Log("[DEBUG] liteSetDebugMode not available\n");
            return;
        }

        if (state == "on") {
            g_liteSetDebugMode(DebugJump::g_retouchSystem, 0x10001);
            DebugJump::g_debugModeActive = true;  // Enable console output
            Log("[DEBUG] Debug mode ENABLED (0x10001)\n");
        } else if (state == "off") {
            g_liteSetDebugMode(DebugJump::g_retouchSystem, 0);
            DebugJump::g_debugModeActive = false;  // Disable console output
            Log("[DEBUG] Debug mode DISABLED\n");
        } else {
            // Show current state
            DWORD* pSystem = (DWORD*)DebugJump::g_retouchSystem;
            DWORD currentFlags = pSystem[0x112C / 4];
            Log("[DEBUG] Current debug flags: 0x%08X\n", currentFlags);
            Log("[DEBUG] Console output: %s\n", DebugJump::g_debugModeActive ? "ON" : "OFF");
            Log("[DEBUG] Usage: debug on | debug off\n");
        }
    } else if (verb == "stats") {
        g_translationDB.PrintStats();
    }
    else if (verb == "reload") {
        g_translationDB.Reload();
        Log("[*] Reloaded!\n");
    }
    else if (verb == "scene") {
        std::lock_guard<std::mutex> lock(g_sceneMutex);
        Log("[SCENE] File: %s\n", g_currentFile.c_str());
        Log("[SCENE] Label: %s\n", g_currentLabel.c_str());

        std::lock_guard<std::mutex> lock2(DebugJump::g_mutex);
        Log("[SCENE] RetouchSystem: %p\n", DebugJump::g_retouchSystem);
    }
    else if (verb == "find") {
        std::string searchText;
        std::getline(iss >> std::ws, searchText);
        if (!searchText.empty()) {
            g_translationDB.FindInDB(searchText);
        }
    }
    else if (verb == "log") {
        std::string state;
        iss >> state;
        Config::enableTextLogging = (state == "on");
        Log("[*] Logging: %s\n", Config::enableTextLogging ? "ON" : "OFF");
    }
    else if (verb == "goto") {
        std::string sceneName;
        iss >> sceneName;

        if (sceneName.empty()) {
            Log("\n[DEBUG] Usage: goto <sceneName>\n");
            Log("[DEBUG] Examples:\n");
            Log("[DEBUG]   goto y0011001       - Start of prologue\n");
            Log("[DEBUG]   goto y1034001       - Chapter 1, Day 3-4\n");
            Log("[DEBUG]\n");
            Log("[DEBUG] Jump happens on next scene transition.\n");
            Log("[DEBUG] Advance the game or return to title to trigger.\n\n");
        } else {
            std::lock_guard<std::mutex> lock(DebugJump::g_mutex);
            DebugJump::g_pendingScene = sceneName;
            DebugJump::g_jumpRequested = true;

            Log("\n[DEBUG] =======================================\n");
            Log("[DEBUG] Jump queued: %s\n", sceneName.c_str());
            Log("[DEBUG] Advance game or use title menu to trigger.\n");
            Log("[DEBUG] =======================================\n\n");
        }
    }

    else if (verb == "list") {
        Log("\n=== Scene List ===\n");
        Log("  Prologue:\n");
        Log("    y0011001 - y0017001\n");
        Log("    y0021001 - y0024001\n");
        Log("  Chapter 1 (Day 1-4):\n");
        Log("    y1011001 - y1015001 (Day 1)\n");
        Log("    y1021001 - y1025001 (Day 2)\n");
        Log("    y1031001 - y1036001 (Day 3)\n");
        Log("    y1041001 - y1043001 (Day 4)\n");
        Log("  Chapters 2-9: y2XXXXXX - y9XXXXXX\n");
        Log("  Chapter 10: yAXXXXXX\n");
        Log("  Endings: yEA11001, yEB11001, yEC11001, yED11001\n");
        Log("  H-Scenes: yHR0_001 - yHR0_016\n");
        Log("  Extras: yotuiro_omake\n");
        Log("==================\n\n");
    }
    else if (!verb.empty()) {
        Log("[?] Unknown command: %s (type 'help')\n", verb.c_str());
    }
}

static DWORD WINAPI ConsoleInputThread(LPVOID) {
    Log("[*] Debug console ready. Type 'help' for commands.\n\n");

    char buffer[256];
    while (g_running) {
        if (fgets(buffer, sizeof(buffer), stdin)) {
            std::string cmd(buffer);
            // Trim newline
            while (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r')) {
                cmd.pop_back();
            }
            if (!cmd.empty()) {
                ProcessDebugCommand(cmd);
            }
        }
    }
    return 0;
}

//=============================================================================
// Hook Installation
//=============================================================================
typedef HMODULE(WINAPI* Fn_LoadLibraryExA)(LPCSTR, HANDLE, DWORD);
static Fn_LoadLibraryExA g_origLoadLibraryExA = nullptr;

static bool InstallHooks(HMODULE hResident) {
    uintptr_t base = (uintptr_t)hResident;
    Log("[*] resident.dll base: 0x%p\n", (void*)base);

    ForceCRTJapaneseCodepage();

    // Hook calcIniValue - bypass INI checksum protection
    {
        uintptr_t addr = base + Offsets::CalcIniValue;
        Log("[*] Hooking calcIniValue at 0x%p\n", (void*)addr);
        if (MH_CreateHook((void*)addr, (void*)&CalcIniValue_Hook, (void**)&g_origCalcIniValue) == MH_OK) {
            MH_EnableHook((void*)addr);
            Log("[+] calcIniValue hooked - INI protection bypassed!\n");
        }
    }

    // Hook RetouchAdvCharacter::say()
    {
        uintptr_t addr = base + Offsets::AdvCharSay;
        Log("[*] Trying to hook RetouchAdvCharacter::say() at 0x%p\n", (void*)addr);
        if (MH_CreateHook((void*)addr, (void*)&AdvCharSay_Hook, (void**)&g_origAdvCharSay) == MH_OK) {
            MH_EnableHook((void*)addr);
            Log("[+] RetouchAdvCharacter::say() hooked\n");
        }
    }

    // Hook RetouchSystem::cmdMessage() -- captures the command index into
    // t_cmdIndex so AdvCharSay_Hook can key its lookup on (file, index)
    // instead of on the message text. Must be installed for position-keyed
    // translation to work; without it every lookup falls back to text-keyed.
    {
        uintptr_t addr = base + Offsets::CmdMessage;
        Log("[*] Trying to hook RetouchSystem::cmdMessage() at 0x%p\n", (void*)addr);
        if (MH_CreateHook((void*)addr, (void*)&CmdMessage_Hook, (void**)&g_origCmdMessage) == MH_OK) {
            MH_EnableHook((void*)addr);
            Log("[+] RetouchSystem::cmdMessage() hooked - position-keyed lookup active\n");
        }
        else {
            Log("[!] cmdMessage hook FAILED - falling back to text-keyed lookup\n");
        }
    }

    // Hook printSub -- supplies the print cursor for message text.
    {
        uintptr_t addr = base + Offsets::PrintSub;
        Log("[*] Trying to hook RetouchPrintManager::printSub() at 0x%p\n", (void*)addr);
        if (MH_CreateHook((void*)addr, (void*)&PrintSub_Hook,
                          (void**)&g_origPrintSub) == MH_OK) {
            MH_EnableHook((void*)addr);
            Log("[+] Glyph loop entry hooked\n");
        }
        else {
            Log("[!] printSub hook FAILED - restored characters will not render\n");
        }
    }

    // Hook RetouchPrintManager::printEx()
    {
        uintptr_t addr = base + Offsets::PrintEx;
        Log("[*] Trying to hook RetouchPrintManager::printEx() at 0x%p\n", (void*)addr);
        if (MH_CreateHook((void*)addr, (void*)&PrintEx_Hook, (void**)&g_origPrintEx) == MH_OK) {
            MH_EnableHook((void*)addr);
            Log("[+] RetouchPrintManager::printEx() hooked\n");
        }
    }

    // Hook RetouchPrintManager::drawHistory() -- marks backlog repaints so they
    // do not inherit the live line's command index. Without it, rewinding shows
    // the current message for every entry.
    {
        uintptr_t addr = base + Offsets::DrawHistory;
        Log("[*] Trying to hook RetouchPrintManager::drawHistory() at 0x%p\n", (void*)addr);
        if (MH_CreateHook((void*)addr, (void*)&DrawHistory_Hook, (void**)&g_origDrawHistory) == MH_OK) {
            MH_EnableHook((void*)addr);
            Log("[+] RetouchPrintManager::drawHistory() hooked - backlog isolated\n");
        }
        else {
            Log("[!] drawHistory hook FAILED - backlog may repeat the current line\n");
        }
    }

    // Hook SaveDataTitle() for LABEL translation
    g_SaveDataIsValid = (Fn_SaveDataIsValid)(base + Offsets::SaveDataIsValid);
    g_SaveDataGetItem = (Fn_SaveDataGetItem)(base + Offsets::SaveDataGetItem);
    {
        uintptr_t addr = base + Offsets::SaveDataTitle;
        Log("[*] Trying to hook SaveDataTitle() at 0x%p\n", (void*)addr);
        if (MH_CreateHook((void*)addr, (void*)&SaveDataTitle_Hook, (void**)&g_origSaveDataTitle) == MH_OK) {
            MH_EnableHook((void*)addr);
            Log("[+] SaveDataTitle() hooked\n");
        }
    }

    // Hook RetouchSystem::prepareQuestion() for CHOICE translation
    {
        uintptr_t addr = base + Offsets::PrepareQuestion;
        Log("[*] Trying to hook RetouchSystem::prepareQuestion() at 0x%p\n", (void*)addr);
        if (MH_CreateHook((void*)addr, (void*)&PrepareQuestion_Hook, (void**)&g_origPrepareQuestion) == MH_OK) {
            MH_EnableHook((void*)addr);
            Log("[+] RetouchSystem::prepareQuestion() hooked\n");
        }
    }

    // Hook RetouchSystem::liteLoad() for scene tracking
    {
        uintptr_t addr = base + Offsets::LiteLoad;
        Log("[*] Trying to hook RetouchSystem::liteLoad() at 0x%p\n", (void*)addr);
        if (MH_CreateHook((void*)addr, (void*)&LiteLoad_Hook, (void**)&g_origLiteLoad) == MH_OK) {
            MH_EnableHook((void*)addr);
            Log("[+] RetouchSystem::liteLoad() hooked - scene tracking active\n");
        }
    }

    // Get liteSetDebugMode function pointer
    g_liteSetDebugMode = (Fn_LiteSetDebugMode)(base + Offsets::LiteSetDebugMode);
    Log("[+] liteSetDebugMode at 0x%p\n", (void*)g_liteSetDebugMode);

    Log("\n========================================\n");
    Log("Translation Hook Active!\n");
    Log("[*] Hotkeys: 0x%02X=Reload, 0x%02X=Stats, 0x%02X=Toggle Logging\n",
        Config::reloadHotkey, Config::statsHotkey, Config::logToggleHotkey);
    Log("========================================\n\n");

    return true;
}

//=============================================================================
// DialogBoxParamA Hook - Translate resource-based dialogs
//=============================================================================
typedef INT_PTR(WINAPI* Fn_DialogBoxParamA)(HINSTANCE, LPCSTR, HWND, DLGPROC, LPARAM);
static Fn_DialogBoxParamA g_origDialogBoxParamA = nullptr;

// Callback to translate each child control in dialog
static BOOL CALLBACK TranslateDialogChildProc(HWND hwnd, LPARAM lParam) {
    wchar_t wideText[256];
    if (GetWindowTextW(hwnd, wideText, 256) > 0 && wideText[0]) {
        // Convert Unicode to SJIS for lookup (dialog resources are SJIS-encoded)
        char sjisText[512];
        if (WideCharToMultiByte(932, 0, wideText, -1, sjisText, sizeof(sjisText),
                                nullptr, nullptr) == 0) {
            return TRUE;  // didn't fit or unconvertible -- leave this control alone
        }

        // Try to find translation
        std::string translation = g_translationDB.FindUITranslation(sjisText);
        if (!translation.empty()) {
            // Convert UTF-8 translation to Unicode and set
            wchar_t translatedText[256];
            if (MultiByteToWideChar(CP_UTF8, 0, translation.c_str(), -1,
                                    translatedText, 256) > 0) {
                SetWindowTextW(hwnd, translatedText);
            }
        }
    }
    return TRUE; // Continue enumeration
}

static INT_PTR WINAPI DialogBoxParamA_Hook(
    HINSTANCE hInstance, LPCSTR lpTemplateName, HWND hWndParent,
    DLGPROC lpDialogFunc, LPARAM dwInitParam)
{
    // Install a temporary CBT hook to translate the dialog. These are
    // thread_local because the WH_CBT hook is per-thread (installed with
    // GetCurrentThreadId) and two threads must not share this state; the lambda
    // stays captureless so it still converts to a HOOKPROC.
    thread_local HHOOK s_cbtHook = nullptr;
    thread_local std::string s_pendingTitle;
    thread_local bool s_seenFirstCreate = false;

    auto cbtProc = [](int nCode, WPARAM wParam, LPARAM lParam) -> LRESULT {
        if (nCode == HCBT_CREATEWND) {
            if (!s_seenFirstCreate) {
                s_seenFirstCreate = true;
                CBT_CREATEWND* pCreate = (CBT_CREATEWND*)lParam;
                if (pCreate && pCreate->lpcs && pCreate->lpcs->lpszName) {
                    // lpszName is actually Unicode (wchar_t*) even for ANSI API calls
                    const wchar_t* wideName = (const wchar_t*)pCreate->lpcs->lpszName;
                    
                    // Only process if it looks like a string (not a resource ID)
                    if ((uintptr_t)wideName > 0xFFFF && wideName[0]) {
                        // Convert Unicode to SJIS for lookup
                        char sjisName[512];
                        if (WideCharToMultiByte(932, 0, wideName, -1, sjisName,
                                                sizeof(sjisName), nullptr, nullptr) > 0) {
                            std::string translation = g_translationDB.FindUITranslation(sjisName);
                            if (!translation.empty()) {
                                s_pendingTitle = translation;
                            }
                        }
                    }
                }
            }
        }
        else if (nCode == HCBT_ACTIVATE) {
            HWND hwnd = (HWND)wParam;
            
            // Apply pending title translation if we found one
            if (!s_pendingTitle.empty()) {
                wchar_t wideTitle[256];
                MultiByteToWideChar(CP_UTF8, 0, s_pendingTitle.c_str(), -1, wideTitle, 256);
                SetWindowTextW(hwnd, wideTitle);
                s_pendingTitle.clear();
            }
            
            // Translate all child controls
            EnumChildWindows(hwnd, TranslateDialogChildProc, 0);
        }
        return CallNextHookEx(s_cbtHook, nCode, wParam, lParam);
    };
    
    s_pendingTitle.clear();
    s_seenFirstCreate = false;
    s_cbtHook = SetWindowsHookExW(WH_CBT, cbtProc, nullptr, GetCurrentThreadId());
    
    INT_PTR result = g_origDialogBoxParamA(hInstance, lpTemplateName, hWndParent, lpDialogFunc, dwInitParam);
    
    if (s_cbtHook) {
        UnhookWindowsHookEx(s_cbtHook);
        s_cbtHook = nullptr;
    }
    
    return result;
}

// Converts UI text to UTF-16, translating when possible. On a translation
// hit the source is the TSV's UTF-8; on a miss it is the original CP932
// text. Returns false when there is no text to show.
static bool UiTextToWide(LPCSTR sjis, std::wstring& out, bool* translated = nullptr) {
    if (translated) *translated = false;
    if (!sjis || !*sjis) return false;

    std::string tl = g_translationDB.FindUITranslation(sjis);
    if (translated) *translated = !tl.empty();
    UINT cp         = tl.empty() ? 932u : CP_UTF8;
    const char* src = tl.empty() ? sjis : tl.c_str();

    // Explicit code pages, so MultiByteToWideChar_Hook passes them through
    // (it rewrites only the locale-relative values).
    int n = MultiByteToWideChar(cp, 0, src, -1, nullptr, 0);
    if (n <= 0) return false;
    out.resize(n);
    MultiByteToWideChar(cp, 0, src, -1, &out[0], n);
    if (!out.empty() && out.back() == L'\0') out.pop_back();
    return true;
}

// Startup and environment errors call MessageBoxA directly, bypassing
// DialogBoxParamA. Convert the text ourselves and call MessageBoxW:
// MessageBoxA's own ANSI conversion uses the process ACP through ntdll's
// internal NLS tables, which neither the MultiByteToWideChar hook nor the
// GetACP hook intercepts. On a Western system, the Shift-JIS bytes would be
// decoded as Windows-1252.
typedef int(WINAPI* Fn_MessageBoxA)(HWND, LPCSTR, LPCSTR, UINT);
static Fn_MessageBoxA g_origMessageBoxA = nullptr;

static int WINAPI MessageBoxA_Hook(HWND hWnd, LPCSTR lpText, LPCSTR lpCaption, UINT uType) {
    std::wstring wText, wCaption;
    bool haveText    = UiTextToWide(lpText, wText);
    bool haveCaption = UiTextToWide(lpCaption, wCaption);
    return MessageBoxW(hWnd,
                       haveText    ? wText.c_str()    : nullptr,
                       haveCaption ? wCaption.c_str() : nullptr,
                       uType);
}

// Dialog bodies and buttons take that same ACP path through SetDlgItemTextA.
typedef BOOL(WINAPI* Fn_SetDlgItemTextA)(HWND, int, LPCSTR);
static Fn_SetDlgItemTextA g_origSetDlgItemTextA = nullptr;

static BOOL WINAPI SetDlgItemTextA_Hook(HWND hDlg, int nIDDlgItem, LPCSTR lpString) {
    std::wstring wide;
    bool translated = false;
    if (UiTextToWide(lpString, wide, &translated)) {
        // If there is no translation, the text is still Japanese. Sending
        // UTF-16 to an ANSI window procedure would convert it back through
        // the ACP and corrupt it, so use the W call only for translated
        // text or Unicode windows.
        HWND item = GetDlgItem(hDlg, nIDDlgItem);
        if (item && (translated || IsWindowUnicode(item))) {
            return SetDlgItemTextW(hDlg, nIDDlgItem, wide.c_str());
        }
    }
    return g_origSetDlgItemTextA(hDlg, nIDDlgItem, lpString);
}

static HMODULE WINAPI LoadLibraryExA_Hook(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    HMODULE result = g_origLoadLibraryExA(lpLibFileName, hFile, dwFlags);

    if (lpLibFileName && result) {
        const char* name = strrchr(lpLibFileName, '\\');
        name = name ? name + 1 : lpLibFileName;

        if (_stricmp(name, "resident.dll") == 0) {
            Log("[*] resident.dll loaded\n");
            InstallHooks(result);
            if (MH_STATUS st = MH_DisableHook((void*)&LoadLibraryExA); st != MH_OK) {
                Log("[!] MH_DisableHook(LoadLibraryExA) returned %d"
                    " - this hook stays live for the process\n", st);
            }
        }
    }

    return result;
}


//=============================================================================
// Codepage/Locale Hook Forward Declarations
//=============================================================================
typedef int(WINAPI* Fn_MultiByteToWideChar)(UINT, DWORD, LPCCH, int, LPWSTR, int);
typedef int(WINAPI* Fn_WideCharToMultiByte)(UINT, DWORD, LPCWCH, int, LPSTR, int, LPCCH, LPBOOL);
static Fn_MultiByteToWideChar g_origMultiByteToWideChar = nullptr;
static Fn_WideCharToMultiByte g_origWideCharToMultiByte = nullptr;

typedef HWND(WINAPI* Fn_CreateWindowExA)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
static Fn_CreateWindowExA g_origCreateWindowExA = nullptr;

typedef BOOL(WINAPI* Fn_SetWindowTextA)(HWND, LPCSTR);
static Fn_SetWindowTextA g_origSetWindowTextA = nullptr;

static int WINAPI MultiByteToWideChar_Hook(UINT, DWORD, LPCCH, int, LPWSTR, int);
static int WINAPI WideCharToMultiByte_Hook(UINT, DWORD, LPCWCH, int, LPSTR, int, LPCCH, LPBOOL);
static HWND WINAPI CreateWindowExA_Hook(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
static BOOL WINAPI SetWindowTextA_Hook(HWND, LPCSTR);

// Used only by the title hooks. GameHasFocus intentionally does not use it;
// it checks the foreground window's process instead.
static std::atomic<HWND> g_mainGameWindow{ nullptr };

static BOOL CALLBACK FixEarlyWindowProc(HWND hwnd, LPARAM) {
    if (!Config::windowTitle[0]) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    wchar_t w[256];
    if (MultiByteToWideChar(CP_UTF8, 0, Config::windowTitle, -1, w, 256) > 0) {
        DefWindowProcW(hwnd, WM_SETTEXT, 0, (LPARAM)w);
        if (!g_mainGameWindow && !GetParent(hwnd)) g_mainGameWindow = hwnd;
    }
    return TRUE;
}


// Hooks a codepage API in whichever module actually implements it.
// On Win7+ kernel32 forwards to kernelbase, so hooking kernelbase catches calls that
// bypass the forwarder; pre-Win7 there is no kernelbase at all.
static bool HookLocaleApi(const char* name, void* detour, void** original) {
    static const wchar_t* kModules[] = { L"kernelbase", L"kernel32" };

    for (const wchar_t* module : kModules) {
        if (MH_CreateHookApi(module, name, detour, original) == MH_OK) {
            Log("[LOCALE] %s hooked in %S (locale codepages -> CP932)\n", name, module);
            return true;
        }
    }
    return false;
}

// Deliberately uses the unhooked converter: our hook rewrites CP_ACP to
// 932, but this path was produced by the machine's real ANSI code page, and
// converting it as 932 would corrupt it.
static std::wstring AnsiPathToWide(const char* s) {
    std::wstring out;
    if (!s || !*s) return out;

    auto convert = [&](wchar_t* dst, int cap) {
        return g_origMultiByteToWideChar
            ? g_origMultiByteToWideChar(CP_ACP, 0, s, -1, dst, cap)
            : MultiByteToWideChar(CP_ACP, 0, s, -1, dst, cap);
    };

    int len = convert(nullptr, 0);          // includes the terminator
    if (len <= 1) return out;

    out.resize((size_t)len);                // room for the terminator Win32 writes
    if (convert(&out[0], len) <= 0) {
        out.clear();
        return out;
    }
    if (!out.empty() && out.back() == L'\0') out.pop_back();
    return out;
}

//=============================================================================
// Initialization
//=============================================================================
static bool Initialize() {
    // Load config FIRST (before console init, so we know if console is enabled)
    LoadConfig();
    InitDiskLog();

    if (g_modulePinFailed.load()) {
        Log("[!] Module pin failed at load; unloading this DLL would be unsafe."
            " Harmless for the normal statically-imported install.\n");
    }

    // Log the real system code page before the hooks change what GetACP
    // returns.
    Log("[LOCALE] system ACP=%u\n", GetACP());

    // Call this before the first probe. FontProbe caches each face name.
    LoadBundledFonts();

    if (MH_Initialize() != MH_OK) {
        MessageBoxA(nullptr, "MinHook initialization failed.\nTranslation hook is disabled.",
                    "Translation Hook", MB_ICONERROR | MB_OK);
        return false;
    }

    // --- hooks ---
    if (!HookLocaleApi("MultiByteToWideChar",
                       (void*)&MultiByteToWideChar_Hook,
                       (void**)&g_origMultiByteToWideChar)) {
        Log("[!] LOCALE: MultiByteToWideChar hook failed - Japanese text will be mangled\n");
    }
    if (!HookLocaleApi("WideCharToMultiByte",
                       (void*)&WideCharToMultiByte_Hook,
                       (void**)&g_origWideCharToMultiByte)) {
        Log("[!] LOCALE: WideCharToMultiByte hook failed - Japanese text will be mangled\n");
    }
    if (MH_CreateHookApi(L"user32", "CreateWindowExA",
        (void*)&CreateWindowExA_Hook, (void**)&g_origCreateWindowExA) == MH_OK) {
        Log("[LOCALE] CreateWindowExA hooked (-> Unicode + DefWindowProcW)\n");
    }
    if (MH_CreateHookApi(L"user32", "SetWindowTextA",
        (void*)&SetWindowTextA_Hook, (void**)&g_origSetWindowTextA) == MH_OK) {
        Log("[LOCALE] SetWindowTextA hooked (-> DefWindowProcW)\n");
    }
    if (MH_CreateHookApi(L"user32", "DialogBoxParamA",
        (void*)&DialogBoxParamA_Hook, (void**)&g_origDialogBoxParamA) == MH_OK) {
        Log("[DIALOG] DialogBoxParamA hooked (for UI translation)\n");
    }
    if (MH_CreateHookApi(L"user32", "MessageBoxA",
        (void*)&MessageBoxA_Hook, (void**)&g_origMessageBoxA) == MH_OK) {
        Log("[DIALOG] MessageBoxA hooked (startup/error dialogs)\n");
    }
    if (MH_CreateHookApi(L"user32", "SetDlgItemTextA",
        (void*)&SetDlgItemTextA_Hook, (void**)&g_origSetDlgItemTextA) == MH_OK) {
        Log("[DIALOG] SetDlgItemTextA hooked (dialog body + buttons)\n");
    }
    else {
        Log("[!] DIALOG: SetDlgItemTextA hook failed - dialog body and buttons"
            " stay untranslated\n");
    }
    if (MH_CreateHookApi(L"kernel32", "GetACP",
        (void*)&GetACP_Hook, (void**)&g_origGetACP) == MH_OK) {
        Log("[LOCALE] GetACP hooked -> 932 (Japanese)\n");
    }
    if (MH_CreateHookApi(L"kernel32", "GetOEMCP",
        (void*)&GetOEMCP_Hook, (void**)&g_origGetOEMCP) == MH_OK) {
        Log("[LOCALE] GetOEMCP hooked -> 932 (Japanese)\n");
    }
    if (MH_CreateHookApi(L"user32", "CharPrevA",
        (void*)&CharPrevA_Hook, (void**)&g_origCharPrevA) == MH_OK) {
        Log("[LOCALE] CharPrevA hooked -> SJIS\n");
    }
    if (MH_CreateHookApi(L"user32", "CharNextA",
        (void*)&CharNextA_Hook, (void**)&g_origCharNextA) == MH_OK) {
        Log("[LOCALE] CharNextA hooked -> SJIS\n");
    }
    if (MH_CreateHookApi(L"kernel32", "OutputDebugStringA",
        (void*)&OutputDebugStringA_Hook, (void**)&g_origOutputDebugStringA) == MH_OK) {
        Log("[+] OutputDebugStringA hooked - game debug -> console\n");
    }
    if (MH_CreateHookApi(L"gdi32", "GetGlyphOutlineA",
        (void*)&GetGlyphOutlineA_Hook, (void**)&g_origGetGlyphOutlineA) == MH_OK) {
        Log("[+] GetGlyphOutlineA hooked\n");
    }
    if (MH_CreateHookApi(L"gdi32", "GetTextExtentPoint32A",
        (void*)&GetTextExtentPoint32A_Hook,
        (void**)&g_origGetTextExtentPoint32A) == MH_OK) {
        Log("[+] GetTextExtentPoint32A hooked - textOut fragment boundaries\n");
    }
    else {
        Log("[!] GetTextExtentPoint32A hook failed - backlog/save-title"
            " restoration will fall back to best-fit spelling\n");
    }
    if (MH_CreateHookApi(L"gdi32", "GetOutlineTextMetricsA",
        (void*)&GetOutlineTextMetricsA_Hook,
        (void**)&g_origGetOutlineTextMetricsA) == MH_OK) {
        Log("[+] GetOutlineTextMetricsA hooked - glyph loop / textOut split\n");
    }
    if (MH_CreateHookApi(L"gdi32", "CreateFontIndirectA",
        (void*)&CreateFontIndirectA_Hook,
        (void**)&g_origCreateFontIndirectA) == MH_OK) {
        Log("[+] Font hook installed\n");
    }
    if (Config::enableAssetRedirect) {
        CreateDirectoryA(".\\tl", nullptr);
        CreateDirectoryA(Config::tlAssetsPath, nullptr);
        if (MH_CreateHookApi(L"kernel32", "CreateFileA",
            (void*)&CreateFileA_Hook, (void**)&g_origCreateFileA) == MH_OK) {
            Log("[+] Asset redirection hooked (%s)\n", Config::tlAssetsPath);
        }
    }

    // Load before any hook is enabled: g_charIdToName has no mutex, and
    // AdvCharSay_Hook reads it from the game's render thread once hooks are
    // live.
    LoadCharIdTable(Config::charIdFile);

    HMODULE hResident = GetModuleHandleA("resident.dll");
    if (hResident) {
        InstallHooks(hResident);
    } else if (MH_CreateHookApi(L"kernel32", "LoadLibraryExA",
        (void*)&LoadLibraryExA_Hook, (void**)&g_origLoadLibraryExA) == MH_OK) {
        Log("[*] Waiting for resident.dll...\n");
    }

    // ok hooks
    MH_EnableHook(MH_ALL_HOOKS);

    // Retitle any windows the game created before the hooks were installed.
    EnumWindows(FixEarlyWindowProc, 0);

    InitConsole();
    if (Config::enableConsole) {
        FILE* stdinFile;
        freopen_s(&stdinFile, "CONIN$", "r", stdin);
        CreateThread(nullptr, 0, ConsoleInputThread, nullptr, 0, nullptr);
    }

    const char* base_title = Config::windowTitle[0] ? Config::windowTitle : "よついろ★パッショナート！";
    Log("==================================================\n");
    Log("%s - Translation Hook\n", base_title);
    Log("==================================================\n\n");

    if (Config::enableDiscordPresence) {
        InitDiscordRPC();
        Log("[Discord] Rich Presence enabled (can disable in ini: EnableDiscordPresence=false)\n");
    } else {
        Log("[Discord] Rich Presence disabled in config\n");
    }

    // Load translations using config paths
    TranslationDB::LoadSummary tlSummary =
        g_translationDB.Load(Config::translationFile, Config::namesFile);

    if (Config::enableEditingTools) {
        size_t watchers = StartTranslationWatchers([]() {
            g_translationDB.Reload();
            MessageBeep(MB_OK);
        });
        g_hotkeyThread = CreateThread(nullptr, 0, HotkeyThreadProc, nullptr, 0, nullptr);
        if (!g_hotkeyThread) {
            Log("[!] Hotkey thread failed to start (error %lu)\n", GetLastError());
        }
        Log("[*] Editing tools: reload hotkey %s, %zu file watcher(s)\n",
            g_hotkeyThread ? "on" : "FAILED", watchers);
    }

    if (tlSummary.sourceFound && !tlSummary.dialogueFound) {
        Log("[!] Translation files present but no dialogue was loaded"
            " (ScriptsDir=%s, TranslationFile=%s)\n",
            Config::scriptsDir, Config::translationFile);

        // A translator starting from a fresh dump has all-empty TRANSLATION
        // columns; don't show them a warning box on every launch.
        if (!Config::enableEditingTools) {
            std::wstring msg =
                L"Translation files were found, but no dialogue translations "
                L"were loaded.\n\n"
                L"This usually means the patch is installed incompletely. Check "
                L"the paths under [Files] in yotsuiro_tl.ini";
            std::wstring wlog = AnsiPathToWide(Config::logFile);
            if (!wlog.empty()) {
                msg += L", and see " + wlog + L" for what was actually read";
            }
            msg += L".\n\nThe game will run with untranslated dialogue.";

            MessageBoxW(nullptr, msg.c_str(), L"Yotsuiro Translation Hook",
                        MB_ICONWARNING | MB_OK | MB_SETFOREGROUND);
        }
    }

    return true;
}

// Normally never runs: the module pins itself, the proxy is statically
// imported, and at process exit DllMain receives a non-null lpReserved,
// which skips this call. Treat it as best effort; FileWatcher::Stop can
// time out and leave a thread running, so dynamically unloading this DLL is
// not supported.
static void Shutdown() {
    g_running = false;
    StopTranslationWatchers();

    if (Config::enableDiscordPresence) {
        ShutdownDiscordRPC();
    }

    if (g_hotkeyThread) {
        WaitForSingleObject(g_hotkeyThread, 1000);
        CloseHandle(g_hotkeyThread);
    }

    Log("\n[*] Shutting down...\n");
    MH_Uninitialize();
    WordWrap::ReleaseMeasureDC();
    JpFallback::Release();
    UnloadBundledFonts();

    if (g_logFile) {
        std::lock_guard<std::mutex> lock(g_logMutex);
        fclose(g_logFile);
        g_logFile = nullptr;   // threads that outlive Shutdown still call Log()
        FreeConsole();
    }

    if (g_diskLog) {
        std::lock_guard<std::mutex> lock(g_logMutex);
        fclose(g_diskLog);
        g_diskLog = nullptr;
    }
}

//=============================================================================
// Codepage Hook Functions
//=============================================================================

// These four values are placeholders that resolve to a code page from the
// current locale settings; every other value names a code page directly.
static inline bool IsLocaleRelativeCodePage(UINT cp) {
    return cp == CP_ACP || cp == CP_OEMCP || cp == CP_MACCP || cp == CP_THREAD_ACP;
}

static int WINAPI MultiByteToWideChar_Hook(
    UINT CodePage, DWORD dwFlags, LPCCH lpMultiByteStr, int cbMultiByte,
    LPWSTR lpWideCharStr, int cchWideChar)
{
    // Force every locale-relative codepage to Japanese. ATL's conversion
    // helpers pass CP_THREAD_ACP, which resolves against the thread locale
    // rather than GetACP, so it needs redirecting too.
    if (IsLocaleRelativeCodePage(CodePage)) {
        CodePage = 932;
    }
    return g_origMultiByteToWideChar(CodePage, dwFlags, lpMultiByteStr, cbMultiByte,
        lpWideCharStr, cchWideChar);
}

static int WINAPI WideCharToMultiByte_Hook(
    UINT CodePage, DWORD dwFlags, LPCWCH lpWideCharStr, int cchWideChar,
    LPSTR lpMultiByteStr, int cbMultiByte, LPCCH lpDefaultChar, LPBOOL lpUsedDefaultChar)
{
    if (IsLocaleRelativeCodePage(CodePage)) {
        CodePage = 932;
    }
    return g_origWideCharToMultiByte(CodePage, dwFlags, lpWideCharStr, cchWideChar,
        lpMultiByteStr, cbMultiByte, lpDefaultChar, lpUsedDefaultChar);
}

//=============================================================================
// CreateWindowExA Hook - Create windows with Unicode title
//=============================================================================
static HWND WINAPI CreateWindowExA_Hook(
    DWORD dwExStyle, LPCSTR lpClassName, LPCSTR lpWindowName, DWORD dwStyle,
    int x, int y, int nWidth, int nHeight,
    HWND hWndParent, HMENU hMenu, HINSTANCE hInstance, LPVOID lpParam)
{
    // Convert class name to Unicode (handle ATOM case)
    wchar_t wideClassName[256] = {};
    LPCWSTR pWideClassName = nullptr;
    if (lpClassName) {
        if ((uintptr_t)lpClassName <= 0xFFFF) {
            // It's an ATOM, pass as-is
            pWideClassName = (LPCWSTR)lpClassName;
        } else {
            g_origMultiByteToWideChar(932, 0, lpClassName, -1, wideClassName, 256);
            pWideClassName = wideClassName;
        }
    }

    // Convert window title to Unicode (with optional translation)
    wchar_t wideTitle[512] = {};
    LPCWSTR pWideTitle = nullptr;
    
    // Only apply custom title to main window (top-level, has title, no parent)
    bool isMainWindow = (hWndParent == nullptr) && lpWindowName && lpWindowName[0];
    
    if (isMainWindow && Config::windowTitle[0] != '\0') {
        // Custom window title from config
        MultiByteToWideChar(CP_UTF8, 0, Config::windowTitle, -1, wideTitle, 512);
        pWideTitle = wideTitle;
    } 
    else if (lpWindowName && lpWindowName[0]) {
        // Try to find UI translation (for dialogs, buttons, etc.)
        std::string uiTranslation = g_translationDB.FindUITranslation(lpWindowName);
        if (!uiTranslation.empty()) {
            // Found translation - convert UTF-8 to Unicode
            MultiByteToWideChar(CP_UTF8, 0, uiTranslation.c_str(), -1, wideTitle, 512);
        } else {
            // No translation - just convert SJIS to Unicode
            g_origMultiByteToWideChar(932, 0, lpWindowName, -1, wideTitle, 512);
        }
        pWideTitle = wideTitle;
    }

    // Create window using Unicode API
    HWND result = CreateWindowExW(dwExStyle, pWideClassName, pWideTitle, dwStyle,
        x, y, nWidth, nHeight, hWndParent, hMenu, hInstance, lpParam);

    // Force-set the window text using DefWindowProcW
    // This bypasses the ANSI WndProc and sets Unicode text directly
    if (result && pWideTitle) {
        DefWindowProcW(result, WM_SETTEXT, 0, (LPARAM)pWideTitle);
    }

    // Track main window for SetWindowTextA filtering
    if (result && isMainWindow && g_mainGameWindow == nullptr) {
        g_mainGameWindow = result;
    }

    return result;
}

//=============================================================================
// SetWindowTextA Hook - Title often set AFTER window creation
//=============================================================================

static BOOL WINAPI SetWindowTextA_Hook(HWND hWnd, LPCSTR lpString)
{
    // Only override main game window with custom title (not dialogs)
    if (Config::windowTitle[0] != '\0' && hWnd == g_mainGameWindow) {
        wchar_t wideTitle[256];
        MultiByteToWideChar(CP_UTF8, 0, Config::windowTitle, -1, wideTitle, 256);
        DefWindowProcW(hWnd, WM_SETTEXT, 0, (LPARAM)wideTitle);
        return TRUE;
    }

    if (lpString) {
        // Dialog captions are set here during WM_INITDIALOG (engine code at
        // 0x417E80). The caption belongs to the dialog window itself, not a
        // child control, so the EnumChildWindows pass in DialogBoxParamA_Hook
        // never reaches it.
        std::wstring wide;
        if (UiTextToWide(lpString, wide)) {
            DefWindowProcW(hWnd, WM_SETTEXT, 0, (LPARAM)wide.c_str());
            return TRUE;
        }
    }
    return g_origSetWindowTextA(hWnd, lpString);
}

//=============================================================================
// winmm.dll Proxy Entry Point
//=============================================================================
static DWORD WINAPI DeferredInit(LPVOID) {
    Initialize();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    switch (reason) {
    case DLL_PROCESS_ATTACH: {
        DisableThreadLibraryCalls(hModule);
        // Pin this module so it can never be unloaded: the worker threads
        // cannot be joined while the loader lock is held, so unloading
        // would free code that is still running. If pinning fails, continue
        // anyway; returning FALSE from DLL_PROCESS_ATTACH would abort the
        // game's startup.
        {
            HMODULE pin = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN |
                                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                    (LPCWSTR)&DllMain, &pin)) {
                // OutputDebugString is the only logging available this
                // early; Initialize() repeats the warning to hook.log.
                OutputDebugStringA("[YotsuiroHook] module pin failed; "
                                   "unload would be unsafe\n");
                g_modulePinFailed.store(true);
            }
        }
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(hModule, path, MAX_PATH);
        const wchar_t* name = wcsrchr(path, L'\\');
        name = name ? name + 1 : path;
        if (_wcsicmp(name, L"winmm.dll") == 0) proxy_init();  // only proxy when we ARE winmm.dll
        CreateThread(nullptr, 0, DeferredInit, nullptr, 0, nullptr);
        break;
    }

    case DLL_PROCESS_DETACH:
        if (!lpReserved) {
            Shutdown();
            proxy_free();
        }
        break;
    }
    return TRUE;
}
