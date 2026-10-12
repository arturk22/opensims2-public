#include "anim_snaps.hpp"

#include "app/vm_objects.hpp"
#include "formats/model.hpp"
#include "render/renderer.hpp"
#include "vm/vm.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace os2 {

namespace {
std::string lower(std::string s) {
    for (char& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
const Xform kHidden{{}, {0, 0, -1000}};

// A snap applied: the object's model is where it says, and nowhere else
// (the bottle the fridge's get put in the hand as "babyBottle" is the one
// the feeder's start snaps there again as "o2a", the a2o object).
void applySnap(std::map<std::string, AnimSnap>& state, const AnimSnap& snap) {
    const std::string model = lower(snap.model);
    for (auto it = state.begin(); it != state.end();)
        it = it->first != snap.object && !model.empty() && lower(it->second.model) == model ? state.erase(it) : std::next(it);
    state[snap.object] = snap;
}
} // namespace

AnimSnapView::AnimSnapView(Renderer& renderer, ResourceIndex& index, VmObjectView& objects, vm::VM& vm)
    : renderer_(renderer), index_(index), objects_(objects), vm_(vm) {}

AnimSnapView::~AnimSnapView() = default;

void AnimSnapView::release(SimSnaps& s) {
    s.state.clear();
    for (int16_t id : s.hidden)
        objects_.setHidden(id, false);
    s.hidden.clear();
    for (auto& [name, m] : s.models)
        if (m.actor)
            m.actor->transform = kHidden;
}

void AnimSnapView::simAnimation(int16_t sim, SimActor& actor, const Animation* animation, double at) {
    SimSnaps& s = sims_[sim];
    // The last animation's snaps up to now (when no frame was drawn since).
    const float until = float((at - s.start) * Animation::kTicksPerSecond);
    for (; s.applied < s.snaps.size() && float(s.snaps[s.applied].ticks) <= until; ++s.applied)
        applySnap(s.state, s.snaps[s.applied]);
    s.actor = &actor;
    s.animation = animation;
    s.start = at;
    s.snaps = animation ? animSnaps(*animation) : std::vector<AnimSnap>{};
    s.applied = 0;
    if (!animation)
        release(s);
}

void AnimSnapView::objectAnimation(int16_t object, const std::string& name, double at) {
    objectAnims_[object] = {name, at};
}

AnimSnapView::Model& AnimSnapView::model(SimSnaps& s, const AnimSnap& snap) {
    Model& m = s.models[snap.object + ":" + snap.model];
    if (m.actor || !m.skeleton.empty())
        return m;
    m.skeleton = "?"; // tried
    try {
        // The snap's model is a CRES name, or a root node's
        // ("__foodPizzaSlice__8": foodPizzaSlice_8; "foodTray":
        // templateFoodTray; "doll": dollFemale01).
        auto load = [&]() -> ObjectModel {
            try {
                return loadObjectModel(index_, snap.model);
            } catch (const std::exception&) {
                const size_t cut = snap.model.find("__");
                if (cut != std::string::npos)
                    try {
                        return loadObjectModel(index_, snap.model.substr(0, cut) + "_" + snap.model.substr(cut + 2));
                    } catch (const std::exception&) {
                    }
                const std::string byRoot = cresByRootNode(index_, snap.model.substr(0, cut));
                if (byRoot.empty())
                    throw;
                return loadObjectModel(index_, byRoot);
            }
        };
        const ObjectModel om = load();
        SkinnedModel skin(om.skeleton);
        skin.add(om);
        auto actor = std::make_unique<SimActor>(std::move(skin));
        actor->transform = kHidden;
        actor->model = renderer_.addObject(actor->skin.model, index_, true);
        renderer_.addInstance(actor->model, matrix_identity_float4x4);
        if (!actor->skin.skeleton.bones.empty())
            m.skeleton = lower(actor->skin.skeleton.bones[0].name);
        m.actor = std::move(actor);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "opensims2: snap model %s: %s\n", snap.model.c_str(), e.what());
    }
    return m;
}

void AnimSnapView::update(double now) {
    for (auto& [sim, s] : sims_) {
        if (!s.animation || !s.actor)
            continue;
        const float ticks = float((now - s.start) * Animation::kTicksPerSecond);
        for (; s.applied < s.snaps.size() && float(s.snaps[s.applied].ticks) <= ticks; ++s.applied)
            applySnap(s.state, s.snaps[s.applied]);
        const auto& state = s.state;
        if (state.empty())
            continue;
        std::set<std::string> shown;
        for (const auto& [object, snap] : state) {
            Model& m = model(s, snap);
            if (!m.actor)
                continue;
            const int b = snap.bone.empty() ? -1 : s.actor->skin.skeleton.find(snap.bone);
            if (b < 0 || size_t(b) >= s.actor->bones.size())
                continue;
            m.actor->transform = s.actor->bones[size_t(b)];
            shown.insert(snap.object + ":" + snap.model);
            // The snap's pose of the model in the slot (snapObjectPose).
            if (!snap.pose.empty() && m.pose != snap.pose) {
                m.pose = snap.pose;
                try {
                    const Animation& a = m.actor->animation(index_, snap.pose);
                    const double at = s.start + snap.ticks / Animation::kTicksPerSecond;
                    m.actor->cue(at, a, {.fade = 0.0f, .loop = true});
                    m.playing = snap.pose;
                    m.poseAt = at;
                } catch (const std::exception&) {
                }
            }
        }
        for (auto& [key, m] : s.models)
            if (m.actor && !shown.count(key))
                m.actor->transform = kHidden;
        // The held objects the snaps stand in for, and their animations.
        for (const auto& [id, o] : vm_.objects()) {
            if (o.person || id == sim || objects_.outermostContainer(id) != sim)
                continue;
            const std::string cres = lower(objects_.cresOf(id));

            bool named = false;
            for (const auto& [object, snap] : state)
                named = named || (!object.empty() && cres.rfind(lower(object), 0) == 0) ||
                        (!snap.model.empty() && cres.rfind(lower(snap.model), 0) == 0); // "o2a": the a2o object
            if (!named)
                continue;
            if (s.hidden.insert(id).second)
                objects_.setHidden(id, true);
            const auto anim = objectAnims_.find(id);
            if (anim == objectAnims_.end())
                continue;
            for (auto& [key, m] : s.models) {
                if (!m.actor || m.playing == anim->second.first || anim->second.second < m.poseAt)
                    continue;
                try {
                    const Animation& a = m.actor->animation(index_, anim->second.first);
                    const std::string tag = lower(a.skeletonTag.empty() && !a.targets.empty() ? a.targets[0].tag : a.skeletonTag);
                    if (tag != m.skeleton)
                        continue;
                    m.actor->cue(anim->second.second, a, {.fade = 0.1f, .loop = false});
                    m.playing = anim->second.first;
                    if (std::getenv("OS2_DEBUG_VM"))
                        std::fprintf(stderr, "opensims2: snapped %s plays %s\n", key.c_str(), m.playing.c_str());
                } catch (const std::exception& e) {
                    static std::set<std::string> reported; // once per animation (as the objects' own)
                    if (reported.insert(anim->second.first).second)
                        std::fprintf(stderr, "opensims2: snapped %s: %s\n", key.c_str(), e.what());
                    m.playing = anim->second.first; // do not retry
                }
            }
        }
    }
    for (auto& [sim, s] : sims_)
        for (auto& [key, m] : s.models)
            if (m.actor) {
                m.actor->update(now);
                renderer_.updateObject(m.actor->model, m.actor->posed);
            }
}

} // namespace os2
