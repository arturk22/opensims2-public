#include "resindex.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

namespace os2 {

namespace {

constexpr size_t kMaxOpenFiles = 32;

std::string lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// OpenPGP CRC-24 (poly 0x864CFB, init 0xB704CE).
uint32_t crc24(std::string_view s) {
    uint32_t crc = 0xB704CE;
    for (unsigned char c : s) {
        crc ^= uint32_t(c) << 16;
        for (int i = 0; i < 8; ++i) {
            crc <<= 1;
            if (crc & 0x1000000)
                crc ^= 0x864CFB;
        }
    }
    return crc & 0xFFFFFF;
}

// MSB-first CRC-32 (poly 0x04C11DB7, init 0xFFFFFFFF, no final xor).
uint32_t crc32msb(std::string_view s) {
    uint32_t crc = 0xFFFFFFFF;
    for (unsigned char c : s) {
        crc ^= uint32_t(c) << 24;
        for (int i = 0; i < 8; ++i)
            crc = (crc & 0x80000000) ? (crc << 1) ^ 0x04C11DB7 : crc << 1;
    }
    return crc;
}

} // namespace

uint32_t nameInstanceHash(std::string_view name) { return crc24(lower(name)) | 0xFF000000; }
uint32_t nameInstanceHiHash(std::string_view name) { return crc32msb(lower(name)); }

ResourceIndex::ResourceIndex(const std::filesystem::path& root, const std::string& locale) {
    const auto localeDir = root / "Locale";
    const bool localized = std::filesystem::is_directory(localeDir);
    if (localized)
        locale_ = locale;
    std::vector<std::filesystem::path> paths, localePaths;
    for (auto it = std::filesystem::recursive_directory_iterator(root); it != std::filesystem::recursive_directory_iterator();
         ++it) {
        const auto& de = *it;
        if (localized && de.is_directory() && de.path().parent_path() == localeDir) {
            if (de.path().filename() != locale)
                it.disable_recursion_pending();
            continue;
        }
        if (!de.is_regular_file())
            continue;
        auto ext = lower(de.path().extension().string());
        if (ext != ".package")
            continue;
        const bool inLocale = localized && de.path().lexically_relative(localeDir).begin()->string() != "..";
        (inLocale ? localePaths : paths).push_back(de.path());
    }
    addPackages(std::move(localePaths));
    addPackages(std::move(paths));
}

ResourceIndex::ResourceIndex(const std::vector<std::filesystem::path>& packages, const ResourceIndex* fallback)
    : fallback_(fallback) {
    addPackages(packages);
}

void ResourceIndex::addPackages(std::vector<std::filesystem::path> paths) {
    std::sort(paths.begin(), paths.end());
    for (const auto& p : paths) {
        auto pkg = std::make_unique<DbpfPackage>(p);
        pkg->closeFile();
        for (const auto& e : pkg->entries()) {
            if (e.tgi.type == type::DIR)
                continue;
            if (!byTgi_.emplace(e.tgi, Location{pkg.get(), &e}).second)
                ++duplicates_;
        }
        packages_.push_back(std::move(pkg));
    }
}

std::optional<ResourceIndex::Location> ResourceIndex::find(const Tgi& tgi) const {
    auto it = byTgi_.find(tgi);
    if (it == byTgi_.end())
        return fallback_ ? fallback_->find(tgi) : std::nullopt;
    return it->second;
}

std::vector<uint8_t> ResourceIndex::read(const Location& loc) {
    if (!loc.package->fileOpen()) {
        if (open_.size() >= kMaxOpenFiles) {
            open_.front()->closeFile();
            open_.pop_front();
        }
        open_.push_back(loc.package);
    }
    return loc.package->read(*loc.entry);
}

std::optional<std::vector<uint8_t>> ResourceIndex::read(const Tgi& tgi) {
    auto loc = find(tgi);
    if (!loc)
        return std::nullopt;
    return read(*loc);
}

std::optional<ResourceIndex::Location> ResourceIndex::findByName(std::string_view name, uint32_t type) const {
    uint32_t group = kLocalGroup;
    if (name.starts_with("#0x")) {
        // "#0xGGGGGGGG!name": a resource of the same package (character
        // files), stored with group 0xFFFFFFFF and hashed without the prefix.
        if (auto bang = name.find('!'); bang != std::string_view::npos) {
            name = name.substr(bang + 1);
            group = 0xFFFFFFFF;
        }
    } else if (name.starts_with("##0x")) {
        if (auto bang = name.find('!'); bang != std::string_view::npos) {
            group = uint32_t(std::stoul(std::string(name.substr(4, bang - 4)), nullptr, 16));
            name = name.substr(bang + 1);
        }
    }
    Tgi k{type, group, nameInstanceHash(name), nameInstanceHiHash(name)};
    if (auto loc = find(k))
        return loc;
    k.instanceHi = 0; // older resources leave the high instance empty
    if (auto loc = find(k))
        return loc;
    // "Scope!name" (a lot's imposter: "N001_Lot11!imposter_gmnd"): a resource
    // of the same package, group 0xFFFFFFFF, hashed without the scope.
    if (group == kLocalGroup && !name.starts_with("#"))
        if (auto bang = name.find('!'); bang != std::string_view::npos && bang + 1 < name.size())
            return findByName("#0xFFFFFFFF!" + std::string(name.substr(bang + 1)), type);
    return std::nullopt;
}

std::vector<ResourceIndex::Location> ResourceIndex::ofType(uint32_t type) const {
    std::vector<Location> out;
    for (const auto& pkg : packages_)
        for (const auto& e : pkg->entries())
            if (e.tgi.type == type)
                out.push_back(Location{pkg.get(), &e});
    return out;
}

} // namespace os2
