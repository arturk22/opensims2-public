#include "app/game_save.hpp"

#include "formats/dbpf.hpp"
#include "formats/lot.hpp"
#include "formats/lot_objects.hpp"
#include "formats/neighborhood.hpp"
#include "vm/hood_bridge.hpp"
#include "vm/hood_save.hpp"
#include "vm/lot_records.hpp"
#include "vm/lot_save.hpp"

#include <algorithm>
#include <string>
#include <unistd.h>
#include <cstdio>
#include <map>

namespace os2 {

namespace fs = std::filesystem;

namespace {

bool under(const fs::path& path, const fs::path& dir) {
    const std::string d = fs::weakly_canonical(fs::absolute(dir)).string();
    const std::string p = fs::weakly_canonical(fs::absolute(path)).string();
    return p == d || (p.size() > d.size() && p.compare(0, d.size(), d) == 0 && p[d.size()] == '/');
}

} // namespace

GameSaveResult saveGame(vm::VM& vm, const fs::path& lotFile, const fs::path& gameDir) {
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
        fs::rename(tmpLot, lotFile);
        // Character files rewritten in play (aging) replace the neighborhood's.
        if (const fs::path chars = lotFile.parent_path().parent_path() / "Characters"; fs::is_directory(chars)) {
            std::error_code ec;
            for (const auto& f : fs::directory_iterator(sessionCharactersDir(), ec))
                if (f.path().extension() == ".package" && fs::exists(chars / f.path().filename())) {
                    const fs::path tmp = chars / (f.path().filename().string() + ".saving");
                    fs::copy_file(f.path(), tmp, fs::copy_options::overwrite_existing);
                    fs::rename(tmp, chars / f.path().filename());
                    out.lines.push_back("character " + f.path().filename().string() + " (grown up)");
                }
        }
        if (const fs::path hoodPackage = vm::neighborhoodPackageOf(lotFile); !hoodPackage.empty()) {
            const fs::path tmpHood = hoodPackage.parent_path() / (hoodPackage.filename().string() + ".saving");
            const hood::Neighborhood hood = hood::Neighborhood::load(hoodPackage, true);
            const vm::HoodSaveResult h = vm::saveNeighborhood(vm, hood, hoodPackage, tmpHood);
            fs::rename(tmpHood, hoodPackage);
            char buf[512];
            std::snprintf(buf, sizeof buf,
                          "neighborhood %s: %zu families, %zu Sim descriptions, relationships %zu changed, %zu new%s",
                          hoodPackage.filename().string().c_str(), h.families, h.descriptions, h.relationshipsChanged,
                          h.relationshipsNew, h.inventories ? ", inventories" : "");
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

fs::path sessionCharactersDir() {
    static const fs::path dir = [] {
        fs::path d = fs::temp_directory_path() / ("opensims2-characters-" + std::to_string(::getpid()));
        std::error_code ec;
        fs::create_directories(d, ec);
        return d;
    }();
    return dir;
}

} // namespace os2
