/* Copyright 2024 The OpenXLA Authors.

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
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

#include "absl/algorithm/container.h"
#include "absl/base/optimization.h"
#include "absl/log/check.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/LogicalResult.h"
#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Affine/LoopUtils.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/IR/AffineExpr.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/ImplicitLocOpBuilder.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/IR/Types.h"
#include "mlir/IR/Value.h"
#include "mlir/IR/ValueRange.h"
#include "mlir/Interfaces/DataLayoutInterfaces.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Support/LLVM.h"
#include "mlir/Support/LogicalResult.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include "xla/backends/gpu/codegen/ir/xla_gpu_ops.h"
#include "xla/backends/gpu/codegen/transforms/passes.h"
#include "xla/codegen/ir/xla_ops.h"
#include "xla/hlo/analysis/indexing_map.h"

namespace xla {
namespace gpu {
namespace {

using mlir::AffineBinaryOpExpr;
using mlir::AffineConstantExpr;
using mlir::AffineDimExpr;
using mlir::AffineExpr;
using mlir::AffineExprKind;
using mlir::AffineMap;
using mlir::AffineSymbolExpr;
using mlir::ImplicitLocOpBuilder;
using mlir::IndexType;
using mlir::LogicalResult;
using mlir::MLIRContext;
using mlir::ModuleOp;
using mlir::OpRewritePattern;
using mlir::PatternRewriter;
using mlir::SmallVector;
using mlir::Type;
using mlir::Value;
using mlir::ValueRange;
using mlir::affine::AffineApplyOp;

namespace arith = mlir::arith;

#define GEN_PASS_DEF_SIMPLIFYAFFINEPASS
#include "xla/backends/gpu/codegen/transforms/passes.h.inc"

int Distance(ImplicitLocOpBuilder& builder, Value a) {
  auto* block = builder.getInsertionBlock();
  auto* parent = a.getParentBlock();
  int distance = 0;
  while (block && block != parent) {
    ++distance;
    block = block->getParentOp()->getBlock();
  }
  return distance;
}

void CollectArgs(AffineExpr expr, AffineExprKind kind,
                 llvm::SmallVector<AffineExpr>& ret) {
  if (auto bin_op = mlir::dyn_cast<AffineBinaryOpExpr>(expr)) {
    if (bin_op.getKind() == kind) {
      CollectArgs(bin_op.getLHS(), kind, ret);
      CollectArgs(bin_op.getRHS(), kind, ret);
      return;
    }
  }
  ret.push_back(expr);
}

struct ExpressionEvaluator {
  ExpressionEvaluator(ImplicitLocOpBuilder& builder, unsigned dim_count,
                      ValueRange operands)
      : builder(builder), operands(operands) {
    for (int i = 0; i < dim_count; ++i) {
      dim_distances.push_back(Distance(builder, operands[i]));
    }
    for (int i = dim_count; i < operands.size(); ++i) {
      sym_distances.push_back(Distance(builder, operands[i]));
    }
  }

  // Returns the distance (in basic blocks) from the insertion point to the
  // values used in the given expression.
  int ExprDistance(AffineExpr e, int depth = 0) {
    if (auto dim = mlir::dyn_cast<AffineDimExpr>(e)) {
      return dim_distances[dim.getPosition()];
    }
    if (auto sym = mlir::dyn_cast<AffineSymbolExpr>(e)) {
      return sym_distances[sym.getPosition()];
    }
    if (auto binop = mlir::dyn_cast<AffineBinaryOpExpr>(e)) {
      return std::min(ExprDistance(binop.getLHS(), depth + 1),
                      ExprDistance(binop.getRHS(), depth + 1));
    }
    if (depth == 0) {
      // Top-level constant. Always add these last.
      return std::numeric_limits<int>::min();
    }
    // Nested constant. Ignore these for distances.
    return std::numeric_limits<int>::max();
  }

  Value EvaluateExpression(AffineExpr expr);

  template <typename Op>
  Value EvaluateAddMul(AffineExpr expr);

  ImplicitLocOpBuilder& builder;
  ValueRange operands;
  SmallVector<int> dim_distances;
  SmallVector<int> sym_distances;
};

template <typename Op>
Value ExpressionEvaluator::EvaluateAddMul(AffineExpr expr) {
  llvm::SmallVector<AffineExpr> args;
  CollectArgs(expr, expr.getKind(), args);
  // Sort the args so that the ones that are closest to the insertion point
  // are evaluated last - this improves LICM.
  llvm::stable_sort(args, [&](AffineExpr a, AffineExpr b) {
    int dist_a = ExprDistance(a);
    int dist_b = ExprDistance(b);
    return dist_a > dist_b;
  });

  Value result = nullptr;
  for (auto arg : args) {
    Value arg_evaluated = EvaluateExpression(arg);
    if (result) {
      result = builder.create<Op>(result, arg_evaluated);
    } else {
      result = arg_evaluated;
    }
  }

  return result;
}

Value ExpressionEvaluator::EvaluateExpression(AffineExpr expr) {
  if (auto bin_op = mlir::dyn_cast<AffineBinaryOpExpr>(expr)) {
    switch (expr.getKind()) {
      case AffineExprKind::Add:
        return EvaluateAddMul<arith::AddIOp>(expr);
      case AffineExprKind::Mul:
        return EvaluateAddMul<arith::MulIOp>(expr);
      case AffineExprKind::Mod:
        return builder.create<arith::RemUIOp>(
            EvaluateExpression(bin_op.getLHS()),
            EvaluateExpression(bin_op.getRHS()));
      case AffineExprKind::FloorDiv:
        return builder.create<arith::DivUIOp>(
            EvaluateExpression(bin_op.getLHS()),
            EvaluateExpression(bin_op.getRHS()));
      default:
        ABSL_UNREACHABLE();
    }
  }
  switch (expr.getKind()) {
    case AffineExprKind::Constant:
      return builder.create<arith::ConstantIndexOp>(
          mlir::cast<AffineConstantExpr>(expr).getValue());
    case AffineExprKind::DimId:
      return operands[mlir::cast<AffineDimExpr>(expr).getPosition()];
    case AffineExprKind::SymbolId:
      return operands[dim_distances.size() +
                      mlir::cast<AffineSymbolExpr>(expr).getPosition()];
    default:
      ABSL_UNREACHABLE();
  }
}

bool IsLoweringSupported(AffineExpr expr, RangeEvaluator& range_evaluator) {
  auto bin_op = llvm::dyn_cast<AffineBinaryOpExpr>(expr);
  if (!bin_op) {
    return true;
  }
  // Mod and div can be lowered if their LHS is >= 0 and their RHS is a
  // constant.
  if (expr.getKind() == AffineExprKind::Mod ||
      expr.getKind() == AffineExprKind::FloorDiv) {
    if (!range_evaluator.IsAlwaysPositiveOrZero(bin_op.getLHS()) ||
        !range_evaluator.ComputeExpressionRange(bin_op.getRHS()).IsPoint()) {
      return false;
    }
  }
  if (expr.getKind() == AffineExprKind::CeilDiv) {
    return false;
  }
  return IsLoweringSupported(bin_op.getLHS(), range_evaluator) &&
         IsLoweringSupported(bin_op.getRHS(), range_evaluator);
}

struct RewriteAffineApply : OpRewritePattern<mlir::affine::AffineApplyOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(mlir::affine::AffineApplyOp op,
                                PatternRewriter& rewriter) const override {
    AffineMap affine_map = op.getAffineMap();
    std::vector<IndexingMap::Variable> dim_ranges(affine_map.getNumDims());
    std::vector<IndexingMap::Variable> symbol_ranges(
        affine_map.getNumSymbols());

    for (int i = 0; i < affine_map.getNumInputs(); ++i) {
      if (auto range = GetRange(op->getOperand(i))) {
        if (i >= dim_ranges.size()) {
          symbol_ranges[i - dim_ranges.size()] = IndexingMap::Variable{*range};
        } else {
          dim_ranges[i] = IndexingMap::Variable{*range};
        }
      } else {
        return rewriter.notifyMatchFailure(op, "failed to deduce range");
      }
    }

    IndexingMap indexing_map(affine_map, std::move(dim_ranges),
                             std::move(symbol_ranges),
                             /*rt_vars=*/{});
    indexing_map.Simplify();
    auto result_expr = indexing_map.GetAffineMap().getResult(0);

    ImplicitLocOpBuilder b(op.getLoc(), rewriter);
    RangeEvaluator range_evaluator = indexing_map.GetRangeEvaluator();
    if (!IsLoweringSupported(result_expr, range_evaluator)) {
      return rewriter.notifyMatchFailure(op,
                                         "unable to lower the affine apply");
    }
    b.setInsertionPoint(op);
    auto result = ExpressionEvaluator(b, indexing_map.GetDimensionCount(),
                                      op->getOperands())
                      .EvaluateExpression(result_expr);
    rewriter.replaceOp(op, result);
    return mlir::success();
  }
};

