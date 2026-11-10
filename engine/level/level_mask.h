#pragma once
// A layered level mesh's painted mask (level_meshes.h LevelMask): kMaskSize² RGBA8, r g b the
// weights of its layers 1..3, a the wear. Saved in the scene's edits as base64 of a PNG.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fx::level {

inline constexpr uint32_t kMaskSize = 512;
inline constexpr size_t kMaskBytes = size_t(kMaskSize) * kMaskSize * 4;

std::string maskToText(const std::vector<uint8_t>& rgba);
bool maskFromText(std::string_view text, std::vector<uint8_t>& rgba);  // false: not a mask (left empty)
// A copy of `from` (empty: all zero) with channel `channel` (0..3) set to `value` (0..1) everywhere.
std::vector<uint8_t> maskFilled(const std::vector<uint8_t>* from, uint32_t channel, float value);

}  // namespace fx::level
