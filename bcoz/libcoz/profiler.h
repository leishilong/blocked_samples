/*
 * Copyright (c) 2015, Charlie Curtsinger and Emery Berger,
 *                     University of Massachusetts Amherst
 * This file is part of the Coz project. See LICENSE.md file at the top-level
 * directory of this distribution and at http://github.com/plasma-umass/coz.
 */

#if !defined(CAUSAL_RUNTIME_PROFILER_H)
#define CAUSAL_RUNTIME_PROFILER_H

#include <atomic>
#include <cstdint>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <map>

#include "coz.h"

#include "inspect.h"
#include "progress_point.h"
#include "thread_state.h"
#include "util.h"

#include "ccutil/spinlock.h"
#include "ccutil/static_map.h"

#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/types.h>

#define SHARED_MEMORY_KEY 1234

using namespace std;

/// Type of a thread entry function
typedef void* (*thread_fn_t)(void*);

/// The type of a main function
typedef int (*main_fn_t)(int, char**, char**);

enum {
  SampleSignal = SIGPROF, //< Signal to generate when samples are ready
  //BlockedSignal = SIGUSR1, //< Signal to process blocked samples when they are injected
  SamplePeriod = 1000000, //< Time between samples (1ms)
  SampleBatchSize = 1,   //< Samples to batch together for one processing run
  SpeedupDivisions = 20,  //< How many different speedups to try (20 = 5% increments)
  ZeroSpeedupWeight = 7,  //< Weight of speedup=0 versus other speedup values (7 = ~25% of experiments run with zero speedup)
  //ExperimentMinTime = SamplePeriod * SampleBatchSize * 50 * 20 * 2,  //< Minimum experiment length
  ExperimentMinTime = SamplePeriod * SampleBatchSize * 50,  //< Minimum experiment length
  //ExperimentCoolOffTime = SamplePeriod * SampleBatchSize * 20 * 2,   //< Time to wait after an experiment
  ExperimentCoolOffTime = SamplePeriod * SampleBatchSize,   //< Time to wait after an experiment
  ExperimentTargetDelta = 5 //< Target minimum number of visits to a progress point during an experiment
};

/**
 * Argument type passed to wrapped threads
 */
struct thread_start_arg {
  thread_fn_t _fn;
  void* _arg;
  size_t _parent_delay_time;

  thread_start_arg(thread_fn_t fn, void* arg, size_t t) :
      _fn(fn), _arg(arg), _parent_delay_time(t) {}
};

void init_coz(void);
class profiler {
public:
  /// Start the profiler
  void startup(const std::string& outfile,
               line* fixed_line,
               int fixed_speedup,
               bool end_to_end, char blocked_scope, bool print_log, line* based_line, char based_blocked, int based_speedup);

  /// Shut down the profiler
  void shutdown();

  /// Get or create a progress point to measure throughput
  throughput_point* get_throughput_point(const std::string& name) {
    // Lock the map of throughput points
    _throughput_points_lock.lock();
  
    // Search for a matching point
    auto search = _throughput_points.find(name);
  
    // If there is no match, add a new throughput point
    if(search == _throughput_points.end()) {
      search = _throughput_points.emplace_hint(search, name, new throughput_point(name));
    }
  
    // Get the matching or inserted value
    throughput_point* result = search->second;
  
    // Unlock the map and return the result
    _throughput_points_lock.unlock();
    return result;
  }
  
  /// Get or create a progress point to measure latency
  latency_point* get_latency_point(const std::string& name) {
    // Lock the map of latency points
    _latency_points_lock.lock();
  
    // Search for a matching point
    auto search = _latency_points.find(name);
  
    // If there is no match, add a new latency point
    if(search == _latency_points.end()) {
      search = _latency_points.emplace_hint(search, name, new latency_point(name));
    }
  
    // Get the matching or inserted value
    latency_point* result = search->second;
  
    // Unlock the map and return the result
    _latency_points_lock.unlock();
    return result;
  }

  /// Pass local delay counts and excess delay time to the child thread
  int handle_pthread_create(pthread_t* thread,
                            const pthread_attr_t* attr,
                            thread_fn_t fn,
                            void* arg) {
    thread_start_arg* new_arg;

    thread_state* state = get_thread_state();
    if (NULL == state) {
      init_coz();
      state = get_thread_state();
    }
    REQUIRE(state) << "Thread state not found";

    if (_enable_print_log)	state->enable_print_log = true;

    // Allocate a struct to pass as an argument to the new thread
    new_arg = new thread_start_arg(fn, arg, state->local_delay);

    // Create a wrapped thread and pass in the wrapped argument
    return real::pthread_create(thread, attr, profiler::start_thread, new_arg);
  }

  /// Force threads to catch up on delays, and stop sampling before the thread exits
  void handle_pthread_exit(void* result) __attribute__((noreturn)) {
    end_sampling();
    real::pthread_exit(result);
    abort(); // Silence g++ warning about noreturn
  }

  /// Ensure a thread has executed all the required delays before possibly unblocking another thread
  void catch_up() {
    thread_state* state = get_thread_state();

    if(!state)
      return;

    // Handle all samples and add delays as required
    if(_experiment_active) {
      state->set_in_use(true);
      process_blocked_samples(state);
      //add_delays(state);
      state->set_in_use(false);
    }
  }

  /// Call before (possibly) blocking
  void pre_block() {
    thread_state* state = get_thread_state();
    if(!state)
      return;
    if(state->in_wait)
      return;
	
	state->in_wait = true;
    state->pre_block_time = _global_delay.load();
    //state->pre_local_time = state->local_delay;
    state->ex_count = (_experiment_active)? ex_count.load() : 0;
  }

