// aepbake.exe - turns queued .aep paths into cached preview frames.
//
//   aepbake --queue <project.aep>   drop a request in the queue, start the
//                                   daemon if needed, return immediately
//   aepbake --daemon                drain the queue, then exit
//   aepbake --scan <folder> [-r]    queue every project under a folder
//   aepbake --status                what is cached, what is pending
//
// Only one daemon runs at a time, held by a named mutex. Requests that arrive
// while it is working are picked up by the same pass, so After Effects starts
// once for a whole folder rather than once per file.

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shlobj.h>

#include <algorithm>
#include <string>
#include <vector>

#include "aep.h"
#include "cache.h"

using namespace Gdiplus;

namespace {

const int     kMaxCacheEdge = 1024;   // longest side stored in the cache
const DWORD   kCollectMs    = 1500;   // wait for stragglers before starting AE
const DWORD   kAeTimeoutMs  = 15 * 60 * 1000;

std::wstring g_root, g_cache, g_queue;

void Log(const std::wstring& msg) {
    std::wstring path = g_root + L"\\aepbake.log";
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t stamp[32];
    swprintf(stamp, 32, L"[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);

    std::wstring line = std::wstring(stamp) + msg + L"\r\n";
    int need = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), -1, NULL, 0, NULL, NULL);
    if (need > 1) {
        std::vector<char> utf8(need);
        WideCharToMultiByte(CP_UTF8, 0, line.c_str(), -1, &utf8[0], need, NULL, NULL);
        DWORD wrote = 0;
        WriteFile(h, &utf8[0], static_cast<DWORD>(need - 1), &wrote, NULL);
    }
    CloseHandle(h);
}

std::wstring ExeDir() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(NULL, buf, ARRAYSIZE(buf));
    if (n == 0) return std::wstring();
    std::wstring s(buf, n);
    size_t slash = s.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : s.substr(0, slash);
}

bool ReadFileBytes(const std::wstring& path, std::vector<unsigned char>& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0 ||
        size.QuadPart > 512ll * 1024 * 1024) {
        CloseHandle(h);
        return false;
    }
    out.resize(static_cast<size_t>(size.QuadPart));
    size_t done = 0;
    while (done < out.size()) {
        DWORD want = static_cast<DWORD>(
            (out.size() - done) > 0x400000 ? 0x400000 : (out.size() - done));
        DWORD got = 0;
        if (!ReadFile(h, &out[done], want, &got, NULL) || got == 0) {
            CloseHandle(h);
            return false;
        }
        done += got;
    }
    CloseHandle(h);
    return true;
}

bool WriteTextFile(const std::wstring& path, const std::string& text) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    BOOL ok = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &wrote, NULL);
    CloseHandle(h);
    return ok != FALSE;
}

std::string Utf8(const std::wstring& s) {
    if (s.empty()) return std::string();
    int need = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                   NULL, 0, NULL, NULL);
    std::string out(static_cast<size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        &out[0], need, NULL, NULL);
    return out;
}

std::wstring Wide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int need = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                   NULL, 0);
    std::wstring out(static_cast<size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        &out[0], need);
    return out;
}

// ExtendScript reads these paths out of JSON, where a backslash needs escaping.
std::string JsonEscape(const std::wstring& s) {
    std::string u = Utf8(s);
    std::string out;
    out.reserve(u.size() + 16);
    for (size_t i = 0; i < u.size(); ++i) {
        char c = u[i];
        if (c == '\\' || c == '"') { out.push_back('\\'); out.push_back(c); }
        else if (c == '\n' || c == '\r' || c == '\t') out.push_back(' ');
        else out.push_back(c);
    }
    return out;
}

// --- queue ------------------------------------------------------------

struct Job {
    std::wstring key;
    std::wstring project;     // real path, or a spooled copy
    std::wstring display;     // what to call it in the log
    std::wstring comp;
};

std::wstring QueueFileFor(const std::wstring& key) {
    return g_queue + L"\\" + key + L".job";
}

bool Enqueue(const std::wstring& project) {
    return AepWriteJob(project);
}

