#include "family_write.hpp"
#include "hood_save.hpp"

#include "formats/binwriter.hpp"
#include "formats/character_write.hpp"
#include "formats/dbpf.hpp"
#include "formats/neighborhood.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <regex>
#include <stdexcept>

namespace os2::vm {

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kLocal = 0xFFFFFFFF;
constexpr int32_t kDefaultFamilyFunds = 20000; // CB980E50: kDefaultFamilyFunds=20000

std::vector<uint8_t> writeStrings(const std::vector<std::string>& values) {
    BinWriter w;
    for (int i = 0; i < 64; ++i)
        w.u8(0);
    w.u16(0xFFFD);
    w.u16(uint16_t(values.size() * 2));
    for (uint8_t lang : {uint8_t(1), uint8_t(35)}) // US English and Brazilian Portuguese
        for (const auto& v : values) {
            w.u8(lang);
            w.raw(std::span(reinterpret_cast<const uint8_t*>(v.data()), v.size()));
            w.u8(0);
            w.u8(0);
        }
    return std::move(w.bytes());
}

int16_t lifeStage(uint32_t ageBit) {
    switch (ageBit) {
    case 0x20: return hood::lifestage::Baby;
    case 0x01: return hood::lifestage::Toddler;
    case 0x02: return hood::lifestage::Child;
    case 0x04: return hood::lifestage::Teen;
    case 0x10: return hood::lifestage::Elder;
    default: return hood::lifestage::Adult;
    }
}

fs::path neighborhoodPackage(const fs::path& dir) {
    for (const auto& e : fs::directory_iterator(dir))
        if (e.path().filename().string().ends_with("_Neighborhood.package"))
            return e.path();
    throw std::runtime_error("no _Neighborhood.package in " + dir.string());
}

} // namespace

void writeFamilyThumbnail(const fs::path& neighborhood, uint32_t family, const std::vector<uint8_t>& png) {
    constexpr uint32_t kTypeFamilyThumb = 0x8C3CE95A;
    const std::string pkgName = neighborhoodPackage(neighborhood).filename().string();
    const std::string prefix = pkgName.substr(0, pkgName.find('_'));
    const fs::path file = neighborhood / "Thumbnails" / (prefix + "_FamilyThumbnails.package");
    DbpfWriter w;
    uint32_t group = 0x7FE65A3D; // the shipped pictures' group
    if (fs::exists(file)) {
        DbpfPackage pkg(file);
        for (const auto& e : pkg.entries()) {
            if (e.tgi.type == type::DIR)
                continue;
            if (e.tgi.type == kTypeFamilyThumb) {
                group = e.tgi.group;
                if (e.tgi.instance == family)
                    continue; // replaced below
            }
            w.addStored(e.tgi, pkg.readRaw(e), e.rawSize);
        }
    }
    w.add(Tgi{kTypeFamilyThumb, group, family, 0}, png);
    fs::create_directories(file.parent_path());
    w.save(file);
}

fs::path defaultUserNeighborhoods() {
    const char* home = std::getenv("HOME");
    return fs::path(home ? home : ".") / "Documents" / "opensims2" / "Neighborhoods";
}

