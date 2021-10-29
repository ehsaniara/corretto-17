/*
 * Copyright (c) 2003, 2021, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "classfile/vmSymbols.hpp"
#include "gc/shared/collectedHeap.hpp"
#include "logging/logConfiguration.hpp"
#include "memory/heap.hpp"
#include "memory/memRegion.hpp"
#include "memory/resourceArea.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/globals.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/javaCalls.hpp"
#include "services/classLoadingService.hpp"
#include "services/lowMemoryDetector.hpp"
#include "services/management.hpp"
#include "services/memoryManager.hpp"
#include "services/memoryPool.hpp"
#include "services/memoryService.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/macros.hpp"

GrowableArray<MemoryPool*>* MemoryService::_pools_list =
  new (ResourceObj::C_HEAP, mtServiceability) GrowableArray<MemoryPool*>(init_pools_list_size, mtServiceability);
GrowableArray<MemoryManager*>* MemoryService::_managers_list =
  new (ResourceObj::C_HEAP, mtServiceability) GrowableArray<MemoryManager*>(init_managers_list_size, mtServiceability);

MemoryManager*   MemoryService::_code_cache_manager    = NULL;
GrowableArray<MemoryPool*>* MemoryService::_code_heap_pools =
    new (ResourceObj::C_HEAP, mtServiceability) GrowableArray<MemoryPool*>(init_code_heap_pools_size, mtServiceability);
MemoryPool*      MemoryService::_metaspace_pool        = NULL;
MemoryPool*      MemoryService::_compressed_class_pool = NULL;

class GcThreadCountClosure: public ThreadClosure {
 private:
  int _count;
 public:
  GcThreadCountClosure() : _count(0) {};
  void do_thread(Thread* thread);
  int count() { return _count; }
};

void GcThreadCountClosure::do_thread(Thread* thread) {
  _count++;
}

void MemoryService::set_universe_heap(CollectedHeap* heap) {
  ResourceMark rm; // For internal allocations in GrowableArray.

  GrowableArray<MemoryPool*> gc_mem_pools = heap->memory_pools();
  _pools_list->appendAll(&gc_mem_pools);

  // set the GC thread count
  GcThreadCountClosure gctcc;
  heap->gc_threads_do(&gctcc);
  int count = gctcc.count();

  GrowableArray<GCMemoryManager*> gc_memory_managers = heap->memory_managers();
  for (int i = 0; i < gc_memory_managers.length(); i++) {
    GCMemoryManager* gc_manager = gc_memory_managers.at(i);

    if (count > 0) {
      gc_manager->set_num_gc_threads(count);
    }
    gc_manager->initialize_gc_stat_info();
    _managers_list->append(gc_manager);
  }
}

void MemoryService::add_code_heap_memory_pool(CodeHeap* heap, const char* name) {
  // Create new memory pool for this heap
  MemoryPool* code_heap_pool = new CodeHeapPool(heap, name, true /* support_usage_threshold */);

  // Append to lists
  _code_heap_pools->append(code_heap_pool);
  _pools_list->append(code_heap_pool);

  if (_code_cache_manager == NULL) {
    // Create CodeCache memory manager
    _code_cache_manager = MemoryManager::get_code_cache_memory_manager();
    _managers_list->append(_code_cache_manager);
  }

  _code_cache_manager->add_pool(code_heap_pool);
}

void MemoryService::add_metaspace_memory_pools() {
  MemoryManager* mgr = MemoryManager::get_metaspace_memory_manager();

  _metaspace_pool = new MetaspacePool();
  mgr->add_pool(_metaspace_pool);
  _pools_list->append(_metaspace_pool);

  if (UseCompressedClassPointers) {
    _compressed_class_pool = new CompressedKlassSpacePool();
    mgr->add_pool(_compressed_class_pool);
    _pools_list->append(_compressed_class_pool);
  }

  _managers_list->append(mgr);
}

MemoryManager* MemoryService::get_memory_manager(instanceHandle mh) {
  for (int i = 0; i < _managers_list->length(); i++) {
    MemoryManager* mgr = _managers_list->at(i);
    if (mgr->is_manager(mh)) {
      return mgr;
    }
  }
  return NULL;
}

