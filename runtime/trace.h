/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ART_RUNTIME_TRACE_H_
#define ART_RUNTIME_TRACE_H_

#include <bitset>
#include <map>
#include <memory>
#include <ostream>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "base/atomic.h"
#include "base/locks.h"
#include "base/macros.h"
#include "base/mutex.h"
#include "base/os.h"
#include "base/safe_map.h"
#include "instrumentation.h"
#include "runtime_globals.h"

namespace unix_file {
class FdFile;
}  // namespace unix_file

namespace art {

class ArtField;
class ArtMethod;
class DexFile;
class ShadowFrame;
class Thread;

using DexIndexBitSet = std::bitset<65536>;

enum TracingMode {
  kTracingInactive,
  kMethodTracingActive,  // Trace activity synchronous with method progress.
  kSampleProfilingActive,  // Trace activity captured by sampling thread.
};
std::ostream& operator<<(std::ostream& os, TracingMode rhs);

// File format:
//     header
//     record 0
//     record 1
//     ...
//
// Header format:
//     u4  magic ('SLOW')
//     u2  version
//     u2  offset to data
//     u8  start date/time in usec
//     u2  record size in bytes (version >= 2 only)
//     ... padding to 32 bytes
//
// Record format v1:
//     u1  thread ID
//     u4  method ID | method action
//     u4  time delta since start, in usec
//
// Record format v2:
//     u2  thread ID
//     u4  method ID | method action
//     u4  time delta since start, in usec
//
// Record format v3:
//     u2  thread ID
//     u4  method ID | method action
//     u4  time delta since start, in usec
//     u4  wall time since start, in usec (when clock == "dual" only)
//
// 32 bits of microseconds is 70 minutes.
//
// All values are stored in little-endian order.

enum TraceAction {
    kTraceMethodEnter = 0x00,       // method entry
    kTraceMethodExit = 0x01,        // method exit
    kTraceUnroll = 0x02,            // method exited by exception unrolling
    // 0x03 currently unused
    kTraceMethodActionMask = 0x03,  // two bits
};

// We need 3 entries to store 64-bit timestamp counter as two 32-bit values on 32-bit architectures.
static constexpr uint32_t kNumEntriesForWallClock =
    (kRuntimePointerSize == PointerSize::k64) ? 2 : 3;
static constexpr uint32_t kNumEntriesForDualClock = kNumEntriesForWallClock + 1;

// These define offsets in bytes for the individual fields of a trace entry. These are used by the
// JITed code when storing a trace entry.
static constexpr int32_t kMethodOffsetInBytes = 0;
static constexpr int32_t kTimestampOffsetInBytes = -1 * static_cast<uint32_t>(kRuntimePointerSize);
// This is valid only for 32-bit architectures.
static constexpr int32_t kLowTimestampOffsetInBytes =
    -2 * static_cast<uint32_t>(kRuntimePointerSize);

static constexpr uintptr_t kMaskTraceAction = ~0b11;

// Class for recording event traces. Trace data is either collected
// synchronously during execution (TracingMode::kMethodTracingActive),
// or by a separate sampling thread (TracingMode::kSampleProfilingActive).
class Trace final : public instrumentation::InstrumentationListener {
 public:
  enum TraceFlag {
    kTraceCountAllocs = 0x001,
    kTraceClockSourceWallClock = 0x010,
    kTraceClockSourceThreadCpu = 0x100,
  };

  enum class TraceOutputMode {
    kFile,
    kDDMS,
    kStreaming
  };

  enum class TraceMode {
    kMethodTracing,
    kSampling
  };

  static void SetDefaultClockSource(TraceClockSource clock_source);

