// Create-a-Sim's family (cas_mode.hpp): the family tree's ties, Make a
// Child, the premade Sim bin, a saved Sim opened, DONE adding the Sim and
// the puck's accept writing the family, and CasMode::screen's family view
// with its panels (the Sim bin, the family tree, Make a Child).

#include "app/cas_mode_internal.hpp"
#include "formats/log_once.hpp"

#include "app/ui_panel_util.hpp"
#include "formats/cas_thumbnails.hpp"
#include "formats/dbpf.hpp"
#include "formats/property_set.hpp"
#include "formats/resindex.hpp"
#include "formats/sim_character.hpp"
#include "formats/ui_resources.hpp"
#include "render/camera.hpp"
#include "vm/family_write.hpp"
#include "vm/genetics.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>

namespace os2 {

namespace fs = std::filesystem;

namespace {

// The family photo: the members' head shots side by side (two rows from
// four Sims on) over a soft blue backdrop, 512 x 512 like the shipped ones.
RgbaImage composeFamilyPhoto(const std::vector<const RgbaImage*>& heads) {
    constexpr uint32_t kSize = 512;
    RgbaImage out;
    out.width = out.height = kSize;
    out.pixels.resize(size_t(kSize) * kSize * 4);
    for (uint32_t y = 0; y < kSize; ++y) {
        const float t = float(y) / float(kSize - 1);
        for (uint32_t x = 0; x < kSize; ++x) {
            uint8_t* px = &out.pixels[(size_t(y) * kSize + x) * 4];
            px[0] = uint8_t(150 - 50 * t), px[1] = uint8_t(180 - 50 * t), px[2] = uint8_t(225 - 40 * t), px[3] = 255;
        }
    }
    const size_t n = heads.size();
    if (!n)
        return out;
    const size_t cols = n <= 3 ? n : (n + 1) / 2, rows = n <= 3 ? 1 : 2;
    const float stride = std::min(float(kSize) / float(cols), float(kSize) / float(rows));
    // One row: the shots overlap a little, so the family fills the picture.
    const float cell = rows == 1 ? std::min(float(kSize), stride * 1.3f) : stride;
    for (size_t i = 0; i < n; ++i) {
        const RgbaImage& h = *heads[i];
        if (!h.width || !h.height)
            continue;
        const size_t inRow = (i / cols + 1 == rows) ? n - (rows - 1) * cols : cols;
        const float x0 = (float(kSize) - float(inRow) * stride) / 2 + float(i % cols) * stride - (cell - stride) / 2;
        const float y0 = (float(kSize) - float(rows) * stride) / 2 + float(i / cols) * stride - (cell - stride) / 2 +
                          (rows == 1 ? 0.12f * cell : 0.0f);
        for (int dy = 0; dy < int(cell); ++dy)
            for (int dx = 0; dx < int(cell); ++dx) {
                const int ox = int(x0) + dx, oy = int(y0) + dy;
                if (ox < 0 || oy < 0 || ox >= int(kSize) || oy >= int(kSize))
                    continue;
                const float sx = (float(dx) + 0.5f) * float(h.width) / cell - 0.5f;
                const float sy = (float(dy) + 0.5f) * float(h.height) / cell - 0.5f;
                const int ix = std::clamp(int(std::floor(sx)), 0, int(h.width) - 1);
                const int iy = std::clamp(int(std::floor(sy)), 0, int(h.height) - 1);
                const int jx = std::min(ix + 1, int(h.width) - 1), jy = std::min(iy + 1, int(h.height) - 1);
                const float fx = std::clamp(sx - float(ix), 0.0f, 1.0f), fy = std::clamp(sy - float(iy), 0.0f, 1.0f);
                auto at = [&](int x, int y, int c) { return float(h.pixels[(size_t(y) * h.width + size_t(x)) * 4 + size_t(c)]); };
                uint8_t* px = &out.pixels[(size_t(oy) * kSize + size_t(ox)) * 4];
                const float a = (at(ix, iy, 3) * (1 - fx) + at(jx, iy, 3) * fx) * (1 - fy) +
                                (at(ix, jy, 3) * (1 - fx) + at(jx, jy, 3) * fx) * fy;
                for (int c = 0; c < 3; ++c) {
                    const float v = (at(ix, iy, c) * (1 - fx) + at(jx, iy, c) * fx) * (1 - fy) +
                                    (at(ix, jy, c) * (1 - fx) + at(jx, jy, c) * fx) * fy;
                    px[c] = uint8_t(std::lround(float(px[c]) * (1 - a / 255) + v * a / 255));
                }
            }
    }
    return out;
}

} // namespace

void CasMode::setTie(int a, int b, int choice) {
    if (a < 0 || b < 0 || a == b || size_t(std::max(a, b)) >= family_.size())
        return;
    Member& A = family_[size_t(a)];
    Member& B = family_[size_t(b)];
    if (!A.id)
        A.id = nextId_++;
    if (!B.id)
        B.id = nextId_++;
    A.ties.erase(B.id);
    B.ties.erase(A.id);
    auto parentSlot = [&](const Member& child) {
        for (const auto& [o, t] : child.ties)
            if (t == 0)
                return 1u; // the other parent
        return 0u;
    };
    switch (choice) {
    case 1: A.ties[B.id] = 3, B.ties[A.id] = 3; break; // siblings
    case 2: A.ties[B.id] = 2, B.ties[A.id] = 2; break; // spouses
    case 3: A.ties[B.id] = parentSlot(A), B.ties[A.id] = 4; break; // A a child of B
    case 4: B.ties[A.id] = parentSlot(B), A.ties[B.id] = 4; break; // A a parent of B
    default: break;                                                  // roommates
    }
}

void CasMode::makeChild() {
    if (mabParent_[0] < 0 || mabParent_[1] < 0 || size_t(std::max(mabParent_[0], mabParent_[1])) >= family_.size())
        return;
    const Member& a = family_[size_t(mabParent_[0])];
    const Member& b = family_[size_t(mabParent_[1])];
    std::mt19937 rng(mabSeed_ ? mabSeed_ : std::random_device{}());
    auto random = [&](uint32_t n) { return n ? uint32_t(rng() % n) : 0u; };
    auto dnaOf = [&](const Member& m) {
        return genetics::dnaFromLook(cas::toneFamily(m.spec.hairTone), cas::toneFamily(m.spec.skinTone), m.spec.eyes,
                                     m.spec.gender == cas::gender::Female, random);
    };
    const genetics::Dna da = dnaOf(a), db = dnaOf(b);
    genetics::Dna dna = genetics::determineGenetics(da, db, genetics::baseGameGeneticValues(), random,
                                                    genetics::baseGameSkinTones());
    const genetics::DescendantFace df = genetics::determineDescendantFace(da, db, random);
    Member child;
    cas::CharacterSpec& s = child.spec;
    s.age = mabAge_;
    s.gender = mabGender_ ? mabGender_
                          : (genetics::babyIsFemale(false, false, random) ? cas::gender::Female : cas::gender::Male);
    if (int t = cas::toneNumber(dna.expressed(genetics::gene::Hair)))
        s.hairTone = std::clamp(t, 1, 4);
    if (int t = cas::toneNumber(dna.expressed(genetics::gene::Skin)))
        s.skinTone = std::clamp(t, 1, 4);
    if (!dna.expressed(genetics::gene::Eyes).empty())
        s.eyes = dna.expressed(genetics::gene::Eyes);
    // The face: the parents' structures mixed region by region.
    const cas::FaceStructure fa = baker_->structureOf(a.spec.face, a.spec.gender);
    const cas::FaceStructure fb = baker_->structureOf(b.spec.face, b.spec.gender);
    s.face.structure = std::make_shared<cas::FaceStructure>(baker_->descendant(fa, fb, df.regions));
    s.lastName = familyName_;
    cas::completeSpec(*catalog_, s, uint32_t(rng() | 1));
    child.personality = cas::Personality{0, 0, 0, 0, 0};
    for (int i = 0; i < cas::kPersonalityPoints; ++i) {
        int t = int(rng() % 5);
        while (child.personality[t] >= 10)
            t = (t + 1) % 5;
        ++child.personality[t];
    }
    child.zodiac = cas::zodiacSign(child.personality);
    child.parents = {a.id, b.id};
    child.ties[a.id] = 0; // my parent is
    child.ties[b.id] = 1; // the other parent
    child.portrait = portrait(child, 256);
    mabChild_ = std::move(child);
    std::string regions;
    for (const auto& [r, p] : df.regions)
        regions += r + (p ? "<" + (b.spec.firstName.empty() ? std::string("2") : b.spec.firstName) + " "
                          : "<" + (a.spec.firstName.empty() ? std::string("1") : a.spec.firstName) + " ");
    status_ = "Filho: " + regions;
    dirty_ = true;
}

void CasMode::loadBin() {
    if (binLoaded_)
        return;
    binLoaded_ = true;
    binFile_ = gameDir_ / "TSData/Res/Catalog/Bins/globalcatbin.bundle.package";
    try {
        DbpfPackage pkg(binFile_);
        constexpr uint32_t kBinx = 0x0C560F39, kIdr = 0xAC506764, kAged = 0xAC598EAC;
        std::map<std::pair<uint32_t, uint32_t>, const DbpfEntry*> idrs, ageds;
        for (const auto& e : pkg.entries()) {
            if (e.tgi.type == kIdr)
                idrs[{e.tgi.group, e.tgi.instance}] = &e;
            else if (e.tgi.type == kAged)
                ageds[{e.tgi.group, e.tgi.instance}] = &e;
        }
        for (const auto& e : pkg.entries()) {
            if (e.tgi.type != kBinx)
                continue;
            const auto props = readPropertySet(pkg.read(e));
            auto num = [&](const char* k) -> uint32_t {
                auto it = props.find(k);
                return it == props.end() ? 0 : uint32_t(std::strtoul(it->second.c_str(), nullptr, 0));
            };
            const DbpfEntry* idr = idrs.count({e.tgi.group, e.tgi.instance}) ? idrs[{e.tgi.group, e.tgi.instance}] : nullptr;
            if (!idr)
                continue;
            const auto refs = parseReferenceList(pkg.read(*idr));
            const uint32_t oi = num("objectidx");
            if (oi >= refs.size() || refs[oi].type != kAged)
                continue;
            const Tgi aged = refs[oi];
            if (auto ai = ageds.find({aged.group, aged.instance}); ai != ageds.end()) {
                {
                    const auto ap = readPropertySet(pkg.read(*ai->second));
                    BinSim b;
                    b.group = aged.group;
                    b.age = ap.count("age") ? uint32_t(std::strtoul(ap.at("age").c_str(), nullptr, 0)) : 0;
                    b.gender = ap.count("gender") ? uint32_t(std::strtoul(ap.at("gender").c_str(), nullptr, 0)) : 0;
                    b.sort = int(num("sortindex"));
                    b.thumbnail = cas::outfitDataThumbnailId(aged, false);
                    bin_.push_back(b);
                }
            }
        }
        std::stable_sort(bin_.begin(), bin_.end(), [](const BinSim& a, const BinSim& b) { return a.sort < b.sort; });
    } catch (const std::exception& e) {
        os2::logOnce("CAS: sim bin", "", e);
        warnings_.push_back(std::string("sim bin: ") + e.what());
    }
}

bool CasMode::openCharacter(const fs::path& file, uint32_t group) {
    try {
        DbpfPackage pkg(file);
        Member m;
        m.spec = cas::readCharacterSpec(pkg, group, *catalog_);
        cas::completeSpec(*catalog_, m.spec);
        edit_ = std::move(m);
        if (!edit_.spec.lastName.empty() && familyName_.empty())
            familyName_ = edit_.spec.lastName;
        editing_ = int(family_.size());
        undo_.clear();
        redo_.clear();
        step_ = 0;
        status_ = edit_.spec.firstName.empty() ? (group != 0xFFFFFFFF ? std::string("Sim do estoque") : "Editando " + file.stem().string())
                                               : "Editando " + edit_.spec.firstName;
        rebuild();
        return true;
    } catch (const std::exception& e) {
        status_ = "Não abriu " + file.filename().string() + ": " + e.what();
        dirty_ = true;
        return false;
    }
}

bool CasMode::validateSim() {
    // cTSUISimEditor::ValidateSim (BV 0x1c7a18, base 1.0 0xeeef..; docs/
    // cas-mac.md, round n51), in its order: the game shows the text in its
    // "Sim Changes" box (STR# 0x8C #16) and goes to the step that lacks.
    const bool young = edit_.spec.age <= cas::age::Child || edit_.spec.age == cas::age::Baby;
    auto refuse = [this](uint32_t index, int step) {
        status_ = res_ ? res_->string({packageGroupHash("CAS"), 0x8C, index - 1}).value_or("") : std::string();
        setStep(step);
        return false;
    };
    // New Sims have no aspiration ("New Sims default to no aspiration"):
    // teens and up must choose one.
    if (!young && !edit_.aspiration)
        return refuse(27, 5); // "Please choose an Aspiration for your Sim."
    if (edit_.personality.total() * 100 < 2500) // kMinPersonalityTraitsValue (CB980E50)
        return refuse(11, 5); // "Your Sim has unused Personality points..."
    if (edit_.spec.firstName.empty()) {
        refuse(18, 0); // "Please give your Sim a first name."
        focus_ = Focus::FirstName;
        return false;
    }
    if (edit_.spec.firstName.find_first_of("\\/:*<>|?,;") != std::string::npos)
        return refuse(12, 0); // "Please remove any of the following characters..."
    return true;
}

void CasMode::finishSim() {
    if (editing_ < 0 || !validateSim())
        return;
    if (!edit_.zodiacChosen)
        edit_.zodiac = cas::zodiacSign(edit_.personality);
    // SaveFamily (BV 0x1cb798): Grow Up for babies, toddlers and children.
    if (edit_.spec.age <= cas::age::Child || edit_.spec.age == cas::age::Baby)
        edit_.aspiration = cas::aspiration::GrowUp;
    if (!edit_.id)
        edit_.id = nextId_++;
    if (!edit_.thumbnail)
        edit_.portrait = portrait(edit_);
    if (size_t(editing_) < family_.size())
        family_[size_t(editing_)] = edit_;
    else
        family_.push_back(edit_);
    editing_ = -1;
    zoom_ = 0;
    if (camera_)
        camera_->reset();
    status_ = edit_.spec.firstName + " entrou na família";
    rebuild();
}

void CasMode::acceptFamily() {
    if (editing_ >= 0) {
        finishSim();
        if (editing_ >= 0)
            return; // ValidateSim refused it
    }
    if (family_.empty()) {
        status_ = "A família precisa de pelo menos um Sim";
        dirty_ = true;
        return;
    }
    // ValidateHouseHold: a family name, and an adult or elder when there are children.
    bool grown = false;
    for (const auto& m : family_)
        grown |= m.spec.age == cas::age::Teen || m.spec.age == cas::age::Adult || m.spec.age == cas::age::Elder;
    if (!grown) {
        status_ = "Adicione um adulto ou idoso à família"; // STR# 0x8C #10
        dirty_ = true;
        return;
    }
    if (familyName_.empty())
        familyName_ = family_.front().spec.lastName.empty() ? "Família" : family_.front().spec.lastName;
    auto arg = [&](const std::string& key) -> std::optional<std::string> {
        for (size_t i = 1; i + 1 < args_.size(); ++i)
            if (args_[i] == key)
                return args_[i + 1];
        return std::nullopt;
    };
    const fs::path out = arg("--cas-out").value_or(vm::defaultUserNeighborhoods().string());
    try {
        if (auto hood = arg("--cas-hood")) {
            fs::path src = *hood;
            if (!fs::exists(src))
                src = gameDir_ / "TSData/Res/UserData/Neighborhoods" / *hood;
            // The user's own copy of the neighborhood is written in place;
            // the shipped one (under the game data) into a copy under --cas-out.
            std::error_code ec;
            const fs::path canonSrc = fs::weakly_canonical(src, ec), canonGame = fs::weakly_canonical(gameDir_, ec);
            const std::string rel = canonSrc.lexically_relative(canonGame).string();
            const bool shipped = !rel.empty() && rel.rfind("..", 0) != 0;
            const fs::path target = shipped ? out / src.filename() : src;
            std::vector<vm::NewSim> sims;
            std::vector<std::shared_ptr<RgbaImage>> heads;
            std::mt19937 rng(std::random_device{}());
            for (auto& m : family_) {
                vm::NewSim s;
                s.firstName = m.spec.firstName;
                s.lastName = m.spec.lastName.empty() ? familyName_ : m.spec.lastName;
                s.ageBit = m.spec.age;
                s.genderBit = m.spec.gender;
                s.fitness = m.spec.fitness;
                s.personality = m.personality;
                s.aspiration = m.aspiration;
                s.zodiac = m.zodiac;
                const auto dna = genetics::dnaFromLook(cas::toneFamily(m.spec.hairTone), cas::toneFamily(m.spec.skinTone),
                                                       m.spec.eyes, m.spec.gender == cas::gender::Female,
                                                       [&](uint32_t n) { return n ? uint32_t(rng() % n) : 0u; });
                s.dna = dna.toProperties();
                for (const auto& [otherId, tie] : m.ties)
                    for (size_t k = 0; k < family_.size(); ++k)
                        if (family_[k].id == otherId)
                            s.ties.push_back({k, tie});
                s.clothing = cas::familyBinClothing(*catalog_, m.spec); // the family's clothing bin
                cas::CharacterSpec spec = m.spec;
                spec.lastName = s.lastName;
                // The head shot the family panels and the lot's Sim picker show.
                std::shared_ptr<RgbaImage> head = m.portrait ? m.portrait : portrait(m);
                if (head) {
                    spec.portraitPng = encodePng(*head);
                    heads.push_back(head);
                }
                s.writeCharacter = [this, spec](const std::string& objectName, const fs::path& file) {
                    return cas::writeCharacter(index_, *catalog_, *baker_, spec, objectName, file).guid;
                };
                sims.push_back(std::move(s));
            }
            const auto r = vm::writeNewFamily(src, target, familyName_, "", std::move(sims), 1, !shipped);
            // The photo the family bin and the lot panel show.
            if (!heads.empty()) {
                std::vector<const RgbaImage*> ptrs;
                for (const auto& h : heads)
                    ptrs.push_back(h.get());
                try {
                    vm::writeFamilyThumbnail(r.neighborhood, r.family, encodePng(composeFamilyPhoto(ptrs)));
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "opensims2: cas: family photo: %s\n", e.what());
                }
            }
            status_ = "Família " + familyName_ + " (" + std::to_string(r.family) + ") salva em " + r.neighborhood.string();
            std::printf("opensims2: cas: family %u \"%s\" written to %s:", r.family, familyName_.c_str(),
                        r.neighborhood.string().c_str());
            for (const auto& c : r.characters)
                std::printf(" %s", c.filename().string().c_str());
            std::printf("\n");
            std::fflush(stdout);
            if (onFamilySaved)
                onFamilySaved(r.family);
        } else {
            // No neighborhood: the character files alone.
            fs::create_directories(out / "CAS");
            int n = 0;
            for (const auto& m : family_) {
                char stem[64];
                std::snprintf(stem, sizeof stem, "CAS_User%05d", n++);
                const fs::path file = out / "CAS" / (std::string(stem) + ".package");
                cas::writeCharacter(index_, *catalog_, *baker_, m.spec, stem, file);
                std::printf("opensims2: cas: wrote %s\n", file.string().c_str());
            }
            status_ = "Sims salvos em " + (out / "CAS").string();
        }
    } catch (const std::exception& e) {
        status_ = std::string("Erro ao salvar: ") + e.what();
        std::fprintf(stderr, "opensims2: cas: %s\n", e.what());
    }
    std::fflush(stdout);
    dirty_ = true;
}

