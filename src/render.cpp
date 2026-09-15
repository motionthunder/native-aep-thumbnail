#include "render.h"

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace Gdiplus;

namespace {

// --- palette -----------------------------------------------------------

const ARGB kBgTop    = 0xFF1E1830;
const ARGB kBgBottom = 0xFF120E1C;
const ARGB kBorder   = 0xFF3A2E5C;
const ARGB kAccent   = 0xFF9D8CFF;
const ARGB kText     = 0xFFEDEAF7;
const ARGB kDim      = 0xFF9C93B8;

// --- small path helpers ------------------------------------------------

std::wstring DirOf(const std::wstring& p) {
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? std::wstring() : p.substr(0, s + 1);
}

std::wstring FileOf(const std::wstring& p) {
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? p : p.substr(s + 1);
}

std::wstring StemOf(const std::wstring& p) {
    std::wstring f = FileOf(p);
    size_t d = f.find_last_of(L'.');
    return d == std::wstring::npos ? f : f.substr(0, d);
}

std::wstring LowerExtOf(const std::wstring& p) {
    std::wstring f = FileOf(p);
    size_t d = f.find_last_of(L'.');
    if (d == std::wstring::npos) return std::wstring();
    std::wstring e = f.substr(d + 1);
    for (size_t i = 0; i < e.size(); ++i)
        e[i] = static_cast<wchar_t>(towlower(e[i]));
    return e;
}

bool FileSizeOf(const std::wstring& p, unsigned long long* out) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fad)) return false;
    if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return false;
    *out = (static_cast<unsigned long long>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    return true;
}

// Formats GDI+ decodes without extra codecs installed.
bool IsLoadableImage(const std::wstring& path) {
    std::wstring e = LowerExtOf(path);
    return e == L"png" || e == L"jpg" || e == L"jpeg" || e == L"jpe" ||
           e == L"tif" || e == L"tiff" || e == L"bmp" || e == L"gif";
}

const unsigned long long kMaxImageBytes = 64ull * 1024 * 1024;

// --- picking the picture ----------------------------------------------

std::wstring FindSidecar(const std::wstring& aepPath) {
    if (aepPath.empty()) return std::wstring();

    const std::wstring dir  = DirOf(aepPath);
    const std::wstring file = FileOf(aepPath);   // project.aep
    const std::wstring stem = StemOf(aepPath);   // project

    static const wchar_t* kExts[] = { L".png", L".jpg", L".jpeg" };
    std::vector<std::wstring> cand;
    for (int i = 0; i < 3; ++i) cand.push_back(dir + file + kExts[i]);
    for (int i = 0; i < 3; ++i) cand.push_back(dir + stem + kExts[i]);
    for (int i = 0; i < 3; ++i) cand.push_back(dir + L"_previews\\" + stem + kExts[i]);
    for (int i = 0; i < 3; ++i) cand.push_back(dir + L"previews\\" + stem + kExts[i]);

    unsigned long long sz = 0;
    for (size_t i = 0; i < cand.size(); ++i)
        if (FileSizeOf(cand[i], &sz) && sz > 0 && sz <= kMaxImageBytes)
            return cand[i];
    return std::wstring();
}

// The biggest referenced still is the best stand-in for "what is in here".
std::wstring FindFootageImage(const AepProject& proj) {
    std::wstring best;
    unsigned long long bestSize = 0;

    for (size_t i = 0; i < proj.footage.size(); ++i) {
        const std::wstring& p = proj.footage[i].path;
        if (p.empty() || !IsLoadableImage(p)) continue;
        unsigned long long sz = 0;
        if (!FileSizeOf(p, &sz)) continue;      // offline footage
        if (sz == 0 || sz > kMaxImageBytes) continue;
        if (sz > bestSize) {
            bestSize = sz;
            best = p;
        }
    }
    return best;
}

// --- text helpers ------------------------------------------------------

std::wstring FormatFps(double fps) {
    if (fps <= 0.0) return std::wstring();
    wchar_t buf[32];
    if (std::fabs(fps - std::floor(fps + 0.5)) < 0.01) {
        swprintf(buf, 32, L"%d fps", static_cast<int>(std::floor(fps + 0.5)));
        return std::wstring(buf);
    }
    swprintf(buf, 32, L"%.3f", fps);
    std::wstring s(buf);
    while (!s.empty() && s[s.size() - 1] == L'0') s.erase(s.size() - 1);
    if (!s.empty() && s[s.size() - 1] == L'.') s.erase(s.size() - 1);
    return s + L" fps";
}

