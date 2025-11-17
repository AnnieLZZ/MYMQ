#ifndef PRINTQUEUE_H
#define PRINTQUEUE_H

#include <string>
#include <cerrno>
#include <deque>
#include <mutex>
#include <memory>
#include <condition_variable>
#include <iostream>
#include <thread>
#include <atomic>

class Printqueue {
public:

    Printqueue& operator=(const Printqueue&) = delete;
    Printqueue(const Printqueue&) = delete;

public:
    static Printqueue& instance() {
        static Printqueue ins;
        return ins;
    }

    Printqueue() : running_(true) {
        consumer_thread_ = std::thread(&Printqueue::consumer_loop, this);
    }


    ~Printqueue() {
        stop();
        if (consumer_thread_.joinable()) {
            consumer_thread_.join();
        }
    }

    void out(const std::string& str,bool is_cerr=0, bool perior = 0) {
        std::unique_lock<std::mutex> ulock(mtx);
        if (perior) {
            str_queue.emplace_front(std::pair<std::string,bool>(str,is_cerr));
        } else {
            str_queue.emplace_back(std::pair<std::string,bool>(str,is_cerr));
        }
        cv.notify_one();
    }


    void stop() {
        std::unique_lock<std::mutex> ulock(mtx);
        running_ = false;
        cv.notify_all();
    }

private:

    void consumer_loop() {
        while (running_) {
            std::pair<std::string,bool> print_pair;
            {
                std::unique_lock<std::mutex> ulock(mtx);

                cv.wait(ulock, [this] {
                    return !str_queue.empty() || !running_;
                });

                if (!running_ && str_queue.empty()) {
                    break;
                }

                if (!str_queue.empty()) {
                    print_pair= str_queue.front();
                    str_queue.pop_front();
                }
            }

            if (!print_pair.first.empty()) {
                if(print_pair.second){
                    std::cerr << print_pair.first<<std::endl;
                }
                else{
                    std::cout <<print_pair.first<<std::endl;
                }

            }
        }
    }

    std::deque<std::pair<std::string,bool>> str_queue;
    std::mutex mtx;
    std::condition_variable cv;
    std::thread consumer_thread_;
    std::atomic<bool> running_;
};

#endif // PRINTQUEUE_H
