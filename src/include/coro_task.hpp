#pragma once
// Infrastructure coroutine partagée par les versions de table.
//  - CoroutinePool : pool d'arène thread-local pour les frames de coroutine
//  - Task<T>       : coroutine "lazy" (lancée suspendue, pilotée par resume())

#include <coroutine>
#include <cstddef>
#include <new>

// ---------------------------------------------------------
// Pool d'arène pour les frames de coroutine
// ---------------------------------------------------------
// Le coût qui plafonnait l'async (Run 1) était l'allocation tas par requête.
// Ce pool recycle les frames via une free-list thread-local : après le
// préchauffage, allocate()/deallocate() ne font qu'empiler/dépiler un pointeur
// (zéro appel à malloc). Toutes les coroutines ayant la même taille de frame,
// une seule classe de taille suffit ; tout le reste retombe sur l'allocateur global.
class CoroutinePool {
    static inline thread_local void*       free_head_ = nullptr; // free-list
    static inline thread_local std::size_t block_size_ = 0;      // classe de taille
public:
    static void* allocate(std::size_t size) {
        if (size == block_size_ && free_head_) {       // chemin chaud : pop O(1)
            void* p = free_head_;
            free_head_ = *reinterpret_cast<void**>(p);
            return p;
        }
        if (block_size_ == 0) block_size_ = size;       // fixe la classe au 1er appel
        return ::operator new(size);                    // pas de bloc libre -> neuf
    }
    static void deallocate(void* ptr, std::size_t size) noexcept {
        if (size == block_size_) {                      // chemin chaud : push O(1)
            *reinterpret_cast<void**>(ptr) = free_head_;
            free_head_ = ptr;
        } else {
            ::operator delete(ptr);
        }
    }
};

// ---------------------------------------------------------
// Coroutine Task<T>
// ---------------------------------------------------------
template<typename T>
struct Task {
    struct promise_type;
    using handle_type = std::coroutine_handle<promise_type>;

    struct promise_type {
        T value;
        Task get_return_object() { return Task{handle_type::from_promise(*this)}; }
        std::suspend_always initial_suspend() { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_value(T v) { value = v; }
        void unhandled_exception() { std::terminate(); }

        // Frame alloué dans le pool d'arène (cf. CoroutinePool ci-dessus)
        static void* operator new(std::size_t size) { return CoroutinePool::allocate(size); }
        static void  operator delete(void* ptr, std::size_t size) noexcept { CoroutinePool::deallocate(ptr, size); }
    };

    handle_type coro;
    Task(handle_type h) : coro(h) {}
    ~Task() { if (coro) coro.destroy(); }

    // Pas de copie (sinon double destroy du handle), uniquement le déplacement
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    Task(Task&& other) noexcept : coro(other.coro) { other.coro = nullptr; }
    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            if (coro) coro.destroy();
            coro = other.coro;
            other.coro = nullptr;
        }
        return *this;
    }

    bool resume() {
        if (!coro.done()) coro.resume();
        return !coro.done();
    }
    T get_result() { return coro.promise().value; }
};