std::wstring FormatSize(const AepComp* c) {
    if (!c || c->width == 0 || c->height == 0) return std::wstring();
    wchar_t buf[64];
    swprintf(buf, 64, L"%u \x00D7 %u", c->width, c->height);
    return std::wstring(buf);
}

std::wstring ShortCreator(const std::wstring& creator) {
    if (creator.empty()) return std::wstring();
    std::wstring s = creator;
    size_t p = s.find(L" (");                       // drop " (Windows)"
    if (p != std::wstring::npos) s.erase(p);
    if (s.compare(0, 6, L"Adobe ") == 0) s.erase(0, 6);
    return s;
}

std::wstring CountsLine(const AepProject& proj, const AepComp* main) {
    wchar_t buf[128];
    std::wstring s;
    if (main && main->layers > 0) {
        swprintf(buf, 128, L"%d layer%s", main->layers,
                 main->layers == 1 ? L"" : L"s");
        s = buf;
    }
    if (!proj.comps.empty()) {
        unsigned n = static_cast<unsigned>(proj.comps.size());
        swprintf(buf, 128, L"%u comp%s", n, n == 1 ? L"" : L"s");
        if (!s.empty()) s += L" \x00B7 ";
        s += buf;
    }
    if (!proj.footage.empty()) {
        swprintf(buf, 128, L"%u footage",
                 static_cast<unsigned>(proj.footage.size()));
        if (!s.empty()) s += L" \x00B7 ";
        s += buf;
    }
    return s;
}

void AddRoundRect(GraphicsPath* path, const RectF& r, REAL rad) {
    REAL d = rad * 2;
    path->AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
    path->AddArc(r.GetRight() - d, r.Y, d, d, 270.0f, 90.0f);
    path->AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0.0f, 90.0f);
    path->AddArc(r.X, r.GetBottom() - d, d, d, 90.0f, 90.0f);
    path->CloseFigure();
}

const wchar_t* PickFontFamily() {
    FontFamily probe(L"Segoe UI");
    return probe.IsAvailable() ? L"Segoe UI" : L"Arial";
}

void DrawCentredString(Graphics& g, const wchar_t* s, const Font& font,
                       const Brush& brush, const RectF& box) {
    StringFormat sf;
    sf.SetAlignment(StringAlignmentCenter);
    sf.SetLineAlignment(StringAlignmentCenter);
    g.DrawString(s, -1, &font, box, &sf, &brush);
}

// --- drawing -----------------------------------------------------------

void DrawBackdrop(Graphics& g, int w, int h) {
    LinearGradientBrush bg(Point(0, 0), Point(0, h), Color(kBgTop), Color(kBgBottom));
    g.FillRectangle(&bg, 0, 0, w, h);
    Pen border(Color(kBorder), 1.0f);
    g.DrawRectangle(&border, 0, 0, w - 1, h - 1);
}

// Small "Ae" chip so a project tile is never mistaken for a plain image.
void DrawBadge(Graphics& g, const wchar_t* family, REAL x, REAL y, REAL size,
               bool onImage) {
    RectF box(x, y, size * 2.05f, size * 1.5f);
    if (onImage) {
        GraphicsPath path;
        AddRoundRect(&path, box, size * 0.34f);
        SolidBrush back(Color(0xC0000000));
        g.FillPath(&back, &path);
    }
    Font font(family, size, FontStyleBold, UnitPixel);
    SolidBrush ink{Color(kAccent)};
    DrawCentredString(g, L"Ae", font, ink, box);
}

// Outline in the comp's aspect ratio: tells 9:16 from 16:9 at a glance.
void DrawAspectMark(Graphics& g, const AepComp* main, REAL cx, REAL cy, REAL box) {
    if (!main || main->width == 0 || main->height == 0) return;
    double ar = static_cast<double>(main->width) / main->height;
    REAL mw = box, mh = box;
    if (ar >= 1.0) mh = static_cast<REAL>(box / ar);
    else           mw = static_cast<REAL>(box * ar);
    Pen pen(Color(0x809D8CFF), (std::max)(1.0f, box * 0.045f));
    g.DrawRectangle(&pen, cx - mw / 2, cy - mh / 2, mw, mh);
}

