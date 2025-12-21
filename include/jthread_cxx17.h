#pragma once
#ifndef JTHREAD_CXX17_H_
#    define JTHREAD_CXX17_H_

// 定义宏开关：1 使用自旋锁，0 使用互斥锁
// 自旋锁适合低竞争场景，互斥锁适合高竞争或节能需求
#    ifndef JTHREAD_USE_SPINLOCK
#        define JTHREAD_USE_SPINLOCK 0
#    endif

#    include <atomic>
#    include <chrono>
#    include <condition_variable>
#    include <mutex>
#    include <thread>
#    include <type_traits>

namespace jthread_cxx17 {

struct nostopstate_t {
    explicit nostopstate_t() = default;
};
inline constexpr nostopstate_t nostopstate{};

class stop_source;

class stop_token {
public:
    stop_token() noexcept = default;

    stop_token(const stop_token &) noexcept = default;
    stop_token(stop_token &&) noexcept      = default;

    ~stop_token() = default;

    stop_token &operator=(const stop_token &) noexcept = default;

    stop_token &operator=(stop_token &&) noexcept = default;

    [[nodiscard]] bool stop_possible() const noexcept {
        return static_cast<bool>(_M_state) && _M_state->_M_stop_possible();
    }

    [[nodiscard]] bool stop_requested() const noexcept {
        return static_cast<bool>(_M_state) && _M_state->_M_stop_requested();
    }

    void swap(stop_token &__rhs) noexcept { _M_state.swap(__rhs._M_state); }

    [[nodiscard]] friend bool operator==(const stop_token &__a, const stop_token &__b) {
        return __a._M_state == __b._M_state;
    }

    friend void swap(stop_token &__lhs, stop_token &__rhs) noexcept { __lhs.swap(__rhs); }

private:
    friend class stop_source;
    template <typename _Callback> friend class stop_callback;

    struct binary_semaphore {
        explicit binary_semaphore(int __d) : _M_available(__d > 0) {}

        void release() {
            std::unique_lock<std::mutex> lock(_M_mutex);
            _M_available = true;
            _M_cv.notify_one();
        }

        void acquire() {
            std::unique_lock<std::mutex> lock(_M_mutex);
            _M_cv.wait(lock, [this]() { return _M_available; });
            _M_available = false;
        }

    private:
        std::mutex              _M_mutex;
        std::condition_variable _M_cv;
        bool                    _M_available;
    };

    struct _Stop_cb {
        using __cb_type = void(_Stop_cb *) noexcept;
        __cb_type       *_M_callback;
        _Stop_cb        *_M_prev      = nullptr;
        _Stop_cb        *_M_next      = nullptr;
        bool            *_M_destroyed = nullptr;
        binary_semaphore _M_done{0};

        explicit _Stop_cb(__cb_type *__cb) : _M_callback(__cb) {}

        void _M_run() noexcept { _M_callback(this); }
    };

    struct _Stop_state_t {
        using value_type = uint32_t;

        static constexpr value_type _S_stop_requested_bit = 1;
#    if JTHREAD_USE_SPINLOCK
        static constexpr value_type _S_locked_bit = 2;
#    endif
        static constexpr value_type _S_ssrc_counter_inc = 4;

        std::atomic<value_type> _M_value{_S_ssrc_counter_inc};
        _Stop_cb               *_M_head = nullptr;
        std::thread::id         _M_requester;
        std::mutex              _M_mutex; // 互斥锁版本
#    if JTHREAD_USE_SPINLOCK
        bool _M_try_lock(value_type &__curval, std::memory_order __failure = std::memory_order_acquire) noexcept {
            return _M_do_try_lock(__curval, 0, std::memory_order_acquire, __failure);
        }
        bool _M_try_lock_and_stop(value_type &__curval) noexcept {
            return _M_do_try_lock(__curval, _S_stop_requested_bit, std::memory_order_acq_rel,
                                  std::memory_order_acquire);
        }
        bool _M_do_try_lock(value_type &__curval, value_type __newbits, std::memory_order __success,
                            std::memory_order __failure) noexcept {
            if (__curval & _S_locked_bit) {
                std::this_thread::yield();
                __curval = _M_value.load(__failure);
                return false;
            }
            __newbits |= _S_locked_bit;
            return _M_value.compare_exchange_weak(__curval, __curval | __newbits, __success, __failure);
        }
#    endif

