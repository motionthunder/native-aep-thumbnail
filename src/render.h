#pragma once

#include <windows.h>

#include <string>

#include "aep.h"

// Renders a thumbnail that fits inside cx by cx and returns an opaque 32bpp
// top-down DIB, or NULL on failure. Ownership passes to the caller.
//
// Source of the picture, in order of preference:
//   1. bakedFrame - a frame After Effects rendered for this project, which is
//      the only thing that really shows what is inside it,
//   2. a rendered preview sitting next to the project (needs aepPath),
//   3. the largest still image the project references,
//   4. a card drawn from the project metadata, as a placeholder.
//
// Both paths may be empty. Inside the shell only bakedFrame is available,
// because a thumbnail provider running in the isolated host is given a stream
// rather than a filename.
HBITMAP AepRenderThumbnail(const AepProject& proj,
                           const std::wstring& bakedFrame,
                           const std::wstring& aepPath,
                           UINT cx);
