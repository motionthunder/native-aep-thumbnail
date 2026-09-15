// Shell thumbnail provider for After Effects project files.
//
// Shows a frame baked by aepbake.exe when one is cached, and a metadata card
// as a placeholder until then. The provider never renders anything itself: an
// .aep holds no pixels at all, only the project graph.
//
// Registration is split. The COM class goes in HKLM, because the shell will
// not activate a thumbnail provider registered only per-user. The .aep
// association goes in HKCU, which shadows the dead handler the Ardfry PSD
// codec leaves on .aep without touching another product's keys.

#include <windows.h>
#include <objbase.h>
#include <propsys.h>
#include <shlobj.h>
#include <thumbcache.h>

#include <new>
#include <string>
#include <vector>

#include "aep.h"
#include "cache.h"
#include "render.h"

// {8E76F525-03F4-403B-A170-1623A5878F14}
static const CLSID CLSID_AepThumbProvider =
    { 0x8E76F525, 0x03F4, 0x403B,
      { 0xA1, 0x70, 0x16, 0x23, 0xA5, 0x87, 0x8F, 0x14 } };

static const wchar_t kClsidText[] = L"{8E76F525-03F4-403B-A170-1623A5878F14}";
static const wchar_t kThumbIface[] = L"{e357fccd-a995-4576-b01f-234630154e96}";
static const wchar_t kFriendlyName[] = L"After Effects Project Thumbnail Provider";
static const wchar_t* kExtensions[] = { L".aep", L".aet" };

static const ULONGLONG kMaxProjectBytes = 384ull * 1024 * 1024;

static HINSTANCE g_module = NULL;
static LONG      g_objects = 0;

static bool QueueHasWork() {
    std::wstring dir = AepQueueDir();
    if (dir.empty()) return false;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.job").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    FindClose(h);
    return true;
}

// Records only things that went wrong. This runs inside the shell's thumbnail
// host, where there is nowhere to report an error to, so a one-line note in
// the cache folder is the only way a problem here is ever visible.
static void ProviderLog(const std::wstring& msg) {
    wchar_t path[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", path, ARRAYSIZE(path));
    if (n == 0 || n >= ARRAYSIZE(path)) return;
    std::wstring file = std::wstring(path) + L"\\AepThumb\\provider.log";

    HANDLE h = CreateFileW(file.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    std::wstring line = msg + L"\r\n";
    int need = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), -1, NULL, 0, NULL, NULL);
    if (need > 1) {
        std::vector<char> utf8(need);
        WideCharToMultiByte(CP_UTF8, 0, line.c_str(), -1, &utf8[0], need, NULL, NULL);
        DWORD wrote = 0;
        WriteFile(h, &utf8[0], static_cast<DWORD>(need - 1), &wrote, NULL);
    }
    CloseHandle(h);
}

// ---------------------------------------------------------------- provider

