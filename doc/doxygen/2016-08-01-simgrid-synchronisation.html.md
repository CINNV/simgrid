---
title: C++ synchronisations for SimGrid
tags:
 - computer
 - simgrid
 - c++
 - future
---

This is an overview of some recent additions to the SimGrid code
related to actor synchronisation. It might be interesting for people
using SimGrid, working on SimGrid or for people interested in generic
C++ code for synchronisation or asynchronicity.

READMORE

## SimGrid as a Discrete Event Simulator

[SimGrid](http://simgrid.gforge.inria.fr/) is a discrete event simulator of
distributed systems: it does not simulate the world by small fixed-size steps
but determines the date of the next event (such as the end of a communication,
the end of a computation) and jumps to this date.

A number of actors executing user-provided code run on top of the
simulator kernel[^kernel]. When an actor needs to interact with the simulator
kernel (eg. to start a communication), it issues a <i>simcall</i>
(simulation call, an analogy to system calls) to the simulator kernel.
This freezes the actor until it is woken up by the simulator kernel
(eg. when the communication is finished).

The key ideas here are:

 * The simulator is a discrete event simulator (event-driven).

 * An actor can issue a blocking simcall and will be suspended until
   it is woken up by the simulator kernel (when the operation is
   completed).

 * In order to move forward in (simulated) time, the simulation kernel
   needs to know which actions the actors want to do.

 * As a consequence, the simulated time can only move forward when all the
   actors are waiting on a simcall.

 * An actor cannot synchronise with another actor using OS-level primitives
   such as `pthread_mutex_lock()` or `std::mutex`. The simulation kernel
   would wait for the actor to issue a simcall and would deadlock. Instead it
   must use simulation-level synchronisation primitives
   (such as `simcall_mutex_lock()`).

 * Similarly, it cannot sleep using `std::this_thread::sleep_for()` which waits
   in the real world but must instead wait in the simulation with
   `simcall_process_sleep()` which waits in the simulation.

 * The simulation kernel cannot block.
   Only the actors can block (using simulation primitives).

## Futures

### What is a future?

We need a generic way to represent asynchronous operations in the
simulation kernel. [Futures](https://en.wikipedia.org/wiki/Futures_and_promises)
are a nice abstraction for this which have been added to a lot languages
(Java, Python, C++ since C++11, ECMAScript, etc.).

A future represents the result of an asynchronous operation. As the operation
may not be completed yet, its result is not available yet. Two different sort
of APIs may be available to expose this future result:

 * a <b>blocking API</b> where we wait for the result to be available
   (`res = f.get()`);

 * a <b>continuation-based API</b>[^then] where we say what should be
   done with the result when the operation completes
   (`future.then(something_to_do_with_the_result)`).

C++11 includes a generic class (`std::future<T>`) which implements a blocking API.
The [continuation-based API](http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2015/p0159r0.html#futures.unique_future.6)
is not available in the standard (yet) but is described in the
[Concurrency Technical
Specification](http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2015/p0159r0.html).

### Which future do we need?

We might want to use a solution based on `std::future` but our need is slightly
different from the C++11 futures. C++11 futures are not suitable for usage inside
the simulation kernel because they are only providing a blocking API
(`future.get()`) whereas the simulation kernel <em>cannot</em> block.
Instead, we need a continuation-based API to be used in our event-driven
simulation kernel.

The C++ Concurrency TS describes a continuation-based API.
Our future are based on this with a few differences[^promise_differences]:

 * The simulation kernel is single-threaded so we do not need thread
   synchronisation for out futures.

 * As the simulation kernel cannot block, `f.wait()` and its variants are not
   meaningful in this context.

 * Similarly, `future.get()` does an implicit wait. Calling this method in the
   simulation kernel only makes sense if the future is already ready. If the
   future is not ready, this would deadlock the simulator and an error is
   raised instead.

 * We always call the continuations in the simulation loop (and not
   inside the `future.then()` or `promise.set_value()` calls).

### Implementing `Future`

The implementation of future is in `simgrid::kernel::Future` and
`simgrid::kernel::Promise`[^promise] and is based on the Concurrency
TS[^sharedfuture]:

The future and the associated promise use a shared state defined with:

~~~cpp
enum class FutureStatus {
  not_ready,
  ready,
  done,
};

class FutureStateBase : private boost::noncopyable {
public:
  void schedule(simgrid::xbt::Task<void()>&& job);
  void set_exception(std::exception_ptr exception);
  void set_continuation(simgrid::xbt::Task<void()>&& continuation);
  FutureStatus get_status() const;
  bool is_ready() const;
  // [...]
private:
  FutureStatus status_ = FutureStatus::not_ready;
  std::exception_ptr exception_;
  simgrid::xbt::Task<void()> continuation_;
};

template<class T>
class FutureState : public FutureStateBase {
public:
  void set_value(T value);
  T get();
private:
  boost::optional<T> value_;
};

template<class T>
class FutureState<T&> : public FutureStateBase {
  // ...
};
template<>
class FutureState<void> : public FutureStateBase {
  // ...
};
~~~

Both `Future` and `Promise` have a reference to the shared state:

~~~cpp
template<class T>
class Future {
  // [...]
private:
  std::shared_ptr<FutureState<T>> state_;
};

template<class T>
class Promise {
  // [...]
private:
  std::shared_ptr<FutureState<T>> state_;
  bool future_get_ = false;
};
~~~

The crux of `future.then()` is:

~~~cpp
template<class T>
template<class F>
auto simgrid::kernel::Future<T>::thenNoUnwrap(F continuation)
-> Future<decltype(continuation(std::move(*this)))>
{
  typedef decltype(continuation(std::move(*this))) R;

  if (state_ == nullptr)
    throw std::future_error(std::future_errc::no_state);

  auto state = std::move(state_);
  // Create a new future...
  Promise<R> promise;
  Future<R> future = promise.get_future();
  // ...and when the current future is ready...
  state->set_continuation(simgrid::xbt::makeTask(
    [](Promise<R> promise, std::shared_ptr<FutureState<T>> state,
         F continuation) {
      // ...set the new future value by running the continuation.
      Future<T> future(std::move(state));
      simgrid::xbt::fulfillPromise(promise,[&]{
        return continuation(std::move(future));
      });
    },
    std::move(promise), state, std::move(continuation)));
  return std::move(future);
}
~~~

We added a (much simpler) `future.then_()` method which does not
create a new future:

~~~cpp
template<class T>
template<class F>
void simgrid::kernel::Future<T>::then_(F continuation)
{
  if (state_ == nullptr)
    throw std::future_error(std::future_errc::no_state);
  // Give shared-ownership to the continuation:
  auto state = std::move(state_);
  state->set_continuation(simgrid::xbt::makeTask(
    std::move(continuation), state));
}
~~~

The `.get()` delegates to the shared state. As we mentioned previously, an
error is raised if future is not ready:

~~~cpp
template<class T>
T simgrid::kernel::Future::get()
{
  if (state_ == nullptr)
    throw std::future_error(std::future_errc::no_state);
  std::shared_ptr<FutureState<T>> state = std::move(state_);
  return state->get();
}

template<class T>
T simgrid::kernel::SharedState<T>::get()
{
  if (status_ != FutureStatus::ready)
    xbt_die("Deadlock: this future is not ready");
  status_ = FutureStatus::done;
  if (exception_) {
    std::exception_ptr exception = std::move(exception_);
    std::rethrow_exception(std::move(exception));
  }
  xbt_assert(this->value_);
  auto result = std::move(this->value_.get());
  this->value_ = boost::optional<T>();
  return std::move(result);
}
~~~

## Generic simcalls

### Motivation

Simcalls are not so easy to understand and adding a new one is not so easy.
In order to add one simcall, one has to first
add it to the [list of simcalls](https://github.com/simgrid/simgrid/blob/4ae2fd01d8cc55bf83654e29f294335e3cb1f022/src/simix/simcalls.in)
which looks like this:

~~~cpp
# This looks like C++ but it is a basic IDL-like language
# (one definition per line) parsed by a python script:

void process_kill(smx_process_t process);
void process_killall(int reset_pid);
void process_cleanup(smx_process_t process) [[nohandler]];
void process_suspend(smx_process_t process) [[block]];
void process_resume(smx_process_t process);
void process_set_host(smx_process_t process, sg_host_t dest);
int  process_is_suspended(smx_process_t process) [[nohandler]];
int  process_join(smx_process_t process, double timeout) [[block]];
int  process_sleep(double duration) [[block]];

smx_mutex_t mutex_init();
void        mutex_lock(smx_mutex_t mutex) [[block]];
int         mutex_trylock(smx_mutex_t mutex);
void        mutex_unlock(smx_mutex_t mutex);

[...]
~~~

At runtime, a simcall is represented by a structure containing a simcall
number and its arguments (among some other things):

~~~cpp
struct s_smx_simcall {
  // Simcall number:
  e_smx_simcall_t call;
  // Issuing actor:
  smx_process_t issuer;
  // Arguments of the simcall:
  union u_smx_scalar args[11];
  // Result of the simcall:
  union u_smx_scalar result;
  // Some additional stuff:
  smx_timer_t timer;
  int mc_value;
};
~~~

with the a scalar union type:

~~~cpp
union u_smx_scalar {
  char            c;
  short           s;
  int             i;
  long            l;
  long long       ll;
  unsigned char   uc;
  unsigned short  us;
  unsigned int    ui;
  unsigned long   ul;
  unsigned long long ull;
  double          d;
  void*           dp;
  FPtr            fp;
};
~~~

This file is read by a [Python script](https://github.com/simgrid/simgrid/blob/4ae2fd01d8cc55bf83654e29f294335e3cb1f022/src/simix/simcalls.py)
which generates a bunch of C++ files:

* an enum of all the [simcall numbers](https://github.com/simgrid/simgrid/blob/4ae2fd01d8cc55bf83654e29f294335e3cb1f022/src/simix/popping_enum.h#L19);

* [user-side wrappers](https://github.com/simgrid/simgrid/blob/4ae2fd01d8cc55bf83654e29f294335e3cb1f022/src/simix/popping_bodies.cpp)
  responsible for wrapping the parameters in the `struct s_smx_simcall`;
  and wrapping out the result;

* [accessors](https://github.com/simgrid/simgrid/blob/4ae2fd01d8cc55bf83654e29f294335e3cb1f022/src/simix/popping_accessors.h)
   to get/set values of of `struct s_smx_simcall`;

* a simulation-kernel-side [big switch](https://github.com/simgrid/simgrid/blob/4ae2fd01d8cc55bf83654e29f294335e3cb1f022/src/simix/popping_generated.cpp#L106)
  handling all the simcall numbers.


In order to simplify simulation kernel/actor synchronisations, we added two
generic simcalls which can be used to execute a function in the simulation kernel:

~~~cpp
# This one should really be called run_immediate:
void run_kernel(std::function<void()> const* code) [[nohandler]];
void run_blocking(std::function<void()> const* code) [[block,nohandler]];
~~~

### Immediate simcall

The first one (`simcall_run_kernel()`) executes a function in the simulation
kernel context and returns immediately (without blocking the actor):

~~~cpp
void simcall_run_kernel(std::function<void()> const& code)
{
  simcall_BODY_run_kernel(&code);
}

template<class F> inline
void simcall_run_kernel(F& f)
{
  simcall_run_kernel(std::function<void()>(std::ref(f)));
}
~~~

On top of this, we add a wrapper which can be used to return a value of any
type and properly handles exceptions:

~~~cpp
template<class F>
typename std::result_of<F()>::type kernelImmediate(F&& code)
{
  // If we are in the simulation kernel, we take the fast path and
  // execute the code directly without simcall
  // marshalling/unmarshalling/dispatch:
  if (SIMIX_is_maestro())
    return std::forward<F>(code)();

  // If we are in the application, pass the code to the simulation
  // kernel which executes it for us and reports the result:
  typedef typename std::result_of<F()>::type R;
  simgrid::xbt::Result<R> result;
  simcall_run_kernel([&]{
    xbt_assert(SIMIX_is_maestro(), "Not in maestro");
    simgrid::xbt::fulfillPromise(result, std::forward<F>(code));
  });
  return result.get();
}
~~~

where [`Result<R>`](#result) can store either a `R` or an exception.

Example of usage:

~~~cpp
xbt_dict_t Host::properties() {
  return simgrid::simix::kernelImmediate([&] {
    simgrid::surf::HostImpl* surf_host =
      this->extension<simgrid::surf::HostImpl>();
    return surf_host->getProperties();
  });
}
~~~

### Blocking simcall

The second generic simcall (`simcall_run_blocking()`) executes a function in
the SimGrid simulation kernel immediately but does not wake up the calling actor
immediately:

~~~cpp
void simcall_run_blocking(std::function<void()> const& code);

template<class F>
void simcall_run_blocking(F& f)
{
  simcall_run_blocking(std::function<void()>(std::ref(f)));
}
~~~

The `f` function is expected to setup some callbacks in the simulation
kernel which will wake up the actor (with
`simgrid::simix::unblock(actor)`) when the operation is completed.

This is wrapped in a higher-level primitive as well. The
`kernelSync()` function expects a function-object which is executed
immediately in the simulation kernel and returns a `Future<T>`.  The
simulator blocks the actor and resumes it when the `Future<T>` becomes
ready with its result:

~~~cpp
template<class F>
auto kernelSync(F code) -> decltype(code().get())
{
  typedef decltype(code().get()) T;
  if (SIMIX_is_maestro())
    xbt_die("Can't execute blocking call in kernel mode");

  smx_process_t self = SIMIX_process_self();
  simgrid::xbt::Result<T> result;

  simcall_run_blocking([&result, self, &code]{
    try {
      auto future = code();
      future.then_([&result, self](simgrid::kernel::Future<T> value) {
        // Propagate the result from the future
        // to the simgrid::xbt::Result:
        simgrid::xbt::setPromise(result, value);
        simgrid::simix::unblock(self);
      });
    }
    catch (...) {
      // The code failed immediately. We can wake up the actor
      // immediately with the exception:
      result.set_exception(std::current_exception());
      simgrid::simix::unblock(self);
    }
  });

  // Get the result of the operation (which might be an exception):
  return result.get();
}
~~~

A contrived example of this would be:

~~~cpp
int res = simgrid::simix::kernelSync([&] {
  return kernel_wait_until(30).then(
    [](simgrid::kernel::Future<void> future) {
      return 42;
    }
  );
});
~~~

### Asynchronous operations

We can write the related `kernelAsync()` which wakes up the actor immediately
and returns a future to the actor. As this future is used in the actor context,
it is a different future
(`simgrid::simix::Future` instead of `simgrid::kernel::Furuere`)
which implements a C++11 `std::future` wait-based API:

~~~cpp
template <class T>
class Future {
public:
  Future() {}
  Future(simgrid::kernel::Future<T> future) : future_(std::move(future)) {}
  bool valid() const { return future_.valid(); }
  T get();
  bool is_ready() const;
  void wait();
private:
  // We wrap an event-based kernel future:
  simgrid::kernel::Future<T> future_;
};
~~~

The `future.get()` method is implemented as[^getcompared]:

~~~cpp
template<class T>
T simgrid::simix::Future<T>::get()
{
  if (!valid())
    throw std::future_error(std::future_errc::no_state);
  smx_process_t self = SIMIX_process_self();
  simgrid::xbt::Result<T> result;
  simcall_run_blocking([this, &result, self]{
    try {
      // When the kernel future is ready...
      this->future_.then_(
        [this, &result, self](simgrid::kernel::Future<T> value) {
          // ... wake up the process with the result of the kernel future.
          simgrid::xbt::setPromise(result, value);
          simgrid::simix::unblock(self);
      });
    }
    catch (...) {
      result.set_exception(std::current_exception());
      simgrid::simix::unblock(self);
    }
  });
  return result.get();
}
~~~

`kernelAsync()` simply :wink: calls `kernelImmediate()` and wraps the
`simgrid::kernel::Future` into a `simgrid::simix::Future`:

~~~cpp
template<class F>
auto kernelAsync(F code)
  -> Future<decltype(code().get())>
{
  typedef decltype(code().get()) T;

  // Execute the code in the simulation kernel and get the kernel future:
  simgrid::kernel::Future<T> future =
    simgrid::simix::kernelImmediate(std::move(code));

  // Wrap the kernel future in a user future:
  return simgrid::simix::Future<T>(std::move(future));
}
~~~

A contrived example of this would be:

~~~cpp
simgrid::simix::Future<int> future = simgrid::simix::kernelSync([&] {
  return kernel_wait_until(30).then(
    [](simgrid::kernel::Future<void> future) {
      return 42;
    }
  );
});
do_some_stuff();
int res = future.get();
~~~

`kernelSync()` could be rewritten as:

~~~cpp
template<class F>
auto kernelSync(F code) -> decltype(code().get())
{
  return kernelAsync(std::move(code)).get();
}
~~~

The semantic is equivalent but this form would require two simcalls
instead of one to do the same job (one in `kernelAsync()` and one in
`.get()`).

## Representing the simulated time

SimGrid uses `double` for representing the simulated time:

* durations are expressed in seconds;

* timepoints are expressed as seconds from the beginning of the simulation.

In contrast, all the C++ APIs use `std::chrono::duration` and
`std::chrono::time_point`. They are used in:

* `std::this_thread::wait_for()` and `std::this_thread::wait_until()`;

* `future.wait_for()` and `future.wait_until()`;

* `condvar.wait_for()` and `condvar.wait_until()`.

We can define `future.wait_for(duration)` and `future.wait_until(timepoint)`
for our futures but for better compatibility with standard C++ code, we might
want to define versions expecting `std::chrono::duration` and
`std::chrono::time_point`.

For time points, we need to define a clock (which meets the
[TrivialClock](http://en.cppreference.com/w/cpp/concept/TrivialClock)
requirements, see
[`[time.clock.req]`](http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2014/n4296.pdf#page=642)
working in the simulated time in the C++14 standard):

~~~cpp
struct SimulationClock {
  using rep        = double;
  using period     = std::ratio<1>;
  using duration   = std::chrono::duration<rep, period>;
  using time_point = std::chrono::time_point<SimulationClock, duration>;
  static constexpr bool is_steady = true;
  static time_point now()
  {
    return time_point(duration(SIMIX_get_clock()));
  }
};
~~~

A time point in the simulation is a time point using this clock:

~~~cpp
template<class Duration>
using SimulationTimePoint =
  std::chrono::time_point<SimulationClock, Duration>;
~~~

This is used for example in `simgrid::s4u::this_actor::sleep_for()` and
`simgrid::s4u::this_actor::sleep_until()`:

~~~cpp
void sleep_for(double duration)
{
  if (duration > 0)
    simcall_process_sleep(duration);
}

void sleep_until(double timeout)
{
  double now = SIMIX_get_clock();
  if (timeout > now)
    simcall_process_sleep(timeout - now);
}

template<class Rep, class Period>
void sleep_for(std::chrono::duration<Rep, Period> duration)
{
  auto seconds =
    std::chrono::duration_cast<SimulationClockDuration>(duration);
  this_actor::sleep_for(seconds.count());
}

template<class Duration>
void sleep_until(const SimulationTimePoint<Duration>& timeout_time)
{
  auto timeout_native =
    std::chrono::time_point_cast<SimulationClockDuration>(timeout_time);
  this_actor::sleep_until(timeout_native.time_since_epoch().count());
}
~~~

Which means it is possible to use (since C++14):

~~~cpp
using namespace std::chrono_literals;
simgrid::s4u::actor::sleep_for(42s);
~~~

## Mutexes and condition variables

## Mutexes

SimGrid has had a C-based API for mutexes and condition variables for
some time.  These mutexes are different from the standard
system-level mutex (`std::mutex`, `pthread_mutex_t`, etc.) because
they work at simulation-level.  Locking on a simulation mutex does
not block the thread directly but makes a simcall
(`simcall_mutex_lock()`) which asks the simulator kernel to wake the calling
actor when it can get ownership of the mutex. Blocking directly at the
OS level would deadlock the simulation.

Reusing the C++ standard API for our simulation mutexes has many
benefits:

 * it makes it easier for people familiar with the `std::mutex` to
   understand and use SimGrid mutexes;

 * we can benefit from a proven API;

 * we can reuse from generic library code in SimGrid.

We defined a reference-counted `Mutex` class for this (which supports
the [`Lockable`](http://en.cppreference.com/w/cpp/concept/Lockable)
requirements, see
[`[thread.req.lockable.req]`](http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2014/n4296.pdf#page=1175)
in the C++14 standard):

~~~cpp
class Mutex {
  friend ConditionVariable;
private:
  friend simgrid::simix::Mutex;
  simgrid::simix::Mutex* mutex_;
  Mutex(simgrid::simix::Mutex* mutex) : mutex_(mutex) {}
public:

  friend void intrusive_ptr_add_ref(Mutex* mutex);
  friend void intrusive_ptr_release(Mutex* mutex);
  using Ptr = boost::intrusive_ptr<Mutex>;

  // No copy:
  Mutex(Mutex const&) = delete;
  Mutex& operator=(Mutex const&) = delete;

  static Ptr createMutex();

public:
  void lock();
  void unlock();
  bool try_lock();
};
~~~

The methods are simply wrappers around existing simcalls:

~~~cpp
void Mutex::lock()
{
  simcall_mutex_lock(mutex_);
}
~~~

Using the same API as `std::mutex` (`Lockable`) means we can use existing
C++-standard code such as `std::unique_lock<Mutex>` or
`std::lock_guard<Mutex>` for exception-safe mutex handling:

~~~cpp
{
  std::lock_guard<simgrid::s4u::Mutex> lock(*mutex);
  sum += 1;
}
~~~

### Condition Variables

Similarly SimGrid already had simulation-level condition variables
which can be exposed using the same API as `std::condition_variable`:

~~~cpp
class ConditionVariable {
private:
  friend s_smx_cond;
  smx_cond_t cond_;
  ConditionVariable(smx_cond_t cond) : cond_(cond) {}
public:

  ConditionVariable(ConditionVariable const&) = delete;
  ConditionVariable& operator=(ConditionVariable const&) = delete;

  friend void intrusive_ptr_add_ref(ConditionVariable* cond);
  friend void intrusive_ptr_release(ConditionVariable* cond);
  using Ptr = boost::intrusive_ptr<ConditionVariable>;
  static Ptr createConditionVariable();

  void wait(std::unique_lock<Mutex>& lock);
  template<class P>
  void wait(std::unique_lock<Mutex>& lock, P pred);

  // Wait functions taking a plain double as time:

  std::cv_status wait_until(std::unique_lock<Mutex>& lock,
    double timeout_time);
  std::cv_status wait_for(
    std::unique_lock<Mutex>& lock, double duration);
  template<class P>
  bool wait_until(std::unique_lock<Mutex>& lock,
    double timeout_time, P pred);
  template<class P>
  bool wait_for(std::unique_lock<Mutex>& lock,
    double duration, P pred);

  // Wait functions taking a std::chrono time:

  template<class Rep, class Period, class P>
  bool wait_for(std::unique_lock<Mutex>& lock,
    std::chrono::duration<Rep, Period> duration, P pred);
  template<class Rep, class Period>
  std::cv_status wait_for(std::unique_lock<Mutex>& lock,
    std::chrono::duration<Rep, Period> duration);
  template<class Duration>
  std::cv_status wait_until(std::unique_lock<Mutex>& lock,
    const SimulationTimePoint<Duration>& timeout_time);
  template<class Duration, class P>
  bool wait_until(std::unique_lock<Mutex>& lock,
    const SimulationTimePoint<Duration>& timeout_time, P pred);

  // Notify:

  void notify_one();
  void notify_all();
  
};
~~~

We currently accept both `double` (for simplicity and consistency with
the current codebase) and `std::chrono` types (for compatibility with
C++ code) as durations and timepoints. One important thing to notice here is
that `cond.wait_for()` and `cond.wait_until()` work in the simulated time,
not in the real time.

The simple `cond.wait()` and `cond.wait_for()` delegate to
pre-existing simcalls:

~~~cpp
void ConditionVariable::wait(std::unique_lock<Mutex>& lock)
{
  simcall_cond_wait(cond_, lock.mutex()->mutex_);
}

std::cv_status ConditionVariable::wait_for(
  std::unique_lock<Mutex>& lock, double timeout)
{
  // The simcall uses -1 for "any timeout" but we don't want this:
  if (timeout < 0)
    timeout = 0.0;

  try {
    simcall_cond_wait_timeout(cond_, lock.mutex()->mutex_, timeout);
    return std::cv_status::no_timeout;
  }
  catch (xbt_ex& e) {

    // If the exception was a timeout, we have to take the lock again:
    if (e.category == timeout_error) {
      try {
        lock.mutex()->lock();
        return std::cv_status::timeout;
      }
      catch (...) {
        std::terminate();
      }
    }

    std::terminate();
  }
  catch (...) {
    std::terminate();
  }
}
~~~

Other methods are simple wrappers around those two:

~~~cpp
template<class P>
void ConditionVariable::wait(std::unique_lock<Mutex>& lock, P pred)
{
  while (!pred())
    wait(lock);
}

template<class P>
bool ConditionVariable::wait_until(std::unique_lock<Mutex>& lock,
  double timeout_time, P pred)
{
  while (!pred())
    if (this->wait_until(lock, timeout_time) == std::cv_status::timeout)
      return pred();
  return true;
}

template<class P>
bool ConditionVariable::wait_for(std::unique_lock<Mutex>& lock,
  double duration, P pred)
{
  return this->wait_until(lock,
    SIMIX_get_clock() + duration, std::move(pred));
}
~~~


## Conclusion

We wrote two future implementations based on the `std::future` API:

* the first one is a non-blocking event-based (`future.then(stuff)`)
  future used inside our (non-blocking event-based) simulator kernel;

* the second one is a wait-based (`future.get()`) future used in the actors
  which waits using a simcall.

These futures are used to implement `kernelSync()` and `kernelAsync()` which
expose asynchronous operations in the simulator kernel to the actors.

In addition, we wrote variations of some other C++ standard library
classes (`SimulationClock`, `Mutex`, `ConditionVariable`) which work in
the simulation:
  
  * using simulated time;

  * using simcalls for synchronisation.

Reusing the same API as the C++ standard library is very useful because:

  * we use a proven API with a clearly defined semantic;

  * people already familiar with those API can use our own easily;

  * users can rely on documentation, examples and tutorials made by other
    people;

  * we can reuse generic code with our types (`std::unique_lock`,
   `std::lock_guard`, etc.).

This type of approach might be useful for other libraries which define
their own contexts. An example of this is
[Mordor](https://github.com/mozy/mordor), a I/O library using fibers
(cooperative scheduling): it implements cooperative/fiber
[mutex](https://github.com/mozy/mordor/blob/4803b6343aee531bfc3588ffc26a0d0fdf14b274/mordor/fibersynchronization.h#L70),
[recursive
mutex](https://github.com/mozy/mordor/blob/4803b6343aee531bfc3588ffc26a0d0fdf14b274/mordor/fibersynchronization.h#L105)
which are compatible with the
[`BasicLockable`](http://en.cppreference.com/w/cpp/concept/BasicLockable)
requirements (see
[`[thread.req.lockable.basic]`]((http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2014/n4296.pdf#page=1175))
in the C++14 standard).

## Appendix: useful helpers

### `Result`

Result is like a mix of `std::future` and `std::promise` in a
single-object without shared-state and synchronisation:

~~~cpp
template<class T>
class Result {
  enum class ResultStatus {
    invalid,
    value,
    exception,
  };
public:
  Result();
  ~Result();
  Result(Result const& that);
  Result& operator=(Result const& that);
  Result(Result&& that);
  Result& operator=(Result&& that);
  bool is_valid() const;
  void reset();
  void set_exception(std::exception_ptr e);
  void set_value(T&& value);
  void set_value(T const& value);
  T get();
private:
  ResultStatus status_ = ResultStatus::invalid;
  union {
    T value_;
    std::exception_ptr exception_;
  };
};
~~~~

### Promise helpers

Those helper are useful for dealing with generic future-based code:

~~~cpp
template<class R, class F>
auto fulfillPromise(R& promise, F&& code)
-> decltype(promise.set_value(code()))
{
  try {
    promise.set_value(std::forward<F>(code)());
  }
  catch(...) {
    promise.set_exception(std::current_exception());
  }
}

template<class P, class F>
auto fulfillPromise(P& promise, F&& code)
-> decltype(promise.set_value())
{
  try {
    std::forward<F>(code)();
    promise.set_value();
  }
  catch(...) {
    promise.set_exception(std::current_exception());
  }
}

template<class P, class F>
void setPromise(P& promise, F&& future)
{
  fulfillPromise(promise, [&]{ return std::forward<F>(future).get(); });
}
~~~

### Task

`Task<R(F...)>` is a type-erased callable object similar to
`std::function<R(F...)>` but works for move-only types. It is similar to
`std::package_task<R(F...)>` but does not wrap the result in a `std::future<R>`
(it is not <i>packaged</i>).

|               |`std::future`   |`std::packaged_task`|`simgrid::xbt::Task`
|---------------|----------------|--------------------|--------------------------
|Copyable       | Yes            | No                 | No
|Movable        | Yes            | Yes                | Yes
|Call           | `const`        | non-`const`        | non-`const`
|Callable       | multiple times | once               | once
|Sets a promise | No             | Yes                | No

It could be implemented as:

~~~cpp
template<class T>
class Task {
private:
  std::packaged_task<T> task_;
public:

  template<class F>
  void Task(F f) :
    task_(std::forward<F>(f))
  {}

  template<class... ArgTypes>
  auto operator()(ArgTypes... args)
  -> decltype(task_.get_future().get())
  {
    task_(std::forward<ArgTypes)(args)...);
    return task_.get_future().get();
  }

};
~~~

but we don't need a shared-state.

This is useful in order to bind move-only type arguments:

~~~cpp
template<class F, class... Args>
class TaskImpl {
private:
  F code_;
  std::tuple<Args...> args_;
  typedef decltype(simgrid::xbt::apply(
    std::move(code_), std::move(args_))) result_type;
public:
  TaskImpl(F code, std::tuple<Args...> args) :
    code_(std::move(code)),
    args_(std::move(args))
  {}
  result_type operator()()
  {
    // simgrid::xbt::apply is C++17 std::apply:
    return simgrid::xbt::apply(std::move(code_), std::move(args_));
  }
};

template<class F, class... Args>
auto makeTask(F code, Args... args)
-> Task< decltype(code(std::move(args)...))() >
{
  TaskImpl<F, Args...> task(
    std::move(code), std::make_tuple(std::move(args)...));
  return std::move(task);
}
~~~


## Notes

[^kernel]:

    The relationship between the SimGrid simulation kernel and the simulated
    actors is similar to the relationship between an OS kernel and the OS
    processes: the simulation kernel manages (schedules) the execution of the
    actors; the actors make requests to the simulation kernel using simcalls.
    However, both the simulation kernel and the actors currently run in the same
    OS process (and use same address space).

[^then]:

    This is the kind of futures that are available in ECMAScript which use
    the same kind of never-blocking asynchronous model as our discrete event
    simulator.


[^sharedfuture]:

    Currently, we did not implement some features such as shared
    futures.

[^getcompared]:

    You might want to compare this method with `simgrid::kernel::Future::get()`
    we showed previously: the method of the kernel future does not block and
    raises an error if the future is not ready; the method of the actor future
    blocks after having set a continuation to wake the actor when the future
    is ready.

[^promise_differences]:

    (which are related to the fact that we are in a non-blocking single-threaded
    simulation engine)

[^promise]:

    In the C++ standard library, `std::future<T>` is used <em>by the consumer</em>
    of the result. On the other hand, `std::promise<T>` is used <em>by the
    producer</em> of the result. The consumer calls `promise.set_value(42)`
    or `promise.set_exception(e)` in order to <em>set the result</em> which will
    be made available to the consumer by `future.get()`.