  static void Start(const char* trace_filename,
                    size_t buffer_size,
                    int flags,
                    TraceOutputMode output_mode,
                    TraceMode trace_mode,
                    int interval_us)
      REQUIRES(!Locks::mutator_lock_, !Locks::thread_list_lock_, !Locks::thread_suspend_count_lock_,
               !Locks::trace_lock_);
  static void Start(int trace_fd,
                    size_t buffer_size,
                    int flags,
                    TraceOutputMode output_mode,
                    TraceMode trace_mode,
                    int interval_us)
      REQUIRES(!Locks::mutator_lock_, !Locks::thread_list_lock_, !Locks::thread_suspend_count_lock_,
               !Locks::trace_lock_);
  static void Start(std::unique_ptr<unix_file::FdFile>&& file,
                    size_t buffer_size,
                    int flags,
                    TraceOutputMode output_mode,
                    TraceMode trace_mode,
                    int interval_us)
      REQUIRES(!Locks::mutator_lock_, !Locks::thread_list_lock_, !Locks::thread_suspend_count_lock_,
               !Locks::trace_lock_);
  static void StartDDMS(size_t buffer_size,
                        int flags,
                        TraceMode trace_mode,
                        int interval_us)
      REQUIRES(!Locks::mutator_lock_, !Locks::thread_list_lock_, !Locks::thread_suspend_count_lock_,
               !Locks::trace_lock_);

  // Stop tracing. This will finish the trace and write it to file/send it via DDMS.
  static void Stop()
      REQUIRES(!Locks::mutator_lock_, !Locks::thread_list_lock_, !Locks::trace_lock_);
  // Abort tracing. This will just stop tracing and *not* write/send the collected data.
  static void Abort()
      REQUIRES(!Locks::mutator_lock_, !Locks::thread_list_lock_, !Locks::trace_lock_);
  static void Shutdown()
      REQUIRES(!Locks::mutator_lock_, !Locks::thread_list_lock_, !Locks::trace_lock_);
  static TracingMode GetMethodTracingMode() REQUIRES(!Locks::trace_lock_);

  // Flush the per-thread buffer. This is called when the thread is about to detach.
  static void FlushThreadBuffer(Thread* thread) REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::trace_lock_) NO_THREAD_SAFETY_ANALYSIS;

  bool UseWallClock();
  bool UseThreadCpuClock();
  void MeasureClockOverhead();
  uint32_t GetClockOverheadNanoSeconds();

  void CompareAndUpdateStackTrace(Thread* thread, std::vector<ArtMethod*>* stack_trace)
      REQUIRES_SHARED(Locks::mutator_lock_) REQUIRES(!tracing_lock_);

