/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

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

// This file defines the operations used in the XLA dialect.

#include "tensorflow/compiler/mlir/xla/ir/hlo_ops.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include <algorithm>

#include "absl/container/flat_hash_set.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MathExtras.h"
#include "mlir/Dialect/StandardOps/IR/Ops.h"  // from @llvm-project
#include "mlir/IR/Attributes.h"  // from @llvm-project
#include "mlir/IR/Builders.h"  // from @llvm-project
#include "mlir/IR/Dialect.h"  // from @llvm-project
#include "mlir/IR/Location.h"  // from @llvm-project
#include "mlir/IR/MLIRContext.h"  // from @llvm-project
#include "mlir/IR/Matchers.h"  // from @llvm-project
#include "mlir/IR/OpDefinition.h"  // from @llvm-project
#include "mlir/IR/OpImplementation.h"  // from @llvm-project
#include "mlir/IR/Operation.h"  // from @llvm-project
#include "mlir/IR/OperationSupport.h"  // from @llvm-project
#include "mlir/IR/PatternMatch.h"  // from @llvm-project
#include "mlir/IR/StandardTypes.h"  // from @llvm-project
#include "mlir/IR/TypeUtilities.h"  // from @llvm-project
#include "mlir/IR/Types.h"  // from @llvm-project
#include "mlir/IR/Value.h"  // from @llvm-project
#include "mlir/Support/LLVM.h"  // from @llvm-project
#include "mlir/Support/LogicalResult.h"  // from @llvm-project
#include "mlir/Transforms/InliningUtils.h"  // from @llvm-project
#include "tensorflow/compiler/mlir/xla/convert_op_folder.h"
#include "tensorflow/compiler/mlir/xla/ir/hlo_ops.h.inc"
#include "tensorflow/compiler/mlir/xla/ir/hlo_utils.h"

