/******************************************************************************
Copyright (c) 2018 Advanced Micro Devices, Inc. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*******************************************************************************/

#ifndef SRC_CORE_TRACKER_H_
#define SRC_CORE_TRACKER_H_

#include <amd_hsa_signal.h>
#include <assert.h>
#include <hsa.h>
#include <hsa_ext_amd.h>

#include <atomic>
#include <list>
#include <mutex>

#include "util/hsa_rsrc_factory.h"
#include "inc/rocprofiler.h"
#include "util/exception.h"
#include "util/logger.h"

namespace rocprofiler {

class Tracker {
  public:
  typedef std::mutex mutex_t;
  typedef util::HsaRsrcFactory::timestamp_t timestamp_t;
  typedef rocprofiler_dispatch_record_t record_t;
  struct entry_t;
  typedef std::list<entry_t*> sig_list_t;
  typedef sig_list_t::iterator sig_list_it_t;

  struct entry_t {
    Tracker* tracker;
    sig_list_t::iterator it;
    hsa_agent_t agent;
    hsa_signal_t orig;
    hsa_signal_t signal;
    record_t* record;
    std::atomic<void*> handler;
    void* arg;
    bool context_active;
  };

  Tracker() :
    outstanding_(0),
    hsa_rsrc_(&(util::HsaRsrcFactory::Instance()))
  {}

  ~Tracker() {
    auto it = sig_list_.begin();
    auto end = sig_list_.end();
    while (it != end) {
      auto cur = it++;
      hsa_rsrc_->SignalWait((*cur)->signal);
      Erase(cur);
    }
  }

  // Add tracker entry
  entry_t* Alloc(const hsa_agent_t& agent, const hsa_signal_t& orig) {
    hsa_status_t status = HSA_STATUS_ERROR;

    // Creating a new tracker entry
    entry_t* entry = new entry_t{};
    assert(entry);
    entry->tracker = this;
    entry->agent = agent;
    entry->orig = orig;

    // Creating a record with the dispatch timestamps
    record_t* record = new record_t{};
    assert(record);
    record->dispatch = hsa_rsrc_->TimestampNs();
    entry->record = record;

    // Creating a proxy signal
    status = hsa_signal_create(1, 0, NULL, &(entry->signal));
    if (status != HSA_STATUS_SUCCESS) EXC_RAISING(status, "hsa_signal_create");
    status = hsa_amd_signal_async_handler(entry->signal, HSA_SIGNAL_CONDITION_LT, 1, Handler, entry);
    if (status != HSA_STATUS_SUCCESS) EXC_RAISING(status, "hsa_amd_signal_async_handler");

    // Adding antry to the list
    mutex_.lock();
    entry->it = sig_list_.insert(sig_list_.begin(), entry);
    mutex_.unlock();

    return entry;
  }

  // Delete tracker entry
  void Delete(entry_t* entry) {
    hsa_signal_destroy(entry->signal);
    mutex_.lock();
    sig_list_.erase(entry->it);
    mutex_.unlock();
    delete entry;
  }

  // Enable tracker entry
  void Enable(entry_t* entry, void* handler, void* arg) {
    // Set entry handler and release the entry
    entry->arg = arg;
    entry->handler.store(handler, std::memory_order_release);

    // Debug trace
    if (trace_on_) {
      auto outstanding = outstanding_.fetch_add(1);
      fprintf(stdout, "Tracker::Add: entry %p, record %p, outst %lu\n", entry, entry->record, outstanding);
      fflush(stdout);
    }
  }

  void Enable(entry_t* entry, hsa_amd_signal_handler handler, void* arg) {
    entry->context_active = true;
    Enable(entry, reinterpret_cast<void*>(handler), arg);
  }
  void Enable(entry_t* entry, rocprofiler_handler_t handler, void* arg) {
    Enable(entry, reinterpret_cast<void*>(handler), arg);
  }

  private:
  // Delete an entry by iterator
  void Erase(const sig_list_it_t& it) { Delete(*it); }

  // Entry completion
  void Complete(entry_t* entry) {
    record_t* record = entry->record;

    // Debug trace
    if (trace_on_) {
      auto outstanding = outstanding_.fetch_sub(1);
      fprintf(stdout, "Tracker::Handler: entry %p, record %p, outst %lu\n", entry, entry->record, outstanding);
      fflush(stdout);
    }

    // Query begin/end and complete timestamps
    hsa_amd_profiling_dispatch_time_t dispatch_time{};
    hsa_status_t status = hsa_amd_profiling_get_dispatch_time(entry->agent, entry->signal, &dispatch_time);
    if (status != HSA_STATUS_SUCCESS) EXC_RAISING(status, "hsa_amd_profiling_get_dispatch_time");

    record->begin = hsa_rsrc_->SysclockToNs(dispatch_time.start);
    record->end = hsa_rsrc_->SysclockToNs(dispatch_time.end);
    record->complete = hsa_rsrc_->TimestampNs();

    // Original intercepted signal completion
    hsa_signal_t orig = entry->orig;
    if (orig.handle) {
      amd_signal_t* orig_signal_ptr = reinterpret_cast<amd_signal_t*>(orig.handle);
      amd_signal_t* prof_signal_ptr = reinterpret_cast<amd_signal_t*>(entry->signal.handle);
      orig_signal_ptr->start_ts = prof_signal_ptr->start_ts;
      orig_signal_ptr->end_ts = prof_signal_ptr->end_ts;

      const hsa_signal_value_t value = hsa_signal_load_relaxed(orig);
      hsa_signal_store_screlease(orig, value - 1);
    }
  }

  // Handler for packet completion
  static bool Handler(hsa_signal_value_t, void* arg) {
    // Acquire entry
    entry_t* entry = reinterpret_cast<entry_t*>(arg);
    volatile std::atomic<void*>* ptr = &entry->handler;
    while (ptr->load(std::memory_order_acquire) == NULL) sched_yield();

    // Complete entry
    entry->tracker->Complete(entry);

    // Call entry handler
    void* handler = static_cast<void*>(entry->handler);
    if (entry->context_active) {
      reinterpret_cast<hsa_amd_signal_handler>(handler)(0, entry->arg);
    } else {
      rocprofiler_group_t group{};
      reinterpret_cast<rocprofiler_handler_t>(handler)(group, entry->arg);
    }

    // Delete tracker entry
    entry->tracker->Delete(entry);

    return false;
  }

  // Tracked signals list
  sig_list_t sig_list_;
  // Inter-thread synchronization
  mutex_t mutex_;
  // Outstanding dispatches
  std::atomic<uint64_t> outstanding_;
  // HSA resources factory
  util::HsaRsrcFactory* hsa_rsrc_;
  // Enable tracing
  static const bool trace_on_ = false;
};

} // namespace rocprofiler

#endif // SRC_CORE_TRACKER_H_