struct RewriteApplyIndexingOp : OpRewritePattern<ApplyIndexingOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(ApplyIndexingOp op,
                                PatternRewriter& rewriter) const override {
    auto indexing_map = op.getIndexingMap();
    indexing_map.Simplify();
    auto affine_map = indexing_map.GetAffineMap();
    int64_t dim_count = indexing_map.GetDimensionCount();
    auto operands = op->getOperands();

    ImplicitLocOpBuilder b(op.getLoc(), rewriter);
    RangeEvaluator range_evaluator = indexing_map.GetRangeEvaluator();

    b.setInsertionPoint(op);
    SmallVector<Value, 4> results;
    results.reserve(affine_map.getNumResults());
    for (unsigned i = 0; i < affine_map.getNumResults(); ++i) {
      AffineExpr result_expr = affine_map.getResult(i);
      // If the expression cannot be lowered, we convert it to affine.apply,
      // since it supports more expression types.
      if (IsLoweringSupported(result_expr, range_evaluator)) {
        results.push_back(ExpressionEvaluator(b, dim_count, operands)
                              .EvaluateExpression(result_expr));
      } else {
        results.push_back(
            b.create<AffineApplyOp>(affine_map.getSubMap({i}), operands));
      }
    }
    rewriter.replaceOp(op, results);
    return mlir::success();
  }
};