std::vector<Job> ReadQueue() {
    std::vector<Job> jobs;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((g_queue + L"\\*.job").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return jobs;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring name = fd.cFileName;
        size_t dot = name.find_last_of(L'.');
        if (dot == std::wstring::npos) continue;

        Job j;
        j.key = name.substr(0, dot);
        if (!AepReadJob(g_queue + L"\\" + name, &j.project, &j.display)) continue;
        if (j.display.empty()) j.display = j.project;

        // The key is the hash of the project's bytes, so a request whose file
        // has since been edited or replaced no longer matches and is dropped.
        if (AepCacheKey(j.project) != j.key) {
            DeleteFileW((g_queue + L"\\" + name).c_str());
            if (AepIsSpooled(j.project)) DeleteFileW(j.project.c_str());
            continue;
        }

        // Name the comp our own parser would call the main one, so the baked
        // frame matches what the metadata says.
        std::vector<unsigned char> data;
        if (ReadFileBytes(j.project, data) && !data.empty()) {
            AepProject proj;
            if (AepParse(&data[0], data.size(), proj)) {
                const AepComp* main = AepPickMainComp(proj);
                if (main) j.comp = main->name;
            }
        }
        jobs.push_back(j);
    } while (FindNextFileW(h, &fd));

    FindClose(h);
    return jobs;
}

// --- image conversion -------------------------------------------------

bool GetEncoderClsid(const wchar_t* mime, CLSID* out) {
    UINT count = 0, bytes = 0;
    if (GetImageEncodersSize(&count, &bytes) != Ok || bytes == 0) return false;
    std::vector<unsigned char> buf(bytes);
    ImageCodecInfo* info = reinterpret_cast<ImageCodecInfo*>(&buf[0]);
    if (GetImageEncoders(count, bytes, info) != Ok) return false;
    for (UINT i = 0; i < count; ++i) {
        if (wcscmp(info[i].MimeType, mime) == 0) { *out = info[i].Clsid; return true; }
    }
    return false;
}

// AE writes a big 16-bit TIFF with alpha; the cache wants a modest opaque PNG.
bool ConvertToCache(const std::wstring& tiff, const std::wstring& pngOut) {
    bool ok = false;
    Image* src = Image::FromFile(tiff.c_str(), FALSE);
    if (src && src->GetLastStatus() == Ok &&
        src->GetWidth() > 0 && src->GetHeight() > 0) {

        UINT sw = src->GetWidth(), sh = src->GetHeight();
        double scale = 1.0;
        UINT longest = (std::max)(sw, sh);
        if (longest > static_cast<UINT>(kMaxCacheEdge))
            scale = static_cast<double>(kMaxCacheEdge) / longest;

        int dw = (std::max)(1, static_cast<int>(sw * scale + 0.5));
        int dh = (std::max)(1, static_cast<int>(sh * scale + 0.5));

        Bitmap dst(dw, dh, PixelFormat32bppARGB);
        {
            Graphics g(&dst);
            g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
            g.SetPixelOffsetMode(PixelOffsetModeHalf);
            // Flatten onto a neutral ground so alpha does not read as black.
            SolidBrush back(Color(0xFF1A1526));
            g.FillRectangle(&back, 0, 0, dw, dh);
            g.DrawImage(src, Rect(0, 0, dw, dh));
            g.Flush(FlushIntentionSync);
        }

        CLSID png;
        if (GetEncoderClsid(L"image/png", &png))
            ok = (dst.Save(pngOut.c_str(), &png, NULL) == Ok);
    }
    delete src;
    return ok;
}

// --- result parsing ---------------------------------------------------

// Small hand-rolled reader; the result file is written by our own script and
// has a fixed shape, so a full JSON parser would be overkill.
std::string FieldOf(const std::string& obj, const char* name) {
    std::string key = std::string("\"") + name + "\":";
    size_t p = obj.find(key);
    if (p == std::string::npos) return std::string();
    p += key.size();
    while (p < obj.size() && (obj[p] == ' ')) ++p;
    if (p < obj.size() && obj[p] == '"') {
        ++p;
        std::string out;
        while (p < obj.size() && obj[p] != '"') {
            if (obj[p] == '\\' && p + 1 < obj.size()) ++p;
            out.push_back(obj[p++]);
        }
        return out;
    }
    size_t e = obj.find_first_of(",}", p);
    return obj.substr(p, (e == std::string::npos ? obj.size() : e) - p);
}

struct BakeResult {
    std::wstring key, file, comp, error;
    bool ok;
};

