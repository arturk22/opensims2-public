#pragma once

// Create-a-Sim in the app (--cas), from ui.package's scripts as
// docs/cas-mac.md §1 describes them, drawn with the UI layer over the Sim
// being made, which is rebuilt (a character file written by
// formats/character_write and assembled by loadCharacterSim) whenever an
// option changes.
//
//   CB980E50  the CAS frame: the puck (accept family 0x122, exit 0x105, zoom
//             0x2D486AC2/ACD, turn the Sim 0x2CAFB772/0x4CAFB7AA), the
//             household bar (family name 0x6C6AE0BD), "CreateSim Controls"
//             (the six step buttons 0x8C6B0000-05, DONE 0xEC6B2109, cancel
//             0xED012A1C, undo/redo 0x8D24D87A/8D, the first name), the
//             "rotation window" (dragging there turns the Sim) and the
//             anchors the step panels hang from ("sub panel anchor"
//             0x2D050914: each panel's hidden 0xAAAA window says
//             "2d050914:bl", its bottom-left corner goes there)
//   CB980E81  step 1: first name, gender, age, skin tone, body shape, dice
//   CB980E88  step 2: the head bin (archetypes)
//   CB980E82  step 3: hair (style grid and colour) and face: full face,
//             brow, eyes, nose, mouth, jaw panels, each an archetype region
//             bin (kCAFArchetypeRegionMask) and modifier sliders
//             (kCAFModifiersValidGroup), the eyes' colour overlays
//   CB980E85  step 4: makeup (blush, eyeliner, eye shadow, lipstick),
//             costume makeup, glasses, brows, stubble, facial hair
//   CB980E84  step 5: clothes by category (everyday, formal, undies, PJs,
//             swimwear, athletic) and part (top, bottom, whole outfit)
//   CB980E83  step 6: aspiration, zodiac and the five traits (25 points)
//
// The grids show the game's thumbnails (UserData/Thumbnails/
// CASThumbnails.package, keyed as formats/cas_thumbnails says); the
// overlays it has none of (eyes, makeup) show their texture over the face
// skin, and entries without either their name. Which tab
// button opens which face panel is inferred from their order (hair, full
// face, brow, eyes, nose, mouth, jaw, as the panels' titles go).
//
// DONE adds the Sim to the family and shows the family; the puck's accept
// button writes every Sim's character file and, with --cas-hood <N001>
// (or a neighborhood folder), the family into a copy of the neighborhood
// (vm/family_write) under --cas-out (default: the user's save folder,
// never data/game). Headless: --cas-step N, --cas-tab N, --cas-category N,
// --cas-part N, --cas-name First,Last, --cas-random seed, --click x,y.

#include "formats/cas_catalog.hpp"
#include "formats/cas_face.hpp"
#include "formats/cas_personality.hpp"
#include "formats/character_write.hpp"
#include "formats/image.hpp"
#include "formats/ui_screen.hpp"
#include "formats/ui_script.hpp"

#include <simd/simd.h>

#include <array>
#include <cmath>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace MTL {
class Device;
class RenderCommandEncoder;
} // namespace MTL

namespace os2 {

namespace cas {
class CasThumbnails;
}
class CasStudio;
class FontLibrary;
class LotCamera;
class Renderer;
class ResourceIndex;
class UiLayer;
class UiResources;
struct SimActor;

class CasMode {
public:
    CasMode(Renderer& renderer, ResourceIndex& index, const std::filesystem::path& gameDir,
            std::vector<std::string> args);
    ~CasMode();

    std::unique_ptr<LotCamera> makeCamera();
    void setCamera(LotCamera* camera) { camera_ = camera; }
    // Poses the Sims (seconds since start).
    void update(double seconds);

    void attach(MTL::Device* device, uint32_t colorFormat, uint32_t depthFormat);
    void encode(MTL::RenderCommandEncoder* enc, uint32_t width, uint32_t height, float scale);
    void setCanvas(int width, int height);

    // Mouse in canvas pixels (origin top left).
    void mouseDown(simd_float2 p);
    void mouseDragged(simd_float2 p);
    void mouseUp(simd_float2 p);
    // A key: macOS virtual key code and its characters.
    void keyDown(unsigned short keyCode, const std::string& characters, bool control);
    void keyUp(unsigned short keyCode);

    // When opened for a neighborhood (the game flow): called after Accept
    // wrote the family into it (its family number), and by the puck's Exit.
    std::function<void(uint32_t family)> onFamilySaved;
    std::function<void()> onExit;

    // Edits a saved Sim: a character file (group 0xFFFFFFFF) or a premade
    // Sim of the bin (its group), through cas::readCharacterSpec; false (and
    // a status line) if it cannot be read.
    bool openCharacter(const std::filesystem::path& file, uint32_t group = 0xFFFFFFFF);

    // The --cas-* options of a headless run, before the screenshot.
    void runScripted(const std::vector<std::string>& args);
    std::vector<std::string> report() const;
    std::string title() const;