// IInitializeWithStream is what the shell prefers and what the isolated
// thumbnail host can supply, so implementing it keeps this DLL out of
// explorer.exe. The cost is that we never learn the file's path - which is
// why the cache is keyed on the project's bytes instead.
//
// IInitializeWithFile is kept for the command-line tools, which do have a path
// and can therefore also find a preview sitting next to the project.
class CAepThumbProvider : public IInitializeWithStream,
                          public IInitializeWithFile,
                          public IThumbnailProvider {
public:
    CAepThumbProvider() : refs_(1), ready_(false) {
        InterlockedIncrement(&g_objects);
    }

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) {
        if (!ppv) return E_POINTER;
        *ppv = NULL;
        if (riid == IID_IUnknown || riid == IID_IInitializeWithStream)
            *ppv = static_cast<IInitializeWithStream*>(this);
        else if (riid == IID_IInitializeWithFile)
            *ppv = static_cast<IInitializeWithFile*>(this);
        else if (riid == IID_IThumbnailProvider)
            *ppv = static_cast<IThumbnailProvider*>(this);
        else
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }

    IFACEMETHODIMP_(ULONG) AddRef() {
        return static_cast<ULONG>(InterlockedIncrement(&refs_));
    }

    IFACEMETHODIMP_(ULONG) Release() {
        LONG n = InterlockedDecrement(&refs_);
        if (n == 0) delete this;
        return static_cast<ULONG>(n);
    }

    // IInitializeWithStream - what the shell uses.
    IFACEMETHODIMP Initialize(IStream* stream, DWORD) {
        if (ready_) return E_UNEXPECTED;
        if (!stream) return E_INVALIDARG;
        return ReadFromStream(stream);
    }

    // IInitializeWithFile - used by the command-line tools.
    IFACEMETHODIMP Initialize(LPCWSTR path, DWORD) {
        if (ready_) return E_UNEXPECTED;
        if (!path || !*path) return E_INVALIDARG;

        HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());

        LARGE_INTEGER size;
        if (!GetFileSizeEx(h, &size) || size.QuadPart < 32 ||
            static_cast<ULONGLONG>(size.QuadPart) > kMaxProjectBytes) {
            CloseHandle(h);
            return E_FAIL;
        }

        try {
            data_.resize(static_cast<size_t>(size.QuadPart));
        } catch (...) {
            CloseHandle(h);
            return E_OUTOFMEMORY;
        }

        size_t done = 0;
        while (done < data_.size()) {
            DWORD want = static_cast<DWORD>(
                (data_.size() - done) > 0x400000 ? 0x400000 : (data_.size() - done));
            DWORD got = 0;
            if (!ReadFile(h, &data_[done], want, &got, NULL) || got == 0) {
                CloseHandle(h);
                data_.clear();
                return E_FAIL;
            }
            done += got;
        }
        CloseHandle(h);

        try {
            path_ = path;
        } catch (...) {
            return E_OUTOFMEMORY;
        }
        ready_ = true;
        return S_OK;
    }

    // IThumbnailProvider
    IFACEMETHODIMP GetThumbnail(UINT cx, HBITMAP* phbmp, WTS_ALPHATYPE* alpha) {
        if (!phbmp || !alpha) return E_POINTER;
        *phbmp = NULL;
        *alpha = WTSAT_UNKNOWN;
        if (!ready_ || data_.empty()) return E_UNEXPECTED;

        try {
            AepProject proj;
            if (!AepParse(&data_[0], data_.size(), proj)) return E_FAIL;

            // Keyed on the bytes we were handed, so no filename is needed.
            const std::wstring key = AepCacheKeyFromContent(&data_[0], data_.size());
            std::wstring baked = AepCachedFrameForKey(key);

            if (GetFileAttributesW(baked.c_str()) == INVALID_FILE_ATTRIBUTES) {
                baked.clear();

                // No frame yet but one can be made: ask for it and hand back
                // nothing. Any bitmap returned here - even a placeholder - is
                // written to Explorer's own thumbnail cache and would then be
                // shown instead of the real frame. Failing leaves the standard
                // icon up and caches nothing; once the frame lands the baker
                // tells Explorer to ask again.
                if (AepAfterEffectsInstalled() && !AepBakeFailedRecently(key)) {
                    RequestBake();
                    return E_PENDING;
                }
                // Otherwise there will be no frame - no After Effects, or the
                // project would not render - so the metadata card is the final
                // answer and fine to cache.
            }

            HBITMAP hbm = AepRenderThumbnail(proj, baked, path_, cx);
            if (!hbm) return E_FAIL;

            *phbmp = hbm;
            *alpha = WTSAT_RGB;   // renderer always returns an opaque tile
            return S_OK;
        } catch (...) {
            return E_FAIL;
        }
    }

