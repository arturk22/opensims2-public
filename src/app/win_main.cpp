// Windows host: a real lot, its light map, its objects, Sims driven by the
// SimAntics VM, and one image from the game's own UI scripts.

#include "app/lot_audio.hpp"
#include "app/lot_vm.hpp"
#include "audio/decode.hpp"
#include "formats/image.hpp"
#include "formats/lighting_config.hpp"
#include "formats/lot.hpp"
#include "formats/movie.hpp"
#include "formats/lot_objects.hpp"
#include "formats/lot_lightmap.hpp"
#include "formats/lot_model.hpp"
#include "formats/resindex.hpp"
#include "formats/sim.hpp"
#include "formats/sim_actor.hpp"
#include "formats/sim_censor.hpp"
#include "formats/sim_character.hpp"
#include "formats/skinning.hpp"
#include "vm/lot.hpp"
#include "formats/effects.hpp"
#include "formats/ui_resources.hpp"
#include "render/particle_system.hpp"
#include "formats/ui_script.hpp"
#include "render/gpu_renderer.hpp"

#include <SDL3/SDL.h>

#include <simd/simd.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <windows.h>
#include <dbghelp.h>

namespace {

LONG WINAPI crashFilter(EXCEPTION_POINTERS* info) {
    std::FILE* out = std::fopen("build-win/crash.txt", "w");
    if (!out)
        out = std::fopen("crash.txt", "w");
    if (!out || !info || !info->ExceptionRecord || !info->ContextRecord)
        return EXCEPTION_CONTINUE_SEARCH;
    std::fprintf(out, "exception %08lx at %p\n", static_cast<unsigned long>(info->ExceptionRecord->ExceptionCode),
                 info->ExceptionRecord->ExceptionAddress);
    HANDLE process = GetCurrentProcess();
    HANDLE thread = GetCurrentThread();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(process, nullptr, TRUE);
    CONTEXT context = *info->ContextRecord;
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = context.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < 48; ++i) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context, nullptr, SymFunctionTableAccess64,
                         SymGetModuleBase64, nullptr))
            break;
        char buffer[sizeof(SYMBOL_INFO) + 256];
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;
        DWORD64 displacement = 0;
        if (SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol))
            std::fprintf(out, "%d %s+0x%llx\n", i, symbol->Name, static_cast<unsigned long long>(displacement));
        else
            std::fprintf(out, "%d %llx\n", i, static_cast<unsigned long long>(frame.AddrPC.Offset));
        if (frame.AddrReturn.Offset == 0)
            break;
    }
    std::fclose(out);
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

