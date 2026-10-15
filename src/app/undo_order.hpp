#pragma once

// The game's one undo manager over Buy and Build mode
// (cTSUserToolUndoManager, docs/buy-build-mac.md §7.24): every buy and build
// tool submits to the same manager (cTSUserToolBuyBuild +0x44, the sim
// system's vtable 0xb8), so after switching between the two modes Ctrl-Z
// undoes the last change of either; a new change in either releases what
// both could redo; and only entering Live mode empties it (the Live mode's
// SetMode object, BV 0x159ff64, calls FlushCommandQueues): what was sold or
// whose purchase was undone dies then (ResetSelf). Buy mode and Build mode
// keep their own changes; this keeps whose change came last.

#include <array>
#include <functional>
#include <vector>

namespace os2 {

class UndoOrder {
public:
    enum Owner { Buy = 0, Build = 1 };
    struct Hooks {
        std::function<bool()> undo, redo; // the owner's last change undone / redone
        std::function<void()> dropRedo;   // the owner's redo list released
        std::function<void()> flush;      // both lists released
    };
    void attach(Owner o, Hooks h) { hooks_[size_t(o)] = std::move(h); }

    // `o` pushed a new change on its own undo list (and dropped its own redo
    // list): SubmitUndoable releases the redo list, the other owner's too.
    void pushed(Owner o) {
        order_.push_back(o);
        redoOrder_.clear();
        const Hooks& other = hooks_[size_t(1 - o)];
        if (other.dropRedo)
            other.dropRedo();
    }
    bool canUndo() const { return !order_.empty(); }
    bool canRedo() const { return !redoOrder_.empty(); }
    // UndoLastCommand / RedoLastCommand: the last change, whoever made it.
    bool undo() {
        if (order_.empty())
            return false;
        const Owner o = order_.back();
        const Hooks& h = hooks_[size_t(o)];
        if (!h.undo || !h.undo())
            return false;
        order_.pop_back();
        redoOrder_.push_back(o);
        return true;
    }
    bool redo() {
        if (redoOrder_.empty())
            return false;
        const Owner o = redoOrder_.back();
        const Hooks& h = hooks_[size_t(o)];
        if (!h.redo || !h.redo())
            return false;
        redoOrder_.pop_back();
        order_.push_back(o);
        return true;
    }
    // FlushCommandQueues.
    void flush() {
        for (const Hooks& h : hooks_)
            if (h.flush)
                h.flush();
        order_.clear();
        redoOrder_.clear();
    }

private:
    std::array<Hooks, 2> hooks_;
    std::vector<Owner> order_, redoOrder_;
};

} // namespace os2
