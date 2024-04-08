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

#include "oat_quick_method_header.h"

#include "arch/instruction_set.h"
#include "art_method.h"
#include "dex/dex_file_types.h"
#include "interpreter/mterp/nterp.h"
#include "nterp_helpers.h"
#include "scoped_thread_state_change-inl.h"
#include "stack_map.h"
#include "thread.h"

namespace art {

uint32_t OatQuickMethodHeader::ToDexPc(ArtMethod** frame,
                                       const uintptr_t pc,
                                       bool abort_on_failure) const {
  ArtMethod* method = *frame;
  const void* entry_point = GetEntryPoint();
  uint32_t sought_offset = pc - reinterpret_cast<uintptr_t>(entry_point);
  if (method->IsNative()) {
    return dex::kDexNoIndex;
  } else if (IsNterpMethodHeader()) {
    return NterpGetDexPC(frame);
  } else {
    DCHECK(IsOptimized());
    CodeInfo code_info = CodeInfo::DecodeInlineInfoOnly(this);
    StackMap stack_map = code_info.GetStackMapForNativePcOffset(sought_offset);
    if (stack_map.IsValid()) {
      return stack_map.GetDexPc();
    }
  }
  if (abort_on_failure) {
    LOG(FATAL) << "Failed to find Dex offset for PC offset "
           << reinterpret_cast<void*>(sought_offset)
           << "(PC " << reinterpret_cast<void*>(pc) << ", entry_point=" << entry_point
           << " current entry_point=" << method->GetEntryPointFromQuickCompiledCode()
           << ") in " << method->PrettyMethod();
  }
  return dex::kDexNoIndex;
}

uintptr_t OatQuickMethodHeader::ToNativeQuickPc(ArtMethod* method,
                                                const uint32_t dex_pc,
                                                bool abort_on_failure) const {
  const void* entry_point = GetEntryPoint();
  DCHECK(!method->IsNative());
  // For catch handlers use the ArrayRef<const uint32_t> version of ToNativeQuickPc.
  DCHECK(!IsNterpMethodHeader());
  DCHECK(IsOptimized());
  // Search for the dex-to-pc mapping in stack maps.
  CodeInfo code_info = CodeInfo::DecodeInlineInfoOnly(this);

  StackMap stack_map = code_info.GetStackMapForDexPc(dex_pc);
  if (stack_map.IsValid()) {
    return reinterpret_cast<uintptr_t>(entry_point) + stack_map.GetNativePcOffset(kRuntimeISA);
  }
  if (abort_on_failure) {
    ScopedObjectAccess soa(Thread::Current());
    LOG(FATAL) << "Failed to find native offset for dex pc 0x" << std::hex << dex_pc << " in "
               << method->PrettyMethod();
  }
  return UINTPTR_MAX;
}

uintptr_t OatQuickMethodHeader::ToNativeQuickPcForCatchHandlers(
    ArtMethod* method,
    ArrayRef<const uint32_t> dex_pc_list,
    /* out */ uint32_t* stack_map_row,
    bool abort_on_failure) const {
  const void* entry_point = GetEntryPoint();
  DCHECK(!method->IsNative());
  if (IsNterpMethodHeader()) {
    return NterpGetCatchHandler();
  }
  DCHECK(IsOptimized());
  // Search for the dex-to-pc mapping in stack maps.
  CodeInfo code_info = CodeInfo::DecodeInlineInfoOnly(this);

  StackMap stack_map = code_info.GetCatchStackMapForDexPc(dex_pc_list);
  *stack_map_row = stack_map.Row();
  if (stack_map.IsValid()) {
    return reinterpret_cast<uintptr_t>(entry_point) +
           stack_map.GetNativePcOffset(kRuntimeISA);
  }
  if (abort_on_failure) {
    std::stringstream ss;
    bool first = true;
    ss << "Failed to find native offset for dex pcs (from outermost to innermost) " << std::hex;
    for (auto dex_pc : dex_pc_list) {
      if (!first) {
        ss << ", ";
      }
      first = false;
      ss << "0x" << dex_pc;
    }
    ScopedObjectAccess soa(Thread::Current());
    ss << " in " << method->PrettyMethod();
    LOG(FATAL) << ss.str();
  }
  return UINTPTR_MAX;
}

static inline OatQuickMethodHeader* GetNterpMethodHeader() {
  if (!interpreter::IsNterpSupported()) {
    return nullptr;
  }
  const void* nterp_entrypoint = interpreter::GetNterpEntryPoint();
  uintptr_t nterp_code_pointer =
      reinterpret_cast<uintptr_t>(EntryPointToCodePointer(nterp_entrypoint));
  return reinterpret_cast<OatQuickMethodHeader*>(nterp_code_pointer - sizeof(OatQuickMethodHeader));
}

OatQuickMethodHeader* OatQuickMethodHeader::NterpMethodHeader = GetNterpMethodHeader();

bool OatQuickMethodHeader::IsNterpMethodHeader() const {
  return interpreter::IsNterpSupported() ? (this == NterpMethodHeader) : false;
}

}  // namespace art
