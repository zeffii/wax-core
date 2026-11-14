#include "engine/task_pool.h"

namespace wax {

TaskPool::TaskPool(int threads) {
  for (int i = 1; i < threads; i++) workers_.emplace_back([this] { workerLoop(); });
}

TaskPool::~TaskPool() {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  cv_.notify_all();
  for (std::thread& t : workers_) t.join();
}

void TaskPool::workerLoop() {
  std::uint64_t seen = 0;
  std::unique_lock<std::mutex> lock(mutex_);
  for (;;) {
    cv_.wait(lock, [&] { return stop_ || job_ != seen; });
    if (stop_) return;
    seen = job_;
    drain(lock);
  }
}

// Takes ready tasks and runs them until the job is done. Called with the lock held.
void TaskPool::drain(std::unique_lock<std::mutex>& lock) {
  while (done_ < count_) {
    if (ready_.empty()) {
      cv_.wait(lock);
      continue;
    }
    const int task = ready_.back();
    ready_.pop_back();
    lock.unlock();
    std::exception_ptr failed;
    try {
      (*body_)(task);
    } catch (...) {
      failed = std::current_exception();
    }
    lock.lock();
    if (failed && !error_) error_ = failed;
    done_++;
    for (const int d : (*dependents_)[static_cast<std::size_t>(task)]) if (--remaining_[static_cast<std::size_t>(d)] == 0) ready_.push_back(d);
    cv_.notify_all();
  }
}

void TaskPool::run(const std::vector<int>& depCount, const std::vector<std::vector<int>>& dependents, const std::function<void(int)>& body) {
  const int count = static_cast<int>(depCount.size());
  if (count == 0) return;
  if (workers_.empty()) {   // one thread: the tasks in order (the order of a plan has every task after what it waits for)
    for (int i = 0; i < count; i++) body(i);
    return;
  }
  std::unique_lock<std::mutex> lock(mutex_);
  remaining_ = depCount;
  ready_.clear();
  for (int i = count - 1; i >= 0; i--) if (depCount[static_cast<std::size_t>(i)] == 0) ready_.push_back(i);
  dependents_ = &dependents;
  body_ = &body;
  count_ = count;
  done_ = 0;
  error_ = nullptr;
  job_++;
  cv_.notify_all();
  drain(lock);
  const std::exception_ptr failed = error_;
  lock.unlock();
  if (failed) std::rethrow_exception(failed);
}

}  // namespace wax
