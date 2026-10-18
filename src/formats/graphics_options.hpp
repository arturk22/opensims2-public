#pragma once

// The engine side of the game's graphics options (Config/Graphics Rules.sgr,
// the Graphics window 0x49060F03 of the options panel): the properties of
// the option settings this engine honours, process wide. A change takes
// effect for what is loaded after it (the game too applies them on "Apply
// Changes", and models already loaded keep their parts).
//
//   Shadows        $Low: guob false, heightMapShadows false; $Medium and
//                  $High: both true; $High also simShadows (the Sims'
//                  shadows) and objectShadows (not drawn by this engine)
//   Reflection     $Off/$On: reflectionWithExtraViewer, nhoodWaterReflection
//   LightingQuality $Low: specHighlights false; $Medium/$High: true
//   MaterialDetail $Low/$Medium: bumpMapping false; $High: true (normal maps)
//
// Plain C++.

namespace os2 {

struct GraphicsOptions {
    bool guob = true;                       // objects' ground shadow planes ("groundshadow", "*wallshadow")
    bool heightMapShadows = true;           // the neighborhood terrain's height-map shadows
    bool simShadows = true;                 // the Sims' shadows (nTSSG::cShadow style 1)
    bool reflectionWithExtraViewer = true;  // mirrors (cViewerRefNodeRecursive "screenreflection")
    bool nhoodWaterReflection = true;
    bool specHighlights = true;
    bool bumpMapping = true; // MaterialDetail $High (not in the options window yet)
    // causticsEnabled: registered by cTSConfigMaterialSettings with true
    // (base 1.0 FUN_001ab11c); Graphics Rules.sgr turns it off only on the
    // weakest cards (docs/roof-fence-water-mac.md 3.3). The pool's caustics.
    bool caustics = true;
};

inline GraphicsOptions& graphicsOptions() {
    static GraphicsOptions options;
    return options;
}

} // namespace os2