namespace {

constexpr uint32_t kTypeCres = 0xE519C933;

std::string lower(std::string s) {
    for (auto& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string cresOf(os2::ResourceIndex& index, const os2::Lot::Object& object, std::map<std::string, os2::Tgi>& definitions) {
    if (index.findByName(object.model + "_cres", kTypeCres))
        return object.model;
    if (definitions.empty())
        for (const auto& loc : index.ofType(0x4F424A44))
            definitions.emplace(lower(os2::ObjectDefinition::parse(index.read(loc)).fileName), loc.entry->tgi);
    std::string name = object.definition;
    if (name.ends_with("_objt"))
        name.resize(name.size() - 5);
    auto it = definitions.find(lower(name));
    const std::string cres = it == definitions.end() ? std::string() : os2::objectCresName(index, it->second);
    return cres.empty() ? object.model : cres;
}

simd_float4x4 placementOf(const os2::Lot::Object& object) {
    const auto& q = object.rotation;
    simd_float4x4 at = simd_matrix4x4(simd_quaternion(q[0], q[1], q[2], q[3]));
    at.columns[3] = simd_make_float4(object.position[0], object.position[1], object.position[2], 1);
    return at;
}

const os2::RgbaImage* firstUiImage(os2::UiResources& ui, os2::ResourceIndex& index, float& x, float& y, float& w, float& h) {
    for (const auto& loc : index.ofType(os2::kTypeUiScript)) {
        std::vector<uint8_t> bytes;
        try {
            bytes = index.read(loc);
        } catch (...) {
            continue;
        }
        if (os2::isReia(bytes))
            continue;
        const os2::UiScript script = os2::UiScript::parse(bytes);
        const os2::RgbaImage* found = nullptr;
        auto walk = [&](auto&& self, const os2::UiElement& element, int ox, int oy) -> bool {
            const auto area = element.area();
            const int x0 = ox + area[0], y0 = oy + area[1];
            const int width = std::max(0, area[2] - area[0]), height = std::max(0, area[3] - area[1]);
            if (const std::string* image = element.attr("image")) {
                if (const auto ref = os2::uiResourceRef(*image)) {
                    if (const os2::RgbaImage* picture = ui.image(*ref)) {
                        if (picture->width >= 64 && picture->height >= 64 && width >= 64 && height >= 64) {
                            found = picture;
                            x = float(x0);
                            y = float(y0);
                            w = float(width);
                            h = float(height);
                            return true;
                        }
                    }
                }
            }
            for (const auto& child : element.children)
                if (self(self, child, x0, y0))
                    return true;
            return false;
        };
        for (const auto& root : script.roots)
            if (walk(walk, root, 0, 0))
                return found;
    }
    return nullptr;
}

std::string idleFor(const os2::SkinnedModel& skin) {
    const std::string& name = skin.skeleton.name;
    if (!name.empty() && std::tolower(static_cast<unsigned char>(name[0])) == 'c')
        return "c-idle-neutral-armsSide-wtC-breatheLoop50";
    if (!name.empty() && std::tolower(static_cast<unsigned char>(name[0])) == 'p')
        return "p-idle-neutral-lookAround";
    return "a-idle-neutral-armsSide-wtC-breatheLoop";
}

struct Camera {
    simd_float3 pivot{20, 20, 0};
    float yaw = 0.8f;
    float pitch = 0.55f;
    float distance = 28.f;

    simd_float3 eye() const {
        const float horiz = distance * std::cos(pitch);
        return {pivot.x + std::cos(yaw) * horiz, pivot.y + std::sin(yaw) * horiz, pivot.z + distance * std::sin(pitch)};
    }

    void basis(simd_float3& right, simd_float3& up) const {
        const simd_float3 eyeAt = eye();
        const simd_float3 forward = simd_normalize(pivot - eyeAt);
        right = simd_normalize(simd_cross(forward, simd_float3{0, 0, 1}));
        up = simd_cross(right, forward);
    }

    simd_float4x4 viewProj(float aspect) const {
        const float horiz = distance * std::cos(pitch);
        const simd_float3 eye{pivot.x + std::cos(yaw) * horiz, pivot.y + std::sin(yaw) * horiz, pivot.z + distance * std::sin(pitch)};
        const simd_float3 f = simd_normalize(pivot - eye);
        const simd_float3 s = simd_normalize(simd_cross(f, simd_float3(0, 0, 1)));
        const simd_float3 u = simd_cross(s, f);
        const simd_float4x4 view = simd_matrix(simd_make_float4(s.x, u.x, -f.x, 0), simd_make_float4(s.y, u.y, -f.y, 0),
                                               simd_make_float4(s.z, u.z, -f.z, 0),
                                               simd_make_float4(-simd_dot(s, eye), -simd_dot(u, eye), simd_dot(f, eye), 1));
        const float fov = 50.f * 3.14159265f / 180.f;
        const float t = 1.f / std::tan(fov * 0.5f);
        const float ys = t * std::min(aspect, 4.f / 3.f);
        const float xs = ys / aspect;
        const float n = 0.1f, fa = 400.f;
        const float zs = fa / (n - fa);
        const simd_float4x4 projection = simd_matrix(simd_make_float4(xs, 0, 0, 0), simd_make_float4(0, ys, 0, 0),
                                                     simd_make_float4(0, 0, zs, -1), simd_make_float4(0, 0, zs * n, 0));
        return simd_mul(projection, view);
    }
};

} // namespace

int main(int argc, char** argv) {
    SetUnhandledExceptionFilter(crashFilter);
    try {
        if (argc < 3) {
            std::fprintf(stderr, "usage: opensims2_win <TSData/Res> <lot.package> [--screenshot file.png]\n");
            return 2;
        }
        const std::filesystem::path res = argv[1];
        const std::filesystem::path lotFile = argv[2];
        std::filesystem::path shot = "frame.png";
        bool exitAfterShot = false;
        for (int i = 3; i < argc; ++i) {
            if (std::string(argv[i]) == "--screenshot" && i + 1 < argc) {
                shot = argv[++i];
                exitAfterShot = true;
            }
        }

        std::printf("opensims2: indexing %s\n", res.string().c_str());
        std::fflush(stdout);
        os2::ResourceIndex index(res);
        os2::DbpfPackage package(lotFile);
        os2::Lot lot = os2::Lot::load(package);
        os2::vm::dropLeftoverCObjects(lot, package, index);
        os2::PatternCatalog patterns(index, res);
        std::printf("opensims2: lot %ux%u, building the house\n", lot.width, lot.height);
        std::fflush(stdout);
        const char* vmOnly = std::getenv("OS2_VM_ONLY");
        if (vmOnly && vmOnly[0] == '1') {
            std::fprintf(stderr, "opensims2: VM only, starting SimAntics\n");
            std::fflush(stderr);
            os2::LotSimulation simulation(index, lot, package, lotFile);
            std::printf("opensims2: %s\n", simulation.summary().c_str());
            std::fflush(stdout);
            int people = 0;
            for (const auto& [id, object] : simulation.machine().objects()) {
                if (!object.person || !object.position.inWorld())
                    continue;
                ++people;
                if (people <= 12)
                    std::fprintf(stderr, "opensims2: vm sim %d '%s' at %.2f %.2f\n", id,
                                 object.definition ? object.definition->fileName.c_str() : "?", object.position.x,
                                 object.position.y);
            }
            std::fprintf(stderr, "opensims2: sims in world %d\n", people);
            std::fflush(stderr);
            for (int i = 0; i < 30; ++i)
                simulation.update(i * 0.042);
            std::printf("opensims2: ticks %llu\n", (unsigned long long)simulation.ticks());
            std::fflush(stdout);
            return 0;
        }
        os2::LotViewOptions view;
        view.walls = os2::LotViewOptions::Walls::Up;
        view.roof = true;
        view.level = os2::LotViewOptions::kAllLevels;
        os2::LotLightMapLayout layout;
        os2::LotModelCache cache;
        const os2::ObjectModel house = os2::buildLotModel(lot, index, patterns, view, &layout, &cache);
        for (const auto& warning : house.warnings)
            std::fprintf(stderr, "opensims2: %s\n", warning.c_str());

        wchar_t modulePath[MAX_PATH];
        GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
        std::printf("opensims2: house has %zu parts, uploading\n", house.parts.size());
        std::fflush(stdout);
        const auto exe = std::filesystem::path(modulePath).parent_path();
        os2::GpuRenderer renderer(exe / "shaders", 1280, 720);
        std::printf("opensims2: GPU %s\n", renderer.backend().c_str());
        renderer.setGroundClip(os2::lotSkirtBottom(lot));
        const int houseModel = renderer.addObject(house, index);
        renderer.addInstance(houseModel, matrix_identity_float4x4);

        const os2::LightingConfig lighting = os2::loadLightingConfig(res / "Lights" / "Lighting.txt");
        if (const auto* state = lighting.state("day")) {
            const os2::RgbaImage lightMap =
                os2::computeLotLightMap(layout, *state, {}, os2::LightMapGlobals::from(lighting)).image;
            renderer.setLightMap(lightMap);
            renderer.setLighting(*state);
            std::printf("opensims2: light map %ux%u, lights %d objects / %d Sims\n", lightMap.width, lightMap.height,
                        renderer.objectLightCount(), renderer.simLightCount());
        }
        simd_float3 shadowTravel{0, 0, -1};
        simd_float3 shadowColor{0.55f, 0.55f, 0.62f};
        if (const auto* state = lighting.state("day")) {
            for (const auto& light : state->lights) {
                std::string name = light.name;
                for (char& c : name)
                    c = char(std::tolower(static_cast<unsigned char>(c)));
                if (name != "sun")
                    continue;
                const simd_float3 direction{light.direction[0], light.direction[1], light.direction[2]};
                if (simd_length(direction) > 1e-4f && direction.z > 0)
                    shadowTravel = -simd_normalize(direction);
            }
            const float strength = state->projectiveShadowStrength >= 0 ? state->projectiveShadowStrength : 0.73f;
            const float kept = std::max(0.f, 1.f - strength);
            shadowColor = simd_float3{kept, kept, std::min(1.f, kept + 0.15f)};
        }

        std::map<std::string, os2::Tgi> definitions;
        std::map<std::string, int> models;
        std::map<std::string, os2::Skeleton> skeletons;
        size_t objects = 0;
        for (const auto& object : lot.objects) {
            if (object.model.empty() || object.person || object.model == "nullModel" || object.model == "noModel" ||
                object.model == "null" || object.model == "placeholder")
                continue;
            const bool contained = object.position[0] == 0 && object.position[1] == 0;
            std::string key = cresOf(index, object, definitions);
            for (const auto& [subset, material] : object.materials)
                key += "|" + subset + "=" + material;
            if (!models.count(key)) {
                try {
                    const os2::ObjectModel model = os2::loadObjectModel(index, cresOf(index, object, definitions), object.materials);
                    skeletons[key] = model.skeleton;
                    models[key] = renderer.addObject(model, index);
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "opensims2: %s: %s\n", object.model.c_str(), e.what());
                    models[key] = -1;
                }
            }
            int model = models[key];
            if (!object.nodes.empty() && skeletons.count(key)) {
                if (auto pose = os2::savedPose(skeletons[key], object.nodes)) {
                    const std::string posedKey = key + "@posed";
                    if (!models.count(posedKey)) {
                        try {
                            models[posedKey] = renderer.addObject(
                                os2::posedModel(os2::loadObjectModel(index, cresOf(index, object, definitions), object.materials), *pose),
                                index);
                        } catch (...) {
                            models[posedKey] = -1;
                        }
                    }
                    if (models[posedKey] >= 0)
                        model = models[posedKey];
                }
            }
            if (model < 0)
                continue;
            simd_float4x4 at = placementOf(object);
            if (contained)
                at.columns[3] = simd_make_float4(0, 0, -1000, 1);
            else
                ++objects;
            renderer.addInstance(model, at);
        }

        std::fprintf(stderr, "opensims2: placed %zu objects, starting SimAntics\n", objects);
        std::fflush(stderr);
        os2::LotSimulation simulation(index, lot, package, lotFile);
        std::printf("opensims2: %s\n", simulation.summary().c_str());
        const auto parts = os2::loadSimParts(index);
        struct DrawnSim {
            std::unique_ptr<os2::SimActor> actor;
            int model = -1;
            int16_t id = 0;
            int instance = -1;
        };
        std::vector<DrawnSim> sims;
        os2::LotObjects saved = os2::LotObjects::load(package);
        std::set<uint32_t> inWorld;
        for (const auto& state : saved.objects)
            if (state.person && state.x >= 0)
                inWorld.insert(uint32_t(uint16_t(state.id)));
        for (const auto& object : lot.objects) {
            if (!object.person || !inWorld.count(object.instance & 0xFFFF))
                continue;
            const std::string stem = object.definition.substr(0, object.definition.find(" - "));
            const std::filesystem::path file = lotFile.parent_path().parent_path() / "Characters" / (stem + ".package");
            try {
                auto actor = std::make_unique<os2::SimActor>(os2::loadCharacterSim(index, parts, file));
                actor->position = {object.position[0], object.position[1], object.position[2]};
                actor->heading = 2 * std::atan2(object.rotation[2], object.rotation[3]);
                const std::string idle = idleFor(actor->skin);
                actor->cue(0, actor->animation(index, idle));
                actor->update(0.05);
                const int model = renderer.addObject(actor->skin.model, index, true);
                const int instance = renderer.addInstance(model, matrix_identity_float4x4);
                renderer.setInstanceLightSet(instance, 1);
                renderer.updateObject(model, actor->posed);
                os2::DbpfPackage characterPackage(file);
                const os2::Character character = os2::readCharacter(characterPackage, index);
                simulation.attach(*actor, int16_t(object.instance & 0xFFFF), character, idle);
                std::printf("opensims2: Sim %s at (%.2f, %.2f, %.2f), %zu parts, idle %s\n", stem.c_str(), object.position[0],
                            object.position[1], object.position[2], actor->skin.model.parts.size(), idle.c_str());
                sims.push_back({std::move(actor), model, int16_t(object.instance & 0xFFFF), instance});
            } catch (const std::exception& e) {
                std::fprintf(stderr, "opensims2: Sim %s: %s\n", object.definition.c_str(), e.what());
            }
        }
        if (sims.empty())
            throw std::runtime_error("no Sim could be drawn");

        os2::GameAudio gameAudio(res, {});
        os2::LotAudio lotAudio(gameAudio, index, lot, {});
        for (auto& sim : sims)
            lotAudio.attachSim(*sim.actor, sim.id, {});
        std::printf("opensims2: audio device %s\n", gameAudio.hasDevice() ? "on" : "off");
        std::fflush(stdout);

        std::string movieLabel;
        int movieW = 0, movieH = 0;
        long movieFrame = -1;
        try {
            const std::filesystem::path moviePath = os2::findMovie(res, "ealogo_audio");
            if (!moviePath.empty()) {
                os2::Movie movie = os2::Movie::load(moviePath);
                os2::MovieDecoder decoder(movie);
                os2::RgbaImage picture;
                const double times[] = {0.4, 0.8, 1.2, std::max(0.1, movie.duration() * 0.5)};
                double shownAt = 0;
                for (double t : times) {
                    decoder.seek(t);
                    decoder.picture(picture);
                    uint64_t sum = 0;
                    const size_t n = picture.width * picture.height;
                    for (size_t i = 0; i + 2 < picture.pixels.size(); i += 4)
                        sum += picture.pixels[i] + picture.pixels[i + 1] + picture.pixels[i + 2];
                    shownAt = t;
                    if (n > 0 && double(sum) / double(n * 3) > 12)
                        break;
                }
                movieW = int(picture.width);
                movieH = int(picture.height);
                movieFrame = decoder.current();
                movieLabel = moviePath.filename().string();
                const auto movieShot = shot.parent_path() / "frame_movie.png";
                if (renderer.captureMovie(movieShot, picture))
                    std::printf("opensims2: movie %s %dx%d frame %ld at %.2fs -> %s\n", movieLabel.c_str(), movieW, movieH,
                                movieFrame, shownAt, movieShot.string().c_str());
                if (movie.hasSound()) {
                    auto pcm = std::make_shared<const os2::Pcm>(movie.decodeSound());
                    if (!pcm->samples.empty()) {
                        gameAudio.mixer().play(pcm, {});
                        std::printf("opensims2: movie sound %u Hz %u ch %.2fs\n", pcm->sampleRate, pcm->channels, pcm->seconds());
                    }
                }
            } else {
                std::fprintf(stderr, "opensims2: ealogo_audio.movie not found\n");
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "opensims2: movie: %s\n", e.what());
        }

        os2::UiResources ui(index, res);
        float ux = 16, uy = 16, uw = 128, uh = 128;
        if (const os2::RgbaImage* image = firstUiImage(ui, index, ux, uy, uw, uh)) {
            const float scale = 1280.f / 1024.f;
            renderer.setUiImage(*image, ux * scale, uy * scale, std::max(uw * scale, 64.f), std::max(uh * scale, 64.f));
            std::printf("opensims2: UI image %ux%u\n", image->width, image->height);
        } else {
            std::fprintf(stderr, "opensims2: no UI image found\n");
        }

        std::unique_ptr<os2::fx::EffectsLibrary> effectLibrary;
        std::unique_ptr<os2::fx::EffectSystem> effectSystem;
        std::string effectName;
        try {
            for (const auto& loc : index.ofType(os2::fx::kTypeEffects)) {
                effectLibrary = std::make_unique<os2::fx::EffectsLibrary>(os2::fx::EffectsLibrary::parse(index.read(loc)));
                break;
            }
            if (effectLibrary && !sims.empty()) {
                effectSystem = std::make_unique<os2::fx::EffectSystem>(*effectLibrary);
                os2::fx::Placement where;
                where.position = {sims.front().actor->position[0], sims.front().actor->position[1],
                                  sims.front().actor->position[2] + 1.2f};
                auto consider = [&](const std::string& name) {
                    if (!effectName.empty())
                        return;
                    const int handle = effectSystem->start(name, where);
                    if (handle < 0)
                        return;
                    effectSystem->update(0.4f);
                    std::vector<os2::fx::ParticleQuad> quads;
                    effectSystem->collect(quads);
                    if (!quads.empty())
                        effectName = name;
                    else
                        effectSystem->stop(handle, true);
                };
                for (const auto& [name, id] : effectLibrary->names) {
                    (void)id;
                    std::string lower = name;
                    for (char& c : lower)
                        c = char(std::tolower(static_cast<unsigned char>(c)));
                    if (lower.find("smoke") != std::string::npos || lower.find("steam") != std::string::npos ||
                        lower.find("heart") != std::string::npos || lower.find("fire") != std::string::npos)
                        consider(name);
                }
                if (effectName.empty()) {
                    int tried = 0;
                    for (const auto& [name, id] : effectLibrary->names) {
                        (void)id;
                        consider(name);
                        if (!effectName.empty() || ++tried >= 60)
                            break;
                    }
                }
                if (!effectName.empty()) {
                    std::vector<os2::fx::ParticleQuad> quads;
                    effectSystem->collect(quads);
                    std::printf("opensims2: effect %s, %zu particles\n", effectName.c_str(), quads.size());
                } else {
                    std::printf("opensims2: effects loaded, no particles from the first names\n");
                }
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "opensims2: effects: %s\n", e.what());
            effectSystem.reset();
        }

        Camera camera;
        if (!sims.empty())
            camera.pivot = simd_make_float3(sims.front().actor->position[0], sims.front().actor->position[1], 1.f);
        else
            camera.pivot = simd_make_float3(float(lot.width) / 2, float(lot.height) / 2, 0);
        camera.distance = 12.f;
        const auto start = std::chrono::steady_clock::now();
        auto seconds = [&] {
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        };
        bool keys[4]{};
        auto frame = [&] {
            const double now = seconds();
            const double scene = simulation.update(now);
            for (auto& sim : sims) {
                sim.actor->update(scene);
                renderer.updateObject(sim.model, sim.actor->posed);
            }
            os2::LotAudio::Listener listener;
            listener.x = camera.pivot.x;
            listener.y = camera.pivot.y;
            listener.z = camera.pivot.z;
            listener.rightX = std::sin(camera.yaw);
            listener.rightY = -std::cos(camera.yaw);
            listener.zoom = std::clamp((28.f - camera.distance) / 22.f, 0.f, 1.f);
            lotAudio.update(&simulation.machine(), scene, listener);
            const float speed = 12.f * (1.f / 60.f);
            if (keys[0])
                camera.pivot.x -= speed;
            if (keys[1])
                camera.pivot.x += speed;
            if (keys[2])
                camera.pivot.y -= speed;
            if (keys[3])
                camera.pivot.y += speed;
            std::vector<os2::GpuRenderer::ShadowCaster> casters;
            for (const auto& sim : sims) {
                os2::GpuRenderer::ShadowCaster caster;
                caster.instance = sim.instance;
                caster.feet = simd_make_float3(sim.actor->position[0], sim.actor->position[1], sim.actor->position[2]);
                caster.heading = sim.actor->heading;
                int level = layout.minLevel;
                for (int l = layout.minLevel; l < layout.minLevel + layout.levels; ++l)
                    if (std::fabs(layout.elevationAt(l, caster.feet.x, caster.feet.y) - caster.feet.z) <
                        std::fabs(layout.elevationAt(level, caster.feet.x, caster.feet.y) - caster.feet.z))
                        level = l;
                caster.outside = layout.outdoor(level, layout.roomAt(level, caster.feet.x, caster.feet.y));
                caster.direction = caster.outside ? shadowTravel : simd_float3{0, 0, -1};
                caster.color = shadowColor;
                casters.push_back(caster);
            }
            renderer.setShadowCasters(casters);
            std::vector<os2::GpuRenderer::CensorBox> censor;
            const char* censorOff = std::getenv("OS2_NO_CENSOR");
            const char* censorForce = std::getenv("OS2_CENSOR");
            if (!(censorOff && censorOff[0] == '1')) {
                for (const auto& sim : sims) {
                    uint16_t flags = 0;
                    bool female = false;
                    if (const os2::vm::Object* object = simulation.machine().object(sim.id); object && object->person) {
                        flags = uint16_t(object->person->data[size_t(os2::censor::kPersonData)]);
                        female = object->person->data[65] == 1;
                    }
                    if (censorForce && censorForce[0] == '1' && flags == 0)
                        flags = os2::censor::FullBody;
                    for (const auto& box : os2::censorBoxes(*sim.actor, flags, female)) {
                        os2::GpuRenderer::CensorBox gpu;
                        gpu.lo = simd_make_float3(box.lo[0], box.lo[1], box.lo[2]);
                        gpu.hi = simd_make_float3(box.hi[0], box.hi[1], box.hi[2]);
                        censor.push_back(gpu);
                    }
                }
            }
            renderer.setCensorBoxes(censor);
            static bool censorLogged = false;
            if (!censorLogged) {
                censorLogged = true;
                std::fprintf(stderr, "opensims2: censor boxes %zu\n", censor.size());
            }
            if (effectSystem) {
                effectSystem->update(1.f / 60.f);
                std::vector<os2::fx::ParticleQuad> quads;
                effectSystem->collect(quads);
                simd_float3 right, up;
                camera.basis(right, up);
                renderer.setParticleQuads(quads, right, up, &index);
            }
            renderer.setEye(camera.eye());
            return renderer.draw(camera.viewProj(1280.f / 720.f));
        };

        // A few ticks so the capture shows the VM's animation, not the rest pose.
        for (int i = 0; i < 30; ++i)
            frame();
        renderer.setEye(camera.eye());
        if (!renderer.capture(shot, camera.viewProj(1280.f / 720.f)))
            throw std::runtime_error("screenshot failed");
        const int censorOnFrame = renderer.censorCount();
        simd_float3 mirrorCenter, mirrorNormal;
        if (renderer.mirrorAnchor(mirrorCenter, mirrorNormal)) {
            camera.pivot = mirrorCenter;
            camera.yaw = std::atan2(mirrorNormal.y, mirrorNormal.x);
            camera.pitch = 0.f;
            camera.distance = 1.6f;
            renderer.setEye(camera.eye());
            const auto mirrorShot = shot.parent_path() / "frame_mirror.png";
            if (renderer.capture(mirrorShot, camera.viewProj(1280.f / 720.f)))
                std::printf("opensims2: mirror at (%.2f, %.2f, %.2f) normal (%.2f, %.2f, %.2f) -> %s\n", mirrorCenter.x,
                            mirrorCenter.y, mirrorCenter.z, mirrorNormal.x, mirrorNormal.y, mirrorNormal.z,
                            mirrorShot.string().c_str());
        } else {
            std::printf("opensims2: no mirror in the lot\n");
        }
        std::ofstream log(shot.parent_path() / "frame_vm.txt");
        if (shot.parent_path().empty())
            log = std::ofstream("frame_vm.txt");
        log << simulation.summary() << "\n";
        log << "ticks " << simulation.ticks() << "\n";
        log << "shadows " << renderer.shadowCount() << "\n";
        log << "censor " << censorOnFrame << "\n";
        log << "movie " << movieLabel << " " << movieW << "x" << movieH << " frame " << movieFrame << "\n";
        log << "particles " << renderer.particleCount() << " effect " << effectName << "\n";
        log << "audio " << (gameAudio.hasDevice() ? "on" : "off") << " frames " << gameAudio.mixer().framesRendered()
            << " voices " << gameAudio.mixer().voiceCount() << "\n";
        log << lotAudio.summary() << "\n";
        if (os2::haveMp3Decoder()) {
            try {
                bool found = false;
                for (const auto& loc : index.ofType(os2::type::AUDIO)) {
                    const auto bytes = index.read(loc);
                    if (os2::detectAudioFormat(bytes) != os2::AudioFormat::Mp3)
                        continue;
                    const os2::Pcm pcm = os2::decodeMp3(bytes);
                    std::printf("opensims2: mp3 %u Hz %u ch %.2fs peak %d\n", pcm.sampleRate, pcm.channels, pcm.seconds(),
                                pcm.peak());
                    log << "mp3 " << pcm.sampleRate << " Hz " << pcm.channels << " ch " << pcm.seconds() << " s peak "
                        << pcm.peak() << "\n";
                    found = true;
                    break;
                }
                if (!found)
                    std::fprintf(stderr, "opensims2: no MP3 resource\n");
            } catch (const std::exception& e) {
                std::fprintf(stderr, "opensims2: mp3: %s\n", e.what());
            }
        } else {
            std::fprintf(stderr, "opensims2: MP3 decoder unavailable\n");
        }
        log << "objects drawn " << objects << "\n";
        log << "sims drawn " << sims.size() << "\n";
        for (const auto& [id, object] : simulation.machine().objects()) {
            if (!object.person)
                continue;
            log << "sim " << id << " " << (object.definition ? object.definition->fileName : "?") << " at " << object.position.x
                << " " << object.position.y << " anim '" << object.animation.name << "'\n";
        }
        log.close();
        std::printf("opensims2: wrote %s (%zu objects, %zu Sims, %llu ticks)\n", shot.string().c_str(), objects, sims.size(),
                    (unsigned long long)simulation.ticks());
        if (exitAfterShot)
            return 0;

        bool running = true;
        while (running) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_EVENT_QUIT)
                    running = false;
                if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) {
                    const bool down = event.type == SDL_EVENT_KEY_DOWN;
                    switch (event.key.scancode) {
                    case SDL_SCANCODE_A: keys[0] = down; break;
                    case SDL_SCANCODE_D: keys[1] = down; break;
                    case SDL_SCANCODE_S: keys[2] = down; break;
                    case SDL_SCANCODE_W: keys[3] = down; break;
                    case SDL_SCANCODE_Q: if (down) camera.yaw -= 0.08f; break;
                    case SDL_SCANCODE_E: if (down) camera.yaw += 0.08f; break;
                    case SDL_SCANCODE_ESCAPE: running = false; break;
                    default: break;
                    }
                }
                if (event.type == SDL_EVENT_MOUSE_WHEEL)
                    camera.distance = std::clamp(camera.distance - event.wheel.y * 1.5f, 6.f, 80.f);
            }
            if (!frame())
                running = false;
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "opensims2: %s\n", e.what());
        return 1;
    }
}