// Rewrites a binary elementwise op on 'index' types to a binary elementwise op
// on integers with the specified bit width.
template <typename BinaryElementwiseOp>
class RewriteIndexBinaryElementwiseOp
    : public OpRewritePattern<BinaryElementwiseOp> {
 public:
  using OpRewritePattern<BinaryElementwiseOp>::OpRewritePattern;
  RewriteIndexBinaryElementwiseOp(MLIRContext* context, uint64_t index_bitwidth)
      : OpRewritePattern<BinaryElementwiseOp>(context, 2),
        index_bitwidth_(index_bitwidth) {}

  LogicalResult matchAndRewrite(BinaryElementwiseOp op,
                                PatternRewriter& rewriter) const override {
    CHECK_EQ(op->getNumOperands(), 2);
    CHECK_EQ(op->getNumResults(), 1);

    auto is_index = [](Type type) { return llvm::isa<mlir::IndexType>(type); };

    bool operands_are_indices = absl::c_all_of(op->getOperandTypes(), is_index);
    bool result_is_index = is_index(op->getResultTypes().front());

    if (!operands_are_indices || !result_is_index) {
      return rewriter.notifyMatchFailure(
          op, "operation has non-index operands or results");
    }

    ImplicitLocOpBuilder b(op->getLoc(), rewriter);
    b.setInsertionPoint(op);

    Type index_type = IndexType::get(op->getContext());
    Type dst_type = b.getIntegerType(index_bitwidth_);

    auto lhs = b.create<arith::IndexCastUIOp>(dst_type, op->getOperand(0));
    auto rhs = b.create<arith::IndexCastUIOp>(dst_type, op->getOperand(1));
    auto new_op = b.create<BinaryElementwiseOp>(lhs, rhs);

    rewriter.replaceAllUsesWith(
        op.getResult(),
        b.create<arith::IndexCastUIOp>(index_type, new_op.getResult()));

    return mlir::success();
  }

 private:
  uint64_t index_bitwidth_;
};

struct SimplifyAffinePass
    : public impl::SimplifyAffinePassBase<SimplifyAffinePass> {
 public:
  void runOnOperation() override {
    MLIRContext* ctx = &getContext();
    mlir::RewritePatternSet patterns(ctx);
    patterns.add<RewriteAffineApply, RewriteApplyIndexingOp>(ctx);
    mlir::GreedyRewriteConfig config1;
    // There's no point simplifying more than once.
    config1.strictMode = mlir::GreedyRewriteStrictness::ExistingOps;
    if (mlir::failed(mlir::applyPatternsGreedily(
            getOperation(), std::move(patterns), config1))) {
      signalPassFailure();
    }

    auto parent_module = getOperation()->getParentOfType<ModuleOp>();
    mlir::DataLayout layout(parent_module);
    std::optional<uint64_t> index_bitwidth =
        layout.getTypeIndexBitwidth(IndexType::get(ctx));
    CHECK(index_bitwidth.has_value());

    mlir::RewritePatternSet index_patterns(ctx);
    index_patterns.add<RewriteIndexBinaryElementwiseOp<arith::AddIOp>,
                       RewriteIndexBinaryElementwiseOp<arith::DivUIOp>,
                       RewriteIndexBinaryElementwiseOp<arith::MulIOp>,
                       RewriteIndexBinaryElementwiseOp<arith::RemUIOp>,
                       RewriteIndexBinaryElementwiseOp<arith::SubIOp>>(
        ctx, *index_bitwidth);
    arith::IndexCastUIOp::getCanonicalizationPatterns(index_patterns, ctx);

    mlir::GreedyRewriteConfig config2;
    config2.maxIterations = mlir::GreedyRewriteConfig::kNoLimit;
    if (mlir::failed(mlir::applyPatternsAndFoldGreedily(
            getOperation(), std::move(index_patterns), config2))) {
      signalPassFailure();
    }
  }
};

}  // namespace

std::unique_ptr<mlir::Pass> CreateSimplifyAffinePass() {
  return std::make_unique<SimplifyAffinePass>();
}

}  // namespace gpu
}  // namespace xla
