/*
 * Copyright (c) 2015, Charlie Curtsinger and Emery Berger,
 *                     University of Massachusetts Amherst
 * This file is part of the Coz project. See LICENSE.md file at the top-level
 * directory of this distribution and at http://github.com/plasma-umass/coz.
 */

#if !defined(CAUSAL_RUNTIME_PERF_H)
#define CAUSAL_RUNTIME_PERF_H

#include <linux/perf_event.h>
#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <functional>

#include "ccutil/log.h"
#include "ccutil/wrapped_array.h"

#if __GLIBC__ == 2 && __GLIBC_MINOR__ < 30
#include <sys/syscall.h>
#define gettid() syscall(SYS_gettid)
#endif

#define PERF_RECORD_MISC_LOCKWAIT	(64 << 0)
#define PERF_RECORD_MISC_SCHED		(32 << 0)
#define PERF_RECORD_MISC_IOWAIT		(16 << 0)
#define PERF_RECORD_MISC_BLOCKED	(8 << 0)

// Workaround for missing hw_breakpoint.h include file:
//   This include file just defines constants used to configure watchpoint registers.
//   This will be constant across x86 systems.
enum { HW_BREAKPOINT_X = 4};

class perf_event {
public:
  enum class record_type;
  class record;
  class sample_record;
  
  /// Default constructor
  perf_event();
  /// Open a perf_event file using the given options structure
  perf_event(struct perf_event_attr& pe, pid_t pid = 0, int cpu = -1);
  /// Move constructor
  perf_event(perf_event&& other);
  
  /// Close the perf event file and unmap the ring buffer
  ~perf_event();
  
  /// Move assignment is supported
  void operator=(perf_event&& other);
  
  /// Read event count
  uint64_t get_count() const;
  
  /// Start counting events and collecting samples
  void start();
  
  /// Stop counting events
  void stop();
  
  /// Close the perf_event file and unmap the ring buffer
  void close();
  
  /// Configure the perf_event file to deliver a signal when samples are ready to be processed
  void set_ready_signal(int sig);
  
  /// An enum class with all the available sampling data
  enum class sample : uint64_t {
    ip = PERF_SAMPLE_IP,
    pid_tid = PERF_SAMPLE_TID,
    time = PERF_SAMPLE_TIME,
    addr = PERF_SAMPLE_ADDR,
    id = PERF_SAMPLE_ID,
    stream_id = PERF_SAMPLE_STREAM_ID,
    cpu = PERF_SAMPLE_CPU,
    period = PERF_SAMPLE_PERIOD,
    
#if defined(PREF_SAMPLE_READ)
    read = PERF_SAMPLE_READ,
#else
    read = 0,
#endif

    callchain = PERF_SAMPLE_CALLCHAIN,
    raw = PERF_SAMPLE_RAW,
    
#if defined(PERF_SAMPLE_BRANCH_STACK)
    branch_stack = PERF_SAMPLE_BRANCH_STACK,
#else
    branch_stack = 0,
#endif

#if defined(PERF_SAMPLE_REGS_USER)
    regs = PERF_SAMPLE_REGS_USER,
#else
    regs = 0,
#endif

#if defined(PERF_SAMPLE_STACK_USER)
    stack = PERF_SAMPLE_STACK_USER,
#else
    stack = 0,
#endif
	weight = PERF_SAMPLE_WEIGHT,

    _end = PERF_SAMPLE_MAX
  };
  
  /// Check if this perf_event was configured to collect a type of sample data
  inline bool is_sampling(sample s) const {
    return _sample_type & static_cast<uint64_t>(s);
  }
  
  /// Get the configuration for this perf_event's read format
  inline uint64_t get_read_format() const {
    return _read_format;
  }
  
  /// An enum to distinguish types of records in the mmapped ring buffer
  enum class record_type {
    mmap = PERF_RECORD_MMAP,
    lost = PERF_RECORD_LOST,
    comm = PERF_RECORD_COMM,
    exit = PERF_RECORD_EXIT,
    throttle = PERF_RECORD_THROTTLE,
    unthrottle = PERF_RECORD_UNTHROTTLE,
    fork = PERF_RECORD_FORK,
    read = PERF_RECORD_READ,
    sample = PERF_RECORD_SAMPLE,

#if defined(PERF_RECORD_MMAP2)
    mmap2 = PERF_RECORD_MMAP2
#else
    mmap2 = 0
#endif
  };
  
