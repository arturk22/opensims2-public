#include "vm.hpp"

#include "formats/binreader.hpp"
#include "formats/resindex.hpp"
#include "formats/strings.hpp"

#include <algorithm>
#include <cstdio>

namespace os2::vm {

namespace {

uint64_t key(uint32_t group, uint16_t instance) { return uint64_t(group) << 32 | instance; }

template <class T>
const T* loadCached(std::unordered_map<uint64_t, std::unique_ptr<T>>& cache, ResourceIndex* index, uint32_t type,
                    uint32_t group, uint16_t instance) {
    const auto k = key(group, instance);
    if (auto it = cache.find(k); it != cache.end())
        return it->second.get();
    std::unique_ptr<T> value;
    if (index)
        if (auto data = index->read(Tgi{type, group, instance, 0}))
            value = std::make_unique<T>(T::parse(*data));
    return cache.emplace(k, std::move(value)).first->second.get();
}

std::string treeName(const Frame& f) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "0x%08X/0x%04X", f.group, f.instance);
    return std::string(buf) + " \"" + f.behavior->fileName + "\"";
}

template <class V>
auto& slot(V& values, int idx, const char* what) {
    if (idx < 0 || size_t(idx) >= values.size())
        throw VMError(std::string(what) + " " + std::to_string(idx) + " out of range (" +
                      std::to_string(values.size()) + ")");
    return values[size_t(idx)];
}

// Attributes and semi-attributes: an index past the end grows the array.
int16_t& growing(std::vector<int16_t>& values, int idx, const char* what) {
    if (idx < 0 || size_t(idx) >= kMaxAttributes)
        throw VMError(std::string(what) + " " + std::to_string(idx) + " out of range");
    if (size_t(idx) >= values.size())
        values.resize(size_t(idx) + 1);
    return values[size_t(idx)];
}

Person& personOf(Object& o) {
    if (!o.person)
        throw VMError("object " + std::to_string(o.id) + " is not a person");
    return *o.person;
}

// Field of the editor's OBJD numbering: 0-1 are the u32 version, i >= 2 the
// field at file offset 0x40 + 2i.
int16_t definitionField(const Object& o, int idx) {
    if (!o.definition)
        throw VMError("object " + std::to_string(o.id) + " has no definition");
    if (idx < 0)
        throw VMError("definition field " + std::to_string(idx) + " out of range");
    if (idx < 2)
        return int16_t(o.definition->version >> (16 * idx));
    return int16_t(o.definition->field(0x40 + 2 * size_t(idx)));
}

} // namespace

uint32_t semiGlobalGroup(std::string_view name) { return (nameInstanceHash(name) & 0x00FFFFFF) | 0x7F000000; }

const Behavior* CodeLibrary::behavior(uint32_t group, uint16_t instance) {
    return loadCached(behaviors_, index_, kTypeBhav, group, instance);
}

const Constants* CodeLibrary::constants(uint32_t group, uint16_t instance) {
    return loadCached(constants_, index_, kTypeBcon, group, instance);
}

const ObjectDefinition* CodeLibrary::definition(uint32_t group, uint16_t instance) {
    return loadCached(definitions_, index_, kTypeObjd, group, instance);
}

std::optional<std::pair<uint32_t, uint16_t>> CodeLibrary::definitionByGuid(uint32_t guid) {
    if (!guidsScanned_ && index_) {
        guidsScanned_ = true;
        for (const auto& loc : index_->ofType(kTypeObjd)) {
            const auto& tgi = loc.entry->tgi;
            // Keeps the first OBJD of a GUID, like the rest of the index.
            byGuid_.emplace(ObjectDefinition::parse(index_->read(loc)).guid(),
                            std::pair{tgi.group, uint16_t(tgi.instance)});
        }
    }
    if (auto it = byGuid_.find(guid); it != byGuid_.end())
        return it->second;
    return std::nullopt;
}

size_t CodeLibrary::attributeLabelCount(uint32_t group) {
    if (auto it = labelCounts_.find(group); it != labelCounts_.end())
        return it->second;
    size_t n = 0;
    if (index_)
        if (auto data = index_->read(Tgi{kTypeStr, group, 0x100, 0}))
            for (const auto& e : StringTable::parse(*data).entries)
                n += e.language == lang::EnglishUS;
    return labelCounts_[group] = n;
}

