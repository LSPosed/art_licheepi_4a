/*
 * Copyright (C) 2014 The Android Open Source Project
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

#ifndef ART_COMPILER_OPTIMIZING_CONSTANT_FOLDING_H_
#define ART_COMPILER_OPTIMIZING_CONSTANT_FOLDING_H_

#include "base/macros.h"
#include "nodes.h"
#include "optimization.h"
#include "optimizing/optimizing_compiler_stats.h"

namespace art HIDDEN {

/**
 * Optimization pass performing a simple constant-expression
 * evaluation on the SSA form.
 *
 * Note that graph simplifications producing a constant should be
 * implemented in art::HConstantFolding, while graph simplifications
 * not producing constants should be implemented in
 * art::InstructionSimplifier.  (This convention is a choice that was
 * made during the development of these parts of the compiler and is
 * not bound by any technical requirement.)
 *
 * This class is named art::HConstantFolding to avoid name
 * clashes with the art::ConstantPropagation class defined in
 * compiler/dex/post_opt_passes.h.
 */
class HConstantFolding : public HOptimization {
 public:
  HConstantFolding(HGraph* graph,
                   OptimizingCompilerStats* stats = nullptr,
                   const char* name = kConstantFoldingPassName,
                   bool use_all_optimizations = false)
      : HOptimization(graph, name, stats), use_all_optimizations_(use_all_optimizations) {}

  bool Run() override;

  static constexpr const char* kConstantFoldingPassName = "constant_folding";

 private:
  // Use all optimizations without restrictions.
  bool use_all_optimizations_;

  DISALLOW_COPY_AND_ASSIGN(HConstantFolding);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_CONSTANT_FOLDING_H_
