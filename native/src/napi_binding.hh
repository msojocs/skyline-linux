#pragma once

// N-API plumbing for skyline.node: table-driven bindings with uniform argument
// decoding and error conversion, and the threadsafe sinks that carry events
// from engine threads to the JS thread.

#include <node_api.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace skyline::napi {

// Thrown by binding handlers and helpers; the dispatcher turns it into a JS
// Error with the same message.
class Error : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// Throws Error("<what> failed") unless `status` is napi_ok.
void Check(napi_status status, const char* what);

// The arguments of one call.  Decoding is lenient in the way the bootstrap
// relies on: a missing or non-number argument reads as the fallback, and
// booleans are not coerced from other types.
class Args {
 public:
  static constexpr size_t kCapacity = 9;

  Args(napi_env env, napi_callback_info info, void** data);

  napi_env env() const { return env_; }
  // Number of arguments the caller passed (may exceed kCapacity).
  size_t size() const { return count_; }
  // nullptr beyond kCapacity; `undefined` for missing arguments below it.
  napi_value operator[](size_t index) const {
    return index < kCapacity ? values_[index] : nullptr;
  }
  bool Is(size_t index, napi_valuetype type) const;
  double Number(size_t index, double fallback = 0.0) const;
  uint32_t U32(size_t index) const {
    return static_cast<uint32_t>(Number(index));
  }
  int32_t I32(size_t index) const {
    return static_cast<int32_t>(Number(index));
  }
  bool Bool(size_t index, bool fallback = false) const;
  // Empty unless the argument is a string.
  std::string String(size_t index) const;
  // Backing memory of an ArrayBuffer or typed-array argument; valid while the
  // caller keeps the JS object alive.
  bool Buffer(size_t index, void** data, size_t* byte_length) const;

 private:
  napi_env env_;
  size_t count_ = 0;
  napi_value values_[kCapacity] = {};
};

// Backing memory of an ArrayBuffer or typed-array view.
bool ReadBufferView(napi_env env, napi_value value, void** data,
                    size_t* byte_length);

napi_value Undefined(napi_env env);
napi_value NewObject(napi_env env);
void SetNumber(napi_env env, napi_value object, const char* key, double value);
void SetBool(napi_env env, napi_value object, const char* key, bool value);
void SetString(napi_env env, napi_value object, const char* key,
               const char* value);
std::string ToString(napi_env env, napi_value value);

// One `native.<name>` method.  A call with fewer than `min_args` arguments
// throws `usage`, or returns undefined when `usage` is null.  `handler`
// returns nullptr for undefined; exceptions become JS errors.
struct Binding {
  const char* name;
  napi_value (*handler)(const Args& args);
  size_t min_args = 0;
  const char* usage = nullptr;
};

// Defines `bindings` as functions on `target`.  The table must outlive the
// functions (use static storage).
void DefineBindings(napi_env env, napi_value target, const Binding* bindings,
                    size_t count);

// A JS callback fed from engine threads through a threadsafe function.
//
// Post() may run on any thread; Install() runs on the JS thread and Close() on
// the JS thread or during environment teardown.  One mutex orders them, so no
// thread calls a threadsafe function after it was released, and replacing the
// callback never races with a concurrent Post().  `ToJs` builds the callback
// argument on the JS thread; it is a template argument so queued events never
// depend on the sink object's lifetime.
template <typename Event, napi_value (*ToJs)(napi_env, const Event&)>
class ThreadsafeSink {
 public:
  // `description` names the sink in errors; a sink that does not keep the
  // loop alive is unref'd so an idle addon lets Node exit.
  ThreadsafeSink(const char* resource_name, const char* description,
                 size_t max_queue, bool keep_loop_alive)
      : resource_name_(resource_name),
        description_(description),
        max_queue_(max_queue),
        keep_loop_alive_(keep_loop_alive) {}
  ~ThreadsafeSink() { Close(napi_tsfn_release); }

  ThreadsafeSink(const ThreadsafeSink&) = delete;
  ThreadsafeSink& operator=(const ThreadsafeSink&) = delete;

  // JS thread: routes events to `callback` and releases the previous one.
  void Install(napi_env env, napi_value callback) {
    napi_value name = nullptr;
    napi_threadsafe_function tsfn = nullptr;
    if (napi_create_string_utf8(env, resource_name_, NAPI_AUTO_LENGTH,
                                &name) != napi_ok ||
        napi_create_threadsafe_function(env, callback, nullptr, name,
                                        max_queue_, 1, nullptr, nullptr,
                                        nullptr, &CallJs, &tsfn) != napi_ok) {
      throw Error(std::string("Unable to create ") + description_ +
                  " callback");
    }
    if (!keep_loop_alive_) napi_unref_threadsafe_function(env, tsfn);
    napi_threadsafe_function previous = nullptr;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      previous = tsfn_;
      tsfn_ = tsfn;
    }
    if (previous != nullptr) {
      napi_release_threadsafe_function(previous, napi_tsfn_release);
    }
  }

  // Any thread: queues one event.  Returns false when no callback is
  // installed or the queue is full (the event is dropped).
  bool Post(Event event) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (tsfn_ == nullptr) return false;
    auto* data = new Event(std::move(event));
    if (napi_call_threadsafe_function(tsfn_, data, napi_tsfn_nonblocking) !=
        napi_ok) {
      delete data;
      return false;
    }
    return true;
  }

  // Stops delivery.  `napi_tsfn_abort` also drops queued events; use it while
  // the environment is torn down.
  void Close(napi_threadsafe_function_release_mode mode = napi_tsfn_release) {
    napi_threadsafe_function tsfn = nullptr;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      tsfn = tsfn_;
      tsfn_ = nullptr;
    }
    if (tsfn != nullptr) napi_release_threadsafe_function(tsfn, mode);
  }

 private:
  static void CallJs(napi_env env, napi_value callback, void* /*context*/,
                     void* data) {
    std::unique_ptr<Event> event(static_cast<Event*>(data));
    // A null env means the function is being finalized with queued events.
    if (env == nullptr || callback == nullptr || event == nullptr) return;
    try {
      napi_value argument = ToJs(env, *event);
      napi_value undefined = Undefined(env);
      napi_value result = nullptr;
      napi_call_function(env, undefined, callback, 1, &argument, &result);
    } catch (const std::exception&) {
      // Building the argument failed; drop the event.
    }
  }

  const char* resource_name_;
  const char* description_;
  size_t max_queue_;
  bool keep_loop_alive_;
  std::mutex mutex_;
  napi_threadsafe_function tsfn_ = nullptr;
};

}  // namespace skyline::napi
