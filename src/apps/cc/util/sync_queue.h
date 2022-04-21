#ifndef SCDETECT_APPS_CC_UTIL_SYNCQUEUE_H_
#define SCDETECT_APPS_CC_UTIL_SYNCQUEUE_H_

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <list>
#include <mutex>

namespace Seiscomp {
namespace detect {
namespace util {

// A queue providing `put()` and `get()` without data races
template <typename T>
class SyncQueue {
 public:
  // Enqueue `val`
  void put(T val);
  // Enqueue `val` only if the queue size is below `n`
  void put(T val, std::size_t n);
  // Enqueue `val` only if the queue size is below `n` within `d`
  //
  // - returns whether `val` was enqueued (`true`) or whether not `false`
  bool put(T val, std::size_t n, std::chrono::steady_clock::duration d);
  // Consume `val` from the queue
  //
  // - the caller will remain blocked until the queue is non-empty
  void get(T& val);
  // Consume `val` from the queue
  //
  // - the caller will remain blocked for duration `d`
  // - returns `true` if a value was available, else `false`
  bool get(T& val, std::chrono::steady_clock::duration d);

 private:
  std::mutex _mtx;
  std::condition_variable _cond;
  std::list<T> _queue;
};

template <typename T>
void SyncQueue<T>::put(T val) {
  std::lock_guard<std::mutex> lock{_mtx};
  _queue.push_back(std::move(val));
  _cond.notify_one();
}

template <typename T>
void SyncQueue<T>::put(T val, std::size_t n) {
  std::unique_lock<std::mutex> lock{_mtx};
  _cond.wait(lock, [this, n]() { return _queue.size() < n; });
  _queue.push_back(std::move(val));
  _cond.notify_one();
}

template <typename T>
bool SyncQueue<T>::put(T val, std::size_t n,
                       std::chrono::steady_clock::duration d) {
  std::unique_lock<std::mutex> lock{_mtx};
  bool notFull{
      _cond.wait_for(lock, d, [this, n]() { return _queue.size() < n; })};
  if (notFull) {
    _queue.push_back(std::move(val));
    _cond.notify_one();
  } else {
    _cond.notify_all();
  }
  return notFull;
}

template <typename T>
void SyncQueue<T>::get(T& val) {
  std::unique_lock<std::mutex> lock{_mtx};
  _cond.wait(lock, [this]() { return !_queue.empty(); });
  val = _queue.front();
  _queue.pop_front();
}

template <typename T>
bool SyncQueue<T>::get(T& val, std::chrono::steady_clock::duration d) {
  std::unique_lock<std::mutex> lock{_mtx};
  bool notEmpty{_cond.wait_for(lock, d, [this]() { return !_queue.empty(); })};
  if (notEmpty) {
    val = _queue.front();
    _queue.pop_front();
    return true;
  }
  return false;
}

}  // namespace util
}  // namespace detect
}  // namespace Seiscomp

#endif  // SCDETECT_APPS_CC_UTIL_SYNCQUEUE_H_
