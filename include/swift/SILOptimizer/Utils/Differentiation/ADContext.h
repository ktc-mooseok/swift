//===--- ADContext.h - Differentiation Context ----------------*- C++ -*---===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2019 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Per-module contextual information for the differentiation transform.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SILOPTIMIZER_UTILS_DIFFERENTIATION_ADCONTEXT_H
#define SWIFT_SILOPTIMIZER_UTILS_DIFFERENTIATION_ADCONTEXT_H

#include "swift/AST/DiagnosticsSIL.h"
#include "swift/AST/Expr.h"
#include "swift/AST/SynthesizedFileUnit.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SILOptimizer/Utils/Differentiation/Common.h"
#include "swift/SILOptimizer/Utils/Differentiation/DifferentiationInvoker.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"

namespace swift {

class ASTContext;
class DifferentiableFunctionExpr;
class DifferentiableFunctionInst;
class FuncDecl;
class SILDifferentiabilityWitness;
class SILFunction;
class SILModuleTransform;
class SILModule;
class SILPassManager;

namespace autodiff {

/// Stores `apply` instruction information calculated by VJP generation.
struct NestedApplyInfo {
  /// The differentiation indices that are used to differentiate this `apply`
  /// instruction.
  SILAutoDiffIndices indices;
  /// The original pullback type before reabstraction. `None` if the pullback
  /// type is not reabstracted.
  Optional<CanSILFunctionType> originalPullbackType;
};

/// Per-module contextual information for the Differentiation pass.
class ADContext {
private:
  /// Reference to the main transform.
  SILModuleTransform &transform;

  /// The module where Differentiation is performed on.
  SILModule &module;

  /// AST context.
  ASTContext &astCtx = module.getASTContext();

  /// Shared pass manager.
  SILPassManager &passManager;

  /// A synthesized file unit.
  SynthesizedFileUnit *synthesizedFile = nullptr;

  /// The worklist (stack) of `differentiable_function` instructions to be
  /// processed.
  llvm::SmallVector<DifferentiableFunctionInst *, 32>
      differentiableFunctionInsts;

  /// The set of `differentiable_function` instructions that have been
  /// processed. Used to avoid reprocessing invalidated instructions.
  /// NOTE(TF-784): if we use `CanonicalizeInstruction` subclass to replace
  /// `ADContext::processDifferentiableFunctionInst`, this field may be removed.
  llvm::SmallPtrSet<DifferentiableFunctionInst *, 32>
      processedDifferentiableFunctionInsts;

  /// Mapping from witnesses to invokers.
  /// `SmallMapVector` is used for deterministic insertion order iteration.
  llvm::SmallMapVector<SILDifferentiabilityWitness *, DifferentiationInvoker,
                       32>
      invokers;

  /// Mapping from `differentiable_function` instructions to result indices.
  llvm::DenseMap<DifferentiableFunctionInst *, unsigned> resultIndices;

  /// Mapping from original `apply` instructions to their corresponding
  /// `NestedApplyInfo`s.
  llvm::DenseMap<ApplyInst *, NestedApplyInfo> nestedApplyInfo;

  /// List of generated functions (JVPs, VJPs, pullbacks, and thunks).
  /// Saved for deletion during cleanup.
  llvm::SmallVector<SILFunction *, 32> generatedFunctions;

  /// List of references to generated functions.
  /// Saved for deletion during cleanup.
  llvm::SmallVector<SILValue, 32> generatedFunctionReferences;

  /// The AdditiveArithmetic protocol in the standard library.
  ProtocolDecl *additiveArithmeticProtocol =
      astCtx.getProtocol(KnownProtocolKind::AdditiveArithmetic);

  /// `AdditiveArithmetic.+` declaration.
  mutable FuncDecl *cachedPlusFn = nullptr;
  /// `AdditiveArithmetic.+=` declaration.
  mutable FuncDecl *cachedPlusEqualFn = nullptr;

public:
  /// Construct an ADContext for the given module.
  explicit ADContext(SILModuleTransform &transform);

  //--------------------------------------------------------------------------//
  // General utilities
  //--------------------------------------------------------------------------//

  SILModuleTransform &getTransform() const { return transform; }
  SILModule &getModule() const { return module; }
  ASTContext &getASTContext() const { return module.getASTContext(); }
  SILPassManager &getPassManager() const { return passManager; }
  Lowering::TypeConverter &getTypeConverter() { return module.Types; }

  /// Get or create a synthesized file for adding generated linear map structs
  /// and branching trace enums. Used by `LinearMapInfo`.
  SynthesizedFileUnit &getOrCreateSynthesizedFile();

  /// Returns true if the `differentiable_function` instruction worklist is
  /// empty.
  bool isDifferentiableFunctionInstsWorklistEmpty() const {
    return differentiableFunctionInsts.empty();
  }

  /// Pops and returns a `differentiable_function` instruction from the
  /// worklist. Returns nullptr if the worklist is empty.
  DifferentiableFunctionInst *popDifferentiableFunctionInstFromWorklist() {
    if (differentiableFunctionInsts.empty())
      return nullptr;
    return differentiableFunctionInsts.pop_back_val();
  }

