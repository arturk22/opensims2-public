#include "design_catalog.hpp"
#include "formats/log_once.hpp"

#include "property_set.hpp"
#include "resindex.hpp"

#include <algorithm>
#include <cctype>
#include <set>

namespace os2 {

namespace {

constexpr uint32_t kTypeMaterialOverride = 0x4C697E5A; // MMAT

std::string lower(std::string s) {
    for (char& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

uint32_t number(const std::map<std::string, std::string>& p, const char* k) {
    auto it = p.find(k);
    if (it == p.end() || it->second.empty())
        return 0;
    try {
        return uint32_t(std::stoul(it->second, nullptr, 0));
    } catch (const std::logic_error& err) { // not a number (std::stoul): 0
        os2::logOnce("design catalog number", std::string(k) + "=" + it->second, err);
        return 0;
    }
}

std::string text(const std::map<std::string, std::string>& p, const char* k) {
    auto it = p.find(k);
    return it == p.end() ? std::string() : it->second;
}

// A bool property (readPropertySet writes "true"/"false"); a number too.
bool flag(const std::map<std::string, std::string>& p, const char* k) {
    const std::string v = lower(text(p, k));
    if (v == "true")
        return true;
    if (v == "false")
        return false;
    return number(p, k) != 0;
}

} // namespace

std::string DesignCatalog::key(std::string name) {
    name = lower(std::move(name));
    if (name.size() > 5 && name.compare(name.size() - 5, 5, "_cres") == 0)
        name.resize(name.size() - 5);
    return name;
}

void DesignCatalog::add(const std::map<std::string, std::string>& p) {
    if (text(p, "type") != "modelMaterial")
        return;
    DesignMaterial m;
    m.model = key(text(p, "modelName"));
    m.subset = lower(text(p, "subsetName"));
    m.material = text(p, "name");
    m.family = text(p, "family");
    m.objectGuid = number(p, "objectGUID");
    m.stateFlags = number(p, "materialStateFlags");
    m.isDefault = flag(p, "defaultMaterial");
    if (m.model.empty() || m.subset.empty() || m.material.empty())
        return;
    byModel_[m.model].push_back(all_.size());
    all_.push_back(std::move(m));
}

DesignCatalog DesignCatalog::load(ResourceIndex& index) {
    DesignCatalog c;
    for (const auto& loc : index.ofType(kTypeMaterialOverride)) {
        try {
            const auto bytes = index.read(loc);
            c.add(readPropertySet(bytes));
        } catch (const std::exception& err) {
            os2::logOnce("design catalog", std::to_string(loc.entry->tgi.instance), err);
        }
    }
    return c;
}

std::vector<std::string> DesignCatalog::subsets(const std::string& model, const std::vector<std::string>& order,
                                                size_t limit) const {
    std::vector<std::string> out;
    auto it = byModel_.find(key(model));
    if (it == byModel_.end())
        return out;
    std::set<std::string> have;
    for (size_t i : it->second)
        if (all_[i].stateFlags == 0)
            have.insert(all_[i].subset);
    auto take = [&](const std::string& s) {
        if (out.size() < limit && have.count(s) && std::find(out.begin(), out.end(), s) == out.end())
            out.push_back(s);
    };
    for (const auto& s : order)
        take(lower(s));
    // Subsets the model's shapes did not list come after, in catalog order.
    for (size_t i : it->second)
        take(all_[i].subset);
    return out;
}

std::vector<const DesignMaterial*> DesignCatalog::options(const std::string& model, const std::string& subset) const {
    std::vector<const DesignMaterial*> out;
    auto it = byModel_.find(key(model));
    if (it == byModel_.end())
        return out;
    const std::string s = lower(subset);
    std::set<std::string> seen;
    for (size_t i : it->second) {
        const DesignMaterial& m = all_[i];
        if (m.subset != s || m.stateFlags != 0)
            continue;
        if (!seen.insert(lower(m.material)).second) {
            // The same material in another family: a default one wins.
            if (m.isDefault)
                for (auto& o : out)
                    if (lower(o->material) == lower(m.material))
                        o = &m;
            continue;
        }
        out.push_back(&m);
    }
    std::stable_sort(out.begin(), out.end(), [](const DesignMaterial* a, const DesignMaterial* b) {
        if (a->isDefault != b->isDefault)
            return a->isDefault;
        return lower(a->material) < lower(b->material);
    });
    return out;
}

} // namespace os2