        bool _M_stop_possible() noexcept {
#    if JTHREAD_USE_SPINLOCK
            return _M_value.load(std::memory_order_acquire) & ~_S_locked_bit;
#    else
            return _M_value.load(std::memory_order_acquire) >= _S_ssrc_counter_inc;
#    endif
        }

        bool _M_stop_requested() noexcept { return _M_value.load(std::memory_order_acquire) & _S_stop_requested_bit; }

        void _M_add_ssrc() noexcept { _M_value.fetch_add(_S_ssrc_counter_inc, std::memory_order_relaxed); }

        void _M_sub_ssrc() noexcept { _M_value.fetch_sub(_S_ssrc_counter_inc, std::memory_order_release); }

#    if JTHREAD_USE_SPINLOCK
        void _M_lock() noexcept {
            auto         __old   = _M_value.load(std::memory_order_relaxed);
            unsigned int __spins = 0;
            while (!_M_try_lock(__old, std::memory_order_relaxed)) {
                if (++__spins > 100) { // 避免过度自旋，转为 yield
                    std::this_thread::yield();
                    __spins = 0;
                }
            }
        }

        void _M_unlock() noexcept { _M_value.fetch_sub(_S_locked_bit, std::memory_order_release); }
#    else
        void _M_lock() { _M_mutex.lock(); }

        void _M_unlock() { _M_mutex.unlock(); }
#    endif

        bool _M_request_stop() noexcept {
#    if JTHREAD_USE_SPINLOCK
            // 快速路径：用 relaxed 检查是否已停止，避免不必要的锁竞争
            if (_M_value.load(std::memory_order_relaxed) & _S_stop_requested_bit) {
                return false;
            }

            auto __old = _M_value.load(std::memory_order_acquire);
            do {
                if (__old & _S_stop_requested_bit) // stop request already made
                    return false;
            } while (!_M_try_lock_and_stop(__old));

            _M_requester = std::this_thread::get_id();

            while (_M_head) {
                bool      __last_cb;
                _Stop_cb *__cb = _M_head;
                _M_head        = _M_head->_M_next;
                if (_M_head) {
                    _M_head->_M_prev = nullptr;
                    __last_cb        = false;
                } else
                    __last_cb = true;

                // Allow other callbacks to be unregistered while __cb runs.
                _M_unlock();

                bool __destroyed   = false;
                __cb->_M_destroyed = &__destroyed;

                // run callback
                __cb->_M_run();

                if (!__destroyed) {
                    __cb->_M_destroyed = nullptr;
                    __cb->_M_done.release();
                }

                if (__last_cb)
                    return true;

                _M_lock();
            }

            _M_unlock();
            return true;
#    else
            std::unique_lock<std::mutex> lock(_M_mutex);
            if (_M_value.load(std::memory_order_acquire) & _S_stop_requested_bit) {
                return false;
            }
            _M_value.fetch_or(_S_stop_requested_bit, std::memory_order_acq_rel);
            _M_requester = std::this_thread::get_id();

            while (_M_head) {
                bool      __last_cb;
                _Stop_cb *__cb = _M_head;
                _M_head        = _M_head->_M_next;
                if (_M_head) {
                    _M_head->_M_prev = nullptr;
                    __last_cb        = false;
                } else
                    __last_cb = true;

                // Allow other callbacks to be unregistered while __cb runs.
                lock.unlock();

                bool __destroyed   = false;
                __cb->_M_destroyed = &__destroyed;

                // run callback
                __cb->_M_run();

                if (!__destroyed) {
                    __cb->_M_destroyed = nullptr;
                    __cb->_M_done.release();
                }

                if (__last_cb)
                    return true;

                lock.lock();
            }

            return true;
#    endif
        }