  /// Adds the given `differentiable_function` instruction to the worklist.
  void
  addDifferentiableFunctionInstToWorklist(DifferentiableFunctionInst *dfi) {
    differentiableFunctionInsts.push_back(dfi);
  }

  /// Returns true if the given `differentiable_function` instruction has
  /// already been processed.
  bool
  isDifferentiableFunctionInstProcessed(DifferentiableFunctionInst *dfi) const {
    return processedDifferentiableFunctionInsts.count(dfi);
  }

  /// Adds the given `differentiable_function` instruction to the worklist.
  void
  markDifferentiableFunctionInstAsProcessed(DifferentiableFunctionInst *dfi) {
    processedDifferentiableFunctionInsts.insert(dfi);
  }

  const llvm::SmallMapVector<SILDifferentiabilityWitness *,
                             DifferentiationInvoker, 32> &
  getInvokers() const {
    return invokers;
  }

  void addInvoker(SILDifferentiabilityWitness *witness) {
    assert(!invokers.count(witness) &&
           "Differentiability witness already has an invoker");
    invokers.insert({witness, DifferentiationInvoker(witness)});
  }

  /// Returns the result index for `dfi` if found in this context. Otherwise,
  /// sets the result index to zero and returns it.
  unsigned getResultIndex(DifferentiableFunctionInst *dfi) {
    return resultIndices[dfi];
  }

  /// Sets the result index for `dfi`.
  void setResultIndex(DifferentiableFunctionInst *dfi, unsigned index) {
    resultIndices[dfi] = index;
  }

  llvm::DenseMap<ApplyInst *, NestedApplyInfo> &getNestedApplyInfo() {
    return nestedApplyInfo;
  }

  void recordGeneratedFunction(SILFunction *function) {
    generatedFunctions.push_back(function);
  }

  void recordGeneratedFunctionReference(SILValue functionRef) {
    generatedFunctionReferences.push_back(functionRef);
  }

  ProtocolDecl *getAdditiveArithmeticProtocol() const {
    return additiveArithmeticProtocol;
  }

  FuncDecl *getPlusDecl() const;
  FuncDecl *getPlusEqualDecl() const;

  /// Cleans up all the internal state.
  void cleanUp();

  /// Creates an `differentiable_function` instruction using the given builder
  /// and arguments. Erase the newly created instruction from the processed set,
  /// if it exists - it may exist in the processed set if it has the same
  /// pointer value as a previously processed and deleted instruction.
  /// TODO(TF-784): The pointer reuse is a real concern and the use of
  /// `CanonicalizeInstruction` may get rid of the need for this workaround.
  DifferentiableFunctionInst *createDifferentiableFunction(
      SILBuilder &builder, SILLocation loc, IndexSubset *parameterIndices,
      SILValue original,
      Optional<std::pair<SILValue, SILValue>> derivativeFunctions = None);

  // Given an `differentiable_function` instruction, finds the corresponding
  // differential operator used in the AST. If no differential operator is
  // found, return nullptr.
  DifferentiableFunctionExpr *
  findDifferentialOperator(DifferentiableFunctionInst *inst);

  template <typename... T, typename... U>
  InFlightDiagnostic diagnose(SourceLoc loc, Diag<T...> diag,
                              U &&... args) const {
    return getASTContext().Diags.diagnose(loc, diag, std::forward<U>(args)...);
  }

  /// Given an instruction and a differentiation task associated with the
  /// parent function, emits a "not differentiable" error based on the task. If
  /// the task is indirect, emits notes all the way up to the outermost task,
  /// and emits an error at the outer task. Otherwise, emits an error directly.
  template <typename... T, typename... U>
  InFlightDiagnostic
  emitNondifferentiabilityError(SILInstruction *inst,
                                DifferentiationInvoker invoker, Diag<T...> diag,
                                U &&... args);

  /// Given a value and a differentiation task associated with the parent
  /// function, emits a "not differentiable" error based on the task. If the
  /// task is indirect, emits notes all the way up to the outermost task, and
  /// emits an error at the outer task. Otherwise, emits an error directly.
  template <typename... T, typename... U>
  InFlightDiagnostic
  emitNondifferentiabilityError(SILValue value, DifferentiationInvoker invoker,
                                Diag<T...> diag, U &&... args);

