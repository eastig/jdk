/*
 * Copyright Amazon.com Inc. or its affiliates. All Rights Reserved.
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

#ifdef COMPILER2

#include "code/codeCache.hpp"
#include "code/compiledIC.hpp"
#include "code/nmethod.hpp"
#include "compiler/compilerDefinitions.inline.hpp"
#include "logging/log.hpp"
#include "memory/resourceArea.hpp"
#include "oops/method.inline.hpp"
#include "runtime/hotCodeCollector.hpp"
#include "runtime/hotCodeSampler.hpp"
#include "runtime/java.hpp"
#include "runtime/javaThread.inline.hpp"

#include <cstdint>

// Initialize static variables
bool      HotCodeCollector::_is_initialized = false;
int       HotCodeCollector::_new_c2_nmethods_count = 0;
int       HotCodeCollector::_total_c2_nmethods_count = 0;

HotCodeCollector::HotCodeCollector() : JavaThread(thread_entry) {}

void HotCodeCollector::initialize() {
  EXCEPTION_MARK;

  assert(HotCodeHeap, "HotCodeCollector requires HotCodeHeap enabled");
  assert(CompilerConfig::is_c2_enabled(), "HotCodeCollector requires C2 enabled");
  assert(NMethodRelocation, "HotCodeCollector requires NMethodRelocation enabled");
  assert(HotCodeHeapSize > 0, "HotCodeHeapSize must be non-zero to use HotCodeCollector");
  assert(CodeCache::get_code_heap(CodeBlobType::MethodHot) != nullptr, "MethodHot code heap not found");

  Handle thread_oop = JavaThread::create_system_thread_object("HotCodeCollectorThread", CHECK);
  HotCodeCollector* thread = new HotCodeCollector();
  JavaThread::vm_exit_on_osthread_failure(thread);
  JavaThread::start_internal_daemon(THREAD, thread, thread_oop, NormPriority);

  _is_initialized = true;
}

bool HotCodeCollector::is_nmethod_count_stable() {
  if (HotCodeStablePercent < 0) {
    log_info(hotcode)("HotCodeStablePercent is less than zero, stable check disabled");
    return true;
  }

  MutexLocker ml_CodeCache_lock(CodeCache_lock, Mutex::_no_safepoint_check_flag);

  if (_total_c2_nmethods_count <= 0) {
    log_info(hotcode)("No registered C2 nmethods");
    return false;
  }

  const double percent_new = 100.0 * _new_c2_nmethods_count / _total_c2_nmethods_count;
  bool is_stable_nmethod_count = percent_new <= HotCodeStablePercent;

  log_info(hotcode)("C2 nmethod count %s", is_stable_nmethod_count ? "stable" : "not stable");
  log_debug(hotcode)("C2 nmethod stats: New: %d, Total: %d, Percent new: %f", _new_c2_nmethods_count, _total_c2_nmethods_count, percent_new);

  _new_c2_nmethods_count = 0;

  return is_stable_nmethod_count;
}

void HotCodeCollector::thread_entry(JavaThread* thread, TRAPS) {
  // Initial sleep to allow JVM to warm up
  thread->sleep(HotCodeStartupDelaySeconds * 1000);

  while (true) {
    ResourceMark rm;

    // Sample application and group hot nmethods if nmethod count is stable
    if (is_nmethod_count_stable()) {
      log_info(hotcode)("Sampling...");

      ThreadSampler sampler;
      uint64_t start_time = os::javaTimeMillis();
      while (os::javaTimeMillis() - start_time <= HotCodeSampleSeconds * 1000) {
        if (!sampler.sample_all_java_threads()) {
          break;
        }
        thread->sleep(rand_sampling_period_ms());
      }

      Candidates candidates(sampler);
      do_grouping(candidates);
    }

    thread->sleep(HotCodeIntervalSeconds * 1000);
  }
}

static nmethod* find_nmethod(void* addr) {
  if (addr == nullptr) {
    return nullptr;
  }
  CodeBlob* blob = CodeCache::find_blob(addr);
  return blob == nullptr ? nullptr : blob->as_nmethod_or_null();
}

void HotCodeCollector::do_grouping(Candidates& candidates) {
  // Number of nmethods relocated (candidate + callees)
  int num_relocated = 0;

  // Sort nmethods by increasing sample count so pop() returns the hottest
  candidates.sort();

  while (candidates.has_candidates()) {

    double percent_from_hot = candidates.get_hot_sample_percent();
    log_debug(hotcode)("Percentage of samples from hot code heap: %f", percent_from_hot);
    if (percent_from_hot >= HotCodeSamplePercent) {
      log_info(hotcode)("Percentage of samples from hot nmethods over threshold. Done collecting hot code");
      break;
    }

    Pair<uint64_t, int> candidate = candidates.get_candidate();

    MutexLocker ml_Compile_lock(Compile_lock);
    MutexLocker ml_CompiledIC_lock(CompiledIC_lock, Mutex::_no_safepoint_check_flag);
    MutexLocker ml_CodeCache_lock(CodeCache_lock, Mutex::_no_safepoint_check_flag);

    address nm_addr = (address)Candidates::nmethod_from_id(candidate.first);
    nmethod* nm = find_nmethod(nm_addr);
    uint32_t compile_id = Candidates::nmethod_compile_id(candidate.first);
    int sample_count = candidate.second;
    if (nm == nullptr ||
        (address)nm != nm_addr ||
        (uint32_t)nm->compile_id() != compile_id) {
      log_debug(hotcode)("Skipped stale candidate: address=%p, compile_id=%u",
                         nm_addr, compile_id);
      candidates.reduce_c2_sample_count(sample_count);
      continue;
    }

    switch (do_relocation(nm, 0, &num_relocated)) {
      case RelocationResult::Success:
        candidates.add_hot_sample_count(sample_count);
        break;
      case RelocationResult::InvalidNmethod:
        candidates.reduce_c2_sample_count(sample_count);
        break;
      case RelocationResult::Failed:
      case RelocationResult::NoSpaceInCodeHeap:
        break;
    }
  }

  log_info(hotcode)("Collection done. Relocated %d nmethods to the MethodHot heap", num_relocated);
}

HotCodeCollector::RelocationResult HotCodeCollector::do_relocation(nmethod* nm, uint call_level, int *num_relocated) {
  if (nm == nullptr ||
      nm->is_osr_method() ||
      nm->method() == nullptr) {
    return RelocationResult::InvalidNmethod;
  }

  assert(num_relocated != nullptr, "num_relocated must be provided");

  // The candidate may have been recompiled or already relocated.
  // Retrieve the latest nmethod from the Method
  nm = nm->method()->code();

  // Verify the nmethod is still valid for relocation
  if (nm == nullptr || !nm->is_in_use() || !nm->is_compiled_by_c2()) {
    return RelocationResult::InvalidNmethod;
  }

  // Pointer to nmethod in hot heap
  nmethod* hot_nm = nullptr;

  if (CodeCache::get_code_blob_type(nm) != CodeBlobType::MethodHot) {
    if (!nm->is_relocatable()) {
      return RelocationResult::InvalidNmethod;
    }

    // Verify code heap has space
    CodeHeap* hot_heap = CodeCache::get_code_heap(CodeBlobType::MethodHot);
    if (hot_heap->unallocated_capacity() < (size_t)nm->size()) {
      log_info(hotcode)("Not enough free space in MethodHot heap (%zd bytes) to relocate nm (%d bytes). Skipping nmethod.",
        hot_heap->unallocated_capacity(), nm->size());
      return RelocationResult::NoSpaceInCodeHeap;
    }

    CompiledICLocker ic_locker(nm);
    hot_nm = nm->relocate(CodeBlobType::MethodHot);

    if (hot_nm != nullptr) {
      // Successfully relocated nmethod. Update counts and proceed to callee relocation.
      log_debug(hotcode)("Successful relocation: nmethod (%p), method (%s), call level (%d)", nm, hot_nm->method()->name_and_sig_as_C_string(), call_level);
      (*num_relocated)++;
    } else {
      // Relocation failed so return and do not attempt to relocate callees
      log_debug(hotcode)("Failed relocation: nmethod (%p), call level (%d)", nm, call_level);
      return nm->is_relocatable() ? RelocationResult::Failed : RelocationResult::InvalidNmethod;
    }
  } else {
    // Skip relocation since already in hot heap, but still relocate callees
    // since they may not have been compiled when this method was first relocated
    log_debug(hotcode)("Already relocated: nmethod (%p), method (%s), call level (%d)", nm, nm->method()->name_and_sig_as_C_string(), call_level);
    hot_nm = nm;
  }

  assert(hot_nm != nullptr, "unable to relocate callees");

  if (call_level < HotCodeCallLevel) {
    // Loop over relocations to relocate callees
    RelocIterator relocIter(hot_nm);
    while (relocIter.next()) {
      // Check if this is a call
      Relocation* reloc = relocIter.reloc();
      if (!reloc->is_call()) {
        continue;
      }

      // Find the call destination address
      address dest = ((CallRelocation*) reloc)->destination();

      // Recursively relocate callees
      do_relocation(find_nmethod(dest), call_level + 1, num_relocated);
    }
  }

  return RelocationResult::Success;
}

void HotCodeCollector::unregister_nmethod(nmethod* nm) {
  assert_lock_strong(CodeCache_lock);
  if (!_is_initialized) {
    return;
  }

  if (!nm->is_compiled_by_c2()) {
    return;
  }

  if (CodeCache::get_code_blob_type(nm) == CodeBlobType::MethodHot) {
    // Nmethods in the hot code heap do not count towards total C2 nmethods.
    return;
  }

  // CodeCache_lock is held, so we can safely decrement the count.
  _total_c2_nmethods_count--;
}

void HotCodeCollector::register_nmethod(nmethod* nm) {
  assert_lock_strong(CodeCache_lock);
  if (!_is_initialized) {
    return;
  }

  if (!nm->is_compiled_by_c2()) {
    return; // Only C2 nmethods are relocated to HotCodeHeap.
  }

  if (CodeCache::get_code_blob_type(nm) == CodeBlobType::MethodHot) {
    // Nmethods in the hot code heap do not count towards total C2 nmethods.
    return;
  }

  // CodeCache_lock is held, so we can safely increment the count.
  _new_c2_nmethods_count++;
  _total_c2_nmethods_count++;
}
#endif // COMPILER2
