/* Copyright 2017 The OpenXLA Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "xla/service/dot_decomposer.h"

#include <utility>

#include "absl/algorithm/container.h"
#include "absl/strings/str_join.h"
#include "xla/hlo/ir/hlo_computation.h"
#include "xla/hlo/ir/hlo_instruction.h"
#include "xla/hlo/ir/hlo_opcode.h"
#include "xla/layout_util.h"
#include "xla/permutation_util.h"
#include "xla/service/sparse_util.h"
#include "xla/shape_util.h"
#include "xla/status_macros.h"
#include "xla/types.h"
#include "tsl/platform/logging.h"

namespace xla {

namespace {

// Convert a dot into a canonical form;
// * Non-contracting dimensions are reshaped together,
// * Contracting dimensions are reshaped together,
// * Batch dimensions are the most major dimensions.
// This requires transposing and reshaping of the lhs and rhs, and reshaping the
// output batch to the original shape.
Status CanonicalizeDot(HloInstruction* original_dot) {
  auto computation = original_dot->parent();
  const auto& original_dnums = original_dot->dot_dimension_numbers();
  const int64_t num_batch_dims = original_dnums.lhs_batch_dimensions_size();
  const int64_t num_contracting_dims =
      original_dnums.lhs_contracting_dimensions_size();

  const auto& lhs_shape = original_dot->operand(0)->shape();
  const int64_t lhs_rank = lhs_shape.rank();
  const int64_t num_lhs_non_contracting_dims =
      lhs_rank - num_batch_dims - num_contracting_dims;

  std::vector<int64_t> lhs_non_contracting_dims;
  lhs_non_contracting_dims.reserve(num_lhs_non_contracting_dims);
  int64_t lhs_contracting_size = 1;
  bool lhs_contracting_dynamic = false;
  int64_t lhs_non_contracting_size = 1;
  bool lhs_non_contracting_dynamic = false;
  std::vector<int64_t> batch_dim_sizes;
  batch_dim_sizes.reserve(num_batch_dims);
  std::vector<bool> batch_dynamic_dims;
  batch_dynamic_dims.reserve(num_batch_dims);
  for (int64_t i = 0; i < lhs_rank; ++i) {
    if (absl::c_linear_search(original_dnums.lhs_contracting_dimensions(), i)) {
      lhs_contracting_size *= lhs_shape.dimensions(i);
      lhs_contracting_dynamic |= lhs_shape.is_dynamic_dimension(i);
    } else if (absl::c_linear_search(original_dnums.lhs_batch_dimensions(),
                                     i)) {
      batch_dim_sizes.push_back(lhs_shape.dimensions(i));
      batch_dynamic_dims.push_back(lhs_shape.is_dynamic_dimension(i));
    } else {
      lhs_non_contracting_dims.push_back(i);
      lhs_non_contracting_size *= lhs_shape.dimensions(i);
      lhs_non_contracting_dynamic |= lhs_shape.is_dynamic_dimension(i);
    }
  }
  // The canonical form of the lhs is
  // [BatchDims, NonContractingDimsProduct, ContractingsDimsProduct]
  // If NonContractingDimsProduct is 1, it is omitted.
  std::vector<int64_t> lhs_transpose;
  lhs_transpose.reserve(lhs_rank);
  lhs_transpose.insert(lhs_transpose.end(),
                       original_dnums.lhs_batch_dimensions().begin(),
                       original_dnums.lhs_batch_dimensions().end());
  lhs_transpose.insert(lhs_transpose.end(), lhs_non_contracting_dims.begin(),
                       lhs_non_contracting_dims.end());
  lhs_transpose.insert(lhs_transpose.end(),
                       original_dnums.lhs_contracting_dimensions().begin(),
                       original_dnums.lhs_contracting_dimensions().end());
  HloInstruction* lhs_operand = original_dot->mutable_operand(0);
  HloInstruction* transposed_lhs = computation->AddInstruction(
      HloInstruction::CreateTranspose(
          ShapeUtil::PermuteDimensions(lhs_transpose, lhs_shape), lhs_operand,
          lhs_transpose),
      &lhs_operand->metadata());

  std::vector<int64_t> lhs_reshape_dims = batch_dim_sizes;
  std::vector<bool> lhs_reshape_dynamic_dims = batch_dynamic_dims;
  if (lhs_non_contracting_size > 1) {
    lhs_reshape_dims.push_back(lhs_non_contracting_size);
    lhs_reshape_dynamic_dims.push_back(lhs_non_contracting_dynamic);
  }
  lhs_reshape_dims.push_back(lhs_contracting_size);
  lhs_reshape_dynamic_dims.push_back(lhs_contracting_dynamic);
  // Reshape the contracting and non-contracting dimensions together.
  HloInstruction* reshaped_lhs = computation->AddInstruction(
      HloInstruction::CreateReshape(
          ShapeUtil::MakeShape(lhs_shape.element_type(), lhs_reshape_dims,
                               lhs_reshape_dynamic_dims),
          transposed_lhs),
      &transposed_lhs->metadata());

  const auto& rhs_shape = original_dot->operand(1)->shape();
  const int64_t rhs_rank = rhs_shape.rank();
  const int64_t num_rhs_non_contracting_dims =
      rhs_rank - num_batch_dims - num_contracting_dims;
  std::vector<int64_t> rhs_non_contracting_dims;
  rhs_non_contracting_dims.reserve(num_rhs_non_contracting_dims);
  int64_t rhs_non_contracting_size = 1;
  bool rhs_non_contracting_dynamic = false;
  int64_t rhs_contracting_size = 1;
  bool rhs_contracting_dynamic = false;
  for (int64_t i = 0; i < rhs_rank; ++i) {
    if (absl::c_linear_search(original_dnums.rhs_contracting_dimensions(), i)) {
      rhs_contracting_size *= rhs_shape.dimensions(i);
      rhs_contracting_dynamic |= rhs_shape.is_dynamic_dimension(i);
    } else if (!absl::c_linear_search(original_dnums.rhs_batch_dimensions(),
                                      i)) {
      rhs_non_contracting_dims.push_back(i);
      rhs_non_contracting_size *= rhs_shape.dimensions(i);
      rhs_non_contracting_dynamic |= rhs_shape.is_dynamic_dimension(i);
    }
  }

  // The canonical form of the rhs is
  // [BatchDims, ContractingsDimsProduct, NonContractingDimsProduct]
  // If NonContractingDimsProduct is 1, it is omitted.
  std::vector<int64_t> rhs_transpose;
  rhs_transpose.reserve(rhs_rank);
  rhs_transpose.insert(rhs_transpose.end(),
                       original_dnums.rhs_batch_dimensions().begin(),
                       original_dnums.rhs_batch_dimensions().end());
  rhs_transpose.insert(rhs_transpose.end(),
                       original_dnums.rhs_contracting_dimensions().begin(),
                       original_dnums.rhs_contracting_dimensions().end());
  rhs_transpose.insert(rhs_transpose.end(), rhs_non_contracting_dims.begin(),
                       rhs_non_contracting_dims.end());
  HloInstruction* rhs_operand = original_dot->mutable_operand(1);
  HloInstruction* transposed_rhs = computation->AddInstruction(
      HloInstruction::CreateTranspose(
          ShapeUtil::PermuteDimensions(rhs_transpose, rhs_shape), rhs_operand,
          rhs_transpose),
      &rhs_operand->metadata());

  std::vector<int64_t> rhs_reshape_dims = batch_dim_sizes;
  rhs_reshape_dims.push_back(rhs_contracting_size);
  std::vector<bool> rhs_reshape_dynamic_dims = batch_dynamic_dims;
  rhs_reshape_dynamic_dims.push_back(rhs_contracting_dynamic);
  if (rhs_non_contracting_size > 1) {
    rhs_reshape_dims.push_back(rhs_non_contracting_size);
    rhs_reshape_dynamic_dims.push_back(rhs_non_contracting_dynamic);
  }
  // Reshape the contracting and non-contracting dimensions together.
  HloInstruction* reshaped_rhs = computation->AddInstruction(
      HloInstruction::CreateReshape(
          ShapeUtil::MakeShape(rhs_shape.element_type(), rhs_reshape_dims,
                               rhs_reshape_dynamic_dims),
          transposed_rhs),
      &transposed_rhs->metadata());

  std::vector<int64_t> dot_dims = batch_dim_sizes;
  std::vector<bool> dot_dynamic_dims = batch_dynamic_dims;
  if (lhs_non_contracting_size > 1) {
    dot_dims.push_back(lhs_non_contracting_size);
    dot_dynamic_dims.push_back(lhs_non_contracting_dynamic);
  }
  if (rhs_non_contracting_size > 1) {
    dot_dims.push_back(rhs_non_contracting_size);
    dot_dynamic_dims.push_back(rhs_non_contracting_dynamic);
  }

  DotDimensionNumbers dot_dnums;
  for (int64_t i = 0; i < num_batch_dims; ++i) {
    dot_dnums.add_lhs_batch_dimensions(i);
    dot_dnums.add_rhs_batch_dimensions(i);
  }
  dot_dnums.add_lhs_contracting_dimensions(
      num_batch_dims + (lhs_non_contracting_size > 1 ? 1 : 0));
  dot_dnums.add_rhs_contracting_dimensions(num_batch_dims);

  HloInstruction* dot = computation->AddInstruction(HloInstruction::CreateDot(
      ShapeUtil::MakeShape(original_dot->shape().element_type(), dot_dims,
                           dot_dynamic_dims),
      reshaped_lhs, reshaped_rhs, dot_dnums, original_dot->precision_config()));
  original_dot->SetupDerivedInstruction(dot);

  std::unique_ptr<HloInstruction> replacement =
      HloInstruction::CreateReshape(original_dot->shape(), dot);
  VLOG(3) << "Canonicalizing dot:\n"
          << "\t old: " << original_dot->ToString() << "\n"
          << "\t new: " << dot->ToString() << "\n"
          << "\t   -> " << replacement->ToString();
  return computation->ReplaceWithNewInstruction(original_dot,
                                                std::move(replacement));
}

}  // namespace

absl::StatusOr<bool> DotDecomposer::Run(
    HloModule* module,
    const absl::flat_hash_set<absl::string_view>& execution_threads) {
  // Gather all Non-canonical Dot operations.
  std::vector<HloInstruction*> non_canonical_dots;
  for (auto* computation :
       module->MakeNonfusionComputations(execution_threads)) {
    for (auto* instruction : computation->instructions()) {
      if (instruction->opcode() != HloOpcode::kDot) {
        continue;
      }
      // Skips sparse instruction as DotDecomposer does not know how to handle
      // sparse input yet.
      if (SparseUtil::HasSparseInOut(instruction)) {
        continue;
      }
      const DotDimensionNumbers& dnums = instruction->dot_dimension_numbers();
      // A dot it not canonical if there is more than one contracting dimension.
      if (dnums.lhs_contracting_dimensions_size() != 1) {
        non_canonical_dots.push_back(instruction);
        continue;
      }
      // A dot is not canonical if it has more than one non-contracting
      // dimension.
      if (dnums.lhs_batch_dimensions_size() + 2 <
              instruction->operand(0)->shape().rank() ||
          dnums.rhs_batch_dimensions_size() + 2 <
              instruction->operand(1)->shape().rank()) {
        non_canonical_dots.push_back(instruction);
        continue;
      }
      if (dnums.lhs_batch_dimensions().empty() &&
          dnums.lhs_contracting_dimensions().empty()) {
        non_canonical_dots.push_back(instruction);
        continue;
      }
      // Check that batch dims, if present, are canonical.
      std::vector<int64_t> canonical_batch_dims(
          dnums.lhs_batch_dimensions_size());
      absl::c_iota(canonical_batch_dims, 0);
      if (!absl::c_equal(dnums.lhs_batch_dimensions(), canonical_batch_dims) ||
          !absl::c_equal(dnums.rhs_batch_dimensions(), canonical_batch_dims)) {
        non_canonical_dots.push_back(instruction);
      }
    }
  }
  bool changed = false;
  for (auto* dot : non_canonical_dots) {
    TF_RETURN_IF_ERROR(CanonicalizeDot(dot));
    changed = true;
  }
  return changed;
}

}  // namespace xla
