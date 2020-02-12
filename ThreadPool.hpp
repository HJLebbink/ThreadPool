#pragma once
#include <functional>
#include <future>
#include <queue>
#include <memory>
#include <functional>
#include <type_traits>


namespace {
    // workaround for https://developercommunity.visualstudio.com/content/problem/108672/unable-to-move-stdpackaged-task-into-any-stl-conta.html
    // and: https://developercommunity.visualstudio.com/content/problem/324418/c-stdpackaged-task-does-not-work-with-non-copyable.html
#if defined(_MSC_VER) && _MSC_VER < 2000
#define WORKAROUND_PTR_T(T) std::shared_ptr<T>
#define WORKAROUND_PTR_GET(x) (*(x))
#define WORKAROUND_MAKE_PTR(T) std::make_shared<T>

    template<typename MoveOnlyCallable>
    class Copyable_Bind
    {
    public:
        template<typename F, typename... Args>
        Copyable_Bind(F&& f, Args&&... args) :
            c(std::make_shared<MoveOnlyCallable>(std::bind(std::forward<F>(f), std::forward<Args>(args)...)))
        {}

        std::invoke_result_t<MoveOnlyCallable> operator()()
        {
            return (*c)();
        }
    private:
        std::shared_ptr<MoveOnlyCallable> c;
    };
    template<typename F, typename... Args>
    Copyable_Bind(F&& f, Args&&... args)->Copyable_Bind<decltype(std::bind(std::forward<F>(f), std::forward<Args>(args)...)) >;

#define WPRKAROUND_BIND(...) Copyable_Bind(__VA_ARGS__)
#else
#define WORKAROUND_PTR_T(T) T
#define WORKAROUND_PTR_GET(x) (x)
#define WORKAROUND_MAKE_PTR(T) T
#define WPRKAROUND_BIND(...) std::bind(__VA_ARGS__)
#endif
}

class ThreadPool {
public:
    // the constructor launches some workers
    inline explicit ThreadPool(int threads)
        : stop_(false)
    {
        for (int i = 0; i < threads; ++i)
            workers_.emplace_back(
                [this]
                {
                    for (;;)
                    {
                        decltype(tasks_)::value_type task;

                        {
                            std::unique_lock<std::mutex> lock(this->queue_mutex_);
                            this->condition_.wait(lock,
                                [this] { return this->stop_ || !this->tasks_.empty(); });
                            if (this->stop_ && this->tasks_.empty())
                                return;
                            task = std::move(this->tasks_.front());
                            this->tasks_.pop();
                        }

                        task();
                    }
                }
                );
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    // add new work item to the pool
    template<class F, class... Args>
    auto enqueue(F&& f, Args&&... args)
    {
        using return_type = std::invoke_result_t<F, Args...>;

        auto task = WORKAROUND_MAKE_PTR(std::packaged_task<return_type()>)(
            WPRKAROUND_BIND(std::forward<F>(f), std::forward<Args>(args)...)
        );

        std::future<return_type> res = WORKAROUND_PTR_GET(task).get_future();
        {
            std::unique_lock<std::mutex> lock(this->queue_mutex_);

            // don't allow enqueueing after stopping the pool
            if (this->stop_) {
                throw std::runtime_error("enqueue on stopped ThreadPool");
            }
            tasks_.emplace([task = std::move(task)]() mutable { WORKAROUND_PTR_GET(task)(); });
        }
        condition_.notify_one();
        return res;
    }

    // the destructor joins all threads
    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(this->queue_mutex_);
            this->stop_ = true;
        }
        condition_.notify_all();
        for (std::thread& worker : workers_) {
            worker.join();
        }
    }

private:
    // need to keep track of threads so we can join them
    std::vector<std::thread> workers_;
    // the task queue
    std::queue<std::function<void()>> tasks_;

    // synchronization
    std::mutex queue_mutex_;
    std::condition_variable condition_;
    bool stop_;
};