        bool _M_register_callback(_Stop_cb *__cb) noexcept {
#    if JTHREAD_USE_SPINLOCK
            auto __old = _M_value.load(std::memory_order_acquire);
            do {
                if (__old & _S_stop_requested_bit) {
                    __cb->_M_run();
                    return false;
                }

                if (__old < _S_ssrc_counter_inc)
                    return false;
            } while (!_M_try_lock(__old));

            __cb->_M_next = _M_head;
            if (_M_head) {
                _M_head->_M_prev = __cb;
            }
            _M_head = __cb;
            _M_unlock();
            return true;
#    else
            std::unique_lock<std::mutex> lock(_M_mutex);
            auto                         __val = _M_value.load(std::memory_order_acquire);
            if (__val & _S_stop_requested_bit) {
                __cb->_M_run();
                return false;
            }

            if (__val < _S_ssrc_counter_inc)
                return false;

            __cb->_M_next = _M_head;
            if (_M_head) {
                _M_head->_M_prev = __cb;
            }
            _M_head = __cb;
            return true;
#    endif
        }

        void _M_remove_callback(_Stop_cb *__cb) {
#    if JTHREAD_USE_SPINLOCK
            _M_lock();

            if (__cb == _M_head) {
                _M_head = _M_head->_M_next;
                if (_M_head)
                    _M_head->_M_prev = nullptr;
                _M_unlock();
                return;
            } else if (__cb->_M_prev) {
                __cb->_M_prev->_M_next = __cb->_M_next;
                if (__cb->_M_next)
                    __cb->_M_next->_M_prev = __cb->_M_prev;
                _M_unlock();
                return;
            }

            _M_unlock();

            if (!(_M_requester == std::this_thread::get_id())) {
                __cb->_M_done.acquire();
                return;
            }

            if (__cb->_M_destroyed)
                *__cb->_M_destroyed = true;
#    else
            std::unique_lock<std::mutex> lock(_M_mutex);

            if (__cb == _M_head) {
                _M_head = _M_head->_M_next;
                if (_M_head)
                    _M_head->_M_prev = nullptr;
                return;
            } else if (__cb->_M_prev) {
                __cb->_M_prev->_M_next = __cb->_M_next;
                if (__cb->_M_next)
                    __cb->_M_next->_M_prev = __cb->_M_prev;
                return;
            }

            lock.unlock();

            if (!(_M_requester == std::this_thread::get_id())) {
                __cb->_M_done.acquire();
                return;
            }

            if (__cb->_M_destroyed)
                *__cb->_M_destroyed = true;
#    endif
        };
    };

    struct _Stop_state_ref {
        _Stop_state_ref() = default;

        explicit _Stop_state_ref(const stop_source &) : _M_ptr(std::make_shared<_Stop_state_t>()) {}

        _Stop_state_ref(const _Stop_state_ref &__other) noexcept : _M_ptr(__other._M_ptr) {}

        _Stop_state_ref(_Stop_state_ref &&__other) noexcept : _M_ptr(std::move(__other._M_ptr)) {}

        _Stop_state_ref &operator=(const _Stop_state_ref &__other) noexcept {
            _M_ptr = __other._M_ptr;
            return *this;
        }

        _Stop_state_ref &operator=(_Stop_state_ref &&__other) noexcept {
            _M_ptr = std::move(__other._M_ptr);
            return *this;
        }

        ~_Stop_state_ref() = default;

        void swap(_Stop_state_ref &__other) noexcept { std::swap(_M_ptr, __other._M_ptr); }

        explicit operator bool() const noexcept { return _M_ptr != nullptr; }

        _Stop_state_t *operator->() const noexcept { return _M_ptr.get(); }

#    if __cpp_impl_three_way_comparison >= 201907L
        friend bool operator==(const _Stop_state_ref &, const _Stop_state_ref &) = default;
#    else
        friend bool operator==(const _Stop_state_ref &__lhs, const _Stop_state_ref &__rhs) noexcept {
            return __lhs._M_ptr == __rhs._M_ptr;
        }

        friend bool operator!=(const _Stop_state_ref &__lhs, const _Stop_state_ref &__rhs) noexcept {
            return __lhs._M_ptr != __rhs._M_ptr;
        }
#    endif

    private:
        std::shared_ptr<_Stop_state_t> _M_ptr;
    };

    _Stop_state_ref _M_state;

    explicit stop_token(const _Stop_state_ref &__state) noexcept : _M_state{__state} {}
};

class stop_source {
public:
    stop_source() : _M_state(*this) {}

    explicit stop_source(nostopstate_t) noexcept {}

    stop_source(const stop_source &__other) noexcept : _M_state(__other._M_state) {}

    stop_source(stop_source &&) noexcept = default;