uint32_t CodeLibrary::semiGlobalFor(uint32_t group) {
    if (auto it = semiGlobals_.find(group); it != semiGlobals_.end())
        return it->second;
    uint32_t semi = 0;
    if (index_)
        for (uint32_t instance : {0x01u, 0x80u})
            if (auto data = index_->read(Tgi{kTypeGlob, group, instance, 0})) {
                semi = semiGlobalGroup(SemiGlobal::parse(*data).semiGlobalName);
                break;
            }
    semiGlobals_[group] = semi;
    return semi;
}

void CodeLibrary::add(uint32_t group, uint16_t instance, Behavior b) {
    behaviors_[key(group, instance)] = std::make_unique<Behavior>(std::move(b));
}

void CodeLibrary::add(uint32_t group, uint16_t instance, Constants c) {
    constants_[key(group, instance)] = std::make_unique<Constants>(std::move(c));
}

void CodeLibrary::add(uint32_t group, uint16_t instance, ObjectDefinition d) {
    byGuid_.emplace(d.guid(), std::pair{group, instance});
    definitions_[key(group, instance)] = std::make_unique<ObjectDefinition>(std::move(d));
}

// --- Context ---------------------------------------------------------------

Object& Context::me() const {
    auto* o = vm.object(thread.me);
    if (!o)
        throw VMError("thread object " + std::to_string(thread.me) + " does not exist");
    return *o;
}

Object& Context::stackObject() const {
    auto* o = vm.object(frame.stackObject);
    if (!o)
        throw VMError("stack object " + std::to_string(frame.stackObject) + " does not exist");
    return *o;
}

// Storage behind a data owner, or null for the computed/read-only ones.
// Owners whose index comes from another variable read it first.
static int16_t* storage(const Context& c, uint8_t owner, int16_t data) {
    auto& t = c.thread;
    switch (owner) {
    case 0x00: return &growing(c.me().attributes, data, "attribute");
    case 0x01: return &growing(c.stackObject().attributes, data, "stack object attribute");
    case 0x02: return &growing(c.me().semiAttributes, data, "semi-attribute");
    case 0x03: return &slot(c.me().data, data, "object data");
    case 0x04: return &slot(c.stackObject().data, data, "stack object data");
    case 0x05: return &growing(c.stackObject().semiAttributes, data, "stack object semi-attribute");
    case 0x06: return &slot(c.vm.globals, data, "global");
    case 0x08: return &slot(t.temps, data, "temp");
    case 0x09: return &slot(c.frame.params, data, "param");
    case 0x0B: return &slot(t.temps, slot(t.temps, data, "temp"), "temp[temp]");
    case 0x0E: return &slot(personOf(c.me()).motives, data, "motive");
    case 0x0F: return &slot(personOf(c.stackObject()).motives, data, "stack object motive");
    case 0x11: return &slot(personOf(c.stackObject()).motives, slot(t.temps, data, "temp"), "stack object motive[temp]");
    case 0x12: return &slot(personOf(c.me()).data, data, "person data");
    case 0x13: return &slot(personOf(c.stackObject()).data, data, "stack object person data");
    case 0x16:
        return &growing(c.stackObject().attributes, slot(c.frame.params, data, "param"), "stack object attribute[param]");
    case 0x19: return &slot(c.frame.locals, data, "local");
    case 0x1E: return &slot(personOf(c.me()).data, slot(t.temps, data, "temp"), "person data[temp]");
    case 0x1F:
        return &slot(personOf(c.stackObject()).data, slot(t.temps, data, "temp"), "stack object person data[temp]");
    case 0x32:
        return &growing(c.stackObject().semiAttributes, slot(c.frame.params, data, "param"),
                        "stack object semi-attribute[param]");
    default: return nullptr;
    }
}

int16_t Context::get(uint8_t owner, int16_t data) const {
    if (int16_t* v = storage(*this, owner, data))
        return *v;
    switch (owner) {
    case 0x07: return data; // literal
    case 0x0A: return frame.stackObject;
    case 0x15: return definitionField(stackObject(), data);
    case 0x1A: {
        // High bits pick the BCON (private, semi-global or global), low 7 bits the entry.
        auto* owner = vm.object(frame.codeOwner);
        if (!owner)
            throw VMError("code owner does not exist");
        const auto& c = vm.resolveConstants(*owner, uint16_t(data) >> 7);
        return slot(c.values, data & 0x7F, "constant");
    }
    default:
        ++vm.missingOwners[owner];
        throw VMError("data owner " + hexString(owner) + " not implemented");
    }
}