namespace mlir {
#include "tensorflow/compiler/mlir/xla/ir/hlo_structs.cc.inc"
namespace xla_hlo {

Operation* XlaHloDialect::materializeConstant(OpBuilder& builder,
                                              Attribute value, Type type,
                                              Location loc) {
  // HLO dialect constants only support ElementsAttr unlike standard dialect
  // constant which supports all attributes.
  if (value.isa<ElementsAttr>())
    return builder.create<xla_hlo::ConstOp>(loc, type,
                                            value.cast<ElementsAttr>());
  return nullptr;
}

template <typename T>
static LogicalResult Verify(T op) {
  return success();
}

namespace {

//===----------------------------------------------------------------------===//
// Utilities for the canonicalize patterns
//===----------------------------------------------------------------------===//

// Returns 1D 64-bit dense elements attribute with the given values.
DenseIntElementsAttr GetI64ElementsAttr(ArrayRef<int64_t> values,
                                        Builder* builder) {
  RankedTensorType ty = RankedTensorType::get(
      {static_cast<int64_t>(values.size())}, builder->getIntegerType(64));
  return DenseIntElementsAttr::get(ty, values);
}

// Given the start indices and slice sizes for a dynamic-slice that can be
// converted to a static slice, returns the limits for the static slice.
DenseIntElementsAttr BuildSliceLimits(DenseIntElementsAttr start_indices,
                                      DenseIntElementsAttr slice_sizes,
                                      Builder* builder) {
  SmallVector<int64_t, 4> slice_limits;
  for (int64_t i = 0; i < slice_sizes.getNumElements(); ++i) {
    int64_t start_index = start_indices.getValue<IntegerAttr>(i).getInt();
    int64_t slice_size = slice_sizes.getValue<IntegerAttr>(i).getInt();
    slice_limits.push_back(start_index + slice_size);
  }
  return GetI64ElementsAttr(slice_limits, builder);
}

#include "tensorflow/compiler/mlir/xla/transforms/generated_canonicalize.inc"
}  // namespace

//===----------------------------------------------------------------------===//
// ConstOp
//===----------------------------------------------------------------------===//

static void Print(ConstOp op, OpAsmPrinter* printer) {
  // Use short form only if the result type matches type of attribute 'value'.
  bool use_short_form = op.value().getType() == op.getType();

  // Print op name.
  *printer << op.getOperationName();

  // If short form, elide attribute value while printing the attribute
  // dictionary.
  SmallVector<StringRef, 1> elided_attrs;
  if (use_short_form) elided_attrs.push_back("value");
  printer->printOptionalAttrDict(op.getAttrs(), elided_attrs);

  if (use_short_form) {
    *printer << ' ' << op.value();
  } else {
    *printer << " : " << op.getType();
  }
}

static ParseResult ParseConstOp(OpAsmParser* parser, OperationState* result) {
  if (parser->parseOptionalAttrDict(result->attributes)) return failure();

  // If colon is not present after attribute dictionary, it should be short form
  // and attribute 'value' is outside the dictionary.
  if (failed(parser->parseOptionalColon())) {
    Attribute value;
    if (parser->parseAttribute(value, "value", result->attributes))
      return failure();
    return parser->addTypeToList(value.getType(), result->types);
  }

  // Long form should have type of the result after colon.
  Type ty;
  if (parser->parseType(ty)) return failure();
  result->types.push_back(ty);
  return success();
}

OpFoldResult ConstOp::fold(ArrayRef<Attribute> operands) {
  assert(operands.empty() && "constant has no operands");

  // Return the held attribute value.
  return value();
}

// Builds a constant op with the specified attribute `value`.
void ConstOp::build(Builder* builder, OperationState& result, Attribute value) {
  Type type;
  if (auto elemAttr = value.dyn_cast<ElementsAttr>()) {
    type = elemAttr.getType();
  } else if (value.isa<BoolAttr>() || value.isa<FloatAttr>() ||
             value.isa<IntegerAttr>()) {
    // All XLA types must be tensor types. In the build() method, we want to
    // provide more flexibility by allowing attributes of scalar types. But we
    // need to wrap it up with ElementsAttr to construct valid XLA constants.
    type = RankedTensorType::get(/*shape=*/{}, value.getType());
    value = DenseElementsAttr::get(type.cast<TensorType>(), value);
  }

  // TODO: support other XLA specific types.
  assert(type && "unsupported attribute type for building xla_hlo.constant");
  result.types.push_back(type);
  result.addAttribute("value", value);
}

//===----------------------------------------------------------------------===//
// DotGeneralOp
//===----------------------------------------------------------------------===//

static LogicalResult Verify(DotGeneralOp op) {
  auto dot_dimension_numbers = op.dot_dimension_numbers();
  int64_t lhs_batching_dimensions_size = llvm::size(
      dot_dimension_numbers.lhs_batching_dimensions().getValues<int64_t>());
  int64_t rhs_batching_dimensions_size = llvm::size(
      dot_dimension_numbers.rhs_batching_dimensions().getValues<int64_t>());
  if (lhs_batching_dimensions_size != rhs_batching_dimensions_size) {
    return op.emitError()
           << "lhs and rhs should have the same number of batching dimensions";
  }
  int64_t lhs_contracting_dimensions_size = llvm::size(
      dot_dimension_numbers.lhs_contracting_dimensions().getValues<int64_t>());
  int64_t rhs_contracting_dimensions_size = llvm::size(
      dot_dimension_numbers.rhs_contracting_dimensions().getValues<int64_t>());
  if (lhs_contracting_dimensions_size != rhs_contracting_dimensions_size) {
    return op.emitError() << "lhs and rhs should have the same number of "
                             "contracting dimensions";
  }
  return success();
}

//===----------------------------------------------------------------------===//
// IotaOp
//===----------------------------------------------------------------------===//

static LogicalResult Verify(IotaOp op) {
  auto shape = op.getType().cast<ShapedType>();
  if (!shape.hasRank()) return success();

  if (shape.getRank() == 0)
    return op.emitOpError() << "does not support scalars.";

  auto iota_dimension = op.iota_dimension().getSExtValue();
  if (iota_dimension >= shape.getRank() || iota_dimension < 0)
    return op.emitOpError() << "iota dimension cannot go beyond the output "
                               "rank or be negative.";
  return success();
}

//===----------------------------------------------------------------------===//
// AbsOp
//===----------------------------------------------------------------------===//

void AbsOp::build(Builder* builder, OperationState& result, Value operand) {
  auto shaped_type = operand.getType().cast<ShapedType>();
  Type new_type;
  if (!shaped_type.getElementType().isa<ComplexType>()) {
    new_type = operand.getType();
  } else if (shaped_type.hasRank()) {
    new_type = RankedTensorType::get(shaped_type.getShape(), operand.getType());
  } else {
    new_type = UnrankedTensorType::get(operand.getType());
  }

  return AbsOp::build(builder, result, new_type, operand);
}

//===----------------------------------------------------------------------===//
// CollectivePermuteOp
//===----------------------------------------------------------------------===//

static LogicalResult Verify(CollectivePermuteOp op) {
  // Check that source target pair is Nx2 tensor.
  auto type = op.source_target_pairs().getType().dyn_cast<RankedTensorType>();
  if (type.getRank() != 2)
    return op.emitError() << "expect source_target_pairs attribute to be of "
                             "rank 2, but got rank "
                          << type.getRank();
  if (type.getShape()[1] != 2)
    return op.emitError()
           << "expect source_target_pairs attribute of shape (N, 2), but got ("
           << type.getShape() << ")";
  // Check source target pairs for duplicate sources or targets
  absl::flat_hash_set<int64_t> sources;
  absl::flat_hash_set<int64_t> targets;
  for (auto i = op.source_target_pairs().begin(),
            e = op.source_target_pairs().end();
       i != e; ++i) {
    auto val = (*i).getSExtValue();
    if (i.getIndex() % 2 == 0) {
      bool is_unique = sources.insert(val).second;
      if (!is_unique) return op.emitError() << "duplicate sources not allowed.";
    } else {
      bool is_unique = targets.insert(val).second;
      if (!is_unique) return op.emitError() << "duplicate targets not allowed.";
    }
  }
  return success();
}

//===----------------------------------------------------------------------===//
// ConvertOp
//===----------------------------------------------------------------------===//

void ConvertOp::build(Builder* builder, OperationState& result, Value operand,
                      Type result_element_ty) {
  Type result_ty;
  Type operand_ty = operand.getType();
  if (auto ranked_ty = operand_ty.dyn_cast<RankedTensorType>()) {
    result_ty = RankedTensorType::get(ranked_ty.getShape(), result_element_ty);
  } else {
    result_ty = UnrankedTensorType::get(result_element_ty);
  }
  build(builder, result, result_ty, operand);
}

OpFoldResult ConvertOp::fold(ArrayRef<Attribute> operands) {
  if (getOperand().getType() == getResult().getType()) return getOperand();

  // If the operand is constant, we can do the conversion now.
  if (auto elementsAttr = operands.front().dyn_cast_or_null<ElementsAttr>()) {
    return xla::ConvertElementsAttr(elementsAttr,
                                    getElementTypeOrSelf(getResult()));
  }

  return {};
}

//===----------------------------------------------------------------------===//
// DequantizeOp
//===----------------------------------------------------------------------===//

static LogicalResult Verify(DequantizeOp op) {
  auto input_type = op.input().getType().dyn_cast<ShapedType>();
  auto output_type = op.output().getType().dyn_cast<ShapedType>();
  if (!input_type || !output_type) {
    return op.emitError() << "ranked input and output.";
  }
  auto input_shape = input_type.getShape();
  auto output_shape = output_type.getShape().vec();
  if (op.transpose_output()) {
    std::reverse(output_shape.begin(), output_shape.end());
  }

  // Check the input rank and output rank are same, and also the lower
  // dimensions are same.
  if (input_shape.size() != output_shape.size() ||
      !std::equal(input_shape.begin(),
                  std::next(input_shape.begin(), input_shape.size() - 1),
                  output_shape.begin())) {
    return op.emitError() << "mismatched dimensions.";
  }

  // Check that the last dimension of the output is 2x or 4x of that of the
  // input depending on the unpacked input is 16 or 8 bits.
  int input_last_dim = *input_shape.rbegin();
  int output_last_dim = *output_shape.rbegin();
  int scale_factor = op.is_16bits() ? 2 : 4;
  if (output_last_dim != scale_factor * input_last_dim) {
    return op.emitError() << "last dimension of output should be "
                          << scale_factor << "x of the input.";
  }

  return success();
}

//===----------------------------------------------------------------------===//
// GetTupleElementOp
//===----------------------------------------------------------------------===//

static LogicalResult Verify(GetTupleElementOp op) {
  auto indexVal = op.index().getZExtValue();
  auto operandType = op.getOperand().getType().cast<TupleType>();
  if (indexVal >= operandType.size()) {
    return op.emitOpError(
        llvm::formatv("index {0} is out of bounds of operand with size {1}",
                      indexVal, operandType.size()));
  }

  auto expectedType = operandType.getType(indexVal);
  if (op.getType() != expectedType) {
    return op.emitOpError(llvm::formatv("has return type {0}, but expected {1}",
                                        op.getType(), expectedType));
  }
  return success();
}

OpFoldResult GetTupleElementOp::fold(ArrayRef<Attribute> operands) {
  if (auto tupleOp =
          dyn_cast_or_null<xla_hlo::TupleOp>(getOperand().getDefiningOp())) {
    return tupleOp.getOperand(index().getLimitedValue());
  }

  return {};
}

//===----------------------------------------------------------------------===//
// TupleOp
//===----------------------------------------------------------------------===//

static LogicalResult Verify(TupleOp op) {
  SmallVector<Type, 8> operandTypes = {op.operand_type_begin(),
                                       op.operand_type_end()};
  auto expectedType = TupleType::get(operandTypes, op.getContext());
  if (op.getType() != expectedType) {
    return op.emitOpError(llvm::formatv("has return type {0}, but expected {1}",
                                        op.getType(), expectedType));
  }
  return success();
}

//===----------------------------------------------------------------------===//
// AllToAllOp
//===----------------------------------------------------------------------===//

static LogicalResult Verify(AllToAllOp op) {
  // If operand is ranked, size of split dimension should be a multiple of split
  // count.
  auto type = op.getOperand().getType().dyn_cast<RankedTensorType>();
  if (!type) return success();
  auto split_dim_size = type.getDimSize(op.split_dimension().getSExtValue());
  auto split_count = op.split_count().getSExtValue();
  if (split_dim_size % split_count != 0) {
    return op.emitError() << "split dimension has size " << split_dim_size
                          << ", expected to be a multiple of split_count "
                          << split_count;
  }
  return success();
}

//===----------------------------------------------------------------------===//
// BroadcastOp
//===----------------------------------------------------------------------===//

// TODO(b/129012527) These should be expressed as type constraints.
static LogicalResult Verify(BroadcastOp op) {
  auto sizes = op.broadcast_sizes();
  auto sizesType = sizes.getType();
  auto sizesRank = sizesType.getRank();
  if (sizesRank != 1) {
    return op.emitOpError(llvm::formatv(
        "broadcast_sizes has rank {0} instead of rank 1", sizesRank));
  }

  auto resultType = op.getResult().getType().cast<RankedTensorType>();
  auto resultRank = resultType.getRank();
  auto operandType = op.operand().getType().cast<RankedTensorType>();
  auto operandRank = operandType.getRank();
  auto sizesSize = sizesType.getNumElements();
  auto expectedRank = operandRank + sizesSize;

  if (resultRank != expectedRank) {
    return op.emitOpError(
        llvm::formatv("result rank ({0}) does not match operand rank "
                      "({1}) plus size of broadcast_sizes ({2})",
                      resultRank, operandRank, sizesSize));
  }

  llvm::SmallVector<int64_t, 10> expectedShape(sizes.getValues<int64_t>());

  auto operandShape = operandType.getShape();
  expectedShape.insert(expectedShape.end(), operandShape.begin(),
                       operandShape.end());

  auto resultShape = resultType.getShape();
  if (resultShape != llvm::makeArrayRef(expectedShape)) {
    return op.emitOpError(llvm::formatv(
        "result has shape [{0}] instead of [{1}]",
        llvm::make_range(resultShape.begin(), resultShape.end()),
        llvm::make_range(expectedShape.begin(), expectedShape.end())));
  }

  return success();
}

//===----------------------------------------------------------------------===//
// BroadcastInDimOp
//===----------------------------------------------------------------------===//

static LogicalResult Verify(BroadcastInDimOp op) {
  auto operandType = op.operand().getType().dyn_cast<RankedTensorType>();
  auto operandRank = operandType.getRank();
  if (!op.broadcast_dimensions()) {
    if (operandRank == 0) {
      return success();
    }
    return op.emitOpError(
        llvm::formatv("broadcast_dimensions is absent, but required because "
                      "operand has non-zero rank ({0})",
                      operandRank));
  }

  auto dimensions = op.broadcast_dimensions();
  auto dimensionsType = op.broadcast_dimensions().getType();
  auto dimensionsRank = dimensionsType.getRank();
  if (dimensionsRank != 1) {
    return op.emitOpError(llvm::formatv(
        "broadcast_dimensions has rank {0} instead of rank 1", dimensionsRank));
  }

  auto dimensionsSize = dimensionsType.getNumElements();
  if (dimensionsSize != operandRank) {
    return op.emitOpError(llvm::formatv(
        "broadcast_dimensions size ({0}) does not match operand rank ({1})",
        dimensionsSize, operandRank));
  }

  auto resultType = op.getResult().getType().cast<RankedTensorType>();
  auto resultRank = resultType.getRank();
  if (resultRank < operandRank) {
    return op.emitOpError(
        llvm::formatv("result rank ({0}) is less than operand rank ({1})",
                      resultRank, operandRank));
  }

  for (int i = 0; i != dimensionsSize; ++i) {
    auto dimIndex = dimensions.getValue<int64_t>(i);
    if (dimIndex >= resultRank) {
      return op.emitOpError(
          llvm::formatv("broadcast_dimensions contains invalid value {0} for "
                        "result result with rank {1}",
                        dimIndex, resultRank));
    }

    auto dimSize = operandType.getDimSize(i);
    auto resultDimSize = resultType.getDimSize(dimIndex);
    if (dimSize != 1 && dimSize != resultDimSize) {
      return op.emitOpError(
          llvm::formatv("size of operand dimension {0} ({1}) is not equal to "
                        "1 or size of result dimension {2} ({3})",
                        i, dimSize, dimIndex, resultDimSize));
    }
  }

  return success();
}

//===----------------------------------------------------------------------===//
// ScalarsToDimensionTensorOp
//===----------------------------------------------------------------------===//

namespace {

// Canonicalizes the pattern of the form
//
// %2 = "xla_hlo.scalars_to_dimension_tensor"(%0, %1)
//          : (i32, i32) -> tensor<2xi32>
// %3 = extract_element %2[%c0] : tensor<2xi32>
//
// to just %0.
struct ExtractElementFromScalarsToDimensionTensor
    : public OpRewritePattern<ExtractElementOp> {
  using OpRewritePattern<ExtractElementOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(ExtractElementOp extract,
                                PatternRewriter& rewriter) const override {
    if (extract.indices().size() != 1) return failure();

    if (auto scalars_to_tensor = dyn_cast_or_null<ScalarsToDimensionTensorOp>(
            extract.aggregate().getDefiningOp())) {
      APInt index;
      if (!matchPattern(*extract.indices().begin(), m_ConstantInt(&index))) {
        return failure();
      }
      rewriter.replaceOp(extract,
                         scalars_to_tensor.getOperand(index.getZExtValue()));
      return success();
    }
    return failure();
  }
};

}  // namespace

void ScalarsToDimensionTensorOp::getCanonicalizationPatterns(
    OwningRewritePatternList& results, MLIRContext* context) {
  results.insert<ExtractElementFromScalarsToDimensionTensor>(context);
}

//===----------------------------------------------------------------------===//
// DynamicBroadcastInDimOp
//===----------------------------------------------------------------------===//

static LogicalResult Verify(DynamicBroadcastInDimOp op) {
  auto operandType = op.operand().getType().dyn_cast<RankedTensorType>();
  auto resultType = op.getResult().getType().dyn_cast<RankedTensorType>();

  // If either the operand or result are unranked, there is very little
  // to verify statically.
  if (!operandType || !resultType) {
    return success();
  }

  auto outputDimensionsType =
      op.output_dimensions().getType().cast<RankedTensorType>();
  auto outputDimensionsSize = outputDimensionsType.getDimSize(0);
  auto operandRank = operandType.getRank();
  auto resultRank = resultType.getRank();

  // Verify broadcast_dimensions.
  auto bcastDimensions = op.broadcast_dimensions();
  auto bcastDimensionsType = op.broadcast_dimensions().getType();
  auto bcastDimensionsRank = bcastDimensionsType.getRank();
  // TODO(laurenzo): Update the BroadcastDimAttr to constrain its rank to 1.
  if (bcastDimensionsRank != 1) {
    return op.emitOpError(
        llvm::formatv("broadcast_dimensions has rank {0} instead of rank 1",
                      bcastDimensionsRank));
  }

  auto bcastDimensionsSize = bcastDimensionsType.getNumElements();
  if (bcastDimensionsSize != operandRank) {
    return op.emitOpError(llvm::formatv(
        "broadcast_dimensions size ({0}) does not match operand rank ({1})",
        bcastDimensionsSize, operandRank));
  }

  if (resultRank < operandRank) {
    return op.emitOpError(
        llvm::formatv("result rank ({0}) is less than operand rank ({1})",
                      resultRank, operandRank));
  }

  for (int i = 0; i != bcastDimensionsSize; ++i) {
    auto dimIndex = bcastDimensions.getValue<int64_t>(i);
    if (dimIndex >= resultRank) {
      return op.emitOpError(
          llvm::formatv("broadcast_dimensions contains invalid value {0} for "
                        "result result with rank {1}",
                        dimIndex, resultRank));
    }

    auto dimSize = operandType.getDimSize(i);
    auto resultDimSize = resultType.getDimSize(dimIndex);
    if (dimSize != 1 && dimSize != resultDimSize) {
      return op.emitOpError(
          llvm::formatv("size of operand dimension {0} ({1}) is not equal to "
                        "1 or size of result dimension {2} ({3})",
                        i, dimSize, dimIndex, resultDimSize));
    }
  }

  if (outputDimensionsSize != resultRank) {
    return op.emitOpError(
        llvm::formatv("result rank ({0}) is not equal to number of output "
                      "dimensions ({1})",
                      resultRank, outputDimensionsSize));
  }

  return success();
}

// If a DynamicBroadCastInDimOp is not actually dynamic, use an ordinary
// BroadcastInDimOp.
class DynamicBroadcastInDimOpNotActuallyDynamic
    : public OpRewritePattern<DynamicBroadcastInDimOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(DynamicBroadcastInDimOp op,
                                PatternRewriter& rewriter) const override {
    auto type = op.getType().dyn_cast<RankedTensorType>();
    if (!type || !type.hasStaticShape()) {
      return rewriter.notifyMatchFailure(op, "requires static shape");
    }
    rewriter.replaceOpWithNewOp<BroadcastInDimOp>(
        op, op.getType(), op.operand(), op.broadcast_dimensions());
    return success();
  }
};

void DynamicBroadcastInDimOp::getCanonicalizationPatterns(
    OwningRewritePatternList& results, MLIRContext* context) {
  results.insert<DynamicBroadcastInDimOpNotActuallyDynamic>(context);
}

//===----------------------------------------------------------------------===//
// ClampOp
//===----------------------------------------------------------------------===//

static LogicalResult Verify(ClampOp op) {
  auto operandType = op.operand().getType().cast<RankedTensorType>();
  auto operandShape = operandType.getShape();
  auto minType = op.min().getType().cast<RankedTensorType>();

  auto minShape = minType.getShape();
  if (minShape != operandShape && minType.getRank() != 0) {
    return op.emitOpError(llvm::formatv(
        "min shape [{0}] is not scalar and does not match operand shape [{1}]",
        llvm::make_range(minShape.begin(), minShape.end()),
        llvm::make_range(operandShape.begin(), operandShape.end())));
  }

  auto maxType = op.max().getType().cast<RankedTensorType>();
  auto maxShape = maxType.getShape();
  if (maxShape != operandShape && maxType.getRank() != 0) {
    return op.emitOpError(llvm::formatv(
        "max shape [{0}] is not scalar and does not match operand shape [{1}]",
        llvm::make_range(maxShape.begin(), maxShape.end()),
        llvm::make_range(operandShape.begin(), operandShape.end())));
  }

  return success();
}

//===----------------------------------------------------------------------===//
// ComplexOp
//===----------------------------------------------------------------------===//

void ComplexOp::build(Builder* builder, OperationState& state, Value lhs,
                      Value rhs) {
  auto type = lhs.getType();
  auto element_ty = ComplexType::get(getElementTypeOrSelf(type));
  Type result_ty;
  if (auto ranked_type = type.dyn_cast<RankedTensorType>()) {
    result_ty = RankedTensorType::get(ranked_type.getShape(), element_ty);
  } else if (type.isa<UnrankedTensorType>()) {
    result_ty = UnrankedTensorType::get(element_ty);
  } else {
    result_ty = element_ty;
  }

  build(builder, state, result_ty, lhs, rhs);
}

OpFoldResult ComplexOp::fold(ArrayRef<Attribute> operands) {
  auto real_op =
      dyn_cast_or_null<xla_hlo::RealOp>(getOperand(0).getDefiningOp());
  auto imag_op =
      dyn_cast_or_null<xla_hlo::ImagOp>(getOperand(1).getDefiningOp());
  if (real_op && imag_op && real_op.getOperand() == imag_op.getOperand()) {
    return real_op.getOperand();
  }

  return {};
}

namespace {
Type CreateRealType(Type type) {
  auto element_ty = getElementTypeOrSelf(type);
  if (auto complex_ty = element_ty.dyn_cast<ComplexType>()) {
    element_ty = complex_ty.getElementType();
  }

  if (auto ranked_type = type.dyn_cast<RankedTensorType>()) {
    return RankedTensorType::get(ranked_type.getShape(), element_ty);
  } else if (type.dyn_cast<UnrankedTensorType>()) {
    return UnrankedTensorType::get(element_ty);
  }

  return element_ty;
}
}  // namespace

void ImagOp::build(Builder* builder, OperationState& state, Value val) {
  build(builder, state, CreateRealType(val.getType()), val);
}

OpFoldResult ImagOp::fold(ArrayRef<Attribute> operands) {
  if (auto complex_op =
          dyn_cast_or_null<xla_hlo::ComplexOp>(getOperand().getDefiningOp())) {
    return complex_op.getOperand(1);
  }

  return {};
}

void RealOp::build(Builder* builder, OperationState& state, Value val) {
  build(builder, state, CreateRealType(val.getType()), val);
}

OpFoldResult RealOp::fold(ArrayRef<Attribute> operands) {
  if (auto complex_op =
          dyn_cast_or_null<xla_hlo::ComplexOp>(getOperand().getDefiningOp())) {
    return complex_op.getOperand(0);
  }

  return {};
}

//===----------------------------------------------------------------------===//
// ConcatenateOp
//===----------------------------------------------------------------------===//

OpFoldResult ConcatenateOp::fold(ArrayRef<Attribute> operands) {
  if (getNumOperands() == 1) return getOperand(0);
  return {};
}

static LogicalResult Verify(ConcatenateOp op) {
  auto firstType = op.getOperand(0).getType().cast<RankedTensorType>();

  auto firstShape = firstType.getShape();
  int numOperands = op.getNumOperands();
  for (int i = 1; i < numOperands; i++) {
    auto secondType = op.getOperand(i).getType().cast<RankedTensorType>();

    if (firstType.getRank() != secondType.getRank()) {
      return op.emitOpError(
          llvm::formatv("operands (0) and ({0}) do not match rank", i));
    }

    auto secondShape = secondType.getShape();
    for (int d = 0; d < firstType.getRank(); ++d) {
      if (firstShape[d] != secondShape[d] && d != op.dimension()) {
        return op.emitOpError(llvm::formatv(
            "operands (0) and ({0}) non-concat dimensions do not match "
            "({1}) != ({2})",
            i, llvm::make_range(firstShape.begin(), firstShape.end()),
            llvm::make_range(secondShape.begin(), secondShape.end())));
      }
    }
  }
  return success();
}

//===----------------------------------------------------------------------===//
// DynamicSliceOp
//===----------------------------------------------------------------------===//

void DynamicSliceOp::getCanonicalizationPatterns(
    OwningRewritePatternList& results, MLIRContext* context) {
  results.insert<DynamicSliceToSlice>(context);
}

//===----------------------------------------------------------------------===//
// InfeedOp
//===----------------------------------------------------------------------===//

// Checks that the result type is of the form `tuple< any_type, token >`.
static LogicalResult Verify(InfeedOp op) {
  auto result_ty = op.getResult().getType().cast<TupleType>();
  auto subtypes = result_ty.getTypes();
  if (subtypes.size() != 2)
    return op.emitOpError()
           << "result is expected to be a tuple of size 2, but got "
           << subtypes.size();
  if (!subtypes[1].isa<TokenType>())
    return op.emitOpError() << "second element of result tuple is expected to "
                               "be of token type, but got "
                            << subtypes[1];
  return success();
}

//===----------------------------------------------------------------------===//
// MapOp
//===----------------------------------------------------------------------===//

static LogicalResult Verify(MapOp op) {
  // Checks if the number of `operands` match the arity of the map `computation`
  // region.
  auto& computation_block = op.computation().front();
  auto computation_args = computation_block.getArguments();
  if (op.operands().size() != computation_args.size())
    return op.emitOpError()
           << "expects number of operands to match the arity "
              "of map computation, but got: "
           << op.operands().size() << " and " << computation_args.size();

  // The parameters of computation should all be scalars and match the element
  // type of operands.
  auto operand_type = op.operands()[0].getType().cast<TensorType>();
  auto operand_elem_ty = operand_type.getElementType();

  for (auto indexed_arg : llvm::enumerate(computation_args)) {
    auto arg_type = indexed_arg.value().getType().dyn_cast<TensorType>();
    if (!arg_type || arg_type.getRank() != 0)
      return op.emitOpError()
             << "computation arguments must be 0-rank tensor, but got: arg #"
             << indexed_arg.index() << " of type "
             << indexed_arg.value().getType();
    if (arg_type.getElementType() != operand_elem_ty) {
      return op.emitOpError()
             << "element type of operands and computation arguments must "
                "match, but got: "
             << operand_elem_ty << " and " << arg_type.getElementType();
    }
  }

  // Mapped computation must return single output
  auto computation_outputs = computation_block.getTerminator()->getOperands();
  if (computation_outputs.size() != 1)
    return op.emitOpError()
           << "computation must return single output, but got: "
           << computation_outputs.size();

  // The output of computation must be scalar and have the same element type
  // as op result.
  auto computation_output_type =
      computation_outputs[0].getType().dyn_cast<TensorType>();
  if (!computation_output_type || computation_output_type.getRank() != 0)
    return op.emitOpError()
           << "computation must return 0-rank tensor, but got: "
           << computation_outputs[0].getType();

  auto result_type = op.getType().cast<TensorType>();
  if (computation_output_type.getElementType() != result_type.getElementType())
    return op.emitOpError() << "element type of result and computation output "
                               "must match, but got: "
                            << result_type.getElementType() << " and "
                            << computation_output_type.getElementType();

  // Checks that the requested map dimension numbers are monotonically
  // increasing.
  auto values = op.dimensions().getValues<int64_t>();
  auto dimensions = std::vector<int64_t>{values.begin(), values.end()};
  for (int i = 0; i < dimensions.size(); ++i) {
    if (dimensions[i] != i)
      return op.emitOpError() << "requires monotonically increasing dimension "
                                 "numbers, but got: "
                              << op.dimensions();
  }

  // Checks that number of dimensions of operands matches the size of
  // `dimensions` since we currently only support mapping across all
  // dimensions: i.e., scalar map functions.
  if (operand_type.hasRank()) {
    if (dimensions.size() != operand_type.getShape().size())
      return op.emitOpError()
             << "applied to a subset of dimensions currently not supported: "
                "operand dimensions = "
             << operand_type.getShape().size()
             << ", requested map dimensions size = " << dimensions.size();
  }

  return success();
}

//===----------------------------------------------------------------------===//
// RecvOp
//===----------------------------------------------------------------------===//

// Checks that the result type is of the form `tuple<any_type, xla_hlo::token>`
static LogicalResult Verify(RecvOp op) {
  auto result_ty = op.getResult().getType().cast<TupleType>();
  auto subtypes = result_ty.getTypes();
  if (subtypes.size() != 2)
    return op.emitOpError()
           << "result is expected to be a tuple of size 2, but got "
           << subtypes.size();
  if (!subtypes[1].isa<TokenType>())
    return op.emitOpError() << "second element of result tuple is expected to "
                               "be of token type, but got "
                            << subtypes[1];
  return success();
}

//===----------------------------------------------------------------------===//
// CopyOp
//===----------------------------------------------------------------------===//

OpFoldResult CopyOp::fold(ArrayRef<Attribute> operands) { return getOperand(); }

//===----------------------------------------------------------------------===//
// ReshapeOp
//===----------------------------------------------------------------------===//

OpFoldResult ReshapeOp::fold(ArrayRef<Attribute> operands) {
  if (getOperand().getType() == getType()) {
    return getOperand();
  }

  if (auto prev_op =
          dyn_cast_or_null<ReshapeOp>(getOperand().getDefiningOp())) {
    setOperand(prev_op.getOperand());
    return getResult();
  }

  if (auto elements = operands.front().dyn_cast_or_null<DenseElementsAttr>()) {
    return elements.reshape(getResult().getType().cast<ShapedType>());
  }

  return {};
}

//===----------------------------------------------------------------------===//
// ReverseOp
//===----------------------------------------------------------------------===//

OpFoldResult ReverseOp::fold(ArrayRef<Attribute> operands) {
  // No dimensions to reverse.
  if (dimensions().getNumElements() == 0) return operand();
  return nullptr;
}

//===----------------------------------------------------------------------===//
// ReduceOp
//===----------------------------------------------------------------------===//

// Returns the result type after reducing operand of the given type across the
// specified dimensions.
static TensorType GetReduceResultType(Type operand_ty,
                                      DenseIntElementsAttr dimensions,
                                      Builder* builder) {
  Type element_ty = getElementTypeOrSelf(operand_ty);

  auto ranked_ty = operand_ty.dyn_cast<RankedTensorType>();
  if (!ranked_ty) return UnrankedTensorType::get(element_ty);

  int64_t rank = ranked_ty.getRank();
  llvm::SmallVector<bool, 4> dims_mask(rank, false);
  for (int64_t dim : dimensions.getValues<int64_t>()) dims_mask[dim] = true;

  SmallVector<int64_t, 4> shape;
  for (int64_t i = 0; i < rank; ++i) {
    if (!dims_mask[i]) shape.push_back(ranked_ty.getDimSize(i));
  }

  return RankedTensorType::get(shape, element_ty);
}

void ReduceOp::build(Builder* builder, OperationState& state,
                     ValueRange operands, ValueRange init_values,
                     DenseIntElementsAttr dimensions) {
  SmallVector<Type, 1> result_ty;
  result_ty.reserve(operands.size());

  for (Value operand : operands) {
    result_ty.push_back(
        GetReduceResultType(operand.getType(), dimensions, builder));
  }
  build(builder, state, result_ty, operands, init_values, dimensions);
}

LogicalResult ReduceOp::fold(ArrayRef<Attribute> operands,
                             SmallVectorImpl<OpFoldResult>& results) {
  // No dimensions to reduce.
  if (dimensions().getNumElements() == 0) {
    for (Value input : this->operands()) {
      results.push_back(input);
    }
    return success();
  }
  return failure();
}

//===----------------------------------------------------------------------===//
// SelectOp
//===----------------------------------------------------------------------===//

static LogicalResult Verify(SelectOp op) {
  // TODO(jpienaar): Update to allow broadcastable and unranked inputs. This
  // corresponds to the client side HLO.
  return success();
}

// Makes it such that a SelectOp that is a non-root operation in a DRR infers
// the return type based on operand type.
LogicalResult SelectOp::inferReturnTypes(
    MLIRContext*, Optional<Location> location, ValueRange operands,
    ArrayRef<NamedAttribute> attributes, RegionRange regions,
    SmallVectorImpl<Type>& inferredReturnTypes) {
  auto x_type = operands[1].getType();
  auto y_type = operands[2].getType();
  auto x_tensor = x_type.cast<TensorType>();
  auto y_tensor = y_type.cast<TensorType>();

  // Check for type compatibility in the select op. This requires that the two
  // non-predicate operands:
  //   (a) have the same element type
  //   (b) have compatible shapes (i.e. the same shape and/or at least one
  //       dynamic shape)
  if (x_tensor.getElementType() != y_tensor.getElementType() ||
      failed(mlir::verifyCompatibleShape(x_type, y_type))) {
    return emitOptionalError(location, "incompatible operand types: ", x_type,
                             " and ", y_type);
  }

  // TODO(lucyfox): Support output shape inference when operands have compatible
  // shapes. (The output shape should be the most general of the operand shapes
  // at each dimension.) For now, handle the straightforward cases and fail
  // otherwise. When this is fully implemented, this logic should move into
  // reusable functionality in MLIR Core.
  Type output_type;
  if (x_type == y_type || !x_tensor.hasRank()) {
    output_type = x_type;
  } else if (!y_tensor.hasRank()) {
    output_type = y_type;
  } else {
    return emitOptionalError(location,
                             "currently unsupported operand types: ", x_type,
                             " and ", y_type);
  }
  inferredReturnTypes.assign({output_type});
  return success();
}

//===----------------------------------------------------------------------===//
// PadOp
//===----------------------------------------------------------------------===//

static LogicalResult Verify(PadOp op) {
  auto input_type = op.operand().getType().cast<RankedTensorType>();
  auto pad_type = op.padding_value().getType().cast<RankedTensorType>();

  if (pad_type.getRank() != 0) {
    return op.emitOpError(
        llvm::formatv("padding value type should be a rank-0 "
                      "tensor, is rank {0}",
                      pad_type.getRank()));
  }

  const auto& padding_low = op.edge_padding_low();
  if (padding_low.getType().getNumElements() != input_type.getRank()) {
    return op.emitOpError(llvm::formatv(
        "edge_padding_low length ({0}) must match operand rank ({1})",
        padding_low.getType().getNumElements(), input_type.getRank()));
  }

  const auto& padding_high = op.edge_padding_high();
  if (padding_high.getType().getNumElements() != input_type.getRank()) {
    return op.emitOpError(llvm::formatv(
        "edge_padding_high length ({0}) must match operand rank ({1})",
        padding_high.getType().getNumElements(), input_type.getRank()));
  }

  const auto& padding_interior = op.interior_padding();
  if (padding_interior.getType().getNumElements() != input_type.getRank()) {
    return op.emitOpError(llvm::formatv(
        "interior_padding length ({0}) must match operand rank ({1})",
        padding_interior.getType().getNumElements(), input_type.getRank()));
  }

  auto input_shape = input_type.getShape();
  auto output_shape =
      op.getResult().getType().cast<RankedTensorType>().getShape();
  if (input_shape.size() != output_shape.size()) {
    return op.emitOpError(
        llvm::formatv("operand rank ({0}) and result rank({0}) should match",
                      input_shape.size(), output_shape.size()));
  }

  for (int i = 0, e = input_shape.size(); i < e; i++) {
    int padding_low_val = padding_low.getValue<IntegerAttr>(i).getInt();
    int padding_high_val = padding_high.getValue<IntegerAttr>(i).getInt();
    int padding_interior_val =
        padding_interior.getValue<IntegerAttr>(i).getInt();
    int expected_output =
        input_shape[i] + padding_low_val + padding_high_val +
        std::max<int64_t>(input_shape[i] - 1, 0LL) * padding_interior_val;
    if (expected_output != output_shape[i]) {
      return op.emitOpError(llvm::formatv(
          "expected output shape's dimension #{0} to be {1} but found {2}", i,
          expected_output, output_shape[i]));
    }
  }

  return success();
}

//===----------------------------------------------------------------------===//
// ReshapeOp
//===----------------------------------------------------------------------===//

static LogicalResult Verify(ReshapeOp op) {
  auto operand_ty = op.operand().getType().cast<TensorType>();
  if (!operand_ty || !operand_ty.hasStaticShape()) return success();
  int64_t num_input_elements = operand_ty.getNumElements();

  auto out_ty = op.getType().cast<RankedTensorType>();
  if (out_ty && out_ty.hasStaticShape()) {
    int64_t num_output_elements = out_ty.getNumElements();
    if (num_input_elements != num_output_elements)
      return op.emitOpError()
             << "number of output elements (" << num_output_elements
             << ") doesn't match expected number of elements ("
             << num_input_elements << ")";
  }
  return success();
}

//===----------------------------------------------------------------------===//
// BinaryOps
//===----------------------------------------------------------------------===//

namespace {
// Gets the resulting type from a broadcast between two types.
static Type GetBroadcastType(Builder* builder, Type x, Type y,
                             Type element_type,
                             DenseIntElementsAttr broadcast_dimensions) {
  auto x_ranked = x.dyn_cast<RankedTensorType>();
  auto y_ranked = y.dyn_cast<RankedTensorType>();
  if (!x_ranked || !y_ranked) {
    return UnrankedTensorType::get(element_type);
  }

  auto shape_x = x_ranked.getShape();
  auto shape_y = y_ranked.getShape();

  if (shape_x.size() == shape_y.size()) {
    llvm::SmallVector<int64_t, 4> out_shape(shape_x.size());
    for (int i = 0; i < shape_x.size(); i++) {
      auto x_val = shape_x[i];
      auto y_val = shape_y[i];
      if (x_val == -1 || y_val == -1) {
        out_shape[i] = -1;
      } else {
        out_shape[i] = std::max(x_val, y_val);
      }
    }
    return RankedTensorType::get(out_shape, element_type);
  }

  // Return unranked tensor for invalid broadcast dimensions.
  if (!broadcast_dimensions) return UnrankedTensorType::get(element_type);

  auto shape_large = shape_x.size() > shape_y.size() ? shape_x : shape_y;
  auto shape_small = shape_x.size() <= shape_y.size() ? shape_x : shape_y;

  llvm::SmallVector<int64_t, 4> out_shape(shape_large.begin(),
                                          shape_large.end());

  // Update according to the broadcast dimensions.
  for (auto index_pair : llvm::enumerate(broadcast_dimensions.getIntValues())) {
    auto old_value = out_shape[index_pair.value().getSExtValue()];
    auto new_value = shape_small[index_pair.index()];
    if (old_value != -1 && (new_value == -1 || new_value > old_value)) {
      out_shape[index_pair.value().getSExtValue()] = new_value;
    }
  }

  return RankedTensorType::get(out_shape, element_type);
}
}  // namespace

#define BINARY_BUILDER(Op)                                                   \
  void Op::build(Builder* builder, OperationState& result, Value left,       \
                 Value right, DenseIntElementsAttr broadcast_dimensions) {   \
    auto type = GetBroadcastType(builder, left.getType().cast<ShapedType>(), \
                                 right.getType().cast<ShapedType>(),         \
                                 getElementTypeOrSelf(right.getType()),      \
                                 broadcast_dimensions);                      \
    return Op::build(builder, result, type, left, right,                     \
                     broadcast_dimensions);                                  \
  }

BINARY_BUILDER(AddOp);
BINARY_BUILDER(AndOp);
BINARY_BUILDER(Atan2Op);
BINARY_BUILDER(DivOp);
BINARY_BUILDER(MaxOp);
BINARY_BUILDER(MinOp);
BINARY_BUILDER(MulOp);
BINARY_BUILDER(OrOp);
BINARY_BUILDER(PowOp);
BINARY_BUILDER(RemOp);
BINARY_BUILDER(ShiftLeftOp);
BINARY_BUILDER(ShiftRightArithmeticOp);
BINARY_BUILDER(ShiftRightLogicalOp);
BINARY_BUILDER(SubOp);
BINARY_BUILDER(XorOp);

#undef BINARY_BUILDER

//===----------------------------------------------------------------------===//
// SliceOp
//===----------------------------------------------------------------------===//

void SliceOp::build(Builder* builder, OperationState& result, Value operand,
                    DenseIntElementsAttr start_indices,
                    DenseIntElementsAttr limit_indices,
                    DenseIntElementsAttr strides) {
  return build(
      builder, result,
      InferOutputTypes(builder, operand, start_indices, limit_indices, strides),
      operand, start_indices, limit_indices, strides);
}

// Returns output dimension size for slice result for the given arguments.
// Returns -1 if arguments are illegal.
static int64_t InferSliceDim(int64_t input_dim, int64_t start, int64_t end,
                             int64_t stride) {
  if (input_dim == -1 || start < 0 || start > end || end > input_dim ||
      stride == 0)
    return -1;

  return llvm::divideCeil(end - start, stride);
}

Type SliceOp::InferOutputTypes(Builder* builder, Value operand,
                               DenseIntElementsAttr start_indices,
                               DenseIntElementsAttr limit_indices,
                               DenseIntElementsAttr strides) {
  Type ty = operand.getType();
  RankedTensorType ranked_ty = ty.dyn_cast<RankedTensorType>();
  if (!ranked_ty) return ty;
  int64_t rank = ranked_ty.getRank();

  // Illegal attributes.
  ShapedType attr_ty = start_indices.getType();
  if (attr_ty.getRank() != 1 || attr_ty.getNumElements() != rank ||
      !attr_ty.getElementType().isSignlessInteger(64) ||
      limit_indices.getType() != attr_ty || strides.getType() != attr_ty)
    return ty;

  SmallVector<int64_t, 4> start(start_indices.getValues<int64_t>());
  SmallVector<int64_t, 4> limit(limit_indices.getValues<int64_t>());
  SmallVector<int64_t, 4> stride_vals(strides.getValues<int64_t>());

  SmallVector<int64_t, 4> shape;
  shape.reserve(rank);
  for (int64_t i = 0, e = rank; i != e; i++) {
    shape.push_back(InferSliceDim(ranked_ty.getDimSize(i), start[i], limit[i],
                                  stride_vals[i]));
  }
  return RankedTensorType::get(shape, ranked_ty.getElementType());
}

//===----------------------------------------------------------------------===//
// SortOp
//===----------------------------------------------------------------------===//

void SortOp::build(Builder* builder, OperationState& state, ValueRange operands,
                   int64_t dimension, bool is_stable) {
  state.addOperands(operands);
  state.addAttribute("dimension", builder->getI64IntegerAttr(dimension));
  state.addAttribute("is_stable", builder->getBoolAttr(dimension));

  SmallVector<Type, 2> element_types;
  element_types.reserve(operands.size());
  for (Value operand : operands) element_types.push_back(operand.getType());
  state.addTypes(builder->getTupleType(element_types));

  state.addRegion();
}

static LogicalResult Verify(SortOp op) {
  Operation::operand_range operands = op.operands();
  if (operands.empty()) return op.emitOpError("requires at least one input");

  // TODO(antiagainst): verify partionally dynamic shapes
  if (llvm::all_of(operands, [](Value operand) {
        return operand.getType().cast<ShapedType>().hasRank();
      })) {
    ArrayRef<int64_t> input_shape =
        (*operands.begin()).getType().cast<ShapedType>().getShape();

    if (llvm::any_of(llvm::drop_begin(operands, 1), [&](Value operand) {
          return operand.getType().cast<ShapedType>().getShape() != input_shape;
        }))
      return op.emitOpError("requires all inputs to have the same dimensions");

    int64_t rank = input_shape.size();
    int64_t cmp_dim = op.dimension().getSExtValue();
    if (cmp_dim < -rank || cmp_dim >= rank)
      return op.emitOpError("dimension attribute value must be in range [-")
             << rank << ", " << rank << "), but found " << cmp_dim;
  }

  Block& block = op.comparator().front();
  size_t num_operands = op.getOperation()->getNumOperands();
  if (block.getNumArguments() != 2 * num_operands)
    return op.emitOpError("comparator block should have ")
           << 2 * num_operands << " arguments";

  for (auto indexed_operand : llvm::enumerate(operands)) {
    int index = indexed_operand.index();
    Type element_type =
        indexed_operand.value().getType().cast<ShapedType>().getElementType();
    Type tensor_type = RankedTensorType::get({}, element_type);
    for (int i : {2 * index, 2 * index + 1}) {
      Type arg_type = block.getArgument(i).getType();
      if (arg_type != tensor_type)
        return op.emitOpError("comparator block argument #")
               << i << " should be of type " << tensor_type << " but got "
               << arg_type;
    }
  }

  return success();
}

//===----------------------------------------------------------------------===//
// TransposeOp
//===----------------------------------------------------------------------===//

OpFoldResult TransposeOp::fold(ArrayRef<Attribute> operands) {
  for (auto it : llvm::enumerate(permutation().getValues<APInt>())) {
    if (it.index() != it.value()) {
      return {};
    }
  }
  return getOperand();
}

static LogicalResult Verify(TransposeOp op) {
  // permutation is an attribute of the op so it has static shape.
  auto permutationType = op.permutation().getType();
  auto permutationRank = permutationType.getRank();
  if (permutationRank != 1) {
    return op.emitOpError(llvm::formatv(
        "permutation has rank {0} instead of rank 1", permutationRank));
  }
  auto permutationSize = permutationType.getNumElements();

  auto operandType = op.operand().getType().dyn_cast<RankedTensorType>();
  if (operandType) {
    auto operandRank = operandType.getRank();
    if (operandRank != permutationSize) {
      return op.emitOpError(llvm::formatv(
          "operand rank ({0}) does not match permutation size ({1})",
          operandRank, permutationSize));
    }
  }

  auto resultType = op.getResult().getType().dyn_cast<RankedTensorType>();
  if (resultType) {
    auto resultRank = resultType.getRank();
    if (resultRank != permutationSize) {
      return op.emitOpError(llvm::formatv(
          "result rank ({0}) does not match permutation size ({1})", resultRank,
          permutationSize));
    }
  }

  if (!resultType || !operandType) return success();

  auto operandRank = operandType.getRank();
  SmallVector<int64_t, 4> expectedShape(operandRank);
  for (int i = 0; i != operandRank; ++i) {
    auto permutedDim = op.permutation().getValue<IntegerAttr>(i).getInt();
    expectedShape[i] = operandType.getDimSize(permutedDim);
  }

  auto expectedType =
      RankedTensorType::get(expectedShape, resultType.getElementType());
  if (failed(verifyCompatibleShape(resultType, expectedType))) {
    return op.emitOpError(llvm::formatv(
        "result type {0} is incompatible with the expected type {1}",
        resultType, expectedType));
  }

  return success();
}

//===----------------------------------------------------------------------===//
// TriangularSolveOp
//===----------------------------------------------------------------------===//

static LogicalResult Verify(TriangularSolveOp op) {
  auto a_type = op.a().getType().dyn_cast<RankedTensorType>();

  // Skip verifier if a is unranked tensor.
  if (!a_type) return success();

  // Check that a should have rank >= 2
  auto a_rank = a_type.getRank();
  if (a_rank < 2)
    return op.emitOpError()
           << "operand 'a' must have rank >= 2, but got " << a_type;

  // The two minor dimensions of a must have same size.
  if (a_type.getDimSize(a_rank - 2) != a_type.getDimSize(a_rank - 1))
    return op.emitOpError() << "two minor dimensions of operand 'a' must have "
                               "equal size, but got "
                            << a_type;

  auto b_type = op.b().getType().dyn_cast<RankedTensorType>();
  // If b is unranked skip remaining checks.
  if (!b_type) return success();

  // Check that a and b have same rank.
  auto b_rank = b_type.getRank();
  if (a_rank != b_rank)
    return op.emitOpError() << "operands must have equal rank, but got "
                            << a_type << " and " << b_type;

  // The shared dimension of a and b should match.
  if (a_type.getDimSize(a_rank - 1) !=
      b_type.getDimSize(b_rank - (op.left_side() ? 2 : 1)))
    return op.emitOpError() << "shared dimension of operands 'a' and 'b' does "
                               "not match, but got "
                            << a_type << " and " << b_type;

  // The leading batch dimensions of a and b must be equal.
  auto a_batch_dims = a_type.getShape().drop_back(2);
  auto b_batch_dims = b_type.getShape().drop_back(2);
  if (a_batch_dims != b_batch_dims)
    return op.emitOpError()
           << "leading batch dimensions of the operands must be same, but got "
           << a_type << " and " << b_type;

  // Result and argument b must have same shape.
  auto result_type = op.getType().dyn_cast<RankedTensorType>();
  if (!result_type) return success();
  if (result_type != b_type)
    return op.emitOpError()
           << "result and operand 'b' must have same shape, but got "
           << result_type << " and " << b_type;
  return success();
}

//===----------------------------------------------------------------------===//
// GetTupleElementOp
//===----------------------------------------------------------------------===//

void GetTupleElementOp::build(Builder* builder, OperationState& result,
                              Value tuple, int32_t index) {
  if (auto tuple_type = tuple.getType().dyn_cast<TupleType>()) {
    auto element_type = tuple_type.getType(index);
    build(builder, result, element_type, tuple,
          builder->getI32IntegerAttr(index));
    return;
  }

  build(builder, result, tuple.getType(), tuple,
        builder->getI32IntegerAttr(index));
}

//===----------------------------------------------------------------------===//
// TupleOp
//===----------------------------------------------------------------------===//

void TupleOp::build(Builder* builder, OperationState& result,
                    ValueRange values) {
  SmallVector<Type, 4> types;
  types.reserve(values.size());
  for (auto val : values) {
    types.push_back(val.getType());
  }

  build(builder, result, builder->getTupleType(types), values);
}

//===----------------------------------------------------------------------===//
// UnaryEinsumOp
//===----------------------------------------------------------------------===//

void UnaryEinsumOp::getCanonicalizationPatterns(
    OwningRewritePatternList& results, MLIRContext* context) {
  results.insert<UnaryEinsumToEinsum>(context);
}

//===----------------------------------------------------------------------===//
// CompareOp
//===----------------------------------------------------------------------===//

void CompareOp::build(Builder* builder, OperationState& result, Value lhs,
                      Value rhs, DenseIntElementsAttr broadcast_dimensions,
                      StringAttr comparison_direction) {
  auto new_type = GetBroadcastType(builder, lhs.getType(), rhs.getType(),
                                   builder->getI1Type(), broadcast_dimensions);
  build(builder, result, new_type, lhs, rhs, broadcast_dimensions,
        comparison_direction);
}

#define GET_OP_CLASSES
#include "tensorflow/compiler/mlir/xla/ir/hlo_ops.cc.inc"

//===----------------------------------------------------------------------===//
// xla_hlo Dialect Interfaces
//===----------------------------------------------------------------------===//

namespace {
struct HLOInlinerInterface : public DialectInlinerInterface {
  using DialectInlinerInterface::DialectInlinerInterface;
  // We don't have any special restrictions on what can be inlined into
  // destination regions (e.g. while/conditional bodies). Always allow it.
  bool isLegalToInline(Region* dest, Region* src,
                       BlockAndValueMapping& valueMapping) const final {
    return true;
  }
  // Operations in xla_hlo dialect are always legal to inline since they are
  // pure.
  bool isLegalToInline(Operation*, Region*, BlockAndValueMapping&) const final {
    return true;
  }
};
}  // end anonymous namespace

//===----------------------------------------------------------------------===//
// xla_hlo Dialect Constructor
//===----------------------------------------------------------------------===//

XlaHloDialect::XlaHloDialect(MLIRContext* context)
    : Dialect(getDialectNamespace(), context) {
  addOperations<
#define GET_OP_LIST
#include "tensorflow/compiler/mlir/xla/ir/hlo_ops.cc.inc"
      >();
  addInterfaces<HLOInlinerInterface>();
  addTypes<TokenType>();
  // Support unknown operations because not all XLA operations are registered.
  // allowUnknownOperations();
}

Type XlaHloDialect::parseType(DialectAsmParser& parser) const {
  StringRef data_type;
  if (parser.parseKeyword(&data_type)) return Type();

  if (data_type == "token") return TokenType::get(getContext());
  parser.emitError(parser.getNameLoc())
      << "unknown xla_hlo type: " << data_type;
  return nullptr;
}

void XlaHloDialect::printType(Type type, DialectAsmPrinter& os) const {
  if (type.isa<TokenType>()) {
    os << "token";
    return;
  }
  os << "<unknown xla_hlo type>";
}

//===----------------------------------------------------------------------===//
// Shape inference
//===----------------------------------------------------------------------===//

LogicalResult deriveShapeFromFirstOperand(
    OpBuilder* builder, Operation* op,
    SmallVectorImpl<Value>* reifiedReturnShapes) {
  Value operand = op->getOperand(0);
  ShapedType operand_type = operand.getType().dyn_cast<ShapedType>();
  if (!operand_type) {
    op->emitOpError() << "first operand is not a shaped type";
    return failure();
  }
  auto loc = op->getLoc();
  SmallVector<Value, 4> shape_values;
  shape_values.reserve(operand_type.getRank());
  auto shape_scalar_type = builder->getIntegerType(64);
  for (auto element : llvm::enumerate(operand_type.getShape())) {
    if (element.value() == ShapedType::kDynamicSize) {
      Value dim = builder->create<DimOp>(loc, operand, element.index());
      shape_values.push_back(
          builder->create<IndexCastOp>(loc, dim, shape_scalar_type));
    } else {
      shape_values.push_back(builder->create<ConstantOp>(
          loc, builder->getI64IntegerAttr(element.value())));
    }
  }
  *reifiedReturnShapes =
      SmallVector<Value, 1>{builder->create<ScalarsToDimensionTensorOp>(
          loc,
          RankedTensorType::get({operand_type.getRank()}, shape_scalar_type),
          shape_values)};
  return success();
}

}  // namespace xla_hlo
}  // namespace mlir