void DrawInfoCard(Graphics& g, const wchar_t* family, int w, int h,
                  const AepProject& proj, const AepComp* main) {
    const REAL u   = static_cast<REAL>(w < h ? w : h);
    const REAL pad = (std::max)(4.0f, u * 0.085f);

    DrawBackdrop(g, w, h);

    // Below ~48px no glyph survives; leave a recognisable accent mark.
    if (u < 48) {
        SolidBrush accent{Color(kAccent)};
        REAL bw = (std::max)(2.0f, u * 0.12f);
        g.FillRectangle(&accent, pad, h - pad - bw, w - pad * 2, bw);
        return;
    }

    SolidBrush inkText{Color(kText)};
    SolidBrush inkDim{Color(kDim)};
    SolidBrush inkAccent{Color(kAccent)};

    // 48-96px: wordmark plus the format outline, no prose.
    if (u < 96) {
        Font font(family, u * 0.30f, FontStyleBold, UnitPixel);
        RectF box(0.0f, 0.0f, static_cast<REAL>(w), static_cast<REAL>(h) * 0.62f);
        DrawCentredString(g, L"Ae", font, inkAccent, box);
        DrawAspectMark(g, main, static_cast<REAL>(w) / 2,
                       static_cast<REAL>(h) * 0.78f, u * 0.24f);
        return;
    }

    const bool full = (u >= 168);

    const REAL badge = u * 0.145f;
    DrawBadge(g, family, pad, pad * 0.75f, badge, false);

    // Metadata sits on the bottom edge so every card lines up the same way.
    std::vector<std::wstring> meta;
    std::wstring dim = FormatSize(main);
    std::wstring fps = main ? FormatFps(main->fps) : std::wstring();
    if (full && !dim.empty() && !fps.empty()) dim += L" \x00B7 " + fps;
    if (!dim.empty())      meta.push_back(dim);
    else if (!fps.empty()) meta.push_back(fps);
    if (full) {
        std::wstring counts = CountsLine(proj, main);
        if (!counts.empty()) meta.push_back(counts);
        std::wstring creator = ShortCreator(proj.creatorTool);
        if (!creator.empty()) meta.push_back(creator);
    }

    const REAL metaSize = (std::max)(8.0f, u * (full ? 0.053f : 0.082f));
    const REAL lineH    = metaSize * 1.55f;
    Font metaFont(family, metaSize, FontStyleRegular, UnitPixel);
    StringFormat metaFmt;
    metaFmt.SetTrimming(StringTrimmingEllipsisCharacter);
    metaFmt.SetFormatFlags(StringFormatFlagsNoWrap);

    REAL metaTop = h - pad - lineH * static_cast<REAL>(meta.size());
    for (size_t i = 0; i < meta.size(); ++i) {
        RectF rc(pad, metaTop + lineH * static_cast<REAL>(i), w - pad * 2, lineH);
        bool footnote = (i + 1 == meta.size() && meta.size() > 2);
        g.DrawString(meta[i].c_str(), -1, &metaFont, rc, &metaFmt,
                     footnote ? &inkDim : &inkText);
    }

    // The comp name takes whatever is left between badge and metadata.
    const REAL titleTop    = pad * 0.75f + badge * 1.5f + pad * 0.35f;
    const REAL titleBottom = metaTop - pad * 0.4f;
    if (titleBottom - titleTop < metaSize * 1.1f) return;

    std::wstring title = main ? main->name : std::wstring();
    if (title.empty()) title = L"After Effects project";

    Font titleFont(family, (std::max)(10.0f, u * (full ? 0.090f : 0.105f)),
                   FontStyleBold, UnitPixel);
    StringFormat titleFmt;
    titleFmt.SetTrimming(StringTrimmingEllipsisWord);
    RectF titleRc(pad, titleTop, w - pad * 2, titleBottom - titleTop);
    g.DrawString(title.c_str(), -1, &titleFont, titleRc, &titleFmt, &inkText);
}

