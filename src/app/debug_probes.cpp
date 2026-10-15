// Test aids that watch the drawn Sims frame by frame (OS2_THEN_AT,
// OS2_DEBUG_JUMPS / GLIDE / POSES / SEATS, OS2_WATCH_OBJECT / BONE) and
// OS2_POSE_SHOT's posture sheets (app_state.hpp).

#include "app/app_state.hpp"

namespace os2app {

// A car, bus or truck: its passengers move with its own animation (test aids below).
bool isVehicle(const os2::vm::Object* o) {
    if (!o || !o->definition)
        return false;
    const std::string& n = o->definition->fileName;
    return n.starts_with("Car ") || n.find("Vehicle") != std::string::npos || n.find("Bus") != std::string::npos;
}

// OS2_DEBUG_JUMPS=1 (test aid): each Sim's drawn pelvis checked frame to
// frame (app/jump_detector.hpp); jumps over a tile the VM gave no reason
// for, back-and-forth jumps and repeated VM snaps are printed as they
// happen and summed up at exit. OS2_WATCH_OBJECT=<id|name>: where the VM
// has those objects and where they are drawn, as that changes.
void sampleDrawnPlaces(double scene) {
    if (!gSimulation)
        return;
    static const bool jumps = std::getenv("OS2_DEBUG_JUMPS") != nullptr;
    static const char* watch = std::getenv("OS2_WATCH_OBJECT");
    os2::vm::VM& m = gSimulation->machine();
    // OS2_THEN_AT="<tick>:<object>:<entry>[;<tick>:...]" (test aid, the seat
    // battery): at that VM tick the selected Sim queues the English pie entry
    // of an object found by ID or name (as --vm-queue), or with the object
    // "here" a Go Here to <entry> = x,y[,level] (to get up, change seats,
    // climb out of the pool).
    if (static const char* then = std::getenv("OS2_THEN_AT"); then && gPie) {
        size_t& done = gScene.thenAtDone; // (ticks of this scene's VM)
        std::vector<std::string> items;
        for (std::string rest = then; !rest.empty();) {
            const size_t semi = rest.find(';');
            items.push_back(rest.substr(0, semi));
            rest = semi == std::string::npos ? "" : rest.substr(semi + 1);
        }
        while (done < items.size()) {
            const std::string& it = items[done];
            const size_t c1 = it.find(':'), c2 = c1 == std::string::npos ? c1 : it.find(':', c1 + 1);
            if (c2 == std::string::npos || gSimulation->machine().ticks < std::stoull(it.substr(0, c1)))
                break;
            ++done;
            os2::vm::VM& vm = gSimulation->machine();
            const std::string what = it.substr(c1 + 1, c2 - c1 - 1), entry = it.substr(c2 + 1);
            const os2::vm::ObjectId sim = gPie->sim();
            bool queued = false;
            if (what == "here") {
                float x = 0, y = 0;
                int level = 1;
                std::sscanf(entry.c_str(), "%f,%f,%d", &x, &y, &level);
                const os2::vm::ObjectId dest = sim ? os2::vm::createGoHereDestination(vm, sim, x, y, level) : 0;
                if (dest)
                    for (const auto& e : os2::vm::pieMenu(vm, sim, dest))
                        if (!queued && e.available() && !e.debug)
                            if (auto q = os2::vm::interactionFromPie(vm, e)) {
                                q->pushedBy = sim;
                                os2::vm::enqueue(vm, *vm.object(sim), *q);
                                queued = true;
                            }
                if (dest)
                    os2::vm::releaseDestination(vm, dest);
            } else {
                os2::vm::ObjectId target = 0;
                for (const auto& [id, o] : vm.objects())
                    if (!target && !o.person && o.definition &&
                        (what == std::to_string(id) || strcasestr(o.definition->fileName.c_str(), what.c_str())))
                        target = id;
                os2::vm::PieOptions opt;
                opt.language = os2::lang::EnglishUS;
                if (target && sim)
                    for (const auto& e : os2::vm::pieMenu(vm, sim, target, opt))
                        if (!queued && strcasecmp(e.name.c_str(), entry.c_str()) == 0)
                            if (auto q = os2::vm::interactionFromPie(vm, e)) {
                                os2::vm::addAction(vm, *vm.object(sim), *q);
                                queued = true;
                            }
            }
            std::string offered;
            if (!queued && what != "here")
                for (const auto& [id, o] : vm.objects())
                    if (!o.person && o.definition && strcasestr(o.definition->fileName.c_str(), what.c_str()) && offered.empty()) {
                        os2::vm::PieOptions opt;
                        opt.language = os2::lang::EnglishUS;
                        offered = " (object " + std::to_string(id) + " offers:";
                        for (const auto& e : os2::vm::pieMenu(vm, sim, id, opt))
                            offered += " '" + e.name + "'";
                        offered += ")";
                    }
            std::printf("opensims2: tick %llu: then %s:%s %s%s\n", (unsigned long long)vm.ticks, what.c_str(), entry.c_str(),
                        queued ? "queued" : "not queued", offered.c_str());
        }
    }
    if (jumps) {
        static os2::JumpDetector detector;
        static bool registered = false;
        if (!registered) {
            registered = true;
            std::atexit([] {
                std::printf("opensims2: jumps: %d drawn jumps without a VM reason (%d back and forth), %d Sims snapped over and over\n",
                            detector.drawnJumps(), detector.backAndForth(), detector.repeatedSnaps());
                std::fflush(stdout);
            });
        }
        static uint64_t generation = gSceneGeneration; // (a new scene: forget the last one's Sims)
        if (generation != gSceneGeneration)
            generation = gSceneGeneration, detector.forgetSims();
        for (const auto& s : gLotSims) {
            const os2::vm::Object* o = m.object(s.id);
            if (!o || !o->person)
                continue;
            os2::JumpDetector::Sample x;
            x.id = s.id, x.time = scene, x.tick = m.ticks;
            const os2::SimActor& a = *s.actor;
            int b = a.skin.skeleton.find("pelvis");
            if (b < 0)
                b = a.skin.skeleton.find("root_trans");
            const os2::Vec3 p = b >= 0 && size_t(b) < a.bones.size() ? a.bones[size_t(b)].t : a.placement().t;
            x.drawn[0] = p[0], x.drawn[1] = p[1], x.drawn[2] = p[2];
            x.vm[0] = o->position.x, x.vm[1] = o->position.y, x.level = o->position.level;
            x.walking = o->route.has_value();
            const int16_t c = o->data[os2::vm::objdata::ContainerId];
            x.container = c == s.id ? 0 : c;
            if (const os2::vm::Object* h = x.container ? m.object(x.container) : nullptr)
                x.holder[0] = h->position.x, x.holder[1] = h->position.y;
            x.seat[0] = x.holder[0], x.seat[1] = x.holder[1];
            if (x.container && gObjectView)
                if (const auto at = gObjectView->slotPlacement(s.id, scene))
                    x.seat[0] = at->t[0], x.seat[1] = at->t[1];
            x.inWorld = simInTheWorld(s.id) && !gCarriedSims.count(s.actor) && !isVehicle(m.object(x.container));
            x.animation = o->animation.name;
            // OS2_TRACE_DRAWN=<Sim id>: that Sim's drawn pelvis and VM place each frame.
            static const int traced = std::getenv("OS2_TRACE_DRAWN") ? std::atoi(std::getenv("OS2_TRACE_DRAWN")) : 0;
            if (traced == s.id)
                std::printf("opensims2: drawn: tick %llu %.3f s Sim %d pelvis (%.2f %.2f %.2f) placement (%.2f %.2f %.2f) vm (%.2f %.2f L%d dir %d)%s in %d '%s'\n",
                            (unsigned long long)m.ticks, scene, s.id, p[0], p[1], p[2], a.position[0], a.position[1], a.position[2],
                            o->position.x, o->position.y, int(o->position.level), o->data[os2::vm::objdata::Direction],
                            o->route ? " walking" : "", x.container, x.animation.c_str());
            for (const auto& e : detector.add(x))
                std::printf("opensims2: %s\n", os2::JumpDetector::describe(e).c_str());
        }
    }
    // OS2_DEBUG_GLIDE=1 (test aid, app/glide_detector.hpp): Sims whose drawn
    // pelvis moves on while their legs hold still (gliding with no walk),
    // printed as each episode starts with what the Sim was doing (route,
    // locomotion clip, Animate Sim, overlays, the layers that play, the
    // interaction and the tree), and counted at exit by what played.
    static const bool glide = std::getenv("OS2_DEBUG_GLIDE") != nullptr;
    if (glide) {
        static os2::GlideDetector detector;
        static std::map<std::string, int> byKind;
        static bool registered = false;
        if (!registered) {
            registered = true;
            if (const char* r = std::getenv("OS2_GLIDE_LEGS")) // the feet's range threshold (tiles)
                detector.legRange = float(std::atof(r));
            std::atexit([] {
                std::printf("opensims2: glides: %d episodes of Sims gliding without moving their legs, %.1f s in all\n",
                            detector.episodes(), detector.seconds());
                for (const auto& [k, n] : byKind)
                    std::printf("opensims2: glides:   %3d  %s\n", n, k.c_str());
                std::fflush(stdout);
            });
        }
        static uint64_t generation = gSceneGeneration; // (a new scene: forget the last one's Sims)
        if (generation != gSceneGeneration)
            generation = gSceneGeneration, detector.forgetSims();
        for (const auto& s : gLotSims) {
            const os2::vm::Object* o = m.object(s.id);
            if (!o || !o->person)
                continue;
            os2::SimActor& a = *s.actor;
            const os2::Skeleton& sk = a.skin.skeleton;
            const int pb = sk.find("pelvis"), lb = sk.find("l_foot"), rb = sk.find("r_foot");
            if (pb < 0 || lb < 0 || rb < 0 || a.bones.size() != sk.bones.size())
                continue;
            os2::GlideDetector::Sample x;
            x.id = s.id, x.time = scene, x.tick = m.ticks;
            const os2::Vec3 p = a.bones[size_t(pb)].t;
            x.pelvis[0] = p[0], x.pelvis[1] = p[1];
            const os2::Xform toModel = a.placement().inverse();
            const os2::Vec3 pm = toModel.apply(p), lf = toModel.apply(a.bones[size_t(lb)].t),
                            rf = toModel.apply(a.bones[size_t(rb)].t);
            for (int i = 0; i < 3; ++i)
                x.lfoot[i] = lf[i] - pm[i], x.rfoot[i] = rf[i] - pm[i];
            const int16_t c = o->data[os2::vm::objdata::ContainerId];
            x.inWorld = simInTheWorld(s.id) && !gCarriedSims.count(s.actor) && (c == 0 || c == s.id);
            // The Grim Reaper floats: her walk ("a-female-walk-reaper") carries
            // the feet along with the root (s2pkg anim-root: they hang still
            // under the pelvis), so she is not a glide.
            if (o->route && o->route->loco.animation.find("walk-reaper") != std::string::npos)
                x.inWorld = false;
            // The feet's slide while the locomotion plays: the slower foot's
            // speed over the floor (the planted one; 0 when nothing slides),
            // per locomotion phase, the mean printed at exit.
            {
                struct Feet {
                    double time = -1;
                    float l[2] = {0, 0}, r[2] = {0, 0};
                };
                static std::map<int16_t, Feet> last;
                static double sum[4] = {0, 0, 0, 0};
                static long frames[4] = {0, 0, 0, 0};
                static bool skateRegistered = false;
                if (!skateRegistered) {
                    skateRegistered = true;
                    std::atexit([] {
                        static const char* names[] = {"none", "start", "walk", "stop"};
                        double all = 0;
                        long n = 0;
                        for (int k = 0; k < 4; ++k)
                            all += sum[k], n += frames[k];
                        std::printf("opensims2: skate: %ld walking frames, planted foot %.3f tiles/s", n, n ? all / double(n) : 0.0);
                        for (int k = 1; k < 4; ++k)
                            std::printf(", %s %.3f (%ld)", names[k], frames[k] ? sum[k] / double(frames[k]) : 0.0, frames[k]);
                        std::printf("\n");
                        std::fflush(stdout);
                    });
                }
                const os2::Vec3 lw = a.bones[size_t(lb)].t, rw = a.bones[size_t(rb)].t;
                Feet& f = last[s.id];
                const bool walking = x.inWorld && o->route && o->route->loco.moving &&
                                     o->route->loco.kind != os2::vm::Object::Route::Loco::None;
                if (walking && f.time >= 0 && scene - f.time > 1e-4 && scene - f.time < 0.2) {
                    const double dt = scene - f.time;
                    const double vl = std::hypot(lw[0] - f.l[0], lw[1] - f.l[1]) / dt;
                    const double vr = std::hypot(rw[0] - f.r[0], rw[1] - f.r[1]) / dt;
                    const int k = std::min<int>(o->route->loco.kind, 3);
                    sum[k] += std::min(vl, vr);
                    ++frames[k];
                }
                f.time = walking ? scene : -1;
                f.l[0] = lw[0], f.l[1] = lw[1], f.r[0] = rw[0], f.r[1] = rw[1];
            }
            for (const auto& e : detector.add(x)) {
                std::string overlays;
                for (const auto& ov : o->animation.overlays)
                    overlays += (overlays.empty() ? "" : ", ") + ov.name + " L" + std::to_string(ov.level);
                std::string route = "no route";
                std::string kind = "no route";
                if (o->route) {
                    const auto& L = o->route->loco;
                    static const char* kinds[] = {"none", "start", "walk", "stop"};
                    char buf[300];
                    std::snprintf(buf, sizeof buf, "route%s%s loco %s%s '%s' climb '%s' points %zu tree '%s'",
                                  o->route->waiting ? " waiting" : "", o->route->inPortal ? " in portal" : "",
                                  kinds[std::min<int>(L.kind, 3)], L.moving ? " moving" : "", L.animation.c_str(),
                                  o->route->climb.animation.c_str(), o->route->points.size(),
                                  o->route->tree ? o->route->tree->fileName.c_str() : "");
                    route = buf;
                    kind = std::string("route, loco ") + kinds[std::min<int>(L.kind, 3)] +
                           (L.moving ? " moving" : "") + (o->route->waiting ? " waiting" : "") +
                           (L.animation.empty() ? ", no clip" : "");
                }
                if (!o->animation.name.empty())
                    kind += ", Animate Sim " + o->animation.name;
                const auto th = m.threads.find(s.id);
                std::string tree = th != m.threads.end() && !th->second.stack.empty() && th->second.stack.back().behavior
                                       ? th->second.stack.back().behavior->fileName
                                       : "";
                std::string interaction;
                if (o->activeInteraction >= 0 && size_t(o->activeInteraction) < o->queue.size())
                    interaction = o->queue[size_t(o->activeInteraction)].name;
                std::printf("opensims2: glide: tick %llu %.2f s Sim %d (%s, age %d) moved %.2f tiles in %.1f s, feet range %.3f at "
                            "(%.2f %.2f); %s; Animate Sim '%s' L%d; overlays [%s]; interaction '%s'; tree '%s'; layers %s\n",
                            (unsigned long long)e.tick, e.time, e.id, s.character.filename().string().c_str(),
                            o->person->data[os2::vm::persondata::PersonAge], e.distance, detector.window, e.legs, x.pelvis[0],
                            x.pelvis[1], route.c_str(), o->animation.name.c_str(), o->animation.level, overlays.c_str(),
                            interaction.c_str(), tree.c_str(), a.player.describe(scene).c_str());
                ++byKind[kind];
            }
        }
    }
    // OS2_DEBUG_POSES=1 (test aid): poses that look broken, as they start
    // and with a count at exit: a seated Sim's pelvis far from its seat
    // bone (out of a get-in or get-out), a hand far behind the shoulders.
    static const bool poses = std::getenv("OS2_DEBUG_POSES") != nullptr;
    if (poses && gObjectView) {
        struct Count {
            int seat = 0, behind = 0, frames = 0;
            double seatMax = 0, behindMax = 0;
        };
        static std::map<std::string, Count> counts;
        auto& flagged = gScene.posesFlagged; // (Sim, kind) currently flagged (counts: the whole run, at exit)
        static bool registered = false;
        if (!registered) {
            registered = true;
            std::atexit([] {
                int seat = 0, behind = 0;
                for (const auto& [k, c] : counts)
                    seat += c.seat, behind += c.behind;
                std::printf("opensims2: poses: %d seated frames off the seat, %d frames with a hand behind the back\n", seat, behind);
                for (const auto& [k, c] : counts)
                    if (c.seat || c.behind)
                        std::printf("opensims2: poses:   %-50s seat %d (max %.2f) behind %d (max %.2f) of %d frames\n", k.c_str(),
                                    c.seat, c.seatMax, c.behind, c.behindMax, c.frames);
                std::fflush(stdout);
            });
        }
        for (const auto& s : gLotSims) {
            const os2::vm::Object* o = m.object(s.id);
            if (!o || !o->person || !simInTheWorld(s.id) || gCarriedSims.count(s.actor))
                continue;
            const os2::SimActor& a = *s.actor;
            auto bone = [&](const char* n) -> std::optional<os2::Vec3> {
                const int b = a.skin.skeleton.find(n);
                if (b < 0 || size_t(b) >= a.bones.size())
                    return std::nullopt;
                return a.bones[size_t(b)].t;
            };
            const auto pelvis = bone("pelvis"), rt = bone("r_thigh"), lt = bone("l_thigh");
            const auto rc = bone("r_clavicle"), lc = bone("l_clavicle"), rh = bone("r_hand"), lh = bone("l_hand");
            if (!pelvis || !rt || !lt || !rc || !lc || !rh || !lh)
                continue;
            // What it does, for the counts: Animate Sim's, else the carry/overlays.
            std::string what = o->animation.name;
            if (what.empty())
                for (const auto& ov : o->animation.overlays)
                    if (ov.name.find("blink") == std::string::npos && ov.name.find("idle") == std::string::npos)
                        what = ov.name;
            if (what.empty())
                what = o->route ? "(walking)" : "(idle)";
            Count& c = counts[what];
            ++c.frames;
            auto flag = [&](const char* kind, bool on, double value) {
                bool& f = flagged[{s.id, kind}];
                if (on && !f)
                {
                    const os2::vm::Object* in = m.object(o->data[os2::vm::objdata::ContainerId]);
                    std::printf("opensims2: pose: tick %llu Sim %d %s %.2f in '%s' at (%.2f %.2f) container %d %s\n",
                                (unsigned long long)m.ticks, s.id, kind, value, what.c_str(), (*pelvis)[0], (*pelvis)[1],
                                o->data[os2::vm::objdata::ContainerId],
                                in && in != o && in->definition ? in->definition->fileName.c_str() : "");
                }
                f = on;
            };
            // Seated: pelvis over the seat (out of the get-ins and get-outs).
            const int16_t c0 = o->data[os2::vm::objdata::ContainerId];
            const os2::vm::Object* co = c0 && c0 != s.id ? m.object(c0) : nullptr;
            const bool moving = o->animation.name.find("getIn") != std::string::npos ||
                                o->animation.name.find("getOut") != std::string::npos ||
                                o->animation.name.find("GetIn") != std::string::npos ||
                                o->animation.name.find("GetOut") != std::string::npos ||
                                o->animation.name.find("-start") != std::string::npos || // (a piano's sit-down)
                                o->animation.name.find("-stop") != std::string::npos;
            // (Not on a stair's steps or in a car: their own motion.)
            const bool ownMotion = co && co->definition && (co->definition->fileName.find("Stair") != std::string::npos ||
                                                            isVehicle(co));
            if (co && !co->person && !moving && !ownMotion)
                if (const auto seatAt = gObjectView->slotPlacement(s.id, scene)) {
                    const double d = std::hypot((*pelvis)[0] - seatAt->t[0], (*pelvis)[1] - seatAt->t[1]);
                    c.seatMax = std::max(c.seatMax, d);
                    c.seat += d > 0.35;
                    flag("pelvis off the seat", d > 0.35, d);
                    // And the seat itself near its object (a bone in the wrong frame).
                    if (co->position.inWorld()) {
                        const double far = std::hypot((*pelvis)[0] - co->position.x, (*pelvis)[1] - co->position.y);
                        c.seat += far > 1.2;
                        flag("pelvis far from its seat's object", far > 1.2, far);
                    }
                }
            // A carry pose (an overlay or Animate Sim "...-carry"/"grasp") with
            // nothing in the hands (no object in the Sim's slots).
            {
                std::string carry;
                for (const auto& ov : o->animation.overlays)
                    if (ov.name.find("carry") != std::string::npos || ov.name.find("grasp") != std::string::npos)
                        carry = ov.name;
                bool holds = false;
                if (!carry.empty())
                    for (const auto& [oid, ob] : m.objects())
                        if (oid != s.id && ob.data[os2::vm::objdata::ContainerId] == s.id)
                            holds = true;
                const bool bad = !carry.empty() && !holds;
                if (bad)
                    ++counts["carry " + carry].seat;
                flag("carry pose with empty hands", bad, 0);
            }
            // A hand behind the shoulders' plane (the body's forward from the hips).
            const double lx = (*lt)[0] - (*rt)[0], ly = (*lt)[1] - (*rt)[1];
            const double ln = std::hypot(lx, ly);
            if (ln > 1e-3) {
                const double fx = ly / ln, fy = -lx / ln; // up x left: forward (the sink's scrub has the hands ahead)
                auto behind = [&](const os2::Vec3& h, const os2::Vec3& sh) { return -((h[0] - sh[0]) * fx + (h[1] - sh[1]) * fy); };
                const double b = std::max(behind(*rh, *rc), behind(*lh, *lc));
                c.behindMax = std::max(c.behindMax, b);
                const bool bad = b > 0.3 && !o->route; // (a walk swings the arms back 0.25)
                c.behind += bad;
                flag("hand behind the back", bad, b);
            }
        }
    }
    // OS2_DEBUG_SEATS=1 (test aid, app/seat_check.hpp; tests/seat_battery.sh):
    // each Sim's drawn body against its seat, its IK goals, the tables and
    // chairs around it, and its carry; problems printed as they start and
    // summed up at exit.
    static const bool seats = std::getenv("OS2_DEBUG_SEATS") != nullptr;
    if (seats && gObjectView) {
        static os2::SeatCheck check;
        static bool registered = false;
        if (!registered) {
            registered = true;
            std::atexit([] {
                std::printf("opensims2: seats: %d problems: %s\n", check.episodes(), check.summary().c_str());
                for (const auto& [k, c] : check.counts())
                    if (c.episodes)
                        std::printf("opensims2: seats:   worst %s\n", c.example.c_str());
                for (const auto& [name, r] : check.seats())
                    std::printf("opensims2: seats:   pelvis in the slot of %s: x %.2f..%.2f y %.2f..%.2f z %.2f..%.2f (%d frames)\n",
                                name.c_str(), r.lo[0], r.hi[0], r.lo[1], r.hi[1], r.lo[2], r.hi[2], r.frames);
                std::fflush(stdout);
            });
        }
        static uint64_t generation = gSceneGeneration; // (a new scene: forget the last one's Sims)
        if (generation != gSceneGeneration)
            generation = gSceneGeneration, check.forgetSims();
        // The tables, counters and seats (their models' boxes where they are drawn now).
        std::vector<os2::SeatCheck::Box> boxes;
        for (const auto& b : gPickBoxes) {
            const int16_t id = int16_t(b.instance & 0xFFFF);
            const os2::vm::Object* ob = m.object(id);
            if (!ob || ob->person || !ob->definition)
                continue;
            const std::string& n = ob->definition->fileName;
            const bool table = n.find("Table") != std::string::npos || n.find("Counter") != std::string::npos;
            const bool seat = n.find("Chair") != std::string::npos || n.find("Stool") != std::string::npos ||
                              n.find("Sofa") != std::string::npos || n.find("Loveseat") != std::string::npos ||
                              n.find("Bench") != std::string::npos;
            if (!table && !seat)
                continue;
            const auto at = gObjectView->placement(id);
            if (!at || !gObjectView->shown(id))
                continue;
            os2::SeatCheck::Box x;
            x.id = id, x.name = n, x.at = *at, x.table = table && !seat;
            x.lo = {b.lo.x, b.lo.y, b.lo.z}, x.hi = {b.hi.x, b.hi.y, b.hi.z};
            boxes.push_back(std::move(x));
        }
        // OS2_DEBUG_SEATS=<Sim id> (over 1): that Sim only.
        static const int only = std::atoi(std::getenv("OS2_DEBUG_SEATS"));
        for (const auto& s : gLotSims) {
            const os2::vm::Object* o = m.object(s.id);
            if (!o || !o->person || !simInTheWorld(s.id) || gCarriedSims.count(s.actor) || (only > 1 && s.id != only))
                continue;
            const os2::SimActor& a = *s.actor;
            auto bone = [&](const char* n) -> std::optional<os2::Vec3> {
                const int b = a.skin.skeleton.find(n);
                if (b < 0 || size_t(b) >= a.bones.size())
                    return std::nullopt;
                return a.bones[size_t(b)].t;
            };
            const auto pelvis = bone("pelvis");
            if (!pelvis)
                continue;
            os2::SeatCheck::Sample x;
            x.id = s.id, x.time = scene, x.tick = m.ticks;
            x.pelvis = *pelvis;
            for (const char* n : {"spine1", "l_calf", "r_calf"})
                if (const auto p = bone(n))
                    x.body.push_back({n, *p});
            for (const char* n : {"l_foot", "r_foot"})
                if (const auto p = bone(n))
                    x.feet.push_back(*p);
            x.what = o->animation.name;
            if (x.what.empty())
                for (const auto& ov : o->animation.overlays)
                    if (ov.name.find("blink") == std::string::npos)
                        x.what = ov.name;
            x.walking = o->route.has_value();
            const std::string& an = o->animation.name;
            for (const char* k : {"getIn", "getOut", "GetIn", "GetOut", "-start", "-stop", "-in-", "-out-", "standUp", "sitDown", "Transition", "trans-"})
                x.transitional = x.transitional || an.find(k) != std::string::npos;
            const int16_t c = o->data[os2::vm::objdata::ContainerId];
            const os2::vm::Object* co = c && c != s.id ? m.object(c) : nullptr;
            if (co && !co->person && co->definition && co->definition->fileName.find("Stair") == std::string::npos && !isVehicle(co))
                if (const auto at = gObjectView->slotPlacement(s.id, scene)) {
                    x.container = c, x.containerName = co->definition->fileName, x.seat = *at;
                    x.lying = x.containerName.find("Tub") != std::string::npos || x.containerName.find("Bed") != std::string::npos;
                    // (Some trees walk a contained Sim: the bed's, the bath's.)
                }
            // (The hands: the standing idle's foot goals under a dance are not a seat's business.)
            for (const auto& k : a.player.lastIkMisses())
                if (k.bone.find("hand") != std::string::npos && k.distance > x.ikMiss)
                    x.ikMiss = k.distance, x.ikChain = k.bone + " of " + k.animation;
            for (const auto& ov : o->animation.overlays)
                if (ov.name.find("carry") != std::string::npos || ov.name.find("grasp") != std::string::npos) {
                    bool holds = false;
                    for (const auto& [oid, ob] : m.objects())
                        holds = holds || (oid != s.id && ob.data[os2::vm::objdata::ContainerId] == s.id);
                    x.carryEmpty = !holds;
                }
            // (The other parts of a multi-tile seat are the same seat.)
            const int16_t seatGroup = co ? int16_t(os2::vm::multiTileGroup(*co)) : 0;
            std::vector<os2::SeatCheck::Box> near;
            for (const auto& b : boxes)
                if (const os2::vm::Object* bo = m.object(b.id); seatGroup && bo && int16_t(os2::vm::multiTileGroup(*bo)) == seatGroup)
                    continue;
                else if (std::hypot(b.at.t[0] - x.pelvis[0], b.at.t[1] - x.pelvis[1]) < 3 && std::fabs(b.at.t[2] - a.position[2]) < 1.5f)
                    near.push_back(b);
            for (const auto& e : check.add(x, near))
                std::printf("opensims2: seat check: %s\n", e.c_str());
        }
    }
    // OS2_WATCH_BONE=<object id>:<bone>: that object's actor bone (world) each frame.
    if (const char* wb = std::getenv("OS2_WATCH_BONE")) {
        const std::string spec = wb;
        const int oid = std::atoi(spec.c_str());
        const std::string bone = spec.substr(spec.find(':') + 1);
        for (auto& [id, a] : gObjectActors)
            if (id == oid && a) {
                const int b = a->skin.skeleton.find(bone);
                if (b >= 0 && size_t(b) < a->bones.size())
                    std::printf("opensims2: bone: tick %llu %.3f %d %s (%.3f %.3f %.3f) transform %s\n", (unsigned long long)m.ticks,
                                scene, oid, bone.c_str(), a->bones[size_t(b)].t[0], a->bones[size_t(b)].t[1],
                                a->bones[size_t(b)].t[2], a->transform ? "set" : "unset");
            }
    }
    if (watch && gObjectView) {
        auto& last = gScene.watchedObjects; // (by object ID: per scene)
        for (const auto& [id, o] : m.objects()) {
            if (o.person || !o.definition ||
                (std::to_string(id) != watch && o.definition->fileName.find(watch) == std::string::npos))
                continue;
            const auto at = gObjectView->placement(int16_t(id));
            char buf[400];
            std::snprintf(buf, sizeof buf, "container %d slot %d at %.1f %.1f level %d; drawn %s (%.2f %.2f %.2f)",
                          o.data[os2::vm::objdata::ContainerId], o.data[os2::vm::objdata::SlotNumber], o.position.x,
                          o.position.y, o.position.level, at ? "at" : "nowhere", at ? at->t[0] : 0, at ? at->t[1] : 0,
                          at ? at->t[2] : 0);
            std::string s = buf;
            const bool moved = last[id].substr(0, last[id].find(';')) != s.substr(0, s.find(';'));
            if (moved)
                std::printf("opensims2: tick %llu: object %d (%s) %s\n", (unsigned long long)m.ticks, id,
                            o.definition->fileName.c_str(), s.c_str());
            last[id] = s;
        }
    }
}

// OS2_POSE_SHOT=<stem> (test aid): with --screenshot, each Sim (or the
// --character / --sim actor) also drawn alone from its front and its left
// side at eye level, as <stem>-<id>.png, with its posture measured: the
// lean of the pelvis-to-head line from upright (+ forward), the head's
// pitch (+ up) and each arm's angle out from hanging straight down.
void writePoseSheets(os2::Renderer& renderer, const std::string& stem) {
    std::vector<std::pair<int, os2::SimActor*>> sims;
    for (const auto& s : gLotSims)
        sims.push_back({s.id, s.actor});
    if (sims.empty())
        for (auto& a : gActors)
            if (a && a->skin.skeleton.find("pelvis") >= 0)
                sims.push_back({int(sims.size()), a.get()});
    for (auto [id, actor] : sims) {
        const os2::Skeleton& sk = actor->skin.skeleton;
        if (actor->bones.size() != sk.bones.size() || actor->model < 0)
            continue;
        const os2::Xform toModel = actor->placement().inverse();
        auto at = [&](const char* n) -> std::optional<os2::Vec3> {
            const int b = sk.find(n);
            return b < 0 ? std::nullopt : std::optional(toModel.apply(actor->bones[size_t(b)].t));
        };
        constexpr float kDeg = 180.0f / float(M_PI);
        std::string metrics;
        const auto pelvis = at("pelvis"), head = at("head");
        char buf[160];
        if (pelvis && head) {
            const float dy = (*head)[1] - (*pelvis)[1], dz = (*head)[2] - (*pelvis)[2], dx = (*head)[0] - (*pelvis)[0];
            std::snprintf(buf, sizeof buf, "lean %+.0f fwd %+.0f side", std::atan2(dy, dz) * kDeg, std::atan2(-dx, dz) * kDeg);
            metrics += buf;
        }
        if (const auto lf = at("l_foot"), rf = at("r_foot"); lf && rf && head) { // the whole body's tilt (feet to head)
            const float dx = (*head)[0] - ((*lf)[0] + (*rf)[0]) / 2, dy = (*head)[1] - ((*lf)[1] + (*rf)[1]) / 2,
                        dz = (*head)[2] - ((*lf)[2] + (*rf)[2]) / 2;
            std::snprintf(buf, sizeof buf, ", tilt %+.0f fwd %+.0f side", std::atan2(dy, dz) * kDeg, std::atan2(-dx, dz) * kDeg);
            metrics += buf;
        }
        if (const int hb = sk.find("head"); hb >= 0) {
            const auto rest = sk.world(sk.restPose());
            const os2::Vec3 fwd = os2::Xform{conjugate(rest[size_t(hb)].q), {0, 0, 0}}.rotate({0, 1, 0});
            const os2::Vec3 face = toModel.rotate(actor->bones[size_t(hb)].rotate(fwd));
            std::snprintf(buf, sizeof buf, ", head pitch %+.0f", std::asin(std::clamp(face[2], -1.0f, 1.0f)) * kDeg);
            metrics += buf;
        }
        for (const char* side : {"l", "r"}) {
            const auto sh = at((std::string(side) + "_upperarm").c_str()), hand = at((std::string(side) + "_hand").c_str());
            if (!sh || !hand)
                continue;
            const os2::Vec3 d{(*hand)[0] - (*sh)[0], (*hand)[1] - (*sh)[1], (*hand)[2] - (*sh)[2]};
            const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            std::snprintf(buf, sizeof buf, ", %s arm %.0f (fwd %+.2f out %+.2f)", side, std::acos(std::clamp(-d[2] / len, -1.0f, 1.0f)) * kDeg,
                          d[1], side[0] == 'l' ? d[0] : -d[0]);
            metrics += buf;
        }
        for (const char* side : {"l", "r"}) { // each knee's bend (0: straight)
            const auto th = at((std::string(side) + "_thigh").c_str()), ca = at((std::string(side) + "_calf").c_str()),
                       ft = at((std::string(side) + "_foot").c_str());
            if (!th || !ca || !ft)
                continue;
            const os2::Vec3 u{(*ca)[0] - (*th)[0], (*ca)[1] - (*th)[1], (*ca)[2] - (*th)[2]};
            const os2::Vec3 v{(*ft)[0] - (*ca)[0], (*ft)[1] - (*ca)[1], (*ft)[2] - (*ca)[2]};
            const float c = (u[0] * v[0] + u[1] * v[1] + u[2] * v[2]) /
                            std::max(1e-6f, std::sqrt((u[0] * u[0] + u[1] * u[1] + u[2] * u[2]) * (v[0] * v[0] + v[1] * v[1] + v[2] * v[2])));
            std::snprintf(buf, sizeof buf, ", %s knee %.0f", side, std::acos(std::clamp(c, -1.0f, 1.0f)) * kDeg);
            metrics += buf;
        }
        std::string playing;
        if (gSimulation)
            if (const os2::vm::Object* o = gSimulation->machine().object(int16_t(id))) {
                playing = o->animation.name.empty() ? (o->route ? "(walking)" : "(idle)") : o->animation.name;
                for (const auto& ov : o->animation.overlays)
                    playing += " + " + ov.name;
                if (const auto target = gSimulation->lookTarget(*o)) {
                    char look[96];
                    std::snprintf(look, sizeof look, "; looks at %d (%.1f %.1f %.2f)", o->lookAt->target, (*target)[0], (*target)[1],
                                  (*target)[2]);
                    playing += look;
                }
            }
        std::printf("opensims2: pose shot: %s-%d: Sim %d %s; %s; layers %s\n", fs::path(stem).filename().c_str(), id, id, metrics.c_str(), playing.c_str(),
                    actor->player.describe(gSceneNow).c_str());
        // The Sim alone, far from the lot: its instance moved there for the shot.
        const int inst = renderer.instanceOf(actor->model);
        if (inst < 0)
            continue;
        const simd_float4x4 was = renderer.instanceTransform(inst);
        const simd_float3 away = simd_make_float3(-5000, -5000, 0);
        const simd_float3 p = simd_make_float3(actor->position[0], actor->position[1], actor->position[2]);
        simd_float4x4 moved = matrix_identity_float4x4;
        moved.columns[3] = simd_make_float4(away - p, 1);
        renderer.setInstanceTransform(inst, moved);
        const uint32_t w = 320, h = 560;
        os2::RgbaImage sheet;
        sheet.width = w * 2, sheet.height = h;
        sheet.pixels.assign(size_t(sheet.width) * h * 4, 0);
        const float heading = actor->transform ? 0.0f : actor->heading;
        for (int view = 0; view < 2; ++view) {
            // The Sim faces +Y turned by its heading; front, then its left side.
            const float a = heading + (view ? float(M_PI) / 2 : 0);
            const simd_float3 dir = simd_make_float3(-std::sin(a), std::cos(a), 0);
            const simd_float3 target = away + simd_make_float3(0, 0, 0.85f);
            const float fov = 30.0f * float(M_PI) / 180.0f, dist = 4.6f;
            const simd_float3 e = target + dir * dist;
            const simd_float3 f = simd_normalize(target - e);
            const simd_float3 sv = simd_normalize(simd_cross(f, simd_make_float3(0, 0, 1)));
            const simd_float3 u = simd_cross(sv, f);
            os2::FrameView fv;
            fv.view = simd_matrix(simd_make_float4(sv.x, u.x, -f.x, 0), simd_make_float4(sv.y, u.y, -f.y, 0),
                                  simd_make_float4(sv.z, u.z, -f.z, 0),
                                  simd_make_float4(-simd_dot(sv, e), -simd_dot(u, e), simd_dot(f, e), 1));
            const float n = 1, fa = 20, ys = 1.0f / std::tan(fov * 0.5f), zs = fa / (n - fa);
            fv.projection = simd_matrix(simd_make_float4(ys * float(h) / float(w), 0, 0, 0), simd_make_float4(0, ys, 0, 0),
                                        simd_make_float4(0, 0, zs, -1), simd_make_float4(0, 0, zs * n, 0));
            const os2::Renderer::Overlay overlay = renderer.overlay();
            renderer.setOverlay({});
            const os2::RgbaImage img = renderer.capture(w, h, fv, simd_make_float4(0.55f, 0.62f, 0.68f, 1));
            renderer.setOverlay(overlay);
            for (uint32_t y = 0; y < h && img.pixels.size() >= size_t(w) * h * 4; ++y)
                std::memcpy(&sheet.pixels[(size_t(y) * sheet.width + view * w) * 4], &img.pixels[size_t(y) * w * 4], w * 4);
        }
        renderer.setInstanceTransform(inst, was);
        os2::writePng(stem + "-" + std::to_string(id) + ".png", sheet);
    }
}

} // namespace os2app
