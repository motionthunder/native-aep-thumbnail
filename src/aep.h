// AEP (After Effects project) structure reader.
//
// An .aep is a big-endian RIFF container: "RIFX" <size> "Egg!" followed by
// chunks. The project tree lives in the top-level LIST:Fold. There is no
// raster preview anywhere in the file, so everything here is metadata.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct AepComp {
    std::wstring name;
    uint32_t     width  = 0;
    uint32_t     height = 0;
    double       fps    = 0.0;
    int          layers = 0;
    int          depth  = 0;   // nesting depth in the project folder tree
};

struct AepFootage {
    std::wstring name;
    std::wstring path;         // absolute, as stored by AE
};

struct AepProject {
    std::vector<AepComp>    comps;
    std::vector<AepFootage> footage;
    std::wstring            creatorTool;   // e.g. "Adobe After Effects 2025 (Windows)"
    int                     folders = 0;
};

// Returns false if the buffer is not a RIFX/Egg! project.
bool AepParse(const unsigned char* data, size_t size, AepProject& out);

// Best guess at the comp a human would call the project's main one.
// Returns nullptr when the project has no comps.
const AepComp* AepPickMainComp(const AepProject& proj);