MemoryPool* MemoryService::get_memory_pool(instanceHandle ph) {
  for (int i = 0; i < _pools_list->length(); i++) {
    MemoryPool* pool = _pools_list->at(i);
    if (pool->is_pool(ph)) {
      return pool;
    }
  }
  return NULL;
}

void MemoryService::track_memory_usage() {
  // Track the peak memory usage
  for (int i = 0; i < _pools_list->length(); i++) {
    MemoryPool* pool = _pools_list->at(i);
    pool->record_peak_memory_usage();
  }

  // Detect low memory
  LowMemoryDetector::detect_low_memory();
}

void MemoryService::track_memory_pool_usage(MemoryPool* pool) {
  // Track the peak memory usage
  pool->record_peak_memory_usage();

  // Detect low memory
  if (LowMemoryDetector::is_enabled(pool)) {
    LowMemoryDetector::detect_low_memory(pool);
  }
}

void MemoryService::gc_begin(GCMemoryManager* manager, bool recordGCBeginTime,
                             bool recordAccumulatedGCTime,
                             bool recordPreGCUsage, bool recordPeakUsage) {

  manager->gc_begin(recordGCBeginTime, recordPreGCUsage, recordAccumulatedGCTime);

  // Track the peak memory usage when GC begins
  if (recordPeakUsage) {
    for (int i = 0; i < _pools_list->length(); i++) {
      MemoryPool* pool = _pools_list->at(i);
      pool->record_peak_memory_usage();
    }
  }
}

void MemoryService::gc_end(GCMemoryManager* manager, bool recordPostGCUsage,
                           bool recordAccumulatedGCTime,
                           bool recordGCEndTime, bool countCollection,
                           GCCause::Cause cause,
                           bool allMemoryPoolsAffected) {
  // register the GC end statistics and memory usage
  manager->gc_end(recordPostGCUsage, recordAccumulatedGCTime, recordGCEndTime,
                  countCollection, cause, allMemoryPoolsAffected);
}

bool MemoryService::set_verbose(bool verbose) {
  MutexLocker m(Management_lock);
  // verbose will be set to the previous value
  if (verbose) {
    LogConfiguration::configure_stdout(LogLevel::Info, true, LOG_TAGS(gc));
  } else {
    LogConfiguration::configure_stdout(LogLevel::Off, true, LOG_TAGS(gc));
  }
  ClassLoadingService::reset_trace_class_unloading();

  return verbose;
}

Handle MemoryService::create_MemoryUsage_obj(MemoryUsage usage, TRAPS) {
  InstanceKlass* ik = Management::java_lang_management_MemoryUsage_klass(CHECK_NH);

  JavaCallArguments args(10);
  args.push_long(usage.init_size_as_jlong());
  args.push_long(usage.used_as_jlong());
  args.push_long(usage.committed_as_jlong());
  args.push_long(usage.max_size_as_jlong());

  return JavaCalls::construct_new_instance(
                          ik,
                          vmSymbols::long_long_long_long_void_signature(),
                          &args,
                          CHECK_NH);
}

Handle MemoryService::create_PauseInfo_obj(GCPauseStatInfo info, const char* phase_prefix, TRAPS) {
  InstanceKlass* ik = Management::com_sun_management_PauseInfo_klass(CHECK_NH);
  
  Handle phase_cause_h;
  if (strlen(phase_prefix) == 0) {
    phase_cause_h = java_lang_String::create_from_str(info.get_phase_cause(), CHECK_NH);
  } else {
    char* phase_cause = NEW_C_HEAP_ARRAY(char, strlen(phase_prefix) + strlen(info.get_phase_cause()) + 1, mtGC);
    strcpy(phase_cause, phase_prefix);
    strcat(phase_cause, info.get_phase_cause());
    phase_cause_h = java_lang_String::create_from_str(phase_cause, CHECK_NH);
    FREE_C_HEAP_ARRAY(char, phase_cause);
  }

  JavaCallArguments args(14);
  args.push_long(info.get_index());
  args.push_long(info.get_start_time());
  args.push_long(info.get_end_time());
  args.push_oop(phase_cause_h);
  args.push_long(info.get_operation_start_time());
  args.push_long(info.get_operation_end_time());
  args.push_long(info.get_phase_threads());

  return JavaCalls::construct_new_instance(
                        ik,
                        vmSymbols::com_sun_management_PauseInfo_constructor_signature(),
                        &args,
                        CHECK_NH);
}