void Context::set(uint8_t owner, int16_t data, int16_t value) const {
    if (int16_t* v = storage(*this, owner, data)) {
        *v = value;
        return;
    }
    switch (owner) {
    case 0x0A: frame.stackObject = value; return;
    case 0x07:
    case 0x15: // the editor marks only some definition fields read-only; writes are not supported yet
    case 0x1A: throw VMError("data owner " + hexString(owner) + " is read-only");
    default:
        ++vm.missingOwners[owner];
        throw VMError("data owner " + hexString(owner) + " not implemented");
    }
}

// --- VM --------------------------------------------------------------------

VM::VM(CodeLibrary& code) : code_(code) { registerPrimitives(*this); }

Object& VM::createObject(uint32_t group, size_t attributeCount) {
    ObjectId id = 1;
    while (objects_.count(id))
        ++id;
    Object& o = objects_[id];
    o.id = id;
    o.group = group;
    o.semiGroup = code_.semiGlobalFor(group);
    o.attributes.resize(attributeCount);
    if (o.semiGroup)
        o.semiAttributes.resize(code_.attributeLabelCount(o.semiGroup));
    o.data[objdata::ObjectId] = id;
    o.data[objdata::SemiAttributeCount] = int16_t(o.semiAttributes.size());
    return o;
}

Object& VM::instantiate(uint32_t group, uint16_t definitionInstance) {
    const ObjectDefinition* def = code_.definition(group, definitionInstance);
    if (!def)
        throw VMError("OBJD " + hexString(group) + "/" + hexString(definitionInstance) + " not found");
    Object& o = createObject(group, std::max<size_t>(def->numAttributes(), code_.attributeLabelCount(group)));
    o.definition = def;
    if (def->objectType() == kObjectTypePerson)
        o.person.emplace();
    return o;
}

Object* VM::instantiateGuid(uint32_t guid) {
    auto def = code_.definitionByGuid(guid);
    return def ? &instantiate(def->first, def->second) : nullptr;
}

Object* VM::object(ObjectId id) {
    auto it = objects_.find(id);
    return it == objects_.end() ? nullptr : &it->second;
}

std::pair<uint32_t, const Behavior*> VM::resolveTree(const Object& owner, uint16_t treeId) {
    uint32_t group = 0;
    if (treeId >= 0x100 && treeId < 0x1000)
        group = kGlobalGroup;
    else if (treeId >= 0x1000 && treeId < 0x2000)
        group = owner.group;
    else if (treeId >= 0x2000)
        group = owner.semiGroup;
    const Behavior* b = group ? code_.behavior(group, treeId) : nullptr;
    return {b ? group : 0, b};
}

const Constants& VM::resolveConstants(const Object& owner, uint16_t table) {
    // Private and semi-global tables resolve against the code owner, also
    // when the running code is semi-global or global.
    uint32_t group;
    uint16_t instance;
    if (table < 64)
        group = owner.group, instance = uint16_t(0x1000 + table);
    else if (table < 128)
        group = owner.semiGroup, instance = uint16_t(0x2000 + table - 64);
    else
        group = kGlobalGroup, instance = uint16_t(0x100 + table - 128);
    const Constants* c = group ? code_.constants(group, instance) : nullptr;
    if (!c)
        throw VMError("BCON " + hexString(group) + "/" + hexString(instance) + " not found");
    return *c;
}

Thread VM::start(ObjectId me, uint16_t treeId, std::span<const int16_t> args, ObjectId stackObject,
                 ObjectId codeOwner) {
    Thread t;
    t.me = me;
    std::array<int16_t, 4> a{};
    std::copy_n(args.begin(), std::min<size_t>(args.size(), 4), a.begin());
    try {
        push(t, codeOwner ? codeOwner : me, treeId, a);
        t.stack.back().stackObject = stackObject;
    } catch (const std::exception& e) {
        fail(t, e.what());
    }
    return t;
}

