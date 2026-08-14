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
#include "code/nmethod.hpp"
#include "logging/log.hpp"
#include "memory/heap.hpp"
#include "runtime/hotCodeSampler.hpp"
#include "runtime/javaThread.inline.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/growableArray.hpp"

#if INCLUDE_JFR
#include "jfr/utilities/jfrTryLock.hpp"

using SuspendedThreadTaskTryLock = JfrMutexTryLock;
#endif

static CodeHeap* find_code_heap(address pc) {
  const GrowableArray<CodeHeap*>* nmethod_heaps = CodeCache::nmethod_heaps();
  assert(nmethod_heaps != nullptr, "CodeCache::nmethod_heaps() should not be null");
  for (CodeHeap* heap : *nmethod_heaps) {
    if (heap->contains(pc)) {
      return heap;
    }
  }
  return nullptr;
}

bool ThreadSampler::sample_all_java_threads() {
  // Collect samples for each JavaThread
  for (JavaThreadIteratorWithHandle jtiwh; JavaThread *jt = jtiwh.next(); ) {
    if (jt->is_hidden_from_external_view() ||
        jt->in_deopt_handler() ||
        (jt->thread_state() != _thread_in_native && jt->thread_state() != _thread_in_Java)) {
      continue;
    }

    GetPCTask task(jt);
    {
#if INCLUDE_JFR
      SuspendedThreadTaskTryLock try_lock(SuspendedThreadTask_lock);
      if (!try_lock.acquired()) {
        log_debug(hotcode)("Suspend lock held by JFR sampler; stopping this sampling round, will retry after %u seconds", HotCodeIntervalSeconds);
        return false;
      }
#endif
      task.run();
    }

    address pc = task.pc();
    if (pc == nullptr) {
      continue;
    }

    CodeHeap* code_heap = find_code_heap(pc);
    if (code_heap == nullptr) {
      continue;
    }

    CodeBlob* cb = code_heap->find_blob(pc);
    if (cb == nullptr || !cb->is_nmethod()) {
      continue;
    }

    if (code_heap->code_blob_type() == CodeBlobType::MethodHot) {
      _hot_sample_count++;
      _c2_sample_count++; // Only C2 compiled nmethods are put into HotCodeHeap
      continue;
    }

    nmethod* nm = cb->as_nmethod();
    if (!nm->is_compiled_by_c2() || nm->is_osr_method()) {
      continue;
    }

    _c2_sample_count++;

    bool created = false;
    int* count = _samples.put_if_absent(Candidates::nmethod_id(nm), 0, &created);
    (*count)++;
    if (created) {
      _samples.maybe_grow();
    }
  }
  return true;
}

Candidates::Candidates(ThreadSampler& sampler)
  : _hot_sample_count(sampler.hot_sample_count()), _c2_sample_count(sampler.c2_sample_count()) {
  auto func = [&](uint64_t nm_id, int count) {
    _candidates.append(Pair<uint64_t, int>(nm_id, count));
  };
  sampler.iterate_samples(func);

  log_info(hotcode)("Generated candidate list from %d samples corresponding to %d nmethods", _c2_sample_count, _candidates.length());
}

void Candidates::sort() {
  _candidates.sort(
    [](Pair<uint64_t, int>* a, Pair<uint64_t, int>* b) {
      if (a->second > b->second) return 1;
      if (a->second < b->second) return -1;
      return 0;
    }
  );
}

bool Candidates::has_candidates() {
  return !_candidates.is_empty();
}

Pair<uint64_t, int> Candidates::get_candidate() {
  assert(has_candidates(), "must not be empty");
  return _candidates.pop();
}

void Candidates::add_hot_sample_count(int count) {
  _hot_sample_count += count;
}

void Candidates::reduce_c2_sample_count(int count) {
  _c2_sample_count -= count;
}

double Candidates::get_hot_sample_percent() {
  if (_c2_sample_count <= 0) {
    return 0;
  }

  assert(_hot_sample_count <= _c2_sample_count,
         "_c2_sample_count: sampled c2 nmethods in HotCodeHeap + c2 nmethods outside HotCodeHeap");

  return 100.0 * _hot_sample_count / _c2_sample_count;
}

uint64_t Candidates::nmethod_id(nmethod* nm) {
  uint64_t offset = (uint64_t)((uintptr_t)nm - (uintptr_t)CodeCache::low_bound());
  guarantee(offset < (uint64_t)4*G, "code cache offset overflow");
  return (offset << 32) | (uint32_t)nm->compile_id();
}

uint32_t Candidates::nmethod_compile_id(uint64_t nm_id) {
  return nm_id & 0xffffffffU;
}

nmethod* Candidates::nmethod_from_id(uint64_t nm_id) {
  return (nmethod*)((uintptr_t)(nm_id >> 32) + CodeCache::low_bound());
}

#endif // COMPILER2
