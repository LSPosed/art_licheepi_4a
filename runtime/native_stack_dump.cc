/*
 * Copyright (C) 2016 The Android Open Source Project
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

#include "native_stack_dump.h"

#include <memory>
#include <ostream>
#include <string_view>

#include <stdio.h>

#include "art_method.h"

// For DumpNativeStack.
#include <unwindstack/AndroidUnwinder.h>

#if defined(__linux__)

#include <vector>

#include <linux/unistd.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/types.h>

#include "android-base/file.h"
#include "android-base/stringprintf.h"
#include "android-base/strings.h"

#include "arch/instruction_set.h"
#include "base/aborting.h"
#include "base/bit_utils.h"
#include "base/file_utils.h"
#include "base/memory_tool.h"
#include "base/mutex.h"
#include "base/os.h"
#include "base/unix_file/fd_file.h"
#include "base/utils.h"
#include "class_linker.h"
#include "entrypoints/runtime_asm_entrypoints.h"
#include "oat_quick_method_header.h"
#include "runtime.h"
#include "thread-current-inl.h"

#endif

namespace art {

#if defined(__linux__)

using android::base::StringPrintf;

static constexpr bool kUseAddr2line = !kIsTargetBuild;

std::string FindAddr2line() {
#if !defined(ART_TARGET) && !defined(ART_CLANG_PATH)
  #error "ART_CLANG_PATH must be defined on host build"
#endif
#if defined(ART_CLANG_PATH)
  const char* env_value = getenv("ANDROID_BUILD_TOP");
  std::string_view top(env_value != nullptr ? env_value : ".");
  return std::string(top) + "/" + ART_CLANG_PATH + "/bin/llvm-addr2line";
#else
  return std::string("llvm-addr2line");
#endif
}

ALWAYS_INLINE
static inline void WritePrefix(std::ostream& os, const char* prefix, bool odd) {
  if (prefix != nullptr) {
    os << prefix;
  }
  os << "  ";
  if (!odd) {
    os << " ";
  }
}

// The state of an open pipe to addr2line. In "server" mode, addr2line takes input on stdin
// and prints the result to stdout. This struct keeps the state of the open connection.
struct Addr2linePipe {
  Addr2linePipe(int in_fd, int out_fd, const std::string& file_name, pid_t pid)
      : in(in_fd, false), out(out_fd, false), file(file_name), child_pid(pid), odd(true) {}

  ~Addr2linePipe() {
    kill(child_pid, SIGKILL);
  }

  File in;      // The file descriptor that is connected to the output of addr2line.
  File out;     // The file descriptor that is connected to the input of addr2line.

  const std::string file;     // The file addr2line is working on, so that we know when to close
                              // and restart.
  const pid_t child_pid;      // The pid of the child, which we should kill when we're done.
  bool odd;                   // Print state for indentation of lines.
};

static std::unique_ptr<Addr2linePipe> Connect(const std::string& name, const char* args[]) {
  int caller_to_addr2line[2];
  int addr2line_to_caller[2];

  if (pipe(caller_to_addr2line) == -1) {
    return nullptr;
  }
  if (pipe(addr2line_to_caller) == -1) {
    close(caller_to_addr2line[0]);
    close(caller_to_addr2line[1]);
    return nullptr;
  }

  pid_t pid = fork();
  if (pid == -1) {
    close(caller_to_addr2line[0]);
    close(caller_to_addr2line[1]);
    close(addr2line_to_caller[0]);
    close(addr2line_to_caller[1]);
    return nullptr;
  }

  if (pid == 0) {
    dup2(caller_to_addr2line[0], STDIN_FILENO);
    dup2(addr2line_to_caller[1], STDOUT_FILENO);

    close(caller_to_addr2line[0]);
    close(caller_to_addr2line[1]);
    close(addr2line_to_caller[0]);
    close(addr2line_to_caller[1]);

    execv(args[0], const_cast<char* const*>(args));
    exit(1);
  } else {
    close(caller_to_addr2line[0]);
    close(addr2line_to_caller[1]);
    return std::make_unique<Addr2linePipe>(addr2line_to_caller[0],
                                           caller_to_addr2line[1],
                                           name,
                                           pid);
  }
}

static void Drain(size_t expected,
                  const char* prefix,
                  std::unique_ptr<Addr2linePipe>* pipe /* inout */,
                  std::ostream& os) {
  DCHECK(pipe != nullptr);
  DCHECK(pipe->get() != nullptr);
  int in = pipe->get()->in.Fd();
  DCHECK_GE(in, 0);

  bool prefix_written = false;

  for (;;) {
    constexpr uint32_t kWaitTimeExpectedMilli = 500;
    constexpr uint32_t kWaitTimeUnexpectedMilli = 50;

    int timeout = expected > 0 ? kWaitTimeExpectedMilli : kWaitTimeUnexpectedMilli;
    struct pollfd read_fd{in, POLLIN, 0};
    int retval = TEMP_FAILURE_RETRY(poll(&read_fd, 1, timeout));
    if (retval == -1) {
      // An error occurred.
      pipe->reset();
      return;
    }

    if (retval == 0) {
      // Timeout.
      return;
    }

    if (!(read_fd.revents & POLLIN)) {
      // addr2line call exited.
      pipe->reset();
      return;
    }

    constexpr size_t kMaxBuffer = 128;  // Relatively small buffer. Should be OK as we're on an
    // alt stack, but just to be sure...
    char buffer[kMaxBuffer];
    memset(buffer, 0, kMaxBuffer);
    int bytes_read = TEMP_FAILURE_RETRY(read(in, buffer, kMaxBuffer - 1));
    if (bytes_read <= 0) {
      // This should not really happen...
      pipe->reset();
      return;
    }
    buffer[bytes_read] = '\0';

    char* tmp = buffer;
    while (*tmp != 0) {
      if (!prefix_written) {
        WritePrefix(os, prefix, (*pipe)->odd);
        prefix_written = true;
      }
      char* new_line = strchr(tmp, '\n');
      if (new_line == nullptr) {
        os << tmp;

        break;
      } else {
        char saved = *(new_line + 1);
        *(new_line + 1) = 0;
        os << tmp;
        *(new_line + 1) = saved;

        tmp = new_line + 1;
        prefix_written = false;
        (*pipe)->odd = !(*pipe)->odd;

        if (expected > 0) {
          expected--;
        }
      }
    }
  }
}