  // InstrumentationListener implementation.
  void MethodEntered(Thread* thread, ArtMethod* method) REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!tracing_lock_) override;
  void MethodExited(Thread* thread,
                    ArtMethod* method,
                    instrumentation::OptionalFrame frame,
                    JValue& return_value)
      REQUIRES_SHARED(Locks::mutator_lock_) REQUIRES(!tracing_lock_)
      override;
  void MethodUnwind(Thread* thread,
                    ArtMethod* method,
                    uint32_t dex_pc)
      REQUIRES_SHARED(Locks::mutator_lock_) REQUIRES(!tracing_lock_)
      override;
  void DexPcMoved(Thread* thread,
                  Handle<mirror::Object> this_object,
                  ArtMethod* method,
                  uint32_t new_dex_pc)
      REQUIRES_SHARED(Locks::mutator_lock_) REQUIRES(!tracing_lock_)
      override;
  void FieldRead(Thread* thread,
                 Handle<mirror::Object> this_object,
                 ArtMethod* method,
                 uint32_t dex_pc,
                 ArtField* field)
      REQUIRES_SHARED(Locks::mutator_lock_) REQUIRES(!tracing_lock_) override;
  void FieldWritten(Thread* thread,
                    Handle<mirror::Object> this_object,
                    ArtMethod* method,
                    uint32_t dex_pc,
                    ArtField* field,
                    const JValue& field_value)
      REQUIRES_SHARED(Locks::mutator_lock_) REQUIRES(!tracing_lock_) override;
  void ExceptionThrown(Thread* thread,
                       Handle<mirror::Throwable> exception_object)
      REQUIRES_SHARED(Locks::mutator_lock_) REQUIRES(!tracing_lock_) override;
  void ExceptionHandled(Thread* thread, Handle<mirror::Throwable> exception_object)
      REQUIRES_SHARED(Locks::mutator_lock_) REQUIRES(!tracing_lock_) override;
  void Branch(Thread* thread,
              ArtMethod* method,
              uint32_t dex_pc,
              int32_t dex_pc_offset)
      REQUIRES_SHARED(Locks::mutator_lock_) REQUIRES(!tracing_lock_) override;
  void WatchedFramePop(Thread* thread, const ShadowFrame& frame)
      REQUIRES_SHARED(Locks::mutator_lock_) override;
  // Reuse an old stack trace if it exists, otherwise allocate a new one.
  static std::vector<ArtMethod*>* AllocStackTrace();
  // Clear and store an old stack trace for later use.
  static void FreeStackTrace(std::vector<ArtMethod*>* stack_trace);
  // Save id and name of a thread before it exits.
  static void StoreExitingThreadInfo(Thread* thread);

  static TraceOutputMode GetOutputMode() REQUIRES(!Locks::trace_lock_);
  static TraceMode GetMode() REQUIRES(!Locks::trace_lock_);
  static size_t GetBufferSize() REQUIRES(!Locks::trace_lock_);
  static int GetFlags() REQUIRES(!Locks::trace_lock_);
  static int GetIntervalInMillis() REQUIRES(!Locks::trace_lock_);

  // Used by class linker to prevent class unloading.
  static bool IsTracingEnabled() REQUIRES(!Locks::trace_lock_);

 private:
  Trace(File* trace_file,
        size_t buffer_size,
        int flags,
        TraceOutputMode output_mode,
        TraceMode trace_mode);

  // The sampling interval in microseconds is passed as an argument.
  static void* RunSamplingThread(void* arg) REQUIRES(!Locks::trace_lock_);

  static void StopTracing(bool finish_tracing, bool flush_file)
      REQUIRES(!Locks::mutator_lock_, !Locks::thread_list_lock_, !Locks::trace_lock_)
      // There is an annoying issue with static functions that create a new object and call into
      // that object that causes them to not be able to tell that we don't currently hold the lock.
      // This causes the negative annotations to incorrectly have a false positive. TODO: Figure out
      // how to annotate this.
      NO_THREAD_SAFETY_ANALYSIS;
  void FinishTracing()
      REQUIRES_SHARED(Locks::mutator_lock_) REQUIRES(!tracing_lock_);

  void ReadClocks(Thread* thread, uint32_t* thread_clock_diff, uint64_t* timestamp_counter);

  void LogMethodTraceEvent(Thread* thread,
                           ArtMethod* method,
                           TraceAction action,
                           uint32_t thread_clock_diff,
                           uint64_t timestamp_counter) REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!tracing_lock_);

  // Methods to output traced methods and threads.
  void DumpMethodList(std::ostream& os)
      REQUIRES_SHARED(Locks::mutator_lock_) REQUIRES(!tracing_lock_);
  void DumpThreadList(std::ostream& os)
      REQUIRES(!Locks::thread_list_lock_) REQUIRES(tracing_lock_);

  void RecordMethodEvent(Thread* thread,
                         ArtMethod* method,
                         TraceAction action,
                         uint32_t thread_clock_diff,
                         uint64_t timestamp) REQUIRES(!tracing_lock_);

  // Encodes event in non-streaming mode. This assumes that there is enough space reserved to
  // encode the entry.
  void EncodeEventEntry(uint8_t* ptr,
                        uint16_t thread_id,
                        uint32_t method_index,
                        TraceAction action,
                        uint32_t thread_clock_diff,
                        uint32_t wall_clock_diff) REQUIRES(tracing_lock_);

  // These methods are used to encode events in streaming mode.

  // This records the method event in the per-thread buffer if there is sufficient space for the
  // entire record. If the buffer is full then it just flushes the buffer and then records the
  // entry.
  void RecordStreamingMethodEvent(Thread* thread,
                                  ArtMethod* method,
                                  TraceAction action,
                                  uint32_t thread_clock_diff,
                                  uint64_t timestamp) REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!tracing_lock_);
  // This encodes all the events in the per-thread trace buffer and writes it to the trace file.
  // This acquires streaming lock to prevent any other threads writing concurrently. It is required
  // to serialize these since each method is encoded with a unique id which is assigned when the
  // method is seen for the first time in the recoreded events. So we need to serialize these
  // flushes across threads.
  void FlushStreamingBuffer(Thread* thread) REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!tracing_lock_);
  // Ensures there is sufficient space in the buffer to record the requested_size. If there is not
  // enough sufficient space the current contents of the buffer are written to the file and
  // current_index is reset to 0. This doesn't check if buffer_size is big enough to hold the
  // requested size.
  void EnsureSpace(uint8_t* buffer,
                   size_t* current_index,
                   size_t buffer_size,
                   size_t required_size);
  // Writes header followed by data to the buffer at the current_index. This also updates the
  // current_index to point to the next entry.
  void WriteToBuf(uint8_t* header,
                  size_t header_size,
                  const std::string& data,
                  size_t* current_index,
                  uint8_t* buffer,
                  size_t buffer_size);

  uint32_t EncodeTraceMethod(ArtMethod* method) REQUIRES(tracing_lock_);
  ArtMethod* DecodeTraceMethod(uint32_t tmid) REQUIRES(tracing_lock_);
  std::string GetMethodLine(ArtMethod* method, uint32_t method_id) REQUIRES(tracing_lock_)
      REQUIRES_SHARED(Locks::mutator_lock_);

  void DumpBuf(uint8_t* buf, size_t buf_size, TraceClockSource clock_source)
      REQUIRES_SHARED(Locks::mutator_lock_) REQUIRES(!tracing_lock_);

  void UpdateThreadsList(Thread* thread);

  uint16_t GetThreadEncoding(pid_t thread_id) REQUIRES(tracing_lock_);

  // Singleton instance of the Trace or null when no method tracing is active.
  static Trace* volatile the_trace_ GUARDED_BY(Locks::trace_lock_);

  // The default profiler clock source.
  static TraceClockSource default_clock_source_;

  // Sampling thread, non-zero when sampling.
  static pthread_t sampling_pthread_;

  // Used to remember an unused stack trace to avoid re-allocation during sampling.
  static std::unique_ptr<std::vector<ArtMethod*>> temp_stack_trace_;

  // File to write trace data out to, null if direct to ddms.
  std::unique_ptr<File> trace_file_;

  // Buffer to store trace data. In streaming mode, this is protected
  // by the tracing_lock_. In non-streaming mode, reserved regions
  // are atomically allocated (using cur_offset_) for log entries to
  // be written.
  std::unique_ptr<uint8_t[]> buf_;

  // Flags enabling extra tracing of things such as alloc counts.
  const int flags_;

  // The kind of output for this tracing.
  const TraceOutputMode trace_output_mode_;

  // The tracing method.
  const TraceMode trace_mode_;

  const TraceClockSource clock_source_;

  // Size of buf_.
  const size_t buffer_size_;

  // Time trace was created.
  const uint64_t start_time_;

  // Clock overhead.
  const uint32_t clock_overhead_ns_;

  // Offset into buf_. The field is atomic to allow multiple writers
  // to concurrently reserve space in the buffer. The newly written
  // buffer contents are not read without some other form of thread
  // synchronization, such as suspending all potential writers or
  // acquiring *tracing_lock_. Reading cur_offset_ is thus never
  // used to ensure visibility of any other objects, and all accesses
  // are memory_order_relaxed.
  //
  // All accesses to buf_ in streaming mode occur whilst holding the
  // streaming lock. In streaming mode, the buffer may be written out
  // so cur_offset_ can move forwards and backwards.
  //
  // When not in streaming mode, the buf_ writes can come from
  // multiple threads when the trace mode is kMethodTracing. When
  // trace mode is kSampling, writes only come from the sampling
  // thread.
  //
  // Reads to the buffer happen after the event sources writing to the
  // buffer have been shutdown and all stores have completed. The
  // stores are made visible in StopTracing() when execution leaves
  // the ScopedSuspendAll block.
  AtomicInteger cur_offset_;

  // Did we overflow the buffer recording traces?
  bool overflow_;

  // Map of thread ids and names. We record the information when the threads are
  // exiting and when the tracing has finished.
  SafeMap<pid_t, std::string> threads_list_;

  // Sampling profiler sampling interval.
  int interval_us_;

  // A flag to indicate to the sampling thread whether to stop tracing
  bool stop_tracing_;

  // Streaming mode data.
  Mutex tracing_lock_;

  // Map from ArtMethod* to index.
  std::unordered_map<ArtMethod*, uint32_t> art_method_id_map_ GUARDED_BY(tracing_lock_);
  uint32_t current_method_index_ = 0;

  // Map from thread_id to a 16-bit identifier.
  std::unordered_map<pid_t, uint16_t> thread_id_map_ GUARDED_BY(tracing_lock_);
  uint16_t current_thread_index_;

  DISALLOW_COPY_AND_ASSIGN(Trace);
};

}  // namespace art

#endif  // ART_RUNTIME_TRACE_H_
