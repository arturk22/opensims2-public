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
#include "app/live_ui.hpp"
#include "app/game_save.hpp"
#include "app/build_mode.hpp"
#include "app/buy_mode.hpp"
#include "app/cas_mode.hpp"
#include "render/camera.hpp"
#include "app/lot_lighting.hpp"
#include "app/vm_objects.hpp"
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
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
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

std::filesystem::path exeDir() {
    wchar_t modulePath[MAX_PATH];
    GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    return std::filesystem::path(modulePath).parent_path();
}

// The macOS virtual key codes CasMode reads (Backspace, Return, Z, Y, comma, period).
unsigned short macKeyCode(SDL_Scancode code) {
    switch (code) {
    case SDL_SCANCODE_BACKSPACE: return 51;
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return 36;
    case SDL_SCANCODE_Z: return 6;
    case SDL_SCANCODE_Y: return 16;
    case SDL_SCANCODE_COMMA: return 43;
    case SDL_SCANCODE_PERIOD: return 47;
    default: return 0xFFFF;
    }
}

// --cas: Create-a-Sim (app/cas_mode.hpp), as the Mac host's prepareCas/finishCas.
// Families are written under --cas-out (default: cas-families next to the exe,
// so the game's own neighborhoods are left alone).
int runCas(const std::filesystem::path& res, std::vector<std::string> args, const std::optional<std::filesystem::path>& shot) {
    auto argument = [&](std::string_view name) -> std::optional<std::string> {
        for (size_t i = 0; i + 1 < args.size(); ++i)
            if (args[i] == name)
                return args[i + 1];
        return std::nullopt;
    };
    const std::filesystem::path exe = exeDir();
    if (!argument("--cas-out")) {
        args.push_back("--cas-out");
        args.push_back((exe / "cas-families").string());
    }
    uint32_t w = 1280, h = 720;
    if (auto size = argument("--cas-size"))
        std::sscanf(size->c_str(), "%ux%u", &w, &h);
    std::printf("opensims2: indexing %s\n", res.string().c_str());
    std::fflush(stdout);
    os2::ResourceIndex index(res);
    os2::Renderer renderer(exe / "shaders", w, h);
    std::printf("opensims2: GPU %s\n", renderer.backend().c_str());
    const os2::LightingConfig lighting = os2::loadLightingConfig(res / "Lights" / "Lighting.txt");
    if (const auto* state = lighting.state(argument("--lighting").value_or("CAS"))) {
        renderer.setLighting(*state);
        if (!argument("--lighting")) {
            // The studio lamps of GlobalLots/CAS!.package replace these once it loads; without
            // the studio its boom light stands in from the camera's side, above (as the Mac host).
            auto set = os2::Renderer::outdoorLights(*state, true);
            set.lights.push_back({simd_normalize(simd_make_float3(0.35f, 1.0f, 0.9f)), simd_make_float3(1.0f, 0.95f, 0.85f) * 1.25f * 0.6f});
            renderer.setLightSets({set, set});
        }
    }
    os2::GameAudio gameAudio(res, {});
    const std::filesystem::path gameDir = res.parent_path().parent_path();
    os2::CasMode cas(renderer, index, gameDir, args);
    bool running = true;
    cas.playSound = [&](const std::string& name) { gameAudio.playUi(name); };
    cas.onExit = [&] { running = false; };
    cas.onFamilySaved = [](uint32_t family) { std::printf("opensims2: cas: family %u saved\n", family); };
    std::unique_ptr<os2::LotCamera> camera = cas.makeCamera();
    cas.setCamera(camera.get());
    const simd_float4 background = simd_make_float4(0.33f, 0.36f, 0.45f, 1.f);
    renderer.setClearColor(background);
    cas.setCanvas(int(w), int(h));
    camera->setViewport(float(w), float(h));
    auto showUi = [&] {
        os2::UiScreen screen;
        if (!cas.layout(int(w), int(h), screen))
            return false;
        renderer.setUiImage(os2::composeUi(screen, {0, 0, 0, 0}), 0, 0, float(w), float(h));
        return true;
    };
    auto viewProj = [&] { return simd_mul(camera->projection(float(w) / float(h)), camera->view()); };
    SDL_SetWindowTitle(renderer.window(), cas.title().c_str());
    cas.runScripted(args);

    if (shot) {
        camera->snap();
        cas.update(0.5);
        camera->snap();
        std::ofstream log(shot->parent_path() / "frame_cas.txt");
        for (const auto& line : cas.report()) {
            std::printf("opensims2: %s\n", line.c_str());
            log << line << "\n";
        }
        // The scene alone through the offscreen path portraits use, then the window with the UI.
        const os2::RgbaImage scene = renderer.capture(w, h, os2::FrameView{camera->view(), camera->projection(float(w) / float(h))}, background);
        log << "offscreen " << scene.width << "x" << scene.height << "\n";
        showUi();
        renderer.setEye(camera->eye());
        if (!renderer.capture(*shot, viewProj()))
            return 1;
        std::printf("opensims2: wrote %s\n", shot->string().c_str());
        return 0;
    }

    SDL_StartTextInput(renderer.window());
    bool dragging = false;
    const auto start = std::chrono::steady_clock::now();
    auto last = start;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            const bool control = (SDL_GetModState() & SDL_KMOD_CTRL) != 0;
            switch (event.type) {
            case SDL_EVENT_QUIT: running = false; break;
            case SDL_EVENT_KEY_DOWN:
                if (event.key.scancode == SDL_SCANCODE_ESCAPE)
                    running = false;
                else
                    cas.keyDown(macKeyCode(event.key.scancode), "", control);
                break;
            case SDL_EVENT_KEY_UP: cas.keyUp(macKeyCode(event.key.scancode)); break;
            case SDL_EVENT_TEXT_INPUT: cas.keyDown(0xFFFF, event.text.text, control); break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (event.button.button == SDL_BUTTON_LEFT) {
                    dragging = true;
                    cas.mouseDown(simd_make_float2(event.button.x, event.button.y));
                }
                break;
            case SDL_EVENT_MOUSE_MOTION:
                if (dragging)
                    cas.mouseDragged(simd_make_float2(event.motion.x, event.motion.y));
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (event.button.button == SDL_BUTTON_LEFT) {
                    dragging = false;
                    cas.mouseUp(simd_make_float2(event.button.x, event.button.y));
                }
                break;
            case SDL_EVENT_MOUSE_WHEEL: camera->wheel(event.wheel.y); break;
            default: break;
            }
        }
        const auto now = std::chrono::steady_clock::now();
        camera->update(std::chrono::duration<double>(now - last).count());
        last = now;
        cas.update(std::chrono::duration<double>(now - start).count());
        showUi();
        renderer.setEye(camera->eye());
        if (!renderer.draw(viewProj()))
            running = false;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    SetUnhandledExceptionFilter(crashFilter);
    try {
        if (argc < 3) {
            std::fprintf(stderr, "usage: opensims2_win <TSData/Res> <lot.package | --cas> [--screenshot file.png]\n");
            return 2;
        }
        const std::filesystem::path res = argv[1];
        if (std::string(argv[2]) == "--cas") {
            std::vector<std::string> args(argv + 2, argv + argc);
            std::optional<std::filesystem::path> casShot;
            for (size_t i = 0; i + 1 < args.size(); ++i)
                if (args[i] == "--screenshot")
                    casShot = args[i + 1];
            return runCas(res, args, casShot);
        }
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
        os2::Renderer renderer(exe / "shaders", 1280, 720);
        std::printf("opensims2: GPU %s\n", renderer.backend().c_str());
        renderer.setGroundClip(os2::lotSkirtBottom(lot));
        const int houseModel = renderer.addObject(house, index);
        renderer.addInstance(houseModel, matrix_identity_float4x4);

        // Fence rails and posts: a model per CRES, an instance per piece; called
        // again after Build mode changed the fences (extra instances hidden).
        std::map<std::string, int> fenceModels;
        std::vector<int> fenceInstances;
        auto placeFences = [&] {
            size_t used = 0;
            for (const auto& piece : os2::lotFencePieces(lot, patterns)) {
                auto it = fenceModels.find(piece.model);
                if (it == fenceModels.end()) {
                    int id = -1;
                    try {
                        id = renderer.addObject(os2::loadObjectModel(index, piece.model), index);
                    } catch (const std::exception& e) {
                        std::fprintf(stderr, "opensims2: fence %s: %s\n", piece.model.c_str(), e.what());
                    }
                    it = fenceModels.emplace(piece.model, id).first;
                }
                if (it->second < 0)
                    continue;
                simd_float4x4 at;
                std::memcpy(&at, piece.transform.data(), sizeof at);
                if (used < fenceInstances.size()) {
                    renderer.setInstanceModel(fenceInstances[used], it->second);
                    renderer.setInstanceTransform(fenceInstances[used], at);
                } else {
                    fenceInstances.push_back(renderer.addInstance(it->second, at));
                }
                ++used;
            }
            simd_float4x4 away = matrix_identity_float4x4;
            away.columns[3] = simd_make_float4(0, 0, -1000, 1);
            for (size_t i = used; i < fenceInstances.size(); ++i)
                renderer.setInstanceTransform(fenceInstances[i], away);
            return used;
        };
        std::printf("opensims2: %zu fence pieces\n", placeFences());

        const os2::LightingConfig lighting = os2::loadLightingConfig(res / "Lights" / "Lighting.txt");
        // Redraws the lot after Build mode changed it: the model, the fences and the light map.
        int lotRebuilds = 0;
        std::unique_ptr<os2::LotLighting> lotLighting;
        auto rebuildLot = [&] {
            cache.geometryChanged();
            layout = os2::LotLightMapLayout{};
            const os2::ObjectModel model = os2::buildLotModel(lot, index, patterns, view, &layout, &cache);
            renderer.replaceObject(houseModel, model, index);
            renderer.setGroundClip(os2::lotSkirtBottom(lot));
            placeFences();
            if (lotLighting)
                lotLighting->setLightMapLayout(layout);
            else if (const auto* state = lighting.state("day"))
                renderer.setLightMap(os2::computeLotLightMap(layout, *state, {}, os2::LightMapGlobals::from(lighting)).image);
            ++lotRebuilds;
        };
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
        struct PlacedObject {
            std::string cres;
            std::map<std::string, std::string> materials;
            std::optional<os2::Xform> saved; // nothing: saved in a slot
            std::string definition;
            int instance = -1;
            std::vector<os2::Xform> pose;
        };
        std::map<int16_t, PlacedObject> placed;
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
            std::vector<os2::Xform> savedPose;
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
                    if (models[posedKey] >= 0) {
                        model = models[posedKey];
                        savedPose = std::move(*pose);
                    }
                }
            }
            if (model < 0)
                continue;
            simd_float4x4 at = placementOf(object);
            if (contained)
                at.columns[3] = simd_make_float4(0, 0, -1000, 1);
            else
                ++objects;
            const int instance = renderer.addInstance(model, at);
            // Stale cObjects share IDs with live ones: the first (or one in the
            // world) keeps the ID; app/vm_objects.cpp checks it against the VM.
            const int16_t id = int16_t(object.instance & 0xFFFF);
            if (!placed.count(id) || (!placed[id].saved && !contained)) {
                PlacedObject p{cresOf(index, object, definitions), object.materials, std::nullopt, object.definition,
                               instance, std::move(savedPose)};
                if (!contained)
                    p.saved = os2::Xform{os2::Quat{object.rotation[0], object.rotation[1], object.rotation[2], object.rotation[3]},
                                         {object.position[0], object.position[1], object.position[2]}};
                placed[id] = std::move(p);
            }
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
                actor->model = model;
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

        os2::VmObjectView objectView(renderer, index, simulation);
        simulation.seats.slot = [&](int16_t id, double now) { return objectView.slotPlacement(id, now); };
        simulation.objectBone = [&](int16_t id, uint32_t crc) { return objectView.boneByHash(id, crc); };
        int following = 0;
        for (const auto& [id, p] : placed)
            following += objectView.addPlaced(id, p.definition, p.cres, p.instance, p.saved, p.materials, p.pose) ? 1 : 0;
        for (auto& sim : sims)
            objectView.addSim(sim.id, sim.actor.get());
        std::printf("opensims2: object view %zu placed, %d follow the VM\n", placed.size(), following);
        try {
            lotLighting = std::make_unique<os2::LotLighting>(renderer, index, simulation, objectView, lighting,
                                                             os2::loadLightOverrides(res / "Lights" / "LotLights.txt"));
            for (auto& sim : sims)
                lotLighting->addSim(sim.id, sim.actor.get());
            lotLighting->setLightMapLayout(layout);
            if (const char* state = std::getenv("OS2_LIGHTING"))
                lotLighting->forceState(state);
            // Test aid (main.mm --lamps): lamps the VM has not switched shine at this level.
            if (const char* lamps = std::getenv("OS2_LAMPS"))
                lotLighting->setDefaultLampLevel(std::atoi(lamps));
        } catch (const std::exception& e) {
            std::fprintf(stderr, "opensims2: lighting: %s\n", e.what());
            lotLighting.reset();
        }

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

        std::unique_ptr<os2::LiveUi> liveUi;
        std::unique_ptr<os2::BuyMode> buy;
        std::unique_ptr<os2::BuildMode> build;
        int liveWindows = 0, liveDrawn = 0, buyWindows = 0, buyDrawn = 0, buildWindows = 0, buildDrawn = 0;
        auto showLiveUi = [&] {
            if (!liveUi || !liveUi->ok())
                return false;
            if (!sims.empty())
                liveUi->setSim(sims.front().id);
            os2::UiScreen screen = liveUi->screen(1280, 720);
            liveWindows = int(screen.windows);
            liveDrawn = int(screen.drawn);
            os2::UiScreen buyScreen;
            if (buy && buy->active()) {
                buy->update();
                buyScreen = buy->screen(1280, 720);
                buyWindows = int(buyScreen.windows);
                buyDrawn = int(buyScreen.drawn);
                screen.quads.insert(screen.quads.end(), buyScreen.quads.begin(), buyScreen.quads.end());
            }
            os2::UiScreen buildScreen;
            if (build && build->active()) {
                build->update();
                buildScreen = build->screen(1280, 720);
                buildWindows = int(buildScreen.windows);
                buildDrawn = int(buildScreen.drawn);
                screen.quads.insert(screen.quads.end(), buildScreen.quads.begin(), buildScreen.quads.end());
            }
            renderer.setUiImage(os2::composeUi(screen, {0, 0, 0, 0}), 0, 0, 1280, 720);
            return true;
        };
        try {
            liveUi = std::make_unique<os2::LiveUi>(simulation.machine(), index, res);
            if (showLiveUi())
                std::printf("opensims2: live UI %d windows, %d drawn\n", liveWindows, liveDrawn);
            else
                std::fprintf(stderr, "opensims2: live UI scripts missing\n");
        } catch (const std::exception& e) {
            std::fprintf(stderr, "opensims2: live UI: %s\n", e.what());
        }
        if (!liveUi || !liveUi->ok()) {
            os2::UiResources ui(index, res);
            float ux = 16, uy = 16, uw = 128, uh = 128;
            if (const os2::RgbaImage* image = firstUiImage(ui, index, ux, uy, uw, uh)) {
                const float scale = 1280.f / 1024.f;
                renderer.setUiImage(*image, ux * scale, uy * scale, std::max(uw * scale, 64.f), std::max(uh * scale, 64.f));
                std::printf("opensims2: UI image %ux%u\n", image->width, image->height);
            } else {
                std::fprintf(stderr, "opensims2: no UI image found\n");
            }
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
        if (liveUi && liveUi->ok()) {
            liveUi->setCanvas(1280, 720);
            liveUi->playSound = [&](const std::string& name) { gameAudio.playUi(name); };
            liveUi->onZoom = [&](int step) { camera.distance = std::clamp(camera.distance - float(step) * 2.f, 6.f, 80.f); };
            liveUi->onRotate = [&](int step) { camera.yaw += float(step) * 0.4f; };
            liveUi->onCentreSim = [&](os2::vm::ObjectId id) {
                for (const auto& sim : sims)
                    if (sim.id == id)
                        camera.pivot = simd_make_float3(sim.actor->position[0], sim.actor->position[1], 1.f);
            };
            liveUi->onWallMode = [&](int mode) { liveUi->setWallMode(mode); };
            try {
                buy = std::make_unique<os2::BuyMode>(simulation, objectView, renderer, index, res);
                buy->setCanvas(1280, 720);
                buy->playSound = [&](const std::string& name) { gameAudio.playUi(name); };
                std::printf("opensims2: buy mode %s, %zu catalog entries\n", buy->ok() ? "ready" : "unavailable",
                            buy->catalog().entries().size());
            } catch (const std::exception& e) {
                std::fprintf(stderr, "opensims2: buy mode: %s\n", e.what());
                buy.reset();
            }
            try {
                build = std::make_unique<os2::BuildMode>(simulation, objectView, renderer, index, res, lot);
                build->setCanvas(1280, 720);
                build->onGeometryChanged = rebuildLot;
                build->playSound = [&](const std::string& name) { gameAudio.playUi(name); };
                std::printf("opensims2: build mode %s\n", build->ok() ? "ready" : "unavailable");
            } catch (const std::exception& e) {
                std::fprintf(stderr, "opensims2: build mode: %s\n", e.what());
                build.reset();
            }
            liveUi->onMode = [&](int mode) {
                if (mode == 1 && buy && buy->ok()) {
                    if (build)
                        build->exit();
                    buy->enter();
                    liveUi->setMode(1);
                } else if (mode == 2 && build && build->ok()) {
                    if (buy)
                        buy->exit();
                    build->enter();
                    liveUi->setMode(2);
                } else if (mode == 0) {
                    if (buy)
                        buy->exit();
                    if (build)
                        build->exit();
                    liveUi->setMode(0);
                }
            };
        }
        const auto start = std::chrono::steady_clock::now();
        auto seconds = [&] {
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        };
        bool keys[4]{};
        auto frame = [&] {
            const double now = seconds();
            if (liveUi)
                liveUi->tick(now);
            const double scene = simulation.update(now);
            for (auto& sim : sims) {
                sim.actor->update(scene);
                renderer.updateObject(sim.model, sim.actor->posed);
            }
            objectView.update();
            if (lotLighting)
                lotLighting->update(scene);
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
                if (lotLighting)
                    break; // LotLighting casts the Sims' shadows
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
            if (!lotLighting)
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
        bool pauseClick = false;
        if (liveUi && liveUi->ok()) {
            if (const auto rect = liveUi->controlRect(os2::LiveUi::kMainWindow, 0x27D7258F)) {
                const simd_float2 at{((*rect)[0] + (*rect)[2]) * 0.5f, ((*rect)[1] + (*rect)[3]) * 0.5f};
                pauseClick = liveUi->press(at);
            }
        }
        showLiveUi();
        renderer.setEye(camera.eye());
        if (!renderer.capture(shot, camera.viewProj(1280.f / 720.f)))
            throw std::runtime_error("screenshot failed");
        const int censorOnFrame = renderer.censorCount();
        const Camera homeCamera = camera;
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
        if (lotLighting) {
            size_t points = 0;
            for (const auto& set : renderer.lightSets())
                points += set.points.size();
            log << "lighting " << lotLighting->stateName() << "; light sets " << renderer.lightSets().size() << "; lamps "
                << lotLighting->lamps().size() << "; point lights " << points << "\n";
        } else {
            log << "lighting static day\n";
        }
        log << "censor " << censorOnFrame << "\n";
        log << "live-ui " << liveWindows << " windows " << liveDrawn << " drawn\n";
        log << "ui-pause " << (pauseClick ? "clicked" : "miss") << " vm "
            << simulation.machine().globals[os2::vm::global::Paused] << "\n";
        std::string saveLine = "save skipped";
        try {
            const auto originalStamp = std::filesystem::last_write_time(lotFile);
            const auto hoodDir = lotFile.parent_path().parent_path();
            const auto hoodName = hoodDir.filename().string() + "_Neighborhood.package";
            const auto originalHood = hoodDir / hoodName;
            const bool haveHood = std::filesystem::is_regular_file(originalHood);
            const auto hoodStamp = haveHood ? std::filesystem::last_write_time(originalHood) : std::filesystem::file_time_type{};
            const auto saveDir = shot.parent_path().empty() ? std::filesystem::path("save-test")
                                                            : shot.parent_path() / "save-test";
            const auto saveLot = saveDir / hoodDir.filename() / "Lots" / lotFile.filename();
            std::filesystem::create_directories(saveLot.parent_path());
            std::filesystem::copy_file(lotFile, saveLot, std::filesystem::copy_options::overwrite_existing);
            if (haveHood)
                std::filesystem::copy_file(originalHood, saveLot.parent_path().parent_path() / hoodName,
                                           std::filesystem::copy_options::overwrite_existing);
            os2::LotObjects before;
            {
                os2::DbpfPackage beforePackage(saveLot);
                before = os2::LotObjects::load(beforePackage);
            }
            const os2::GameSaveResult written =
                os2::saveGame(simulation.machine(), saveLot, res.parent_path().parent_path(), os2::sessionCharactersDir());
            const bool untouched = std::filesystem::last_write_time(lotFile) == originalStamp;
            if (!written.ok) {
                saveLine = "save failed: " + written.error;
            } else {
                os2::LotObjects after;
                {
                    os2::DbpfPackage afterPackage(saveLot);
                    after = os2::LotObjects::load(afterPackage);
                }
                int moved = 0;
                for (const auto& now : after.objects) {
                    if (!now.person || now.x < 0)
                        continue;
                    for (const auto& was : before.objects)
                        if (was.id == now.id && (std::fabs(now.x - was.x) > 0.01f || std::fabs(now.y - was.y) > 0.01f))
                            ++moved;
                }
                std::string hoodLine;
                for (const auto& line : written.lines)
                    if (line.rfind("neighborhood ", 0) == 0)
                        hoodLine = line;
                const bool hoodUntouched = !haveHood || std::filesystem::last_write_time(originalHood) == hoodStamp;
                saveLine = (written.lines.empty() ? "save ok" : written.lines.front()) + "; sims moved " + std::to_string(moved) +
                           (hoodLine.empty() ? "; no neighborhood" : "; " + hoodLine) +
                           (untouched ? "; original untouched" : "; ORIGINAL CHANGED") +
                           (hoodUntouched ? "; hood original untouched" : "; HOOD ORIGINAL CHANGED");
            }
            std::printf("opensims2: %s\n", saveLine.c_str());
        } catch (const std::exception& e) {
            saveLine = std::string("save failed: ") + e.what();
            std::fprintf(stderr, "opensims2: %s\n", saveLine.c_str());
        }
        log << "save " << saveLine << "\n";
        std::string buyLine = "buy skipped";
        if (buy && buy->ok() && liveUi && liveUi->ok()) {
            camera = homeCamera;
            bool buyClick = false;
            if (const auto rect = liveUi->controlRect(os2::LiveUi::kMainWindow, 0x67D97B2A)) {
                const simd_float2 at{((*rect)[0] + (*rect)[2]) * 0.5f, ((*rect)[1] + (*rect)[3]) * 0.5f};
                buyClick = liveUi->press(at);
            }
            std::string placedLine = "nothing placed";
            if (buy->active()) {
                const size_t before = simulation.machine().objects().size();
                int tried = 0;
                bool bought = false;
                for (const auto& entry : buy->catalog().entries()) {
                    if (bought || tried >= 8)
                        break;
                    if (entry.multiTile || entry.price <= 0 || entry.price > 400 || !buy->select(entry))
                        continue;
                    ++tried;
                    const int cx = int(camera.pivot.x), cy = int(camera.pivot.y);
                    for (int r = 1; r < 10 && !bought; ++r)
                        for (int dx = -r; dx <= r && !bought; ++dx)
                            for (int dy = -r; dy <= r && !bought; ++dy)
                                if ((std::abs(dx) == r || std::abs(dy) == r) && buy->placeAt(cx + dx, cy + dy, 2)) {
                                    bought = true;
                                    camera.pivot = simd_make_float3(float(cx + dx) + 0.5f, float(cy + dy) + 0.5f, 0.5f);
                                }
                    if (!bought)
                        buy->cancel();
                }
                placedLine = buy->message() + "; vm objects " + std::to_string(before) + " -> " +
                             std::to_string(simulation.machine().objects().size());
                for (int i = 0; i < 5; ++i)
                    frame();
                showLiveUi();
                renderer.setEye(camera.eye());
                const auto buyShot = shot.parent_path() / "frame_buy.png";
                renderer.capture(buyShot, camera.viewProj(1280.f / 720.f));
            }
            buyLine = std::string(buyClick ? "clicked" : "miss") + (buy->active() ? " active" : " inactive") + " panel " +
                      std::to_string(buyWindows) + " windows " + std::to_string(buyDrawn) + " drawn; catalog " +
                      std::to_string(buy->catalog().entries().size()) + "; " + placedLine;
            std::printf("opensims2: buy %s\n", buyLine.c_str());
        }
        log << "buy " << buyLine << "\n";
        std::string buildLine = "build skipped";
        if (build && build->ok() && liveUi && liveUi->ok()) {
            camera = homeCamera;
            bool buildClick = false;
            if (const auto rect = liveUi->controlRect(os2::LiveUi::kMainWindow, 0x77D97B28)) {
                const simd_float2 at{((*rect)[0] + (*rect)[2]) * 0.5f, ((*rect)[1] + (*rect)[3]) * 0.5f};
                buildClick = liveUi->press(at);
            }
            std::string roomLine = "no room drawn";
            if (build->active()) {
                const size_t wallsBefore = lot.walls.size();
                const int rebuildsBefore = lotRebuilds;
                const int cx = int(camera.pivot.x), cy = int(camera.pivot.y);
                std::string drawn;
                for (int r = 2; r < 12 && drawn.empty(); ++r)
                    for (int dx = -r; dx <= r && drawn.empty(); dx += 2)
                        for (int dy = -r; dy <= r && drawn.empty(); dy += 2) {
                            if (std::abs(dx) != r && std::abs(dy) != r)
                                continue;
                            const int x0 = cx + dx, y0 = cy + dy;
                            if (x0 < 1 || y0 < 1 || x0 + 3 >= int(lot.width) || y0 + 3 >= int(lot.height))
                                continue;
                            const std::string area = std::to_string(x0) + "," + std::to_string(y0) + "," +
                                                     std::to_string(x0 + 3) + "," + std::to_string(y0 + 3);
                            build->runSteps({"", "--room", area});
                            if (lotRebuilds > rebuildsBefore) {
                                drawn = area;
                                camera.pivot = simd_make_float3(float(x0) + 2.f, float(y0) + 2.f, 1.f);
                                camera.distance = 16.f;
                            }
                        }
                roomLine = drawn.empty() ? "no room drawn"
                                         : "room " + drawn + "; walls " + std::to_string(wallsBefore) + " -> " +
                                               std::to_string(lot.walls.size()) + "; lot rebuilt " +
                                               std::to_string(lotRebuilds - rebuildsBefore);
                for (int i = 0; i < 5; ++i)
                    frame();
                showLiveUi();
                renderer.setEye(camera.eye());
                renderer.capture(shot.parent_path() / "frame_build.png", camera.viewProj(1280.f / 720.f));
            }
            buildLine = std::string(buildClick ? "clicked" : "miss") + (build->active() ? " active" : " inactive") +
                        " panel " + std::to_string(buildWindows) + " windows " + std::to_string(buildDrawn) +
                        " drawn; " + roomLine;
            std::printf("opensims2: build %s\n", buildLine.c_str());
        }
        log << "build " << buildLine << "\n";
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
                if (liveUi && event.type == SDL_EVENT_MOUSE_MOTION)
                    liveUi->hover(simd_make_float2(event.motion.x, event.motion.y));
                if (liveUi && event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                    (event.button.button == SDL_BUTTON_LEFT || event.button.button == SDL_BUTTON_RIGHT))
                    liveUi->press(simd_make_float2(event.button.x, event.button.y), event.button.button == SDL_BUTTON_RIGHT);
            }
            showLiveUi();
            if (!frame())
                running = false;
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "opensims2: %s\n", e.what());
        return 1;
    }
}