  /// Emit a "not differentiable" error based on the given differentiation task
  /// and diagnostic.
  template <typename... T, typename... U>
  InFlightDiagnostic
  emitNondifferentiabilityError(SourceLoc loc, DifferentiationInvoker invoker,
                                Diag<T...> diag, U &&... args);
};

template <typename... T, typename... U>
InFlightDiagnostic
ADContext::emitNondifferentiabilityError(SILValue value,
                                         DifferentiationInvoker invoker,
                                         Diag<T...> diag, U &&... args) {
  LLVM_DEBUG({
    getADDebugStream() << "Diagnosing non-differentiability.\n";
    getADDebugStream() << "For value:\n" << value;
    getADDebugStream() << "With invoker:\n" << invoker << '\n';
  });
  auto valueLoc = value.getLoc().getSourceLoc();
  // If instruction does not have a valid location, use the function location
  // as a fallback. Improves diagnostics in some cases.
  if (valueLoc.isInvalid())
    valueLoc = value->getFunction()->getLocation().getSourceLoc();
  return emitNondifferentiabilityError(valueLoc, invoker, diag,
                                       std::forward<U>(args)...);
}

template <typename... T, typename... U>
InFlightDiagnostic
ADContext::emitNondifferentiabilityError(SILInstruction *inst,
                                         DifferentiationInvoker invoker,
                                         Diag<T...> diag, U &&... args) {
  LLVM_DEBUG({
    getADDebugStream() << "Diagnosing non-differentiability.\n";
    getADDebugStream() << "For instruction:\n" << *inst;
    getADDebugStream() << "With invoker:\n" << invoker << '\n';
  });
  auto instLoc = inst->getLoc().getSourceLoc();
  // If instruction does not have a valid location, use the function location
  // as a fallback. Improves diagnostics for `ref_element_addr` generated in
  // synthesized stored property getters.
  if (instLoc.isInvalid())
    instLoc = inst->getFunction()->getLocation().getSourceLoc();
  return emitNondifferentiabilityError(instLoc, invoker, diag,
                                       std::forward<U>(args)...);
}

template <typename... T, typename... U>
InFlightDiagnostic
ADContext::emitNondifferentiabilityError(SourceLoc loc,
                                         DifferentiationInvoker invoker,
                                         Diag<T...> diag, U &&... args) {
  switch (invoker.getKind()) {
  // For `differentiable_function` instructions: if the
  // `differentiable_function` instruction comes from a differential operator,
  // emit an error on the expression and a note on the non-differentiable
  // operation. Otherwise, emit both an error and note on the
  // non-differentiation operation.
  case DifferentiationInvoker::Kind::DifferentiableFunctionInst: {
    auto *inst = invoker.getDifferentiableFunctionInst();
    if (auto *expr = findDifferentialOperator(inst)) {
      diagnose(expr->getLoc(), diag::autodiff_function_not_differentiable_error)
          .highlight(expr->getSubExpr()->getSourceRange());
      return diagnose(loc, diag, std::forward<U>(args)...);
    }
    diagnose(loc, diag::autodiff_expression_not_differentiable_error);
    return diagnose(loc, diag, std::forward<U>(args)...);
  }

  // For differentiability witnesses: try to find a `@differentiable` or
  // `@derivative` attribute. If an attribute is found, emit an error on it;
  // otherwise, emit an error on the original function.
  case DifferentiationInvoker::Kind::SILDifferentiabilityWitnessInvoker: {
    auto *witness = invoker.getSILDifferentiabilityWitnessInvoker();
    auto *original = witness->getOriginalFunction();
    // If the witness has an associated attribute, emit an error at its
    // location.
    if (auto *attr = witness->getAttribute()) {
      diagnose(attr->getLocation(),
               diag::autodiff_function_not_differentiable_error)
          .highlight(attr->getRangeWithAt());
      // Emit informative note.
      bool emittedNote = false;
      // If the witness comes from an implicit `@differentiable` attribute
      // inherited from a protocol requirement's `@differentiable` attribute,
      // emit a note on the inherited attribute.
      if (auto *diffAttr = dyn_cast<DifferentiableAttr>(attr)) {
        auto inheritedAttrLoc =
            diffAttr->getImplicitlyInheritedDifferentiableAttrLocation();
        if (inheritedAttrLoc.isValid()) {
          diagnose(inheritedAttrLoc,
                   diag::autodiff_implicitly_inherited_differentiable_attr_here)
              .highlight(inheritedAttrLoc);
          emittedNote = true;
        }
      }
      // Otherwise, emit a note on the original function.
      if (!emittedNote) {
        diagnose(original->getLocation().getSourceLoc(),
                 diag::autodiff_when_differentiating_function_definition);
      }
    }
    // Otherwise, emit an error on the original function.
    else {
      diagnose(original->getLocation().getSourceLoc(),
               diag::autodiff_function_not_differentiable_error);
    }
    return diagnose(loc, diag, std::forward<U>(args)...);
  }

  // For indirect differentiation, emit a "not differentiable" note on the
  // expression first. Then emit an error at the source invoker of
  // differentiation, and a "when differentiating this" note at each indirect
  // invoker.
  case DifferentiationInvoker::Kind::IndirectDifferentiation: {
    SILInstruction *inst;
    SILDifferentiabilityWitness *witness;
    std::tie(inst, witness) = invoker.getIndirectDifferentiation();
    auto invokerLookup = invokers.find(witness);
    assert(invokerLookup != invokers.end() && "Expected parent invoker");
    emitNondifferentiabilityError(
        inst, invokerLookup->second,
        diag::autodiff_expression_not_differentiable_note);
    return diagnose(loc, diag::autodiff_when_differentiating_function_call);
  }
  }
}

} // end namespace autodiff
} // end namespace swift

#endif // SWIFT_SILOPTIMIZER_UTILS_DIFFERENTIATION_ADCONTEXT_H