static void Addr2line(const std::string& map_src,
                      uintptr_t offset,
                      std::ostream& os,
                      const char* prefix,
                      std::unique_ptr<Addr2linePipe>* pipe /* inout */) {
  std::array<const char*, 3> kIgnoreSuffixes{ ".dex", ".jar", ".vdex" };
  for (const char* ignore_suffix : kIgnoreSuffixes) {
    if (android::base::EndsWith(map_src, ignore_suffix)) {
      // Ignore file names that do not have map information addr2line can consume. e.g. vdex
      // files are special frames injected for the interpreter so they don't have any line
      // number information available.
      return;
    }
  }
  if (map_src == "[vdso]") {
    // addr2line will not work on the vdso.
    return;
  }

  if (*pipe == nullptr || (*pipe)->file != map_src) {
    if (*pipe != nullptr) {
      Drain(0, prefix, pipe, os);
    }
    pipe->reset();  // Close early.

    std::string addr2linePath = FindAddr2line();
    const char* args[7] = {
        addr2linePath.c_str(),
        "--functions",
        "--inlines",
        "--demangle",
        "-e",
        map_src.c_str(),
        nullptr
    };
    *pipe = Connect(map_src, args);
  }

  Addr2linePipe* pipe_ptr = pipe->get();
  if (pipe_ptr == nullptr) {
    // Failed...
    return;
  }

  // Send the offset.
  const std::string hex_offset = StringPrintf("0x%zx\n", offset);

  if (!pipe_ptr->out.WriteFully(hex_offset.data(), hex_offset.length())) {
    // Error. :-(
    pipe->reset();
    return;
  }

  // Now drain (expecting two lines).
  Drain(2U, prefix, pipe, os);
}

static bool RunCommand(const std::string& cmd) {
  FILE* stream = popen(cmd.c_str(), "r");
  if (stream) {
    // Consume the stdout until we encounter EOF when the tool exits.
    // Otherwise the tool would complain to stderr when the stream is closed.
    char buffer[64];
    while (fread(buffer, 1, sizeof(buffer), stream) == sizeof(buffer)) {}
    pclose(stream);
    return true;
  } else {
    return false;
  }
}

// Remove method parameters by finding matching top-level parenthesis and removing them.
// Since functions can be defined inside functions, this can remove multiple substrings.
std::string StripParameters(std::string name) {
  size_t end = name.size();
  int nesting = 0;
  for (ssize_t i = name.size() - 1; i > 0; i--) {
    if (name[i] == ')' && nesting++ == 0) {
      end = i + 1;
    }
    if (name[i] == '(' && --nesting == 0) {
      name = name.erase(i, end - i);
    }
  }
  return name;
}

void DumpNativeStack(std::ostream& os,
                     pid_t tid,
                     const char* prefix,
                     ArtMethod* current_method,
                     void* ucontext_ptr,
                     bool skip_frames) {
  unwindstack::AndroidLocalUnwinder unwinder;
  DumpNativeStack(os, unwinder, tid, prefix, current_method, ucontext_ptr, skip_frames);
}

