#include "jthread_cxx17.h"
#include <iostream>
#include <sstream>
using namespace std::chrono_literals;
using namespace jthread_cxx17;

void f(stop_token stop_token, int value) {
    while (!stop_token.stop_requested()) {
        std::cout << value++ << ' ' << std::flush;
        std::this_thread::sleep_for(200ms);
    }
    std::cout << std::endl;
}

void stop_token_test() {
    jthread thread(f, 5); // prints 5 6 7 8... for approximately 3 seconds
    std::this_thread::sleep_for(3s);
    // The destructor of jthread calls request_stop() and join().
}

void stop_token_stop_requested_test() {
    std::cout << std::boolalpha;
    auto print = [](std::string_view name, const stop_token &token) {
        std::cout << name << ": stop_possible = " << token.stop_possible();
        std::cout << ", stop_requested = " << token.stop_requested() << '\n';
    };

    // A worker thread that will listen to stop requests
    auto stop_worker = jthread([](stop_token stoken) {
        for (int i = 10; i; --i) {
            std::this_thread::sleep_for(300ms);
            if (stoken.stop_requested()) {
                std::cout << "  Sleepy worker is requested to stop\n";
                return;
            }
            std::cout << "  Sleepy worker goes back to sleep\n";
        }
    });

    // A worker thread that will only stop when completed
    auto inf_worker = jthread([]() {
        for (int i = 5; i; --i) {
            std::this_thread::sleep_for(300ms);
            std::cout << "  Run as long as we want\n";
        }
    });

    stop_token                def_token;
    stop_token                stop_token = stop_worker.get_stop_token();
    jthread_cxx17::stop_token inf_token  = inf_worker.get_stop_token();
    print("def_token ", def_token);
    print("stop_token", stop_token);
    print("inf_token ", inf_token);

    std::cout << "\nRequest and join stop_worker:\n";
    stop_worker.request_stop();
    stop_worker.join();

    std::cout << "\nRequest and join inf_worker:\n";
    inf_worker.request_stop();
    inf_worker.join();
    std::cout << '\n';

    print("def_token ", def_token);
    print("stop_token", stop_token);
    print("inf_token ", inf_token);
}

void stop_token_stop_possible_test() {
    std::cout << std::boolalpha;
    auto print = [](std::string_view name, const stop_token &token) {
        printf("%s: stop_possible = %s, stop_requested = %s\n", name.data(), token.stop_possible() ? "true" : "false",
               token.stop_requested() ? "true" : "false");
    };

    // A worker thread that will listen to stop requests
    auto stop_worker = jthread([](stop_token stoken) {
        for (int i = 10; i; --i) {
            std::this_thread::sleep_for(300ms);
            if (stoken.stop_requested()) {
                std::cout << "  Sleepy worker is requested to stop\n";
                return;
            }
            std::cout << "  Sleepy worker goes back to sleep\n";
        }
    });

    // A worker thread that will only stop when completed
    auto inf_worker = jthread([]() {
        for (int i = 5; i; --i) {
            std::this_thread::sleep_for(300ms);
            std::cout << "  Run as long as we want\n";
        }
    });

    stop_token def_token;
    stop_token stp_token = stop_worker.get_stop_token();
    stop_token inf_token = inf_worker.get_stop_token();
    print("def_token ", def_token);
    print("stop_token", stp_token);
    print("inf_token ", inf_token);

    std::cout << "\nRequest and join stop_worker:\n";
    stop_worker.request_stop();
    stop_worker.join();

    std::cout << "\nRequest and join inf_worker:\n";
    inf_worker.request_stop();
    inf_worker.join();
    std::cout << '\n';

    print("def_token ", def_token);
    print("stop_token", stp_token);
    print("inf_token ", inf_token);
}

void worker_fun(int id, stop_token stoken) {
    for (int i = 10; i; --i) {
        std::this_thread::sleep_for(300ms);
        if (stoken.stop_requested()) {
            std::printf("  worker%d is requested to stop\n", id);
            return;
        }
        std::printf("  worker%d goes back to sleep\n", id);
    }
}

