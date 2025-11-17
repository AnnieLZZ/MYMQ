#ifndef THREADPOOL_H
#define THREADPOOL_H
#include<queue>
#include<vector>
#include<thread>
#include<future>
#include <condition_variable>
#include <mutex>
#include <functional>
using Task =std::packaged_task<void()>;
class ThreadPool{
    ThreadPool& operator= (const ThreadPool&)=delete;
    ThreadPool(const ThreadPool&)=delete;

public:
    ~ThreadPool(){
        stop();
    }

    static ThreadPool& instance(unsigned int thread_num=(std::thread::hardware_concurrency())){//单例
        static ThreadPool ins(thread_num);
        return ins;
    }

    void start(){
        for(int i=0;i<left_thread_num;i++){
            thread_pool.emplace_back([this]{
                while (true) {
                    Task task;
                    {
                        std::unique_lock<std::mutex> ulock(mtx_operator_task_queue);
                        cv_awake_thread.wait(ulock, [this] {
                            return b_stop.load() || !task_queue.empty();
                        });
                        //如果线程池已经停止了且没有任务就退出
                        if (b_stop.load() && task_queue.empty()) {
                            return;
                        }
                        //如果线程池已经停止了但是还有任务就跑完任务
                        if (!task_queue.empty()) {
                            task = std::move(task_queue.front());
                            task_queue.pop();
                        }
                    }

                    task();



                }
            });
        }
    }

    void stop(){
        {
            std::lock_guard<std::mutex> lockg(mtx_operator_task_queue);
            b_stop.store(1);
            cv_awake_thread.notify_all();
        }


        for(auto& threadit: thread_pool){
            if(threadit.joinable()){
                threadit.join();
            }
        }
    }

    template<class F,class ... Args>
    auto commit(F&& f,Args&&... args)->
        std::future<decltype(std::forward<F>(f)(std::forward<Args>(args)...))>{
        using Rettype=decltype(std::forward<F>(f)(std::forward<Args>(args)...));
        if(b_stop.load()){
            return std::future<Rettype> {};
        }
        auto task = std::make_shared<std::packaged_task<Rettype()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
            );

        std::future<Rettype> ret=task->get_future();
        {
            std::lock_guard<std::mutex> lockg(mtx_operator_task_queue) ;
            task_queue.emplace([task]{
                (*task)();
            });
            cv_awake_thread.notify_one();
        }


        return ret;
    }

private:
    ThreadPool(unsigned int thread_num=(std::thread::hardware_concurrency())):b_stop(0){
        if(thread_num<=1){
            left_thread_num=2;
        }
        else if(thread_num>std::thread::hardware_concurrency()){
            left_thread_num=std::thread::hardware_concurrency();
        }
        else{
            left_thread_num=thread_num;
        }
        start();
    }

private:
    std::vector<std::thread> thread_pool;
    std::queue<Task> task_queue;
    std::atomic<bool> b_stop;
    std::atomic<int> left_thread_num;
    std::mutex mtx_operator_task_queue;

    std::condition_variable cv_awake_thread;

};

#endif // THREADPOOL_H
