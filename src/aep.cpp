#include "aep.h"

#include <windows.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <cwctype>

namespace {

inline uint32_t Be32(const unsigned char* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8)  |  uint32_t(p[3]);
}

inline uint16_t Be16(const unsigned char* p) {
    return uint16_t((uint32_t(p[0]) << 8) | p[1]);
}

struct Chunk {
    char   tag[5];      // for a LIST this is the list type, not "LIST"
    bool   isList;
    size_t begin, end;  // payload, past the list type for a LIST
};

// Walks the chunks in [off, end). Stops at the first malformed header rather
// than trusting sizes read from the file.
class ChunkIter {
public:
    ChunkIter(const unsigned char* d, size_t off, size_t end)
        : d_(d), pos_(off), end_(end) {}

    bool Next(Chunk& c) {
        if (pos_ + 8 > end_) return false;
        const unsigned char* h = d_ + pos_;
        uint32_t sz   = Be32(h + 4);
        size_t   body = pos_ + 8;
        if (sz > end_ - body) return false;

        c.isList = std::memcmp(h, "LIST", 4) == 0;
        if (c.isList) {
            if (sz < 4) return false;
            std::memcpy(c.tag, d_ + body, 4);
            c.begin = body + 4;
        } else {
            std::memcpy(c.tag, h, 4);
            c.begin = body;
        }
        c.tag[4] = 0;
        c.end    = body + sz;
        pos_     = body + sz + (sz & 1);   // chunks are word-aligned
        return true;
    }

private:
    const unsigned char* d_;
    size_t pos_, end_;
};

inline bool Is(const Chunk& c, const char* tag) {
    return std::memcmp(c.tag, tag, 4) == 0;
}

std::wstring FromUtf8(const unsigned char* p, size_t n) {
    if (n == 0) return std::wstring();
    if (n > (1u << 20)) n = 1u << 20;
    int need = MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(p),
                                   static_cast<int>(n), NULL, 0);
    if (need <= 0) return std::wstring();
    std::wstring s(static_cast<size_t>(need), 0);
    MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(p),
                        static_cast<int>(n), &s[0], need);
    return s;
}

// Footage references are stored as a JSON blob inside the alas chunk. The
// field we want is "fullpath".
bool ExtractFullPath(const unsigned char* b, size_t n, std::wstring& out) {
    static const char kKey[] = "\"fullpath\"";
    const size_t klen = sizeof(kKey) - 1;
    if (n < klen) return false;

    size_t i = 0;
    for (; i + klen <= n; ++i)
        if (std::memcmp(b + i, kKey, klen) == 0) break;
    if (i + klen > n) return false;

    i += klen;
    while (i < n && b[i] != ':') ++i;
    while (i < n && b[i] != '"') ++i;
    if (i >= n) return false;
    ++i;

    std::string raw;
    raw.reserve(260);
    for (; i < n && b[i] != '"'; ++i) {
        if (b[i] == '\\' && i + 1 < n) {
            ++i;
            char e = static_cast<char>(b[i]);
            if (e == 'n' || e == 'r' || e == 't') raw.push_back(' ');
            else raw.push_back(e);
        } else {
            raw.push_back(static_cast<char>(b[i]));
        }
        if (raw.size() > 4096) return false;
    }
    if (raw.empty()) return false;
    out = FromUtf8(reinterpret_cast<const unsigned char*>(raw.data()), raw.size());
    return !out.empty();
}

// Offsets inside cdta, verified against four unrelated projects (including
// comps sized 796x854, 606x54 and 216x19).
const size_t kCdtaWidth  = 0x8c;   // uint16
const size_t kCdtaHeight = 0x8e;   // uint16
const size_t kCdtaFps    = 0x9c;   // 16.16 fixed point
const size_t kCdtaMin    = 0xa0;

void ParseFolderBody(const unsigned char* d, size_t off, size_t end,
                     int depth, AepProject& out);

void ParseItem(const unsigned char* d, size_t off, size_t end,
               int depth, AepProject& out) {
    std::wstring name;
    const unsigned char* cdta = NULL;
    size_t cdtaLen = 0;
    size_t subBegin = 0, subEnd = 0;
    size_t pinBegin = 0, pinEnd = 0;
    int layers = 0;

    ChunkIter it(d, off, end);
    Chunk c;
    while (it.Next(c)) {
        if (!c.isList && Is(c, "Utf8")) {
            if (name.empty()) name = FromUtf8(d + c.begin, c.end - c.begin);
        } else if (!c.isList && Is(c, "cdta")) {
            cdta = d + c.begin;
            cdtaLen = c.end - c.begin;
        } else if (c.isList && Is(c, "Sfdr")) {
            subBegin = c.begin;
            subEnd = c.end;
        } else if (c.isList && Is(c, "Pin ")) {
            pinBegin = c.begin;
            pinEnd = c.end;
        } else if (c.isList && Is(c, "Layr")) {
            ++layers;
        }
    }

    // A comp carries cdta; footage carries a Pin list; anything else with
    // children is a folder.
    if (cdta && cdtaLen >= kCdtaMin) {
        AepComp comp;
        comp.name   = name;
        comp.width  = Be16(cdta + kCdtaWidth);
        comp.height = Be16(cdta + kCdtaHeight);
        comp.fps    = Be32(cdta + kCdtaFps) / 65536.0;
        comp.layers = layers;
        comp.depth  = depth;
        out.comps.push_back(comp);
    } else if (pinEnd > pinBegin) {
        AepFootage f;
        f.name = name;
        ChunkIter pit(d, pinBegin, pinEnd);
        Chunk pc;
        while (pit.Next(pc)) {
            if (!pc.isList && Is(pc, "alas")) {
                if (ExtractFullPath(d + pc.begin, pc.end - pc.begin, f.path)) break;
            } else if (pc.isList && Is(pc, "Als2")) {
                ChunkIter ait(d, pc.begin, pc.end);
                Chunk ac;
                while (ait.Next(ac)) {
                    if (!ac.isList && Is(ac, "alas") &&
                        ExtractFullPath(d + ac.begin, ac.end - ac.begin, f.path))
                        break;
                }
                if (!f.path.empty()) break;
            }
        }
        if (!f.path.empty()) out.footage.push_back(f);
    } else if (subEnd > subBegin) {
        ++out.folders;
    }

    if (subEnd > subBegin && depth < 32)
        ParseFolderBody(d, subBegin, subEnd, depth + 1, out);
}

