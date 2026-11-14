// A few worker threads that run the tasks of one block side by side. A task can wait for others (the instrument that
// hears another through a sidechain waits for it); the calling thread works too, and run() returns when every task is done.
// The tasks of a block are independent apart from those waits, so what they compute does not depend on which thread
// ran them or in what order.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace wax {

class TaskPool {
public:
  // `threads` counts the calling thread: 1 starts no workers (run() then does everything itself, in task order).
  explicit TaskPool(int threads);
  ~TaskPool();
  TaskPool(const TaskPool&) = delete;
  TaskPool& operator=(const TaskPool&) = delete;

  int threads() const { return static_cast<int>(workers_.size()) + 1; }

  // Runs body(i) once for each task i in [0, depCount.size()). Task i starts when the depCount[i] tasks it waits for are
  // done; dependents[t] lists the tasks that wait for t. An exception thrown by a task is rethrown here once all are done.
  void run(const std::vector<int>& depCount, const std::vector<std::vector<int>>& dependents, const std::function<void(int)>& body);

private:
  void workerLoop();
  void drain(std::unique_lock<std::mutex>& lock);

  std::vector<std::thread> workers_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool stop_ = false;
  std::uint64_t job_ = 0;

  std::vector<int> ready_, remaining_;
  const std::vector<std::vector<int>>* dependents_ = nullptr;
  const std::function<void(int)>* body_ = nullptr;
  int count_ = 0, done_ = 0;
  std::exception_ptr error_;
};

}  // namespace wax