std::vector<BakeResult> ReadResults(const std::wstring& path) {
    std::vector<BakeResult> out;
    std::vector<unsigned char> raw;
    if (!ReadFileBytes(path, raw) || raw.empty()) return out;

    std::string text(reinterpret_cast<char*>(&raw[0]), raw.size());
    size_t pos = 0;
    while (true) {
        size_t s = text.find('{', pos);
        if (s == std::string::npos) break;
        size_t e = text.find('}', s);
        if (e == std::string::npos) break;
        std::string obj = text.substr(s, e - s + 1);
        pos = e + 1;

        BakeResult r;
        r.key   = Wide(FieldOf(obj, "key"));
        r.file  = Wide(FieldOf(obj, "file"));
        r.comp  = Wide(FieldOf(obj, "comp"));
        r.error = Wide(FieldOf(obj, "error"));
        r.ok    = FieldOf(obj, "ok") == "true";
        if (!r.key.empty()) out.push_back(r);
    }
    return out;
}

// --- After Effects ----------------------------------------------------

// Adobe records every installed version under HKLM\SOFTWARE\Adobe\After
// Effects\<version>\InstallPath, which beats guessing at folder names: it
// survives non-default install locations and future releases. The highest
// version key wins.
std::wstring FindAfterFx() {
    static const wchar_t* kRoots[] = {
        L"SOFTWARE\\Adobe\\After Effects",
        L"SOFTWARE\\WOW6432Node\\Adobe\\After Effects"
    };

    std::wstring best, bestVer;
    for (size_t r = 0; r < ARRAYSIZE(kRoots); ++r) {
        HKEY root = NULL;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRoots[r], 0,
                          KEY_READ | KEY_WOW64_64KEY, &root) != ERROR_SUCCESS)
            continue;

        for (DWORD i = 0; ; ++i) {
            wchar_t ver[64];
            DWORD verLen = ARRAYSIZE(ver);
            if (RegEnumKeyExW(root, i, ver, &verLen, NULL, NULL, NULL, NULL)
                    != ERROR_SUCCESS)
                break;

            wchar_t installPath[MAX_PATH * 2];
            DWORD cb = sizeof(installPath);
            if (RegGetValueW(root, ver, L"InstallPath", RRF_RT_REG_SZ, NULL,
                             installPath, &cb) != ERROR_SUCCESS)
                continue;

            std::wstring exe = installPath;
            if (!exe.empty() && exe[exe.size() - 1] != L'\\') exe += L"\\";
            exe += L"AfterFX.exe";
            if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) continue;

            if (bestVer.empty() || _wcsicmp(ver, bestVer.c_str()) > 0) {
                bestVer = ver;
                best = exe;
            }
        }
        RegCloseKey(root);
    }

    if (!best.empty()) return best;

    // Fall back to the stock layout if the registry says nothing.
    static const wchar_t* kYears[] = { L"2027", L"2026", L"2025", L"2024", L"2023" };
    for (size_t y = 0; y < ARRAYSIZE(kYears); ++y) {
        std::wstring p = std::wstring(L"C:\\Program Files\\Adobe\\Adobe After Effects ") +
                         kYears[y] + L"\\Support Files\\AfterFX.exe";
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
    }
    return std::wstring();
}