  /// Call after unblocking. If by_thread is true, delays will be skipped
  void post_block(bool skip_delays) {
    thread_state* state = get_thread_state();
    if(!state)
      return;
    if(!state->in_wait)
      return;


    state->set_in_use(true);

    if (!_experiment_active)	state->local_delay = _global_delay.load();
    else if (skip_delays) {
      state->local_delay += _global_delay.load() - state->pre_block_time;
    }

    state->in_wait = false;

    if (_experiment_active)	process_blocked_samples(state);
    //process_samples(state);
    
    state->set_in_use(false);
  }

  void call_process_blocked_samples() {
    thread_state* state = get_thread_state();
    if (!state)
      return;

    state->set_in_use(true);
    process_blocked_samples(state);
    state->set_in_use(false);
  }

  /// Only allow one instance of the profiler, and never run the destructor
  static profiler& get_instance() {
    static char buf[sizeof(profiler)];
    static profiler* p = new(buf) profiler();
    return *p;
  }

private:
  profiler()  {
  	_experiment_active.store(false);
    _global_delay.store(0);
    _based_global_delay.store(0);
    _delay_size.store(0);
    _based_delay_size.store(0);
    _blocked_all.store(0);
    _blocked_io.store(0);
    _blocked_sched.store(0);
    _blocked_lock.store(0);
    _blocked_blocked.store(0);
    _blocked_oncpu.store(0);
    _selected_line.store(nullptr);
    _next_line.store(nullptr);
    _running.store(true);
    ex_count.store(0);
    print.store(false);
  }

  // Disallow copy and assignment
  profiler(const profiler&) = delete;
  void operator=(const profiler&) = delete;

  void profiler_thread(spinlock& l);          //< Body of the main profiler thread
  void begin_sampling(thread_state* state);   //< Start sampling in the current thread
  void end_sampling();                        //< Stop sampling in the current thread
  void add_delays(thread_state* state);       //< Add any required delays
  void process_samples(thread_state* state);  //< Process all available samples and insert delays
  void process_blocked_samples(thread_state* state);
  std::pair<line*,bool> match_line(perf_event::record&);       //< Map a sample to its source line and matches with selected_line
  bool based_match_line(perf_event::record&);       //< Map a sample to its source line and matches with selected_line
  void log_samples(std::ofstream&, size_t);   //< Log runtime and sample counts for all identified regions

  thread_state* add_thread(); //< Add a thread state entry for this thread
  thread_state* get_thread_state(); //< Get a reference to the thread state object for this thread
  void remove_thread(); //< Remove the thread state structure for the current thread

  static void* start_profiler_thread(void*);          //< Entry point for the profiler thread
  static void* start_thread(void* arg);               //< Entry point for wrapped threads
  static void start_process(size_t parent_delay);	  //< Entry point for wrapped processes
  static void samples_ready(int, siginfo_t*, void*);  //< Signal handler for sample processing
  static void samples_ready_empty(int, siginfo_t*, void*);  //< Signal handler for (empty) sample processing of profiler thread
  static void blocked_samples_ready(int, siginfo_t*, void*);  //< Signal handler for blocked sample processing
  static void on_error(int, siginfo_t*, void*);       //< Handle errors

  /// A map from name to throughput monitoring progress points
  std::unordered_map<std::string, throughput_point*> _throughput_points;
  spinlock _throughput_points_lock; //< Spinlock that protects the throughput points map
  
  /// A map from name to latency monitoring progress points
  std::unordered_map<std::string, latency_point*> _latency_points;
  spinlock _latency_points_lock;  //< Spinlock that protects the latency points map

  static_map<pid_t, thread_state> _thread_states;   //< Map from thread IDs to thread-local state

  std::atomic<uint64_t> ex_count;

  std::atomic<bool> _experiment_active; //< Is an experiment running?
  std::atomic<size_t> _global_delay;    //< The global delay time required
  std::atomic<size_t> _based_global_delay;    //< The global delay time required
  std::atomic<size_t> _delay_size;      //< The current delay size
  std::atomic<size_t> _based_delay_size;      //< The current based_delay size
  std::atomic<size_t> _blocked_all;
  std::atomic<size_t> _blocked_io;
  std::atomic<size_t> _blocked_lock;
  std::atomic<size_t> _blocked_sched;
  std::atomic<size_t> _blocked_blocked;
  std::atomic<size_t> _blocked_oncpu;
  std::atomic<line*> _selected_line;    //< The line to speed up
  std::atomic<line*> _next_line;        //< The next line to speed up
  std::atomic<bool> print;

  pthread_t _profiler_thread;     //< Handle for the profiler thread
  std::atomic<bool> _running;     //< Clear to signal the profiler thread to quit
  std::string _output_filename;   //< File for profiler output
  line* _fixed_line;              //< The only line that should be sped up, if set
  line* _based_line = NULL;			  //< Additional line that should be sped up (relational profiling), if set
  char _blocked_scope = 0;
  char _based_blocked = 0;
  int _fixed_delay_size = -1;     //< The only delay size that should be used, if set
  int _based_fixed_delay_size = -1;     //< Additional delay size that should be used (relational profiling), if set
  int num_tid = 0;
  bool omit_experiment = false;

  /// Should coz run in end-to-end mode?
  bool _enable_end_to_end;
  bool _enable_print_log;

  /// Atomic flag to guarantee shutdown procedures run exactly one time
  std::atomic_flag _shutdown_run = ATOMIC_FLAG_INIT;
};

#endif
