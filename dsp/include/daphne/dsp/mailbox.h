#pragma once
// Lock-free hand-over of a parameter object from a control thread to the audio thread.
//
//   control thread: post(std::make_unique<T>(...))   any time; a newer post replaces one not yet taken
//   audio thread:   take()                           the newest posted object, or nullptr
//
// The audio thread never frees memory: the object it replaces goes back to the control side (the
// retired slot) and is freed by the next post() or the destructor. Only one control thread may
// post at a time (callers serialize their settings changes anyway).

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
        delete retired_.exchange(nullptr, std::memory_order_acq_rel);
        delete pending_.exchange(p.release(), std::memory_order_acq_rel);
    }

    // Audio thread: switches to the newest posted object; returns it, or nullptr when nothing new.
    const T* take() {
        T* p = pending_.exchange(nullptr, std::memory_order_acq_rel);
        if (!p) return nullptr;
        T* old = current_;
        current_ = p;
        if (old) {
            T* expected = nullptr;
            if (!retired_.compare_exchange_strong(expected, old, std::memory_order_acq_rel))
                delete old;   // the control side has not collected the previous one yet (rare)
        }
        return p;
    }

    // Audio thread: the object in use (nullptr before the first take()).
    const T* current() const { return current_; }

private:
    std::atomic<T*> pending_{nullptr};
    std::atomic<T*> retired_{nullptr};
    T* current_ = nullptr;   // audio thread only
};

}  // namespace daphne::dsp
