/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-2014 Facebook, Inc. (http://www.facebook.com)     |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#include "hphp/runtime/base/memory-profile.h"
#include "hphp/runtime/base/pprof-server.h"
#include "hphp/runtime/base/typed-value.h"
#include "hphp/runtime/vm/jit/translator-inline.h"
#include "hphp/runtime/base/mixed-array-defs.h"

namespace HPHP {

TRACE_SET_MOD(heap);

IMPLEMENT_THREAD_LOCAL(MemoryProfile, MemoryProfile::s_memory_profile);

// stack trace helper
static ProfileStackTrace getStackTrace() {
  ProfileStackTrace trace;

  if (g_context.isNull()) return trace;
  JIT::VMRegAnchor _;
  ActRec *fp = g_context->getFP();
  if (!fp) return trace;
  PC pc = g_context->getPC();

  const Func *f = fp->m_func;
  Unit *u = f->unit();
  Offset off = pc - u->entry();
  for (;;) {
    trace.push_back({ f, off, fp->inGenerator() });
    fp = g_context->getPrevVMState(fp, &off);
    if (!fp) break;
    f = fp->m_func;
  }
  return trace;
}

void MemoryProfile::startProfilingImpl() {
  if (RuntimeOption::ClientExecutionMode() &&
      RuntimeOption::HHProfServerProfileClientMode) {
    HeapProfileServer::Server = std::make_shared<HeapProfileServer>();
    ProfileController::requestNext(ProfileType::Default);
  }

  if (!ProfileController::isProfiling()) {
    return;
  }

  TRACE(1, "request started: initializing memory profile\n");
  m_active = true;
  m_livePointers.clear();
  m_dump.clear();
}

void MemoryProfile::finishProfilingImpl() {
  if (!m_active) { return; }

  TRACE(1, "request ended\n");

  TRACE(2, "offering dump to profile controller");

  ProfileController::offerProfile(m_dump);
  if (RuntimeOption::ClientExecutionMode() &&
      RuntimeOption::HHProfServerProfileClientMode) {
    HeapProfileServer::waitForPProf();
  }
}

void MemoryProfile::logAllocationImpl(void *ptr, size_t size) {
  if (!m_active) { return; }

  TRACE(3, "logging allocation at %p of %lu bytes\n", ptr, size);
  ProfileStackTrace trace = getStackTrace();

  Allocation alloc { size, trace };
  m_livePointers[ptr] = alloc;

  m_dump.addAlloc(size, trace);
}

void MemoryProfile::logDeallocationImpl(void *ptr) {
  if (!m_active) { return; }

  TRACE(3, "logging deallocation at %p\n", ptr);
  const auto &it = m_livePointers.find(ptr);
  if (it == m_livePointers.end()) return;

  auto profileType = ProfileController::profileType();
  if (profileType == ProfileType::Heap ||
      (profileType == ProfileType::Default &&
      !RuntimeOption::HHProfServerAllocationProfile)) {
    const Allocation &alloc = it->second;
    m_dump.removeAlloc(alloc.m_size, alloc.m_trace);
  }

  // Even if we are doing an allocation profile, this list is only used for
  // providing information about reachable pointers so we still want to remove
  // deallocated userland structures from the list.
  m_livePointers.erase(it);
}

// static
size_t MemoryProfile::getSizeOfPtr(void* ptr) {
  if (!RuntimeOption::HHProfServerEnabled) return 0;
  const MemoryProfile &mp = *s_memory_profile;

  const auto &allocIt = mp.m_livePointers.find(ptr);
  return allocIt != mp.m_livePointers.end() ? allocIt->second.m_size : 0;
}

// static
size_t MemoryProfile::getSizeOfTV(const TypedValue* tv) {
  if (!RuntimeOption::HHProfServerEnabled) return 0;

  switch (tv->m_type) {
  case KindOfString:
    return getSizeOfPtr(tv->m_data.pstr);
  case KindOfArray:
    return getSizeOfArray(tv->m_data.parr);
  case KindOfObject:
    return getSizeOfObject(tv->m_data.pobj);
  case KindOfRef:
    return getSizeOfPtr(tv->m_data.pref);
  default:
    return 0;
  }
}

// static
size_t MemoryProfile::getSizeOfArray(ArrayData* arr) {
  // Some non-flat array types have extra memory (e.g. the local TV
  // cache of an apc local array), but we haven't implemented this.
  return getSizeOfPtr(arr);
}

// static
size_t MemoryProfile::getSizeOfObject(ObjectData *obj) {
  auto ret = getSizeOfPtr(obj);
  if (ret == 0) { return 0; }
  if (UNLIKELY(obj->getAttribute(ObjectData::HasDynPropArr))) {
    auto& props = obj->dynPropArray();
    ret += getSizeOfArray(props.get());
  }
  return ret;
}

}