// After Effects refuses to let a script write files unless
// Preferences > Scripting and Expressions > "Allow Scripts to Write Files and
// Access Network" is on. With it off the bake fails silently, so it is worth
// reporting plainly rather than letting people wonder.
//
// Returns 1 enabled, 0 disabled, -1 unknown (no prefs file found yet).
int ScriptingPrefState(std::wstring* detail) {
    wchar_t appdata[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, ARRAYSIZE(appdata));
    if (n == 0 || n >= ARRAYSIZE(appdata)) return -1;

    std::wstring base = std::wstring(appdata) + L"\\Adobe\\After Effects";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((base + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;

    int state = -1;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        std::wstring ver = fd.cFileName;
        if (ver == L"." || ver == L"..") continue;

        std::wstring prefs = base + L"\\" + ver + L"\\Adobe After Effects " +
                             ver + L" Prefs.txt";
        std::vector<unsigned char> raw;
        if (!ReadFileBytes(prefs, raw) || raw.empty()) continue;

        std::string text(reinterpret_cast<char*>(&raw[0]), raw.size());
        size_t p = text.find("\"Pref_SCRIPTING_FILE_NETWORK_SECURITY\"");
        int here = 0;
        if (p != std::string::npos) {
            size_t q = text.find('=', p);
            size_t eol = text.find('\n', p);
            if (q != std::string::npos && (eol == std::string::npos || q < eol) &&
                text.find("\"1\"", q) != std::string::npos &&
                text.find("\"1\"", q) < (eol == std::string::npos ? text.size() : eol))
                here = 1;
        }
        if (detail) *detail = ver;
        // Any version with it enabled is good enough for the version we drive.
        if (here == 1) { state = 1; break; }
        state = 0;
    } while (FindNextFileW(h, &fd));

    FindClose(h);
    return state;
}

bool AfterFxRunning() {
    // A script sent to a running instance would execute inside the user's own
    // session and disrupt it, so the baker stands down instead.
    HWND w = FindWindowW(L"AE_CApplication_11.0", NULL);
    if (w) return true;
    return FindWindowW(NULL, L"Adobe After Effects") != NULL;
}

bool RunAfterFx(const std::wstring& script) {
    std::wstring afx = FindAfterFx();
    if (afx.empty()) { Log(L"After Effects not found"); return false; }

    // After Effects does not strip quotes around the -r argument: quote the
    // script path and it silently exits without running anything. The 8.3 form
    // has no spaces, so it survives unquoted even under "C:\Program Files".
    std::wstring scriptArg = script;
    wchar_t shortBuf[MAX_PATH * 2];
    DWORD sn = GetShortPathNameW(script.c_str(), shortBuf, ARRAYSIZE(shortBuf));
    if (sn > 0 && sn < ARRAYSIZE(shortBuf)) scriptArg = shortBuf;
    if (scriptArg.find(L' ') != std::wstring::npos)
        Log(L"warning: script path still contains a space: " + scriptArg);

    std::wstring cmd = L"\"" + afx + L"\" -noui -r " + scriptArg;
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessW(NULL, &buf[0], NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        Log(L"could not launch After Effects");
        return false;
    }

    DWORD wait = WaitForSingleObject(pi.hProcess, kAeTimeoutMs);
    if (wait == WAIT_TIMEOUT) {
        Log(L"After Effects timed out; terminating");
        TerminateProcess(pi.hProcess, 1);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return wait != WAIT_TIMEOUT;
}

// --- daemon -----------------------------------------------------------

void NotifyShell(const std::wstring& project) {
    SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSHNOWAIT,
                   project.c_str(), NULL);
}

// Used when the bake came from a spooled copy and the original path is
// unknown: ask the shell to re-read associations, which drops its cached
// tiles for the type.
void NotifyShellAll() {
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, NULL, NULL);
}