void VM::push(Thread& t, ObjectId codeOwner, uint16_t treeId, std::array<int16_t, 4> args) {
    const Object* owner = object(codeOwner);
    if (!owner)
        throw VMError("code owner " + std::to_string(codeOwner) + " does not exist");
    auto [group, behavior] = resolveTree(*owner, treeId);
    if (!behavior)
        throw VMError("tree " + hexString(treeId) + " not found");
    if (t.stack.size() >= kMaxStackDepth)
        throw VMError("stack overflow");
    Frame f;
    f.behavior = behavior;
    f.group = group;
    f.instance = treeId;
    f.codeOwner = codeOwner;
    if (!t.stack.empty())
        f.stackObject = t.stack.back().stackObject;
    f.params.assign(std::max<size_t>(behavior->argCount, args.size()), 0);
    std::copy(args.begin(), args.end(), f.params.begin());
    f.locals.assign(behavior->localCount, 0);
    if (behavior->instructions.empty())
        throw VMError("tree " + hexString(treeId) + " has no instructions");
    t.stack.push_back(std::move(f));
}

void VM::fail(Thread& t, const std::string& message) {
    t.status = Thread::Status::Failed;
    t.error = message;
    constexpr size_t kShownFrames = 8;
    for (size_t i = 0; i < t.stack.size() && i < kShownFrames; ++i) {
        const Frame& f = t.stack[t.stack.size() - 1 - i];
        t.error += "\n  at " + treeName(f) + " #" + std::to_string(f.pc);
    }
    if (t.stack.size() > kShownFrames)
        t.error += "\n  ... " + std::to_string(t.stack.size() - kShownFrames) + " more frames";
}

bool VM::branch(Thread& t, Behavior::Target target) {
    for (;;) {
        switch (target.kind) {
        case Behavior::TargetKind::Instruction:
            t.stack.back().pc = target.index;
            return true;
        case Behavior::TargetKind::Error:
            fail(t, "error branch taken");
            return false;
        case Behavior::TargetKind::True:
        case Behavior::TargetKind::False: {
            const bool value = target.kind == Behavior::TargetKind::True;
            t.stack.pop_back();
            if (t.stack.empty()) {
                t.status = Thread::Status::Finished;
                t.result = value;
                return false;
            }
            // The caller's call instruction branches on the returned value.
            const Frame& caller = t.stack.back();
            const auto& call = caller.behavior->instructions[caller.pc];
            target = value ? call.onTrue : call.onFalse;
            break;
        }
        }
    }
}

void VM::run(Thread& t, size_t maxInstructions) {
    for (size_t budget = maxInstructions; t.status == Thread::Status::Running;) {
        if (budget-- == 0) {
            fail(t, "instruction budget exhausted (" + std::to_string(maxInstructions) + ")");
            return;
        }
        if (!object(t.me)) {
            t.status = Thread::Status::Killed;
            return;
        }
        Frame& frame = t.stack.back();
        const auto& in = frame.behavior->instructions[frame.pc];
        if (trace)
            trace(t, frame, in);
        ++t.instructions;
        try {
            Context ctx{*this, t, frame, in};
            if (in.opcode >= 0x100) {
                // Call: four arguments, either literals, the temps 0-3 (all
                // literals 0xFFFF) or, with byte 12 = 1, (owner u8, data u16) pairs.
                const auto& o = in.operands;
                std::array<int16_t, 4> args{};
                if (o[12] == 1) {
                    const Object* owner = object(frame.codeOwner);
                    const Behavior* callee = owner ? resolveTree(*owner, in.opcode).second : nullptr;
                    const size_t n = callee ? std::min<size_t>(callee->argCount, 4) : 0;
                    for (size_t k = 0; k < n; ++k)
                        args[k] = ctx.get(o[k * 3], int16_t(le16(&o[k * 3 + 1])));
                } else if (std::all_of(o.begin(), o.begin() + 8, [](uint8_t b) { return b == 0xFF; })) {
                    std::copy_n(t.temps.begin(), 4, args.begin());
                } else {
                    for (size_t k = 0; k < 4; ++k)
                        args[k] = int16_t(le16(&o[k * 2]));
                }
                push(t, frame.codeOwner, in.opcode, args);
                continue;
            }
            auto it = primitives_.find(in.opcode);
            if (it == primitives_.end()) {
                ++missingPrimitives[in.opcode];
                fail(t, "primitive " + hexString(in.opcode) + " (" + primitiveName(in.opcode) + ") not implemented");
                return;
            }
            switch (it->second(ctx)) {
            case Exit::Yield: return;
            case Exit::Error: fail(t, "primitive " + primitiveName(in.opcode) + " returned error"); return;
            case Exit::True: branch(t, in.onTrue); break;
            case Exit::False: branch(t, in.onFalse); break;
            }
        } catch (const std::exception& e) { // VMError, or a resource that failed to parse
            fail(t, e.what());
        }
    }
}

} // namespace os2::vm
