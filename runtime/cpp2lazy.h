#ifndef CPP2_CPP2LAZY_H
#define CPP2_CPP2LAZY_H

// Experiment `lazy`: the holder a `name: lazy T = init;` object lowers to.
// The initializer runs on the first `.value()`, exactly once, from whichever
// thread gets there first; other threads block until it has finished.

#include <atomic>
#include <mutex>
#include <new>
#include <type_traits>

namespace cpp2 {

template <typename T>
class lazy {
public:
    using initializer = T (*)();

    constexpr explicit lazy(initializer init) noexcept : init_{init} {}
    lazy(lazy const&) = delete;
    auto operator=(lazy const&) -> lazy& = delete;

    ~lazy() {
        if (constructed_.load()) object()->~T();
    }

    auto value() -> T& {
        std::call_once(once_, [this] {
            ::new (static_cast<void*>(storage_)) T(init_());
            constructed_ = true;
        });
        return *object();
    }

    operator T&() { return value(); }
    auto operator*() -> T& { return value(); }
    auto operator->() -> T* { return &value(); }

    // Whether the initializer has run.
    auto has_value() const noexcept -> bool { return constructed_.load(); }

private:
    auto object() noexcept -> T* { return std::launder(reinterpret_cast<T*>(storage_)); }

    initializer init_;
    std::once_flag once_;
    std::atomic<bool> constructed_{false};
    alignas(T) unsigned char storage_[sizeof(T)];
};

}

#endif