Handle MemoryService::create_ConcurrentInfo_obj(GCConcurrentStatInfo info, const char* phase_prefix, TRAPS) {
  InstanceKlass* ik = Management::com_sun_management_ConcurrentInfo_klass(CHECK_NH);
  
  Handle phase_cause_h;
  if (strlen(phase_prefix) == 0) {
    phase_cause_h = java_lang_String::create_from_str(info.get_phase_cause(), CHECK_NH);
  } else {
    char* phase_cause = NEW_C_HEAP_ARRAY(char, strlen(phase_prefix) + strlen(info.get_phase_cause()) + 1, mtGC);
    strcpy(phase_cause, phase_prefix);
    strcat(phase_cause, info.get_phase_cause());
    phase_cause_h = java_lang_String::create_from_str(phase_cause, CHECK_NH);
    FREE_C_HEAP_ARRAY(char, phase_cause);
  }

  JavaCallArguments args(10);
  args.push_long(info.get_index());
  args.push_long(info.get_start_time());
  args.push_long(info.get_end_time());
  args.push_long(info.get_phase_threads());
  args.push_oop(phase_cause_h);

  return JavaCalls::construct_new_instance(
                        ik,
                        vmSymbols::com_sun_management_ConcurrentInfo_constructor_signature(),
                        &args,
                        CHECK_NH);

}

TraceMemoryManagerStats::TraceMemoryManagerStats(GCMemoryManager* gc_memory_manager,
                                                 GCCause::Cause cause,
                                                 bool allMemoryPoolsAffected,
                                                 bool recordGCBeginTime,
                                                 bool recordPreGCUsage,
                                                 bool recordPeakUsage,
                                                 bool recordPostGCUsage,
                                                 bool recordAccumulatedGCTime,
                                                 bool recordGCEndTime,
                                                 bool countCollection) {
  initialize(gc_memory_manager, cause, allMemoryPoolsAffected,
             recordGCBeginTime, recordPreGCUsage, recordPeakUsage,
             recordPostGCUsage, recordAccumulatedGCTime, recordGCEndTime,
             countCollection);
}

// for a subclass to create then initialize an instance before invoking
// the MemoryService
void TraceMemoryManagerStats::initialize(GCMemoryManager* gc_memory_manager,
                                         GCCause::Cause cause,
                                         bool allMemoryPoolsAffected,
                                         bool recordGCBeginTime,
                                         bool recordPreGCUsage,
                                         bool recordPeakUsage,
                                         bool recordPostGCUsage,
                                         bool recordAccumulatedGCTime,
                                         bool recordGCEndTime,
                                         bool countCollection) {
  _gc_memory_manager = gc_memory_manager;
  _allMemoryPoolsAffected = allMemoryPoolsAffected;
  _recordGCBeginTime = recordGCBeginTime;
  _recordPreGCUsage = recordPreGCUsage;
  _recordPeakUsage = recordPeakUsage;
  _recordPostGCUsage = recordPostGCUsage;
  _recordAccumulatedGCTime = recordAccumulatedGCTime;
  _recordGCEndTime = recordGCEndTime;
  _countCollection = countCollection;
  _cause = cause;

  MemoryService::gc_begin(_gc_memory_manager, _recordGCBeginTime, _recordAccumulatedGCTime,
                          _recordPreGCUsage, _recordPeakUsage);
}

TraceMemoryManagerStats::~TraceMemoryManagerStats() {
  MemoryService::gc_end(_gc_memory_manager, _recordPostGCUsage, _recordAccumulatedGCTime,
                        _recordGCEndTime, _countCollection, _cause, _allMemoryPoolsAffected);
}