    struct Member {
        cas::CharacterSpec spec;
        cas::Personality personality;
        uint32_t aspiration = 0;
        bool zodiacChosen = false;
        int zodiac = 1;
        uint32_t thumbnail = 0; // CASThumbnails head id (premade Sims), 0 if none
        std::shared_ptr<RgbaImage> portrait; // a head shot rendered for the family bar
        int id = 0;                          // stable within the session (the family tree)
        std::vector<int> parents;            // ids of the family's members that are its parents
        // The family tree from this Sim's side: other member's id -> tie
        // (formats/neighborhood.hpp TieType: 0/1 my parent is, 2 spouse, 3
        // sibling, 4 my child is); no entry = roommates.
        std::map<int, uint32_t> ties;
    };

private:
    // The premade Sim bin (CB980E86): the Sims of Catalog/Bins/
    // globalcatbin.bundle.package (one BINX per Sim, objectidx -> its AGED).
    struct BinSim {
        uint32_t group = 0, age = 0, gender = 0;
        int sort = 0;
        uint32_t thumbnail = 0;
    };
    void loadBin();
    // A head shot of a Sim (the renderer's capture of it alone, far from
    // the studio), for the family bar and the Make a Child panel.
    std::shared_ptr<RgbaImage> portrait(const Member& m, uint32_t size = 256, float closeness = 1.0f);
    // Head shots of the archetypes the CAS thumbnail cache has none of (the
    // alien), made when the face step opens, by age, gender and family.
    void ensureArchetypeShots();
    std::map<std::string, std::shared_ptr<RgbaImage>> archetypeShots_;
    int portraitInstance_ = -1;
    bool capturing_ = false;
    int nextId_ = 1;
    // Make a Child (CB980E87): two of the family's grown-ups, the child's
    // age and gender (0 = by the genes' rule), the child made.
    void makeChild();
    bool mabOpen_ = false;
    int mabParent_[2] = {-1, -1};
    uint32_t mabAge_ = 0x01, mabGender_ = 0;
    std::optional<Member> mabChild_;
    // The family tree dialog (2C054EF0) and its "Make %s a ..." popup
    // (2C054EF3): pick a member, then another, then the tie.
    bool treeOpen_ = false;
    int treeFirst_ = -1, treeSecond_ = -1, treeChoice_ = -1;
    void setTie(int a, int b, int choice); // choice: 0 roomie, 1 sibling, 2 spouse, 3 child of, 4 parent of
    std::vector<BinSim> bin_;
    bool binLoaded_ = false, binOpen_ = false;
    int binSel_ = -1;
    uint32_t binAge_ = 0, binGender_ = 0; // filters, 0 = all
    std::filesystem::path binFile_;

    // Rebuilds the edited Sim's model (and the camera height for its age).
    void rebuild();
    void setStep(int step);
    void newSim();
    void randomize(uint32_t seed);
    void finishSim();
    void acceptFamily();
    void pushUndo();
    void undo();
    void redo();
    void changed(); // after an edit: undo state and rebuild
    UiScreen screen(int width, int height);
    void applyCamera(bool smooth);
    float simHeight() const;

    Renderer& renderer_;
    ResourceIndex& index_;
    std::filesystem::path gameDir_;
    std::vector<std::string> args_;
    std::unique_ptr<cas::CasCatalog> catalog_;
    std::unique_ptr<cas::FaceBaker> baker_;
    std::unique_ptr<UiResources> res_;
    std::unique_ptr<FontLibrary> fonts_;
    std::unique_ptr<cas::CasThumbnails> thumbs_;
    std::unique_ptr<CasStudio> studio_; // the studio lot, null with --cas-no-studio
    // Where the Sims stand and the way they face (towards the camera): the
    // podium for the edited Sim, the backdrop for the family; the origin
    // facing +Y without the studio.
    simd_float3 spot() const;
    simd_float2 facing() const;
    size_t familyMark(size_t member) const; // index into the studio's family marks
    // The heading that turns the skeleton's +Y to facing().
    float baseHeading() const {
        const simd_float2 f = facing();
        return std::atan2(-f.x, f.y);
    }
    std::map<std::string, std::unique_ptr<RgbaImage>> previews_; // overlay previews (face|texture)
    std::map<uint32_t, UiScript> scripts_;
    LotCamera* camera_ = nullptr;

    // The family and the Sim being edited (-1: none, the family view).
    std::vector<Member> family_;
    std::string familyName_;
    Member edit_;
    int editing_ = -1; // index in family_ of the Sim edited, family_.size() for a new one
    std::vector<Member> undo_, redo_;

    // Panels.
    int step_ = 0;        // 0..5
    int faceTab_ = 1;     // CB980E82: 0 hair, 1 full face, 2 brow, 3 eyes, 4 nose, 5 mouth, 6 jaw
    int cosmeticTab_ = 0; // CB980E85: 0 makeup, 1 costume, 2 glasses, 3 brows, 4 stubble, 5 facial hair
    int clothesCategory_ = 0, clothesPart_ = 0;
    std::map<std::string, int> pages_; // grid key -> page
    enum class Focus { None, FirstName, FamilyName } focus_ = Focus::None;
    std::string status_;

    // The Sims drawn: the edited one, or the family's side by side.
    struct Shown {
        std::unique_ptr<SimActor> actor;
        int instance = -1;
    };
    std::vector<Shown> shown_;
    float heading_ = 0; // the edited Sim's turn (radians)
    int turning_ = 0;   // , and . held
    bool dragging_ = false;
    simd_float2 dragFrom_{0, 0};
    double lastUpdate_ = 0;
    int zoom_ = 0; // camera: 0 whole body, 1 upper body, 2 face
    std::filesystem::path tempDir_;
    int rebuilds_ = 0;
    std::vector<std::string> warnings_;

    // UI targets of the last layout.
    struct Target {
        std::array<float, 4> rect;
        std::function<void(simd_float2)> action;
    };
    std::vector<Target> targets_;
    std::vector<std::array<float, 4>> solid_;
    std::array<float, 4> rotationRect_{0, 0, 0, 0};
    std::vector<std::unique_ptr<RgbaImage>> keep_;
    int canvasW_ = 1024, canvasH_ = 768, laidW_ = 0, laidH_ = 0;
    bool dirty_ = true;
    std::array<std::unique_ptr<UiLayer>, 4> layers_;
    size_t layer_ = 0;
};

} // namespace os2
