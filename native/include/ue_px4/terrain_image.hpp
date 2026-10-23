#pragma once
#include <opencv2/core/mat.hpp>
#include <string>
#include <cstddef>
namespace dynamic_terrain {
// Decode JPEG through a private C library: Unreal exports an incompatible JPEG ABI.
cv::Mat loadTerrainImage(const std::string& path);
cv::Mat loadTerrainImageBytes(const unsigned char* data, std::size_t size);
}