FamilyWriteResult writeNewFamily(const fs::path& source, const fs::path& target, const std::string& familyName,
                                 const std::string& description, std::vector<NewSim> sims, uint32_t seed,
                                 bool inPlace) {
    FamilyWriteResult res;
    if (sims.empty())
        throw std::runtime_error("a family needs at least one Sim");
    if (fs::exists(target) && fs::equivalent(source, target) && !inPlace)
        throw std::runtime_error("refusing to write into the source neighborhood " + source.string());
    if (!fs::exists(target)) {
        fs::create_directories(target.parent_path());
        fs::copy(source, target, fs::copy_options::recursive);
    }
    res.neighborhood = target;
    const fs::path pkgPath = neighborhoodPackage(target);
    const std::string prefix = pkgPath.filename().string().substr(0, pkgPath.filename().string().find('_'));
    const hood::Neighborhood hood = hood::Neighborhood::load(target, false);

    // New IDs: the family number and neighbor IDs after the highest in use
    // (the townie families 0x7FFE/0x7FFF left out), the character numbers
    // after the highest file.
    uint32_t family = 1;
    for (const auto& f : hood.families)
        if (f.id < 0x7000)
            family = std::max(family, f.id + 1);
    uint16_t nextNeighbor = 1;
    for (const auto& n : hood.neighbors)
        nextNeighbor = std::max<uint16_t>(nextNeighbor, uint16_t(n.id + 1));
    int nextUser = 0;
    const fs::path charDir = target / "Characters";
    fs::create_directories(charDir);
    const std::regex userRe(prefix + "_User([0-9]+)\\.package", std::regex::icase);
    for (const auto& e : fs::directory_iterator(charDir)) {
        std::smatch m;
        const std::string name = e.path().filename().string();
        if (std::regex_match(name, m, userRe))
            nextUser = std::max(nextUser, std::stoi(m[1]) + 1);
    }
    res.family = family;

    std::mt19937 rng(seed);
    std::map<Tgi, std::vector<uint8_t>, bool (*)(const Tgi&, const Tgi&)> added(
        [](const Tgi& a, const Tgi& b) { return std::tie(a.type, a.group, a.instance) < std::tie(b.type, b.group, b.instance); });
    hood::FamilyRecord fami;
    fami.version = 0x4E;
    fami.lot = 0;
    fami.simGroupId = family;
    fami.funds = kDefaultFamilyFunds;
    fami.flags = 1;
    std::vector<uint16_t> ids;
    for (auto& sim : sims) {
        char stem[64];
        std::snprintf(stem, sizeof stem, "%s_User%05d", prefix.c_str(), nextUser++);
        const fs::path file = charDir / (std::string(stem) + ".package");
        if (!sim.writeCharacter)
            throw std::runtime_error("no character writer for " + sim.firstName);
        const uint32_t guid = sim.writeCharacter(stem, file);
        res.characters.push_back(file);
        const uint16_t id = nextNeighbor++;
        ids.push_back(id);
        fami.members.push_back(guid);

        hood::SimDescription d;
        d.unknown0 = 0x70;
        d.version = 0x20;
        d.unknown2 = 0x20;
        d.unknown3 = 3;
        d.trailer = 1;
        d.neighborId = id;
        d.guid = guid;
        auto& p = d.personData;
        const cas::Personality& t = sim.personality;
        p[cas::traitdata::Neat] = int16_t(t.neat * 100);
        p[cas::traitdata::Nice] = int16_t(t.nice * 100);
        p[cas::traitdata::Active] = int16_t(t.active * 100);
        p[cas::traitdata::Outgoing] = int16_t(t.outgoing * 100);
        p[cas::traitdata::Playful] = int16_t(t.playful * 100);
        p[47] = p[cas::traitdata::Neat];
        p[48] = p[cas::traitdata::Nice];
        p[49] = p[cas::traitdata::Active];
        p[50] = p[cas::traitdata::Outgoing];
        p[51] = p[cas::traitdata::Playful];
        p[cas::traitdata::Aspiration] = int16_t(sim.aspiration);
        p[hood::sdsc::Age] = lifeStage(sim.ageBit);
        p[hood::sdsc::Gender] = sim.genderBit == 1 ? 1 : 0;
        p[hood::sdsc::FamilyNumber] = int16_t(family);
        p[cas::traitdata::Zodiac] = int16_t(sim.zodiac);
        p[81] = sim.fitness == 2 ? 1 : 0;
        p[82] = sim.fitness == 2 ? 1000 : 500;
        p[hood::sdsc::AgeDaysLeft] = 25;
        p[156] = 25;
        p[118] = 3100;
        p[158] = 1;
        for (size_t i = hood::sdsc::InterestFirst; i <= hood::sdsc::InterestLast; ++i)
            p[i] = int16_t(rng() % 1001);
        added[Tgi{hoodtype::SDSC, kLocal, id, 0}] = d.write();
        if (!sim.dna.empty()) {
            std::map<std::string, cas::PropertyValue> props;
            for (const auto& [k, v] : sim.dna)
                props[k] = {cas::PropertyValue::String, v};
            added[Tgi{hoodtype::SDNA, kLocal, id, 0}] = cas::writePropertySetXml(props);
        }
    }
    res.neighbors = ids;
    added[Tgi{hoodtype::FAMI, kLocal, family, 0}] = fami.write();
    added[Tgi{kTypeStr, kLocal, family, 0}] = writeStrings({familyName, description});

    // The NGBH with the new (empty) inventories.
    DbpfPackage pkg(pkgPath);
    // The family's clothing bin: each member's clothes (cTSFamily::
    // AddMemberClothing -> AddClothingFromOutfitData, BV 0x47ec68/0x47e5ac:
    // the parts of the Sim's outfits), as SaveFamilyClothing writes them.
    std::vector<Tgi> clothing;
    for (const auto& sim : sims)
        clothing.insert(clothing.end(), sim.clothing.begin(), sim.clothing.end());
    for (auto& [tgi, bytes] : familyClothingEntries(pkg, family, clothing))
        added[tgi] = std::move(bytes);
    // The family tree: the new Sims' ties, added to the neighborhood's
    // (u32 version, u32 count, per Sim u16 neighbor ID, u32 1, u32 n,
    // n x (u32 type, u16 neighbor ID)).
    bool anyTie = false;
    for (const auto& sim : sims)
        anyTie |= !sim.ties.empty();
    if (anyTie) {
        BinWriter tw;
        std::vector<std::pair<uint16_t, std::vector<std::pair<uint32_t, uint16_t>>>> rows;
        uint32_t version = 1;
        for (const auto& e : pkg.entries())
            if (e.tgi.type == hoodtype::FamilyTies) {
                const auto old = hood::FamilyTies::parse(pkg.read(e));
                version = old.version ? old.version : 1;
                for (const auto& sm : old.sims) {
                    std::vector<std::pair<uint32_t, uint16_t>> t;
                    for (const auto& tie : sm.ties)
                        t.push_back({uint32_t(tie.type), tie.other});
                    rows.push_back({sm.neighborId, std::move(t)});
                }
                break;
            }
        for (size_t i = 0; i < sims.size(); ++i) {
            std::vector<std::pair<uint32_t, uint16_t>> t;
            for (const auto& [other, type] : sims[i].ties)
                if (other < ids.size())
                    t.push_back({type, ids[other]});
            if (!t.empty())
                rows.push_back({ids[i], std::move(t)});
        }
        tw.u32(version);
        tw.u32(uint32_t(rows.size()));
        for (const auto& [id, t] : rows) {
            tw.u16(id);
            tw.u32(1);
            tw.u32(uint32_t(t.size()));
            for (const auto& [type, other] : t) {
                tw.u32(type);
                tw.u16(other);
            }
        }
        Tgi tiesTgi{hoodtype::FamilyTies, kLocal, 0, 0};
        for (const auto& e : pkg.entries())
            if (e.tgi.type == hoodtype::FamilyTies)
                tiesTgi = e.tgi;
        added[tiesTgi] = std::move(tw.bytes());
    }
    for (const auto& e : pkg.entries())
        if (e.tgi.type == hoodtype::NGBH) {
            auto inv = hood::NeighborhoodInventories::parse(pkg.read(e));
            hood::Inventory f;
            f.id = family;
            inv.families.push_back(f);
            for (uint16_t id : ids) {
                hood::Inventory n;
                n.id = id;
                inv.neighbors.push_back(n);
            }
            added[e.tgi] = inv.write();
            break;
        }

    DbpfWriter w;
    for (const auto& e : pkg.entries()) {
        if (e.tgi.type == type::DIR)
            continue;
        auto it = added.find(e.tgi);
        if (it != added.end()) {
            w.add(e.tgi, std::move(it->second), true);
            added.erase(it);
        } else {
            w.addStored(e.tgi, pkg.readRaw(e), e.rawSize);
        }
    }
    for (auto& [tgi, bytes] : added)
        w.add(tgi, std::move(bytes), true);
    pkg.closeFile();
    const fs::path tmp = pkgPath.string() + ".new";
    w.save(tmp);
    fs::rename(tmp, pkgPath);
    return res;
}

} // namespace os2::vm