private:
    ~CAepThumbProvider() { InterlockedDecrement(&g_objects); }

    HRESULT ReadFromStream(IStream* stream) {
        STATSTG st;
        ZeroMemory(&st, sizeof(st));
        // STATFLAG_DEFAULT asks the stream to hand back its name too. The
        // shell does not promise one, so whatever arrives is treated as a hint.
        HRESULT hr = stream->Stat(&st, STATFLAG_DEFAULT);
        if (FAILED(hr)) {
            ZeroMemory(&st, sizeof(st));
            hr = stream->Stat(&st, STATFLAG_NONAME);
        }
        if (FAILED(hr)) return hr;

        // The shell supplies the leaf name but no directory, so it is only good
        // for labelling the baker's log.
        if (st.pwcsName) {
            try { name_ = st.pwcsName; } catch (...) {}
            CoTaskMemFree(st.pwcsName);
            st.pwcsName = NULL;
        }

        ULONGLONG size = st.cbSize.QuadPart;
        if (size < 32 || size > kMaxProjectBytes) return E_FAIL;

        try {
            data_.resize(static_cast<size_t>(size));
        } catch (...) {
            return E_OUTOFMEMORY;
        }

        size_t done = 0;
        while (done < data_.size()) {
            ULONG want = static_cast<ULONG>(
                (data_.size() - done) > 0x400000 ? 0x400000 : (data_.size() - done));
            ULONG got = 0;
            hr = stream->Read(&data_[done], want, &got);
            if (FAILED(hr) || got == 0) {
                data_.clear();
                return FAILED(hr) ? hr : E_FAIL;
            }
            done += got;
        }

        ready_ = true;
        return S_OK;
    }

    // Queues a bake for a project we have only as bytes. Everything here runs
    // inside the isolated thumbnail host, which may write to the user's profile
    // but is not guaranteed to be allowed to start processes; if the spawn is
    // refused the request simply waits for the next baker run.
    void RequestBake() {
        if (data_.empty()) return;
        AepSpoolAndQueue(&data_[0], data_.size(), name_.empty() ? path_ : name_);

        // Start the baker whenever anything is waiting, not only when this call
        // queued something new. A request left behind by an earlier run - the
        // baker was stopped, the machine went to sleep - must not sit in the
        // queue forever just because it was already there.
        if (AepBakerRunning()) return;
        if (!QueueHasWork()) return;

        wchar_t self[MAX_PATH * 2];
        DWORD n = GetModuleFileNameW(g_module, self, ARRAYSIZE(self));
        if (n == 0 || n >= ARRAYSIZE(self)) return;
        std::wstring dir(self, n);
        size_t slash = dir.find_last_of(L"\\/");
        if (slash == std::wstring::npos) return;

        std::wstring cmd = L"\"" + dir.substr(0, slash) + L"\\aepbake.exe\" --daemon";
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
                           CREATE_NO_WINDOW | DETACHED_PROCESS,
                           NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        } else {
            // The request stays queued, so a later scan still picks it up.
            wchar_t msg[96];
            swprintf(msg, 96, L"could not start aepbake.exe (error %lu)",
                     GetLastError());
            ProviderLog(msg);
        }
    }

    LONG                       refs_;
    bool                       ready_;
    std::vector<unsigned char> data_;
    std::wstring               path_;
    std::wstring               name_;
};

// ----------------------------------------------------------- class factory

class CClassFactory : public IClassFactory {
public:
    CClassFactory() : refs_(1) { InterlockedIncrement(&g_objects); }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = NULL;
        return E_NOINTERFACE;
    }

    IFACEMETHODIMP_(ULONG) AddRef() {
        return static_cast<ULONG>(InterlockedIncrement(&refs_));
    }

    IFACEMETHODIMP_(ULONG) Release() {
        LONG n = InterlockedDecrement(&refs_);
        if (n == 0) delete this;
        return static_cast<ULONG>(n);
    }

    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) {
        if (!ppv) return E_POINTER;
        *ppv = NULL;
        if (outer) return CLASS_E_NOAGGREGATION;

        CAepThumbProvider* obj = new (std::nothrow) CAepThumbProvider();
        if (!obj) return E_OUTOFMEMORY;
        HRESULT hr = obj->QueryInterface(riid, ppv);
        obj->Release();
        return hr;
    }

    IFACEMETHODIMP LockServer(BOOL lock) {
        if (lock) InterlockedIncrement(&g_objects);
        else      InterlockedDecrement(&g_objects);
        return S_OK;
    }

private:
    ~CClassFactory() { InterlockedDecrement(&g_objects); }
    LONG refs_;
};