int RunDaemon() {
    HANDLE mutex = CreateMutexW(NULL, FALSE, AepBakerMutexName());
    if (!mutex) return 1;
    if (WaitForSingleObject(mutex, 0) != WAIT_OBJECT_0) {
        // Another daemon already owns the queue; it will pick up our request.
        CloseHandle(mutex);
        return 0;
    }

    GdiplusStartupInput gsi;
    ULONG_PTR token = 0;
    GdiplusStartup(&token, &gsi, NULL);

    const std::wstring batchFile  = g_root + L"\\batch.json";
    const std::wstring resultFile = g_root + L"\\batch_result.json";
    const std::wstring tmpDir     = g_root + L"\\tmp";
    const std::wstring script     = ExeDir() + L"\\bake_batch.jsx";

    CreateDirectoryW(tmpDir.c_str(), NULL);

    if (GetFileAttributesW(script.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Log(L"bake_batch.jsx missing next to aepbake.exe");
        GdiplusShutdown(token);
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        return 1;
    }

    for (;;) {
        // Let a burst of requests accumulate so AE starts once for the folder.
        Sleep(kCollectMs);

        std::vector<Job> jobs = ReadQueue();
        if (jobs.empty()) break;

        if (AfterFxRunning()) {
            Log(L"After Effects is open; leaving the queue for later");
            break;
        }

        Log(L"baking " + std::to_wstring(jobs.size()) + L" project(s)");

        std::string json = "{\"outDir\":\"" + JsonEscape(tmpDir) + "\",\"jobs\":[";
        for (size_t i = 0; i < jobs.size(); ++i) {
            if (i) json += ",";
            json += "{\"key\":\"" + JsonEscape(jobs[i].key) +
                    "\",\"project\":\"" + JsonEscape(jobs[i].project) +
                    "\",\"comp\":\"" + JsonEscape(jobs[i].comp) + "\"}";
        }
        json += "]}";

        DeleteFileW(resultFile.c_str());
        if (!WriteTextFile(batchFile, json)) { Log(L"could not write batch.json"); break; }

        if (!RunAfterFx(script)) break;

        std::vector<BakeResult> results = ReadResults(resultFile);
        Log(L"After Effects returned " + std::to_wstring(results.size()) + L" result(s)");

        for (size_t i = 0; i < results.size(); ++i) {
            const BakeResult& r = results[i];

            std::wstring project, display;
            for (size_t j = 0; j < jobs.size(); ++j) {
                if (jobs[j].key == r.key) {
                    project = jobs[j].project;
                    display = jobs[j].display;
                }
            }

            if (r.ok && !r.file.empty()) {
                std::wstring png = g_cache + L"\\" + r.key + L".png";
                if (ConvertToCache(r.file, png)) {
                    Log(L"cached " + display);
                    // Tell the shell to re-ask. For a spooled job we have no
                    // original path, so nudge by extension instead.
                    if (!project.empty() && !AepIsSpooled(project)) NotifyShell(project);
                    else NotifyShellAll();
                } else {
                    Log(L"conversion failed for " + display);
                }
                DeleteFileW(r.file.c_str());
            } else {
                Log(L"bake failed for " + display + L": " + r.error);
            }

            // Drop the request either way; a failure retried forever would
            // relaunch After Effects on every folder view.
            DeleteFileW(QueueFileFor(r.key).c_str());
            if (!project.empty() && AepIsSpooled(project)) DeleteFileW(project.c_str());
        }

        // Anything After Effects never reported on would loop forever.
        for (size_t j = 0; j < jobs.size(); ++j) {
            bool seen = false;
            for (size_t i = 0; i < results.size(); ++i)
                if (results[i].key == jobs[j].key) seen = true;
            if (!seen) {
                Log(L"no result for " + jobs[j].display + L"; dropping request");
                DeleteFileW(QueueFileFor(jobs[j].key).c_str());
                if (AepIsSpooled(jobs[j].project)) DeleteFileW(jobs[j].project.c_str());
            }
        }
    }

    GdiplusShutdown(token);
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return 0;
}

void StartDaemon() {
    if (AepBakerRunning()) return;

    wchar_t self[MAX_PATH * 2];
    if (GetModuleFileNameW(NULL, self, ARRAYSIZE(self)) == 0) return;

    std::wstring cmd = std::wstring(L"\"") + self + L"\" --daemon";
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    if (CreateProcessW(NULL, &buf[0], NULL, NULL, FALSE,
                       CREATE_NO_WINDOW | DETACHED_PROCESS, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

void ScanFolder(const std::wstring& dir, bool recurse, int* queued, int* already) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        std::wstring full = dir + L"\\" + name;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (recurse) ScanFolder(full, true, queued, already);
            continue;
        }

        size_t dot = name.find_last_of(L'.');
        if (dot == std::wstring::npos) continue;
        std::wstring ext = name.substr(dot);
        for (size_t i = 0; i < ext.size(); ++i)
            ext[i] = static_cast<wchar_t>(towlower(ext[i]));
        if (ext != L".aep" && ext != L".aet") continue;

        if (Enqueue(full)) ++(*queued);
        else ++(*already);
    } while (FindNextFileW(h, &fd));

    FindClose(h);
}

int CountFiles(const std::wstring& pattern) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) ++n;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