  class iterator;
  
  /// A generic record type
  struct record {
    friend class perf_event::iterator;
  public:
    record_type get_type() const { return static_cast<record_type>(_header->type); }
	inline bool is_lock() const { return (_header->misc & PERF_RECORD_MISC_LOCKWAIT);}
	inline bool is_sched() const { return (_header->misc & (PERF_RECORD_MISC_SCHED));}
	inline bool is_io() const { return (_header->misc & (PERF_RECORD_MISC_IOWAIT));}
	inline bool is_blocked() const { return (_header->misc & (PERF_RECORD_MISC_BLOCKED));}
	inline bool is_blocked_any() const { return (_header->misc & (PERF_RECORD_MISC_BLOCKED | PERF_RECORD_MISC_IOWAIT | PERF_RECORD_MISC_SCHED | PERF_RECORD_MISC_LOCKWAIT));}

    inline bool is_mmap() const { return get_type() == record_type::mmap; }
    inline bool is_lost() const { return get_type() == record_type::lost; }
    inline bool is_comm() const { return get_type() == record_type::comm; }
    inline bool is_exit() const { return get_type() == record_type::exit; }
    inline bool is_throttle() const { return get_type() == record_type::throttle; }
    inline bool is_unthrottle() const { return get_type() == record_type::unthrottle; }
    inline bool is_fork() const { return get_type() == record_type::fork; }
    inline bool is_read() const { return get_type() == record_type::read; }
    inline bool is_sample() const { return get_type() == record_type::sample; }
    inline bool is_mmap2() const { return get_type() == record_type::mmap2; }
    
    uint64_t get_ip() const;
    uint64_t get_pid() const;
    uint64_t get_tid() const;
    uint64_t get_time() const;
    uint32_t get_cpu() const;
    ccutil::wrapped_array<uint64_t> get_callchain() const;
	uint64_t get_weight() const;
    
  private:
    record(const perf_event& source, struct perf_event_header* header) :
        _source(source), _header(header) {}
    
    template<perf_event::sample s, typename T=void*> T locate_field() const;
    
    const perf_event& _source;
    struct perf_event_header* _header;
  };
  
  class iterator {
  public:
    iterator(perf_event& source, struct perf_event_mmap_page* mapping) : 
        _source(source), _mapping(mapping) {
      if(mapping != nullptr) {
        _index = mapping->data_tail;
        _head = mapping->data_head;
        //__atomic_thread_fence(__ATOMIC_SEQ_CST); // Not required for thread-local perf events
      } else {
        _index = 0;
        _head = 0;
      }
    }
    
    ~iterator() {
      if(_mapping != nullptr) {
        _mapping->data_tail = _index;
      }
    }
    
    void next();
    record get();
    bool has_data() const;
    
    iterator& operator++() { next(); return *this; }
    record operator*() { return get(); }
    bool operator!=(const iterator& other) { return has_data() != other.has_data(); }
    
  private:
    perf_event& _source;
    size_t _index;
    size_t _head;
    struct perf_event_mmap_page* _mapping;
    
    // Buffer to hold the current record. Just a hack until records play nice with the ring buffer
    uint8_t _buf[4096];
  };
  
  /// Get an iterator to the beginning of the memory mapped ring buffer
  iterator begin() {
    return iterator(*this, _mapping);
  }
  
  // Get an iterator to the end of the memory mapped ring buffer
  iterator end() {
    return iterator(*this, nullptr);
  }
    
private:
  // Disallow copy and assignment
  perf_event(const perf_event&) = delete;
  void operator=(const perf_event&) = delete;
  
  // Copy data out of the mmap ring buffer
  static void copy_from_ring_buffer(struct perf_event_mmap_page* mapping,
                                    ptrdiff_t index, void* dest, size_t bytes);
  
  /// File descriptor for the perf event
  long _fd = -1;
  
  /// Memory mapped perf event region
  struct perf_event_mmap_page* _mapping = nullptr;
  
  /// The sample type from this perf_event's configuration
  uint64_t _sample_type = 0;
  /// The read format from this perf event's configuration
  uint64_t _read_format = 0;
};

#endif