// Real pixels are shown plain. The whole point of the baked frame is to see
// what is inside the project, so nothing is drawn over it - no badge, no
// caption. Explorer already prints the file name under the tile.
void DrawImageTile(Graphics& g, int w, int h, Image* img) {
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);

    // Fill behind the image so frames with alpha stay legible.
    LinearGradientBrush bg(Point(0, 0), Point(0, h), Color(kBgTop), Color(kBgBottom));
    g.FillRectangle(&bg, 0, 0, w, h);
    g.DrawImage(img, Rect(0, 0, w, h));

    Pen border(Color(0x66000000), 1.0f);
    g.DrawRectangle(&border, 0, 0, w - 1, h - 1);
}

// --- bitmap plumbing ---------------------------------------------------

HBITMAP CreateTopDownDib(int w, int h, void** bits) {
    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;          // top-down
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    return CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, bits, NULL, 0);
}

void ForceOpaque(void* bits, int w, int h) {
    unsigned char* p = static_cast<unsigned char*>(bits);
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
    for (size_t i = 0; i < n; ++i) p[i * 4 + 3] = 0xFF;
}

void FitInto(UINT cx, UINT srcW, UINT srcH, int* outW, int* outH) {
    if (srcW == 0 || srcH == 0) {
        *outW = *outH = static_cast<int>(cx);
        return;
    }
    double s = (std::min)(static_cast<double>(cx) / srcW,
                          static_cast<double>(cx) / srcH);
    if (s > 1.0) s = 1.0;                     // never upscale a real image
    int w = static_cast<int>(srcW * s + 0.5);
    int h = static_cast<int>(srcH * s + 0.5);
    *outW = (std::max)(1, w);
    *outH = (std::max)(1, h);
}

}  // namespace

HBITMAP AepRenderThumbnail(const AepProject& proj, const std::wstring& bakedFrame,
                           const std::wstring& aepPath, UINT cx) {
    if (cx == 0) return NULL;
    if (cx > 4096) cx = 4096;

    GdiplusStartupInput gsi;
    ULONG_PTR token = 0;
    if (GdiplusStartup(&token, &gsi, NULL) != Ok) return NULL;

    HBITMAP result = NULL;
    {
        const wchar_t* family = PickFontFamily();
        const AepComp* main = AepPickMainComp(proj);

        // A frame baked by After Effects is the only thing that really shows
        // what is inside the project, so it wins over everything else.
        std::wstring imgPath;
        unsigned long long sz = 0;
        if (!bakedFrame.empty() && FileSizeOf(bakedFrame, &sz) && sz > 0)
            imgPath = bakedFrame;
        if (imgPath.empty()) imgPath = FindSidecar(aepPath);
        if (imgPath.empty()) imgPath = FindFootageImage(proj);

        Image* img = NULL;
        if (!imgPath.empty()) {
            Image* candidate = Image::FromFile(imgPath.c_str(), FALSE);
            if (candidate) {
                if (candidate->GetLastStatus() == Ok &&
                    candidate->GetWidth() > 0 && candidate->GetHeight() > 0) {
                    img = candidate;
                } else {
                    delete candidate;
                }
            }
        }

        // An image keeps its own aspect; a metadata card is square, because a
        // 16:9 card leaves no room for text at the sizes Explorer asks for.
        int w, h;
        if (img) FitInto(cx, img->GetWidth(), img->GetHeight(), &w, &h);
        else     w = h = static_cast<int>(cx);

        void* bits = NULL;
        HBITMAP hbm = CreateTopDownDib(w, h, &bits);
        if (hbm && bits) {
            HDC dc = CreateCompatibleDC(NULL);
            if (dc) {
                HGDIOBJ old = SelectObject(dc, hbm);
                {
                    Graphics g(dc);
                    g.SetSmoothingMode(SmoothingModeAntiAlias);
                    g.SetTextRenderingHint(TextRenderingHintAntiAlias);
                    if (img) DrawImageTile(g, w, h, img);
                    else     DrawInfoCard(g, family, w, h, proj, main);
                    g.Flush(FlushIntentionSync);
                }
                SelectObject(dc, old);
                DeleteDC(dc);
                GdiFlush();
                ForceOpaque(bits, w, h);
                result = hbm;
            } else {
                DeleteObject(hbm);
            }
        } else if (hbm) {
            DeleteObject(hbm);
        }

        delete img;
    }

    GdiplusShutdown(token);
    return result;
}
