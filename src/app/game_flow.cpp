#include "app/game_flow.hpp"

#include <algorithm>
#include <cstdio>
#include <initializer_list>

namespace os2 {

namespace {

// Flags that choose or set up a scene, with a value / without.
const std::initializer_list<const char*> kSceneFlags = {"--hood",  "--hood-lot", "--hood-hover", "--hood-select",
                                                        "--lot",   "--ui",       "--pivot",      "--zoom",
                                                        "--orientation", "--pie", "--pie-select", "--mode",
                                                        "--menu-return", "--menu-select", "--ui-show",
                                                        "--hood-panel", "--hood-family", "--hood-tool", "--hood-lot-click", "--hood-options-window", "--hood-package", "--hood-friends",
                                                        "--hood-place", "--hood-place-at", "--hood-place-click",
                                                        "--cas-hood", "--cas-return"};
const std::initializer_list<const char*> kSceneBare = {"--vm", "--menu", "--cas"};
// Things to show other than the game's scenes.
const std::initializer_list<const char*> kContentFlags = {"--object", "--lot", "--sim", "--skinned", "--character",
                                                          "--ui"};

bool isStep(const std::string& a) { return a == "--click" || a == "--right-click" || a == "--wait" || a == "--pie-step" || a == "--tutorial-auto" || a == "--key"; }

} // namespace

std::optional<std::string> argValue(const std::vector<std::string>& args, const std::string& flag) {
    for (size_t i = 1; i + 1 < args.size(); ++i)
        if (args[i] == flag)
            return args[i + 1];
    return std::nullopt;
}

bool hasArg(const std::vector<std::string>& args, const std::string& flag) {
    return args.size() > 1 && std::find(args.begin() + 1, args.end(), flag) != args.end();
}

Scene sceneOf(const std::vector<std::string>& args) {
    Scene s;
    if (auto ret = argValue(args, "--menu-return"))
        s.fromNeighborhood = true, s.returnHood = *ret;
    if (hasArg(args, "--menu")) {
        s.kind = Scene::Kind::MainMenu;
        return s;
    }
    if (hasArg(args, "--cas")) {
        s.kind = Scene::Kind::Cas;
        s.hood = argValue(args, "--cas-hood").value_or("");
        s.returnHood = argValue(args, "--cas-return").value_or("");
        return s;
    }
    if (auto hood = argValue(args, "--hood")) {
        s.hood = *hood;
        if (auto lot = argValue(args, "--hood-lot")) {
            s.kind = Scene::Kind::Lot;
            s.lot = uint32_t(std::stoul(*lot));
        } else {
            s.kind = Scene::Kind::Neighborhood;
        }
        return s;
    }
    for (const char* f : kContentFlags)
        if (hasArg(args, f))
            return s; // Other
    if (hasArg(args, "--play-sound"))
        return s;
    s.kind = Scene::Kind::MainMenu;
    return s;
}

std::vector<FlowStep> flowSteps(const std::vector<std::string>& args) {
    std::vector<FlowStep> out;
    for (size_t i = 1; i + 1 < args.size(); ++i) {
        FlowStep st;
        if (args[i] == "--wait") {
            st.kind = FlowStep::Kind::Wait;
            try {
                st.ticks = std::stol(args[i + 1]);
            } catch (const std::exception&) {
                continue;
            }
        } else if (args[i] == "--pie-step") {
            st.kind = FlowStep::Kind::Pie;
            const std::string& v = args[i + 1];
            const size_t colon = v.rfind(':');
            st.object = v.substr(0, colon);
            st.text = colon == std::string::npos ? std::string() : v.substr(colon + 1);
            try {
                st.entry = colon == std::string::npos ? 0 : std::stoi(st.text);
            } catch (const std::exception&) {
                st.entry = -1; // not a number: the entry whose name holds the text
            }
        } else if (args[i] == "--click" || args[i] == "--right-click") {
            st.kind = args[i] == "--click" ? FlowStep::Kind::Click : FlowStep::Kind::RightClick;
            if (std::sscanf(args[i + 1].c_str(), "%f,%f", &st.x, &st.y) != 2)
                continue;
        } else if (args[i] == "--tutorial-auto") {
            st.kind = FlowStep::Kind::TutorialAuto;
            try {
                st.ticks = std::stol(args[i + 1]);
            } catch (const std::exception&) {
                continue;
            }
        } else if (args[i] == "--key") {
            st.kind = FlowStep::Kind::Key;
            st.text = args[i + 1];
        } else {
            continue;
        }
        out.push_back(st);
        ++i;
    }
    return out;
}

std::vector<std::string> sceneArguments(const std::vector<std::string>& args, const Scene& next, size_t stepsUsed) {
    std::vector<std::string> out;
    size_t steps = 0;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i == 0) {
            out.push_back(args[i]);
            continue;
        }
        const std::string& a = args[i];
        if (isStep(a) && i + 1 < args.size()) {
            if (steps++ >= stepsUsed)
                out.insert(out.end(), {a, args[i + 1]});
            ++i;
            continue;
        }
        bool drop = false;
        for (const char* f : kSceneFlags)
            if (a == f) {
                drop = true, ++i;
                break;
            }
        for (const char* f : kSceneBare)
            if (a == f)
                drop = true;
        if (!drop)
            out.push_back(a);
    }
    switch (next.kind) {
    case Scene::Kind::MainMenu:
        out.push_back("--menu");
        if (next.fromNeighborhood)
            out.insert(out.end(), {"--menu-return", next.returnHood});
        break;
    case Scene::Kind::Neighborhood: out.insert(out.end(), {"--hood", next.hood}); break;
    case Scene::Kind::Lot:
        out.insert(out.end(), {"--hood", next.hood, "--hood-lot", std::to_string(next.lot), "--vm", "--ui", "live"});
        break;
    case Scene::Kind::Cas:
        out.push_back("--cas");
        if (!next.hood.empty())
            out.insert(out.end(), {"--cas-hood", next.hood});
        if (!next.returnHood.empty())
            out.insert(out.end(), {"--cas-return", next.returnHood});
        break;
    case Scene::Kind::Other: break;
    }
    return out;
}

std::string numberedShot(const std::string& shot, int n) {
    const auto dot = shot.rfind('.');
    const auto slash = shot.rfind('/');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return shot + "-" + std::to_string(n);
    return shot.substr(0, dot) + "-" + std::to_string(n) + shot.substr(dot);
}

} // namespace os2