void DumpNativeStack(std::ostream& os,
                     unwindstack::AndroidLocalUnwinder& unwinder,
                     pid_t tid,
                     const char* prefix,
                     ArtMethod* current_method,
                     void* ucontext_ptr,
                     bool skip_frames) {
  // Historical note: This was disabled when running under Valgrind (b/18119146).

  unwindstack::AndroidUnwinderData data(!skip_frames /*show_all_frames*/);
  bool unwind_ret;
  if (ucontext_ptr != nullptr) {
    unwind_ret = unwinder.Unwind(ucontext_ptr, data);
  } else {
    unwind_ret = unwinder.Unwind(tid, data);
  }
  if (!unwind_ret) {
    os << prefix << "(Unwind failed for thread " << tid << ": "
       <<  data.GetErrorString() << ")" << std::endl;
    return;
  }

  // Check whether we have and should use addr2line.
  bool use_addr2line;
  if (kUseAddr2line) {
    // Try to run it to see whether we have it. Push an argument so that it doesn't assume a.out
    // and print to stderr.
    use_addr2line = (gAborting > 0) && RunCommand(FindAddr2line() + " -h");
  } else {
    use_addr2line = false;
  }

  std::unique_ptr<Addr2linePipe> addr2line_state;
  data.DemangleFunctionNames();
  bool holds_mutator_lock =  Locks::mutator_lock_->IsSharedHeld(Thread::Current());
  for (const unwindstack::FrameData& frame : data.frames) {
    // We produce output like this:
    // ]    #00 pc 000075bb8  /system/lib/libc.so (unwind_backtrace_thread+536)
    // In order for parsing tools to continue to function, the stack dump
    // format must at least adhere to this format:
    //  #XX pc <RELATIVE_ADDR>  <FULL_PATH_TO_SHARED_LIBRARY> ...
    // The parsers require a single space before and after pc, and two spaces
    // after the <RELATIVE_ADDR>. There can be any prefix data before the
    // #XX. <RELATIVE_ADDR> has to be a hex number but with no 0x prefix.
    os << prefix << StringPrintf("#%02zu pc ", frame.num);
    bool try_addr2line = false;
    if (frame.map_info == nullptr) {
      os << StringPrintf("%08" PRIx64 "  ???", frame.pc);
    } else {
      os << StringPrintf("%08" PRIx64 "  ", frame.rel_pc);
      const std::shared_ptr<unwindstack::MapInfo>& map_info = frame.map_info;
      if (map_info->name().empty()) {
        os << StringPrintf("<anonymous:%" PRIx64 ">", map_info->start());
      } else {
        os << map_info->name().c_str();
      }
      if (map_info->elf_start_offset() != 0) {
        os << StringPrintf(" (offset %" PRIx64 ")", map_info->elf_start_offset());
      }
      os << " (";
      if (!frame.function_name.empty()) {
        // Remove parameters from the printed function name to improve signal/noise in the logs.
        // Also, ANRs are often trimmed, so printing less means we get more useful data out.
        // We can still symbolize the function based on the PC and build-id (including inlining).
        os << StripParameters(frame.function_name.c_str());
        if (frame.function_offset != 0) {
          os << "+" << frame.function_offset;
        }
        // Functions found using the gdb jit interface will be in an empty
        // map that cannot be found using addr2line.
        if (!map_info->name().empty()) {
          try_addr2line = true;
        }
      } else if (current_method != nullptr && holds_mutator_lock) {
        const OatQuickMethodHeader* header = current_method->GetOatQuickMethodHeader(frame.pc);
        if (header != nullptr) {
          const void* start_of_code = header->GetCode();
          os << current_method->JniLongName() << "+"
             << (frame.pc - reinterpret_cast<uint64_t>(start_of_code));
        } else {
          os << "???";
        }
      } else {
        os << "???";
      }
      os << ")";
      std::string build_id = map_info->GetPrintableBuildID();
      if (!build_id.empty()) {
        os << " (BuildId: " << build_id << ")";
      }
    }
    os << std::endl;
    if (try_addr2line && use_addr2line) {
      // Guaranteed that map_info is not nullptr and name is non-empty.
      Addr2line(frame.map_info->name(), frame.rel_pc, os, prefix, &addr2line_state);
    }
  }

  if (addr2line_state != nullptr) {
    Drain(0, prefix, &addr2line_state, os);
  }
}

#elif defined(__APPLE__)

void DumpNativeStack([[maybe_unused]] std::ostream& os,
                     [[maybe_unused]] pid_t tid,
                     [[maybe_unused]] const char* prefix,
                     [[maybe_unused]] ArtMethod* current_method,
                     [[maybe_unused]] void* ucontext_ptr,
                     [[maybe_unused]] bool skip_frames) {}

void DumpNativeStack([[maybe_unused]] std::ostream& os,
                     [[maybe_unused]] unwindstack::AndroidLocalUnwinder& existing_map,
                     [[maybe_unused]] pid_t tid,
                     [[maybe_unused]] const char* prefix,
                     [[maybe_unused]] ArtMethod* current_method,
                     [[maybe_unused]] void* ucontext_ptr,
                     [[maybe_unused]] bool skip_frames) {}

#else
#error "Unsupported architecture for native stack dumps."
#endif

}  // namespace art
