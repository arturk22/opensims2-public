#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace os2 {

// One camera from the game's text camera configuration
// (TSData/Res/UserData/Cameras/*.txt, syntax described in cameras.txt).
// Per-zoom-level tables are indexed by zoom level; a table given as a single
// value (e.g. "clip 0.2 2000") holds one entry.
struct CameraDef {
    std::string name;       // e.g. "Live"
    std::string controller; // e.g. "Live", "Build", "SmoothSims1"

    int defaultZoom = 0;
    std::vector<float> distances; // horizontal distance to the pivot per zoom level
    std::vector<float> fov;       // degrees: the full horizontal angle (binary, docs/camera-ui-mac.md 1.1)
    bool verticalFov = false;     // ours: `fov` is the vertical angle instead (the CAS camera's own framing)
    std::vector<float> pitchMin;  // degrees above horizontal
    std::vector<float> pitchMax;
    std::vector<float> nearClip;
    std::vector<float> farClip;

    int defaultOrientation = 0;
    std::vector<float> yaws; // degrees

    int zoomSmoothing = 1; // ">= 1, smaller is more abrupt"
    int spinSmoothing = 1;
    float slope = 0.0f; // dy/dx, used when no pitch range is given
    float wheel = 1.0f; // mouse wheel scaling

    std::map<std::string, float> params; // setparam, lowercased names

    float param(std::string_view name, float fallback) const;
};

// Parses one configuration text. `include`/`sinclude` lines are resolved
// against `dir` (a missing sinclude is ignored, a missing include throws);
// `if ... endif` blocks (expansion-pack version checks) are skipped.
std::vector<CameraDef> parseCameraConfig(std::string_view text, const std::filesystem::path& dir = {});

// Reads `path` and everything it includes.
std::vector<CameraDef> loadCameraConfig(const std::filesystem::path& path);

// Camera by name (case-insensitive), or nullptr.
const CameraDef* findCamera(const std::vector<CameraDef>& cameras, std::string_view name);

} // namespace os2
