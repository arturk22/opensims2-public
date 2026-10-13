// The game flow's pure parts: scenes from arguments, the steps across scenes,
// the user folder (copies of the shipped neighborhoods, the options).
#include "app/game_flow.hpp"
#include "app/user_data.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

using namespace os2;
namespace fs = std::filesystem;

static int failures = 0;
static void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

static void touch(const fs::path& p, const std::string& text = "x") {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << text;
}

int main() {
    using V = std::vector<std::string>;
    // Scenes.
    check(sceneOf(V{"app"}).kind == Scene::Kind::MainMenu, "no flags: the main menu");
    check(sceneOf(V{"app", "--data", "d", "--no-audio"}).kind == Scene::Kind::MainMenu, "only global flags: the main menu");
    check(sceneOf(V{"app", "--lot", "N001_Lot11"}).kind == Scene::Kind::Other, "--lot: not a flow scene");
    check(sceneOf(V{"app", "--object", "sofa"}).kind == Scene::Kind::Other, "--object: the test scene");
    {
        const Scene h = sceneOf(V{"app", "--hood", "N001"});
        check(h.kind == Scene::Kind::Neighborhood && h.hood == "N001", "--hood: the neighborhood");
        const Scene l = sceneOf(V{"app", "--hood", "N001", "--hood-lot", "11", "--vm", "--ui", "live"});
        check(l.kind == Scene::Kind::Lot && l.hood == "N001" && l.lot == 11, "--hood-lot: the lot");
        const Scene m = sceneOf(V{"app", "--menu", "--menu-return", "N002"});
        check(m.kind == Scene::Kind::MainMenu && m.fromNeighborhood && m.returnHood == "N002", "menu from a neighborhood");
    }
    // Steps.
    {
        const V args{"app", "--click", "1,2", "--wait", "30", "--right-click", "3.5,4", "--click", "bad"};
        const auto st = flowSteps(args);
        check(st.size() == 3, "three steps (a malformed one dropped)");
        check(st.size() == 3 && st[0].kind == FlowStep::Kind::Click && st[0].x == 1 && st[0].y == 2, "a click");
        check(st.size() == 3 && st[1].kind == FlowStep::Kind::Wait && st[1].ticks == 30, "a wait");
        check(st.size() == 3 && st[2].kind == FlowStep::Kind::RightClick && st[2].x == 3.5f, "a right click");
    }
    {
        const V args{"app", "--drag", "1,2,3,4", "--drag", "5,6,7,8,ctrl", "--drag", "1,1,2,2,hold", "--hover", "9,10"};
        const auto st = flowSteps(args);
        check(st.size() == 4 && st[0].kind == FlowStep::Kind::Drag && st[0].x1 == 3 && st[0].y1 == 4 && !st[0].control &&
                  !st[0].hold,
              "a drag");
        check(st.size() == 4 && st[1].control && !st[1].hold && st[2].hold && st[3].kind == FlowStep::Kind::Hover &&
                  st[3].y == 10,
              "a Ctrl-drag, a held drag and a hover");
    }
    // Arguments of the next scene: scene flags replaced, used steps dropped.
    {
        const V args{"app", "--data", "d", "--click", "1,1", "--hood", "N001", "--zoom", "2", "--click", "2,2",
                     "--wait", "5", "--screenshot", "s.png", "--vm"};
        Scene lot;
        lot.kind = Scene::Kind::Lot, lot.hood = "N001", lot.lot = 11;
        const V out = sceneArguments(args, lot, 1);
        const V want{"app", "--data", "d", "--click", "2,2", "--wait", "5", "--screenshot", "s.png",
                     "--hood", "N001", "--hood-lot", "11", "--vm", "--ui", "live"};
        check(out == want, "lot arguments");
        Scene menu;
        menu.kind = Scene::Kind::MainMenu, menu.fromNeighborhood = true, menu.returnHood = "N001";
        const V back = sceneArguments(out, menu, 2);
        const V want2{"app", "--data", "d", "--screenshot", "s.png", "--menu", "--menu-return", "N001"};
        check(back == want2, "menu arguments, all steps used");
        check(sceneOf(back).kind == Scene::Kind::MainMenu, "the menu again");
    }
    check(numberedShot("out/flow.png", 2) == "out/flow-2.png", "numbered shot");
    check(numberedShot("a.b/flow", 1) == "a.b/flow-1", "numbered shot without extension");

    // The user folder: copies made on demand, the game's folder untouched.
    const fs::path tmp = fs::temp_directory_path() / ("os2_test_game_flow_" + std::to_string(::getpid()));
    fs::remove_all(tmp);
    const fs::path game = tmp / "game", user = tmp / "user";
    const fs::path shipped = game / "TSData/Res/UserData/Neighborhoods";
    touch(shipped / "N002/N002_Neighborhood.package", "two");
    touch(shipped / "N002/Lots/N002_Lot1.package", "lot");
    touch(shipped / "N001/N001_Neighborhood.package", "one");
    touch(shipped / "Tutorial/Tutorial_Neighborhood.package", "t");
    touch(shipped / "NeighborhoodTemplate/x.png");
    {
        UserData u(game, user);
        check(u.shipped() == V({"N001", "N002", "Tutorial"}), "shipped neighborhoods, tutorial last");
        check(!u.hasCopy("N002"), "no copy before use");
        check(u.neighborhoodForReading("N002") == shipped / "N002", "read from the shipped folder before a copy");
        const fs::path n2 = u.neighborhood("N002");
        check(n2 == user / "Neighborhoods/N002" && fs::exists(n2 / "Lots/N002_Lot1.package"), "copied on first use");
        check(u.hasCopy("N002") && !u.hasCopy("N001"), "only the one used is copied");
        touch(n2 / "Lots/N002_Lot1.package", "saved");
        std::ifstream in(shipped / "N002/Lots/N002_Lot1.package");
        std::string text;
        in >> text;
        check(text == "lot", "writing the copy leaves the game's file as it was");
        check(u.neighborhood("N002") == n2, "the copy is used from then on");
        check(u.inGameData(shipped / "N002/Lots/N002_Lot1.package") && !u.inGameData(n2), "game data told apart");
        bool threw = false;
        try {
            u.neighborhood("../x");
        } catch (const std::exception&) {
            threw = true;
        }
        check(threw, "bad names refused");
        GameSettings s;
        s.volume[2] = 0.25f, s.muted[0] = true, s.edgeScrolling = false;
        s.save(u);
    }
    {
        UserData u(game, user);
        const GameSettings s = GameSettings::load(u);
        check(s.volume[2] == 0.25f && s.muted[0] && !s.muted[1] && !s.edgeScrolling && s.volume[0] == 1,
              "options read back");
        bool threw = false;
        try {
            UserData bad(game, game / "inside");
        } catch (const std::exception&) {
            threw = true;
        }
        check(threw, "a user folder inside the game's data is refused");
    }
    fs::remove_all(tmp);

    // A lot without a family opens in Buy mode; an occupied one in Live; --mode wins.
    check(lotStartMode(false, std::nullopt) == "buy", "an unoccupied lot opens in Buy mode");
    check(lotStartMode(true, std::nullopt).empty(), "an occupied lot opens in Live mode");
    check(lotStartMode(false, std::string("build")) == "build", "--mode build on an unoccupied lot");
    check(lotStartMode(true, std::string("buy")) == "buy", "--mode buy on an occupied lot");

    if (failures)
        std::fprintf(stderr, "%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
