// s2pkg lot-save: a lot loaded into the VM, run and written back (vm/lot_save.hpp).

#include "save_cmds.hpp"

#include "formats/binreader.hpp"
#include "formats/dbpf.hpp"
#include "formats/lot.hpp"
#include "formats/lot_objects.hpp"
#include "formats/neighborhood.hpp"
#include "vm/hood_bridge.hpp"
#include "formats/resindex.hpp"
#include "vm/lot.hpp"
#include "vm/lot_resume.hpp"
#include "vm/lot_save.hpp"
#include "vm/save_types.hpp"
#include "vm/lot_records.hpp"
#include "vm/hood_save.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace os2 {

namespace {

namespace fs = std::filesystem;

fs::path resRootOfLot(const fs::path& pkg) {
    for (auto p = fs::absolute(pkg).parent_path(); p.has_relative_path(); p = p.parent_path())
        if (p.filename() == "Res")
            return p;
    throw std::runtime_error("the lot is not under a TSData/Res directory");
}

// A VM holding the lot in `package`, resumed from its saved state; the Sims'
// character files come from `charactersDir`.
struct LoadedLot {
    std::unique_ptr<vm::VM> vm;
    LotObjects saved;
    vm::LotLoad load;
    vm::ResumeResult resumed;
};

// As lot-run: the lot's neighbourhood (`source` names it), entered, and the
// Sims bound to their neighbors.
LoadedLot loadInto(vm::CodeLibrary& code, const fs::path& package, const fs::path& source,
                   const hood::Neighborhood* hood, const fs::path& charactersDir) {
    LoadedLot out;
    DbpfPackage pkg(package);
    const Lot lot = Lot::load(pkg);
    out.saved = LotObjects::load(pkg);
    out.vm = std::make_unique<vm::VM>(code);
    vm::VM& m = *out.vm;
    m.initCreatedObjects = true;
    m.instructionsPerTurn = 20000;
    m.setClockGlobals();
    if (hood) {
        vm::loadNeighborhood(m, *hood);
        vm::enterLot(m, vm::lotIdOf(source), hood->info ? int16_t(hood->info->id) : int16_t(0));
    }
    out.load = vm::loadLot(m, lot, out.saved, charactersDir);
    if (hood)
        vm::bindLotSims(m);
    out.resumed = vm::startSavedLot(m, out.load, out.saved);
    return out;
}

int cmdLotSave(const std::vector<std::string>& args) {
    std::vector<fs::path> paths;
    long ticks = 0;
    bool check = false;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--ticks" && i + 1 < args.size())
            ticks = std::stol(args[++i]);
        else if (args[i] == "--check")
            check = true;
        else
            paths.emplace_back(args[i]);
    }
    if (paths.size() != 2)
        throw std::runtime_error("usage: s2pkg lot-save <lot.package> <out.package> [--ticks N] [--check]");
    const fs::path& src = paths[0];
    const fs::path& dst = paths[1];
    if (fs::exists(dst) && fs::equivalent(src, dst))
        throw std::runtime_error("refusing to write over the lot itself");
    const fs::path res = resRootOfLot(src);
    const fs::path game = fs::weakly_canonical(res.parent_path().parent_path()); // Res -> TSData -> the game
    if (fs::weakly_canonical(fs::absolute(dst)).string().starts_with(game.string()))
        throw std::runtime_error("refusing to write into the game's data: pick an output outside " + game.string());
    ResourceIndex index(res);
    vm::CodeLibrary code(&index);
    const fs::path characters = vm::charactersDirOf(src);
    std::optional<hood::Neighborhood> hood;
    if (const auto hoodPackage = vm::neighborhoodPackageOf(src); !hoodPackage.empty())
        hood = hood::Neighborhood::load(hoodPackage, true);
    const hood::Neighborhood* h = hood ? &*hood : nullptr;
    LoadedLot a = loadInto(code, src, src, h, characters);
    for (long i = 0; i < ticks; ++i)
        a.vm->tick();
    // Save types of objects the run may create, from the other lots' tables.
    std::map<uint32_t, LotObjects::SaveType> more;
    for (const auto& f : fs::directory_iterator(src.parent_path()))
        if (f.path().extension() == ".package" && !fs::equivalent(f.path(), src)) {
            DbpfPackage other(f.path());
            for (auto& [g, t] : LotObjects::loadSaveTypes(other))
                more.emplace(g, std::move(t));
        }
    vm::CaptureResult captured = vm::captureLot(*a.vm, a.saved, &more);
    for (const auto& w : captured.warnings)
        std::printf("  warning: %s\n", w.c_str());
    vm::RecordsResult records;
    {
        DbpfPackage pkg(src);
        const Lot world = Lot::load(pkg);
        // cObjects for created objects are copied from the other lots.
        std::vector<fs::path> others;
        for (const auto& d : fs::directory_iterator(res / "UserData" / "Neighborhoods"))
            if (fs::is_directory(d.path() / "Lots"))
                for (const auto& f : fs::directory_iterator(d.path() / "Lots"))
                    if (f.path().extension() == ".package" && !fs::equivalent(f.path(), src))
                        others.push_back(f.path());
        std::sort(others.begin(), others.end());
        records = vm::captureLotRecords(*a.vm, world, pkg, a.saved, captured.lot, others);
        for (const auto& w : records.warnings)
            std::printf("  warning: %s\n", w.c_str());
        captured.lot.save(pkg, dst);
    }
    std::printf("  records: %zu cObjects moved, %zu new; %zu routing states; %zu relationships changed, %zu new, "
                "%zu gone\n",
                records.sceneMoved, records.sceneNew, records.routing, records.relationshipsChanged,
                records.relationshipsNew, records.relationshipsGone);
    std::printf("%s: %zu objects saved after %ld ticks (%zu updated, %zu added, %zu gone, %zu kept as read; %zu new "
                "objt records) to %s\n",
                src.filename().string().c_str(), captured.lot.objects.size(), ticks, captured.updated, captured.added,
                captured.removed, captured.kept, captured.newSaveTypes, dst.string().c_str());
    if (!check)
        return 0;
    // The saved package reads back, state by state, to what was written.
    size_t same = 0, differ = 0, unread = 0;
    {
        DbpfPackage written(dst);
        const LotObjects back = LotObjects::load(written);
        for (const auto& w : back.warnings)
            std::printf("  read back: %s\n", w.c_str());
        for (const auto& s : back.objects) {
            unread += !s.tailError.empty();
            const LotObjects::State* w = captured.lot.state(s.id);
            if (w && LotObjects::writeState(*w, back.version) == LotObjects::writeState(s, back.version))
                ++same;
            else
                ++differ;
        }
        // With no ticks run, each state is written back as the game wrote it.
        if (ticks == 0) {
            DbpfPackage original(src);
            size_t bytesSame = 0, bytesDiffer = 0;
            for (const auto& s : a.saved.objects) {
                const auto* e = original.find({LotObjects::resourceType(s.kind), 0xFFFFFFFF, uint32_t(uint16_t(s.id)), 0});
                const auto* f = written.find({LotObjects::resourceType(s.kind), 0xFFFFFFFF, uint32_t(uint16_t(s.id)), 0});
                if (e && f && original.read(*e) == written.read(*f))
                    ++bytesSame;
                else
                    ++bytesDiffer;
            }
            std::printf("  states byte for byte as the game wrote them: %zu, different: %zu\n", bytesSame,
                        bytesDiffer);
            // The other records the save rewrites, by type.
            std::map<uint32_t, std::pair<size_t, size_t>> others; // type -> (same, different)
            for (const auto& e : original.entries()) {
                const uint32_t t = e.tgi.type;
                if (t == type::DIR || t == 0x584F424A || t == 0x584D544F || t == 0x50455253)
                    continue;
                const auto* f = written.find(e.tgi);
                auto& c = others[t];
                if (f && original.read(e) == written.read(*f))
                    ++c.first;
                else if (f || t != 0x3053CF74)
                    ++c.second;
            }
            for (const auto& [t, c] : others)
                if (c.second)
                    std::printf("  records of type %s different from the game's: %zu of %zu\n", hexString(t).c_str(),
                                c.second, c.first + c.second);
        }
    }
    std::printf("  read back: %zu states as written, %zu different, %zu not read through\n", same, differ, unread);
    LoadedLot b = loadInto(code, dst, src, h, characters);
    const auto diffs = vm::compareLotState(*a.vm, *b.vm);
    std::printf("  VM state after load -> save -> load: %s\n", diffs.empty() ? "the same" : "differs:");
    for (const auto& d : diffs)
        std::printf("    %s\n", d.c_str());
    return diffs.empty() && differ == 0 ? 0 : 1;
}

