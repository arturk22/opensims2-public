#pragma once

// A Sim's head shot rendered from its character file, for the many Maxis
// Sims whose character files carry no portrait IMG: the family panel's
// thumbnails of the game are rendered the same way (cTSUIThumbnailFactorySim,
// docs/ui-live-mac.md 6.2: the Sim is posed in its UI idle and a camera aimed
// at the head renders it). The camera numbers are CAS's (app/cas_mode.cpp,
// CasMode::portrait): inferred, as the factory was not read.

#include "formats/image.hpp"
#include "formats/sim.hpp"

#include <array>
#include <filesystem>
#include <optional>
#include <vector>

namespace os2 {

class Renderer;
class ResourceIndex;

// Renders the Sim of `characterFile` alone, far from the scene (so nothing
// else is in view), on a transparent background; `size` pixels square.
// nullopt when the character cannot be assembled.
std::optional<RgbaImage> renderHeadShot(Renderer& renderer, ResourceIndex& index, const std::vector<SimPart>& parts,
                                        const std::filesystem::path& characterFile, uint32_t size = 128,
                                        const std::filesystem::path& resDir = {});

// A neighborhood object's picture (the Decorations catalog's tiles): the
// renderer's `model` seen from the south-east and above, framed on its local
// bounds (min xyz, max xyz), on a transparent background. Inferred framing.
RgbaImage renderModelThumbnail(Renderer& renderer, int model, const std::array<float, 6>& bounds, uint32_t size = 94);

} // namespace os2