// Watches folders and bakes projects as they appear or change. This is what
// keeps the cache warm: the thumbnail provider runs on a stream and has no
// filename, so it cannot queue anything itself.
int RunWatch(const std::vector<std::wstring>& dirs, bool recurse) {
    std::vector<HANDLE> handles;
    for (size_t i = 0; i < dirs.size(); ++i) {
        HANDLE h = FindFirstChangeNotificationW(
            dirs[i].c_str(), recurse ? TRUE : FALSE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE |
            FILE_NOTIFY_CHANGE_SIZE);
        if (h == INVALID_HANDLE_VALUE) {
            wprintf(L"cannot watch %s\n", dirs[i].c_str());
            continue;
        }
        handles.push_back(h);
        wprintf(L"watching %s%s\n", dirs[i].c_str(), recurse ? L" (recursive)" : L"");
    }
    if (handles.empty()) {
        wprintf(L"nothing to watch\n");
        return 1;
    }

    // Bake whatever is already there before waiting for changes.
    int queued = 0, already = 0;
    for (size_t i = 0; i < dirs.size(); ++i)
        ScanFolder(dirs[i], recurse, &queued, &already);
    wprintf(L"initial scan: %d queued, %d already cached\n", queued, already);
    if (queued > 0) StartDaemon();

    wprintf(L"watching for changes; press Ctrl+C to stop\n");
    for (;;) {
        DWORD w = WaitForMultipleObjects(static_cast<DWORD>(handles.size()),
                                         &handles[0], FALSE, INFINITE);
        if (w < WAIT_OBJECT_0 || w >= WAIT_OBJECT_0 + handles.size()) break;

        size_t idx = w - WAIT_OBJECT_0;
        // Let a save finish before reading the file: a key taken mid-write
        // would be wrong and the bake would fail.
        Sleep(1500);

        int q = 0, a = 0;
        for (size_t i = 0; i < dirs.size(); ++i)
            ScanFolder(dirs[i], recurse, &q, &a);
        if (q > 0) {
            wprintf(L"queued %d changed project(s)\n", q);
            StartDaemon();
        }

        if (!FindNextChangeNotification(handles[idx])) break;
    }

    for (size_t i = 0; i < handles.size(); ++i) FindCloseChangeNotification(handles[i]);
    return 0;
}