TraceMemoryManagerPauseStats::TraceMemoryManagerPauseStats() :
  _gc_memory_manager(NULL),
  _record_accumulated_pause_time(false),
  _record_individual_pauses(false),
  _record_duration(false),
  _record_operation_time(false),
  _cycle_pause(false)
  {}


TraceMemoryManagerPauseStats::TraceMemoryManagerPauseStats(ConcurrentGCMemoryManager* gc_memory_manager,
                        const char* phase_cause,
                        size_t phase_threads,
                        bool record_accumulated_pause_time,
                        bool count_pauses,
                        bool record_individual_pauses, 
                        bool record_duration,
                        bool record_operation_time,
                        bool record_pause_cause,
                        bool record_phase_threads,
                        bool cycle_pause) {
  initialize(gc_memory_manager, phase_cause, phase_threads, record_accumulated_pause_time, count_pauses,
             record_individual_pauses, record_duration, record_operation_time, record_pause_cause, record_phase_threads, cycle_pause);
}

void TraceMemoryManagerPauseStats::initialize(ConcurrentGCMemoryManager* gc_memory_manager,
                        const char* phase_cause,
                        size_t phase_threads,
                        bool record_accumulated_pause_time,
                        bool count_pauses,
                        bool record_individual_pauses, 
                        bool record_duration,
                        bool record_operation_time,
                        bool record_pause_cause,
                        bool record_phase_threads,
                        bool cycle_pause) {
  _gc_memory_manager = gc_memory_manager;
  _record_accumulated_pause_time = record_accumulated_pause_time;
  _record_individual_pauses = record_individual_pauses;
  _record_duration = record_duration;
  _record_operation_time = record_operation_time;
  _cycle_pause = cycle_pause;
  _gc_memory_manager->pause_begin(phase_cause, phase_threads, record_accumulated_pause_time, count_pauses, record_individual_pauses, 
                                  record_duration, record_operation_time, record_pause_cause, record_phase_threads);
}

TraceMemoryManagerPauseStats::~TraceMemoryManagerPauseStats() {
  if (_gc_memory_manager != NULL) {
    _gc_memory_manager->pause_end(_record_accumulated_pause_time, _record_individual_pauses,
                                  _record_duration, _record_operation_time, _cycle_pause);
  }
}

TraceMemoryManagerConcurrentStats::TraceMemoryManagerConcurrentStats() :
  _gc_memory_manager(NULL),
  _phase_index(0),
  _record_individual_phases(false),
  _record_duration(false)
  {}

TraceMemoryManagerConcurrentStats::TraceMemoryManagerConcurrentStats(ConcurrentGCMemoryManager* gc_memory_manager,
                          const char* phase_cause,
                          size_t phase_threads,
                          bool record_individual_phases,
                          bool record_duration,
                          bool record_pause_cause,
                          bool record_phase_threads) {
  initialize(gc_memory_manager, phase_cause, phase_threads, record_individual_phases,
             record_duration, record_pause_cause, record_phase_threads);
}

void TraceMemoryManagerConcurrentStats::initialize(ConcurrentGCMemoryManager* gc_memory_manager,
                          const char* phase_cause,
                          size_t phase_threads,
                          bool record_individual_phases,
                          bool record_duration,
                          bool record_pause_cause,
                          bool record_phase_threads) {
  _gc_memory_manager = gc_memory_manager;
  _record_individual_phases = record_individual_phases;
  _record_duration = record_duration;
  _phase_index = _gc_memory_manager->concurrent_phase_begin(phase_cause, phase_threads, record_individual_phases, 
                                                            record_duration, record_pause_cause, record_phase_threads);
}

TraceMemoryManagerConcurrentStats::~TraceMemoryManagerConcurrentStats() {
  if (_gc_memory_manager != NULL) {
    _gc_memory_manager->concurrent_phase_end(_phase_index, _record_individual_phases, _record_duration);
  }
}