// ------------------------------------------------------------ registration

namespace {

bool SetStringValue(HKEY root, const std::wstring& subkey,
                    const wchar_t* name, const std::wstring& value) {
    HKEY key = NULL;
    LONG rc = RegCreateKeyExW(root, subkey.c_str(), 0, NULL, 0,
                              KEY_SET_VALUE, NULL, &key, NULL);
    if (rc != ERROR_SUCCESS) return false;
    rc = RegSetValueExW(key, name, 0, REG_SZ,
                        reinterpret_cast<const BYTE*>(value.c_str()),
                        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

bool AssociationIsOurs(HKEY root, const std::wstring& subkey) {
    wchar_t value[256];
    DWORD cb = sizeof(value);
    if (RegGetValueW(root, subkey.c_str(), NULL, RRF_RT_REG_SZ, NULL, value, &cb)
            != ERROR_SUCCESS)
        return false;
    return _wcsicmp(value, kClsidText) == 0;
}

// The ProgID carries the AE version (Adobe.AfterEffects.Project.25), so read
// it rather than hard-coding it. The ProgID wins over the extension when the
// shell resolves a handler, so both get registered.
bool ProgIdForExt(const wchar_t* ext, std::wstring& progid) {
    wchar_t buf[256];
    DWORD cb = sizeof(buf);
    if (RegGetValueW(HKEY_CLASSES_ROOT, ext, NULL, RRF_RT_REG_SZ,
                     NULL, buf, &cb) != ERROR_SUCCESS)
        return false;
    if (!buf[0]) return false;
    progid = buf;
    return true;
}

std::wstring ModulePath() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(g_module, buf, ARRAYSIZE(buf));
    return (n == 0 || n >= ARRAYSIZE(buf)) ? std::wstring() : std::wstring(buf);
}

void DeleteKeyIfEmpty(HKEY root, const std::wstring& subkey) {
    HKEY key = NULL;
    if (RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS)
        return;
    DWORD subkeys = 0, values = 0;
    RegQueryInfoKeyW(key, NULL, NULL, NULL, &subkeys, NULL, NULL,
                     &values, NULL, NULL, NULL, NULL);
    RegCloseKey(key);
    if (subkeys == 0 && values == 0) RegDeleteKeyW(root, subkey.c_str());
}

std::vector<std::wstring> HandlerKeys() {
    std::vector<std::wstring> keys;
    for (size_t i = 0; i < ARRAYSIZE(kExtensions); ++i) {
        keys.push_back(std::wstring(L"Software\\Classes\\") + kExtensions[i]);
        std::wstring progid;
        if (ProgIdForExt(kExtensions[i], progid))
            keys.push_back(L"Software\\Classes\\" + progid);
    }
    return keys;
}

}  // namespace

// The split below is deliberate.
//
// The class itself goes in HKLM: the shell does not resolve a thumbnail
// provider through plain CoCreateInstance, and a class registered only under
// HKCU comes back as REGDB_E_CLASSNOTREG from BindToHandler even though an
// ordinary COM client can create it. This step needs elevation.
//
// The .aep association goes in HKCU, which the association layer honours
// (AssocQueryString resolves it). That shadows the dead handler the Ardfry PSD
// codec registered on .aep in HKLM without editing another product's keys.
STDAPI DllRegisterServer() {
    std::wstring dll = ModulePath();
    if (dll.empty()) return E_UNEXPECTED;

    const std::wstring clsKey = std::wstring(L"Software\\Classes\\CLSID\\") + kClsidText;
    if (!SetStringValue(HKEY_LOCAL_MACHINE, clsKey, NULL, kFriendlyName))
        return E_ACCESSDENIED;
    if (!SetStringValue(HKEY_LOCAL_MACHINE, clsKey + L"\\InprocServer32", NULL, dll))
        return E_ACCESSDENIED;
    if (!SetStringValue(HKEY_LOCAL_MACHINE, clsKey + L"\\InprocServer32",
                        L"ThreadingModel", L"Apartment"))
        return E_ACCESSDENIED;

    // Deliberately no DisableProcessIsolation here. This provider initializes
    // from a stream, which the isolated thumbnail host can supply, so it keeps
    // running in dllhost.exe instead of being loaded into explorer.exe.

    // Associations go in HKLM too, so the tool works for everyone on the
    // machine rather than only whoever happened to run the installer. Any
    // handler already claiming .aep is recorded first and put back on
    // uninstall - on this developer's machine that is the Ardfry PSD codec,
    // which claims .aep but cannot produce anything for it.
    const std::wstring backupKey = clsKey + L"\\PreviousHandlers";
    std::vector<std::wstring> keys = HandlerKeys();
    for (size_t i = 0; i < keys.size(); ++i) {
        std::wstring k = keys[i] + L"\\ShellEx\\" + kThumbIface;

        wchar_t prev[256];
        DWORD cb = sizeof(prev);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, k.c_str(), NULL, RRF_RT_REG_SZ,
                         NULL, prev, &cb) == ERROR_SUCCESS &&
            _wcsicmp(prev, kClsidText) != 0) {
            SetStringValue(HKEY_LOCAL_MACHINE, backupKey, keys[i].c_str(), prev);
        }