void PrintUsage() {
    wprintf(L"aepbake - bakes After Effects project previews into the thumbnail cache\n\n"
            L"  aepbake --scan <folder> [-r]     queue every project in a folder\n"
            L"  aepbake --watch <folder> [-r]    scan, then keep baking as files change\n"
            L"  aepbake --queue <project.aep>    queue a single project\n"
            L"  aepbake --daemon                 drain the queue now (blocks)\n"
            L"  aepbake --status                 show cache and queue counts\n"
            L"  aepbake --doctor                 check the whole setup\n"
            L"  aepbake --clear                  drop cached frames so they re-bake\n"
            L"\n"
            L"Baking pauses while After Effects is open, so your session is never\n"
            L"disturbed; it resumes on the next scan or change.\n");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (!AepEnsureCacheDirs()) {
        wprintf(L"error: cannot create the cache directory\n");
        return 1;
    }
    g_root  = AepCacheRoot();
    g_cache = AepCacheDir();
    g_queue = AepQueueDir();

    if (argc < 2) { PrintUsage(); return 2; }
    std::wstring mode = argv[1];

    if (mode == L"--daemon") return RunDaemon();

    if (mode == L"--queue" && argc >= 3) {
        wchar_t full[MAX_PATH * 4];
        DWORD n = GetFullPathNameW(argv[2], ARRAYSIZE(full), full, NULL);
        std::wstring project = (n > 0 && n < ARRAYSIZE(full)) ? full : argv[2];
        if (Enqueue(project)) wprintf(L"queued: %s\n", project.c_str());
        else                  wprintf(L"already cached or unreadable: %s\n", project.c_str());
        StartDaemon();
        return 0;
    }

    if (mode == L"--scan" && argc >= 3) {
        bool recurse = (argc >= 4 && (std::wstring(argv[3]) == L"-r" ||
                                      std::wstring(argv[3]) == L"--recurse"));
        wchar_t full[MAX_PATH * 4];
        DWORD n = GetFullPathNameW(argv[2], ARRAYSIZE(full), full, NULL);
        std::wstring dir = (n > 0 && n < ARRAYSIZE(full)) ? full : argv[2];

        int queued = 0, already = 0;
        ScanFolder(dir, recurse, &queued, &already);
        wprintf(L"queued %d project(s), %d already cached\n", queued, already);
        if (queued > 0) {
            wprintf(L"baking in the background; run --status to follow progress\n");
            StartDaemon();
        }
        return 0;
    }

    if (mode == L"--watch" && argc >= 3) {
        bool recurse = false;
        std::vector<std::wstring> dirs;
        for (int i = 2; i < argc; ++i) {
            std::wstring a = argv[i];
            if (a == L"-r" || a == L"--recurse") { recurse = true; continue; }
            wchar_t full[MAX_PATH * 4];
            DWORD n = GetFullPathNameW(a.c_str(), ARRAYSIZE(full), full, NULL);
            dirs.push_back((n > 0 && n < ARRAYSIZE(full)) ? full : a);
        }
        return RunWatch(dirs, recurse);
    }

    if (mode == L"--clear") {
        // Frames rendered while a footage drive was offline show After Effects'
        // media-offline colour bars. Dropping them makes those projects bake
        // again once the drive is back.
        int removed = 0;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((g_cache + L"\\*.png").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                if (DeleteFileW((g_cache + L"\\" + fd.cFileName).c_str())) ++removed;
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        wprintf(L"removed %d cached frame(s)\n", removed);
        wprintf(L"they will be baked again as folders are browsed, or run --scan\n");
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, NULL, NULL);
        return 0;
    }

    if (mode == L"--doctor") {
        wprintf(L"AepThumb self-check\n\n");

        std::wstring afx = FindAfterFx();
        if (afx.empty()) {
            wprintf(L"  [FAIL] After Effects not found.\n");
            wprintf(L"         Previews cannot be rendered without it.\n");
        } else {
            wprintf(L"  [ ok ] After Effects: %s\n", afx.c_str());
        }

        std::wstring ver;
        int pref = ScriptingPrefState(&ver);
        if (pref == 1) {
            wprintf(L"  [ ok ] Scripts may write files (AE %s)\n", ver.c_str());
        } else if (pref == 0) {
            wprintf(L"  [FAIL] After Effects blocks scripts from writing files.\n");
            wprintf(L"         Turn on: Edit > Preferences > Scripting and Expressions >\n");
            wprintf(L"         \"Allow Scripts to Write Files and Access Network\",\n");
            wprintf(L"         then restart After Effects. Without it every bake fails.\n");
        } else {
            wprintf(L"  [ ?? ] Could not read After Effects preferences.\n");
            wprintf(L"         Run After Effects once, then check again.\n");
        }

        std::wstring script = ExeDir() + L"\\bake_batch.jsx";
        wprintf(L"  [%s] Render script: %s\n",
                GetFileAttributesW(script.c_str()) != INVALID_FILE_ATTRIBUTES
                    ? L" ok " : L"FAIL",
                script.c_str());

        wchar_t clsid[512];
        DWORD cb = sizeof(clsid);
        bool registered = RegGetValueW(
            HKEY_CLASSES_ROOT,
            L".aep\\ShellEx\\{e357fccd-a995-4576-b01f-234630154e96}",
            NULL, RRF_RT_REG_SZ, NULL, clsid, &cb) == ERROR_SUCCESS;
        if (registered &&
            _wcsicmp(clsid, L"{8E76F525-03F4-403B-A170-1623A5878F14}") == 0) {
            wprintf(L"  [ ok ] Explorer uses this provider for .aep\n");
        } else if (registered) {
            wprintf(L"  [FAIL] .aep thumbnails belong to another handler: %s\n", clsid);
            wprintf(L"         Re-run install.cmd.\n");
        } else {
            wprintf(L"  [FAIL] No thumbnail handler registered for .aep.\n");
            wprintf(L"         Re-run install.cmd.\n");
        }

        wprintf(L"\n  cache  : %d baked, %d pending, %d spooled\n",
                CountFiles(g_cache + L"\\*.png"),
                CountFiles(g_queue + L"\\*.job"),
                CountFiles(AepSpoolDir() + L"\\*.aep"));
        wprintf(L"  baker  : %s\n", AepBakerRunning() ? L"running" : L"idle");
        wprintf(L"  AE now : %s\n",
                AfterFxRunning() ? L"open - baking is paused" : L"closed");
        return 0;
    }

    if (mode == L"--status") {
        wprintf(L"cache : %s\n", g_cache.c_str());
        wprintf(L"  baked previews : %d\n", CountFiles(g_cache + L"\\*.png"));
        wprintf(L"  pending        : %d\n", CountFiles(g_queue + L"\\*.job"));
        wprintf(L"  baker running  : %s\n", AepBakerRunning() ? L"yes" : L"no");
        wprintf(L"  After Effects  : %s\n",
                AfterFxRunning() ? L"open (baking is paused)" : L"closed");
        return 0;
    }

    PrintUsage();
    return 2;
}