bool CasMode::familyScreen(LayoutContext& L) {
    UiScreen& out = L.out;
    auto& rects = L.rects;
    const UiElement& frame = L.frame;
    const float ox = L.ox, oy = L.oy;
    auto& text = L.text;
    auto& rect = L.rect;
    auto& target = L.target;
    auto find = [](const UiElement& root, uint32_t id) { return findWindow(root, id); };

    // The family view: the members over the household bar (the head
    // thumbnail when there is one, the name), and the ways to add one:
    // create a Sim, take one from the Sim bin, make a child (STR# 0x80).
    if (editing_ < 0) {
        float x = ox + 180;
        const float y = oy + 650;
        for (size_t i = 0; i < family_.size(); ++i) {
            const std::string n = family_[i].spec.firstName;
            const RgbaImage* t = family_[i].thumbnail && thumbs_ ? thumbs_->image(family_[i].thumbnail) : family_[i].portrait.get();
            if (t) {
                rect(x - 4, y - 62, x + 56, y - 2, kThumbBg);
                UiQuad q;
                q.image = t;
                q.x0 = x - 4, q.y0 = y - 62, q.x1 = x + 56, q.y1 = y - 2;
                out.quads.push_back(q);
            }
            rect(x - 4, y - 2, x + 90, y + 18, kCellColor);
            text(x, y, 86, n, kCellText);
            const int idx = int(i);
            targets_.push_back({{x - 4, y - 62, x + 90, y + 18}, [this, idx](simd_float2) {
                                    edit_ = family_[size_t(idx)];
                                    editing_ = idx;
                                    step_ = 0;
                                    binOpen_ = false;
                                    rebuild();
                                }});
            x += 98;
        }
        if (family_.size() < 8) {
            const std::pair<const char*, std::function<void()>> adds[] = {
                {"+ Criar um Sim", [this] { binOpen_ = false, newSim(); }},
                {"Estoque de Sims", [this] {
                     loadBin();
                     binOpen_ = !binOpen_;
                     binSel_ = -1;
                 }},
                {"Fazer um Filho", [this] {
                     binOpen_ = false;
                     mabOpen_ = true;
                     mabChild_.reset();
                     // The first two grown-ups as parents.
                     mabParent_[0] = mabParent_[1] = -1;
                     for (size_t i = 0; i < family_.size(); ++i) {
                         const uint32_t a = family_[i].spec.age;
                         if (a != cas::age::Adult && a != cas::age::Elder)
                             continue;
                         if (mabParent_[0] < 0)
                             mabParent_[0] = int(i);
                         else if (mabParent_[1] < 0)
                             mabParent_[1] = int(i);
                     }
                     if (mabParent_[1] < 0) {
                         mabOpen_ = false;
                         status_ = "Fazer um Filho precisa de dois adultos ou idosos na família";
                     }
                 }},
            };
            float bx = x, by = y - 48; // stacked where the next member would go
            for (const auto& [label, act] : adds) {
                rect(bx - 4, by - 2, bx + 116, by + 18, kCellSelected);
                text(bx, by, 112, label, kCellText);
                auto a = act;
                targets_.push_back({{bx - 4, by - 2, bx + 116, by + 18}, [a](simd_float2) { a(); }});
                by += 24;
            }
        }
        // The Sim bin, hanging from the frame's "sim bin anchor" (2d016e9f:tl).
        if (binOpen_)
            if (auto it = scripts_.find(kSimBin); it != scripts_.end()) {
                const UiElement& panel = it->second.roots.front();
                float ax = 0, ay = 0;
                if (const UiElement* pan = panel.child(kPanelAnchor)) {
                    const auto a = pan->area();
                    ax = float(a[0]), ay = float(a[1]);
                }
                float anchorX = ox + 421, anchorY = oy + 152;
                std::function<bool(const UiElement&, float, float)> locateBin = [&](const UiElement& e, float px, float py) {
                    const auto a = e.area();
                    const float ex = px + float(a[0]), ey = py + float(a[1]);
                    if (e.id() == 0x2D016E9F) {
                        anchorX = ex, anchorY = ey;
                        return true;
                    }
                    for (const auto& c : e.children)
                        if (locateBin(c, ex, ey))
                            return true;
                    return false;
                };
                locateBin(frame, ox, oy);
                const auto pa = panel.area();
                const float px = anchorX - ax - float(pa[0]), py = anchorY - ay - float(pa[1]);
                UiDrawHooks bh;
                bh.rects = &rects;
                for (int k = 0; k < 2; ++k)
                    if (const UiElement* b = find(panel, 0x4C6C0000 + uint32_t(k)); b && binGender_ == (k ? cas::gender::Male : cas::gender::Female))
                        bh.buttonState[b] = 2;
                const uint32_t ageBits[5] = {cas::age::Toddler, cas::age::Child, cas::age::Teen, cas::age::Adult, cas::age::Elder};
                for (int k = 0; k < 5; ++k)
                    if (const UiElement* b = find(panel, 0xCC6C0200 + uint32_t(k)); b && binAge_ == ageBits[k])
                        bh.buttonState[b] = 2;
                for (uint32_t id : {0x4C6CA47Du, 0xCC6CA4E0u}) // the gender and age filters
                    if (const UiElement* e = find(panel, id))
                        bh.show.insert(e);
                if (const UiElement* g = find(panel, 0x0CFD6EC6))
                    bh.hide.insert(g); // custom collections
                layoutUiElement(panel, px, py, *res_, *fonts_, out, bh);
                if (auto r = rects.find(&panel); r != rects.end())
                    solid_.push_back(r->second);
                std::vector<int> shownSims;
                for (size_t i = 0; i < bin_.size(); ++i)
                    if ((!binAge_ || (bin_[i].age & binAge_)) && (!binGender_ || (bin_[i].gender & binGender_)))
                        shownSims.push_back(int(i));
                std::map<std::string, std::string> c;
                readConstants(panel, c);
                const int cols = constantInt(c, "kCASBinGridNumColumns", 4), rows = constantInt(c, "kCASBinGridNumRows", 2);
                if (const UiElement* grid = find(panel, 0x4011))
                    if (auto r = rects.find(grid); r != rects.end()) {
                        const auto gr = r->second;
                        const int perPage = cols * rows;
                        const int pages = std::max(1, (int(shownSims.size()) + perPage - 1) / perPage);
                        int& page = pages_["simbin"];
                        page = std::clamp(page, 0, pages - 1);
                        const float cw = (gr[2] - gr[0]) / float(cols), ch = (gr[3] - gr[1]) / float(rows);
                        for (int i = 0; i < perPage; ++i) {
                            const size_t k = size_t(page * perPage + i);
                            if (k >= shownSims.size())
                                break;
                            const int sim = shownSims[k];
                            const float x0 = gr[0] + cw * float(i % cols) + 4, y0 = gr[1] + ch * float(i / cols) + 4;
                            const float x1 = x0 + cw - 8, y1 = y0 + ch - 8;
                            if (sim == binSel_)
                                rect(x0 - 3, y0 - 3, x1 + 3, y1 + 3, kCellSelected);
                            rect(x0, y0, x1, y1, sim == binSel_ ? kThumbSelectedBg : kThumbBg);
                            if (const RgbaImage* t = thumbs_ ? thumbs_->image(bin_[size_t(sim)].thumbnail) : nullptr) {
                                const float side = std::min(x1 - x0, y1 - y0);
                                UiQuad q;
                                q.image = t;
                                q.x0 = (x0 + x1 - side) / 2, q.y0 = (y0 + y1 - side) / 2;
                                q.x1 = q.x0 + side, q.y1 = q.y0 + side;
                                out.quads.push_back(q);
                            }
                            targets_.push_back({{x0, y0, x1, y1}, [this, sim](simd_float2) { binSel_ = sim; }});
                        }
                        target(find(panel, 0x4008), [this](simd_float2) { pages_["simbin"] = std::max(0, pages_["simbin"] - 1); });
                        target(find(panel, 0x4009), [this, pages](simd_float2) {
                            pages_["simbin"] = std::min(pages - 1, pages_["simbin"] + 1);
                        });
                    }
                for (int k = 0; k < 2; ++k)
                    target(find(panel, 0x4C6C0000 + uint32_t(k)), [this, k](simd_float2) {
                        const uint32_t g = k ? cas::gender::Male : cas::gender::Female;
                        binGender_ = binGender_ == g ? 0 : g;
                        pages_["simbin"] = 0;
                    });
                for (int k = 0; k < 5; ++k) {
                    const uint32_t a = ageBits[k];
                    target(find(panel, 0xCC6C0200 + uint32_t(k)), [this, a](simd_float2) {
                        binAge_ = binAge_ == a ? 0 : a;
                        pages_["simbin"] = 0;
                    });
                }
                target(find(panel, 0x2C6CA64E), [this](simd_float2) { binOpen_ = false; }); // cancel
                target(find(panel, 0x4C6CA66D), [this](simd_float2) { // accept: edit the Sim chosen
                    if (binSel_ < 0 || size_t(binSel_) >= bin_.size())
                        return;
                    const BinSim b = bin_[size_t(binSel_)];
                    binOpen_ = false;
                    if (openCharacter(binFile_, b.group)) {
                        edit_.thumbnail = b.thumbnail;
                        edit_.spec.lastName = familyName_;
                    }
                });
            }
    }
    // The family tree (2C054EF0), centred: the members, picked one then
    // another, then the tie in the popup (2C054EF3).
    if (editing_ < 0 && treeOpen_)
        if (auto it = scripts_.find(kFamilyTree); it != scripts_.end()) {
            const UiElement& panel = it->second.roots.front();
            const auto pa = panel.area();
            const float px = ox + float(1024 - (pa[2] - pa[0])) / 2 - float(pa[0]);
            const float py = oy + float(768 - (pa[3] - pa[1])) / 2 - float(pa[1]);
            targets_.clear(); // modal
            UiDrawHooks th;
            th.rects = &rects;
            layoutUiElement(panel, px, py, *res_, *fonts_, out, th);
            static const char* kTieNames[5] = {"pai/mãe", "pai/mãe", "cônjuge", "irmão(ã)", "filho(a)"};
            if (const UiElement* area = find(panel, 0x2000))
                if (auto r = rects.find(area); r != rects.end()) {
                    const auto ar = r->second;
                    const float tw = std::min(72.0f, (ar[2] - ar[0]) / float(std::max<size_t>(family_.size(), 1)) - 8);
                    for (size_t i = 0; i < family_.size(); ++i) {
                        const float x0 = ar[0] + 8 + float(i) * (tw + 8), y0 = ar[1] + 8;
                        const bool sel = int(i) == treeFirst_ || int(i) == treeSecond_;
                        if (sel)
                            rect(x0 - 3, y0 - 3, x0 + tw + 3, y0 + tw + 3, kCellSelected);
                        rect(x0, y0, x0 + tw, y0 + tw, kThumbBg);
                        const Member& m = family_[i];
                        const RgbaImage* img = m.thumbnail && thumbs_ ? thumbs_->image(m.thumbnail) : m.portrait.get();
                        if (img) {
                            UiQuad q;
                            q.image = img;
                            q.x0 = x0, q.y0 = y0, q.x1 = x0 + tw, q.y1 = y0 + tw;
                            out.quads.push_back(q);
                        }
                        text(x0, y0 + tw + 4, tw + 6, m.spec.firstName, kCellText);
                        // Its ties, one line each.
                        float ly = y0 + tw + 20;
                        for (const auto& [otherId, tie] : m.ties)
                            for (const auto& o : family_)
                                if (o.id == otherId && tie < 5) {
                                    text(x0, ly, tw + 6, std::string(kTieNames[tie]) + ": " + o.spec.firstName, kCellText);
                                    ly += 14;
                                }
                        const int idx = int(i);
                        targets_.push_back({{x0, y0, x0 + tw, y0 + tw}, [this, idx](simd_float2) {
                                                if (treeFirst_ < 0 || treeFirst_ == idx) {
                                                    treeFirst_ = treeFirst_ == idx ? -1 : idx;
                                                    treeSecond_ = -1;
                                                } else {
                                                    treeSecond_ = idx;
                                                    treeChoice_ = -1;
                                                }
                                            }});
                    }
                }
            target(find(panel, 0xCC33DE0B), [this](simd_float2) { treeOpen_ = false; }); // accept
            target(find(panel, 0xCC33DE1F), [this](simd_float2) { treeOpen_ = false; }); // cancel
            // The popup: "Make <first> a ... of <second>".
            if (treeFirst_ >= 0 && treeSecond_ >= 0)
                if (auto dt = scripts_.find(kTieDialog); dt != scripts_.end()) {
                    targets_.clear();
                    const UiElement& dlg = dt->second.roots.front();
                    const auto da = dlg.area();
                    const float dx = ox + float(1024 - (da[2] - da[0])) / 2 - float(da[0]);
                    const float dy = oy + float(768 - (da[3] - da[1])) / 2 - float(da[1]) + 60;
                    UiDrawHooks dh;
                    dh.rects = &rects;
                    const std::string first = family_[size_t(treeFirst_)].spec.firstName;
                    const std::string second = family_[size_t(treeSecond_)].spec.firstName;
                    auto fill = [](std::string t, const std::string& name) {
                        if (auto p = t.find("%s"); p != std::string::npos)
                            t.replace(p, 2, name);
                        return t;
                    };
                    auto resText = [&](const UiElement* e, const std::string& fallback) {
                        if (e)
                            if (auto ref = uiStringRef(e->get("captionres")))
                                if (auto str = res_->string(*ref))
                                    return *str;
                        return fallback;
                    };
                    if (const UiElement* t = find(dlg, 0xECDDA449))
                        dh.captions[t] = fill(resText(t, "Make %s a"), first);
                    layoutUiElement(dlg, dx, dy, *res_, *fonts_, out, dh);
                    // The five ties share one place (0x2000-0x2004): the one chosen
                    // shows there, a click goes to the next.
                    if (treeChoice_ < 0)
                        treeChoice_ = 0;
                    if (const UiElement* b = find(dlg, 0x2000 + uint32_t(treeChoice_))) {
                        const auto ba = b->area();
                        const float lx = dx + float(ba[0]), ly = dy + float(ba[1]);
                        const float lw = float(ba[2] - ba[0]), lh = float(ba[3] - ba[1]);
                        rect(lx, ly, lx + lw, ly + lh, kCellSelected);
                        text(lx + 18, ly + 1, lw - 36, fill(resText(b, b->get("caption")), second), kCellText);
                        text(lx + 4, ly + 1, 12, "<", kCellText);
                        text(lx + lw - 12, ly + 1, 12, ">", kCellText);
                        targets_.push_back({{lx, ly, lx + lw / 2, ly + lh}, [this](simd_float2) { treeChoice_ = (treeChoice_ + 4) % 5; }});
                        targets_.push_back({{lx + lw / 2, ly, lx + lw, ly + lh}, [this](simd_float2) { treeChoice_ = (treeChoice_ + 1) % 5; }});
                    }
                    target(find(dlg, 0x1000), [this](simd_float2) { // accept
                        if (treeChoice_ >= 0)
                            setTie(treeFirst_, treeSecond_, treeChoice_);
                        treeFirst_ = treeSecond_ = treeChoice_ = -1;
                    });
                    target(find(dlg, 0x1001), [this](simd_float2) { treeFirst_ = treeSecond_ = treeChoice_ = -1; });
                }
        }
    // Make a Child (CB980E87): the whole screen, from the "MAB anchor"
    // (0d016e8a:bl); the parents' and the child's views show head shots.
    if (editing_ < 0 && mabOpen_)
        if (auto it = scripts_.find(kMakeBaby); it != scripts_.end()) {
            const UiElement& panel = it->second.roots.front();
            float ax = 0, ay = 768;
            if (const UiElement* pan = panel.child(kPanelAnchor)) {
                const auto a = pan->area();
                ax = float(a[0]), ay = float(a[1]);
            }
            const auto pa = panel.area();
            const float px = ox + 0 - ax - float(pa[0]), py = oy + 768 - ay - float(pa[1]);
            UiDrawHooks mh;
            mh.rects = &rects;
            const uint32_t ageBits[5] = {cas::age::Toddler, cas::age::Child, cas::age::Teen, cas::age::Adult, cas::age::Elder};
            for (int k = 0; k < 5; ++k)
                if (const UiElement* b = find(panel, 0xCC6C0200 + uint32_t(k)); b && mabAge_ == ageBits[k])
                    mh.buttonState[b] = 2;
            for (int k = 0; k < 2; ++k)
                if (const UiElement* b = find(panel, 0x4C6C0000 + uint32_t(k));
                    b && mabGender_ == (k ? cas::gender::Male : cas::gender::Female))
                    mh.buttonState[b] = 2;
            // The adult and elder age buttons do nothing for a child: hidden.
            for (int k = 3; k < 5; ++k)
                if (const UiElement* b = find(panel, 0xCC6C0200 + uint32_t(k)))
                    mh.hide.insert(b);
            out.quads.clear(); // the panel covers the frame
            out.textImages.clear();
            targets_.clear();
            layoutUiElement(panel, px, py, *res_, *fonts_, out, mh);
            auto image = [&](const UiElement* w, const RgbaImage* img) {
                if (!w || !img)
                    return;
                auto r = rects.find(w);
                if (r == rects.end())
                    return;
                const auto rr = r->second;
                const float side = std::min(rr[2] - rr[0], rr[3] - rr[1]);
                UiQuad q;
                q.image = img;
                q.x0 = (rr[0] + rr[2] - side) / 2, q.y0 = (rr[1] + rr[3] - side) / 2;
                q.x1 = q.x0 + side, q.y1 = q.y0 + side;
                out.quads.push_back(q);
            };
            auto shot = [&](int idx) -> const RgbaImage* {
                if (idx < 0 || size_t(idx) >= family_.size())
                    return nullptr;
                const Member& m = family_[size_t(idx)];
                if (m.thumbnail && thumbs_)
                    if (const RgbaImage* t = thumbs_->image(m.thumbnail))
                        return t;
                return m.portrait.get();
            };
            image(find(panel, 0xCCB911B8), shot(mabParent_[0]));
            image(find(panel, 0xCCB911A2), shot(mabParent_[1]));
            if (mabChild_)
                image(find(panel, 0xACB91188), mabChild_->portrait.get());
            // The parent grids: the family's adults and elders.
            std::vector<int> grown;
            for (size_t i = 0; i < family_.size(); ++i)
                if (family_[i].spec.age == cas::age::Adult || family_[i].spec.age == cas::age::Elder)
                    grown.push_back(int(i));
            for (int side = 0; side < 2; ++side)
                if (const UiElement* grid = find(panel, side ? 0x4CD98491 : 0x4CD98490))
                    if (auto r = rects.find(grid); r != rects.end()) {
                        const auto gr = r->second;
                        const float cw = (gr[2] - gr[0]) / 2, ch = (gr[3] - gr[1]) / 3;
                        for (size_t k = 0; k < grown.size() && k < 6; ++k) {
                            const int idx = grown[k];
                            const float x0 = gr[0] + cw * float(k % 2) + 2, y0 = gr[1] + ch * float(k / 2) + 2;
                            const float x1 = x0 + cw - 4, y1 = y0 + ch - 4;
                            const bool sel = mabParent_[side] == idx;
                            if (sel)
                                rect(x0 - 2, y0 - 2, x1 + 2, y1 + 2, kCellSelected);
                            rect(x0, y0, x1, y1, kThumbBg);
                            if (const RgbaImage* t = shot(idx)) {
                                UiQuad q;
                                q.image = t;
                                const float sd = std::min(x1 - x0, y1 - y0);
                                q.x0 = (x0 + x1 - sd) / 2, q.y0 = (y0 + y1 - sd) / 2, q.x1 = q.x0 + sd, q.y1 = q.y0 + sd;
                                out.quads.push_back(q);
                            } else {
                                text(x0 + 2, y0 + 2, x1 - x0 - 4, family_[size_t(idx)].spec.firstName, kCellText);
                            }
                            targets_.push_back({{x0, y0, x1, y1}, [this, side, idx](simd_float2) {
                                                    if (mabParent_[1 - side] == idx)
                                                        std::swap(mabParent_[0], mabParent_[1]);
                                                    else
                                                        mabParent_[side] = idx;
                                                    mabChild_.reset();
                                                }});
                        }
                    }
            for (int k = 0; k < 3; ++k) {
                const uint32_t a = ageBits[k];
                target(find(panel, 0xCC6C0200 + uint32_t(k)), [this, a](simd_float2) { mabAge_ = a; });
            }
            for (int k = 0; k < 2; ++k)
                target(find(panel, 0x4C6C0000 + uint32_t(k)), [this, k](simd_float2) {
                    const uint32_t g = k ? cas::gender::Male : cas::gender::Female;
                    mabGender_ = mabGender_ == g ? 0 : g;
                });
            target(find(panel, 0x4C76BB8F), [this](simd_float2) { makeChild(); });
            target(find(panel, 0x2C6CA64E), [this](simd_float2) {
                mabOpen_ = false;
                mabChild_.reset();
            });
            target(find(panel, 0x4C6CA66D), [this](simd_float2) {
                if (!mabChild_ || family_.size() >= 8)
                    return;
                Member c = std::move(*mabChild_);
                mabChild_.reset();
                mabOpen_ = false;
                c.id = nextId_++;
                if (c.spec.firstName.empty())
                    c.spec.firstName = c.spec.gender == cas::gender::Female ? "Filha" : "Filho";
                c.aspiration = cas::aspiration::GrowUp;
                if (!c.portrait)
                    c.portrait = portrait(c);
                status_ = c.spec.firstName + " entrou na família";
                for (int pid : c.parents)
                    for (auto& m : family_)
                        if (m.id == pid)
                            m.ties[c.id] = 4; // my child is
                family_.push_back(std::move(c));
                rebuild();
            });
            if (!status_.empty()) {
                rect(ox + 200, oy + 8, ox + 820, oy + 30, {0.1f, 0.1f, 0.3f, 0.75f});
                text(ox + 206, oy + 11, 600, status_, {1, 1, 1, 1});
            }
            return true;
        }
    return false;
}

} // namespace os2
