#include "app/game_save.hpp"

#include "formats/character_write.hpp"
#include "formats/dbpf.hpp"
#include "formats/simantics.hpp"
#include "formats/lot.hpp"
#include "formats/lot_objects.hpp"
#include "formats/lot_ui_state.hpp"
#include "formats/neighborhood.hpp"
#include "vm/hood_bridge.hpp"
#include "vm/hood_save.hpp"
#include "vm/lot_records.hpp"
#include "vm/lot_save.hpp"
#include "port/process.hpp"

#include <algorithm>
#include <string>
#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <map>
#include "port/files.hpp"

namespace os2 {

namespace fs = std::filesystem;

namespace {

bool under(const fs::path& path, const fs::path& dir) {
    std::error_code ec;
    const fs::path d = fs::weakly_canonical(fs::absolute(dir), ec);
    const fs::path p = fs::weakly_canonical(fs::absolute(path), ec);
    auto di = d.begin();
    auto pi = p.begin();
    for (; di != d.end(); ++di, ++pi)
        if (pi == p.end() || *pi != *di)
            return false;
    return true;
}

} // namespace

std::vector<std::string> copySessionCharacters(vm::VM& vm, const fs::path& sessionDir, const fs::path& chars,
                                               const std::string& prefix) {
    // Character files written in play (a Sim grown up, a baby born, an
    // NPC made from a template: the session's NPCs folder) go to the
    // neighborhood's, with the neighbors saveNeighborhood writes (the
    // game makes each new character's file in Characters at once,
    // cTSNeighborManager::AddNewCharacter, and writes the neighbors on
    // save). The unique NPCs' files drawn from their objects
    // ("Unique_<GUID>") stay out: the game keeps those characters in
    // the objects.
    std::vector<std::string> lines;
    const std::string userPrefix = prefix + "_User";
    for (const fs::path& dir : {sessionDir, sessionDir / "NPCs"}) {
        std::error_code ec;
        for (const auto& f : fs::directory_iterator(dir, ec)) {
            const std::string name = f.path().filename().string();
            if (f.path().extension() != ".package" ||
                !(name.rfind(userPrefix, 0) == 0 || (dir == sessionDir && fs::exists(chars / name))))
                continue;
            // The name the neighbor has now (a newborn named by the player).
            try {
                uint32_t guid = 0;
                {
                    DbpfPackage pkg(f.path());
                    for (const auto& x : pkg.entries())
                        if (x.tgi.type == 0x4F424A44) { // OBJD
                            guid = ObjectDefinition::parse(pkg.read(x)).guid();
                            break;
                        }
                }
                for (const auto& [id, n] : vm.neighbors)
                    if (guid && n.guid == guid && !n.firstName.empty()) {
                        if (cas::setCharacterNames(f.path(), n.firstName, n.lastName))
                            lines.push_back("character " + name + " named " + n.firstName);
                        break;
                    }
            } catch (const std::exception& e) {
                lines.push_back("warning: " + name + ": " + e.what());
            }
            const fs::path tmp = chars / (name + ".saving");
            fs::copy_file(f.path(), tmp, fs::copy_options::overwrite_existing);
            os2::replaceFile(tmp, chars / name);
            lines.push_back("character " + name + (dir == sessionDir ? " (grown up or born)" : " (NPC)"));
        }
    }
    return lines;
}

GameSaveResult saveGame(vm::VM& vm, const fs::path& lotFile, const fs::path& gameDir, const fs::path& sessionDir,
                        const std::optional<LotUiState>& ui) {
    GameSaveResult out;
    try {
        if (under(lotFile, gameDir))
            throw std::runtime_error("refusing to save into the game's data (" + lotFile.string() + ")");
        if (!fs::exists(lotFile))
            throw std::runtime_error("no lot file " + lotFile.string());
        const fs::path tmpLot = lotFile.parent_path() / (lotFile.filename().string() + ".saving");
        // Save types of objects created during play, from the other lots' tables.
        std::map<uint32_t, LotObjects::SaveType> more;
        for (const auto& f : fs::directory_iterator(lotFile.parent_path()))
            if (f.path().extension() == ".package" && f.path().filename() != lotFile.filename()) {
                DbpfPackage other(f.path());
                for (auto& [g, t] : LotObjects::loadSaveTypes(other))
                    more.emplace(g, std::move(t));
            }
        // cObjects of created objects come from the shipped lots (read only).
        std::vector<fs::path> templates;
        std::error_code ec;
        for (const auto& d : fs::directory_iterator(gameDir / "TSData/Res/UserData/Neighborhoods", ec))
            if (fs::is_directory(d.path() / "Lots"))
                for (const auto& f : fs::directory_iterator(d.path() / "Lots"))
                    if (f.path().extension() == ".package" && f.path().filename() != lotFile.filename())
                        templates.push_back(f.path());
        std::sort(templates.begin(), templates.end());
        {
            DbpfPackage pkg(lotFile);
            const LotObjects saved = LotObjects::load(pkg);
            vm::CaptureResult captured = vm::captureLot(vm, saved, &more);
            const Lot world = Lot::load(pkg);
            const vm::RecordsResult records = vm::captureLotRecords(vm, world, pkg, saved, captured.lot, templates);
            if (ui) {
                // cTSMainToolbar::Write's state over the lot's own (its name
                // field kept), or a new one at instance 2 as the game's lots.
                LotObjects::Record r{kTypeLotUiState, 0xFFFFFFFF, 2, {}};
                std::vector<uint8_t> name;
                for (const auto& e : pkg.entries())
                    if (e.tgi.type == kTypeLotUiState) {
                        r.group = e.tgi.group, r.instance = e.tgi.instance;
                        const auto old = pkg.read(e);
                        name.assign(old.begin(), old.begin() + std::min<size_t>(old.size(), 64));
                        break;
                    }
                r.bytes = writeLotUiState(*ui, name);
                captured.lot.otherRecords.push_back(std::move(r));
            }
            captured.lot.save(pkg, tmpLot);
            char buf[512];
            std::snprintf(buf, sizeof buf,
                          "lot %s: %zu objects (%zu updated, %zu added, %zu gone, %zu kept as read); cObjects %zu "
                          "moved, %zu new; relationships %zu changed, %zu new",
                          lotFile.filename().string().c_str(), captured.lot.objects.size(), captured.updated,
                          captured.added, captured.removed, captured.kept, records.sceneMoved, records.sceneNew,
                          records.relationshipsChanged, records.relationshipsNew);
            out.lines.push_back(buf);
            for (const auto& w : captured.warnings)
                out.lines.push_back("warning: " + w);
            for (const auto& w : records.warnings)
                out.lines.push_back("warning: " + w);
        }
        os2::replaceFile(tmpLot, lotFile);
        if (const fs::path chars = lotFile.parent_path().parent_path() / "Characters"; fs::is_directory(chars))
            for (auto& l : copySessionCharacters(vm, sessionDir, chars, lotFile.filename().string().substr(0, 4)))
                out.lines.push_back(std::move(l));
        if (const fs::path hoodPackage = vm::neighborhoodPackageOf(lotFile); !hoodPackage.empty()) {
            const fs::path tmpHood = hoodPackage.parent_path() / (hoodPackage.filename().string() + ".saving");
            const hood::Neighborhood hood = hood::Neighborhood::load(hoodPackage, true);
            const vm::HoodSaveResult h = vm::saveNeighborhood(vm, hood, hoodPackage, tmpHood);
            os2::replaceFile(tmpHood, hoodPackage);
            char buf[512];
            std::snprintf(buf, sizeof buf,
                          "neighborhood %s: %zu families, %zu Sim descriptions, relationships %zu changed, %zu new%s, "
                          "%zu clothes bought, %zu born, %zu NPCs made",
                          hoodPackage.filename().string().c_str(), h.families, h.descriptions, h.relationshipsChanged,
                          h.relationshipsNew, h.inventories ? ", inventories" : "", h.clothing, h.born, h.npcs);
            out.lines.push_back(buf);
            for (const auto& w : h.warnings)
                out.lines.push_back("warning: " + w);
        }
        out.ok = true;
    } catch (const std::exception& e) {
        out.error = e.what();
    }
    return out;
}

namespace {

const char* const kSessionPrefix = "opensims2-characters-";

void removeSessionCharacters() {
    std::error_code ec;
    fs::remove_all(fs::temp_directory_path(ec) / (kSessionPrefix + std::to_string(processId())), ec);
}

} // namespace

fs::path sessionCharactersDir() {
    static const fs::path dir = [] {
        removeStaleSessionCharacters(fs::temp_directory_path());
        fs::path d = fs::temp_directory_path() / (kSessionPrefix + std::to_string(processId()));
        std::error_code ec;
        fs::create_directories(d, ec);
        std::atexit(removeSessionCharacters); // what was not saved goes with the session
        return d;
    }();
    return dir;
}

size_t removeStaleSessionCharacters(const fs::path& tempDir) {
    size_t removed = 0;
    std::error_code ec;
    const std::string prefix = kSessionPrefix;
    std::vector<fs::path> stale;
    for (const auto& e : fs::directory_iterator(tempDir, ec)) {
        const std::string name = e.path().filename().string();
        if (name.rfind(prefix, 0) != 0 || !e.is_directory(ec))
            continue;
        const std::string digits = name.substr(prefix.size());
        if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos)
            continue;
        const long pid = std::stol(digits);
        // A session still running keeps its folder (kill 0: the process
        // exists; EPERM: it exists under another user).
        if (processAlive(pid))
            continue;
        stale.push_back(e.path());
    }
    for (const auto& p : stale)
        removed += fs::remove_all(p, ec) > 0;
    return removed;
}

} // namespace os2