    stop_source &operator=(const stop_source &__other) noexcept {
        _M_state = __other._M_state;
        return *this;
    }

    stop_source &operator=(stop_source &&) noexcept = default;

    ~stop_source() {}

    [[nodiscard]]
    bool stop_possible() const noexcept {
        return static_cast<bool>(_M_state);
    }

    [[nodiscard]]
    bool stop_requested() const noexcept {
        return static_cast<bool>(_M_state) && _M_state->_M_stop_requested();
    }

    bool request_stop() const noexcept {
        if (stop_possible())
            return _M_state->_M_request_stop();
        return false;
    }

    [[nodiscard]]
    stop_token get_token() const noexcept {
        return stop_token{_M_state};
    }

    void swap(stop_source &__other) noexcept { _M_state.swap(__other._M_state); }

    [[nodiscard]]
    friend bool operator==(const stop_source &__a, const stop_source &__b) noexcept {
        return __a._M_state == __b._M_state;
    }

    friend void swap(stop_source &__lhs, stop_source &__rhs) noexcept { __lhs.swap(__rhs); }

private:
    stop_token::_Stop_state_ref _M_state;
};

template <typename _Callback> class [[nodiscard]] stop_callback {
    static_assert(std::is_nothrow_destructible_v<_Callback>);
    static_assert(std::is_invocable_v<_Callback>);

public:
    using callback_type = _Callback;

    template <typename _Cb, std::enable_if_t<std::is_constructible_v<_Callback, _Cb>, int> = 0>
    explicit stop_callback(const stop_token &__token,
                           _Cb             &&__cb) noexcept(std::is_nothrow_constructible_v<_Callback, _Cb>)
        : _M_cb(std::forward<_Cb>(__cb)) {
        if (auto __state = __token._M_state) {
            if (__state->_M_register_callback(&_M_cb))
                _M_state.swap(__state);
        }
    }

    template <typename _Cb, std::enable_if_t<std::is_constructible_v<_Callback, _Cb>, int> = 0>
    explicit stop_callback(stop_token &&__token, _Cb &&__cb) noexcept(std::is_nothrow_constructible_v<_Callback, _Cb>)
        : _M_cb(std::forward<_Cb>(__cb)) {
        auto __state = std::move(__token._M_state);
        if (__state && __state->_M_register_callback(&_M_cb))
            _M_state.swap(__state);
    }

    ~stop_callback() {
        if (_M_state) {
            _M_state->_M_remove_callback(&_M_cb);
        }
    }

    stop_callback(const stop_callback &)            = delete;
    stop_callback &operator=(const stop_callback &) = delete;
    stop_callback(stop_callback &&)                 = delete;
    stop_callback &operator=(stop_callback &&)      = delete;

private:
    struct _Cb_impl : stop_token::_Stop_cb {
        template <typename _Cb> explicit _Cb_impl(_Cb &&__cb) : _Stop_cb(&_S_execute), _M_cb(std::forward<_Cb>(__cb)) {}

        _Callback _M_cb;

        static void _S_execute(_Stop_cb *__that) noexcept {
            _Callback &__cb = static_cast<_Cb_impl *>(__that)->_M_cb;
            std::forward<_Callback>(__cb)();
        }
    };

    _Cb_impl                    _M_cb;
    stop_token::_Stop_state_ref _M_state;
};

template <typename _Callback> stop_callback(stop_token, _Callback) -> stop_callback<_Callback>;

template <typename _Callable, typename... _Args> constexpr bool __pmf_expects_stop_token = false;

template <typename _Callable, typename _Obj, typename... _Args>
constexpr bool __pmf_expects_stop_token<_Callable, _Obj, _Args...> =
    std::conjunction<std::is_member_function_pointer<std::remove_reference_t<_Callable>>,
                     std::is_invocable<_Callable, _Obj, stop_token, _Args...>>::value;

template <typename T> struct remove_cvref : std::remove_cv<std::remove_reference_t<T>> {};
template <typename T> using remove_cvref_t = typename remove_cvref<T>::type;

class jthread {
    stop_source _M_stop_source;
    std::thread _M_thread;