// hood-save: every lot of a neighborhood loaded with the neighborhood, run
// and saved to <out>/Lots, each lot's changes written back to the
// neighborhood package (<out>/<P>_Neighborhood.package, which the next lot
// is loaded with); the character files and the rest of the folder are
// copied. With --check each saved lot is loaded back with the saved
// neighborhood and compared with the VM that saved it.
int cmdHoodSave(const std::vector<std::string>& args) {
    std::vector<fs::path> paths;
    long ticks = 0;
    bool check = false;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--ticks" && i + 1 < args.size())
            ticks = std::stol(args[++i]);
        else if (args[i] == "--check")
            check = true;
        else
            paths.emplace_back(args[i]);
    }
    if (paths.size() != 2)
        throw std::runtime_error("usage: s2pkg hood-save <neighborhood dir> <out dir> [--ticks N] [--check]");
    const fs::path hoodDir = fs::canonical(paths[0]);
    const fs::path outDir = fs::weakly_canonical(fs::absolute(paths[1]));
    const std::string prefix = hoodDir.filename().string();
    const fs::path hoodPackage = hoodDir / (prefix + "_Neighborhood.package");
    if (!fs::exists(hoodPackage))
        throw std::runtime_error("no " + hoodPackage.filename().string() + " in " + hoodDir.string());
    std::vector<fs::path> lots;
    if (fs::is_directory(hoodDir / "Lots"))
        for (const auto& f : fs::directory_iterator(hoodDir / "Lots"))
            if (f.path().extension() == ".package")
                lots.push_back(f.path());
    std::sort(lots.begin(), lots.end());
    if (lots.empty())
        throw std::runtime_error("no lots in " + hoodDir.string());
    const fs::path res = resRootOfLot(lots.front());
    const fs::path game = fs::weakly_canonical(res.parent_path().parent_path());
    if (outDir.string().starts_with(game.string()) || outDir == hoodDir)
        throw std::runtime_error("refusing to write into the game's data: pick an output outside " + game.string());
    fs::create_directories(outDir / "Lots");
    // The folder's other files (characters, thumbnails, images) as they are.
    for (const auto& e : fs::recursive_directory_iterator(hoodDir)) {
        const fs::path rel = fs::relative(e.path(), hoodDir);
        if (*rel.begin() == "Lots" || e.path() == hoodPackage)
            continue;
        const fs::path to = outDir / rel;
        if (e.is_directory())
            fs::create_directories(to);
        else if (!fs::exists(to))
            fs::copy_file(e.path(), to);
    }
    const fs::path outHood = outDir / hoodPackage.filename();
    const fs::path tmpHood = outDir / (prefix + "_Neighborhood.package.tmp");
    fs::copy_file(hoodPackage, outHood, fs::copy_options::overwrite_existing);
    ResourceIndex index(res);
    const fs::path characters = hoodDir / "Characters";
    std::vector<fs::path> templates;
    for (const auto& d : fs::directory_iterator(res / "UserData" / "Neighborhoods"))
        if (fs::is_directory(d.path() / "Lots"))
            for (const auto& f : fs::directory_iterator(d.path() / "Lots"))
                if (f.path().extension() == ".package")
                    templates.push_back(f.path());
    std::sort(templates.begin(), templates.end());
    std::map<uint32_t, LotObjects::SaveType> more;
    for (const auto& f : lots) {
        DbpfPackage other(f);
        for (auto& [g, t] : LotObjects::loadSaveTypes(other))
            more.emplace(g, std::move(t));
    }
    size_t same = 0, differ = 0, skipped = 0;
    for (const auto& src : lots) {
        const fs::path dst = outDir / "Lots" / src.filename();
        vm::CodeLibrary code(&index); // each lot adds its Sims' character files
        const hood::Neighborhood hood = hood::Neighborhood::load(outHood, true);
        std::unique_ptr<LoadedLot> a;
        try {
            a = std::make_unique<LoadedLot>(loadInto(code, src, src, &hood, characters));
        } catch (const std::exception& ex) {
            std::printf("%s: not saved (%s); copied as it is\n", src.filename().string().c_str(), ex.what());
            fs::copy_file(src, dst, fs::copy_options::overwrite_existing);
            ++skipped;
            continue;
        }
        for (long i = 0; i < ticks; ++i)
            a->vm->tick();
        std::vector<fs::path> others;
        for (const auto& t : templates)
            if (!fs::equivalent(t, src))
                others.push_back(t);
        vm::CaptureResult captured = vm::captureLot(*a->vm, a->saved, &more);
        vm::RecordsResult records;
        {
            DbpfPackage pkg(src);
            const Lot world = Lot::load(pkg);
            records = vm::captureLotRecords(*a->vm, world, pkg, a->saved, captured.lot, others);
            captured.lot.save(pkg, dst);
        }
        const vm::HoodSaveResult h = vm::saveNeighborhood(*a->vm, hood, outHood, tmpHood);
        fs::rename(tmpHood, outHood);
        std::printf("%s: %zu objects (%zu added, %zu gone, %zu new objt records); cObjects %zu moved, %zu new; %zu "
                    "routing states; lot relationships %zu changed, %zu new, %zu gone; neighborhood: %zu families, %zu "
                    "Sim descriptions, relationships %zu changed, %zu new%s\n",
                    src.filename().string().c_str(), captured.lot.objects.size(), captured.added, captured.removed,
                    captured.newSaveTypes, records.sceneMoved, records.sceneNew, records.routing,
                    records.relationshipsChanged, records.relationshipsNew, records.relationshipsGone, h.families,
                    h.descriptions, h.relationshipsChanged, h.relationshipsNew, h.inventories ? ", inventories" : "");
        for (const auto& w : captured.warnings)
            std::printf("  warning: %s\n", w.c_str());
        for (const auto& w : records.warnings)
            std::printf("  warning: %s\n", w.c_str());
        for (const auto& w : h.warnings)
            std::printf("  warning: %s\n", w.c_str());
        if (!check)
            continue;
        vm::CodeLibrary code2(&index);
        const hood::Neighborhood back = hood::Neighborhood::load(outHood, true);
        LoadedLot b = loadInto(code2, dst, dst, &back, characters);
        auto diffs = vm::compareLotState(*a->vm, *b.vm);
        for (auto& d : vm::compareNeighborhoodState(*a->vm, *b.vm))
            diffs.push_back("neighborhood: " + d);
        if (diffs.empty()) {
            ++same;
        } else {
            ++differ;
            std::printf("  load -> save -> load differs:\n");
            for (const auto& d : diffs)
                std::printf("    %s\n", d.c_str());
        }
    }
    std::printf("%s: %zu lots saved to %s after %ld ticks", prefix.c_str(), lots.size() - skipped, outDir.string().c_str(),
                ticks);
    if (check)
        std::printf("; the same VM after load -> save -> load in %zu, different in %zu", same, differ);
    std::printf("\n");
    return differ ? 1 : 0;
}