        if (!SetStringValue(HKEY_LOCAL_MACHINE, k, NULL, kClsidText))
            return E_ACCESSDENIED;
    }

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
    return S_OK;
}

STDAPI DllUnregisterServer() {
    const std::wstring clsKey = std::wstring(L"Software\\Classes\\CLSID\\") + kClsidText;
    const std::wstring backupKey = clsKey + L"\\PreviousHandlers";

    std::vector<std::wstring> keys = HandlerKeys();
    for (size_t i = 0; i < keys.size(); ++i) {
        std::wstring k = keys[i] + L"\\ShellEx\\" + kThumbIface;

        // Only an association that points at us is ours to undo. Unregistering
        // twice - the panic switch followed by uninstall, say - must not delete
        // whichever handler was put back the first time.
        if (AssociationIsOurs(HKEY_LOCAL_MACHINE, k)) {
            wchar_t prev[256];
            DWORD cb = sizeof(prev);
            if (RegGetValueW(HKEY_LOCAL_MACHINE, backupKey.c_str(), keys[i].c_str(),
                             RRF_RT_REG_SZ, NULL, prev, &cb) == ERROR_SUCCESS) {
                SetStringValue(HKEY_LOCAL_MACHINE, k, NULL, prev);
            } else {
                RegDeleteKeyW(HKEY_LOCAL_MACHINE, k.c_str());
                DeleteKeyIfEmpty(HKEY_LOCAL_MACHINE, keys[i] + L"\\ShellEx");
            }
        }

        // Older builds wrote the association per-user; clear that too, under
        // the same rule.
        if (AssociationIsOurs(HKEY_CURRENT_USER, k)) {
            RegDeleteKeyW(HKEY_CURRENT_USER, k.c_str());
            DeleteKeyIfEmpty(HKEY_CURRENT_USER, keys[i] + L"\\ShellEx");
            DeleteKeyIfEmpty(HKEY_CURRENT_USER, keys[i]);
        }
    }

    HKEY roots[2] = { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER };
    for (int i = 0; i < 2; ++i) {
        RegDeleteKeyW(roots[i], backupKey.c_str());
        RegDeleteKeyW(roots[i], (clsKey + L"\\InprocServer32").c_str());
        RegDeleteKeyW(roots[i], clsKey.c_str());
    }

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
    return S_OK;
}

// ----------------------------------------------------------------- exports

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = NULL;
    if (rclsid != CLSID_AepThumbProvider) return CLASS_E_CLASSNOTAVAILABLE;

    CClassFactory* factory = new (std::nothrow) CClassFactory();
    if (!factory) return E_OUTOFMEMORY;
    HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

STDAPI DllCanUnloadNow() {
    return (InterlockedCompareExchange(&g_objects, 0, 0) == 0) ? S_OK : S_FALSE;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = inst;
        DisableThreadLibraryCalls(inst);
    }
    return TRUE;
}
