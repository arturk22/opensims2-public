#pragma once

#include "formats/bhav.hpp"
#include "formats/simantics.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace os2 {
class ResourceIndex;
}

namespace os2::vm {

// Group of the global BHAVs/BCONs (opcodes 0x100-0xFFF, in objects.package).
constexpr uint32_t kGlobalGroup = 0x7FD46CD0;
constexpr uint32_t kTypeBhav = 0x42484156;
constexpr uint32_t kTypeBcon = 0x42434F4E;
constexpr uint32_t kTypeGlob = 0x474C4F42;

// Group of the semi-global library a GLOB resource names: CRC24 of the
// lowercased name with the high byte 0x7F (holds for every GLOB in the game).
uint32_t semiGlobalGroup(std::string_view name);

constexpr size_t kTempCount = 20;
constexpr size_t kObjectDataCount = 128; // provisional, until the object model is known
constexpr size_t kGlobalCount = 256;     // provisional
constexpr size_t kMaxStackDepth = 128;

struct VMError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// BHAVs and BCONs by (group, instance), read on demand from a ResourceIndex
// and/or added directly (tests). Returned pointers stay valid.
class CodeLibrary {
public:
    CodeLibrary() = default;
    explicit CodeLibrary(ResourceIndex* index) : index_(index) {}

    const Behavior* behavior(uint32_t group, uint16_t instance);
    const Constants* constants(uint32_t group, uint16_t instance);
    // Semi-global group the objects of `group` use (from their GLOB), 0 if none.
    uint32_t semiGlobalFor(uint32_t group);

    void add(uint32_t group, uint16_t instance, Behavior b);
    void add(uint32_t group, uint16_t instance, Constants c);
    void setSemiGlobal(uint32_t group, uint32_t semiGroup) { semiGlobals_[group] = semiGroup; }

private:
    // A null entry records a resource known to be missing.
    template <class T>
    using Cache = std::unordered_map<uint64_t, std::unique_ptr<T>>;

    ResourceIndex* index_ = nullptr;
    Cache<Behavior> behaviors_;
    Cache<Constants> constants_;
    std::unordered_map<uint32_t, uint32_t> semiGlobals_;
};

using ObjectId = int16_t;

// Minimal object state the interpreter needs; the full object model comes later.
struct Object {
    ObjectId id = 0;
    uint32_t group = 0;     // private BHAVs/BCONs (the group of the object's OBJD)
    uint32_t semiGroup = 0; // semi-global BHAVs/BCONs, 0 if none
    std::vector<int16_t> attributes;
    std::array<int16_t, kObjectDataCount> data{};
};

struct Frame {
    const Behavior* behavior = nullptr;
    uint32_t group = 0;  // where the BHAV was found
    uint16_t instance = 0;
    ObjectId codeOwner = 0; // object whose private/semi-global code and constants are in scope
    uint16_t pc = 0;
    std::vector<int16_t> params;
    std::vector<int16_t> locals;
};

// Temps and the stack object belong to the thread, so subroutines can return
// values in them; parameters and locals belong to each frame.
struct Thread {
    enum class Status { Running, Finished, Failed };

    ObjectId me = 0;
    ObjectId stackObject = 0;
    std::array<int16_t, kTempCount> temps{};
    std::vector<Frame> stack;

    Status status = Status::Running;
    bool result = false;   // return value of the root tree once Finished
    std::string error;     // once Failed: the message and the stack at the failure
    size_t instructions = 0; // total executed
};

// Outcome of a primitive. Yield keeps the thread on the same instruction,
// which runs again on the next call to VM::run.
enum class Exit : uint8_t { True, False, Error, Yield };

class VM;

// What a primitive sees while it runs.
struct Context {
    VM& vm;
    Thread& thread;
    Frame& frame;
    const Behavior::Instruction& instruction;

    Object& me() const;
    Object& stackObject() const; // throws VMError if there is none

    // Variable access by data owner ("scope") and data value, as in the operands.
    int16_t get(uint8_t owner, int16_t data) const;
    void set(uint8_t owner, int16_t data, int16_t value) const;
};

using Primitive = std::function<Exit(Context&)>;

class VM {
public:
    explicit VM(CodeLibrary& code);

    CodeLibrary& code() { return code_; }

    // Objects get ids from 1 up.
    Object& createObject(uint32_t group, size_t attributeCount);
    Object* object(ObjectId id);

    std::array<int16_t, kGlobalCount> globals{};

    void setPrimitive(uint16_t opcode, Primitive p) { primitives_[opcode] = std::move(p); }

    // A thread for `me` that starts in tree `treeId` (a call opcode: global,
    // private or semi-global, resolved against me's code).
    Thread start(ObjectId me, uint16_t treeId, std::span<const int16_t> args = {});
    // Runs until the thread yields, finishes or fails. Exceeding the budget
    // fails the thread (an infinite loop, as far as the game is concerned).
    void run(Thread& thread, size_t maxInstructions = 100000);

    // Called before each instruction executes.
    std::function<void(const Thread&, const Frame&, const Behavior::Instruction&)> trace;

    // Opcodes and data owners that were needed but are not implemented.
    std::map<uint16_t, size_t> missingPrimitives;
    std::map<uint8_t, size_t> missingOwners;

    // Resolves a call opcode for code owned by `owner`: {group, BHAV} or {0, null}.
    std::pair<uint32_t, const Behavior*> resolveTree(const Object& owner, uint16_t treeId);
    const Constants& resolveConstants(const Object& owner, uint16_t table);

private:
    CodeLibrary& code_;
    std::map<ObjectId, Object> objects_;
    std::unordered_map<uint16_t, Primitive> primitives_;

    void push(Thread& t, ObjectId codeOwner, uint16_t treeId, std::array<int16_t, 4> args);
    // Follows a branch; returns false when the thread finished or failed.
    bool branch(Thread& t, Behavior::Target target);
    void fail(Thread& t, const std::string& message);
};

// Registers the primitives implemented so far (Expression).
void registerPrimitives(VM& vm);

} // namespace os2::vm