void ParseFolderBody(const unsigned char* d, size_t off, size_t end,
                     int depth, AepProject& out) {
    ChunkIter it(d, off, end);
    Chunk c;
    while (it.Next(c)) {
        if (c.isList && Is(c, "Item")) {
            ParseItem(d, c.begin, c.end, depth, out);
            if (out.comps.size() + out.footage.size() > 20000) return;
        }
    }
}

// The XMP packet sits near the end of the file as plain XML.
std::wstring ReadCreatorTool(const unsigned char* d, size_t n) {
    static const char kOpen[]  = "<xmp:CreatorTool>";
    static const char kClose[] = "</xmp:CreatorTool>";
    const size_t olen = sizeof(kOpen) - 1, clen = sizeof(kClose) - 1;
    if (n < olen + clen) return std::wstring();

    for (size_t i = 0; i + olen <= n; ++i) {
        if (std::memcmp(d + i, kOpen, olen) != 0) continue;
        size_t s = i + olen;
        for (size_t j = s; j + clen <= n && j < s + 512; ++j) {
            if (std::memcmp(d + j, kClose, clen) == 0)
                return FromUtf8(d + s, j - s);
        }
        return std::wstring();
    }
    return std::wstring();
}

bool ContainsNoCase(const std::wstring& hay, const wchar_t* needle) {
    std::wstring h = hay;
    for (size_t i = 0; i < h.size(); ++i)
        h[i] = static_cast<wchar_t>(towlower(h[i]));
    return h.find(needle) != std::wstring::npos;
}

}  // namespace

bool AepParse(const unsigned char* data, size_t size, AepProject& out) {
    if (!data || size < 32) return false;
    if (std::memcmp(data, "RIFX", 4) != 0) return false;
    if (std::memcmp(data + 8, "Egg!", 4) != 0) return false;

    ChunkIter it(data, 12, size);
    Chunk c;
    while (it.Next(c)) {
        if (c.isList && Is(c, "Fold")) {
            ParseFolderBody(data, c.begin, c.end, 0, out);
            break;
        }
    }

    out.creatorTool = ReadCreatorTool(data, size);
    return true;
}

const AepComp* AepPickMainComp(const AepProject& proj) {
    if (proj.comps.empty()) return NULL;

    const AepComp* best = NULL;
    long long bestScore = LLONG_MIN;

    for (size_t i = 0; i < proj.comps.size(); ++i) {
        const AepComp& c = proj.comps[i];
        long long score = 0;

        // Render-target names, strongest signal first.
        if      (ContainsNoCase(c.name, L"main"))    score += 1000;
        else if (ContainsNoCase(c.name, L"final"))   score += 900;
        else if (ContainsNoCase(c.name, L"render"))  score += 850;
        else if (ContainsNoCase(c.name, L"master"))  score += 800;
        else if (ContainsNoCase(c.name, L"preview")) score += 600;

        // Rig and helper comps are never what a person wants to look at.
        if (ContainsNoCase(c.name, L"control")  ||
            ContainsNoCase(c.name, L"ctrl")     ||
            ContainsNoCase(c.name, L"guide")    ||
            ContainsNoCase(c.name, L"placeholder") ||
            ContainsNoCase(c.name, L"adjust")   ||
            ContainsNoCase(c.name, L"null"))
            score -= 300;

        // Comps near the project root beat deeply nested precomps.
        score -= static_cast<long long>(c.depth) * 40;

        // Delivery-sized comps beat element and text precomps.
        if (c.width >= 640 && c.height >= 360) score += 200;
        if (c.width < 320 || c.height < 240)   score -= 400;

        score += static_cast<long long>(c.layers) * 3;

        long long area = static_cast<long long>(c.width) * c.height;
        long long areaBonus = area / 100000;
        if (areaBonus > 50) areaBonus = 50;
        score += areaBonus;

        if (score > bestScore) {
            bestScore = score;
            best = &c;
        }
    }
    return best;
}