    template <typename _Callable, typename... _Args>
    static std::thread _S_create(stop_source &__ssrc, _Callable &&__f, _Args &&...__args) {
        if constexpr (__pmf_expects_stop_token<_Callable, _Args...>)
            return _S_create_pmf(__ssrc, __f, std::forward<_Args>(__args)...);
        else

            if constexpr (std::is_invocable_v<std::decay_t<_Callable>, stop_token, std::decay_t<_Args>...>)
            return std::thread{std::forward<_Callable>(__f), __ssrc.get_token(), std::forward<_Args>(__args)...};
        else {
            static_assert(std::is_invocable_v<std::decay_t<_Callable>, std::decay_t<_Args>...>,
                          "jthread_cxx17::jthread arguments must be invocable after"
                          " conversion to rvalues");
            return std::thread{std::forward<_Callable>(__f), std::forward<_Args>(__args)...};
        }
    }

    template <typename _Callable, typename _Obj, typename... _Args>
    static std::thread _S_create_pmf(stop_source &__ssrc, _Callable __f, _Obj &&__obj, _Args &&...__args) {
        return std::thread{__f, std::forward<_Obj>(__obj), __ssrc.get_token(), std::forward<_Args>(__args)...};
    }

public:
    using id                 = std::thread::id;
    using native_handle_type = std::thread::native_handle_type;

    jthread() noexcept : _M_stop_source{nostopstate} {}
    jthread(jthread &&other) noexcept = default;

    template <typename _Callable, typename... _Args,
              typename = std::enable_if_t<!std::is_same_v<remove_cvref_t<_Callable>, jthread>>>
    explicit jthread(_Callable &&__f, _Args &&...__args)
        : _M_thread{_S_create(_M_stop_source, std::forward<_Callable>(__f), std::forward<_Args>(__args)...)} {}

    jthread(const jthread &) = delete;
    ~jthread() {
        if (joinable()) {
            request_stop();
            join();
        }
    }
    jthread &operator=(jthread &&other) noexcept {
        jthread(std::move(other)).swap(*this);
        return *this;
    }

    [[nodiscard]] bool joinable() const noexcept { return _M_thread.joinable(); }

    [[nodiscard]] id                  get_id() const noexcept { return _M_thread.get_id(); }
    [[nodiscard]] native_handle_type  native_handle() { return _M_thread.native_handle(); }
    [[nodiscard]] static unsigned int hardware_concurrency() noexcept { return std::thread::hardware_concurrency(); }

    void join() { _M_thread.join(); }
    void detach() { _M_thread.detach(); }
    void swap(jthread &other) noexcept {
        std::swap(_M_thread, other._M_thread);
        std::swap(_M_stop_source, other._M_stop_source);
    }

    [[nodiscard]] stop_source get_stop_source() noexcept { return _M_stop_source; }
    [[nodiscard]] stop_token  get_stop_token() const noexcept { return _M_stop_source.get_token(); }
    bool                      request_stop() noexcept { return _M_stop_source.request_stop(); }

    friend void swap(jthread &lhs, jthread &rhs) noexcept { lhs.swap(rhs); }
};

class condition_variable_any {
    using __clock_t = std::chrono::steady_clock;
    std::condition_variable _M_cond;
    std::mutex              _M_mutex; // 直接使用成员变量

    template <typename _Lock> struct _Unlock {
        explicit _Unlock(_Lock &__lk) : _M_lock(__lk) { __lk.unlock(); }
        ~_Unlock() noexcept(false) {
            if (std::uncaught_exceptions()) {
                try {
                    _M_lock.lock();
                } catch (...) {
                }
            } else
                _M_lock.lock();
        }

        _Unlock(const _Unlock &)            = delete;
        _Unlock &operator=(const _Unlock &) = delete;

        _Lock &_M_lock;
    };

public:
    condition_variable_any() : _M_mutex() {}
    ~condition_variable_any() = default;

    condition_variable_any(const condition_variable_any &)            = delete;
    condition_variable_any &operator=(const condition_variable_any &) = delete;

    void notify_one() noexcept {
        std::lock_guard<std::mutex> __lock(_M_mutex);
        _M_cond.notify_one();
    }

    void notify_all() noexcept {
        std::lock_guard<std::mutex> __lock(_M_mutex);
        _M_cond.notify_all();
    }

