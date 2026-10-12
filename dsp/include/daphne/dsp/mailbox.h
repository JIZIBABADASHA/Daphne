#pragma once
// Lock-free hand-over of a parameter object from a control thread to the audio thread.
//
//   control thread: post(std::make_unique<T>(...))   any time; a newer post replaces one not yet taken
//   audio thread:   take()                           the newest posted object, or nullptr
//
// The audio thread never frees memory: the object it replaces goes back to the control side (the
// retired slot) and is freed by the next post() or the destructor. Only the audio thread fills the
// slot, only after seeing it empty, and before it takes the pending object; the control side only
// empties it, after publishing. So once a post() has returned, either the audio thread already holds
// that object or the slot is empty and its next take() gets it. While the slot is full, take()
// leaves the pending object for the next call. Only one control thread may post at a time (callers
// serialize their settings changes anyway).

#include <atomic>
#include <memory>

namespace daphne::dsp {

template <class T>
class Mailbox {
public:
    Mailbox() = default;
    Mailbox(const Mailbox&) = delete;
    Mailbox& operator=(const Mailbox&) = delete;
    ~Mailbox() {
        delete pending_.load();
        delete retired_.load();
        delete current_;
    }

    void post(std::unique_ptr<T> p) {
        delete pending_.exchange(p.release(), std::memory_order_acq_rel);   // one never taken
        delete retired_.exchange(nullptr, std::memory_order_acq_rel);      // after publishing: see take()
    }

    // Audio thread: switches to the newest posted object; returns it, or nullptr when nothing new (or
    // when the retired slot is still full: then the next call takes it, after post() has emptied it).
    const T* take() {
        if (retired_.load(std::memory_order_acquire) || !pending_.load(std::memory_order_acquire)) return nullptr;
        // the slot is empty (only this thread fills it): hand the current object back first, then take
        // the pending one (non-null: only this thread empties pending)
        if (current_) retired_.store(current_, std::memory_order_release);
        current_ = pending_.exchange(nullptr, std::memory_order_acq_rel);
        return current_;
    }

    // Audio thread: the object in use (nullptr before the first take()).
    const T* current() const { return current_; }

private:
    std::atomic<T*> pending_{nullptr};
    std::atomic<T*> retired_{nullptr};
    T* current_ = nullptr;   // audio thread only
};

}  // namespace daphne::dsp
