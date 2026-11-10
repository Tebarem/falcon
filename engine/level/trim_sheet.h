#pragma once
// A trim sheet (.ftrim): one material whose texture is cut into horizontal strips (trims: an edge,
// a bevel's wear, a row of bolts, a panel seam), each a range of v across the texture's full width.
// Faces mapped onto a strip (mesh_ops "trim") show that strip, repeating along u at square texels,
// so walls, frames and bevels get detailed edges from one texture and little geometry.
//
//   {"material": "Assets/Trims/Metal Trims.fmat", "aspect": 1,
//    "strips": [{"name": "Thick edge", "v0": 0, "v1": 0.125}, ...]}

#include <string>
#include <vector>

namespace fx::level {

struct TrimStrip {
    std::string name;
    float v0 = 0.0f, v1 = 1.0f;  // its rows of the texture (0 top, 1 bottom)
};

struct TrimSheet {
    std::string material;   // the .fmat (project-relative) the strips are on
    float aspect = 1.0f;    // the texture's width / height (u's texel size)
    std::vector<TrimStrip> strips;
};

bool readTrimSheet(const std::string& path, TrimSheet& out, std::string& error);
bool writeTrimSheet(const std::string& path, const TrimSheet& sheet, std::string& error);
std::string trimSheetJson(const TrimSheet& sheet);  // (what the file holds)
bool parseTrimSheet(const std::string& json, TrimSheet& out, std::string& error);

// A starter set in `folder` (project-relative, under Assets/): a generated trim texture (metal
// edges, a bevel's wear, bolts, panel seams, a chipped concrete edge, planks, a painted stripe,
// grime), its material and its .ftrim. `sheetPath`: the .ftrim made (project-relative).
bool makeStarterTrims(const std::string& projectDir, const std::string& folder, std::string& sheetPath, std::string& error);

}  // namespace fx::level