void stop_source_test() {
    jthread threads[4];
    std::cout << std::boolalpha;
    auto print = [](const stop_source &source) {
        std::printf("stop_source stop_possible = %s, stop_requested = %s\n", source.stop_possible() ? "true" : "false",
                    source.stop_requested() ? "true" : "false");
    };

    // Common source
    stop_source stop_source;

    print(stop_source);

    // Create worker threads
    for (int i = 0; i < 4; ++i)
        threads[i] = jthread(worker_fun, i + 1, stop_source.get_token());

    std::this_thread::sleep_for(500ms);

    std::puts("Request stop");
    stop_source.request_stop();

    print(stop_source);

    // Note: destructor of jthreads will call join so no need for explicit calls
}

void stop_callback_test() {
    // A worker thread.
    // It will wait until it is requested to stop.
    jthread worker([](stop_token stoken) {
        std::cout << "Worker thread's id: " << std::this_thread::get_id() << '\n';
        std::mutex       mutex;
        std::unique_lock lock(mutex);
        condition_variable_any().wait(lock, stoken, [&stoken] { return stoken.stop_requested(); });
    });

    // Register a stop callback on the worker thread.
    stop_callback callback(worker.get_stop_token(), [] {
        std::cout << "Stop callback executed by thread: " << std::this_thread::get_id() << '\n';
    });

    // Stop_callback objects can be destroyed prematurely to prevent execution.
    {
        stop_callback scoped_callback(worker.get_stop_token(), [] {
            // This will not be executed.
            std::cout << "Scoped stop callback executed by thread: " << std::this_thread::get_id() << '\n';
        });
    }

    // Demonstrate which thread executes the stop_callback and when.
    // Define a stopper function.
    auto stopper_func = [&worker] {
        if (worker.request_stop())
            std::cout << "Stop request executed by thread: " << std::this_thread::get_id() << '\n';
        else
            std::cout << "Stop request not executed by thread: " << std::this_thread::get_id() << '\n';
    };

    // Let multiple threads compete for stopping the worker thread.
    jthread stopper1(stopper_func);
    jthread stopper2(stopper_func);
    stopper1.join();
    stopper2.join();

    // After a stop has already been requested,
    // a new stop_callback executes immediately.
    std::cout << "Main thread: " << std::this_thread::get_id() << '\n';
    stop_callback callback_after_stop(worker.get_stop_token(), [] {
        std::cout << "Stop callback executed by thread: " << std::this_thread::get_id() << '\n';
    });

    std::puts("Exit");
}

// Helper function to quickly show which thread printed what
void print(const char *txt) { std::cout << std::this_thread::get_id() << ' ' << txt; }
void jthread_request_stop_test() {
    // A sleepy worker thread
    jthread sleepy_worker([](stop_token stoken) {
        for (int i = 10; i; --i) {
            std::this_thread::sleep_for(300ms);
            if (stoken.stop_requested()) {
                print("Sleepy worker is requested to stop\n");
                return;
            }
            print("Sleepy worker goes back to sleep\n");
        }
    });

    // A waiting worker thread
    // The condition variable will be awoken by the stop request.
    jthread waiting_worker([](stop_token stoken) {
        std::mutex       mutex;
        std::unique_lock lock(mutex);
        condition_variable_any().wait(lock, stoken, [] { return false; });
        print("Waiting worker is requested to stop\n");
        return;
    });

    // Sleep this thread to give threads time to spin
    std::this_thread::sleep_for(400ms);

    // std::jthread::request_stop() can be called explicitly:
    print("Requesting stop of sleepy worker\n");
    sleepy_worker.request_stop();
    sleepy_worker.join();
    print("Sleepy worker joined\n");

    // Or automatically using RAII:
    // waiting_worker's destructor will call request_stop()
    // and join the thread automatically.
}

int main() {
    std::cout << std::endl << "=====> stop_token_test: " << std::endl;
    stop_token_test();
    std::cout << std::endl << "=====> stop_token_stop_requested_test: " << std::endl;
    stop_token_stop_requested_test();
    std::cout << std::endl << "=====> stop_token_stop_possible_test: " << std::endl;
    stop_token_stop_possible_test();
    std::cout << std::endl << "=====> stop_source_test: " << std::endl;
    stop_source_test();
    std::cout << std::endl << "=====> stop_callback_test: " << std::endl;
    stop_callback_test();
    std::cout << std::endl << "=====> jthread_request_stop_test: " << std::endl;
    jthread_request_stop_test();

    return 0;
}
