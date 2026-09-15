// Test harness for the AEP parser and thumbnail renderer.
//
//   aepinfo <project.aep> [-o out.png] [-s 512]
//
// Prints what the parser found and, with -o, writes the thumbnail it would
// hand to Explorer. Lets the renderer be checked without registering
// anything with the shell.

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <cstdio>
#include <string>
#include <vector>

#include "../src/aep.h"
#include "../src/cache.h"
#include "../src/render.h"

using namespace Gdiplus;

namespace {

bool ReadWholeFile(const wchar_t* path, std::vector<unsigned char>& out) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
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
        DWORD chunk = static_cast<DWORD>(
            (out.size() - done) > 0x400000 ? 0x400000 : (out.size() - done));
        DWORD got = 0;
        if (!ReadFile(h, &out[done], chunk, &got, NULL) || got == 0) {
            CloseHandle(h);
            return false;
        }
        done += got;
    }
    CloseHandle(h);
    return true;
}

bool GetEncoderClsid(const wchar_t* mime, CLSID* out) {
    UINT count = 0, bytes = 0;
    if (GetImageEncodersSize(&count, &bytes) != Ok || bytes == 0) return false;
    std::vector<unsigned char> buf(bytes);
    ImageCodecInfo* info = reinterpret_cast<ImageCodecInfo*>(&buf[0]);
    if (GetImageEncoders(count, bytes, info) != Ok) return false;
    for (UINT i = 0; i < count; ++i) {
        if (wcscmp(info[i].MimeType, mime) == 0) {
            *out = info[i].Clsid;
            return true;
        }
    }
    return false;
}

bool SavePng(HBITMAP hbm, const wchar_t* path) {
    GdiplusStartupInput gsi;
    ULONG_PTR token = 0;
    if (GdiplusStartup(&token, &gsi, NULL) != Ok) return false;

    bool ok = false;
    {
        Bitmap* bmp = Bitmap::FromHBITMAP(hbm, NULL);
        if (bmp) {
            CLSID png;
            if (GetEncoderClsid(L"image/png", &png))
                ok = (bmp->Save(path, &png, NULL) == Ok);
            delete bmp;
        }
    }
    GdiplusShutdown(token);
    return ok;
}

void PrintProject(const AepProject& proj, const AepComp* main) {
    wprintf(L"creator : %s\n",
            proj.creatorTool.empty() ? L"(none)" : proj.creatorTool.c_str());
    wprintf(L"folders : %d\n", proj.folders);
    wprintf(L"comps   : %u\n", static_cast<unsigned>(proj.comps.size()));
    wprintf(L"footage : %u\n\n", static_cast<unsigned>(proj.footage.size()));

    size_t shown = proj.comps.size() > 20 ? 20 : proj.comps.size();
    for (size_t i = 0; i < shown; ++i) {
        const AepComp& c = proj.comps[i];
        wprintf(L"  %s[comp] %-32.32s %5ux%-5u %7.3f fps  %3d layers%s\n",
                (&c == main) ? L"* " : L"  ",
                c.name.c_str(), c.width, c.height, c.fps, c.layers,
                (&c == main) ? L"   <- main" : L"");
    }
    if (proj.comps.size() > shown)
        wprintf(L"  ... %u more comps\n", static_cast<unsigned>(proj.comps.size() - shown));

    if (!proj.footage.empty()) wprintf(L"\n");
    size_t fshown = proj.footage.size() > 10 ? 10 : proj.footage.size();
    for (size_t i = 0; i < fshown; ++i) {
        DWORD attr = GetFileAttributesW(proj.footage[i].path.c_str());
        wprintf(L"  [foot] %-28.28s %s  %s\n",
                proj.footage[i].name.c_str(),
                (attr == INVALID_FILE_ATTRIBUTES) ? L"OFFLINE" : L"online ",
                proj.footage[i].path.c_str());
    }
    if (proj.footage.size() > fshown)
        wprintf(L"  ... %u more footage items\n",
                static_cast<unsigned>(proj.footage.size() - fshown));
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        wprintf(L"usage: aepinfo <project.aep> [-o out.png] [-s size]\n");
        return 2;
    }

    const wchar_t* inPath  = argv[1];
    const wchar_t* outPath = NULL;
    UINT size = 512;

    for (int i = 2; i < argc; ++i) {
        if (wcscmp(argv[i], L"-o") == 0 && i + 1 < argc)      outPath = argv[++i];
        else if (wcscmp(argv[i], L"-s") == 0 && i + 1 < argc) size = static_cast<UINT>(_wtoi(argv[++i]));
    }

    std::vector<unsigned char> data;
    if (!ReadWholeFile(inPath, data)) {
        wprintf(L"error: cannot read %s\n", inPath);
        return 1;
    }

    AepProject proj;
    if (!AepParse(&data[0], data.size(), proj)) {
        wprintf(L"error: %s is not a RIFX/Egg! After Effects project\n", inPath);
        return 1;
    }

    const AepComp* main = AepPickMainComp(proj);
    wprintf(L"file    : %s (%u bytes)\n", inPath, static_cast<unsigned>(data.size()));
    PrintProject(proj, main);

    std::wstring key = AepCacheKeyFromContent(&data[0], data.size());
    std::wstring baked = AepCachedFrameForKey(key);
    bool haveBaked = GetFileAttributesW(baked.c_str()) != INVALID_FILE_ATTRIBUTES;
    wprintf(L"\ncache key : %s  (%s)\n", key.c_str(),
            haveBaked ? L"baked frame present" : L"not baked yet");

    if (outPath) {
        HBITMAP hbm = AepRenderThumbnail(proj, haveBaked ? baked : std::wstring(),
                                         inPath, size);
        if (!hbm) {
            wprintf(L"\nerror: render failed\n");
            return 1;
        }
        BITMAP bm;
        GetObject(hbm, sizeof(bm), &bm);
        bool ok = SavePng(hbm, outPath);
        DeleteObject(hbm);
        wprintf(L"\nthumbnail: %ldx%ld -> %s (%s)\n", bm.bmWidth, bm.bmHeight,
                outPath, ok ? L"ok" : L"save failed");
        if (!ok) return 1;
    }
    return 0;
}