    template <typename _Lock> void wait(_Lock &__lock) {
        std::mutex                  &__mutex = _M_mutex;
        std::unique_lock<std::mutex> __my_lock(__mutex);
        _Unlock<_Lock>               __unlock(__lock);
        // *__mutex must be unlocked before re-locking __lock so move
        // ownership of *__mutex lock to an object with shorter lifetime.
        std::unique_lock<std::mutex> __my_lock2(std::move(__my_lock));
        _M_cond.wait(__my_lock2);
    }

    template <typename _Lock, typename _Predicate> void wait(_Lock &__lock, _Predicate __p) {
        while (!__p())
            wait(__lock);
    }

    template <typename _Lock, typename _Clock, typename _Duration>
    std::cv_status wait_until(_Lock &__lock, const std::chrono::time_point<_Clock, _Duration> &__atime) {
        std::mutex                  &__mutex = _M_mutex;
        std::unique_lock<std::mutex> __my_lock(__mutex);
        _Unlock<_Lock>               __unlock(__lock);
        // *__mutex must be unlocked before re-locking __lock so move
        // ownership of *__mutex lock to an object with shorter lifetime.
        std::unique_lock<std::mutex> __my_lock2(std::move(__my_lock));
        return _M_cond.wait_until(__my_lock2, __atime);
    }

    template <typename _Lock, typename _Clock, typename _Duration, typename _Predicate>
    bool wait_until(_Lock &__lock, const std::chrono::time_point<_Clock, _Duration> &__atime, _Predicate __p) {
        while (!__p())
            if (wait_until(__lock, __atime) == std::cv_status::timeout)
                return __p();
        return true;
    }

    template <typename _Lock, typename _Rep, typename _Period>
    std::cv_status wait_for(_Lock &__lock, const std::chrono::duration<_Rep, _Period> &__rtime) {
        return wait_until(__lock, __clock_t::now() + __rtime);
    }

    template <typename _Lock, typename _Rep, typename _Period, typename _Predicate>
    bool wait_for(_Lock &__lock, const std::chrono::duration<_Rep, _Period> &__rtime, _Predicate __p) {
        return wait_until(__lock, __clock_t::now() + __rtime, std::move(__p));
    }

    template <class _Lock, class _Predicate> bool wait(_Lock &__lock, stop_token __stoken, _Predicate __p) {
        if (__stoken.stop_requested()) {
            return __p();
        }

        stop_callback __cb(__stoken, [this] { notify_all(); });
        std::mutex   &__mutex = _M_mutex;
        while (!__p()) {
            std::unique_lock<std::mutex> __my_lock(__mutex);
            if (__stoken.stop_requested()) {
                return false;
            }
            // *__mutex must be unlocked before re-locking __lock so move
            // ownership of *__mutex lock to an object with shorter lifetime.
            _Unlock<_Lock>               __unlock(__lock);
            std::unique_lock<std::mutex> __my_lock2(std::move(__my_lock));
            _M_cond.wait(__my_lock2);
        }
        return true;
    }

    template <class _Lock, class _Clock, class _Duration, class _Predicate>
    bool wait_until(_Lock &__lock, stop_token __stoken, const std::chrono::time_point<_Clock, _Duration> &__abs_time,
                    _Predicate __p) {
        if (__stoken.stop_requested()) {
            return __p();
        }

        stop_callback __cb(__stoken, [this] { notify_all(); });
        std::mutex   &__mutex = _M_mutex;
        while (!__p()) {
            bool __stop;
            {
                std::unique_lock<std::mutex> __my_lock(__mutex);
                if (__stoken.stop_requested()) {
                    return false;
                }
                _Unlock<_Lock>               __u(__lock);
                std::unique_lock<std::mutex> __my_lock2(std::move(__my_lock));
                const auto                   __status = _M_cond.wait_until(__my_lock2, __abs_time);
                __stop = (__status == std::cv_status::timeout) || __stoken.stop_requested();
            }
            if (__stop) {
                return __p();
            }
        }
        return true;
    }

    template <class _Lock, class _Rep, class _Period, class _Predicate>
    bool wait_for(_Lock &__lock, stop_token __stoken, const std::chrono::duration<_Rep, _Period> &__rel_time,
                  _Predicate __p) {
        auto __abst = std::chrono::steady_clock::now() + __rel_time;
        return wait_until(__lock, std::move(__stoken), __abst, std::move(__p));
    }
};

} // namespace jthread_cxx17
#endif // JTHREAD_CXX17_H_