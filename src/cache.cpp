#include "cache.h"

#include <windows.h>
#include <shlobj.h>

#include <vector>

namespace {

std::wstring LocalAppData() {
    PWSTR p = NULL;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, NULL, &p)) || !p)
        return std::wstring();
    std::wstring s(p);
    CoTaskMemFree(p);
    return s;
}

bool EnsureDir(const std::wstring& path) {
    if (CreateDirectoryW(path.c_str(), NULL)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

// FNV-1a, 64-bit. Not a security hash - it only has to separate projects.
unsigned long long Fnv1a(const void* data, size_t len, unsigned long long h) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

bool ReadWhole(const std::wstring& path, std::vector<unsigned char>& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
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

}  // namespace

std::wstring AepCacheRoot() {
    std::wstring base = LocalAppData();
    if (base.empty()) return std::wstring();
    return base + L"\\AepThumb";
}

std::wstring AepCacheDir() {
    std::wstring r = AepCacheRoot();
    return r.empty() ? r : r + L"\\cache";
}

std::wstring AepQueueDir() {
    std::wstring r = AepCacheRoot();
    return r.empty() ? r : r + L"\\queue";
}

std::wstring AepSpoolDir() {
    std::wstring r = AepCacheRoot();
    return r.empty() ? r : r + L"\\spool";
}

bool AepEnsureCacheDirs() {
    std::wstring root = AepCacheRoot();
    if (root.empty()) return false;
    return EnsureDir(root) && EnsureDir(root + L"\\cache") &&
           EnsureDir(root + L"\\queue") && EnsureDir(root + L"\\spool");
}

std::wstring AepCacheKeyFromContent(const unsigned char* data, size_t size) {
    if (!data || size == 0) return std::wstring();

    unsigned long long h = 14695981039346656037ull;
    h = Fnv1a(&size, sizeof(size), h);
    h = Fnv1a(data, size, h);

    wchar_t buf[24];
    swprintf(buf, 24, L"%016llx", h);
    return std::wstring(buf);
}

std::wstring AepCacheKey(const std::wstring& projectPath) {
    if (projectPath.empty()) return std::wstring();
    std::vector<unsigned char> data;
    if (!ReadWhole(projectPath, data) || data.empty()) return std::wstring();
    return AepCacheKeyFromContent(&data[0], data.size());
}

std::wstring AepCachedFrameForKey(const std::wstring& key) {
    std::wstring dir = AepCacheDir();
    if (key.empty() || dir.empty()) return std::wstring();
    return dir + L"\\" + key + L".png";
}

std::wstring AepCachedFrame(const std::wstring& projectPath) {
    return AepCachedFrameForKey(AepCacheKey(projectPath));
}

const wchar_t* AepBakerMutexName() {
    return L"Local\\AepThumbBakerMutex";
}

bool AepBakerRunning() {
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, AepBakerMutexName());
    if (!m) return false;
    CloseHandle(m);
    return true;
}

namespace {

std::string Utf8Of(const std::wstring& s) {
    if (s.empty()) return std::string();
    int need = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                   NULL, 0, NULL, NULL);
    if (need <= 0) return std::string();
    std::string out(static_cast<size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        &out[0], need, NULL, NULL);
    return out;
}

std::wstring WideOf(const std::string& s) {
    if (s.empty()) return std::wstring();
    int need = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                   NULL, 0);
    if (need <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        &out[0], need);
    return out;
}

bool WriteAll(const std::wstring& path, const std::string& bytes) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    BOOL ok = WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &wrote, NULL);
    CloseHandle(h);
    return ok != FALSE;
}

// A queued request is one UTF-8 line with the project path, optionally a
// second line with a friendlier name for the log.
bool WriteJobFile(const std::wstring& key, const std::wstring& projectPath,
                  const std::wstring& displayName) {
    std::wstring cached = AepCachedFrameForKey(key);
    if (GetFileAttributesW(cached.c_str()) != INVALID_FILE_ATTRIBUTES) return false;

    std::wstring job = AepQueueDir() + L"\\" + key + L".job";
    if (GetFileAttributesW(job.c_str()) != INVALID_FILE_ATTRIBUTES) return false;

    std::string body = Utf8Of(projectPath);
    if (body.empty()) return false;
    if (!displayName.empty()) body += "\n" + Utf8Of(displayName);
    return WriteAll(job, body);
}

}  // namespace

bool AepWriteJob(const std::wstring& projectPath, const std::wstring& displayName) {
    std::wstring key = AepCacheKey(projectPath);
    if (key.empty()) return false;
    if (!AepEnsureCacheDirs()) return false;
    return WriteJobFile(key, projectPath, displayName);
}