// objt-check: each record of the lots' save type tables against the one
// saveTypeRecord builds from the definition, field by field.
int cmdObjtCheck(const std::vector<std::string>& args) {
    std::map<std::string, size_t> wrong;
    std::map<std::string, std::vector<std::string>> examples;
    size_t records = 0, missing = 0, same = 0;
    for (const auto& a : args) {
        const fs::path src(a);
        ResourceIndex index(resRootOfLot(src));
        vm::CodeLibrary code(&index);
        std::optional<hood::Neighborhood> hood;
        if (const auto hp = vm::neighborhoodPackageOf(src); !hp.empty())
            hood = hood::Neighborhood::load(hp, true);
        LoadedLot l = loadInto(code, src, src, hood ? &*hood : nullptr, vm::charactersDirOf(src));
        for (const auto& [st, t] : l.saved.saveTypes) {
            ++records;
            const auto g = vm::saveTypeRecord(l.vm->code(), t.guid);
            if (!g) {
                ++missing;
                continue;
            }
            bool ok = true;
            auto check = [&](const char* name, auto x, auto y) {
                if (x == y)
                    return;
                ok = false;
                ++wrong[name];
                auto& e = examples[name];
                if (e.size() < 4) {
                    std::string sx, sy;
                    if constexpr (std::is_same_v<decltype(x), std::string>)
                        sx = x, sy = y;
                    else
                        sx = std::to_string(x), sy = std::to_string(y);
                    e.push_back(src.stem().string() + " " + hexString(t.guid) + " \"" + t.catalogName + "\": " + sx +
                                " vs " + sy);
                }
            };
            check("init tree version", t.initTreeVersion, g->initTreeVersion);
            check("main tree version", t.mainTreeVersion, g->mainTreeVersion);
            check("field 0x3A", t.field3A, g->field3A);
            check("field 0x3B", t.field3B, g->field3B);
            check("string count", t.stringCount, g->stringCount);
            check("object type", t.objectType, g->objectType);
            check("name", t.catalogName, g->catalogName);
            check("flags", t.flags, g->flags);
            check("field 0xD0", t.guid2, g->guid2);
            same += ok;
        }
    }
    std::printf("%zu records: %zu built the same, %zu without a definition\n", records, same, missing);
    for (const auto& [k, n] : wrong) {
        std::printf("  %s differs in %zu\n", k.c_str(), n);
        for (const auto& e : examples[k])
            std::printf("    %s\n", e.c_str());
    }
    return 0;
}

} // namespace

int saveCommand(const std::string& cmd, int argc, char** argv) {
    if (cmd == "lot-save" && argc >= 4)
        return cmdLotSave({argv + 2, argv + argc});
    if (cmd == "hood-save" && argc >= 4)
        return cmdHoodSave({argv + 2, argv + argc});
    if (cmd == "objt-check" && argc >= 3)
        return cmdObjtCheck({argv + 2, argv + argc});
    return -1;
}

const char* saveUsage() {
    return "       s2pkg lot-save <lot.package> <out.package> [--ticks N] [--check]\n"
           "       s2pkg hood-save <neighborhood dir> <out dir> [--ticks N] [--check]\n"
           "       s2pkg objt-check <lot.package>...\n";
}

} // namespace os2