bool AepReadJob(const std::wstring& jobFile, std::wstring* projectPath,
                std::vector<std::wstring>* names) {
    std::vector<unsigned char> raw;
    if (!ReadWhole(jobFile, raw) || raw.empty()) return false;

    std::string text(reinterpret_cast<char*>(&raw[0]), raw.size());
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        std::string line = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        while (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);
        if (!line.empty()) lines.push_back(line);
        if (nl == std::string::npos) break;
        start = nl + 1;
    }

    if (lines.empty()) return false;
    if (projectPath) *projectPath = WideOf(lines[0]);
    if (names) {
        names->clear();
        for (size_t i = 1; i < lines.size(); ++i) names->push_back(WideOf(lines[i]));
    }
    return true;
}

namespace {

// Adds a file name to a request that is already queued. Copies of one project
// share a key and so a single request, but each copy may carry its own name,
// and every one of them needs telling when the frame is ready.
void AddNameToJob(const std::wstring& jobFile, const std::wstring& name) {
    if (name.empty()) return;
    std::wstring project;
    std::vector<std::wstring> names;
    if (!AepReadJob(jobFile, &project, &names)) return;
    for (size_t i = 0; i < names.size(); ++i)
        if (_wcsicmp(names[i].c_str(), name.c_str()) == 0) return;

    HANDLE h = CreateFileW(jobFile.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;    // the baker just took it
    std::string line = "\n" + Utf8Of(name);
    DWORD wrote = 0;
    WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &wrote, NULL);
    CloseHandle(h);
}

}  // namespace

bool AepSpoolAndQueue(const unsigned char* data, size_t size,
                      const std::wstring& displayName) {
    // Spooling copies the whole project, so cap it. Anything this large is
    // better handled by pointing aepbake at the real folder.
    if (!data || size < 32 || size > 256ull * 1024 * 1024) return false;

    std::wstring key = AepCacheKeyFromContent(data, size);
    if (key.empty()) return false;
    if (!AepEnsureCacheDirs()) return false;

    std::wstring cached = AepCachedFrameForKey(key);
    if (GetFileAttributesW(cached.c_str()) != INVALID_FILE_ATTRIBUTES) return false;

    std::wstring job = AepQueueDir() + L"\\" + key + L".job";
    if (GetFileAttributesW(job.c_str()) != INVALID_FILE_ATTRIBUTES) {
        AddNameToJob(job, displayName);
        return false;
    }

    std::wstring spool = AepSpoolDir() + L"\\" + key + L".aep";
    if (GetFileAttributesW(spool.c_str()) == INVALID_FILE_ATTRIBUTES) {
        // Write beside, then move into place, so the baker can never pick up a
        // half-written project.
        std::wstring partial = spool + L".part";
        HANDLE h = CreateFileW(partial.c_str(), GENERIC_WRITE, 0, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) return false;

        size_t done = 0;
        bool ok = true;
        while (done < size) {
            DWORD want = static_cast<DWORD>(
                (size - done) > 0x400000 ? 0x400000 : (size - done));
            DWORD wrote = 0;
            if (!WriteFile(h, data + done, want, &wrote, NULL) || wrote == 0) {
                ok = false;
                break;
            }
            done += wrote;
        }
        CloseHandle(h);
        if (!ok) {
            DeleteFileW(partial.c_str());
            return false;
        }
        if (!MoveFileExW(partial.c_str(), spool.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            DeleteFileW(partial.c_str());
            return false;
        }
    }

    return WriteJobFile(key, spool, displayName);
}

std::wstring AepFailMarkerForKey(const std::wstring& key) {
    std::wstring dir = AepCacheDir();
    if (key.empty() || dir.empty()) return std::wstring();
    return dir + L"\\" + key + L".fail";
}

bool AepBakeFailedRecently(const std::wstring& key) {
    std::wstring marker = AepFailMarkerForKey(key);
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (marker.empty() ||
        !GetFileAttributesExW(marker.c_str(), GetFileExInfoStandard, &fad))
        return false;

    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER a, b;
    a.LowPart = now.dwLowDateTime;               a.HighPart = now.dwHighDateTime;
    b.LowPart = fad.ftLastWriteTime.dwLowDateTime; b.HighPart = fad.ftLastWriteTime.dwHighDateTime;
    const ULONGLONG kDay = 24ull * 60 * 60 * 10000000ull;   // FILETIME ticks
    return a.QuadPart >= b.QuadPart && (a.QuadPart - b.QuadPart) < kDay;
}

bool AepAfterEffectsInstalled() {
    HKEY key = NULL;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Adobe\\After Effects", 0,
                      KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
        return false;
    RegCloseKey(key);
    return true;
}

bool AepIsSpooled(const std::wstring& path) {
    std::wstring dir = AepSpoolDir();
    if (dir.empty() || path.size() <= dir.size()) return false;
    return _wcsnicmp(path.c_str(), dir.c_str(), dir.size()) == 0;
}
