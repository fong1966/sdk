// Copyright (c) 2013, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#ifndef RUNTIME_VM_COMPILER_BACKEND_IL_H_
#define RUNTIME_VM_COMPILER_BACKEND_IL_H_

#if defined(DART_PRECOMPILED_RUNTIME)
#error "AOT runtime should not use compiler sources (including header files)"
#endif  // defined(DART_PRECOMPILED_RUNTIME)

#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>

#include "vm/allocation.h"
#include "vm/code_descriptors.h"
#include "vm/compiler/backend/compile_type.h"
#include "vm/compiler/backend/il_serializer.h"
#include "vm/compiler/backend/locations.h"
#include "vm/compiler/backend/slot.h"
#include "vm/compiler/compiler_pass.h"
#include "vm/compiler/compiler_state.h"
#include "vm/compiler/ffi/marshaller.h"
#include "vm/compiler/ffi/native_calling_convention.h"
#include "vm/compiler/ffi/native_location.h"
#include "vm/compiler/ffi/native_type.h"
#include "vm/compiler/method_recognizer.h"
#include "vm/dart_entry.h"
#include "vm/flags.h"
#include "vm/growable_array.h"
#include "vm/native_entry.h"
#include "vm/object.h"
#include "vm/parser.h"
#include "vm/runtime_entry.h"
#include "vm/static_type_exactness_state.h"
#include "vm/token_position.h"

namespace dart {

class BaseTextBuffer;
class BinaryFeedback;
class BitVector;
class BlockEntryInstr;
class BlockEntryWithInitialDefs;
class BoxIntegerInstr;
class CallTargets;
class CatchBlockEntryInstr;
class CheckBoundBaseInstr;
class ComparisonInstr;
class Definition;
class Environment;
class FlowGraph;
class FlowGraphCompiler;
class FlowGraphVisitor;
class ForwardInstructionIterator;
class Instruction;
class InstructionVisitor;
class LocalVariable;
class LoopInfo;
class MoveSchedule;
class ParsedFunction;
class Range;
class RangeAnalysis;
class RangeBoundary;
class TypeUsageInfo;
class UnboxIntegerInstr;

namespace compiler {
class BlockBuilder;
struct TableSelector;
}  // namespace compiler

class Value : public ZoneAllocated {
 public:
  // A forward iterator that allows removing the current value from the
  // underlying use list during iteration.
  class Iterator {
   public:
    explicit Iterator(Value* head) : next_(head) { Advance(); }
    Value* Current() const { return current_; }
    bool Done() const { return current_ == nullptr; }
    void Advance() {
      // Pre-fetch next on advance and cache it.
      current_ = next_;
      if (next_ != nullptr) next_ = next_->next_use();
    }

   private:
    Value* current_;
    Value* next_;
  };

  explicit Value(Definition* definition)
      : definition_(definition),
        previous_use_(nullptr),
        next_use_(nullptr),
        instruction_(nullptr),
        use_index_(-1),
        reaching_type_(nullptr) {}

  Definition* definition() const { return definition_; }
  void set_definition(Definition* definition) {
    definition_ = definition;
    // Clone the reaching type if there was one and the owner no longer matches
    // this value's definition.
    SetReachingType(reaching_type_);
  }

  Value* previous_use() const { return previous_use_; }
  void set_previous_use(Value* previous) { previous_use_ = previous; }

  Value* next_use() const { return next_use_; }
  void set_next_use(Value* next) { next_use_ = next; }

  bool IsSingleUse() const {
    return (next_use_ == nullptr) && (previous_use_ == nullptr);
  }

  Instruction* instruction() const { return instruction_; }
  void set_instruction(Instruction* instruction) { instruction_ = instruction; }

  intptr_t use_index() const { return use_index_; }
  void set_use_index(intptr_t index) { use_index_ = index; }

  static void AddToList(Value* value, Value** list);
  void RemoveFromUseList();

  // Change the definition after use lists have been computed.
  inline void BindTo(Definition* definition);
  inline void BindToEnvironment(Definition* definition);

  Value* Copy(Zone* zone) { return new (zone) Value(definition_); }

  // CopyWithType() must only be used when the new Value is dominated by
  // the original Value.
  Value* CopyWithType(Zone* zone) {
    Value* copy = new (zone) Value(definition_);
    copy->reaching_type_ = reaching_type_;
    return copy;
  }
  Value* CopyWithType() { return CopyWithType(Thread::Current()->zone()); }

  CompileType* Type();

  CompileType* reaching_type() const { return reaching_type_; }
  void SetReachingType(CompileType* type);
  void RefineReachingType(CompileType* type);

#if defined(INCLUDE_IL_PRINTER)
  void PrintTo(BaseTextBuffer* f) const;
#endif  // defined(INCLUDE_IL_PRINTER)

  const char* ToCString() const;

  bool IsSmiValue() { return Type()->ToCid() == kSmiCid; }

  // Return true if the value represents a constant.
  bool BindsToConstant() const;
  bool BindsToConstant(ConstantInstr** constant_defn) const;

  // Return true if the value represents the constant null.
  bool BindsToConstantNull() const;

  // Assert if BindsToConstant() is false, otherwise returns the constant value.
  const Object& BoundConstant() const;

  // Return true if the value represents Smi constant.
  bool BindsToSmiConstant() const;

  // Return value of represented Smi constant.
  intptr_t BoundSmiConstant() const;

  // Return true if storing the value into a heap object requires applying the
  // write barrier. Can change the reaching type of the Value or other Values
  // in the same chain of redefinitions.
  bool NeedsWriteBarrier();

  bool Equals(const Value& other) const;

  // Returns true if this |Value| can evaluate to the given |value| during
  // execution.
  inline bool CanBe(const Object& value);

 private:
  friend class FlowGraphPrinter;

  Definition* definition_;
  Value* previous_use_;
  Value* next_use_;
  Instruction* instruction_;
  intptr_t use_index_;

  CompileType* reaching_type_;

  DISALLOW_COPY_AND_ASSIGN(Value);
};

// Represents a range of class-ids for use in class checks and polymorphic
// dispatches.  The range includes both ends, i.e. it is [cid_start, cid_end].
struct CidRange : public ZoneAllocated {
  CidRange(intptr_t cid_start_arg, intptr_t cid_end_arg)
      : cid_start(cid_start_arg), cid_end(cid_end_arg) {}
  CidRange() : cid_start(kIllegalCid), cid_end(kIllegalCid) {}

  bool IsSingleCid() const { return cid_start == cid_end; }
  bool Contains(intptr_t cid) const {
    return cid_start <= cid && cid <= cid_end;
  }
  int32_t Extent() const { return cid_end - cid_start; }

  // The number of class ids this range covers.
  intptr_t size() const { return cid_end - cid_start + 1; }

  bool IsIllegalRange() const {
    return cid_start == kIllegalCid && cid_end == kIllegalCid;
  }

  intptr_t cid_start;
  intptr_t cid_end;

  DISALLOW_COPY_AND_ASSIGN(CidRange);
};

struct CidRangeValue {
  CidRangeValue(intptr_t cid_start_arg, intptr_t cid_end_arg)
      : cid_start(cid_start_arg), cid_end(cid_end_arg) {}
  CidRangeValue(const CidRange& other)  // NOLINT
      : cid_start(other.cid_start), cid_end(other.cid_end) {}

  bool IsSingleCid() const { return cid_start == cid_end; }
  bool Contains(intptr_t cid) const {
    return cid_start <= cid && cid <= cid_end;
  }
  int32_t Extent() const { return cid_end - cid_start; }

  // The number of class ids this range covers.
  intptr_t size() const { return cid_end - cid_start + 1; }

  bool IsIllegalRange() const {
    return cid_start == kIllegalCid && cid_end == kIllegalCid;
  }

  bool Equals(const CidRangeValue& other) const {
    return cid_start == other.cid_start && cid_end == other.cid_end;
  }

  intptr_t cid_start;
  intptr_t cid_end;
};

typedef MallocGrowableArray<CidRangeValue> CidRangeVector;

class CidRangeVectorUtils : public AllStatic {
 public:
  static bool ContainsCid(const CidRangeVector& ranges, intptr_t cid) {
    for (const CidRangeValue& range : ranges) {
      if (range.Contains(cid)) {
        return true;
      }
    }
    return false;
  }
};

class HierarchyInfo : public ThreadStackResource {
 public:
  explicit HierarchyInfo(Thread* thread)
      : ThreadStackResource(thread),
        cid_subtype_ranges_nullable_(),
        cid_subtype_ranges_abstract_nullable_(),
        cid_subtype_ranges_nonnullable_(),
        cid_subtype_ranges_abstract_nonnullable_() {
    thread->set_hierarchy_info(this);
  }

  ~HierarchyInfo() { thread()->set_hierarchy_info(nullptr); }

  // Returned from FindBestTAVOffset and SplitOnConsistentTypeArguments
  // to denote a failure to find a compatible concrete, finalized class.
  static constexpr intptr_t kNoCompatibleTAVOffset = 0;

  const CidRangeVector& SubtypeRangesForClass(const Class& klass,
                                              bool include_abstract,
                                              bool exclude_null);

  bool InstanceOfHasClassRange(const AbstractType& type,
                               intptr_t* lower_limit,
                               intptr_t* upper_limit);

  // Returns `true` if a simple [CidRange]-based subtype-check can be used to
  // determine if a given instance's type is a subtype of [type].
  //
  // This is the case for [type]s without type arguments or where the type
  // arguments are all dynamic (known as "rare type").
  bool CanUseSubtypeRangeCheckFor(const AbstractType& type);

  // Returns `true` if a combination of [CidRange]-based checks can be used to
  // determine if a given instance's type is a subtype of [type].
  //
  // This is the case for [type]s with type arguments where we are able to do a
  // [CidRange]-based subclass-check against the class and [CidRange]-based
  // subtype-checks against the type arguments.
  //
  // This method should only be called if [CanUseSubtypeRangecheckFor] returned
  // false.
  bool CanUseGenericSubtypeRangeCheckFor(const AbstractType& type);

  // Returns `true` if [type] is a record type which fields can be tested using
  // simple [CidRange]-based subtype-check.
  bool CanUseRecordSubtypeRangeCheckFor(const AbstractType& type);

 private:
  // Does not use any hierarchy information available in the system but computes
  // it via O(n) class table traversal.
  //
  // The boolean parameters denote:
  //   include_abstract : if set, include abstract types (don't care otherwise)
  //   exclude_null     : if set, exclude null types (don't care otherwise)
  void BuildRangesUsingClassTableFor(ClassTable* table,
                                     CidRangeVector* ranges,
                                     const Class& klass,
                                     bool include_abstract,
                                     bool exclude_null);

  // Uses hierarchy information stored in the [Class]'s direct_subclasses() and
  // direct_implementors() arrays, unless that information is not available
  // in which case we fall back to the class table.
  //
  // The boolean parameters denote:
  //   include_abstract : if set, include abstract types (don't care otherwise)
  //   exclude_null     : if set, exclude null types (don't care otherwise)
  void BuildRangesFor(ClassTable* table,
                      CidRangeVector* ranges,
                      const Class& klass,
                      bool include_abstract,
                      bool exclude_null);

  std::unique_ptr<CidRangeVector[]> cid_subtype_ranges_nullable_;
  std::unique_ptr<CidRangeVector[]> cid_subtype_ranges_abstract_nullable_;
  std::unique_ptr<CidRangeVector[]> cid_subtype_ranges_nonnullable_;
  std::unique_ptr<CidRangeVector[]> cid_subtype_ranges_abstract_nonnullable_;
};

// An embedded container with N elements of type T.  Used (with partial
// specialization for N=0) because embedded arrays cannot have size 0.
template <typename T, intptr_t N>
class EmbeddedArray {
 public:
  EmbeddedArray() : elements_() {}

  intptr_t length() const { return N; }

  const T& operator[](intptr_t i) const {
    ASSERT(i < length());
    return elements_[i];
  }

  T& operator[](intptr_t i) {
    ASSERT(i < length());
    return elements_[i];
  }

  const T& At(intptr_t i) const { return (*this)[i]; }

  void SetAt(intptr_t i, const T& val) { (*this)[i] = val; }

 private:
  T elements_[N];
};

template <typename T>
class EmbeddedArray<T, 0> {
 public:
  intptr_t length() const { return 0; }
  const T& operator[](intptr_t i) const {
    UNREACHABLE();
    static T sentinel = nullptr;
    return sentinel;
  }
  T& operator[](intptr_t i) {
    UNREACHABLE();
    static T sentinel = nullptr;
    return sentinel;
  }
};

// Instructions.

// M is a two argument macro. It is applied to each concrete instruction type
// name. The concrete instruction classes are the name with Instr concatenated.

struct InstrAttrs {
  enum Attributes {
    _ = 0,  // No special attributes.
            //
    // The instruction is guaranteed to not trigger GC on a non-exceptional
    // path. If the conditions depend on parameters of the instruction, do not
    // use this attribute but overload CanTriggerGC() instead.
    kNoGC = 1,
  };
};

#define FOR_EACH_INSTRUCTION(M)                                                \
  M(GraphEntry, kNoGC)                                                         \
  M(JoinEntry, kNoGC)                                                          \
  M(TargetEntry, kNoGC)                                                        \
  M(FunctionEntry, kNoGC)                                                      \
  M(NativeEntry, kNoGC)                                                        \
  M(OsrEntry, kNoGC)                                                           \
  M(IndirectEntry, kNoGC)                                                      \
  M(CatchBlockEntry, kNoGC)                                                    \
  M(Phi, kNoGC)                                                                \
  M(Redefinition, kNoGC)                                                       \
  M(ReachabilityFence, kNoGC)                                                  \
  M(Parameter, kNoGC)                                                          \
  M(NativeParameter, kNoGC)                                                    \
  M(LoadIndexedUnsafe, kNoGC)                                                  \
  M(StoreIndexedUnsafe, kNoGC)                                                 \
  M(MemoryCopy, kNoGC)                                                         \
  M(TailCall, kNoGC)                                                           \
  M(ParallelMove, kNoGC)                                                       \
  M(MoveArgument, kNoGC)                                                       \
  M(Return, kNoGC)                                                             \
  M(NativeReturn, kNoGC)                                                       \
  M(Throw, kNoGC)                                                              \
  M(ReThrow, kNoGC)                                                            \
  M(Stop, kNoGC)                                                               \
  M(Goto, kNoGC)                                                               \
  M(IndirectGoto, kNoGC)                                                       \
  M(Branch, kNoGC)                                                             \
  M(AssertAssignable, _)                                                       \
  M(AssertSubtype, _)                                                          \
  M(AssertBoolean, _)                                                          \
  M(SpecialParameter, kNoGC)                                                   \
  M(ClosureCall, _)                                                            \
  M(FfiCall, _)                                                                \
  M(CCall, kNoGC)                                                              \
  M(RawStoreField, kNoGC)                                                      \
  M(InstanceCall, _)                                                           \
  M(PolymorphicInstanceCall, _)                                                \
  M(DispatchTableCall, _)                                                      \
  M(StaticCall, _)                                                             \
  M(LoadLocal, kNoGC)                                                          \
  M(DropTemps, kNoGC)                                                          \
  M(MakeTemp, kNoGC)                                                           \
  M(StoreLocal, kNoGC)                                                         \
  M(StrictCompare, kNoGC)                                                      \
  M(EqualityCompare, kNoGC)                                                    \
  M(RelationalOp, kNoGC)                                                       \
  M(NativeCall, _)                                                             \
  M(DebugStepCheck, _)                                                         \
  M(RecordCoverage, kNoGC)                                                     \
  M(LoadIndexed, kNoGC)                                                        \
  M(LoadCodeUnits, _)                                                          \
  M(StoreIndexed, kNoGC)                                                       \
  M(StoreField, _)                                                             \
  M(LoadStaticField, _)                                                        \
  M(StoreStaticField, kNoGC)                                                   \
  M(BooleanNegate, kNoGC)                                                      \
  M(InstanceOf, _)                                                             \
  M(CreateArray, _)                                                            \
  M(AllocateObject, _)                                                         \
  M(AllocateClosure, _)                                                        \
  M(AllocateRecord, _)                                                         \
  M(AllocateSmallRecord, _)                                                    \
  M(AllocateTypedData, _)                                                      \
  M(LoadField, _)                                                              \
  M(LoadUntagged, kNoGC)                                                       \
  M(LoadClassId, kNoGC)                                                        \
  M(InstantiateType, _)                                                        \
  M(InstantiateTypeArguments, _)                                               \
  M(AllocateContext, _)                                                        \
  M(AllocateUninitializedContext, _)                                           \
  M(CloneContext, _)                                                           \
  M(BinarySmiOp, kNoGC)                                                        \
  M(BinaryInt32Op, kNoGC)                                                      \
  M(HashDoubleOp, kNoGC)                                                       \
  M(HashIntegerOp, kNoGC)                                                      \
  M(UnarySmiOp, kNoGC)                                                         \
  M(UnaryDoubleOp, kNoGC)                                                      \
  M(CheckStackOverflow, _)                                                     \
  M(SmiToDouble, kNoGC)                                                        \
  M(Int32ToDouble, kNoGC)                                                      \
  M(Int64ToDouble, kNoGC)                                                      \
  M(DoubleToInteger, _)                                                        \
  M(DoubleToSmi, kNoGC)                                                        \
  M(DoubleToDouble, kNoGC)                                                     \
  M(DoubleToFloat, kNoGC)                                                      \
  M(FloatToDouble, kNoGC)                                                      \
  M(CheckClass, kNoGC)                                                         \
  M(CheckClassId, kNoGC)                                                       \
  M(CheckSmi, kNoGC)                                                           \
  M(CheckNull, kNoGC)                                                          \
  M(CheckCondition, kNoGC)                                                     \
  M(Constant, kNoGC)                                                           \
  M(UnboxedConstant, kNoGC)                                                    \
  M(CheckEitherNonSmi, kNoGC)                                                  \
  M(BinaryDoubleOp, kNoGC)                                                     \
  M(DoubleTestOp, kNoGC)                                                       \
  M(MathUnary, kNoGC)                                                          \
  M(MathMinMax, kNoGC)                                                         \
  M(Box, _)                                                                    \
  M(Unbox, kNoGC)                                                              \
  M(BoxInt64, _)                                                               \
  M(UnboxInt64, kNoGC)                                                         \
  M(CaseInsensitiveCompare, kNoGC)                                             \
  M(BinaryInt64Op, kNoGC)                                                      \
  M(ShiftInt64Op, kNoGC)                                                       \
  M(SpeculativeShiftInt64Op, kNoGC)                                            \
  M(UnaryInt64Op, kNoGC)                                                       \
  M(CheckArrayBound, kNoGC)                                                    \
  M(GenericCheckBound, kNoGC)                                                  \
  M(CheckWritable, kNoGC)                                                      \
  M(Constraint, kNoGC)                                                         \
  M(StringToCharCode, kNoGC)                                                   \
  M(OneByteStringFromCharCode, kNoGC)                                          \
  M(Utf8Scan, kNoGC)                                                           \
  M(InvokeMathCFunction, kNoGC)                                                \
  M(TruncDivMod, kNoGC)                                                        \
  /*We could be more precise about when these 2 instructions can trigger GC.*/ \
  M(GuardFieldClass, _)                                                        \
  M(GuardFieldLength, _)                                                       \
  M(GuardFieldType, _)                                                         \
  M(IfThenElse, kNoGC)                                                         \
  M(MaterializeObject, _)                                                      \
  M(TestSmi, kNoGC)                                                            \
  M(TestCids, kNoGC)                                                           \
  M(TestRange, kNoGC)                                                          \
  M(ExtractNthOutput, kNoGC)                                                   \
  M(MakePair, kNoGC)                                                           \
  M(BinaryUint32Op, kNoGC)                                                     \
  M(ShiftUint32Op, kNoGC)                                                      \
  M(SpeculativeShiftUint32Op, kNoGC)                                           \
  M(UnaryUint32Op, kNoGC)                                                      \
  M(BoxUint32, _)                                                              \
  M(UnboxUint32, kNoGC)                                                        \
  M(BoxInt32, _)                                                               \
  M(UnboxInt32, kNoGC)                                                         \
  M(BoxSmallInt, kNoGC)                                                        \
  M(IntConverter, kNoGC)                                                       \
  M(BitCast, kNoGC)                                                            \
  M(Call1ArgStub, _)                                                           \
  M(LoadThread, kNoGC)                                                         \
  M(Deoptimize, kNoGC)                                                         \
  M(SimdOp, kNoGC)                                                             \
  M(Suspend, _)

#define FOR_EACH_ABSTRACT_INSTRUCTION(M)                                       \
  M(Allocation, _)                                                             \
  M(ArrayAllocation, _)                                                        \
  M(BinaryIntegerOp, _)                                                        \
  M(BlockEntry, _)                                                             \
  M(BoxInteger, _)                                                             \
  M(CheckBoundBase, _)                                                         \
  M(Comparison, _)                                                             \
  M(InstanceCallBase, _)                                                       \
  M(ShiftIntegerOp, _)                                                         \
  M(UnaryIntegerOp, _)                                                         \
  M(UnboxInteger, _)

#define FORWARD_DECLARATION(type, attrs) class type##Instr;
FOR_EACH_INSTRUCTION(FORWARD_DECLARATION)
FOR_EACH_ABSTRACT_INSTRUCTION(FORWARD_DECLARATION)
#undef FORWARD_DECLARATION

#define DEFINE_INSTRUCTION_TYPE_CHECK(type)                                    \
  virtual type##Instr* As##type() { return this; }                             \
  virtual const type##Instr* As##type() const { return this; }                 \
  virtual const char* DebugName() const { return #type; }

// Functions required in all concrete instruction classes.
#define DECLARE_INSTRUCTION_NO_BACKEND(type)                                   \
  virtual Tag tag() const { return k##type; }                                  \
  virtual void Accept(InstructionVisitor* visitor);                            \
  DEFINE_INSTRUCTION_TYPE_CHECK(type)

#define DECLARE_INSTRUCTION_BACKEND()                                          \
  virtual LocationSummary* MakeLocationSummary(Zone* zone, bool optimizing)    \
      const;                                                                   \
  virtual void EmitNativeCode(FlowGraphCompiler* compiler);

// Functions required in all concrete instruction classes.
#define DECLARE_INSTRUCTION(type)                                              \
  DECLARE_INSTRUCTION_NO_BACKEND(type)                                         \
  DECLARE_INSTRUCTION_BACKEND()

// Functions required in all abstract instruction classes.
#define DECLARE_ABSTRACT_INSTRUCTION(type)                                     \
  /* Prevents allocating an instance of abstract instruction */                \
  /* even if it has a concrete base class. */                                  \
  virtual Tag tag() const = 0;                                                 \
  DEFINE_INSTRUCTION_TYPE_CHECK(type)

#define DECLARE_COMPARISON_METHODS                                             \
  virtual LocationSummary* MakeLocationSummary(Zone* zone, bool optimizing)    \
      const;                                                                   \
  virtual Condition EmitComparisonCode(FlowGraphCompiler* compiler,            \
                                       BranchLabels labels);

#define DECLARE_COMPARISON_INSTRUCTION(type)                                   \
  DECLARE_INSTRUCTION_NO_BACKEND(type)                                         \
  DECLARE_COMPARISON_METHODS

template <typename T, bool is_enum>
struct unwrap_enum {};

template <typename T>
struct unwrap_enum<T, true> {
  using type = std::underlying_type_t<T>;
};

template <typename T>
struct unwrap_enum<T, false> {
  using type = T;
};

template <typename T>
using serializable_type_t =
    typename unwrap_enum<std::remove_cv_t<T>, std::is_enum<T>::value>::type;

#define WRITE_INSTRUCTION_FIELD(type, name)                                    \
  s->Write<serializable_type_t<type>>(                                         \
      static_cast<serializable_type_t<type>>(name));
#define READ_INSTRUCTION_FIELD(type, name)                                     \
  , name(static_cast<std::remove_cv_t<type>>(                                  \
        d->Read<serializable_type_t<type>>()))
#define DECLARE_INSTRUCTION_FIELD(type, name) type name;

// Every instruction class should declare its serialization via
// DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS, DECLARE_EMPTY_SERIALIZATION
// or DECLARE_CUSTOM_SERIALIZATION.
// If instruction class has fields which reference other instructions,
// then it should also use DECLARE_EXTRA_SERIALIZATION and serialize
// those references in WriteExtra/ReadExtra methods.
#define DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(Instr, BaseClass, FieldList)   \
 public:                                                                       \
  virtual void WriteTo(FlowGraphSerializer* s) {                               \
    BaseClass::WriteTo(s);                                                     \
    FieldList(WRITE_INSTRUCTION_FIELD)                                         \
  }                                                                            \
  explicit Instr(FlowGraphDeserializer* d)                                     \
      : BaseClass(d) FieldList(READ_INSTRUCTION_FIELD) {}                      \
                                                                               \
 private:                                                                      \
  FieldList(DECLARE_INSTRUCTION_FIELD)

#define DECLARE_CUSTOM_SERIALIZATION(Instr)                                    \
 public:                                                                       \
  virtual void WriteTo(FlowGraphSerializer* s);                                \
  explicit Instr(FlowGraphDeserializer* d);

#define DECLARE_EMPTY_SERIALIZATION(Instr, BaseClass)                          \
 public:                                                                       \
  explicit Instr(FlowGraphDeserializer* d) : BaseClass(d) {}

#define DECLARE_EXTRA_SERIALIZATION                                            \
 public:                                                                       \
  virtual void WriteExtra(FlowGraphSerializer* s);                             \
  virtual void ReadExtra(FlowGraphDeserializer* d);

#if defined(INCLUDE_IL_PRINTER)
#define PRINT_TO_SUPPORT virtual void PrintTo(BaseTextBuffer* f) const;
#define PRINT_OPERANDS_TO_SUPPORT                                              \
  virtual void PrintOperandsTo(BaseTextBuffer* f) const;
#define DECLARE_ATTRIBUTES(...)                                                \
  auto GetAttributes() const { return std::make_tuple(__VA_ARGS__); }          \
  static auto GetAttributeNames() { return std::make_tuple(#__VA_ARGS__); }
#define DECLARE_ATTRIBUTES_NAMED(names, values)                                \
  auto GetAttributes() const {                                                 \
    return std::make_tuple values;                                             \
  }                                                                            \
  static auto GetAttributeNames() {                                            \
    return std::make_tuple names;                                              \
  }
#else
#define PRINT_TO_SUPPORT
#define PRINT_OPERANDS_TO_SUPPORT
#define DECLARE_ATTRIBUTES(...)
#define DECLARE_ATTRIBUTES_NAMED(names, values)
#endif  // defined(INCLUDE_IL_PRINTER)

// Together with CidRange, this represents a mapping from a range of class-ids
// to a method for a given selector (method name).  Also can contain an
// indication of how frequently a given method has been called at a call site.
// This information can be harvested from the inline caches (ICs).
struct TargetInfo : public CidRange {
  TargetInfo(intptr_t cid_start_arg,
             intptr_t cid_end_arg,
             const Function* target_arg,
             intptr_t count_arg,
             StaticTypeExactnessState exactness)
      : CidRange(cid_start_arg, cid_end_arg),
        target(target_arg),
        count(count_arg),
        exactness(exactness) {
    DEBUG_ASSERT(target->IsNotTemporaryScopedHandle());
  }
  const Function* target;
  intptr_t count;
  StaticTypeExactnessState exactness;

  DISALLOW_COPY_AND_ASSIGN(TargetInfo);
};

// A set of class-ids, arranged in ranges. Used for the CheckClass
// and PolymorphicInstanceCall instructions.
class Cids : public ZoneAllocated {
 public:
  explicit Cids(Zone* zone) : cid_ranges_(zone, 6) {}
  // Creates the off-heap Cids object that reflects the contents
  // of the on-VM-heap IC data.
  // Ranges of Cids are merged if there is only one target function and
  // it is used for all cids in the gaps between ranges.
  static Cids* CreateForArgument(Zone* zone,
                                 const BinaryFeedback& binary_feedback,
                                 int argument_number);
  static Cids* CreateMonomorphic(Zone* zone, intptr_t cid);

  bool Equals(const Cids& other) const;

  bool HasClassId(intptr_t cid) const;

  void Add(CidRange* target) { cid_ranges_.Add(target); }

  CidRange& operator[](intptr_t index) const { return *cid_ranges_[index]; }

  CidRange* At(int index) const { return cid_ranges_[index]; }

  intptr_t length() const { return cid_ranges_.length(); }

  void SetLength(intptr_t len) { cid_ranges_.SetLength(len); }

  bool is_empty() const { return cid_ranges_.is_empty(); }

  void Sort(int compare(CidRange* const* a, CidRange* const* b)) {
    cid_ranges_.Sort(compare);
  }

  bool IsMonomorphic() const;
  intptr_t MonomorphicReceiverCid() const;
  intptr_t ComputeLowestCid() const;
  intptr_t ComputeHighestCid() const;

 protected:
  GrowableArray<CidRange*> cid_ranges_;

 private:
  DISALLOW_IMPLICIT_CONSTRUCTORS(Cids);
};

class CallTargets : public Cids {
 public:
  explicit CallTargets(Zone* zone) : Cids(zone) {}

  static const CallTargets* CreateMonomorphic(Zone* zone,
                                              intptr_t receiver_cid,
                                              const Function& target);

  // Creates the off-heap CallTargets object that reflects the contents
  // of the on-VM-heap IC data.
  static const CallTargets* Create(Zone* zone, const ICData& ic_data);

  // This variant also expands the class-ids to neighbouring classes that
  // inherit the same method.
  static const CallTargets* CreateAndExpand(Zone* zone, const ICData& ic_data);

  TargetInfo* TargetAt(int i) const { return static_cast<TargetInfo*>(At(i)); }

  intptr_t AggregateCallCount() const;

  StaticTypeExactnessState MonomorphicExactness() const;
  bool HasSingleTarget() const;
  bool HasSingleRecognizedTarget() const;
  const Function& FirstTarget() const;
  const Function& MostPopularTarget() const;

  void Print() const;

  bool ReceiverIs(intptr_t cid) const {
    return IsMonomorphic() && MonomorphicReceiverCid() == cid;
  }
  bool ReceiverIsSmiOrMint() const {
    if (cid_ranges_.is_empty()) {
      return false;
    }
    for (intptr_t i = 0, n = cid_ranges_.length(); i < n; i++) {
      for (intptr_t j = cid_ranges_[i]->cid_start; j <= cid_ranges_[i]->cid_end;
           j++) {
        if (j != kSmiCid && j != kMintCid) {
          return false;
        }
      }
    }
    return true;
  }

  void Write(FlowGraphSerializer* s) const;
  explicit CallTargets(FlowGraphDeserializer* d);

 private:
  void CreateHelper(Zone* zone, const ICData& ic_data);
  void MergeIntoRanges();
};

// Represents type feedback for the binary operators, and a few recognized
// static functions (see MethodRecognizer::NumArgsCheckedForStaticCall).
class BinaryFeedback : public ZoneAllocated {
 public:
  explicit BinaryFeedback(Zone* zone) : feedback_(zone, 2) {}

  static const BinaryFeedback* Create(Zone* zone, const ICData& ic_data);
  static const BinaryFeedback* CreateMonomorphic(Zone* zone,
                                                 intptr_t receiver_cid,
                                                 intptr_t argument_cid);

  bool ArgumentIs(intptr_t cid) const {
    if (feedback_.is_empty()) {
      return false;
    }
    for (intptr_t i = 0, n = feedback_.length(); i < n; i++) {
      if (feedback_[i].second != cid) {
        return false;
      }
    }
    return true;
  }

  bool OperandsAreEither(intptr_t cid_a, intptr_t cid_b) const {
    if (feedback_.is_empty()) {
      return false;
    }
    for (intptr_t i = 0, n = feedback_.length(); i < n; i++) {
      if ((feedback_[i].first != cid_a) && (feedback_[i].first != cid_b)) {
        return false;
      }
      if ((feedback_[i].second != cid_a) && (feedback_[i].second != cid_b)) {
        return false;
      }
    }
    return true;
  }
  bool OperandsAreSmiOrNull() const {
    return OperandsAreEither(kSmiCid, kNullCid);
  }
  bool OperandsAreSmiOrMint() const {
    return OperandsAreEither(kSmiCid, kMintCid);
  }
  bool OperandsAreSmiOrDouble() const {
    return OperandsAreEither(kSmiCid, kDoubleCid);
  }

  bool OperandsAre(intptr_t cid) const {
    if (feedback_.length() != 1) return false;
    return (feedback_[0].first == cid) && (feedback_[0].second == cid);
  }

  bool IncludesOperands(intptr_t cid) const {
    for (intptr_t i = 0, n = feedback_.length(); i < n; i++) {
      if ((feedback_[i].first == cid) && (feedback_[i].second == cid)) {
        return true;
      }
    }
    return false;
  }

 private:
  GrowableArray<std::pair<intptr_t, intptr_t>> feedback_;

  friend class Cids;
};

typedef GrowableArray<Value*> InputsArray;
typedef ZoneGrowableArray<MoveArgumentInstr*> MoveArgumentsArray;

template <typename Trait>
class InstructionIndexedPropertyIterable {
 public:
  struct Iterator {
    const Instruction* instr;
    intptr_t index;

    decltype(Trait::At(instr, index)) operator*() const {
      return Trait::At(instr, index);
    }
    Iterator& operator++() {
      index++;
      return *this;
    }

    bool operator==(const Iterator& other) {
      return instr == other.instr && index == other.index;
    }

    bool operator!=(const Iterator& other) { return !(*this == other); }
  };

  explicit InstructionIndexedPropertyIterable(const Instruction* instr)
      : instr_(instr) {}

  Iterator begin() const { return {instr_, 0}; }
  Iterator end() const { return {instr_, Trait::Length(instr_)}; }

 private:
  const Instruction* instr_;
};

class ValueListIterable {
 public:
  struct Iterator {
    Value* value;

    Value* operator*() const { return value; }

    Iterator& operator++() {
      value = value->next_use();
      return *this;
    }

    bool operator==(const Iterator& other) { return value == other.value; }

    bool operator!=(const Iterator& other) { return !(*this == other); }
  };

  explicit ValueListIterable(Value* value) : value_(value) {}

  Iterator begin() const { return {value_}; }
  Iterator end() const { return {nullptr}; }

 private:
  Value* value_;
};

class Instruction : public ZoneAllocated {
 public:
#define DECLARE_TAG(type, attrs) k##type,
  enum Tag { FOR_EACH_INSTRUCTION(DECLARE_TAG) kNumInstructions };
#undef DECLARE_TAG

  static const intptr_t kInstructionAttrs[kNumInstructions];

  enum SpeculativeMode {
    // Types of inputs should be checked when unboxing for this instruction.
    kGuardInputs,
    // Each input is guaranteed to have a valid type for the input
    // representation and its type should not be checked when unboxing.
    kNotSpeculative
  };

  // If the source has the inlining ID of the root function, then don't set
  // the inlining ID to that; instead, treat it as unset.
  explicit Instruction(const InstructionSource& source,
                       intptr_t deopt_id = DeoptId::kNone)
      : deopt_id_(deopt_id), inlining_id_(source.inlining_id) {}

  explicit Instruction(intptr_t deopt_id = DeoptId::kNone)
      : Instruction(InstructionSource(), deopt_id) {}

  virtual ~Instruction() {}

  virtual Tag tag() const = 0;

  virtual intptr_t statistics_tag() const { return tag(); }

  intptr_t deopt_id() const {
    ASSERT(ComputeCanDeoptimize() || ComputeCanDeoptimizeAfterCall() ||
           CanBecomeDeoptimizationTarget() || MayThrow() ||
           CompilerState::Current().is_aot());
    return GetDeoptId();
  }

  static const ICData* GetICData(
      const ZoneGrowableArray<const ICData*>& ic_data_array,
      intptr_t deopt_id,
      bool is_static_call);

  virtual TokenPosition token_pos() const { return TokenPosition::kNoSource; }

  // Returns the source information for this instruction.
  InstructionSource source() const {
    return InstructionSource(token_pos(), inlining_id());
  }

  virtual intptr_t InputCount() const = 0;
  virtual Value* InputAt(intptr_t i) const = 0;
  void SetInputAt(intptr_t i, Value* value) {
    ASSERT(value != nullptr);
    value->set_instruction(this);
    value->set_use_index(i);
    RawSetInputAt(i, value);
  }

  struct InputsTrait {
    static Definition* At(const Instruction* instr, intptr_t index) {
      return instr->InputAt(index)->definition();
    }

    static intptr_t Length(const Instruction* instr) {
      return instr->InputCount();
    }
  };

  using InputsIterable = InstructionIndexedPropertyIterable<InputsTrait>;

  InputsIterable inputs() { return InputsIterable(this); }

  // Remove all inputs (including in the environment) from their
  // definition's use lists.
  void UnuseAllInputs();

  // Call instructions override this function and return the number of
  // pushed arguments.
  virtual intptr_t ArgumentCount() const { return 0; }
  inline Value* ArgumentValueAt(intptr_t index) const;
  inline Definition* ArgumentAt(intptr_t index) const;

  // Sets array of MoveArgument instructions.
  virtual void SetMoveArguments(MoveArgumentsArray* move_arguments) {
    UNREACHABLE();
  }
  // Returns array of MoveArgument instructions
  virtual MoveArgumentsArray* GetMoveArguments() const {
    UNREACHABLE();
    return nullptr;
  }
  // Replace inputs with separate MoveArgument instructions detached from call.
  virtual void ReplaceInputsWithMoveArguments(
      MoveArgumentsArray* move_arguments) {
    UNREACHABLE();
  }
  bool HasMoveArguments() const { return GetMoveArguments() != nullptr; }

  // Replaces direct uses of arguments with uses of corresponding MoveArgument
  // instructions.
  void RepairArgumentUsesInEnvironment() const;

  // Returns true, if this instruction can deoptimize with its current inputs.
  // This property can change if we add or remove redefinitions that constrain
  // the type or the range of input operands during compilation.
  virtual bool ComputeCanDeoptimize() const = 0;

  virtual bool ComputeCanDeoptimizeAfterCall() const {
    // TODO(dartbug.com/45213): Incrementally migrate IR instructions from using
    // [ComputeCanDeoptimize] to [ComputeCanDeoptimizeAfterCall] if they
    // can only lazy deoptimize.
    return false;
  }

  // Once we removed the deopt environment, we assume that this
  // instruction can't deoptimize.
  bool CanDeoptimize() const {
    return env() != nullptr &&
           (ComputeCanDeoptimize() || ComputeCanDeoptimizeAfterCall());
  }

  // Visiting support.
  virtual void Accept(InstructionVisitor* visitor) = 0;

  Instruction* previous() const { return previous_; }
  void set_previous(Instruction* instr) {
    ASSERT(!IsBlockEntry());
    previous_ = instr;
  }

  Instruction* next() const { return next_; }
  void set_next(Instruction* instr) {
    ASSERT(!IsGraphEntry());
    ASSERT(!IsReturn());
    ASSERT(!IsBranch() || (instr == nullptr));
    ASSERT(!IsPhi());
    ASSERT(instr == nullptr || !instr->IsBlockEntry());
    // TODO(fschneider): Also add Throw and ReThrow to the list of instructions
    // that do not have a successor. Currently, the graph builder will continue
    // to append instruction in case of a Throw inside an expression. This
    // condition should be handled in the graph builder
    next_ = instr;
  }

  // Link together two instruction.
  void LinkTo(Instruction* next) {
    ASSERT(this != next);
    this->set_next(next);
    next->set_previous(this);
  }

  // Removed this instruction from the graph, after use lists have been
  // computed.  If the instruction is a definition with uses, those uses are
  // unaffected (so the instruction can be reinserted, e.g., hoisting).
  Instruction* RemoveFromGraph(bool return_previous = true);

  // Normal instructions can have 0 (inside a block) or 1 (last instruction in
  // a block) successors. Branch instruction with >1 successors override this
  // function.
  virtual intptr_t SuccessorCount() const;
  virtual BlockEntryInstr* SuccessorAt(intptr_t index) const;

  struct SuccessorsTrait {
    static BlockEntryInstr* At(const Instruction* instr, intptr_t index) {
      return instr->SuccessorAt(index);
    }

    static intptr_t Length(const Instruction* instr) {
      return instr->SuccessorCount();
    }
  };

  using SuccessorsIterable =
      InstructionIndexedPropertyIterable<SuccessorsTrait>;

  inline SuccessorsIterable successors() const {
    return SuccessorsIterable(this);
  }

  void Goto(JoinEntryInstr* entry);

  virtual const char* DebugName() const = 0;

#if defined(DEBUG)
  // Checks that the field stored in an instruction has proper form:
  // - must be a zone-handle
  // - In background compilation, must be cloned.
  // Aborts if field is not OK.
  void CheckField(const Field& field) const;
#else
  void CheckField(const Field& field) const {}
#endif  // DEBUG

  // Printing support.
  const char* ToCString() const;
  PRINT_TO_SUPPORT
  PRINT_OPERANDS_TO_SUPPORT

#define DECLARE_INSTRUCTION_TYPE_CHECK(Name, Type)                             \
  bool Is##Name() const { return (As##Name() != nullptr); }                    \
  Type* As##Name() {                                                           \
    auto const_this = static_cast<const Instruction*>(this);                   \
    return const_cast<Type*>(const_this->As##Name());                          \
  }                                                                            \
  virtual const Type* As##Name() const { return nullptr; }
#define INSTRUCTION_TYPE_CHECK(Name, Attrs)                                    \
  DECLARE_INSTRUCTION_TYPE_CHECK(Name, Name##Instr)

  DECLARE_INSTRUCTION_TYPE_CHECK(Definition, Definition)
  DECLARE_INSTRUCTION_TYPE_CHECK(BlockEntryWithInitialDefs,
                                 BlockEntryWithInitialDefs)
  FOR_EACH_INSTRUCTION(INSTRUCTION_TYPE_CHECK)
  FOR_EACH_ABSTRACT_INSTRUCTION(INSTRUCTION_TYPE_CHECK)

#undef INSTRUCTION_TYPE_CHECK
#undef DECLARE_INSTRUCTION_TYPE_CHECK

  template <typename T>
  T* Cast() {
    return static_cast<T*>(this);
  }

  template <typename T>
  const T* Cast() const {
    return static_cast<const T*>(this);
  }

  // Returns structure describing location constraints required
  // to emit native code for this instruction.
  LocationSummary* locs() {
    ASSERT(locs_ != nullptr);
    return locs_;
  }

  bool HasLocs() const { return locs_ != nullptr; }

  virtual LocationSummary* MakeLocationSummary(Zone* zone,
                                               bool is_optimizing) const = 0;

  void InitializeLocationSummary(Zone* zone, bool optimizing) {
    ASSERT(locs_ == nullptr);
    locs_ = MakeLocationSummary(zone, optimizing);
  }

  // Makes a new call location summary (or uses `locs`) and initializes the
  // output register constraints depending on the representation of [instr].
  static LocationSummary* MakeCallSummary(Zone* zone,
                                          const Instruction* instr,
                                          LocationSummary* locs = nullptr);

  virtual void EmitNativeCode(FlowGraphCompiler* compiler) { UNIMPLEMENTED(); }

  Environment* env() const { return env_; }
  void SetEnvironment(Environment* deopt_env);
  void RemoveEnvironment();
  void ReplaceInEnvironment(Definition* current, Definition* replacement);

  virtual intptr_t NumberOfInputsConsumedBeforeCall() const { return 0; }

  // Different compiler passes can assign pass specific ids to the instruction.
  // Only one id can be stored at a time.
  intptr_t GetPassSpecificId(CompilerPass::Id pass) const {
    return (PassSpecificId::DecodePass(pass_specific_id_) == pass)
               ? PassSpecificId::DecodeId(pass_specific_id_)
               : PassSpecificId::kNoId;
  }
  void SetPassSpecificId(CompilerPass::Id pass, intptr_t id) {
    pass_specific_id_ = PassSpecificId::Encode(pass, id);
  }
  bool HasPassSpecificId(CompilerPass::Id pass) const {
    return (PassSpecificId::DecodePass(pass_specific_id_) == pass) &&
           (PassSpecificId::DecodeId(pass_specific_id_) !=
            PassSpecificId::kNoId);
  }

  bool HasUnmatchedInputRepresentations() const;

  // Returns representation expected for the input operand at the given index.
  virtual Representation RequiredInputRepresentation(intptr_t idx) const {
    return kTagged;
  }

  SpeculativeMode SpeculativeModeOfInputs() const {
    for (intptr_t i = 0; i < InputCount(); i++) {
      if (SpeculativeModeOfInput(i) == kGuardInputs) {
        return kGuardInputs;
      }
    }
    return kNotSpeculative;
  }

  // By default, instructions should check types of inputs when unboxing
  virtual SpeculativeMode SpeculativeModeOfInput(intptr_t index) const {
    return kGuardInputs;
  }

  // Representation of the value produced by this computation.
  virtual Representation representation() const { return kTagged; }

  bool WasEliminated() const { return next() == nullptr; }

  // Returns deoptimization id that corresponds to the deoptimization target
  // that input operands conversions inserted for this instruction can jump
  // to.
  virtual intptr_t DeoptimizationTarget() const {
    UNREACHABLE();
    return DeoptId::kNone;
  }

  // Returns a replacement for the instruction or nullptr if the instruction can
  // be eliminated.  By default returns the this instruction which means no
  // change.
  virtual Instruction* Canonicalize(FlowGraph* flow_graph);

  // Insert this instruction before 'next' after use lists are computed.
  // Instructions cannot be inserted before a block entry or any other
  // instruction without a previous instruction.
  void InsertBefore(Instruction* next) { InsertAfter(next->previous()); }

  // Insert this instruction after 'prev' after use lists are computed.
  void InsertAfter(Instruction* prev);

  // Append an instruction to the current one and return the tail.
  // This function updated def-use chains of the newly appended
  // instruction.
  Instruction* AppendInstruction(Instruction* tail);

  // Returns true if CSE and LICM are allowed for this instruction.
  virtual bool AllowsCSE() const { return false; }

  // Returns true if this instruction has any side-effects besides storing.
  // See StoreFieldInstr::HasUnknownSideEffects() for rationale.
  virtual bool HasUnknownSideEffects() const = 0;

  // Whether this instruction can call Dart code without going through
  // the runtime.
  //
  // Must be true for any instruction which can call Dart code without
  // first creating an exit frame to transition into the runtime.
  //
  // See also WriteBarrierElimination and Thread::RememberLiveTemporaries().
  virtual bool CanCallDart() const { return false; }

  virtual bool CanTriggerGC() const;

  // Get the block entry for this instruction.
  virtual BlockEntryInstr* GetBlock();

  virtual intptr_t inlining_id() const { return inlining_id_; }
  virtual void set_inlining_id(intptr_t value) {
    ASSERT(value >= 0);
    ASSERT(!has_inlining_id() || inlining_id_ == value);
    inlining_id_ = value;
  }
  virtual bool has_inlining_id() const { return inlining_id_ >= 0; }

  // Returns a hash code for use with hash maps.
  virtual uword Hash() const;

  // Compares two instructions.  Returns true, iff:
  // 1. They have the same tag.
  // 2. All input operands are Equals.
  // 3. They satisfy AttributesEqual.
  bool Equals(const Instruction& other) const;

  // Compare attributes of a instructions (except input operands and tag).
  // All instructions that participate in CSE have to override this function.
  // This function can assume that the argument has the same type as this.
  virtual bool AttributesEqual(const Instruction& other) const {
    UNREACHABLE();
    return false;
  }

  void InheritDeoptTarget(Zone* zone, Instruction* other);

  bool NeedsEnvironment() const {
    return ComputeCanDeoptimize() || ComputeCanDeoptimizeAfterCall() ||
           CanBecomeDeoptimizationTarget() || MayThrow();
  }

  virtual bool CanBecomeDeoptimizationTarget() const { return false; }

  void InheritDeoptTargetAfter(FlowGraph* flow_graph,
                               Definition* call,
                               Definition* result);

  virtual bool MayThrow() const = 0;

  bool IsDominatedBy(Instruction* dom);

  void ClearEnv() { env_ = nullptr; }

  void Unsupported(FlowGraphCompiler* compiler);

  static bool SlowPathSharingSupported(bool is_optimizing) {
#if defined(TARGET_ARCH_IA32)
    return false;
#else
    return FLAG_enable_slow_path_sharing && FLAG_precompiled_mode &&
           is_optimizing;
#endif
  }

  virtual bool UseSharedSlowPathStub(bool is_optimizing) const { return false; }

  // 'RegisterKindForResult()' returns the register kind necessary to hold the
  // result.
  //
  // This is not virtual because instructions should override representation()
  // instead.
  Location::Kind RegisterKindForResult() const {
    const Representation rep = representation();
    if ((rep == kUnboxedFloat) || (rep == kUnboxedDouble) ||
        (rep == kUnboxedFloat32x4) || (rep == kUnboxedInt32x4) ||
        (rep == kUnboxedFloat64x2)) {
      return Location::kFpuRegister;
    }
    return Location::kRegister;
  }

  DECLARE_CUSTOM_SERIALIZATION(Instruction)
  DECLARE_EXTRA_SERIALIZATION

 protected:
  // GetDeoptId and/or CopyDeoptIdFrom.
  friend class CallSiteInliner;
  friend class LICM;
  friend class ComparisonInstr;
  friend class Scheduler;
  friend class BlockEntryInstr;
  friend class CatchBlockEntryInstr;  // deopt_id_
  friend class DebugStepCheckInstr;   // deopt_id_
  friend class StrictCompareInstr;    // deopt_id_

  // Fetch deopt id without checking if this computation can deoptimize.
  intptr_t GetDeoptId() const { return deopt_id_; }

  virtual void CopyDeoptIdFrom(const Instruction& instr) {
    deopt_id_ = instr.deopt_id_;
  }

  // Write/read locs and environment, but not inputs.
  // Used when one instruction embeds another and reuses their inputs
  // (e.g. Branch/IfThenElse/CheckCondition wrap Comparison).
  void WriteExtraWithoutInputs(FlowGraphSerializer* s);
  void ReadExtraWithoutInputs(FlowGraphDeserializer* d);

 private:
  friend class BranchInstr;          // For RawSetInputAt.
  friend class IfThenElseInstr;      // For RawSetInputAt.
  friend class CheckConditionInstr;  // For RawSetInputAt.

  virtual void RawSetInputAt(intptr_t i, Value* value) = 0;

  class PassSpecificId {
   public:
    static intptr_t Encode(CompilerPass::Id pass, intptr_t id) {
      return (id << kPassBits) | pass;
    }

    static CompilerPass::Id DecodePass(intptr_t value) {
      return static_cast<CompilerPass::Id>(value & Utils::NBitMask(kPassBits));
    }

    static intptr_t DecodeId(intptr_t value) { return (value >> kPassBits); }

    static constexpr intptr_t kNoId = -1;

   private:
    static constexpr intptr_t kPassBits = 8;
    static_assert(CompilerPass::kNumPasses <= (1 << kPassBits),
                  "Pass Id does not fit into the bit field");
  };

  intptr_t deopt_id_ = DeoptId::kNone;
  intptr_t pass_specific_id_ = PassSpecificId::kNoId;
  Instruction* previous_ = nullptr;
  Instruction* next_ = nullptr;
  Environment* env_ = nullptr;
  LocationSummary* locs_ = nullptr;
  intptr_t inlining_id_;

  DISALLOW_COPY_AND_ASSIGN(Instruction);
};

struct BranchLabels {
  compiler::Label* true_label;
  compiler::Label* false_label;
  compiler::Label* fall_through;
};

class PureInstruction : public Instruction {
 public:
  explicit PureInstruction(intptr_t deopt_id) : Instruction(deopt_id) {}
  explicit PureInstruction(const InstructionSource& source, intptr_t deopt_id)
      : Instruction(source, deopt_id) {}

  virtual bool AllowsCSE() const { return true; }
  virtual bool HasUnknownSideEffects() const { return false; }

  DECLARE_EMPTY_SERIALIZATION(PureInstruction, Instruction)
};

// Types to be used as ThrowsTrait for TemplateInstruction/TemplateDefinition.
struct Throws {
  static constexpr bool kCanThrow = true;
};

struct NoThrow {
  static constexpr bool kCanThrow = false;
};

// Types to be used as CSETrait for TemplateInstruction/TemplateDefinition.
// Pure instructions are those that allow CSE and have no effects and
// no dependencies.
template <typename DefaultBase, typename PureBase>
struct Pure {
  typedef PureBase Base;
};

template <typename DefaultBase, typename PureBase>
struct NoCSE {
  typedef DefaultBase Base;
};

template <intptr_t N,
          typename ThrowsTrait,
          template <typename Default, typename Pure> class CSETrait = NoCSE>
class TemplateInstruction
    : public CSETrait<Instruction, PureInstruction>::Base {
 public:
  using BaseClass = typename CSETrait<Instruction, PureInstruction>::Base;

  explicit TemplateInstruction(intptr_t deopt_id = DeoptId::kNone)
      : BaseClass(deopt_id), inputs_() {}

  TemplateInstruction(const InstructionSource& source,
                      intptr_t deopt_id = DeoptId::kNone)
      : BaseClass(source, deopt_id), inputs_() {}

  virtual intptr_t InputCount() const { return N; }
  virtual Value* InputAt(intptr_t i) const { return inputs_[i]; }

  virtual bool MayThrow() const { return ThrowsTrait::kCanThrow; }

  DECLARE_EMPTY_SERIALIZATION(TemplateInstruction, BaseClass)

 protected:
  EmbeddedArray<Value*, N> inputs_;

 private:
  virtual void RawSetInputAt(intptr_t i, Value* value) { inputs_[i] = value; }
};

class MoveOperands : public ZoneAllocated {
 public:
  MoveOperands(Location dest, Location src) : dest_(dest), src_(src) {}
  MoveOperands(const MoveOperands& other)
      : ZoneAllocated(), dest_(other.dest_), src_(other.src_) {}

  MoveOperands& operator=(const MoveOperands& other) {
    dest_ = other.dest_;
    src_ = other.src_;
    return *this;
  }

  Location src() const { return src_; }
  Location dest() const { return dest_; }

  Location* src_slot() { return &src_; }
  Location* dest_slot() { return &dest_; }

  void set_src(const Location& value) { src_ = value; }
  void set_dest(const Location& value) { dest_ = value; }

  // The parallel move resolver marks moves as "in-progress" by clearing the
  // destination (but not the source).
  Location MarkPending() {
    ASSERT(!IsPending());
    Location dest = dest_;
    dest_ = Location::NoLocation();
    return dest;
  }

  void ClearPending(Location dest) {
    ASSERT(IsPending());
    dest_ = dest;
  }

  bool IsPending() const {
    ASSERT(!src_.IsInvalid() || dest_.IsInvalid());
    return dest_.IsInvalid() && !src_.IsInvalid();
  }

  // True if this move a move from the given location.
  bool Blocks(Location loc) const {
    return !IsEliminated() && src_.Equals(loc);
  }

  // A move is redundant if it's been eliminated, if its source and
  // destination are the same, or if its destination is unneeded.
  bool IsRedundant() const {
    return IsEliminated() || dest_.IsInvalid() || src_.Equals(dest_);
  }

  // We clear both operands to indicate move that's been eliminated.
  void Eliminate() { src_ = dest_ = Location::NoLocation(); }
  bool IsEliminated() const {
    ASSERT(!src_.IsInvalid() || dest_.IsInvalid());
    return src_.IsInvalid();
  }

  void Write(FlowGraphSerializer* s) const;
  explicit MoveOperands(FlowGraphDeserializer* d);

 private:
  Location dest_;
  Location src_;
};

class ParallelMoveInstr : public TemplateInstruction<0, NoThrow> {
 public:
  ParallelMoveInstr() : moves_(4) {}

  DECLARE_INSTRUCTION(ParallelMove)

  virtual bool ComputeCanDeoptimize() const { return false; }

  virtual bool HasUnknownSideEffects() const {
    UNREACHABLE();  // This instruction never visited by optimization passes.
    return false;
  }

  const GrowableArray<MoveOperands*>& moves() const { return moves_; }

  MoveOperands* AddMove(Location dest, Location src) {
    MoveOperands* move = new MoveOperands(dest, src);
    moves_.Add(move);
    return move;
  }

  MoveOperands* MoveOperandsAt(intptr_t index) const { return moves_[index]; }

  intptr_t NumMoves() const { return moves_.length(); }

  bool IsRedundant() const;

  virtual TokenPosition token_pos() const {
    return TokenPosition::kParallelMove;
  }

  const MoveSchedule& move_schedule() const {
    ASSERT(move_schedule_ != nullptr);
    return *move_schedule_;
  }

  void set_move_schedule(const MoveSchedule& schedule) {
    move_schedule_ = &schedule;
  }

  PRINT_TO_SUPPORT
  DECLARE_EMPTY_SERIALIZATION(ParallelMoveInstr, TemplateInstruction)
  DECLARE_EXTRA_SERIALIZATION

 private:
  GrowableArray<MoveOperands*> moves_;  // Elements cannot be null.
  const MoveSchedule* move_schedule_ = nullptr;

  DISALLOW_COPY_AND_ASSIGN(ParallelMoveInstr);
};

// Basic block entries are administrative nodes.  There is a distinguished
// graph entry with no predecessor.  Joins are the only nodes with multiple
// predecessors.  Targets are all other basic block entries.  The types
// enforce edge-split form---joins are forbidden as the successors of
// branches.
class BlockEntryInstr : public TemplateInstruction<0, NoThrow> {
 public:
  virtual intptr_t PredecessorCount() const = 0;
  virtual BlockEntryInstr* PredecessorAt(intptr_t index) const = 0;

  intptr_t preorder_number() const { return preorder_number_; }
  void set_preorder_number(intptr_t number) { preorder_number_ = number; }

  intptr_t postorder_number() const { return postorder_number_; }
  void set_postorder_number(intptr_t number) { postorder_number_ = number; }

  intptr_t block_id() const { return block_id_; }

  // NOTE: These are SSA positions and not token positions. These are used by
  // the register allocator.
  void set_start_pos(intptr_t pos) { start_pos_ = pos; }
  intptr_t start_pos() const { return start_pos_; }
  void set_end_pos(intptr_t pos) { end_pos_ = pos; }
  intptr_t end_pos() const { return end_pos_; }

  BlockEntryInstr* dominator() const { return dominator_; }
  BlockEntryInstr* ImmediateDominator() const;

  const GrowableArray<BlockEntryInstr*>& dominated_blocks() {
    return dominated_blocks_;
  }

  void AddDominatedBlock(BlockEntryInstr* block) {
    ASSERT(!block->IsFunctionEntry() || this->IsGraphEntry());
    block->set_dominator(this);
    dominated_blocks_.Add(block);
  }
  void ClearDominatedBlocks() { dominated_blocks_.Clear(); }

  bool Dominates(BlockEntryInstr* other) const;

  Instruction* last_instruction() const { return last_instruction_; }
  void set_last_instruction(Instruction* instr) { last_instruction_ = instr; }

  ParallelMoveInstr* parallel_move() const { return parallel_move_; }

  bool HasParallelMove() const { return parallel_move_ != nullptr; }

  bool HasNonRedundantParallelMove() const {
    return HasParallelMove() && !parallel_move()->IsRedundant();
  }

  ParallelMoveInstr* GetParallelMove() {
    if (parallel_move_ == nullptr) {
      parallel_move_ = new ParallelMoveInstr();
    }
    return parallel_move_;
  }

  // Discover basic-block structure of the current block.  Must be called
  // on all graph blocks in preorder to yield valid results.  As a side effect,
  // the block entry instructions in the graph are assigned preorder numbers.
  // The array 'preorder' maps preorder block numbers to the block entry
  // instruction with that number.  The depth first spanning tree is recorded
  // in the array 'parent', which maps preorder block numbers to the preorder
  // number of the block's spanning-tree parent.  As a side effect of this
  // function, the set of basic block predecessors (e.g., block entry
  // instructions of predecessor blocks) and also the last instruction in the
  // block is recorded in each entry instruction.  Returns true when called the
  // first time on this particular block within one graph traversal, and false
  // on all successive calls.
  bool DiscoverBlock(BlockEntryInstr* predecessor,
                     GrowableArray<BlockEntryInstr*>* preorder,
                     GrowableArray<intptr_t>* parent);

  virtual bool CanBecomeDeoptimizationTarget() const {
    // BlockEntry environment is copied to Goto and Branch instructions
    // when we insert new blocks targeting this block.
    return true;
  }

  virtual bool ComputeCanDeoptimize() const { return false; }

  virtual bool HasUnknownSideEffects() const { return false; }

  intptr_t try_index() const { return try_index_; }
  void set_try_index(intptr_t index) { try_index_ = index; }

  // True for blocks inside a try { } region.
  bool InsideTryBlock() const { return try_index_ != kInvalidTryIndex; }

  // Loop related methods.
  LoopInfo* loop_info() const { return loop_info_; }
  void set_loop_info(LoopInfo* loop_info) { loop_info_ = loop_info; }
  bool IsLoopHeader() const;
  intptr_t NestingDepth() const;

  virtual BlockEntryInstr* GetBlock() { return this; }

  virtual TokenPosition token_pos() const {
    return TokenPosition::kControlFlow;
  }

  // Helper to mutate the graph during inlining. This block should be
  // replaced with new_block as a predecessor of all of this block's
  // successors.
  void ReplaceAsPredecessorWith(BlockEntryInstr* new_block);

  void set_block_id(intptr_t block_id) { block_id_ = block_id; }

  // Stack-based IR bookkeeping.
  intptr_t stack_depth() const { return stack_depth_; }
  void set_stack_depth(intptr_t s) { stack_depth_ = s; }

  // For all instruction in this block: Remove all inputs (including in the
  // environment) from their definition's use lists for all instructions.
  void ClearAllInstructions();

  class InstructionsIterable {
   public:
    explicit InstructionsIterable(BlockEntryInstr* block) : block_(block) {}

    inline ForwardInstructionIterator begin() const;
    inline ForwardInstructionIterator end() const;

   private:
    BlockEntryInstr* block_;
  };

  InstructionsIterable instructions() { return InstructionsIterable(this); }

  DECLARE_ABSTRACT_INSTRUCTION(BlockEntry)

  DECLARE_CUSTOM_SERIALIZATION(BlockEntryInstr)
  DECLARE_EXTRA_SERIALIZATION

 protected:
  BlockEntryInstr(intptr_t block_id,
                  intptr_t try_index,
                  intptr_t deopt_id,
                  intptr_t stack_depth)
      : TemplateInstruction(deopt_id),
        block_id_(block_id),
        try_index_(try_index),
        stack_depth_(stack_depth),
        dominated_blocks_(1) {}

  // Perform a depth first search to find OSR entry and
  // link it to the given graph entry.
  bool FindOsrEntryAndRelink(GraphEntryInstr* graph_entry,
                             Instruction* parent,
                             BitVector* block_marks);

 private:
  virtual void ClearPredecessors() = 0;
  virtual void AddPredecessor(BlockEntryInstr* predecessor) = 0;

  void set_dominator(BlockEntryInstr* instr) { dominator_ = instr; }

  intptr_t block_id_;
  intptr_t try_index_;
  intptr_t preorder_number_ = -1;
  intptr_t postorder_number_ = -1;
  // Expected stack depth on entry (for stack-based IR only).
  intptr_t stack_depth_;
  // Starting and ending lifetime positions for this block.  Used by
  // the linear scan register allocator.
  intptr_t start_pos_ = -1;
  intptr_t end_pos_ = -1;
  // Immediate dominator, nullptr for graph entry.
  BlockEntryInstr* dominator_ = nullptr;
  // TODO(fschneider): Optimize the case of one child to save space.
  GrowableArray<BlockEntryInstr*> dominated_blocks_;
  Instruction* last_instruction_ = nullptr;

  // Parallel move that will be used by linear scan register allocator to
  // connect live ranges at the start of the block.
  ParallelMoveInstr* parallel_move_ = nullptr;

  // Closest enveloping loop in loop hierarchy (nullptr at nesting depth 0).
  LoopInfo* loop_info_ = nullptr;

  DISALLOW_COPY_AND_ASSIGN(BlockEntryInstr);
};

class ForwardInstructionIterator {
 public:
  ForwardInstructionIterator(const ForwardInstructionIterator& other) = default;
  ForwardInstructionIterator& operator=(
      const ForwardInstructionIterator& other) = default;

  ForwardInstructionIterator() : current_(nullptr) {}

  explicit ForwardInstructionIterator(BlockEntryInstr* block_entry)
      : current_(block_entry) {
    Advance();
  }

  void Advance() {
    ASSERT(!Done());
    current_ = current_->next();
  }

  bool Done() const { return current_ == nullptr; }

  // Removes 'current_' from graph and sets 'current_' to previous instruction.
  void RemoveCurrentFromGraph();

  Instruction* Current() const { return current_; }

  Instruction* operator*() const { return Current(); }

  bool operator==(const ForwardInstructionIterator& other) const {
    return current_ == other.current_;
  }

  bool operator!=(const ForwardInstructionIterator& other) const {
    return !(*this == other);
  }

  ForwardInstructionIterator& operator++() {
    Advance();
    return *this;
  }

 private:
  Instruction* current_;
};

ForwardInstructionIterator BlockEntryInstr::InstructionsIterable::begin()
    const {
  return ForwardInstructionIterator(block_);
}

ForwardInstructionIterator BlockEntryInstr::InstructionsIterable::end() const {
  return ForwardInstructionIterator();
}

class BackwardInstructionIterator : public ValueObject {
 public:
  explicit BackwardInstructionIterator(BlockEntryInstr* block_entry)
      : block_entry_(block_entry), current_(block_entry->last_instruction()) {
    ASSERT(block_entry_->previous() == nullptr);
  }

  void Advance() {
    ASSERT(!Done());
    current_ = current_->previous();
  }

  bool Done() const { return current_ == block_entry_; }

  void RemoveCurrentFromGraph();

  Instruction* Current() const { return current_; }

 private:
  BlockEntryInstr* block_entry_;
  Instruction* current_;
};

// Base class shared by all block entries which define initial definitions.
//
// The initial definitions define parameters, special parameters and constants.
class BlockEntryWithInitialDefs : public BlockEntryInstr {
 public:
  BlockEntryWithInitialDefs(intptr_t block_id,
                            intptr_t try_index,
                            intptr_t deopt_id,
                            intptr_t stack_depth)
      : BlockEntryInstr(block_id, try_index, deopt_id, stack_depth) {}

  GrowableArray<Definition*>* initial_definitions() {
    return &initial_definitions_;
  }
  const GrowableArray<Definition*>* initial_definitions() const {
    return &initial_definitions_;
  }

  virtual BlockEntryWithInitialDefs* AsBlockEntryWithInitialDefs() {
    return this;
  }
  virtual const BlockEntryWithInitialDefs* AsBlockEntryWithInitialDefs() const {
    return this;
  }

  DECLARE_CUSTOM_SERIALIZATION(BlockEntryWithInitialDefs)
  DECLARE_EXTRA_SERIALIZATION

 protected:
  void PrintInitialDefinitionsTo(BaseTextBuffer* f) const;

 private:
  GrowableArray<Definition*> initial_definitions_;

  DISALLOW_COPY_AND_ASSIGN(BlockEntryWithInitialDefs);
};

class GraphEntryInstr : public BlockEntryWithInitialDefs {
 public:
  GraphEntryInstr(const ParsedFunction& parsed_function, intptr_t osr_id);

  DECLARE_INSTRUCTION(GraphEntry)

  virtual intptr_t PredecessorCount() const { return 0; }
  virtual BlockEntryInstr* PredecessorAt(intptr_t index) const {
    UNREACHABLE();
    return nullptr;
  }
  virtual intptr_t SuccessorCount() const;
  virtual BlockEntryInstr* SuccessorAt(intptr_t index) const;

  void AddCatchEntry(CatchBlockEntryInstr* entry) { catch_entries_.Add(entry); }

  CatchBlockEntryInstr* GetCatchEntry(intptr_t index);

  void AddIndirectEntry(IndirectEntryInstr* entry) {
    indirect_entries_.Add(entry);
  }

  ConstantInstr* constant_null();

  void RelinkToOsrEntry(Zone* zone, intptr_t max_block_id);
  bool IsCompiledForOsr() const;
  intptr_t osr_id() const { return osr_id_; }

  intptr_t entry_count() const { return entry_count_; }
  void set_entry_count(intptr_t count) { entry_count_ = count; }

  intptr_t spill_slot_count() const { return spill_slot_count_; }
  void set_spill_slot_count(intptr_t count) {
    ASSERT(count >= 0);
    spill_slot_count_ = count;
  }

  // Returns true if this flow graph needs a stack frame.
  bool NeedsFrame() const { return needs_frame_; }
  void MarkFrameless() { needs_frame_ = false; }

  // Number of stack slots reserved for compiling try-catch. For functions
  // without try-catch, this is 0. Otherwise, it is the number of local
  // variables.
  intptr_t fixed_slot_count() const { return fixed_slot_count_; }
  void set_fixed_slot_count(intptr_t count) {
    ASSERT(count >= 0);
    fixed_slot_count_ = count;
  }
  FunctionEntryInstr* normal_entry() const { return normal_entry_; }
  FunctionEntryInstr* unchecked_entry() const { return unchecked_entry_; }
  void set_normal_entry(FunctionEntryInstr* entry) { normal_entry_ = entry; }
  void set_unchecked_entry(FunctionEntryInstr* target) {
    unchecked_entry_ = target;
  }
  OsrEntryInstr* osr_entry() const { return osr_entry_; }
  void set_osr_entry(OsrEntryInstr* entry) { osr_entry_ = entry; }

  const ParsedFunction& parsed_function() const { return parsed_function_; }

  const GrowableArray<CatchBlockEntryInstr*>& catch_entries() const {
    return catch_entries_;
  }

  const GrowableArray<IndirectEntryInstr*>& indirect_entries() const {
    return indirect_entries_;
  }

  bool HasSingleEntryPoint() const {
    return catch_entries().is_empty() && unchecked_entry() == nullptr;
  }

  PRINT_TO_SUPPORT
  DECLARE_CUSTOM_SERIALIZATION(GraphEntryInstr)
  DECLARE_EXTRA_SERIALIZATION

 private:
  GraphEntryInstr(const ParsedFunction& parsed_function,
                  intptr_t osr_id,
                  intptr_t deopt_id);

  virtual void ClearPredecessors() {}
  virtual void AddPredecessor(BlockEntryInstr* predecessor) { UNREACHABLE(); }

  const ParsedFunction& parsed_function_;
  FunctionEntryInstr* normal_entry_ = nullptr;
  FunctionEntryInstr* unchecked_entry_ = nullptr;
  OsrEntryInstr* osr_entry_ = nullptr;
  GrowableArray<CatchBlockEntryInstr*> catch_entries_;
  // Indirect targets are blocks reachable only through indirect gotos.
  GrowableArray<IndirectEntryInstr*> indirect_entries_;
  const intptr_t osr_id_;
  intptr_t entry_count_;
  intptr_t spill_slot_count_;
  intptr_t fixed_slot_count_;  // For try-catch in optimized code.
  bool needs_frame_ = true;

  DISALLOW_COPY_AND_ASSIGN(GraphEntryInstr);
};

class JoinEntryInstr : public BlockEntryInstr {
 public:
  JoinEntryInstr(intptr_t block_id,
                 intptr_t try_index,
                 intptr_t deopt_id,
                 intptr_t stack_depth = 0)
      : BlockEntryInstr(block_id, try_index, deopt_id, stack_depth),
        phis_(nullptr),
        predecessors_(2)  // Two is the assumed to be the common case.
  {}

  DECLARE_INSTRUCTION(JoinEntry)

  virtual intptr_t PredecessorCount() const { return predecessors_.length(); }
  virtual BlockEntryInstr* PredecessorAt(intptr_t index) const {
    return predecessors_[index];
  }

  // Returns -1 if pred is not in the list.
  intptr_t IndexOfPredecessor(BlockEntryInstr* pred) const;

  ZoneGrowableArray<PhiInstr*>* phis() const { return phis_; }

  PhiInstr* InsertPhi(intptr_t var_index, intptr_t var_count);
  void RemoveDeadPhis(Definition* replacement);

  void InsertPhi(PhiInstr* phi);
  void RemovePhi(PhiInstr* phi);

  virtual bool HasUnknownSideEffects() const { return false; }

  PRINT_TO_SUPPORT

#define FIELD_LIST(F) F(ZoneGrowableArray<PhiInstr*>*, phis_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(JoinEntryInstr,
                                          BlockEntryInstr,
                                          FIELD_LIST)
#undef FIELD_LIST
  DECLARE_EXTRA_SERIALIZATION

 private:
  // Classes that have access to predecessors_ when inlining.
  friend class BlockEntryInstr;
  friend class InlineExitCollector;
  friend class PolymorphicInliner;
  friend class IndirectEntryInstr;  // Access in il_printer.cc.

  // Direct access to phis_ in order to resize it due to phi elimination.
  friend class ConstantPropagator;
  friend class DeadCodeElimination;

  virtual void ClearPredecessors() { predecessors_.Clear(); }
  virtual void AddPredecessor(BlockEntryInstr* predecessor);

  GrowableArray<BlockEntryInstr*> predecessors_;

  DISALLOW_COPY_AND_ASSIGN(JoinEntryInstr);
};

class PhiIterator : public ValueObject {
 public:
  explicit PhiIterator(JoinEntryInstr* join) : phis_(join->phis()), index_(0) {}

  void Advance() {
    ASSERT(!Done());
    index_++;
  }

  bool Done() const {
    return (phis_ == nullptr) || (index_ >= phis_->length());
  }

  PhiInstr* Current() const { return (*phis_)[index_]; }

  // Removes current phi from graph and sets current to previous phi.
  void RemoveCurrentFromGraph();

 private:
  ZoneGrowableArray<PhiInstr*>* phis_;
  intptr_t index_;
};

class TargetEntryInstr : public BlockEntryInstr {
 public:
  TargetEntryInstr(intptr_t block_id,
                   intptr_t try_index,
                   intptr_t deopt_id,
                   intptr_t stack_depth = 0)
      : BlockEntryInstr(block_id, try_index, deopt_id, stack_depth),
        edge_weight_(0.0) {}

  DECLARE_INSTRUCTION(TargetEntry)

  double edge_weight() const { return edge_weight_; }
  void set_edge_weight(double weight) { edge_weight_ = weight; }
  void adjust_edge_weight(double scale_factor) { edge_weight_ *= scale_factor; }

  virtual intptr_t PredecessorCount() const {
    return (predecessor_ == nullptr) ? 0 : 1;
  }
  virtual BlockEntryInstr* PredecessorAt(intptr_t index) const {
    ASSERT((index == 0) && (predecessor_ != nullptr));
    return predecessor_;
  }

  PRINT_TO_SUPPORT

#define FIELD_LIST(F) F(double, edge_weight_)
  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(TargetEntryInstr,
                                          BlockEntryInstr,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  friend class BlockEntryInstr;  // Access to predecessor_ when inlining.

  virtual void ClearPredecessors() { predecessor_ = nullptr; }
  virtual void AddPredecessor(BlockEntryInstr* predecessor) {
    ASSERT(predecessor_ == nullptr);
    predecessor_ = predecessor;
  }

  // Not serialized, set in DiscoverBlocks.
  BlockEntryInstr* predecessor_ = nullptr;

  DISALLOW_COPY_AND_ASSIGN(TargetEntryInstr);
};

// Represents an entrypoint to a function which callers can invoke (i.e. not
// used for OSR entries).
//
// The flow graph builder might decide to create multiple entrypoints
// (e.g. checked/unchecked entrypoints) and will attach those to the
// [GraphEntryInstr].
//
// Every entrypoint has it's own initial definitions.  The SSA renaming
// will insert phi's for parameter instructions if necessary.
class FunctionEntryInstr : public BlockEntryWithInitialDefs {
 public:
  FunctionEntryInstr(GraphEntryInstr* graph_entry,
                     intptr_t block_id,
                     intptr_t try_index,
                     intptr_t deopt_id)
      : BlockEntryWithInitialDefs(block_id,
                                  try_index,
                                  deopt_id,
                                  /*stack_depth=*/0),
        graph_entry_(graph_entry) {}

  DECLARE_INSTRUCTION(FunctionEntry)

  virtual intptr_t PredecessorCount() const {
    return (graph_entry_ == nullptr) ? 0 : 1;
  }
  virtual BlockEntryInstr* PredecessorAt(intptr_t index) const {
    ASSERT(index == 0 && graph_entry_ != nullptr);
    return graph_entry_;
  }

  GraphEntryInstr* graph_entry() const { return graph_entry_; }

  PRINT_TO_SUPPORT
  DECLARE_CUSTOM_SERIALIZATION(FunctionEntryInstr)

 private:
  virtual void ClearPredecessors() { graph_entry_ = nullptr; }
  virtual void AddPredecessor(BlockEntryInstr* predecessor) {
    ASSERT(graph_entry_ == nullptr && predecessor->IsGraphEntry());
    graph_entry_ = predecessor->AsGraphEntry();
  }

  GraphEntryInstr* graph_entry_;

  DISALLOW_COPY_AND_ASSIGN(FunctionEntryInstr);
};

// Represents entry into a function from native code.
//
// Native entries are not allowed to have regular parameters. They should use
// NativeParameter instead (which doesn't count as an initial definition).
class NativeEntryInstr : public FunctionEntryInstr {
 public:
  NativeEntryInstr(const compiler::ffi::CallbackMarshaller& marshaller,
                   GraphEntryInstr* graph_entry,
                   intptr_t block_id,
                   intptr_t try_index,
                   intptr_t deopt_id)
      : FunctionEntryInstr(graph_entry, block_id, try_index, deopt_id),
        marshaller_(marshaller) {}

  DECLARE_INSTRUCTION(NativeEntry)

  PRINT_TO_SUPPORT

#define FIELD_LIST(F)                                                          \
  F(const compiler::ffi::CallbackMarshaller&, marshaller_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(NativeEntryInstr,
                                          FunctionEntryInstr,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  void SaveArguments(FlowGraphCompiler* compiler) const;
  void SaveArgument(FlowGraphCompiler* compiler,
                    const compiler::ffi::NativeLocation& loc) const;
};

// Represents an OSR entrypoint to a function.
//
// The OSR entry has it's own initial definitions.
class OsrEntryInstr : public BlockEntryWithInitialDefs {
 public:
  OsrEntryInstr(GraphEntryInstr* graph_entry,
                intptr_t block_id,
                intptr_t try_index,
                intptr_t deopt_id,
                intptr_t stack_depth)
      : BlockEntryWithInitialDefs(block_id, try_index, deopt_id, stack_depth),
        graph_entry_(graph_entry) {}

  DECLARE_INSTRUCTION(OsrEntry)

  virtual intptr_t PredecessorCount() const {
    return (graph_entry_ == nullptr) ? 0 : 1;
  }
  virtual BlockEntryInstr* PredecessorAt(intptr_t index) const {
    ASSERT(index == 0 && graph_entry_ != nullptr);
    return graph_entry_;
  }

  GraphEntryInstr* graph_entry() const { return graph_entry_; }

  PRINT_TO_SUPPORT
  DECLARE_CUSTOM_SERIALIZATION(OsrEntryInstr)

 private:
  virtual void ClearPredecessors() { graph_entry_ = nullptr; }
  virtual void AddPredecessor(BlockEntryInstr* predecessor) {
    ASSERT(graph_entry_ == nullptr && predecessor->IsGraphEntry());
    graph_entry_ = predecessor->AsGraphEntry();
  }

  GraphEntryInstr* graph_entry_;

  DISALLOW_COPY_AND_ASSIGN(OsrEntryInstr);
};

class IndirectEntryInstr : public JoinEntryInstr {
 public:
  IndirectEntryInstr(intptr_t block_id,
                     intptr_t indirect_id,
                     intptr_t try_index,
                     intptr_t deopt_id)
      : JoinEntryInstr(block_id, try_index, deopt_id),
        indirect_id_(indirect_id) {}

  DECLARE_INSTRUCTION(IndirectEntry)

  intptr_t indirect_id() const { return indirect_id_; }

  PRINT_TO_SUPPORT

#define FIELD_LIST(F) F(const intptr_t, indirect_id_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(IndirectEntryInstr,
                                          JoinEntryInstr,
                                          FIELD_LIST)
#undef FIELD_LIST
};

class CatchBlockEntryInstr : public BlockEntryWithInitialDefs {
 public:
  CatchBlockEntryInstr(bool is_generated,
                       intptr_t block_id,
                       intptr_t try_index,
                       GraphEntryInstr* graph_entry,
                       const Array& handler_types,
                       intptr_t catch_try_index,
                       bool needs_stacktrace,
                       intptr_t deopt_id,
                       const LocalVariable* exception_var,
                       const LocalVariable* stacktrace_var,
                       const LocalVariable* raw_exception_var,
                       const LocalVariable* raw_stacktrace_var)
      : BlockEntryWithInitialDefs(block_id,
                                  try_index,
                                  deopt_id,
                                  /*stack_depth=*/0),
        graph_entry_(graph_entry),
        predecessor_(nullptr),
        catch_handler_types_(Array::ZoneHandle(handler_types.ptr())),
        catch_try_index_(catch_try_index),
        exception_var_(exception_var),
        stacktrace_var_(stacktrace_var),
        raw_exception_var_(raw_exception_var),
        raw_stacktrace_var_(raw_stacktrace_var),
        needs_stacktrace_(needs_stacktrace),
        is_generated_(is_generated) {}

  DECLARE_INSTRUCTION(CatchBlockEntry)

  virtual intptr_t PredecessorCount() const {
    return (predecessor_ == nullptr) ? 0 : 1;
  }
  virtual BlockEntryInstr* PredecessorAt(intptr_t index) const {
    ASSERT((index == 0) && (predecessor_ != nullptr));
    return predecessor_;
  }

  GraphEntryInstr* graph_entry() const { return graph_entry_; }

  const LocalVariable* exception_var() const { return exception_var_; }
  const LocalVariable* stacktrace_var() const { return stacktrace_var_; }

  const LocalVariable* raw_exception_var() const { return raw_exception_var_; }
  const LocalVariable* raw_stacktrace_var() const {
    return raw_stacktrace_var_;
  }

  bool needs_stacktrace() const { return needs_stacktrace_; }

  bool is_generated() const { return is_generated_; }

  // Returns try index for the try block to which this catch handler
  // corresponds.
  intptr_t catch_try_index() const { return catch_try_index_; }

  const Array& catch_handler_types() const { return catch_handler_types_; }

  PRINT_TO_SUPPORT
  DECLARE_CUSTOM_SERIALIZATION(CatchBlockEntryInstr)

 private:
  friend class BlockEntryInstr;  // Access to predecessor_ when inlining.

  virtual void ClearPredecessors() { predecessor_ = nullptr; }
  virtual void AddPredecessor(BlockEntryInstr* predecessor) {
    ASSERT(predecessor_ == nullptr);
    predecessor_ = predecessor;
  }

  GraphEntryInstr* graph_entry_;
  BlockEntryInstr* predecessor_;
  const Array& catch_handler_types_;
  const intptr_t catch_try_index_;
  const LocalVariable* exception_var_;
  const LocalVariable* stacktrace_var_;
  const LocalVariable* raw_exception_var_;
  const LocalVariable* raw_stacktrace_var_;
  const bool needs_stacktrace_;
  bool is_generated_;

  DISALLOW_COPY_AND_ASSIGN(CatchBlockEntryInstr);
};

// If the result of the allocation is not stored into any field, passed
// as an argument or used in a phi then it can't alias with any other
// SSA value.
class AliasIdentity : public ValueObject {
 public:
  // It is unknown if value has aliases.
  static AliasIdentity Unknown() { return AliasIdentity(kUnknown); }

  // It is known that value can have aliases.
  static AliasIdentity Aliased() { return AliasIdentity(kAliased); }

  // It is known that value has no aliases.
  static AliasIdentity NotAliased() { return AliasIdentity(kNotAliased); }

  // It is known that value has no aliases and it was selected by
  // allocation sinking pass as a candidate.
  static AliasIdentity AllocationSinkingCandidate() {
    return AliasIdentity(kAllocationSinkingCandidate);
  }

#define FOR_EACH_ALIAS_IDENTITY_VALUE(V)                                       \
  V(Unknown, 0)                                                                \
  V(NotAliased, 1)                                                             \
  V(Aliased, 2)                                                                \
  V(AllocationSinkingCandidate, 3)

  const char* ToCString() {
    switch (value_) {
#define VALUE_CASE(name, val)                                                  \
  case k##name:                                                                \
    return #name;
      FOR_EACH_ALIAS_IDENTITY_VALUE(VALUE_CASE)
#undef VALUE_CASE
      default:
        UNREACHABLE();
        return nullptr;
    }
  }

  bool IsUnknown() const { return value_ == kUnknown; }
  bool IsAliased() const { return value_ == kAliased; }
  bool IsNotAliased() const { return (value_ & kNotAliased) != 0; }
  bool IsAllocationSinkingCandidate() const {
    return value_ == kAllocationSinkingCandidate;
  }

  AliasIdentity(const AliasIdentity& other)
      : ValueObject(), value_(other.value_) {}

  AliasIdentity& operator=(const AliasIdentity& other) {
    value_ = other.value_;
    return *this;
  }

  void Write(FlowGraphSerializer* s) const;
  explicit AliasIdentity(FlowGraphDeserializer* d);

 private:
  explicit AliasIdentity(intptr_t value) : value_(value) {}

#define VALUE_DEFN(name, val) k##name = val,
  enum { FOR_EACH_ALIAS_IDENTITY_VALUE(VALUE_DEFN) };
#undef VALUE_DEFN

// Undef the FOR_EACH helper macro, since the enum is private.
#undef FOR_EACH_ALIAS_IDENTITY_VALUE

  COMPILE_ASSERT((kUnknown & kNotAliased) == 0);
  COMPILE_ASSERT((kAliased & kNotAliased) == 0);
  COMPILE_ASSERT((kAllocationSinkingCandidate & kNotAliased) != 0);

  intptr_t value_;
};

// Abstract super-class of all instructions that define a value (Bind, Phi).
class Definition : public Instruction {
 public:
  explicit Definition(intptr_t deopt_id = DeoptId::kNone)
      : Instruction(deopt_id) {}

  explicit Definition(const InstructionSource& source,
                      intptr_t deopt_id = DeoptId::kNone)
      : Instruction(source, deopt_id) {}

  // Overridden by definitions that have call counts.
  virtual intptr_t CallCount() const { return -1; }

  intptr_t temp_index() const { return temp_index_; }
  void set_temp_index(intptr_t index) { temp_index_ = index; }
  void ClearTempIndex() { temp_index_ = -1; }
  bool HasTemp() const { return temp_index_ >= 0; }

  intptr_t ssa_temp_index() const { return ssa_temp_index_; }
  void set_ssa_temp_index(intptr_t index) {
    ASSERT(index >= 0);
    ssa_temp_index_ = index;
  }
  bool HasSSATemp() const { return ssa_temp_index_ >= 0; }
  void ClearSSATempIndex() { ssa_temp_index_ = -1; }

  intptr_t vreg(intptr_t index) const {
    ASSERT((index >= 0) && (index < location_count()));
    if (ssa_temp_index_ == -1) return -1;
    return ssa_temp_index_ * kMaxLocationCount + index;
  }
  intptr_t location_count() const { return LocationCount(representation()); }
  bool HasPairRepresentation() const { return location_count() == 2; }

  // Compile time type of the definition, which may be requested before type
  // propagation during graph building.
  CompileType* Type() {
    if (type_ == nullptr) {
      auto type = new CompileType(ComputeType());
      type->set_owner(this);
      set_type(type);
    }
    return type_;
  }

  bool HasType() const { return (type_ != nullptr); }

  inline bool IsInt64Definition();

  bool IsInt32Definition() {
    return IsBinaryInt32Op() || IsBoxInt32() || IsUnboxInt32() ||
           IsIntConverter();
  }

  // Compute compile type for this definition. It is safe to use this
  // approximation even before type propagator was run (e.g. during graph
  // building).
  virtual CompileType ComputeType() const { return CompileType::Dynamic(); }

  // Update CompileType of the definition. Returns true if the type has changed.
  virtual bool RecomputeType() { return false; }

  PRINT_OPERANDS_TO_SUPPORT
  PRINT_TO_SUPPORT

  bool UpdateType(CompileType new_type) {
    if (type_ == nullptr) {
      auto type = new CompileType(new_type);
      type->set_owner(this);
      set_type(type);
      return true;
    }

    if (type_->IsNone() || !type_->IsEqualTo(&new_type)) {
      *type_ = new_type;
      return true;
    }

    return false;
  }

  bool HasUses() const {
    return (input_use_list_ != nullptr) || (env_use_list_ != nullptr);
  }
  bool HasOnlyUse(Value* use) const;
  bool HasOnlyInputUse(Value* use) const;

  Value* input_use_list() const { return input_use_list_; }
  void set_input_use_list(Value* head) { input_use_list_ = head; }

  Value* env_use_list() const { return env_use_list_; }
  void set_env_use_list(Value* head) { env_use_list_ = head; }

  ValueListIterable input_uses() const {
    return ValueListIterable(input_use_list_);
  }

  void AddInputUse(Value* value) { Value::AddToList(value, &input_use_list_); }
  void AddEnvUse(Value* value) { Value::AddToList(value, &env_use_list_); }

  // Replace uses of this definition with uses of other definition or value.
  // Precondition: use lists must be properly calculated.
  // Postcondition: use lists and use values are still valid.
  void ReplaceUsesWith(Definition* other);

  // Replace this definition with another instruction. Use the provided result
  // definition to replace uses of the original definition. If replacing during
  // iteration, pass the iterator so that the instruction can be replaced
  // without affecting iteration order, otherwise pass a nullptr iterator.
  void ReplaceWithResult(Instruction* replacement,
                         Definition* replacement_for_uses,
                         ForwardInstructionIterator* iterator);

  // Replace this definition and all uses with another definition.  If
  // replacing during iteration, pass the iterator so that the instruction
  // can be replaced without affecting iteration order, otherwise pass a
  // nullptr iterator.
  void ReplaceWith(Definition* other, ForwardInstructionIterator* iterator);

  // A value in the constant propagation lattice.
  //    - non-constant sentinel
  //    - a constant (any non-sentinel value)
  //    - unknown sentinel
  Object& constant_value();

  virtual void InferRange(RangeAnalysis* analysis, Range* range);

  Range* range() const { return range_; }
  void set_range(const Range&);

  // Definitions can be canonicalized only into definitions to ensure
  // this check statically we override base Canonicalize with a Canonicalize
  // returning Definition (return type is covariant).
  virtual Definition* Canonicalize(FlowGraph* flow_graph);

  static constexpr intptr_t kReplacementMarker = -2;

  Definition* Replacement() {
    if (ssa_temp_index_ == kReplacementMarker) {
      return reinterpret_cast<Definition*>(temp_index_);
    }
    return this;
  }

  void SetReplacement(Definition* other) {
    ASSERT(ssa_temp_index_ >= 0);
    ASSERT(WasEliminated());
    ssa_temp_index_ = kReplacementMarker;
    temp_index_ = reinterpret_cast<intptr_t>(other);
  }

  virtual AliasIdentity Identity() const { return AliasIdentity::Unknown(); }

  virtual void SetIdentity(AliasIdentity identity) { UNREACHABLE(); }

  // Find the original definition of [this] by following through any
  // redefinition and check instructions.
  Definition* OriginalDefinition();

  // If this definition is a redefinition (in a broad sense, this includes
  // CheckArrayBound and CheckNull instructions) return [Value] corresponding
  // to the input which is being redefined.
  // Otherwise return [nullptr].
  virtual Value* RedefinedValue() const;

  // Find the original definition of [this].
  //
  // This is an extension of [OriginalDefinition] which also follows through any
  // boxing/unboxing and constraint instructions.
  Definition* OriginalDefinitionIgnoreBoxingAndConstraints();

  // Helper method to determine if definition denotes an array length.
  static bool IsArrayLength(Definition* def);

  virtual Definition* AsDefinition() { return this; }
  virtual const Definition* AsDefinition() const { return this; }

  DECLARE_CUSTOM_SERIALIZATION(Definition)

 protected:
  friend class RangeAnalysis;
  friend class Value;

  Range* range_ = nullptr;

  void set_type(CompileType* type) {
    ASSERT(type->owner() == this);
    type_ = type;
  }

#if defined(INCLUDE_IL_PRINTER)
  const char* TypeAsCString() const {
    return HasType() ? type_->ToCString() : "";
  }
#endif

 private:
  intptr_t temp_index_ = -1;
  intptr_t ssa_temp_index_ = -1;
  Value* input_use_list_ = nullptr;
  Value* env_use_list_ = nullptr;

  Object* constant_value_ = nullptr;
  CompileType* type_ = nullptr;

  DISALLOW_COPY_AND_ASSIGN(Definition);
};

// Change a value's definition after use lists have been computed.
inline void Value::BindTo(Definition* def) {
  RemoveFromUseList();
  set_definition(def);
  def->AddInputUse(this);
}

inline void Value::BindToEnvironment(Definition* def) {
  RemoveFromUseList();
  set_definition(def);
  def->AddEnvUse(this);
}

class PureDefinition : public Definition {
 public:
  explicit PureDefinition(intptr_t deopt_id) : Definition(deopt_id) {}
  explicit PureDefinition(const InstructionSource& source, intptr_t deopt_id)
      : Definition(source, deopt_id) {}

  virtual bool AllowsCSE() const { return true; }
  virtual bool HasUnknownSideEffects() const { return false; }

  DECLARE_EMPTY_SERIALIZATION(PureDefinition, Definition)
};

template <intptr_t N,
          typename ThrowsTrait,
          template <typename Impure, typename Pure> class CSETrait = NoCSE>
class TemplateDefinition : public CSETrait<Definition, PureDefinition>::Base {
 public:
  using BaseClass = typename CSETrait<Definition, PureDefinition>::Base;

  explicit TemplateDefinition(intptr_t deopt_id = DeoptId::kNone)
      : BaseClass(deopt_id), inputs_() {}
  TemplateDefinition(const InstructionSource& source,
                     intptr_t deopt_id = DeoptId::kNone)
      : BaseClass(source, deopt_id), inputs_() {}

  virtual intptr_t InputCount() const { return N; }
  virtual Value* InputAt(intptr_t i) const { return inputs_[i]; }

  virtual bool MayThrow() const { return ThrowsTrait::kCanThrow; }

  DECLARE_EMPTY_SERIALIZATION(TemplateDefinition, BaseClass)
 protected:
  EmbeddedArray<Value*, N> inputs_;

 private:
  friend class BranchInstr;
  friend class IfThenElseInstr;

  virtual void RawSetInputAt(intptr_t i, Value* value) { inputs_[i] = value; }
};

class VariadicDefinition : public Definition {
 public:
  explicit VariadicDefinition(InputsArray&& inputs,
                              intptr_t deopt_id = DeoptId::kNone)
      : Definition(deopt_id), inputs_(std::move(inputs)) {
    for (intptr_t i = 0, n = inputs_.length(); i < n; ++i) {
      SetInputAt(i, inputs_[i]);
    }
  }
  VariadicDefinition(InputsArray&& inputs,
                     const InstructionSource& source,
                     intptr_t deopt_id = DeoptId::kNone)
      : Definition(source, deopt_id), inputs_(std::move(inputs)) {
    for (intptr_t i = 0, n = inputs_.length(); i < n; ++i) {
      SetInputAt(i, inputs_[i]);
    }
  }
  explicit VariadicDefinition(const intptr_t num_inputs,
                              intptr_t deopt_id = DeoptId::kNone)
      : Definition(deopt_id), inputs_(num_inputs) {
    inputs_.EnsureLength(num_inputs, nullptr);
  }
  VariadicDefinition(const intptr_t num_inputs,
                     const InstructionSource& source,
                     intptr_t deopt_id = DeoptId::kNone)
      : Definition(source, deopt_id), inputs_(num_inputs) {
    inputs_.EnsureLength(num_inputs, nullptr);
  }

  intptr_t InputCount() const { return inputs_.length(); }
  Value* InputAt(intptr_t i) const { return inputs_[i]; }

  DECLARE_CUSTOM_SERIALIZATION(VariadicDefinition)

 protected:
  InputsArray inputs_;

 private:
  void RawSetInputAt(intptr_t i, Value* value) { inputs_[i] = value; }
};

class PhiInstr : public VariadicDefinition {
 public:
  PhiInstr(JoinEntryInstr* block, intptr_t num_inputs)
      : VariadicDefinition(num_inputs),
        block_(block),
        representation_(kTagged),
        is_alive_(false),
        is_receiver_(kUnknownReceiver) {}

  // Get the block entry for that instruction.
  virtual BlockEntryInstr* GetBlock() { return block(); }
  JoinEntryInstr* block() const { return block_; }

  virtual CompileType ComputeType() const;
  virtual bool RecomputeType();

  virtual bool ComputeCanDeoptimize() const { return false; }

  virtual bool HasUnknownSideEffects() const { return false; }

  // Phi is alive if it reaches a non-environment use.
  bool is_alive() const { return is_alive_; }
  void mark_alive() { is_alive_ = true; }
  void mark_dead() { is_alive_ = false; }

  virtual Representation RequiredInputRepresentation(intptr_t i) const {
    return representation_;
  }

  virtual Representation representation() const { return representation_; }

  virtual void set_representation(Representation r) { representation_ = r; }

  // Only Int32 phis in JIT mode are unboxed optimistically.
  virtual SpeculativeMode SpeculativeModeOfInput(intptr_t index) const {
    return (CompilerState::Current().is_aot() ||
            (representation_ != kUnboxedInt32))
               ? kNotSpeculative
               : kGuardInputs;
  }

  virtual uword Hash() const {
    UNREACHABLE();
    return 0;
  }

  DECLARE_INSTRUCTION(Phi)

  virtual void InferRange(RangeAnalysis* analysis, Range* range);

  BitVector* reaching_defs() const { return reaching_defs_; }

  void set_reaching_defs(BitVector* reaching_defs) {
    reaching_defs_ = reaching_defs;
  }

  virtual bool MayThrow() const { return false; }

  // A phi is redundant if all input operands are the same.
  bool IsRedundant() const;

  // A phi is redundant if all input operands are redefinitions of the same
  // value. Returns the replacement for this phi if it is redundant.
  // The replacement is selected among values redefined by inputs.
  Definition* GetReplacementForRedundantPhi() const;

  virtual Definition* Canonicalize(FlowGraph* flow_graph);

  PRINT_TO_SUPPORT
  DECLARE_CUSTOM_SERIALIZATION(PhiInstr)

  enum ReceiverType { kUnknownReceiver = -1, kNotReceiver = 0, kReceiver = 1 };

  ReceiverType is_receiver() const {
    return static_cast<ReceiverType>(is_receiver_);
  }

  void set_is_receiver(ReceiverType is_receiver) { is_receiver_ = is_receiver; }

 private:
  // Direct access to inputs_ in order to resize it due to unreachable
  // predecessors.
  friend class ConstantPropagator;

  JoinEntryInstr* block_;
  Representation representation_;
  BitVector* reaching_defs_ = nullptr;
  bool is_alive_;
  int8_t is_receiver_;

  DISALLOW_COPY_AND_ASSIGN(PhiInstr);
};

// This instruction represents an incoming parameter for a function entry,
// or incoming value for OSR entry or incoming value for a catch entry.
// [env_index] is a position of the parameter in the flow graph environment.
// [param_index] is a position of the function parameter, or -1 if
// this instruction doesn't correspond to a real function parameter.
class ParameterInstr : public TemplateDefinition<0, NoThrow> {
 public:
  // [param_index] when ParameterInstr doesn't correspond to
  // a function parameter.
  static constexpr intptr_t kNotFunctionParameter = -1;

  ParameterInstr(intptr_t env_index,
                 intptr_t param_index,
                 intptr_t param_offset,
                 BlockEntryInstr* block,
                 Representation representation,
                 Register base_reg = FPREG)
      : env_index_(env_index),
        param_index_(param_index),
        param_offset_(param_offset),
        base_reg_(base_reg),
        representation_(representation),
        block_(block) {}

  DECLARE_INSTRUCTION(Parameter)
  DECLARE_ATTRIBUTES(index())

  // Index of the parameter in the flow graph environment.
  intptr_t env_index() const { return env_index_; }
  intptr_t index() const { return env_index(); }

  // Index of the real function parameter
  // (between 0 and function.NumParameters()), or -1.
  intptr_t param_index() const { return param_index_; }

  intptr_t param_offset() const { return param_offset_; }
  Register base_reg() const { return base_reg_; }

  // Get the block entry for that instruction.
  virtual BlockEntryInstr* GetBlock() { return block_; }
  void set_block(BlockEntryInstr* block) { block_ = block; }

  virtual Representation representation() const { return representation_; }

  virtual Representation RequiredInputRepresentation(intptr_t index) const {
    UNREACHABLE();
    return kTagged;
  }

  virtual bool ComputeCanDeoptimize() const { return false; }

  virtual bool HasUnknownSideEffects() const { return false; }

  virtual uword Hash() const {
    UNREACHABLE();
    return 0;
  }

  virtual CompileType ComputeType() const;

  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F)                                                          \
  F(const intptr_t, env_index_)                                                \
  F(const intptr_t, param_index_)                                              \
  /* The offset (in words) of the last slot of the parameter, relative */      \
  /* to the first parameter. */                                                \
  /* It is used in the FlowGraphAllocator when it sets the assigned */         \
  /* location and spill slot for the parameter definition. */                  \
  F(const intptr_t, param_offset_)                                             \
  F(const Register, base_reg_)                                                 \
  F(const Representation, representation_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(ParameterInstr,
                                          TemplateDefinition,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  BlockEntryInstr* block_ = nullptr;

  DISALLOW_COPY_AND_ASSIGN(ParameterInstr);
};

// Native parameters are not treated as initial definitions because they cannot
// be inlined and are only usable in optimized code. The location must be a
// stack location relative to the position of the stack (SPREG) after
// register-based arguments have been saved on entry to a native call. See
// NativeEntryInstr::EmitNativeCode for more details.
//
// TOOD(33549): Unify with ParameterInstr.
class NativeParameterInstr : public TemplateDefinition<0, NoThrow> {
 public:
  NativeParameterInstr(const compiler::ffi::CallbackMarshaller& marshaller,
                       intptr_t def_index)
      : marshaller_(marshaller), def_index_(def_index) {}

  DECLARE_INSTRUCTION(NativeParameter)

  virtual Representation representation() const {
    return marshaller_.RepInFfiCall(def_index_);
  }

  virtual bool ComputeCanDeoptimize() const { return false; }

  virtual bool HasUnknownSideEffects() const { return false; }

  // TODO(sjindel): We can make this more precise.
  virtual CompileType ComputeType() const { return CompileType::Dynamic(); }

  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F)                                                          \
  F(const compiler::ffi::CallbackMarshaller&, marshaller_)                     \
  F(const intptr_t, def_index_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(NativeParameterInstr,
                                          TemplateDefinition,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(NativeParameterInstr);
};

// Stores a tagged pointer to a slot accessible from a fixed register.  It has
// the form:
//
//     base_reg[index + #constant] = value
//
//   Input 0: A tagged Smi [index]
//   Input 1: A tagged pointer [value]
//   offset:  A signed constant offset which fits into 8 bits
//
// Currently this instruction uses pinpoints the register to be FP.
//
// This low-level instruction is non-inlinable since it makes assumptions about
// the frame.  This is asserted via `inliner.cc::CalleeGraphValidator`.
class StoreIndexedUnsafeInstr : public TemplateInstruction<2, NoThrow> {
 public:
  StoreIndexedUnsafeInstr(Value* index, Value* value, intptr_t offset)
      : offset_(offset) {
    SetInputAt(kIndexPos, index);
    SetInputAt(kValuePos, value);
  }

  enum { kIndexPos = 0, kValuePos = 1 };

  DECLARE_INSTRUCTION(StoreIndexedUnsafe)

  virtual Representation RequiredInputRepresentation(intptr_t index) const {
    ASSERT(index == kIndexPos || index == kValuePos);
    return kTagged;
  }
  virtual bool ComputeCanDeoptimize() const { return false; }
  virtual bool HasUnknownSideEffects() const { return false; }

  virtual bool AttributesEqual(const Instruction& other) const {
    return other.AsStoreIndexedUnsafe()->offset() == offset();
  }

  Value* index() const { return inputs_[kIndexPos]; }
  Value* value() const { return inputs_[kValuePos]; }
  Register base_reg() const { return FPREG; }
  intptr_t offset() const { return offset_; }

  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F) F(const intptr_t, offset_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(StoreIndexedUnsafeInstr,
                                          TemplateInstruction,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(StoreIndexedUnsafeInstr);
};

// Loads a value from slot accessable from a fixed register.  It has
// the form:
//
//     base_reg[index + #constant]
//
//   Input 0: A tagged Smi [index]
//   offset:  A signed constant offset which fits into 8 bits
//
// Currently this instruction uses pinpoints the register to be FP.
//
// This lowlevel instruction is non-inlinable since it makes assumptions about
// the frame.  This is asserted via `inliner.cc::CalleeGraphValidator`.
class LoadIndexedUnsafeInstr : public TemplateDefinition<1, NoThrow> {
 public:
  LoadIndexedUnsafeInstr(Value* index,
                         intptr_t offset,
                         CompileType result_type,
                         Representation representation = kTagged)
      : offset_(offset), representation_(representation) {
    UpdateType(result_type);
    SetInputAt(0, index);
  }

  DECLARE_INSTRUCTION(LoadIndexedUnsafe)

  virtual Representation RequiredInputRepresentation(intptr_t index) const {
    ASSERT(index == 0);
    return kTagged;
  }
  virtual bool ComputeCanDeoptimize() const { return false; }
  virtual bool HasUnknownSideEffects() const { return false; }

  virtual bool AttributesEqual(const Instruction& other) const {
    return other.AsLoadIndexedUnsafe()->offset() == offset();
  }

  virtual Representation representation() const { return representation_; }

  Value* index() const { return InputAt(0); }
  Register base_reg() const { return FPREG; }
  intptr_t offset() const { return offset_; }

  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F)                                                          \
  F(const intptr_t, offset_)                                                   \
  F(const Representation, representation_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(LoadIndexedUnsafeInstr,
                                          TemplateDefinition,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(LoadIndexedUnsafeInstr);
};

class MemoryCopyInstr : public TemplateInstruction<5, NoThrow> {
 public:
  MemoryCopyInstr(Value* src,
                  Value* dest,
                  Value* src_start,
                  Value* dest_start,
                  Value* length,
                  classid_t src_cid,
                  classid_t dest_cid,
                  bool unboxed_inputs,
                  bool can_overlap = true)
      : src_cid_(src_cid),
        dest_cid_(dest_cid),
        element_size_(Instance::ElementSizeFor(src_cid)),
        unboxed_inputs_(unboxed_inputs),
        can_overlap_(can_overlap) {
    ASSERT(IsArrayTypeSupported(src_cid));
    ASSERT(IsArrayTypeSupported(dest_cid));
    ASSERT(Instance::ElementSizeFor(src_cid) ==
           Instance::ElementSizeFor(dest_cid));
    SetInputAt(kSrcPos, src);
    SetInputAt(kDestPos, dest);
    SetInputAt(kSrcStartPos, src_start);
    SetInputAt(kDestStartPos, dest_start);
    SetInputAt(kLengthPos, length);
  }

  enum {
    kSrcPos = 0,
    kDestPos = 1,
    kSrcStartPos = 2,
    kDestStartPos = 3,
    kLengthPos = 4
  };

  DECLARE_INSTRUCTION(MemoryCopy)

  virtual Representation RequiredInputRepresentation(intptr_t index) const {
    if (index == kSrcPos || index == kDestPos) {
      // The object inputs are always tagged.
      return kTagged;
    }
    return unboxed_inputs() ? kUnboxedIntPtr : kTagged;
  }

  virtual bool ComputeCanDeoptimize() const { return false; }
  virtual bool HasUnknownSideEffects() const { return true; }

  virtual bool AttributesEqual(const Instruction& other) const { return true; }

  Value* src() const { return inputs_[kSrcPos]; }
  Value* dest() const { return inputs_[kDestPos]; }
  Value* src_start() const { return inputs_[kSrcStartPos]; }
  Value* dest_start() const { return inputs_[kDestStartPos]; }
  Value* length() const { return inputs_[kLengthPos]; }

  intptr_t element_size() const { return element_size_; }
  bool unboxed_inputs() const { return unboxed_inputs_; }
  bool can_overlap() const { return can_overlap_; }

  // Optimizes MemoryCopyInstr with constant parameters to use larger moves.
  virtual Instruction* Canonicalize(FlowGraph* flow_graph);

  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F)                                                          \
  F(classid_t, src_cid_)                                                       \
  F(classid_t, dest_cid_)                                                      \
  F(intptr_t, element_size_)                                                   \
  F(bool, unboxed_inputs_)                                                     \
  F(bool, can_overlap_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(MemoryCopyInstr,
                                          TemplateInstruction,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  // Set array_reg to point to the index indicated by start (contained in
  // start_loc) of the typed data or string in array (contained in array_reg).
  void EmitComputeStartPointer(FlowGraphCompiler* compiler,
                               classid_t array_cid,
                               Register array_reg,
                               Location start_loc);

  // Generates an unrolled loop for copying a known amount of data from
  // src to dest.
  void EmitUnrolledCopy(FlowGraphCompiler* compiler,
                        Register dest_reg,
                        Register src_reg,
                        intptr_t num_elements,
                        bool reversed);

  // Called prior to EmitLoopCopy() to adjust the length register as needed
  // for the code emitted by EmitLoopCopy. May jump to done if the emitted
  // loop(s) should be skipped.
  void PrepareLengthRegForLoop(FlowGraphCompiler* compiler,
                               Register length_reg,
                               compiler::Label* done);

  // Generates a loop for copying the data from src to dest, for cases where
  // either the length is not known at compile time or too large to unroll.
  //
  // copy_forwards is only provided (not nullptr) when a backwards loop is
  // requested. May jump to copy_forwards if backwards iteration is slower than
  // forwards iteration and the emitted code verifies no actual overlap exists.
  //
  // May jump to done if no copying is needed.
  //
  // Assumes that PrepareLengthRegForLoop() has been called beforehand.
  void EmitLoopCopy(FlowGraphCompiler* compiler,
                    Register dest_reg,
                    Register src_reg,
                    Register length_reg,
                    compiler::Label* done,
                    compiler::Label* copy_forwards = nullptr);

  static bool IsArrayTypeSupported(classid_t array_cid) {
    if (IsTypedDataBaseClassId(array_cid)) {
      return true;
    }
    switch (array_cid) {
      case kOneByteStringCid:
      case kTwoByteStringCid:
      case kExternalOneByteStringCid:
      case kExternalTwoByteStringCid:
        return true;
      default:
        return false;
    }
  }

  DISALLOW_COPY_AND_ASSIGN(MemoryCopyInstr);
};

// Unwinds the current frame and tail calls a target.
//
// The return address saved by the original caller of this frame will be in it's
// usual location (stack or LR).  The arguments descriptor supplied by the
// original caller will be put into ARGS_DESC_REG.
//
// This lowlevel instruction is non-inlinable since it makes assumptions about
// the frame.  This is asserted via `inliner.cc::CalleeGraphValidator`.
class TailCallInstr : public TemplateInstruction<1, Throws, Pure> {
 public:
  TailCallInstr(const Code& code, Value* arg_desc) : code_(code) {
    SetInputAt(0, arg_desc);
  }

  DECLARE_INSTRUCTION(TailCall)

  const Code& code() const { return code_; }

  // Two tailcalls can be canonicalized into one instruction if both have the
  // same destination.
  virtual bool AttributesEqual(const Instruction& other) const {
    return &other.AsTailCall()->code() == &code();
  }

  // Since no code after this instruction will be executed, there will be no
  // side-effects for the following code.
  virtual bool HasUnknownSideEffects() const { return false; }
  virtual bool ComputeCanDeoptimize() const { return false; }

  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F) F(const Code&, code_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(TailCallInstr,
                                          TemplateInstruction,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(TailCallInstr);
};

// Move the given argument value into the place where callee expects it.
// Currently all outgoing arguments are located in [SP+idx]
class MoveArgumentInstr : public TemplateDefinition<1, NoThrow> {
 public:
  explicit MoveArgumentInstr(Value* value,
                             Representation representation,
                             intptr_t sp_relative_index)
      : representation_(representation), sp_relative_index_(sp_relative_index) {
    SetInputAt(0, value);
  }

  DECLARE_INSTRUCTION(MoveArgument)

  intptr_t sp_relative_index() const { return sp_relative_index_; }

  virtual CompileType ComputeType() const;

  Value* value() const { return InputAt(0); }

  virtual bool ComputeCanDeoptimize() const { return false; }

  virtual bool HasUnknownSideEffects() const { return false; }

  virtual TokenPosition token_pos() const {
    return TokenPosition::kMoveArgument;
  }

  virtual Representation representation() const { return representation_; }

  virtual Representation RequiredInputRepresentation(intptr_t index) const {
    ASSERT(index == 0);
    return representation();
  }

  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F)                                                          \
  F(const Representation, representation_)                                     \
  F(const intptr_t, sp_relative_index_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(MoveArgumentInstr,
                                          TemplateDefinition,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(MoveArgumentInstr);
};

inline Value* Instruction::ArgumentValueAt(intptr_t index) const {
  MoveArgumentsArray* move_arguments = GetMoveArguments();
  return move_arguments != nullptr ? (*move_arguments)[index]->value()
                                   : InputAt(index);
}

inline Definition* Instruction::ArgumentAt(intptr_t index) const {
  return ArgumentValueAt(index)->definition();
}

class ReturnInstr : public TemplateInstruction<1, NoThrow> {
 public:
  ReturnInstr(const InstructionSource& source,
              Value* value,
              intptr_t deopt_id,
              Representation representation = kTagged)
      : TemplateInstruction(source, deopt_id),
        token_pos_(source.token_pos),
        representation_(representation) {
    SetInputAt(0, value);
  }

  DECLARE_INSTRUCTION(Return)

  virtual TokenPosition token_pos() const { return token_pos_; }
  Value* value() const { return inputs_[0]; }

  virtual bool CanBecomeDeoptimizationTarget() const {
    // Return instruction might turn into a Goto instruction after inlining.
    // Every Goto must have an environment.
    return true;
  }

  virtual bool ComputeCanDeoptimize() const { return false; }

  virtual bool HasUnknownSideEffects() const { return false; }

  virtual bool AttributesEqual(const Instruction& other) const {
    auto const other_return = other.AsReturn();
    return token_pos() == other_return->token_pos();
  }

  virtual SpeculativeMode SpeculativeModeOfInput(intptr_t index) const {
    ASSERT(index == 0);
    return kNotSpeculative;
  }

  virtual intptr_t DeoptimizationTarget() const { return DeoptId::kNone; }

  virtual Representation representation() const { return representation_; }

  virtual Representation RequiredInputRepresentation(intptr_t index) const {
    ASSERT(index == 0);
    return representation_;
  }

#define FIELD_LIST(F)                                                          \
  F(const TokenPosition, token_pos_)                                           \
  F(const Representation, representation_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(ReturnInstr,
                                          TemplateInstruction,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  const Code& GetReturnStub(FlowGraphCompiler* compiler) const;

  DISALLOW_COPY_AND_ASSIGN(ReturnInstr);
};

// Represents a return from a Dart function into native code.
class NativeReturnInstr : public ReturnInstr {
 public:
  NativeReturnInstr(const InstructionSource& source,
                    Value* value,
                    const compiler::ffi::CallbackMarshaller& marshaller,
                    intptr_t deopt_id)
      : ReturnInstr(source, value, deopt_id), marshaller_(marshaller) {}

  DECLARE_INSTRUCTION(NativeReturn)

  PRINT_OPERANDS_TO_SUPPORT

  virtual Representation RequiredInputRepresentation(intptr_t idx) const {
    ASSERT(idx == 0);
    return marshaller_.RepInFfiCall(compiler::ffi::kResultIndex);
  }

  virtual bool CanBecomeDeoptimizationTarget() const {
    // Unlike ReturnInstr, NativeReturnInstr cannot be inlined (because it's
    // returning into native code).
    return false;
  }

#define FIELD_LIST(F) F(const compiler::ffi::CallbackMarshaller&, marshaller_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(NativeReturnInstr,
                                          ReturnInstr,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  void EmitReturnMoves(FlowGraphCompiler* compiler);

  DISALLOW_COPY_AND_ASSIGN(NativeReturnInstr);
};

class ThrowInstr : public TemplateInstruction<1, Throws> {
 public:
  explicit ThrowInstr(const InstructionSource& source,
                      intptr_t deopt_id,
                      Value* exception)
      : TemplateInstruction(source, deopt_id), token_pos_(source.token_pos) {
    SetInputAt(0, exception);
  }

  DECLARE_INSTRUCTION(Throw)

  virtual TokenPosition token_pos() const { return token_pos_; }
  Value* exception() const { return inputs_[0]; }

  virtual bool ComputeCanDeoptimize() const { return false; }
  virtual bool ComputeCanDeoptimizeAfterCall() const {
    return !CompilerState::Current().is_aot();
  }

  virtual bool HasUnknownSideEffects() const { return false; }

#define FIELD_LIST(F) F(const TokenPosition, token_pos_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(ThrowInstr,
                                          TemplateInstruction,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(ThrowInstr);
};

class ReThrowInstr : public TemplateInstruction<2, Throws> {
 public:
  // 'catch_try_index' can be kInvalidTryIndex if the
  // rethrow has been artificially generated by the parser.
  ReThrowInstr(const InstructionSource& source,
               intptr_t catch_try_index,
               intptr_t deopt_id,
               Value* exception,
               Value* stacktrace)
      : TemplateInstruction(source, deopt_id),
        token_pos_(source.token_pos),
        catch_try_index_(catch_try_index) {
    SetInputAt(0, exception);
    SetInputAt(1, stacktrace);
  }

  DECLARE_INSTRUCTION(ReThrow)

  virtual TokenPosition token_pos() const { return token_pos_; }
  intptr_t catch_try_index() const { return catch_try_index_; }
  Value* exception() const { return inputs_[0]; }
  Value* stacktrace() const { return inputs_[1]; }

  virtual bool ComputeCanDeoptimize() const { return false; }
  virtual bool ComputeCanDeoptimizeAfterCall() const {
    return !CompilerState::Current().is_aot();
  }

  virtual bool HasUnknownSideEffects() const { return false; }

#define FIELD_LIST(F)                                                          \
  F(const TokenPosition, token_pos_)                                           \
  F(const intptr_t, catch_try_index_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(ReThrowInstr,
                                          TemplateInstruction,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(ReThrowInstr);
};

class StopInstr : public TemplateInstruction<0, NoThrow> {
 public:
  explicit StopInstr(const char* message) : message_(message) {
    ASSERT(message != nullptr);
  }

  const char* message() const { return message_; }

  DECLARE_INSTRUCTION(Stop);

  virtual bool ComputeCanDeoptimize() const { return false; }

  virtual bool HasUnknownSideEffects() const { return false; }

#define FIELD_LIST(F) F(const char*, message_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(StopInstr,
                                          TemplateInstruction,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(StopInstr);
};

class GotoInstr : public TemplateInstruction<0, NoThrow> {
 public:
  explicit GotoInstr(JoinEntryInstr* entry, intptr_t deopt_id)
      : TemplateInstruction(deopt_id),
        edge_weight_(0.0),
        parallel_move_(nullptr),
        successor_(entry) {}

  DECLARE_INSTRUCTION(Goto)

  BlockEntryInstr* block() const { return block_; }
  void set_block(BlockEntryInstr* block) { block_ = block; }

  JoinEntryInstr* successor() const { return successor_; }
  void set_successor(JoinEntryInstr* successor) { successor_ = successor; }
  virtual intptr_t SuccessorCount() const;
  virtual BlockEntryInstr* SuccessorAt(intptr_t index) const;

  double edge_weight() const { return edge_weight_; }
  void set_edge_weight(double weight) { edge_weight_ = weight; }
  void adjust_edge_weight(double scale_factor) { edge_weight_ *= scale_factor; }

  virtual bool CanBecomeDeoptimizationTarget() const {
    // Goto instruction can be used as a deoptimization target when LICM
    // hoists instructions out of the loop.
    return true;
  }

  // May require a deoptimization target for int32 Phi input conversions.
  virtual intptr_t DeoptimizationTarget() const { return GetDeoptId(); }

  virtual bool ComputeCanDeoptimize() const { return false; }

  virtual bool HasUnknownSideEffects() const { return false; }

  ParallelMoveInstr* parallel_move() const { return parallel_move_; }

  bool HasParallelMove() const { return parallel_move_ != nullptr; }

  bool HasNonRedundantParallelMove() const {
    return HasParallelMove() && !parallel_move()->IsRedundant();
  }

  ParallelMoveInstr* GetParallelMove() {
    if (parallel_move_ == nullptr) {
      parallel_move_ = new ParallelMoveInstr();
    }
    return parallel_move_;
  }

  virtual TokenPosition token_pos() const {
    return TokenPosition::kControlFlow;
  }

  PRINT_TO_SUPPORT

#define FIELD_LIST(F)                                                          \
  F(double, edge_weight_)                                                      \
  /* Parallel move that will be used by linear scan register allocator to */   \
  /* connect live ranges at the end of the block and resolve phis. */          \
  F(ParallelMoveInstr*, parallel_move_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(GotoInstr,
                                          TemplateInstruction,
                                          FIELD_LIST)
#undef FIELD_LIST
  DECLARE_EXTRA_SERIALIZATION

 private:
  BlockEntryInstr* block_ = nullptr;
  JoinEntryInstr* successor_ = nullptr;

  DISALLOW_COPY_AND_ASSIGN(GotoInstr);
};

// IndirectGotoInstr represents a dynamically computed jump. Only
// IndirectEntryInstr targets are valid targets of an indirect goto. The
// concrete target index to jump to is given as a parameter to the indirect
// goto.
//
// In order to preserve split-edge form, an indirect goto does not itself point
// to its targets. Instead, for each possible target, the successors_ field
// will contain an ordinary goto instruction that jumps to the target.
// TODO(zerny): Implement direct support instead of embedding gotos.
//
// The input to the [IndirectGotoInstr] is the target index to jump to.
// All targets of the [IndirectGotoInstr] are added via [AddSuccessor] and get
// increasing indices.
//
// The FlowGraphCompiler will - as a post-processing step - invoke
// [ComputeOffsetTable] of all [IndirectGotoInstr]s. In there we initialize a
// TypedDataInt32Array containing offsets of all [IndirectEntryInstr]s (the
// offsets are relative to start of the instruction payload).
//
//  => See `FlowGraphCompiler::CompileGraph()`
//  => See `IndirectGotoInstr::ComputeOffsetTable`
class IndirectGotoInstr : public TemplateInstruction<1, NoThrow> {
 public:
  IndirectGotoInstr(intptr_t target_count, Value* target_index)
      : offsets_(TypedData::ZoneHandle(TypedData::New(kTypedDataInt32ArrayCid,
                                                      target_count,
                                                      Heap::kOld))) {
    SetInputAt(0, target_index);
  }

  DECLARE_INSTRUCTION(IndirectGoto)

  virtual Representation RequiredInputRepresentation(intptr_t idx) const {
    ASSERT(idx == 0);
    return kTagged;
  }

  void AddSuccessor(TargetEntryInstr* successor) {
    ASSERT(successor->next()->IsGoto());
    ASSERT(successor->next()->AsGoto()->successor()->IsIndirectEntry());
    successors_.Add(successor);
  }

  virtual intptr_t SuccessorCount() const { return successors_.length(); }
  virtual TargetEntryInstr* SuccessorAt(intptr_t index) const {
    ASSERT(index < SuccessorCount());
    return successors_[index];
  }

  virtual bool ComputeCanDeoptimize() const { return false; }
  virtual bool CanBecomeDeoptimizationTarget() const { return false; }

  virtual bool HasUnknownSideEffects() const { return false; }

  Value* offset() const { return inputs_[0]; }
  void ComputeOffsetTable(FlowGraphCompiler* compiler);

  PRINT_TO_SUPPORT

  DECLARE_CUSTOM_SERIALIZATION(IndirectGotoInstr)
  DECLARE_EXTRA_SERIALIZATION

 private:
  GrowableArray<TargetEntryInstr*> successors_;
  const TypedData& offsets_;

  DISALLOW_COPY_AND_ASSIGN(IndirectGotoInstr);
};

class ComparisonInstr : public Definition {
 public:
  Value* left() const { return InputAt(0); }
  Value* right() const { return InputAt(1); }

  virtual TokenPosition token_pos() const { return token_pos_; }
  Token::Kind kind() const { return kind_; }
  DECLARE_ATTRIBUTES(kind())

  virtual ComparisonInstr* CopyWithNewOperands(Value* left, Value* right) = 0;

  // Emits instructions to do the comparison and branch to the true or false
  // label depending on the result.  This implementation will call
  // EmitComparisonCode and then generate the branch instructions afterwards.
  virtual void EmitBranchCode(FlowGraphCompiler* compiler, BranchInstr* branch);

  // Used by EmitBranchCode and EmitNativeCode depending on whether the boolean
  // is to be turned into branches or instantiated.  May return a valid
  // condition in which case the caller is expected to emit a branch to the
  // true label based on that condition (or a branch to the false label on the
  // opposite condition).  May also branch directly to the labels.
  virtual Condition EmitComparisonCode(FlowGraphCompiler* compiler,
                                       BranchLabels labels) = 0;

  // Emits code that generates 'true' or 'false', depending on the comparison.
  // This implementation will call EmitComparisonCode.  If EmitComparisonCode
  // does not use the labels (merely returning a condition) then EmitNativeCode
  // may be able to use the condition to avoid a branch.
  virtual void EmitNativeCode(FlowGraphCompiler* compiler);

  void SetDeoptId(const Instruction& instr) { CopyDeoptIdFrom(instr); }

  // Operation class id is computed from collected ICData.
  void set_operation_cid(intptr_t value) { operation_cid_ = value; }
  intptr_t operation_cid() const { return operation_cid_; }

  virtual void NegateComparison() { kind_ = Token::NegateComparison(kind_); }

  virtual bool CanBecomeDeoptimizationTarget() const { return true; }
  virtual intptr_t DeoptimizationTarget() const { return GetDeoptId(); }

  virtual bool AttributesEqual(const Instruction& other) const {
    auto const other_comparison = other.AsComparison();
    return kind() == other_comparison->kind() &&
           (operation_cid() == other_comparison->operation_cid());
  }

  // Detects comparison with a constant and returns constant and the other
  // operand.
  bool IsComparisonWithConstant(Value** other_operand,
                                ConstantInstr** constant_operand) {
    if (right()->BindsToConstant(constant_operand)) {
      *other_operand = left();
      return true;
    } else if (left()->BindsToConstant(constant_operand)) {
      *other_operand = right();
      return true;
    } else {
      return false;
    }
  }

  DECLARE_ABSTRACT_INSTRUCTION(Comparison)

#define FIELD_LIST(F)                                                          \
  F(const TokenPosition, token_pos_)                                           \
  F(Token::Kind, kind_)                                                        \
  /* Set by optimizer. */                                                      \
  F(intptr_t, operation_cid_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(ComparisonInstr,
                                          Definition,
                                          FIELD_LIST)
#undef FIELD_LIST

 protected:
  ComparisonInstr(const InstructionSource& source,
                  Token::Kind kind,
                  intptr_t deopt_id = DeoptId::kNone)
      : Definition(source, deopt_id),
        token_pos_(source.token_pos),
        kind_(kind),
        operation_cid_(kIllegalCid) {}

 private:
  DISALLOW_COPY_AND_ASSIGN(ComparisonInstr);
};

class PureComparison : public ComparisonInstr {
 public:
  virtual bool AllowsCSE() const { return true; }
  virtual bool HasUnknownSideEffects() const { return false; }

  DECLARE_EMPTY_SERIALIZATION(PureComparison, ComparisonInstr)
 protected:
  PureComparison(const InstructionSource& source,
                 Token::Kind kind,
                 intptr_t deopt_id)
      : ComparisonInstr(source, kind, deopt_id) {}
};

template <intptr_t N,
          typename ThrowsTrait,
          template <typename Impure, typename Pure> class CSETrait = NoCSE>
class TemplateComparison
    : public CSETrait<ComparisonInstr, PureComparison>::Base {
 public:
  using BaseClass = typename CSETrait<ComparisonInstr, PureComparison>::Base;

  TemplateComparison(const InstructionSource& source,
                     Token::Kind kind,
                     intptr_t deopt_id = DeoptId::kNone)
      : BaseClass(source, kind, deopt_id), inputs_() {}

  virtual intptr_t InputCount() const { return N; }
  virtual Value* InputAt(intptr_t i) const { return inputs_[i]; }

  virtual bool MayThrow() const { return ThrowsTrait::kCanThrow; }

  DECLARE_EMPTY_SERIALIZATION(TemplateComparison, BaseClass)

 protected:
  EmbeddedArray<Value*, N> inputs_;

 private:
  virtual void RawSetInputAt(intptr_t i, Value* value) { inputs_[i] = value; }
};

class BranchInstr : public Instruction {
 public:
  explicit BranchInstr(ComparisonInstr* comparison, intptr_t deopt_id)
      : Instruction(deopt_id), comparison_(comparison) {
    ASSERT(comparison->env() == nullptr);
    for (intptr_t i = comparison->InputCount() - 1; i >= 0; --i) {
      comparison->InputAt(i)->set_instruction(this);
    }
  }

  DECLARE_INSTRUCTION(Branch)

  virtual intptr_t ArgumentCount() const {
    return comparison()->ArgumentCount();
  }
  virtual void SetMoveArguments(MoveArgumentsArray* move_arguments) {
    comparison()->SetMoveArguments(move_arguments);
  }
  virtual MoveArgumentsArray* GetMoveArguments() const {
    return comparison()->GetMoveArguments();
  }

  intptr_t InputCount() const { return comparison()->InputCount(); }

  Value* InputAt(intptr_t i) const { return comparison()->InputAt(i); }

  virtual TokenPosition token_pos() const { return comparison_->token_pos(); }
  virtual intptr_t inlining_id() const { return comparison_->inlining_id(); }
  virtual void set_inlining_id(intptr_t value) {
    return comparison_->set_inlining_id(value);
  }
  virtual bool has_inlining_id() const {
    return comparison_->has_inlining_id();
  }

  virtual bool ComputeCanDeoptimize() const {
    return comparison()->ComputeCanDeoptimize();
  }

  virtual bool CanBecomeDeoptimizationTarget() const {
    return comparison()->CanBecomeDeoptimizationTarget();
  }

  virtual bool HasUnknownSideEffects() const {
    return comparison()->HasUnknownSideEffects();
  }

  virtual bool CanCallDart() const { return comparison()->CanCallDart(); }

  ComparisonInstr* comparison() const { return comparison_; }
  void SetComparison(ComparisonInstr* comp);

  virtual intptr_t DeoptimizationTarget() const {
    return comparison()->DeoptimizationTarget();
  }

  virtual Representation RequiredInputRepresentation(intptr_t i) const {
    return comparison()->RequiredInputRepresentation(i);
  }

  virtual Instruction* Canonicalize(FlowGraph* flow_graph);

  void set_constant_target(TargetEntryInstr* target) {
    ASSERT(target == true_successor() || target == false_successor());
    constant_target_ = target;
  }
  TargetEntryInstr* constant_target() const { return constant_target_; }

  virtual void CopyDeoptIdFrom(const Instruction& instr) {
    Instruction::CopyDeoptIdFrom(instr);
    comparison()->CopyDeoptIdFrom(instr);
  }

  virtual bool MayThrow() const { return comparison()->MayThrow(); }

  TargetEntryInstr* true_successor() const { return true_successor_; }
  TargetEntryInstr* false_successor() const { return false_successor_; }

  TargetEntryInstr** true_successor_address() { return &true_successor_; }
  TargetEntryInstr** false_successor_address() { return &false_successor_; }

  virtual intptr_t SuccessorCount() const;
  virtual BlockEntryInstr* SuccessorAt(intptr_t index) const;

  PRINT_TO_SUPPORT

#define FIELD_LIST(F) F(ComparisonInstr*, comparison_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(BranchInstr, Instruction, FIELD_LIST)
#undef FIELD_LIST
  DECLARE_EXTRA_SERIALIZATION

 private:
  virtual void RawSetInputAt(intptr_t i, Value* value) {
    comparison()->RawSetInputAt(i, value);
  }

  TargetEntryInstr* true_successor_ = nullptr;
  TargetEntryInstr* false_successor_ = nullptr;
  TargetEntryInstr* constant_target_ = nullptr;

  DISALLOW_COPY_AND_ASSIGN(BranchInstr);
};

class DeoptimizeInstr : public TemplateInstruction<0, NoThrow, Pure> {
 public:
  DeoptimizeInstr(ICData::DeoptReasonId deopt_reason, intptr_t deopt_id)
      : TemplateInstruction(deopt_id), deopt_reason_(deopt_reason) {}

  virtual bool ComputeCanDeoptimize() const { return true; }

  virtual bool AttributesEqual(const Instruction& other) const { return true; }

  DECLARE_INSTRUCTION(Deoptimize)

#define FIELD_LIST(F) F(const ICData::DeoptReasonId, deopt_reason_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(DeoptimizeInstr,
                                          TemplateInstruction,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(DeoptimizeInstr);
};

class RedefinitionInstr : public TemplateDefinition<1, NoThrow> {
 public:
  explicit RedefinitionInstr(Value* value,
                             bool inserted_by_constant_propagation = false)
      : constrained_type_(nullptr),
        inserted_by_constant_propagation_(inserted_by_constant_propagation) {
    SetInputAt(0, value);
  }

  DECLARE_INSTRUCTION(Redefinition)

  Value* value() const { return inputs_[0]; }

  virtual CompileType ComputeType() const;
  virtual bool RecomputeType();

  virtual Definition* Canonicalize(FlowGraph* flow_graph);

  void set_constrained_type(CompileType* type) { constrained_type_ = type; }
  CompileType* constrained_type() const { return constrained_type_; }

  bool inserted_by_constant_propagation() const {
    return inserted_by_constant_propagation_;
  }

  virtual bool ComputeCanDeoptimize() const { return false; }
  virtual bool HasUnknownSideEffects() const { return false; }

  virtual Value* RedefinedValue() const;

  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F)                                                          \
  F(CompileType*, constrained_type_)                                           \
  F(bool, inserted_by_constant_propagation_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(RedefinitionInstr,
                                          TemplateDefinition,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(RedefinitionInstr);
};

// Keeps the value alive til after this point.
//
// The fence cannot be moved.
class ReachabilityFenceInstr : public TemplateInstruction<1, NoThrow> {
 public:
  explicit ReachabilityFenceInstr(Value* value) { SetInputAt(0, value); }

  DECLARE_INSTRUCTION(ReachabilityFence)

  virtual Representation RequiredInputRepresentation(intptr_t idx) const {
    return kNoRepresentation;
  }

  Value* value() const { return inputs_[0]; }

  virtual bool ComputeCanDeoptimize() const { return false; }
  virtual bool HasUnknownSideEffects() const { return false; }

  PRINT_OPERANDS_TO_SUPPORT

  DECLARE_EMPTY_SERIALIZATION(ReachabilityFenceInstr, TemplateInstruction)

 private:
  DISALLOW_COPY_AND_ASSIGN(ReachabilityFenceInstr);
};

class ConstraintInstr : public TemplateDefinition<1, NoThrow> {
 public:
  ConstraintInstr(Value* value, Range* constraint) : constraint_(constraint) {
    SetInputAt(0, value);
  }

  DECLARE_INSTRUCTION(Constraint)

  virtual CompileType ComputeType() const;

  virtual bool ComputeCanDeoptimize() const { return false; }

  virtual bool HasUnknownSideEffects() const { return false; }

  virtual bool AttributesEqual(const Instruction& other) const {
    UNREACHABLE();
    return false;
  }

  Value* value() const { return inputs_[0]; }
  Range* constraint() const { return constraint_; }

  virtual void InferRange(RangeAnalysis* analysis, Range* range);

  // Constraints for branches have their target block stored in order
  // to find the comparison that generated the constraint:
  // target->predecessor->last_instruction->comparison.
  void set_target(TargetEntryInstr* target) { target_ = target; }
  TargetEntryInstr* target() const { return target_; }

  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F) F(Range*, constraint_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(ConstraintInstr,
                                          TemplateDefinition,
                                          FIELD_LIST)
#undef FIELD_LIST
  DECLARE_EXTRA_SERIALIZATION

 private:
  TargetEntryInstr* target_ = nullptr;

  DISALLOW_COPY_AND_ASSIGN(ConstraintInstr);
};

class ConstantInstr : public TemplateDefinition<0, NoThrow, Pure> {
 public:
  explicit ConstantInstr(const Object& value)
      : ConstantInstr(value, InstructionSource(TokenPosition::kConstant)) {}
  ConstantInstr(const Object& value, const InstructionSource& source);

  DECLARE_INSTRUCTION(Constant)
  virtual CompileType ComputeType() const;

  virtual Definition* Canonicalize(FlowGraph* flow_graph);

  const Object& value() const { return value_; }

  bool IsSmi() const { return compiler::target::IsSmi(value()); }

  bool HasZeroRepresentation() const {
    switch (representation()) {
      case kTagged:
      case kUnboxedUint8:
      case kUnboxedUint16:
      case kUnboxedUint32:
      case kUnboxedInt32:
      case kUnboxedInt64:
        return IsSmi() && compiler::target::SmiValue(value()) == 0;
      case kUnboxedDouble:
        return compiler::target::IsDouble(value()) &&
               bit_cast<uint64_t>(compiler::target::DoubleValue(value())) == 0;
      default:
        return false;
    }
  }

  virtual bool ComputeCanDeoptimize() const { return false; }

  virtual void InferRange(RangeAnalysis* analysis, Range* range);

  virtual bool AttributesEqual(const Instruction& other) const;

  virtual TokenPosition token_pos() const { return token_pos_; }

  void EmitMoveToLocation(FlowGraphCompiler* compiler,
                          const Location& destination,
                          Register tmp = kNoRegister,
                          intptr_t pair_index = 0);

  PRINT_OPERANDS_TO_SUPPORT
  DECLARE_ATTRIBUTES_NAMED(("value"), (&value()))

#define FIELD_LIST(F)                                                          \
  F(const Object&, value_)                                                     \
  F(const TokenPosition, token_pos_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(ConstantInstr,
                                          TemplateDefinition,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(ConstantInstr);
};

// Merged ConstantInstr -> UnboxedXXX into UnboxedConstantInstr.
// TODO(srdjan): Implemented currently for doubles only, should implement
// for other unboxing instructions.
class UnboxedConstantInstr : public ConstantInstr {
 public:
  explicit UnboxedConstantInstr(const Object& value,
                                Representation representation);

  virtual Representation representation() const { return representation_; }

  // Either nullptr or the address of the unboxed constant.
  uword constant_address() const { return constant_address_; }

  DECLARE_INSTRUCTION(UnboxedConstant)
  DECLARE_CUSTOM_SERIALIZATION(UnboxedConstantInstr)

 private:
  const Representation representation_;
  uword
      constant_address_;  // Either nullptr or points to the untagged constant.

  DISALLOW_COPY_AND_ASSIGN(UnboxedConstantInstr);
};

// Checks that one type is a subtype of another (e.g. for type parameter bounds
// checking). Throws a TypeError otherwise. Both types are instantiated at
// runtime as necessary.
class AssertSubtypeInstr : public TemplateInstruction<5, Throws, Pure> {
 public:
  enum {
    kInstantiatorTAVPos = 0,
    kFunctionTAVPos = 1,
    kSubTypePos = 2,
    kSuperTypePos = 3,
    kDstNamePos = 4,
  };

  AssertSubtypeInstr(const InstructionSource& source,
                     Value* instantiator_type_arguments,
                     Value* function_type_arguments,
                     Value* sub_type,
                     Value* super_type,
                     Value* dst_name,
                     intptr_t deopt_id)
      : TemplateInstruction(source, deopt_id), token_pos_(source.token_pos) {
    SetInputAt(kInstantiatorTAVPos, instantiator_type_arguments);
    SetInputAt(kFunctionTAVPos, function_type_arguments);
    SetInputAt(kSubTypePos, sub_type);
    SetInputAt(kSuperTypePos, super_type);
    SetInputAt(kDstNamePos, dst_name);
  }

  DECLARE_INSTRUCTION(AssertSubtype);

  Value* instantiator_type_arguments() const {
    return inputs_[kInstantiatorTAVPos];
  }
  Value* function_type_arguments() const { return inputs_[kFunctionTAVPos]; }
  Value* sub_type() const { return inputs_[kSubTypePos]; }
  Value* super_type() const { return inputs_[kSuperTypePos]; }
  Value* dst_name() const { return inputs_[kDstNamePos]; }

  virtual TokenPosition token_pos() const { return token_pos_; }

  virtual bool ComputeCanDeoptimize() const { return false; }
  virtual bool ComputeCanDeoptimizeAfterCall() const {
    return !CompilerState::Current().is_aot();
  }
  virtual intptr_t NumberOfInputsConsumedBeforeCall() const {
    return InputCount();
  }

  virtual bool CanBecomeDeoptimizationTarget() const { return true; }

  virtual Instruction* Canonicalize(FlowGraph* flow_graph);

  virtual bool AttributesEqual(const Instruction& other) const { return true; }

  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F) F(const TokenPosition, token_pos_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(AssertSubtypeInstr,
                                          TemplateInstruction,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(AssertSubtypeInstr);
};

class AssertAssignableInstr : public TemplateDefinition<4, Throws, Pure> {
 public:
#define FOR_EACH_ASSERT_ASSIGNABLE_KIND(V)                                     \
  V(ParameterCheck)                                                            \
  V(InsertedByFrontend)                                                        \
  V(FromSource)                                                                \
  V(Unknown)

#define KIND_DEFN(name) k##name,
  enum Kind { FOR_EACH_ASSERT_ASSIGNABLE_KIND(KIND_DEFN) };
#undef KIND_DEFN

  static const char* KindToCString(Kind kind);
  static bool ParseKind(const char* str, Kind* out);

  enum {
    kInstancePos = 0,
    kDstTypePos = 1,
    kInstantiatorTAVPos = 2,
    kFunctionTAVPos = 3,
    kNumInputs = 4,
  };

  AssertAssignableInstr(const InstructionSource& source,
                        Value* value,
                        Value* dst_type,
                        Value* instantiator_type_arguments,
                        Value* function_type_arguments,
                        const String& dst_name,
                        intptr_t deopt_id,
                        Kind kind = kUnknown)
      : TemplateDefinition(source, deopt_id),
        token_pos_(source.token_pos),
        dst_name_(dst_name),
        kind_(kind) {
    ASSERT(!dst_name.IsNull());
    SetInputAt(kInstancePos, value);
    SetInputAt(kDstTypePos, dst_type);
    SetInputAt(kInstantiatorTAVPos, instantiator_type_arguments);
    SetInputAt(kFunctionTAVPos, function_type_arguments);
  }

  virtual intptr_t statistics_tag() const;

  DECLARE_INSTRUCTION(AssertAssignable)
  virtual CompileType ComputeType() const;
  virtual bool RecomputeType();

  Value* value() const { return inputs_[kInstancePos]; }
  Value* dst_type() const { return inputs_[kDstTypePos]; }
  Value* instantiator_type_arguments() const {
    return inputs_[kInstantiatorTAVPos];
  }
  Value* function_type_arguments() const { return inputs_[kFunctionTAVPos]; }

  virtual TokenPosition token_pos() const { return token_pos_; }
  const String& dst_name() const { return dst_name_; }

  virtual bool ComputeCanDeoptimize() const { return false; }
  virtual bool ComputeCanDeoptimizeAfterCall() const {
    return !CompilerState::Current().is_aot();
  }
  virtual intptr_t NumberOfInputsConsumedBeforeCall() const {
#if !defined(TARGET_ARCH_IA32)
    return InputCount();
#else
    // The ia32 implementation calls the stub by pushing the input registers
    // in the same order onto the stack thereby making the deopt-env correct.
    // (Due to lack of registers we cannot use all-argument calling convention
    // as in other architectures.)
    return 0;
#endif
  }

  virtual bool CanBecomeDeoptimizationTarget() const {
    // AssertAssignable instructions that are specialized by the optimizer
    // (e.g. replaced with CheckClass) need a deoptimization descriptor before.
    return true;
  }

  virtual Definition* Canonicalize(FlowGraph* flow_graph);

  virtual bool AttributesEqual(const Instruction& other) const { return true; }

  virtual Value* RedefinedValue() const;

  virtual void InferRange(RangeAnalysis* analysis, Range* range);

  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F)                                                          \
  F(const TokenPosition, token_pos_)                                           \
  F(const String&, dst_name_)                                                  \
  F(const Kind, kind_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(AssertAssignableInstr,
                                          TemplateDefinition,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(AssertAssignableInstr);
};

class AssertBooleanInstr : public TemplateDefinition<1, Throws, Pure> {
 public:
  AssertBooleanInstr(const InstructionSource& source,
                     Value* value,
                     intptr_t deopt_id)
      : TemplateDefinition(source, deopt_id), token_pos_(source.token_pos) {
    SetInputAt(0, value);
  }

  DECLARE_INSTRUCTION(AssertBoolean)
  virtual CompileType ComputeType() const;

  virtual TokenPosition token_pos() const { return token_pos_; }
  Value* value() const { return inputs_[0]; }

  virtual bool ComputeCanDeoptimize() const { return false; }
  virtual bool ComputeCanDeoptimizeAfterCall() const {
    return !CompilerState::Current().is_aot();
  }
  virtual intptr_t NumberOfInputsConsumedBeforeCall() const {
    return InputCount();
  }

  virtual Definition* Canonicalize(FlowGraph* flow_graph);

  virtual bool AttributesEqual(const Instruction& other) const { return true; }

  virtual Value* RedefinedValue() const;

  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F) F(const TokenPosition, token_pos_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(AssertBooleanInstr,
                                          TemplateDefinition,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(AssertBooleanInstr);
};

// Denotes a special parameter, currently either the context of a closure,
// the type arguments of a generic function or an arguments descriptor.
class SpecialParameterInstr : public TemplateDefinition<0, NoThrow> {
 public:
#define FOR_EACH_SPECIAL_PARAMETER_KIND(M)                                     \
  M(Context)                                                                   \
  M(TypeArgs)                                                                  \
  M(ArgDescriptor)                                                             \
  M(Exception)                                                                 \
  M(StackTrace)

#define KIND_DECL(name) k##name,
  enum SpecialParameterKind { FOR_EACH_SPECIAL_PARAMETER_KIND(KIND_DECL) };
#undef KIND_DECL

  // Defined as a static intptr_t instead of inside the enum since some
  // switch statements depend on the exhaustibility checking.
#define KIND_INC(name) +1
  static constexpr intptr_t kNumKinds =
      0 FOR_EACH_SPECIAL_PARAMETER_KIND(KIND_INC);
#undef KIND_INC

  static const char* KindToCString(SpecialParameterKind k);
  static bool ParseKind(const char* str, SpecialParameterKind* out);

  SpecialParameterInstr(SpecialParameterKind kind,
                        intptr_t deopt_id,
                        BlockEntryInstr* block)
      : TemplateDefinition(deopt_id), kind_(kind), block_(block) {}

  DECLARE_INSTRUCTION(SpecialParameter)

  virtual BlockEntryInstr* GetBlock() { return block_; }

  virtual CompileType ComputeType() const;

  virtual bool ComputeCanDeoptimize() const { return false; }

  virtual bool HasUnknownSideEffects() const { return false; }

  virtual bool AttributesEqual(const Instruction& other) const {
    return kind() == other.AsSpecialParameter()->kind();
  }
  SpecialParameterKind kind() const { return kind_; }

  const char* ToCString() const;

  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F) F(const SpecialParameterKind, kind_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(SpecialParameterInstr,
                                          TemplateDefinition,
                                          FIELD_LIST)
#undef FIELD_LIST
  DECLARE_EXTRA_SERIALIZATION

 private:
  BlockEntryInstr* block_ = nullptr;
  DISALLOW_COPY_AND_ASSIGN(SpecialParameterInstr);
};

struct ArgumentsInfo {
  ArgumentsInfo(intptr_t type_args_len,
                intptr_t count_with_type_args,
                intptr_t size_with_type_args,
                const Array& argument_names)
      : type_args_len(type_args_len),
        count_with_type_args(count_with_type_args),
        size_with_type_args(size_with_type_args),
        count_without_type_args(count_with_type_args -
                                (type_args_len > 0 ? 1 : 0)),
        size_without_type_args(size_with_type_args -
                               (type_args_len > 0 ? 1 : 0)),
        argument_names(argument_names) {}

  ArrayPtr ToArgumentsDescriptor() const {
    return ArgumentsDescriptor::New(type_args_len, count_without_type_args,
                                    size_without_type_args, argument_names);
  }

  const intptr_t type_args_len;
  const intptr_t count_with_type_args;
  const intptr_t size_with_type_args;
  const intptr_t count_without_type_args;
  const intptr_t size_without_type_args;
  const Array& argument_names;
};

template <intptr_t kExtraInputs>
class TemplateDartCall : public VariadicDefinition {
 public:
  TemplateDartCall(intptr_t deopt_id,
                   intptr_t type_args_len,
                   const Array& argument_names,
                   InputsArray&& inputs,
                   const InstructionSource& source)
      : VariadicDefinition(std::move(inputs), source, deopt_id),
        type_args_len_(type_args_len),
        argument_names_(argument_names),
        token_pos_(source.token_pos) {
    DEBUG_ASSERT(argument_names.IsNotTemporaryScopedHandle());
    ASSERT(InputCount() >= kExtraInputs);
  }

  inline StringPtr Selector();

  virtual bool MayThrow() const { return true; }
  virtual bool CanCallDart() const { return true; }

  virtual bool ComputeCanDeoptimize() const { return false; }
  virtual bool ComputeCanDeoptimizeAfterCall() const {
    return !CompilerState::Current().is_aot();
  }
  virtual intptr_t NumberOfInputsConsumedBeforeCall() const {
    return kExtraInputs;
  }

  intptr_t FirstArgIndex() const { return type_args_len_ > 0 ? 1 : 0; }
  Value* Receiver() const { return this->ArgumentValueAt(FirstArgIndex()); }
  intptr_t ArgumentCountWithoutTypeArgs() const {
    return ArgumentCount() - FirstArgIndex();
  }
  intptr_t ArgumentsSizeWithoutTypeArgs() const {
    return ArgumentsSize() - FirstArgIndex();
  }
  // ArgumentCount() includes the type argument vector if any.
  // Caution: Must override Instruction::ArgumentCount().
  intptr_t ArgumentCount() const {
    return move_arguments_ != nullptr ? move_arguments_->length()
                                      : InputCount() - kExtraInputs;
  }
  virtual intptr_t ArgumentsSize() const { return ArgumentCount(); }

  virtual void SetMoveArguments(MoveArgumentsArray* move_arguments) {
    ASSERT(move_arguments_ == nullptr);
    move_arguments_ = move_arguments;
  }
  virtual MoveArgumentsArray* GetMoveArguments() const {
    return move_arguments_;
  }
  virtual void ReplaceInputsWithMoveArguments(
      MoveArgumentsArray* move_arguments) {
    ASSERT(move_arguments_ == nullptr);
    ASSERT(move_arguments->length() == ArgumentCount());
    SetMoveArguments(move_arguments);
    ASSERT(InputCount() == ArgumentCount() + kExtraInputs);
    const intptr_t extra_inputs_base = InputCount() - kExtraInputs;
    for (intptr_t i = 0, n = ArgumentCount(); i < n; ++i) {
      InputAt(i)->RemoveFromUseList();
    }
    for (intptr_t i = 0; i < kExtraInputs; ++i) {
      SetInputAt(i, InputAt(extra_inputs_base + i));
    }
    inputs_.TruncateTo(kExtraInputs);
  }
  intptr_t type_args_len() const { return type_args_len_; }
  const Array& argument_names() const { return argument_names_; }
  virtual TokenPosition token_pos() const { return token_pos_; }
  ArrayPtr GetArgumentsDescriptor() const {
    return ArgumentsDescriptor::New(
        type_args_len(), ArgumentCountWithoutTypeArgs(),
        ArgumentsSizeWithoutTypeArgs(), argument_names());
  }

#define FIELD_LIST(F)                                                          \
  F(const intptr_t, type_args_len_)                                            \
  F(const Array&, argument_names_)                                             \
  F(const TokenPosition, token_pos_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(TemplateDartCall,
                                          VariadicDefinition,
                                          FIELD_LIST)
#undef FIELD_LIST
  DECLARE_EXTRA_SERIALIZATION

 private:
  MoveArgumentsArray* move_arguments_ = nullptr;

  DISALLOW_COPY_AND_ASSIGN(TemplateDartCall);
};

class ClosureCallInstr : public TemplateDartCall<1> {
 public:
  ClosureCallInstr(const Function& target_function,
                   InputsArray&& inputs,
                   intptr_t type_args_len,
                   const Array& argument_names,
                   const InstructionSource& source,
                   intptr_t deopt_id)
      : TemplateDartCall(deopt_id,
                         type_args_len,
                         argument_names,
                         std::move(inputs),
                         source),
        target_function_(target_function) {
    DEBUG_ASSERT(target_function.IsNotTemporaryScopedHandle());
  }

  DECLARE_INSTRUCTION(ClosureCall)

  const Function& target_function() const { return target_function_; }

  // TODO(kmillikin): implement exact call counts for closure calls.
  virtual intptr_t CallCount() const { return 1; }

  virtual bool HasUnknownSideEffects() const { return true; }

  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F) F(const Function&, target_function_)
  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(ClosureCallInstr,
                                          TemplateDartCall,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(ClosureCallInstr);
};

// Common base class for various kinds of instance call instructions
// (InstanceCallInstr, PolymorphicInstanceCallInstr).
class InstanceCallBaseInstr : public TemplateDartCall<0> {
 public:
  InstanceCallBaseInstr(const InstructionSource& source,
                        const String& function_name,
                        Token::Kind token_kind,
                        InputsArray&& arguments,
                        intptr_t type_args_len,
                        const Array& argument_names,
                        const ICData* ic_data,
                        intptr_t deopt_id,
                        const Function& interface_target,
                        const Function& tearoff_interface_target)
      : TemplateDartCall(deopt_id,
                         type_args_len,
                         argument_names,
                         std::move(arguments),
                         source),
        ic_data_(ic_data),
        function_name_(function_name),
        token_kind_(token_kind),
        interface_target_(interface_target),
        tearoff_interface_target_(tearoff_interface_target),
        result_type_(nullptr),
        has_unique_selector_(false),
        entry_kind_(Code::EntryKind::kNormal),
        receiver_is_not_smi_(false),
        is_call_on_this_(false) {
    DEBUG_ASSERT(function_name.IsNotTemporaryScopedHandle());
    DEBUG_ASSERT(interface_target.IsNotTemporaryScopedHandle());
    DEBUG_ASSERT(tearoff_interface_target.IsNotTemporaryScopedHandle());
    ASSERT(InputCount() > 0);
    ASSERT(Token::IsBinaryOperator(token_kind) ||
           Token::IsEqualityOperator(token_kind) ||
           Token::IsRelationalOperator(token_kind) ||
           Token::IsUnaryOperator(token_kind) ||
           Token::IsIndexOperator(token_kind) ||
           Token::IsTypeTestOperator(token_kind) ||
           Token::IsTypeCastOperator(token_kind) || token_kind == Token::kGET ||
           token_kind == Token::kSET || token_kind == Token::kILLEGAL);
  }

  const ICData* ic_data() const { return ic_data_; }
  bool HasICData() const {
    return (ic_data() != nullptr) && !ic_data()->IsNull();
  }

  // ICData can be replaced by optimizer.
  void set_ic_data(const ICData* value) { ic_data_ = value; }

  const String& function_name() const { return function_name_; }
  Token::Kind token_kind() const { return token_kind_; }
  const Function& interface_target() const { return interface_target_; }
  const Function& tearoff_interface_target() const {
    return tearoff_interface_target_;
  }

  bool has_unique_selector() const { return has_unique_selector_; }
  void set_has_unique_selector(bool b) { has_unique_selector_ = b; }

  virtual CompileType ComputeType() const;

  virtual bool CanBecomeDeoptimizationTarget() const {
    // Instance calls that are specialized by the optimizer need a
    // deoptimization descriptor before the call.
    return true;
  }

  virtual bool HasUnknownSideEffects() const { return true; }

  void SetResultType(Zone* zone, CompileType new_type) {
    result_type_ = new (zone) CompileType(new_type);
  }

  CompileType* result_type() const { return result_type_; }

  intptr_t result_cid() const {
    if (result_type_ == nullptr) {
      return kDynamicCid;
    }
    return result_type_->ToCid();
  }

  FunctionPtr ResolveForReceiverClass(const Class& cls, bool allow_add = true);

  Code::EntryKind entry_kind() const { return entry_kind_; }
  void set_entry_kind(Code::EntryKind value) { entry_kind_ = value; }

  void mark_as_call_on_this() { is_call_on_this_ = true; }
  bool is_call_on_this() const { return is_call_on_this_; }

  DECLARE_ABSTRACT_INSTRUCTION(InstanceCallBase);

  bool receiver_is_not_smi() const { return receiver_is_not_smi_; }
  void set_receiver_is_not_smi(bool value) { receiver_is_not_smi_ = value; }

  // Tries to prove that the receiver will not be a Smi based on the
  // interface target, CompileType and hints from TFA.
  void UpdateReceiverSminess(Zone* zone);

  bool CanReceiverBeSmiBasedOnInterfaceTarget(Zone* zone) const;

  virtual SpeculativeMode SpeculativeModeOfInput(intptr_t idx) const {
    if (type_args_len() > 0) {
      if (idx == 0) {
        return kGuardInputs;
      }
      idx--;
    }
    if (interface_target_.IsNull()) return kGuardInputs;
    return interface_target_.is_unboxed_parameter_at(idx) ? kNotSpeculative
                                                          : kGuardInputs;
  }

  virtual intptr_t ArgumentsSize() const;

  virtual Representation RequiredInputRepresentation(intptr_t idx) const;

  virtual intptr_t DeoptimizationTarget() const { return DeoptId::kNone; }

  virtual Representation representation() const;

#define FIELD_LIST(F)                                                          \
  F(const ICData*, ic_data_)                                                   \
  F(const String&, function_name_)                                             \
  /* Binary op, unary op, kGET or kILLEGAL. */                                 \
  F(const Token::Kind, token_kind_)                                            \
  F(const Function&, interface_target_)                                        \
  F(const Function&, tearoff_interface_target_)                                \
  /* Inferred result type. */                                                  \
  F(CompileType*, result_type_)                                                \
  F(bool, has_unique_selector_)                                                \
  F(Code::EntryKind, entry_kind_)                                              \
  F(bool, receiver_is_not_smi_)                                                \
  F(bool, is_call_on_this_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(InstanceCallBaseInstr,
                                          TemplateDartCall,
                                          FIELD_LIST)
#undef FIELD_LIST

 protected:
  friend class CallSpecializer;
  void set_ic_data(ICData* value) { ic_data_ = value; }
  void set_result_type(CompileType* result_type) { result_type_ = result_type; }

 private:
  DISALLOW_COPY_AND_ASSIGN(InstanceCallBaseInstr);
};

class InstanceCallInstr : public InstanceCallBaseInstr {
 public:
  InstanceCallInstr(
      const InstructionSource& source,
      const String& function_name,
      Token::Kind token_kind,
      InputsArray&& arguments,
      intptr_t type_args_len,
      const Array& argument_names,
      intptr_t checked_argument_count,
      const ZoneGrowableArray<const ICData*>& ic_data_array,
      intptr_t deopt_id,
      const Function& interface_target = Function::null_function(),
      const Function& tearoff_interface_target = Function::null_function())
      : InstanceCallBaseInstr(
            source,
            function_name,
            token_kind,
            std::move(arguments),
            type_args_len,
            argument_names,
            GetICData(ic_data_array, deopt_id, /*is_static_call=*/false),
            deopt_id,
            interface_target,
            tearoff_interface_target),
        checked_argument_count_(checked_argument_count),
        receivers_static_type_(nullptr) {}

  InstanceCallInstr(
      const InstructionSource& source,
      const String& function_name,
      Token::Kind token_kind,
      InputsArray&& arguments,
      intptr_t type_args_len,
      const Array& argument_names,
      intptr_t checked_argument_count,
      intptr_t deopt_id,
      const Function& interface_target = Function::null_function(),
      const Function& tearoff_interface_target = Function::null_function())
      : InstanceCallBaseInstr(source,
                              function_name,
                              token_kind,
                              std::move(arguments),
                              type_args_len,
                              argument_names,
                              /*ic_data=*/nullptr,
                              deopt_id,
                              interface_target,
                              tearoff_interface_target),
        checked_argument_count_(checked_argument_count),
        receivers_static_type_(nullptr) {}

  DECLARE_INSTRUCTION(InstanceCall)

  intptr_t checked_argument_count() const { return checked_argument_count_; }

  virtual intptr_t CallCount() const {
    return ic_data() == nullptr ? 0 : ic_data()->AggregateCount();
  }

  void set_receivers_static_type(const AbstractType* receiver_type) {
    ASSERT(receiver_type != nullptr);
    receivers_static_type_ = receiver_type;
  }

  virtual Definition* Canonicalize(FlowGraph* flow_graph);

  PRINT_OPERANDS_TO_SUPPORT

  bool MatchesCoreName(const String& name);

  const class BinaryFeedback& BinaryFeedback();
  void SetBinaryFeedback(const class BinaryFeedback* binary) {
    binary_ = binary;
  }

  const CallTargets& Targets();
  void SetTargets(const CallTargets* targets) { targets_ = targets; }

  void EnsureICData(FlowGraph* graph);

#define FIELD_LIST(F)                                                          \
  F(const intptr_t, checked_argument_count_)                                   \
  F(const AbstractType*, receivers_static_type_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(InstanceCallInstr,
                                          InstanceCallBaseInstr,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  const CallTargets* targets_ = nullptr;
  const class BinaryFeedback* binary_ = nullptr;

  DISALLOW_COPY_AND_ASSIGN(InstanceCallInstr);
};

class PolymorphicInstanceCallInstr : public InstanceCallBaseInstr {
 public:
  // Generate a replacement polymorphic call instruction.
  static PolymorphicInstanceCallInstr* FromCall(Zone* zone,
                                                InstanceCallBaseInstr* call,
                                                const CallTargets& targets,
                                                bool complete) {
    ASSERT(!call->HasMoveArguments());
    InputsArray args(zone, call->ArgumentCount());
    for (intptr_t i = 0, n = call->ArgumentCount(); i < n; ++i) {
      args.Add(call->ArgumentValueAt(i)->CopyWithType(zone));
    }
    auto new_call = new (zone) PolymorphicInstanceCallInstr(
        call->source(), call->function_name(), call->token_kind(),
        std::move(args), call->type_args_len(), call->argument_names(),
        call->ic_data(), call->deopt_id(), call->interface_target(),
        call->tearoff_interface_target(), targets, complete);
    new_call->set_result_type(call->result_type());
    new_call->set_entry_kind(call->entry_kind());
    new_call->set_has_unique_selector(call->has_unique_selector());
    if (call->is_call_on_this()) {
      new_call->mark_as_call_on_this();
    }
    return new_call;
  }

  bool complete() const { return complete_; }

  virtual CompileType ComputeType() const;

  bool HasOnlyDispatcherOrImplicitAccessorTargets() const;

  const CallTargets& targets() const { return targets_; }
  intptr_t NumberOfChecks() const { return targets_.length(); }

  bool IsSureToCallSingleRecognizedTarget() const;

  virtual intptr_t CallCount() const;

  // If this polymorphic call site was created to cover the remaining cids after
  // inlining then we need to keep track of the total number of calls including
  // the ones that we inlined. This is different from the CallCount above:  Eg
  // if there were 100 calls originally, distributed across three class-ids in
  // the ratio 50, 40, 7, 3.  The first two were inlined, so now we have only
  // 10 calls in the CallCount above, but the heuristics need to know that the
  // last two cids cover 7% and 3% of the calls, not 70% and 30%.
  intptr_t total_call_count() { return total_call_count_; }

  void set_total_call_count(intptr_t count) { total_call_count_ = count; }

  DECLARE_INSTRUCTION(PolymorphicInstanceCall)

  virtual Definition* Canonicalize(FlowGraph* graph);

  static TypePtr ComputeRuntimeType(const CallTargets& targets);

  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F)                                                          \
  F(const CallTargets&, targets_)                                              \
  F(const bool, complete_)                                                     \
  F(intptr_t, total_call_count_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(PolymorphicInstanceCallInstr,
                                          InstanceCallBaseInstr,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  PolymorphicInstanceCallInstr(const InstructionSource& source,
                               const String& function_name,
                               Token::Kind token_kind,
                               InputsArray&& arguments,
                               intptr_t type_args_len,
                               const Array& argument_names,
                               const ICData* ic_data,
                               intptr_t deopt_id,
                               const Function& interface_target,
                               const Function& tearoff_interface_target,
                               const CallTargets& targets,
                               bool complete)
      : InstanceCallBaseInstr(source,
                              function_name,
                              token_kind,
                              std::move(arguments),
                              type_args_len,
                              argument_names,
                              ic_data,
                              deopt_id,
                              interface_target,
                              tearoff_interface_target),
        targets_(targets),
        complete_(complete) {
    ASSERT(targets.length() != 0);
    total_call_count_ = CallCount();
  }

  friend class PolymorphicInliner;

  DISALLOW_COPY_AND_ASSIGN(PolymorphicInstanceCallInstr);
};

// Instance call using the global dispatch table.
//
// Takes untagged ClassId of the receiver as extra input.
class DispatchTableCallInstr : public TemplateDartCall<1> {
 public:
  DispatchTableCallInstr(const InstructionSource& source,
                         const Function& interface_target,
                         const compiler::TableSelector* selector,
                         InputsArray&& arguments,
                         intptr_t type_args_len,
                         const Array& argument_names)
      : TemplateDartCall(DeoptId::kNone,
                         type_args_len,
                         argument_names,
                         std::move(arguments),
                         source),
        interface_target_(interface_target),
        selector_(selector) {
    ASSERT(selector != nullptr);
    DEBUG_ASSERT(interface_target_.IsNotTemporaryScopedHandle());
    ASSERT(InputCount() > 0);
  }

  static DispatchTableCallInstr* FromCall(
      Zone* zone,
      const InstanceCallBaseInstr* call,
      Value* cid,
      const Function& interface_target,
      const compiler::TableSelector* selector);

  DECLARE_INSTRUCTION(DispatchTableCall)
  DECLARE_ATTRIBUTES(selector_name())

  const Function& interface_target() const { return interface_target_; }
  const compiler::TableSelector* selector() const { return selector_; }
  const char* selector_name() const {
    return String::Handle(interface_target().name()).ToCString();
  }

  Value* class_id() const { return InputAt(InputCount() - 1); }

  virtual CompileType ComputeType() const;

  virtual Definition* Canonicalize(FlowGraph* flow_graph);

  virtual bool CanBecomeDeoptimizationTarget() const { return false; }

  virtual intptr_t DeoptimizationTarget() const { return DeoptId::kNone; }

  virtual bool HasUnknownSideEffects() const { return true; }

  virtual SpeculativeMode SpeculativeModeOfInput(intptr_t idx) const {
    if (type_args_len() > 0) {
      if (idx == 0) {
        return kGuardInputs;
      }
      idx--;
    }
    return interface_target_.is_unboxed_parameter_at(idx) ? kNotSpeculative
                                                          : kGuardInputs;
  }

  virtual intptr_t ArgumentsSize() const;

  virtual Representation RequiredInputRepresentation(intptr_t idx) const;

  virtual Representation representation() const;

  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F)                                                          \
  F(const Function&, interface_target_)                                        \
  F(const compiler::TableSelector*, selector_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(DispatchTableCallInstr,
                                          TemplateDartCall,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(DispatchTableCallInstr);
};

class StrictCompareInstr : public TemplateComparison<2, NoThrow, Pure> {
 public:
  StrictCompareInstr(const InstructionSource& source,
                     Token::Kind kind,
                     Value* left,
                     Value* right,
                     bool needs_number_check,
                     intptr_t deopt_id);

  DECLARE_COMPARISON_INSTRUCTION(StrictCompare)

  virtual ComparisonInstr* CopyWithNewOperands(Value* left, Value* right);

  virtual CompileType ComputeType() const;

  virtual bool ComputeCanDeoptimize() const { return false; }

  virtual Definition* Canonicalize(FlowGraph* flow_graph);

  bool needs_number_check() const { return needs_number_check_; }
  void set_needs_number_check(bool value) { needs_number_check_ = value; }

  bool AttributesEqual(const Instruction& other) const;

  PRINT_OPERANDS_TO_SUPPORT;

#define FIELD_LIST(F)                                                          \
  /* True if the comparison must check for double or Mint and */               \
  /* use value comparison instead. */                                          \
  F(bool, needs_number_check_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(StrictCompareInstr,
                                          TemplateComparison,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  Condition EmitComparisonCodeRegConstant(FlowGraphCompiler* compiler,
                                          BranchLabels labels,
                                          Register reg,
                                          const Object& obj);
  bool TryEmitBoolTest(FlowGraphCompiler* compiler,
                       BranchLabels labels,
                       intptr_t input_index,
                       const Object& obj,
                       Condition* condition_out);

  DISALLOW_COPY_AND_ASSIGN(StrictCompareInstr);
};

// Comparison instruction that is equivalent to the (left & right) == 0
// comparison pattern.
class TestSmiInstr : public TemplateComparison<2, NoThrow, Pure> {
 public:
  TestSmiInstr(const InstructionSource& source,
               Token::Kind kind,
               Value* left,
               Value* right)
      : TemplateComparison(source, kind) {
    ASSERT(kind == Token::kEQ || kind == Token::kNE);
    SetInputAt(0, left);
    SetInputAt(1, right);
  }

  DECLARE_COMPARISON_INSTRUCTION(TestSmi);

  virtual ComparisonInstr* CopyWithNewOperands(Value* left, Value* right);

  virtual CompileType ComputeType() const;

  virtual bool ComputeCanDeoptimize() const { return false; }

  virtual Representation RequiredInputRepresentation(intptr_t idx) const {
    return kTagged;
  }

  DECLARE_EMPTY_SERIALIZATION(TestSmiInstr, TemplateComparison)

 private:
  DISALLOW_COPY_AND_ASSIGN(TestSmiInstr);
};

// Checks the input value cid against cids stored in a table and returns either
// a result or deoptimizes.  If the cid is not in the list and there is a deopt
// id, then the instruction deoptimizes.  If there is no deopt id, all the
// results must be the same (all true or all false) and the instruction returns
// the opposite for cids not on the list.  The first element in the table must
// always be the result for the Smi class-id and is allowed to differ from the
// other results even in the no-deopt case.
class TestCidsInstr : public TemplateComparison<1, NoThrow, Pure> {
 public:
  TestCidsInstr(const InstructionSource& source,
                Token::Kind kind,
                Value* value,
                const ZoneGrowableArray<intptr_t>& cid_results,
                intptr_t deopt_id);

  const ZoneGrowableArray<intptr_t>& cid_results() const {
    return cid_results_;
  }

  DECLARE_COMPARISON_INSTRUCTION(TestCids);

  virtual ComparisonInstr* CopyWithNewOperands(Value* left, Value* right);

  virtual CompileType ComputeType() const;

  virtual Definition* Canonicalize(FlowGraph* flow_graph);

  virtual bool ComputeCanDeoptimize() const {
    return GetDeoptId() != DeoptId::kNone;
  }

  Value* value() const { return inputs_[0]; }
  virtual Representation RequiredInputRepresentation(intptr_t idx) const {
    return kTagged;
  }

  virtual bool AttributesEqual(const Instruction& other) const;

  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F) F(const ZoneGrowableArray<intptr_t>&, cid_results_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(TestCidsInstr,
                                          TemplateComparison,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(TestCidsInstr);
};

class TestRangeInstr : public TemplateComparison<1, NoThrow, Pure> {
 public:
  TestRangeInstr(const InstructionSource& source,
                 Value* value,
                 uword lower,
                 uword upper,
                 Representation value_representation);

  DECLARE_COMPARISON_INSTRUCTION(TestRange);

  uword lower() const { return lower_; }
  uword upper() const { return upper_; }

  virtual ComparisonInstr* CopyWithNewOperands(Value* left, Value* right);

  virtual CompileType ComputeType() const;

  virtual Definition* Canonicalize(FlowGraph* flow_graph);

  virtual bool ComputeCanDeoptimize() const { return false; }

  Value* value() const { return inputs_[0]; }
  virtual Representation RequiredInputRepresentation(intptr_t idx) const {
    return value_representation_;
  }

  virtual bool AttributesEqual(const Instruction& other) const;

  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F)                                                          \
  F(const uword, lower_)                                                       \
  F(const uword, upper_)                                                       \
  F(const Representation, value_representation_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(TestRangeInstr,
                                          TemplateComparison,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(TestRangeInstr);
};

class EqualityCompareInstr : public TemplateComparison<2, NoThrow, Pure> {
 public:
  EqualityCompareInstr(const InstructionSource& source,
                       Token::Kind kind,
                       Value* left,
                       Value* right,
                       intptr_t cid,
                       intptr_t deopt_id,
                       bool null_aware = false,
                       SpeculativeMode speculative_mode = kGuardInputs)
      : TemplateComparison(source, kind, deopt_id),
        null_aware_(null_aware),
        speculative_mode_(speculative_mode) {
    ASSERT(Token::IsEqualityOperator(kind));
    SetInputAt(0, left);
    SetInputAt(1, right);
    set_operation_cid(cid);
  }

  DECLARE_COMPARISON_INSTRUCTION(EqualityCompare)

  virtual ComparisonInstr* CopyWithNewOperands(Value* left, Value* right);

  virtual CompileType ComputeType() const;

  virtual bool ComputeCanDeoptimize() const { return false; }

  bool is_null_aware() const { return null_aware_; }
  void set_null_aware(bool value) { null_aware_ = value; }

  virtual Representation RequiredInputRepresentation(intptr_t idx) const {
    ASSERT((idx == 0) || (idx == 1));
    if (is_null_aware()) return kTagged;
    if (operation_cid() == kDoubleCid) return kUnboxedDouble;
    if (operation_cid() == kMintCid) return kUnboxedInt64;
    if (operation_cid() == kIntegerCid) return kUnboxedUword;
    return kTagged;
  }

  virtual SpeculativeMode SpeculativeModeOfInput(intptr_t index) const {
    return speculative_mode_;
  }

  virtual bool AttributesEqual(const Instruction& other) const {
    return ComparisonInstr::AttributesEqual(other) &&
           (null_aware_ == other.AsEqualityCompare()->null_aware_) &&
           (speculative_mode_ == other.AsEqualityCompare()->speculative_mode_);
  }

  virtual Definition* Canonicalize(FlowGraph* flow_graph);

  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F)                                                          \
  F(bool, null_aware_)                                                         \
  F(const SpeculativeMode, speculative_mode_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(EqualityCompareInstr,
                                          TemplateComparison,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(EqualityCompareInstr);
};

class RelationalOpInstr : public TemplateComparison<2, NoThrow, Pure> {
 public:
  RelationalOpInstr(const InstructionSource& source,
                    Token::Kind kind,
                    Value* left,
                    Value* right,
                    intptr_t cid,
                    intptr_t deopt_id,
                    SpeculativeMode speculative_mode = kGuardInputs)
      : TemplateComparison(source, kind, deopt_id),
        speculative_mode_(speculative_mode) {
    ASSERT(Token::IsRelationalOperator(kind));
    SetInputAt(0, left);
    SetInputAt(1, right);
    set_operation_cid(cid);
  }

  DECLARE_COMPARISON_INSTRUCTION(RelationalOp)

  virtual ComparisonInstr* CopyWithNewOperands(Value* left, Value* right);

  virtual CompileType ComputeType() const;

  virtual bool ComputeCanDeoptimize() const { return false; }

  virtual Representation RequiredInputRepresentation(intptr_t idx) const {
    ASSERT((idx == 0) || (idx == 1));
    if (operation_cid() == kDoubleCid) return kUnboxedDouble;
    if (operation_cid() == kMintCid) return kUnboxedInt64;
    return kTagged;
  }

  virtual SpeculativeMode SpeculativeModeOfInput(intptr_t index) const {
    return speculative_mode_;
  }

  virtual bool AttributesEqual(const Instruction& other) const {
    return ComparisonInstr::AttributesEqual(other) &&
           (speculative_mode_ == other.AsRelationalOp()->speculative_mode_);
  }

  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F) F(const SpeculativeMode, speculative_mode_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(RelationalOpInstr,
                                          TemplateComparison,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(RelationalOpInstr);
};

// TODO(vegorov): ComparisonInstr should be switched to use IfTheElseInstr for
// materialization of true and false constants.
class IfThenElseInstr : public Definition {
 public:
  IfThenElseInstr(ComparisonInstr* comparison,
                  Value* if_true,
                  Value* if_false,
                  intptr_t deopt_id)
      : Definition(deopt_id),
        comparison_(comparison),
        if_true_(Smi::Cast(if_true->BoundConstant()).Value()),
        if_false_(Smi::Cast(if_false->BoundConstant()).Value()) {
    // Adjust uses at the comparison.
    ASSERT(comparison->env() == nullptr);
    for (intptr_t i = comparison->InputCount() - 1; i >= 0; --i) {
      comparison->InputAt(i)->set_instruction(this);
    }
  }

  // Returns true if this combination of comparison and values flowing on
  // the true and false paths is supported on the current platform.
  static bool Supports(ComparisonInstr* comparison, Value* v1, Value* v2);

  DECLARE_INSTRUCTION(IfThenElse)

  intptr_t InputCount() const { return comparison()->InputCount(); }

  Value* InputAt(intptr_t i) const { return comparison()->InputAt(i); }

  virtual bool ComputeCanDeoptimize() const {
    return comparison()->ComputeCanDeoptimize();
  }

  virtual bool CanBecomeDeoptimizationTarget() const {
    return comparison()->CanBecomeDeoptimizationTarget();
  }

  virtual intptr_t DeoptimizationTarget() const {
    return comparison()->DeoptimizationTarget();
  }

  virtual Representation RequiredInputRepresentation(intptr_t i) const {
    return comparison()->RequiredInputRepresentation(i);
  }

  virtual CompileType ComputeType() const;

  virtual void InferRange(RangeAnalysis* analysis, Range* range);

  ComparisonInstr* comparison() const { return comparison_; }
  intptr_t if_true() const { return if_true_; }
  intptr_t if_false() const { return if_false_; }

  virtual bool AllowsCSE() const { return comparison()->AllowsCSE(); }
  virtual bool HasUnknownSideEffects() const {
    return comparison()->HasUnknownSideEffects();
  }
  virtual bool CanCallDart() const { return comparison()->CanCallDart(); }

  virtual bool AttributesEqual(const Instruction& other) const {
    auto const other_if_then_else = other.AsIfThenElse();
    return (comparison()->tag() == other_if_then_else->comparison()->tag()) &&
           comparison()->AttributesEqual(*other_if_then_else->comparison()) &&
           (if_true_ == other_if_then_else->if_true_) &&
           (if_false_ == other_if_then_else->if_false_);
  }

  virtual bool MayThrow() const { return comparison()->MayThrow(); }

  virtual void CopyDeoptIdFrom(const Instruction& instr) {
    Definition::CopyDeoptIdFrom(instr);
    comparison()->CopyDeoptIdFrom(instr);
  }

  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F)                                                          \
  F(ComparisonInstr*, comparison_)                                             \
  F(const intptr_t, if_true_)                                                  \
  F(const intptr_t, if_false_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(IfThenElseInstr,
                                          Definition,
                                          FIELD_LIST)
#undef FIELD_LIST
  DECLARE_EXTRA_SERIALIZATION

 private:
  virtual void RawSetInputAt(intptr_t i, Value* value) {
    comparison()->RawSetInputAt(i, value);
  }

  DISALLOW_COPY_AND_ASSIGN(IfThenElseInstr);
};

class StaticCallInstr : public TemplateDartCall<0> {
 public:
  StaticCallInstr(const InstructionSource& source,
                  const Function& function,
                  intptr_t type_args_len,
                  const Array& argument_names,
                  InputsArray&& arguments,
                  const ZoneGrowableArray<const ICData*>& ic_data_array,
                  intptr_t deopt_id,
                  ICData::RebindRule rebind_rule)
      : TemplateDartCall(deopt_id,
                         type_args_len,
                         argument_names,
                         std::move(arguments),
                         source),
        ic_data_(GetICData(ic_data_array, deopt_id, /*is_static_call=*/true)),
        call_count_(0),
        function_(function),
        rebind_rule_(rebind_rule),
        result_type_(nullptr),
        is_known_list_constructor_(false),
        entry_kind_(Code::EntryKind::kNormal),
        identity_(AliasIdentity::Unknown()) {
    DEBUG_ASSERT(function.IsNotTemporaryScopedHandle());
    ASSERT(!function.IsNull());
  }

  StaticCallInstr(const InstructionSource& source,
                  const Function& function,
                  intptr_t type_args_len,
                  const Array& argument_names,
                  InputsArray&& arguments,
                  intptr_t deopt_id,
                  intptr_t call_count,
                  ICData::RebindRule rebind_rule)
      : TemplateDartCall(deopt_id,
                         type_args_len,
                         argument_names,
                         std::move(arguments),
                         source),
        ic_data_(nullptr),
        call_count_(call_count),
        function_(function),
        rebind_rule_(rebind_rule),
        result_type_(nullptr),
        is_known_list_constructor_(false),
        entry_kind_(Code::EntryKind::kNormal),
        identity_(AliasIdentity::Unknown()) {
    DEBUG_ASSERT(function.IsNotTemporaryScopedHandle());
    ASSERT(!function.IsNull());
  }

  // Generate a replacement call instruction for an instance call which
  // has been found to have only one target.
  template <class C>
  static StaticCallInstr* FromCall(Zone* zone,
                                   const C* call,
                                   const Function& target,
                                   intptr_t call_count) {
    ASSERT(!call->HasMoveArguments());
    InputsArray args(zone, call->ArgumentCount());
    for (intptr_t i = 0; i < call->ArgumentCount(); i++) {
      args.Add(call->ArgumentValueAt(i)->CopyWithType());
    }
    StaticCallInstr* new_call = new (zone) StaticCallInstr(
        call->source(), target, call->type_args_len(), call->argument_names(),
        std::move(args), call->deopt_id(), call_count, ICData::kNoRebind);
    if (call->result_type() != nullptr) {
      new_call->result_type_ = call->result_type();
    }
    new_call->set_entry_kind(call->entry_kind());
    return new_call;
  }

  // ICData for static calls carries call count.
  const ICData* ic_data() const { return ic_data_; }
  bool HasICData() const {
    return (ic_data() != nullptr) && !ic_data()->IsNull();
  }

  void set_ic_data(const ICData* value) { ic_data_ = value; }

  DECLARE_INSTRUCTION(StaticCall)
  virtual CompileType ComputeType() const;
  virtual Definition* Canonicalize(FlowGraph* flow_graph);
  bool Evaluate(FlowGraph* flow_graph, const Object& argument, Object* result);
  bool Evaluate(FlowGraph* flow_graph,
                const Object& argument1,
                const Object& argument2,
                Object* result);

  // Accessors forwarded to the AST node.
  const Function& function() const { return function_; }

  virtual intptr_t CallCount() const {
    return ic_data() == nullptr ? call_count_ : ic_data()->AggregateCount();
  }

  virtual bool ComputeCanDeoptimize() const { return false; }
  virtual bool ComputeCanDeoptimizeAfterCall() const {
    return !CompilerState::Current().is_aot();
  }

  virtual bool CanBecomeDeoptimizationTarget() const {
    // Static calls that are specialized by the optimizer (e.g. sqrt) need a
    // deoptimization descriptor before the call.
    return true;
  }

  virtual bool HasUnknownSideEffects() const { return true; }
  virtual bool CanCallDart() const { return true; }

  // Initialize result type of this call instruction if target is a recognized
  // method or has pragma annotation.
  // Returns true on success, false if result type is still unknown.
  bool InitResultType(Zone* zone);

  void SetResultType(Zone* zone, CompileType new_type) {
    result_type_ = new (zone) CompileType(new_type);
  }

  CompileType* result_type() const { return result_type_; }

  intptr_t result_cid() const {
    if (result_type_ == nullptr) {
      return kDynamicCid;
    }
    return result_type_->ToCid();
  }

  bool is_known_list_constructor() const { return is_known_list_constructor_; }
  void set_is_known_list_constructor(bool value) {
    is_known_list_constructor_ = value;
  }

  Code::EntryKind entry_kind() const { return entry_kind_; }

  void set_entry_kind(Code::EntryKind value) { entry_kind_ = value; }

  bool IsRecognizedFactory() const { return is_known_list_constructor(); }

  virtual SpeculativeMode SpeculativeModeOfInput(intptr_t idx) const {
    if (type_args_len() > 0 || function().IsFactory()) {
      if (idx == 0) {
        return kGuardInputs;
      }
      idx--;
    }
    return function_.is_unboxed_parameter_at(idx) ? kNotSpeculative
                                                  : kGuardInputs;
  }

  virtual intptr_t ArgumentsSize() const;

  virtual Representation RequiredInputRepresentation(intptr_t idx) const;

  virtual intptr_t DeoptimizationTarget() const { return DeoptId::kNone; }

  virtual Representation representation() const;

  virtual AliasIdentity Identity() const { return identity_; }
  virtual void SetIdentity(AliasIdentity identity) { identity_ = identity; }

  const CallTargets& Targets();
  const class BinaryFeedback& BinaryFeedback();

  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F)                                                          \
  F(const ICData*, ic_data_)                                                   \
  F(const intptr_t, call_count_)                                               \
  F(const Function&, function_)                                                \
  F(const ICData::RebindRule, rebind_rule_)                                    \
  /* Known or inferred result type. */                                         \
  F(CompileType*, result_type_)                                                \
  /* 'True' for recognized list constructors. */                               \
  F(bool, is_known_list_constructor_)                                          \
  F(Code::EntryKind, entry_kind_)                                              \
  F(AliasIdentity, identity_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(StaticCallInstr,
                                          TemplateDartCall,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  const CallTargets* targets_ = nullptr;
  const class BinaryFeedback* binary_ = nullptr;

  DISALLOW_COPY_AND_ASSIGN(StaticCallInstr);
};

class LoadLocalInstr : public TemplateDefinition<0, NoThrow> {
 public:
  LoadLocalInstr(const LocalVariable& local, const InstructionSource& source)
      : TemplateDefinition(source),
        local_(local),
        is_last_(false),
        token_pos_(source.token_pos) {}

  DECLARE_INSTRUCTION(LoadLocal)
  virtual CompileType ComputeType() const;

  const LocalVariable& local() const { return local_; }

  virtual bool ComputeCanDeoptimize() const { return false; }

  virtual bool HasUnknownSideEffects() const {
    UNREACHABLE();  // Eliminated by SSA construction.
    return false;
  }

  void mark_last() { is_last_ = true; }
  bool is_last() const { return is_last_; }

  virtual TokenPosition token_pos() const { return token_pos_; }

  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F)                                                          \
  F(const LocalVariable&, local_)                                              \
  F(bool, is_last_)                                                            \
  F(const TokenPosition, token_pos_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(LoadLocalInstr,
                                          TemplateDefinition,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(LoadLocalInstr);
};

class DropTempsInstr : public Definition {
 public:
  DropTempsInstr(intptr_t num_temps, Value* value)
      : num_temps_(num_temps), has_input_(value != nullptr) {
    if (has_input_) {
      SetInputAt(0, value);
    }
  }

  DECLARE_INSTRUCTION(DropTemps)

  virtual intptr_t InputCount() const { return has_input_ ? 1 : 0; }
  virtual Value* InputAt(intptr_t i) const {
    ASSERT(has_input_ && (i == 0));
    return value_;
  }

  Value* value() const { return value_; }

  intptr_t num_temps() const { return num_temps_; }

  virtual CompileType ComputeType() const;

  virtual bool ComputeCanDeoptimize() const { return false; }

  virtual bool HasUnknownSideEffects() const {
    UNREACHABLE();  // Eliminated by SSA construction.
    return false;
  }

  virtual bool MayThrow() const { return false; }

  virtual TokenPosition token_pos() const { return TokenPosition::kTempMove; }

  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F)                                                          \
  F(const intptr_t, num_temps_)                                                \
  F(const bool, has_input_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(DropTempsInstr,
                                          Definition,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  virtual void RawSetInputAt(intptr_t i, Value* value) {
    ASSERT(has_input_);
    value_ = value;
  }

  Value* value_ = nullptr;

  DISALLOW_COPY_AND_ASSIGN(DropTempsInstr);
};

// This instruction is used to reserve a space on the expression stack
// that later would be filled with StoreLocal. Reserved space would be
// filled with a null value initially.
//
// Note: One must not use Constant(#null) to reserve expression stack space
// because it would lead to an incorrectly compiled unoptimized code. Graph
// builder would set Constant(#null) as an input definition to the instruction
// that consumes this value from the expression stack - not knowing that
// this value represents a placeholder - which might lead issues if instruction
// has specialization for constant inputs (see https://dartbug.com/33195).
class MakeTempInstr : public TemplateDefinition<0, NoThrow, Pure> {
 public:
  explicit MakeTempInstr(Zone* zone)
      : null_(new (zone) ConstantInstr(Object::ZoneHandle())) {
    // Note: We put ConstantInstr inside MakeTemp to simplify code generation:
    // having ConstantInstr allows us to use Location::Constant(null_) as an
    // output location for this instruction.
  }

  DECLARE_INSTRUCTION(MakeTemp)

  virtual CompileType ComputeType() const { return CompileType::Dynamic(); }

  virtual bool ComputeCanDeoptimize() const { return false; }

  virtual bool HasUnknownSideEffects() const {
    UNREACHABLE();  // Eliminated by SSA construction.
    return false;
  }

  virtual bool MayThrow() const { return false; }

  virtual TokenPosition token_pos() const { return TokenPosition::kTempMove; }

  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F) F(ConstantInstr*, null_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(MakeTempInstr,
                                          TemplateDefinition,
                                          FIELD_LIST)
#undef FIELD_LIST
  DECLARE_EXTRA_SERIALIZATION

 private:
  DISALLOW_COPY_AND_ASSIGN(MakeTempInstr);
};

class StoreLocalInstr : public TemplateDefinition<1, NoThrow> {
 public:
  StoreLocalInstr(const LocalVariable& local,
                  Value* value,
                  const InstructionSource& source)
      : TemplateDefinition(source),
        local_(local),
        is_dead_(false),
        is_last_(false),
        token_pos_(source.token_pos) {
    SetInputAt(0, value);
  }

  DECLARE_INSTRUCTION(StoreLocal)
  virtual CompileType ComputeType() const;

  const LocalVariable& local() const { return local_; }
  Value* value() const { return inputs_[0]; }

  virtual bool ComputeCanDeoptimize() const { return false; }

  void mark_dead() { is_dead_ = true; }
  bool is_dead() const { return is_dead_; }

  void mark_last() { is_last_ = true; }
  bool is_last() const { return is_last_; }

  virtual bool HasUnknownSideEffects() const {
    UNREACHABLE();  // Eliminated by SSA construction.
    return false;
  }

  virtual TokenPosition token_pos() const { return token_pos_; }

  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F)                                                          \
  F(const LocalVariable&, local_)                                              \
  F(bool, is_dead_)                                                            \
  F(bool, is_last_)                                                            \
  F(const TokenPosition, token_pos_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(StoreLocalInstr,
                                          TemplateDefinition,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(StoreLocalInstr);
};

class NativeCallInstr : public TemplateDartCall<0> {
 public:
  NativeCallInstr(const String& name,
                  const Function& function,
                  bool link_lazily,
                  const InstructionSource& source,
                  InputsArray&& args)
      : TemplateDartCall(DeoptId::kNone,
                         0,
                         Array::null_array(),
                         std::move(args),
                         source),
        native_name_(name),
        function_(function),
        token_pos_(source.token_pos),
        link_lazily_(link_lazily) {
    DEBUG_ASSERT(name.IsNotTemporaryScopedHandle());
    DEBUG_ASSERT(function.IsNotTemporaryScopedHandle());
    // +1 for return value placeholder.
    ASSERT(ArgumentCount() ==
           function.NumParameters() + (function.IsGeneric() ? 1 : 0) + 1);
  }

  DECLARE_INSTRUCTION(NativeCall)

  const String& native_name() const { return native_name_; }
  const Function& function() const { return function_; }
  NativeFunction native_c_function() const { return native_c_function_; }
  bool is_bootstrap_native() const { return is_bootstrap_native_; }
  bool is_auto_scope() const { return is_auto_scope_; }
  bool link_lazily() const { return link_lazily_; }
  virtual TokenPosition token_pos() const { return token_pos_; }

  virtual bool ComputeCanDeoptimize() const { return false; }

  virtual bool HasUnknownSideEffects() const { return true; }

  // Always creates an exit frame before more Dart code can be called.
  virtual bool CanCallDart() const { return false; }

  void SetupNative();

  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F)                                                          \
  F(const String&, native_name_)                                               \
  F(const Function&, function_)                                                \
  F(const TokenPosition, token_pos_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(NativeCallInstr,
                                          TemplateDartCall,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  void set_native_c_function(NativeFunction value) {
    native_c_function_ = value;
  }

  void set_is_bootstrap_native(bool value) { is_bootstrap_native_ = value; }
  void set_is_auto_scope(bool value) { is_auto_scope_ = value; }

  // These fields are not serialized.
  // IL serialization only supports lazy linking of native functions.
  NativeFunction native_c_function_ = nullptr;
  bool is_bootstrap_native_ = false;
  bool is_auto_scope_ = true;
  bool link_lazily_ = true;

  DISALLOW_COPY_AND_ASSIGN(NativeCallInstr);
};

// Performs a call to native C code. In contrast to NativeCall, the arguments
// are unboxed and passed through the native calling convention. However, not
// all dart objects can be passed as arguments. Please see the FFI documentation
// for more details.
//
// Arguments to FfiCallInstr:
// - The arguments to the native call, marshalled in IL as far as possible.
// - The argument address.
// - A TypedData for the return value to populate in machine code (optional).
class FfiCallInstr : public VariadicDefinition {
 public:
  FfiCallInstr(intptr_t deopt_id,
               const compiler::ffi::CallMarshaller& marshaller,
               bool is_leaf)
      : VariadicDefinition(marshaller.NumDefinitions() + 1 +
                               (marshaller.PassTypedData() ? 1 : 0),
                           deopt_id),
        marshaller_(marshaller),
        is_leaf_(is_leaf) {}

  DECLARE_INSTRUCTION(FfiCall)

  // Input index of the function pointer to invoke.
  intptr_t TargetAddressIndex() const { return marshaller_.NumDefinitions(); }

  // Input index of the typed data to populate if return value is struct.
  intptr_t TypedDataIndex() const {
    ASSERT(marshaller_.PassTypedData());
    return marshaller_.NumDefinitions() + 1;
  }

  virtual bool MayThrow() const {
    // By Dart_PropagateError.
    return true;
  }

  // FfiCallInstr calls C code, which can call back into Dart.
  virtual bool ComputeCanDeoptimize() const { return false; }
  virtual bool ComputeCanDeoptimizeAfterCall() const {
    return !CompilerState::Current().is_aot();
  }

  virtual bool HasUnknownSideEffects() const { return true; }

  // Always creates an exit frame before more Dart code can be called.
  virtual bool CanCallDart() const { return false; }

  virtual Representation RequiredInputRepresentation(intptr_t idx) const;
  virtual Representation representation() const;

  // Returns true if we can assume generated code will be executable during a
  // safepoint.
  //
  // TODO(#37739): This should be true when dual-mapping is enabled as well, but
  // there are some bugs where it still switches code protections currently.
  static bool CanExecuteGeneratedCodeInSafepoint() {
    return FLAG_precompiled_mode;
  }

  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F)                                                          \
  F(const compiler::ffi::CallMarshaller&, marshaller_)                         \
  F(bool, is_leaf_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(FfiCallInstr,
                                          VariadicDefinition,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  LocationSummary* MakeLocationSummaryInternal(Zone* zone,
                                               bool is_optimizing,
                                               const RegList temps) const;

  // Clobbers the first two given registers.
  // `saved_fp` is used as the frame base to rebase off of.
  // `temp1` is only used in case of PointerToMemoryLocation.
  void EmitParamMoves(FlowGraphCompiler* compiler,
                      const Register saved_fp,
                      const Register temp0,
                      const Register temp1);
  // Clobbers both given temp registers.
  void EmitReturnMoves(FlowGraphCompiler* compiler,
                       const Register temp0,
                       const Register temp1);

  DISALLOW_COPY_AND_ASSIGN(FfiCallInstr);
};

// Has the target address in a register passed as the last input in IL.
class CCallInstr : public VariadicDefinition {
 public:
  CCallInstr(
      const compiler::ffi::NativeCallingConvention& native_calling_convention,
      InputsArray&& inputs);

  DECLARE_INSTRUCTION(CCall)

  LocationSummary* MakeLocationSummaryInternal(Zone* zone,
                                               const RegList temps) const;

  // Input index of the function pointer to invoke.
  intptr_t TargetAddressIndex() const {
    return native_calling_convention_.argument_locations().length();
  }

  virtual bool MayThrow() const { return false; }

  virtual bool ComputeCanDeoptimize() const { return false; }

  virtual bool HasUnknownSideEffects() const { return true; }

  virtual bool CanCallDart() const { return false; }

  virtual Representation RequiredInputRepresentation(intptr_t idx) const;
  virtual Representation representation() const;

  void EmitParamMoves(FlowGraphCompiler* compiler,
                      Register saved_fp,
                      Register temp0);

  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F)                                                          \
  F(const compiler::ffi::NativeCallingConvention&, native_calling_convention_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(CCallInstr,
                                          VariadicDefinition,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(CCallInstr);
};

// Populates the untagged base + offset outside the heap with a tagged value.
//
// The store must be outside of the heap, does not emit a store barrier.
// For stores in the heap, use StoreIndexedInstr, which emits store barriers.
//
// Does not have a dual RawLoadFieldInstr, because for loads we do not have to
// distinguish between loading from within the heap or outside the heap.
// Use FlowGraphBuilder::RawLoadField.
class RawStoreFieldInstr : public TemplateInstruction<2, NoThrow> {
 public:
  RawStoreFieldInstr(Value* base, Value* value, int32_t offset)
      : offset_(offset) {
    SetInputAt(kBase, base);
    SetInputAt(kValue, value);
  }

  enum { kBase = 0, kValue = 1 };

  DECLARE_INSTRUCTION(RawStoreField)

  virtual Representation RequiredInputRepresentation(intptr_t idx) const;
  virtual bool ComputeCanDeoptimize() const { return false; }
  virtual bool HasUnknownSideEffects() const { return false; }

#define FIELD_LIST(F) F(const int32_t, offset_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(RawStoreFieldInstr,
                                          TemplateInstruction,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(RawStoreFieldInstr);
};

class DebugStepCheckInstr : public TemplateInstruction<0, NoThrow> {
 public:
  DebugStepCheckInstr(const InstructionSource& source,
                      UntaggedPcDescriptors::Kind stub_kind,
                      intptr_t deopt_id)
      : TemplateInstruction(source, deopt_id),
        token_pos_(source.token_pos),
        stub_kind_(stub_kind) {}

  DECLARE_INSTRUCTION(DebugStepCheck)

  virtual TokenPosition token_pos() const { return token_pos_; }
  virtual bool ComputeCanDeoptimize() const { return false; }
  virtual bool HasUnknownSideEffects() const { return true; }
  virtual Instruction* Canonicalize(FlowGraph* flow_graph);

#define FIELD_LIST(F)                                                          \
  F(const TokenPosition, token_pos_)                                           \
  F(const UntaggedPcDescriptors::Kind, stub_kind_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(DebugStepCheckInstr,
                                          TemplateInstruction,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(DebugStepCheckInstr);
};

enum StoreBarrierType { kNoStoreBarrier, kEmitStoreBarrier };

// StoreField instruction represents a store of the given [value] into
// the specified [slot] on the [instance] object. [emit_store_barrier] allows to
// specify whether the store should omit the write barrier. [kind] specifies
// whether this store is an initializing store, i.e. the first store into a
// field after the allocation.
//
// In JIT mode a slot might be a subject to the field unboxing optimization:
// if field type profiling shows that this slot always contains a double or SIMD
// value then this field becomes "unboxed" - in this case when storing into
// such field we update the payload of the box referenced by the field, rather
// than updating the field itself.
//
// Note: even if [emit_store_barrier] is set to [kEmitStoreBarrier] the store
// can still omit the barrier if it establishes that it is not needed.
//
// Note: stores generated from the constructor initializer list and from
// field initializers *must* be marked as initializing. Initializing stores
// into unboxed fields are responsible for allocating the mutable box which
// would be mutated by subsequent stores.
//
// Note: If the value to store is an unboxed derived pointer (e.g. pointer to
// start of internal typed data array backing) then this instruction cannot be
// moved across instructions which can trigger GC, to ensure that
//
//    LoadUntagged + Arithmetic + StoreField
//
// are performed as an effectively atomic set of instructions.
//
// See kernel_to_il.cc:BuildTypedDataViewFactoryConstructor.
class StoreFieldInstr : public TemplateInstruction<2, NoThrow> {
 public:
  enum class Kind {
    // Store is known to be the first store into a slot of an object after
    // object was allocated and before it escapes (e.g. stores in constructor
    // initializer list).
    kInitializing,

    // All other stores.
    kOther,
  };

  StoreFieldInstr(const Slot& slot,
                  Value* instance,
                  Value* value,
                  StoreBarrierType emit_store_barrier,
                  const InstructionSource& source,
                  Kind kind = Kind::kOther,
                  compiler::Assembler::MemoryOrder memory_order =
                      compiler::Assembler::kRelaxedNonAtomic)
      : TemplateInstruction(source),
        slot_(slot),
        emit_store_barrier_(emit_store_barrier),
        memory_order_(memory_order),
        token_pos_(source.token_pos),
        is_initialization_(kind == Kind::kInitializing) {
    SetInputAt(kInstancePos, instance);
    SetInputAt(kValuePos, value);
  }

  // Convenience constructor that looks up an IL Slot for the given [field].
  StoreFieldInstr(const Field& field,
                  Value* instance,
                  Value* value,
                  StoreBarrierType emit_store_barrier,
                  const InstructionSource& source,
                  const ParsedFunction* parsed_function,
                  Kind kind = Kind::kOther)
      : StoreFieldInstr(Slot::Get(field, parsed_function),
                        instance,
                        value,
                        emit_store_barrier,
                        source,
                        kind) {}

  virtual SpeculativeMode SpeculativeModeOfInput(intptr_t index) const {
    // Slots are unboxed based on statically inferrable type information.
    // Either sound non-nullable static types (JIT) or global type flow analysis
    // results (AOT).
    return slot().representation() != kTagged ? kNotSpeculative : kGuardInputs;
  }

  DECLARE_INSTRUCTION(StoreField)

  enum { kInstancePos = 0, kValuePos = 1 };

  Value* instance() const { return inputs_[kInstancePos]; }
  const Slot& slot() const { return slot_; }
  Value* value() const { return inputs_[kValuePos]; }

  virtual TokenPosition token_pos() const { return token_pos_; }
  bool is_initialization() const { return is_initialization_; }

  bool ShouldEmitStoreBarrier() const {
    if (RepresentationUtils::IsUnboxed(slot().representation())) {
      // The target field is native and unboxed, so not traversed by the GC.
      return false;
    }
    if (instance()->definition() == value()->definition()) {
      // `x.slot = x` cannot create an old->new or old&marked->old&unmarked
      // reference.
      return false;
    }

    if (value()->definition()->Type()->IsBool()) {
      return false;
    }
    return value()->NeedsWriteBarrier() &&
           (emit_store_barrier_ == kEmitStoreBarrier);
  }

  void set_emit_store_barrier(StoreBarrierType value) {
    emit_store_barrier_ = value;
  }

  virtual bool CanTriggerGC() const { return false; }

  virtual bool ComputeCanDeoptimize() const { return false; }

  // May require a deoptimization target for input conversions.
  virtual intptr_t DeoptimizationTarget() const { return GetDeoptId(); }

  // Currently CSE/LICM don't operate on any instructions that can be affected
  // by stores/loads. LoadOptimizer handles loads separately. Hence stores
  // are marked as having no side-effects.
  virtual bool HasUnknownSideEffects() const { return false; }

  virtual Representation RequiredInputRepresentation(intptr_t index) const;

  virtual Instruction* Canonicalize(FlowGraph* flow_graph);

  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F)                                                          \
  F(const Slot&, slot_)                                                        \
  F(StoreBarrierType, emit_store_barrier_)                                     \
  F(compiler::Assembler::MemoryOrder, memory_order_)                           \
  F(const TokenPosition, token_pos_)                                           \
  /* Marks initializing stores. E.g. in the constructor. */                    \
  F(const bool, is_initialization_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(StoreFieldInstr,
                                          TemplateInstruction,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  friend class JitCallSpecializer;  // For ASSERT(initialization_).

  intptr_t OffsetInBytes() const { return slot().offset_in_bytes(); }

  compiler::Assembler::CanBeSmi CanValueBeSmi() const {
    // Write barrier is skipped for nullable and non-nullable smis.
    ASSERT(value()->Type()->ToNullableCid() != kSmiCid);
    return value()->Type()->CanBeSmi() ? compiler::Assembler::kValueCanBeSmi
                                       : compiler::Assembler::kValueIsNotSmi;
  }

  DISALLOW_COPY_AND_ASSIGN(StoreFieldInstr);
};

class GuardFieldInstr : public TemplateInstruction<1, NoThrow, Pure> {
 public:
  GuardFieldInstr(Value* value, const Field& field, intptr_t deopt_id)
      : TemplateInstruction(deopt_id), field_(field) {
    SetInputAt(0, value);
    CheckField(field);
  }

  Value* value() const { return inputs_[0]; }

  const Field& field() const { return field_; }

  virtual bool ComputeCanDeoptimize() const { return true; }
  virtual bool CanBecomeDeoptimizationTarget() const {
    // Ensure that we record kDeopt PC descriptor in unoptimized code.
    return true;
  }

  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F) F(const Field&, field_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(GuardFieldInstr,
                                          TemplateInstruction,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(GuardFieldInstr);
};

class GuardFieldClassInstr : public GuardFieldInstr {
 public:
  GuardFieldClassInstr(Value* value, const Field& field, intptr_t deopt_id)
      : GuardFieldInstr(value, field, deopt_id) {
    CheckField(field);
  }

  DECLARE_INSTRUCTION(GuardFieldClass)

  virtual Instruction* Canonicalize(FlowGraph* flow_graph);

  virtual bool AttributesEqual(const Instruction& other) const;

  DECLARE_EMPTY_SERIALIZATION(GuardFieldClassInstr, GuardFieldInstr)

 private:
  DISALLOW_COPY_AND_ASSIGN(GuardFieldClassInstr);
};

class GuardFieldLengthInstr : public GuardFieldInstr {
 public:
  GuardFieldLengthInstr(Value* value, const Field& field, intptr_t deopt_id)
      : GuardFieldInstr(value, field, deopt_id) {
    CheckField(field);
  }

  DECLARE_INSTRUCTION(GuardFieldLength)

  virtual Instruction* Canonicalize(FlowGraph* flow_graph);

  virtual bool AttributesEqual(const Instruction& other) const;

  DECLARE_EMPTY_SERIALIZATION(GuardFieldLengthInstr, GuardFieldInstr)

 private:
  DISALLOW_COPY_AND_ASSIGN(GuardFieldLengthInstr);
};

// For a field of static type G<T0, ..., Tn> and a stored value of runtime
// type T checks that type arguments of T at G exactly match <T0, ..., Tn>
// and updates guarded state (UntaggedField::static_type_exactness_state_)
// accordingly.
//
// See StaticTypeExactnessState for more information.
class GuardFieldTypeInstr : public GuardFieldInstr {
 public:
  GuardFieldTypeInstr(Value* value, const Field& field, intptr_t deopt_id)
      : GuardFieldInstr(value, field, deopt_id) {
    CheckField(field);
  }

  DECLARE_INSTRUCTION(GuardFieldType)

  virtual Instruction* Canonicalize(FlowGraph* flow_graph);

  virtual bool AttributesEqual(const Instruction& other) const;

  DECLARE_EMPTY_SERIALIZATION(GuardFieldTypeInstr, GuardFieldInstr)

 private:
  DISALLOW_COPY_AND_ASSIGN(GuardFieldTypeInstr);
};

template <intptr_t N>
class TemplateLoadField : public TemplateDefinition<N, Throws> {
  using Base = TemplateDefinition<N, Throws>;

 public:
  TemplateLoadField(const InstructionSource& source,
                    bool calls_initializer = false,
                    intptr_t deopt_id = DeoptId::kNone,
                    const Field* field = nullptr)
      : Base(source, deopt_id),
        token_pos_(source.token_pos),
        throw_exception_on_initialization_(
            field != nullptr && !field->has_initializer() && field->is_late()),
        calls_initializer_(calls_initializer) {
    ASSERT(!calls_initializer || field != nullptr);
    ASSERT(!calls_initializer || (deopt_id != DeoptId::kNone));
  }

  virtual TokenPosition token_pos() const { return token_pos_; }
  bool calls_initializer() const { return calls_initializer_; }
  void set_calls_initializer(bool value) { calls_initializer_ = value; }

  bool throw_exception_on_initialization() const {
    return throw_exception_on_initialization_;
  }

  // Slow path is used if load throws exception on initialization.
  virtual bool UseSharedSlowPathStub(bool is_optimizing) const {
    return Base::SlowPathSharingSupported(is_optimizing);
  }

  virtual intptr_t DeoptimizationTarget() const { return Base::GetDeoptId(); }
  virtual bool ComputeCanDeoptimize() const { return false; }
  virtual bool ComputeCanDeoptimizeAfterCall() const {
    return calls_initializer() && !CompilerState::Current().is_aot();
  }
  virtual intptr_t NumberOfInputsConsumedBeforeCall() const {
    return Base::InputCount();
  }

  virtual bool HasUnknownSideEffects() const {
    return calls_initializer() && !throw_exception_on_initialization();
  }

  virtual bool CanCallDart() const {
    // The slow path (running the field initializer) always calls one of a
    // specific set of stubs. For those stubs that do not simply call the
    // runtime, the GC recognizes their frames and restores write barriers
    // automatically (see Thread::RestoreWriteBarrierInvariant).
    return false;
  }
  virtual bool CanTriggerGC() const { return calls_initializer(); }
  virtual bool MayThrow() const { return calls_initializer(); }

#define FIELD_LIST(F)                                                          \
  F(const TokenPosition, token_pos_)                                           \
  F(const bool, throw_exception_on_initialization_)                            \
  F(bool, calls_initializer_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(TemplateLoadField, Base, FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(TemplateLoadField);
};

class LoadStaticFieldInstr : public TemplateLoadField<0> {
 public:
  LoadStaticFieldInstr(const Field& field,
                       const InstructionSource& source,
                       bool calls_initializer = false,
                       intptr_t deopt_id = DeoptId::kNone)
      : TemplateLoadField<0>(source, calls_initializer, deopt_id, &field),
        field_(field) {}

  DECLARE_INSTRUCTION(LoadStaticField)

  virtual CompileType ComputeType() const;

  const Field& field() const { return field_; }

  virtual bool AllowsCSE() const {
    // If two loads of a static-final-late field call the initializer and one
    // dominates another, we can remove the dominated load with the result of
    // the dominating load.
    //
    // Though if the field is final-late there can be stores into it via
    // load/compare-with-sentinel/store. Those loads have
    // `!field().has_initializer()` and we won't allow CSE for them.
    return field().is_final() &&
           (!field().is_late() || field().has_initializer());
  }

  virtual bool AttributesEqual(const Instruction& other) const;

  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F) F(const Field&, field_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(LoadStaticFieldInstr,
                                          TemplateLoadField,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(LoadStaticFieldInstr);
};

class StoreStaticFieldInstr : public TemplateDefinition<1, NoThrow> {
 public:
  StoreStaticFieldInstr(const Field& field,
                        Value* value,
                        const InstructionSource& source)
      : TemplateDefinition(source),
        field_(field),
        token_pos_(source.token_pos) {
    DEBUG_ASSERT(field.IsNotTemporaryScopedHandle());
    SetInputAt(kValuePos, value);
    CheckField(field);
  }

  enum { kValuePos = 0 };

  DECLARE_INSTRUCTION(StoreStaticField)

  const Field& field() const { return field_; }
  Value* value() const { return inputs_[kValuePos]; }

  virtual bool ComputeCanDeoptimize() const { return false; }

  // Currently CSE/LICM don't operate on any instructions that can be affected
  // by stores/loads. LoadOptimizer handles loads separately. Hence stores
  // are marked as having no side-effects.
  virtual bool HasUnknownSideEffects() const { return false; }

  virtual TokenPosition token_pos() const { return token_pos_; }

  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F)                                                          \
  F(const Field&, field_)                                                      \
  F(const TokenPosition, token_pos_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(StoreStaticFieldInstr,
                                          TemplateDefinition,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  compiler::Assembler::CanBeSmi CanValueBeSmi() const {
    ASSERT(value()->Type()->ToNullableCid() != kSmiCid);
    return value()->Type()->CanBeSmi() ? compiler::Assembler::kValueCanBeSmi
                                       : compiler::Assembler::kValueIsNotSmi;
  }

  DISALLOW_COPY_AND_ASSIGN(StoreStaticFieldInstr);
};

enum AlignmentType {
  kUnalignedAccess,
  kAlignedAccess,
};

class LoadIndexedInstr : public TemplateDefinition<2, NoThrow> {
 public:
  LoadIndexedInstr(Value* array,
                   Value* index,
                   bool index_unboxed,
                   intptr_t index_scale,
                   intptr_t class_id,
                   AlignmentType alignment,
                   intptr_t deopt_id,
                   const InstructionSource& source,
                   CompileType* result_type = nullptr);

  TokenPosition token_pos() const { return token_pos_; }

  DECLARE_INSTRUCTION(LoadIndexed)
  virtual CompileType ComputeType() const;
  virtual bool RecomputeType();

  virtual Representation RequiredInputRepresentation(intptr_t idx) const {
    ASSERT(idx == 0 || idx == 1);
    // The array may be tagged or untagged (for external arrays).
    if (idx == 0) return kNoRepresentation;

    if (index_unboxed_) {
#if defined(TARGET_ARCH_IS_64_BIT)
      return kUnboxedInt64;
#else
      return kUnboxedUint32;
#endif
    } else {
      return kTagged;  // Index is a smi.
    }
  }

  bool IsExternal() const {
    return array()->definition()->representation() == kUntagged;
  }

  Value* array() const { return inputs_[0]; }
  Value* index() const { return inputs_[1]; }
  intptr_t index_scale() const { return index_scale_; }
  intptr_t class_id() const { return class_id_; }
  bool aligned() const { return alignment_ == kAlignedAccess; }

  virtual intptr_t DeoptimizationTarget() const { return GetDeoptId(); }
  virtual bool ComputeCanDeoptimize() const {
    return GetDeoptId() != DeoptId::kNone;
  }

  // Representation of LoadIndexed from arrays with given cid.
  static Representation RepresentationOfArrayElement(intptr_t array_cid);

  Representation representation() const {
    return RepresentationOfArrayElement(class_id());
  }

  virtual void InferRange(RangeAnalysis* analysis, Range* range);

  virtual bool HasUnknownSideEffects() const { return false; }

  virtual Definition* Canonicalize(FlowGraph* flow_graph);

#define FIELD_LIST(F)                                                          \
  F(const bool, index_unboxed_)                                                \
  F(const intptr_t, index_scale_)                                              \
  F(const intptr_t, class_id_)                                                 \
  F(const AlignmentType, alignment_)                                           \
  F(const TokenPosition, token_pos_)                                           \
  /* derived from call */                                                      \
  F(CompileType*, result_type_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(LoadIndexedInstr,
                                          TemplateDefinition,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(LoadIndexedInstr);
};

// Loads the specified number of code units from the given string, packing
// multiple code units into a single datatype. In essence, this is a specialized
// version of LoadIndexedInstr which accepts only string targets and can load
// multiple elements at once. The result datatype differs depending on the
// string type, element count, and architecture; if possible, the result is
// packed into a Smi, falling back to a Mint otherwise.
// TODO(zerny): Add support for loading into UnboxedInt32x4.
class LoadCodeUnitsInstr : public TemplateDefinition<2, NoThrow> {
 public:
  LoadCodeUnitsInstr(Value* str,
                     Value* index,
                     intptr_t element_count,
                     intptr_t class_id,
                     const InstructionSource& source)
      : TemplateDefinition(source),
        class_id_(class_id),
        token_pos_(source.token_pos),
        element_count_(element_count),
        representation_(kTagged) {
    ASSERT(element_count == 1 || element_count == 2 || element_count == 4);
    ASSERT(IsStringClassId(class_id));
    SetInputAt(0, str);
    SetInputAt(1, index);
  }

  TokenPosition token_pos() const { return token_pos_; }

  DECLARE_INSTRUCTION(LoadCodeUnits)
  virtual CompileType ComputeType() const;

  virtual Representation RequiredInputRepresentation(intptr_t idx) const {
    if (idx == 0) {
      // The string may be tagged or untagged (for external strings).
      return kNoRepresentation;
    }
    ASSERT(idx == 1);
    return kTagged;
  }

  bool IsExternal() const {
    return array()->definition()->representation() == kUntagged;
  }

  Value* array() const { return inputs_[0]; }
  Value* index() const { return inputs_[1]; }

  intptr_t index_scale() const {
    return compiler::target::Instance::ElementSizeFor(class_id_);
  }

  intptr_t class_id() const { return class_id_; }
  intptr_t element_count() const { return element_count_; }

  bool can_pack_into_smi() const {
    return element_count() <=
           compiler::target::kSmiBits / (index_scale() * kBitsPerByte);
  }

  virtual bool ComputeCanDeoptimize() const { return false; }

  virtual Representation representation() const { return representation_; }
  void set_representation(Representation repr) { representation_ = repr; }
  virtual void InferRange(RangeAnalysis* analysis, Range* range);

  virtual bool HasUnknownSideEffects() const { return false; }

  virtual bool CanTriggerGC() const {
    return !can_pack_into_smi() && (representation() == kTagged);
  }

#define FIELD_LIST(F)                                                          \
  F(const intptr_t, class_id_)                                                 \
  F(const TokenPosition, token_pos_)                                           \
  F(const intptr_t, element_count_)                                            \
  F(Representation, representation_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(LoadCodeUnitsInstr,
                                          TemplateDefinition,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(LoadCodeUnitsInstr);
};

class OneByteStringFromCharCodeInstr
    : public TemplateDefinition<1, NoThrow, Pure> {
 public:
  explicit OneByteStringFromCharCodeInstr(Value* char_code) {
    SetInputAt(0, char_code);
  }

  DECLARE_INSTRUCTION(OneByteStringFromCharCode)
  virtual CompileType ComputeType() const;

  Value* char_code() const { return inputs_[0]; }

  virtual bool ComputeCanDeoptimize() const { return false; }

  virtual bool AttributesEqual(const Instruction& other) const { return true; }

  DECLARE_EMPTY_SERIALIZATION(OneByteStringFromCharCodeInstr,
                              TemplateDefinition)

 private:
  DISALLOW_COPY_AND_ASSIGN(OneByteStringFromCharCodeInstr);
};

class StringToCharCodeInstr : public TemplateDefinition<1, NoThrow, Pure> {
 public:
  StringToCharCodeInstr(Value* str, intptr_t cid) : cid_(cid) {
    ASSERT(str != nullptr);
    SetInputAt(0, str);
  }

  DECLARE_INSTRUCTION(StringToCharCode)
  virtual CompileType ComputeType() const;

  Value* str() const { return inputs_[0]; }

  virtual bool ComputeCanDeoptimize() const { return false; }

  virtual bool AttributesEqual(const Instruction& other) const {
    return other.AsStringToCharCode()->cid_ == cid_;
  }

#define FIELD_LIST(F) F(const intptr_t, cid_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(StringToCharCodeInstr,
                                          TemplateDefinition,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(StringToCharCodeInstr);
};

// Scanning instruction to compute the result size and decoding parameters
// for the UTF-8 decoder. Equivalent to:
//
// int _scan(Uint8List bytes, int start, int end, _OneByteString table,
//     _Utf8Decoder decoder) {
//   int size = 0;
//   int flags = 0;
//   for (int i = start; i < end; i++) {
//     int t = table.codeUnitAt(bytes[i]);
//     size += t & sizeMask;
//     flags |= t;
//   }
//   decoder._scanFlags |= flags & flagsMask;
//   return size;
// }
//
// under these assumptions:
// - The difference between start and end must be less than 2^30, since the
//   resulting length can be twice the input length (and the result has to be in
//   Smi range). This is guaranteed by `_Utf8Decoder.chunkSize` which is set to
//   `65536`.
// - The decoder._scanFlags field is unboxed or contains a smi.
// - The first 128 entries of the table have the value 1.
class Utf8ScanInstr : public TemplateDefinition<5, NoThrow> {
 public:
  Utf8ScanInstr(Value* decoder,
                Value* bytes,
                Value* start,
                Value* end,
                Value* table,
                const Slot& decoder_scan_flags_field)
      : scan_flags_field_(decoder_scan_flags_field) {
    SetInputAt(0, decoder);
    SetInputAt(1, bytes);
    SetInputAt(2, start);
    SetInputAt(3, end);
    SetInputAt(4, table);
  }

  DECLARE_INSTRUCTION(Utf8Scan)

  virtual Representation RequiredInputRepresentation(intptr_t idx) const {
    ASSERT(idx >= 0 || idx <= 4);
    // The start and end inputs are unboxed, but in smi range.
    if (idx == 2 || idx == 3) return kUnboxedIntPtr;
    return kTagged;
  }

  virtual Representation representation() const { return kUnboxedIntPtr; }

  virtual CompileType ComputeType() const { return CompileType::Int(); }
  virtual bool HasUnknownSideEffects() const { return true; }
  virtual bool ComputeCanDeoptimize() const { return false; }
  virtual intptr_t DeoptimizationTarget() const { return DeoptId::kNone; }
  virtual void InferRange(RangeAnalysis* analysis, Range* range);

  virtual SpeculativeMode SpeculativeModeOfInput(intptr_t index) const {
    return kNotSpeculative;
  }

  virtual bool AttributesEqual(const Instruction& other) const {
    return scan_flags_field_.Equals(other.AsUtf8Scan()->scan_flags_field_);
  }

  bool IsScanFlagsUnboxed() const;

  PRINT_TO_SUPPORT

#define FIELD_LIST(F) F(const Slot&, scan_flags_field_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(Utf8ScanInstr,
                                          TemplateDefinition,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(Utf8ScanInstr);
};

class StoreIndexedInstr : public TemplateInstruction<3, NoThrow> {
 public:
  StoreIndexedInstr(Value* array,
                    Value* index,
                    Value* value,
                    StoreBarrierType emit_store_barrier,
                    bool index_unboxed,
                    intptr_t index_scale,
                    intptr_t class_id,
                    AlignmentType alignment,
                    intptr_t deopt_id,
                    const InstructionSource& source,
                    SpeculativeMode speculative_mode = kGuardInputs);
  DECLARE_INSTRUCTION(StoreIndexed)

  enum { kArrayPos = 0, kIndexPos = 1, kValuePos = 2 };

  Value* array() const { return inputs_[kArrayPos]; }
  Value* index() const { return inputs_[kIndexPos]; }
  Value* value() const { return inputs_[kValuePos]; }

  intptr_t index_scale() const { return index_scale_; }
  intptr_t class_id() const { return class_id_; }
  bool aligned() const { return alignment_ == kAlignedAccess; }

  bool ShouldEmitStoreBarrier() const {
    if (array()->definition() == value()->definition()) {
      // `x[slot] = x` cannot create an old->new or old&marked->old&unmarked
      // reference.
      return false;
    }

    if (value()->definition()->Type()->IsBool()) {
      return false;
    }
    return value()->NeedsWriteBarrier() &&
           (emit_store_barrier_ == kEmitStoreBarrier);
  }

  void set_emit_store_barrier(StoreBarrierType value) {
    emit_store_barrier_ = value;
  }

  virtual SpeculativeMode SpeculativeModeOfInput(intptr_t index) const {
    return speculative_mode_;
  }

  virtual bool ComputeCanDeoptimize() const { return false; }

  // Representation of value passed to StoreIndexed for arrays with given cid.
  static Representation RepresentationOfArrayElement(intptr_t array_cid);

  virtual Representation RequiredInputRepresentation(intptr_t idx) const;

  bool IsExternal() const {
    return array()->definition()->representation() == kUntagged;
  }

  virtual intptr_t DeoptimizationTarget() const {
    // Direct access since this instruction cannot deoptimize, and the deopt-id
    // was inherited from another instruction that could deoptimize.
    return GetDeoptId();
  }

  virtual bool HasUnknownSideEffects() const { return false; }

  void PrintOperandsTo(BaseTextBuffer* f) const;

  virtual Instruction* Canonicalize(FlowGraph* flow_graph);

#define FIELD_LIST(F)                                                          \
  F(StoreBarrierType, emit_store_barrier_)                                     \
  F(const bool, index_unboxed_)                                                \
  F(const intptr_t, index_scale_)                                              \
  F(const intptr_t, class_id_)                                                 \
  F(const AlignmentType, alignment_)                                           \
  F(const TokenPosition, token_pos_)                                           \
  F(const SpeculativeMode, speculative_mode_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(StoreIndexedInstr,
                                          TemplateInstruction,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  compiler::Assembler::CanBeSmi CanValueBeSmi() const {
    return compiler::Assembler::kValueCanBeSmi;
  }

  DISALLOW_COPY_AND_ASSIGN(StoreIndexedInstr);
};

class RecordCoverageInstr : public TemplateInstruction<0, NoThrow> {
 public:
  RecordCoverageInstr(const Array& coverage_array,
                      intptr_t coverage_index,
                      const InstructionSource& source)
      : TemplateInstruction(source),
        coverage_array_(coverage_array),
        coverage_index_(coverage_index),
        token_pos_(source.token_pos) {}

  DECLARE_INSTRUCTION(RecordCoverage)

  virtual TokenPosition token_pos() const { return token_pos_; }
  virtual bool ComputeCanDeoptimize() const { return false; }
  virtual bool HasUnknownSideEffects() const { return false; }
  virtual Instruction* Canonicalize(FlowGraph* flow_graph);

#define FIELD_LIST(F)                                                          \
  F(const Array&, coverage_array_)                                             \
  F(const intptr_t, coverage_index_)                                           \
  F(const TokenPosition, token_pos_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(RecordCoverageInstr,
                                          TemplateInstruction,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(RecordCoverageInstr);
};

// Note overridable, built-in: value ? false : true.
class BooleanNegateInstr : public TemplateDefinition<1, NoThrow> {
 public:
  explicit BooleanNegateInstr(Value* value) { SetInputAt(0, value); }

  DECLARE_INSTRUCTION(BooleanNegate)
  virtual CompileType ComputeType() const;

  Value* value() const { return inputs_[0]; }

  virtual bool ComputeCanDeoptimize() const { return false; }

  virtual bool HasUnknownSideEffects() const { return false; }

  virtual Definition* Canonicalize(FlowGraph* flow_graph);

  DECLARE_EMPTY_SERIALIZATION(BooleanNegateInstr, TemplateDefinition)

 private:
  DISALLOW_COPY_AND_ASSIGN(BooleanNegateInstr);
};

class InstanceOfInstr : public TemplateDefinition<3, Throws> {
 public:
  InstanceOfInstr(const InstructionSource& source,
                  Value* value,
                  Value* instantiator_type_arguments,
                  Value* function_type_arguments,
                  const AbstractType& type,
                  intptr_t deopt_id)
      : TemplateDefinition(source, deopt_id),
        token_pos_(source.token_pos),
        type_(type) {
    ASSERT(!type.IsNull());
    SetInputAt(0, value);
    SetInputAt(1, instantiator_type_arguments);
    SetInputAt(2, function_type_arguments);
  }

  DECLARE_INSTRUCTION(InstanceOf)
  virtual CompileType ComputeType() const;

  Value* value() const { return inputs_[0]; }
  Value* instantiator_type_arguments() const { return inputs_[1]; }
  Value* function_type_arguments() const { return inputs_[2]; }

  const AbstractType& type() const { return type_; }
  virtual TokenPosition token_pos() const { return token_pos_; }

  virtual bool ComputeCanDeoptimize() const { return false; }
  virtual bool ComputeCanDeoptimizeAfterCall() const {
    return !CompilerState::Current().is_aot();
  }

  virtual bool HasUnknownSideEffects() const { return false; }

  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F)                                                          \
  F(const TokenPosition, token_pos_)                                           \
  F(const AbstractType&, type_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(InstanceOfInstr,
                                          TemplateDefinition,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(InstanceOfInstr);
};

// Subclasses of 'AllocationInstr' must maintain the invariant that if
// 'WillAllocateNewOrRemembered' is true, then the result of the allocation must
// either reside in new space or be in the store buffer.
class AllocationInstr : public Definition {
 public:
  explicit AllocationInstr(const InstructionSource& source,
                           intptr_t deopt_id = DeoptId::kNone)
      : Definition(source, deopt_id),
        token_pos_(source.token_pos),
        identity_(AliasIdentity::Unknown()) {}

  virtual TokenPosition token_pos() const { return token_pos_; }

  virtual AliasIdentity Identity() const { return identity_; }
  virtual void SetIdentity(AliasIdentity identity) { identity_ = identity; }

  // TODO(sjindel): Update these conditions when the incremental write barrier
  // is added.
  virtual bool WillAllocateNewOrRemembered() const = 0;

  virtual bool MayThrow() const {
    // Any allocation instruction may throw an OutOfMemory error.
    return true;
  }
  virtual bool ComputeCanDeoptimize() const { return false; }
  virtual bool ComputeCanDeoptimizeAfterCall() const {
    // We test that allocation instructions have correct deopt environment
    // (which is needed in case OOM is thrown) by actually deoptimizing
    // optimized code in allocation slow paths.
    return !CompilerState::Current().is_aot();
  }
  virtual intptr_t NumberOfInputsConsumedBeforeCall() const {
    return InputCount();
  }

  // Returns the slot in the allocated object that contains the value at the
  // given input position. Returns nullptr if the input position is invalid
  // or if the input is not stored in the object.
  virtual const Slot* SlotForInput(intptr_t pos) { return nullptr; }

  // Returns the input index that has a corresponding slot which is identical to
  // the given slot. Returns a negative index if no such input found.
  intptr_t InputForSlot(const Slot& slot) {
    for (intptr_t i = 0; i < InputCount(); i++) {
      auto* const input_slot = SlotForInput(i);
      if (input_slot != nullptr && input_slot->IsIdentical(slot)) {
        return i;
      }
    }
    return -1;
  }

  // Returns whether the allocated object has initialized fields and/or payload
  // elements. Override for any subclass that returns an uninitialized object.
  virtual bool ObjectIsInitialized() { return true; }

  PRINT_OPERANDS_TO_SUPPORT

  DECLARE_ABSTRACT_INSTRUCTION(Allocation);

#define FIELD_LIST(F)                                                          \
  F(const TokenPosition, token_pos_)                                           \
  F(AliasIdentity, identity_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(AllocationInstr,
                                          Definition,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(AllocationInstr);
};

template <intptr_t N>
class TemplateAllocation : public AllocationInstr {
 public:
  explicit TemplateAllocation(const InstructionSource& source,
                              intptr_t deopt_id)
      : AllocationInstr(source, deopt_id), inputs_() {}

  virtual intptr_t InputCount() const { return N; }
  virtual Value* InputAt(intptr_t i) const { return inputs_[i]; }

  DECLARE_EMPTY_SERIALIZATION(TemplateAllocation, AllocationInstr)

 protected:
  EmbeddedArray<Value*, N> inputs_;

 private:
  friend class BranchInstr;
  friend class IfThenElseInstr;
  friend class RecordCoverageInstr;

  virtual void RawSetInputAt(intptr_t i, Value* value) { inputs_[i] = value; }
};

class AllocateObjectInstr : public AllocationInstr {
 public:
  enum { kTypeArgumentsPos = 0 };
  AllocateObjectInstr(const InstructionSource& source,
                      const Class& cls,
                      intptr_t deopt_id,
                      Value* type_arguments = nullptr)
      : AllocationInstr(source, deopt_id),
        cls_(cls),
        has_type_arguments_(type_arguments != nullptr),
        type_arguments_slot_(nullptr),
        type_arguments_(type_arguments) {
    DEBUG_ASSERT(cls.IsNotTemporaryScopedHandle());
    ASSERT(!cls.IsNull());
    ASSERT((cls.NumTypeArguments() > 0) == has_type_arguments_);
    if (has_type_arguments_) {
      SetInputAt(kTypeArgumentsPos, type_arguments);
      type_arguments_slot_ =
          &Slot::GetTypeArgumentsSlotFor(Thread::Current(), cls);
    }
  }

  DECLARE_INSTRUCTION(AllocateObject)
  virtual CompileType ComputeType() const;

  const Class& cls() const { return cls_; }
  Value* type_arguments() const { return type_arguments_; }

  virtual intptr_t InputCount() const { return has_type_arguments_ ? 1 : 0; }
  virtual Value* InputAt(intptr_t i) const {
    ASSERT(has_type_arguments_ && i == kTypeArgumentsPos);
    return type_arguments_;
  }

  virtual bool HasUnknownSideEffects() const { return false; }

  virtual bool WillAllocateNewOrRemembered() const {
    return WillAllocateNewOrRemembered(cls());
  }

  static bool WillAllocateNewOrRemembered(const Class& cls) {
    return IsAllocatableInNewSpace(cls.target_instance_size());
  }

  virtual const Slot* SlotForInput(intptr_t pos) {
    return pos == kTypeArgumentsPos ? type_arguments_slot_ : nullptr;
  }

  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F)                                                          \
  F(const Class&, cls_)                                                        \
  F(const bool, has_type_arguments_)                                           \
  F(const Slot*, type_arguments_slot_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(AllocateObjectInstr,
                                          AllocationInstr,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  virtual void RawSetInputAt(intptr_t i, Value* value) {
    ASSERT(has_type_arguments_ && (i == kTypeArgumentsPos));
    type_arguments_ = value;
  }

  Value* type_arguments_ = nullptr;

  DISALLOW_COPY_AND_ASSIGN(AllocateObjectInstr);
};

// Allocates and null initializes a closure object, given the closure function
// and the context as values.
class AllocateClosureInstr : public TemplateAllocation<2> {
 public:
  enum Inputs { kFunctionPos = 0, kContextPos = 1 };
  AllocateClosureInstr(const InstructionSource& source,
                       Value* closure_function,
                       Value* context,
                       intptr_t deopt_id)
      : TemplateAllocation(source, deopt_id) {
    SetInputAt(kFunctionPos, closure_function);
    SetInputAt(kContextPos, context);
  }

  DECLARE_INSTRUCTION(AllocateClosure)
  virtual CompileType ComputeType() const;

  Value* closure_function() const { return inputs_[kFunctionPos]; }
  Value* context() const { return inputs_[kContextPos]; }

  const Function& known_function() const {
    Value* const value = closure_function();
    if (value->BindsToConstant()) {
      ASSERT(value->BoundConstant().IsFunction());
      return Function::Cast(value->BoundConstant());
    }
    return Object::null_function();
  }

  virtual const Slot* SlotForInput(intptr_t pos) {
    switch (pos) {
      case kFunctionPos:
        return &Slot::Closure_function();
      case kContextPos:
        return &Slot::Closure_context();
      default:
        return TemplateAllocation::SlotForInput(pos);
    }
  }

  virtual bool HasUnknownSideEffects() const { return false; }

  virtual bool WillAllocateNewOrRemembered() const {
    return IsAllocatableInNewSpace(compiler::target::Closure::InstanceSize());
  }

  DECLARE_EMPTY_SERIALIZATION(AllocateClosureInstr, TemplateAllocation)

 private:
  DISALLOW_COPY_AND_ASSIGN(AllocateClosureInstr);
};

class AllocateUninitializedContextInstr : public TemplateAllocation<0> {
 public:
  AllocateUninitializedContextInstr(const InstructionSource& source,
                                    intptr_t num_context_variables,
                                    intptr_t deopt_id);

  DECLARE_INSTRUCTION(AllocateUninitializedContext)
  virtual CompileType ComputeType() const;

  intptr_t num_context_variables() const { return num_context_variables_; }

  virtual bool HasUnknownSideEffects() const { return false; }

  virtual bool WillAllocateNewOrRemembered() const {
    return compiler::target::WillAllocateNewOrRememberedContext(
        num_context_variables_);
  }

  virtual bool ObjectIsInitialized() { return false; }

  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F) F(const intptr_t, num_context_variables_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(AllocateUninitializedContextInstr,
                                          TemplateAllocation,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(AllocateUninitializedContextInstr);
};

// Allocates and null initializes a record object.
class AllocateRecordInstr : public TemplateAllocation<0> {
 public:
  AllocateRecordInstr(const InstructionSource& source,
                      RecordShape shape,
                      intptr_t deopt_id)
      : TemplateAllocation(source, deopt_id), shape_(shape) {}

  DECLARE_INSTRUCTION(AllocateRecord)
  virtual CompileType ComputeType() const;

  RecordShape shape() const { return shape_; }
  intptr_t num_fields() const { return shape_.num_fields(); }

  virtual bool HasUnknownSideEffects() const { return false; }

  virtual bool WillAllocateNewOrRemembered() const {
    return IsAllocatableInNewSpace(
        compiler::target::Record::InstanceSize(num_fields()));
  }

#define FIELD_LIST(F) F(const RecordShape, shape_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(AllocateRecordInstr,
                                          TemplateAllocation,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(AllocateRecordInstr);
};

// Allocates and initializes fields of a small record object
// (with 2 or 3 fields).
class AllocateSmallRecordInstr : public TemplateAllocation<3> {
 public:
  AllocateSmallRecordInstr(const InstructionSource& source,
                           RecordShape shape,  // 2 or 3 fields.
                           Value* value0,
                           Value* value1,
                           Value* value2,  // Optional.
                           intptr_t deopt_id)
      : TemplateAllocation(source, deopt_id), shape_(shape) {
    const intptr_t num_fields = shape.num_fields();
    ASSERT(num_fields == 2 || num_fields == 3);
    ASSERT((num_fields > 2) == (value2 != nullptr));
    SetInputAt(0, value0);
    SetInputAt(1, value1);
    if (num_fields > 2) {
      SetInputAt(2, value2);
    }
  }

  DECLARE_INSTRUCTION(AllocateSmallRecord)
  virtual CompileType ComputeType() const;

  RecordShape shape() const { return shape_; }
  intptr_t num_fields() const { return shape().num_fields(); }

  virtual intptr_t InputCount() const { return num_fields(); }

  virtual const Slot* SlotForInput(intptr_t pos) {
    return &Slot::GetRecordFieldSlot(
        Thread::Current(), compiler::target::Record::field_offset(pos));
  }

  virtual bool HasUnknownSideEffects() const { return false; }

  virtual bool WillAllocateNewOrRemembered() const {
    return IsAllocatableInNewSpace(
        compiler::target::Record::InstanceSize(num_fields()));
  }

#define FIELD_LIST(F) F(const RecordShape, shape_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(AllocateSmallRecordInstr,
                                          TemplateAllocation,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(AllocateSmallRecordInstr);
};

// This instruction captures the state of the object which had its allocation
// removed during the AllocationSinking pass.
// It does not produce any real code only deoptimization information.
class MaterializeObjectInstr : public VariadicDefinition {
 public:
  MaterializeObjectInstr(AllocationInstr* allocation,
                         const Class& cls,
                         intptr_t length_or_shape,
                         const ZoneGrowableArray<const Slot*>& slots,
                         InputsArray&& values)
      : VariadicDefinition(std::move(values)),
        cls_(cls),
        length_or_shape_(length_or_shape),
        slots_(slots),
        registers_remapped_(false),
        allocation_(allocation) {
    ASSERT(slots_.length() == InputCount());
  }

  AllocationInstr* allocation() const { return allocation_; }
  const Class& cls() const { return cls_; }

  intptr_t length_or_shape() const { return length_or_shape_; }

  intptr_t FieldOffsetAt(intptr_t i) const {
    return slots_[i]->offset_in_bytes();
  }

  const Location& LocationAt(intptr_t i) {
    ASSERT(0 <= i && i < InputCount());
    return locations_[i];
  }

  DECLARE_INSTRUCTION(MaterializeObject)

  // SelectRepresentations pass is run once more while MaterializeObject
  // instructions are still in the graph. To avoid any redundant boxing
  // operations inserted by that pass we should indicate that this
  // instruction can cope with any representation as it is essentially
  // an environment use.
  virtual Representation RequiredInputRepresentation(intptr_t idx) const {
    ASSERT(0 <= idx && idx < InputCount());
    return kNoRepresentation;
  }

  virtual bool ComputeCanDeoptimize() const { return false; }
  virtual bool HasUnknownSideEffects() const { return false; }

  Location* locations() { return locations_; }
  void set_locations(Location* locations) { locations_ = locations; }

  virtual bool MayThrow() const { return false; }

  void RemapRegisters(intptr_t* cpu_reg_slots, intptr_t* fpu_reg_slots);

  bool was_visited_for_liveness() const { return visited_for_liveness_; }
  void mark_visited_for_liveness() { visited_for_liveness_ = true; }

  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F)                                                          \
  F(const Class&, cls_)                                                        \
  F(intptr_t, length_or_shape_)                                                \
  F(const ZoneGrowableArray<const Slot*>&, slots_)                             \
  F(bool, registers_remapped_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(MaterializeObjectInstr,
                                          VariadicDefinition,
                                          FIELD_LIST)
#undef FIELD_LIST
  DECLARE_EXTRA_SERIALIZATION

 private:
  Location* locations_ = nullptr;

  // Not serialized.
  AllocationInstr* allocation_ = nullptr;
  bool visited_for_liveness_ = false;

  DISALLOW_COPY_AND_ASSIGN(MaterializeObjectInstr);
};

class ArrayAllocationInstr : public AllocationInstr {
 public:
  explicit ArrayAllocationInstr(const InstructionSource& source,
                                intptr_t deopt_id)
      : AllocationInstr(source, deopt_id) {}

  virtual Value* num_elements() const = 0;

  bool HasConstantNumElements() const {
    return num_elements()->BindsToSmiConstant();
  }
  intptr_t GetConstantNumElements() const {
    return num_elements()->BoundSmiConstant();
  }

  DECLARE_ABSTRACT_INSTRUCTION(ArrayAllocation);

  DECLARE_EMPTY_SERIALIZATION(ArrayAllocationInstr, AllocationInstr)

 private:
  DISALLOW_COPY_AND_ASSIGN(ArrayAllocationInstr);
};

template <intptr_t N>
class TemplateArrayAllocation : public ArrayAllocationInstr {
 public:
  explicit TemplateArrayAllocation(const InstructionSource& source,
                                   intptr_t deopt_id)
      : ArrayAllocationInstr(source, deopt_id), inputs_() {}

  virtual intptr_t InputCount() const { return N; }
  virtual Value* InputAt(intptr_t i) const { return inputs_[i]; }

  DECLARE_EMPTY_SERIALIZATION(TemplateArrayAllocation, ArrayAllocationInstr)

 protected:
  EmbeddedArray<Value*, N> inputs_;

 private:
  virtual void RawSetInputAt(intptr_t i, Value* value) { inputs_[i] = value; }

  DISALLOW_COPY_AND_ASSIGN(TemplateArrayAllocation);
};

class CreateArrayInstr : public TemplateArrayAllocation<2> {
 public:
  CreateArrayInstr(const InstructionSource& source,
                   Value* type_arguments,
                   Value* num_elements,
                   intptr_t deopt_id)
      : TemplateArrayAllocation(source, deopt_id) {
    SetInputAt(kTypeArgumentsPos, type_arguments);
    SetInputAt(kLengthPos, num_elements);
  }

  enum { kTypeArgumentsPos = 0, kLengthPos = 1 };

  DECLARE_INSTRUCTION(CreateArray)
  virtual CompileType ComputeType() const;

  Value* type_arguments() const { return inputs_[kTypeArgumentsPos]; }
  virtual Value* num_elements() const { return inputs_[kLengthPos]; }

  virtual bool HasUnknownSideEffects() const { return false; }

  virtual bool WillAllocateNewOrRemembered() const {
    // Large arrays will use cards instead; cannot skip write barrier.
    if (!HasConstantNumElements()) return false;
    return compiler::target::WillAllocateNewOrRememberedArray(
        GetConstantNumElements());
  }

  virtual const Slot* SlotForInput(intptr_t pos) {
    switch (pos) {
      case kTypeArgumentsPos:
        return &Slot::Array_type_arguments();
      case kLengthPos:
        return &Slot::Array_length();
      default:
        return TemplateArrayAllocation::SlotForInput(pos);
    }
  }

  DECLARE_EMPTY_SERIALIZATION(CreateArrayInstr, TemplateArrayAllocation)

 private:
  DISALLOW_COPY_AND_ASSIGN(CreateArrayInstr);
};

class AllocateTypedDataInstr : public TemplateArrayAllocation<1> {
 public:
  AllocateTypedDataInstr(const InstructionSource& source,
                         classid_t class_id,
                         Value* num_elements,
                         intptr_t deopt_id)
      : TemplateArrayAllocation(source, deopt_id), class_id_(class_id) {
    SetInputAt(kLengthPos, num_elements);
  }

  enum { kLengthPos = 0 };

  DECLARE_INSTRUCTION(AllocateTypedData)
  virtual CompileType ComputeType() const;

  classid_t class_id() const { return class_id_; }
  virtual Value* num_elements() const { return inputs_[kLengthPos]; }

  virtual bool HasUnknownSideEffects() const { return false; }

  virtual bool WillAllocateNewOrRemembered() const {
    // No write barriers are generated for typed data accesses.
    return false;
  }

  virtual const Slot* SlotForInput(intptr_t pos) {
    switch (pos) {
      case kLengthPos:
        return &Slot::TypedDataBase_length();
      default:
        return TemplateArrayAllocation::SlotForInput(pos);
    }
  }

#define FIELD_LIST(F) F(const classid_t, class_id_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(AllocateTypedDataInstr,
                                          TemplateArrayAllocation,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(AllocateTypedDataInstr);
};

// Note: This instruction must not be moved without the indexed access that
// depends on it (e.g. out of loops). GC may collect the array while the
// external data-array is still accessed.
// TODO(vegorov) enable LICMing this instruction by ensuring that array itself
// is kept alive.
class LoadUntaggedInstr : public TemplateDefinition<1, NoThrow> {
 public:
  LoadUntaggedInstr(Value* object, intptr_t offset) : offset_(offset) {
    SetInputAt(0, object);
  }

  virtual Representation representation() const { return kUntagged; }
  DECLARE_INSTRUCTION(LoadUntagged)
  virtual CompileType ComputeType() const;

  virtual Representation RequiredInputRepresentation(intptr_t idx) const {
    ASSERT(idx == 0);
    // The object may be tagged or untagged (for external objects).
    return kNoRepresentation;
  }

  Value* object() const { return inputs_[0]; }
  intptr_t offset() const { return offset_; }

  virtual bool ComputeCanDeoptimize() const { return false; }

  virtual bool HasUnknownSideEffects() const { return false; }
  virtual bool AttributesEqual(const Instruction& other) const {
    return other.AsLoadUntagged()->offset_ == offset_;
  }

  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F) F(const intptr_t, offset_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(LoadUntaggedInstr,
                                          TemplateDefinition,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(LoadUntaggedInstr);
};

class LoadClassIdInstr : public TemplateDefinition<1, NoThrow, Pure> {
 public:
  explicit LoadClassIdInstr(Value* object,
                            Representation representation = kTagged,
                            bool input_can_be_smi = true)
      : representation_(representation), input_can_be_smi_(input_can_be_smi) {
    ASSERT(representation == kTagged || representation == kUnboxedUword);
    SetInputAt(0, object);
  }

  virtual Representation representation() const { return representation_; }
  DECLARE_INSTRUCTION(LoadClassId)
  virtual CompileType ComputeType() const;

  virtual Definition* Canonicalize(FlowGraph* flow_graph);

  Value* object() const { return inputs_[0]; }

  virtual bool ComputeCanDeoptimize() const { return false; }

  virtual bool AttributesEqual(const Instruction& other) const {
    auto const other_load = other.AsLoadClassId();
    return other_load->representation_ == representation_ &&
           other_load->input_can_be_smi_ == input_can_be_smi_;
  }

  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F)                                                          \
  F(const Representation, representation_)                                     \
  F(const bool, input_can_be_smi_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(LoadClassIdInstr,
                                          TemplateDefinition,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(LoadClassIdInstr);
};

// LoadFieldInstr represents a load from the given [slot] in the given
// [instance]. If calls_initializer(), then LoadFieldInstr also calls field
// initializer if field is not initialized yet (contains sentinel value).
//
// Note: if slot was a subject of the field unboxing optimization then this load
// would both load the box stored in the field and then load the content of
// the box.
class LoadFieldInstr : public TemplateLoadField<1> {
 public:
  LoadFieldInstr(Value* instance,
                 const Slot& slot,
                 const InstructionSource& source,
                 bool calls_initializer = false,
                 intptr_t deopt_id = DeoptId::kNone)
      : TemplateLoadField(source,
                          calls_initializer,
                          deopt_id,
                          slot.IsDartField() ? &slot.field() : nullptr),
        slot_(slot) {
    SetInputAt(0, instance);
  }

  Value* instance() const { return inputs_[0]; }
  const Slot& slot() const { return slot_; }

  virtual Representation representation() const;

  DECLARE_INSTRUCTION(LoadField)
  DECLARE_ATTRIBUTES(&slot())

  virtual CompileType ComputeType() const;

  virtual void InferRange(RangeAnalysis* analysis, Range* range);

  bool IsImmutableLengthLoad() const { return slot().IsImmutableLengthSlot(); }

  // Try evaluating this load against the given constant value of
  // the instance. Returns true if evaluation succeeded and
  // puts result into result.
  // Note: we only evaluate loads when we can ensure that
  // instance has the field.
  bool Evaluate(const Object& instance_value, Object* result);

  static bool TryEvaluateLoad(const Object& instance,
                              const Field& field,
                              Object* result);

  static bool TryEvaluateLoad(const Object& instance,
                              const Slot& field,
                              Object* result);

  virtual Definition* Canonicalize(FlowGraph* flow_graph);

  static bool IsFixedLengthArrayCid(intptr_t cid);
  static bool IsTypedDataViewFactory(const Function& function);
  static bool IsUnmodifiableTypedDataViewFactory(const Function& function);

  virtual bool AllowsCSE() const { return slot_.is_immutable(); }

  virtual bool CanTriggerGC() const { return calls_initializer(); }

  virtual bool AttributesEqual(const Instruction& other) const;

  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F) F(const Slot&, slot_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(LoadFieldInstr,
                                          TemplateLoadField,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  intptr_t OffsetInBytes() const { return slot().offset_in_bytes(); }

  // Generate code which checks if field is initialized and
  // calls initializer if it is not. Field value is already loaded.
  void EmitNativeCodeForInitializerCall(FlowGraphCompiler* compiler);

  DISALLOW_COPY_AND_ASSIGN(LoadFieldInstr);
};

class InstantiateTypeInstr : public TemplateDefinition<2, Throws> {
 public:
  InstantiateTypeInstr(const InstructionSource& source,
                       const AbstractType& type,
                       Value* instantiator_type_arguments,
                       Value* function_type_arguments,
                       intptr_t deopt_id)
      : TemplateDefinition(source, deopt_id),
        token_pos_(source.token_pos),
        type_(type) {
    DEBUG_ASSERT(type.IsNotTemporaryScopedHandle());
    SetInputAt(0, instantiator_type_arguments);
    SetInputAt(1, function_type_arguments);
  }

  DECLARE_INSTRUCTION(InstantiateType)

  Value* instantiator_type_arguments() const { return inputs_[0]; }
  Value* function_type_arguments() const { return inputs_[1]; }
  const AbstractType& type() const { return type_; }
  virtual TokenPosition token_pos() const { return token_pos_; }

  virtual bool ComputeCanDeoptimize() const { return false; }
  virtual bool ComputeCanDeoptimizeAfterCall() const {
    return !CompilerState::Current().is_aot();
  }
  virtual intptr_t NumberOfInputsConsumedBeforeCall() const {
    return InputCount();
  }

  virtual bool HasUnknownSideEffects() const { return false; }

  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F)                                                          \
  F(const TokenPosition, token_pos_)                                           \
  F(const AbstractType&, type_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(InstantiateTypeInstr,
                                          TemplateDefinition,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(InstantiateTypeInstr);
};

class InstantiateTypeArgumentsInstr : public TemplateDefinition<3, Throws> {
 public:
  InstantiateTypeArgumentsInstr(const InstructionSource& source,
                                Value* instantiator_type_arguments,
                                Value* function_type_arguments,
                                Value* type_arguments,
                                const Class& instantiator_class,
                                const Function& function,
                                intptr_t deopt_id)
      : TemplateDefinition(source, deopt_id),
        token_pos_(source.token_pos),
        instantiator_class_(instantiator_class),
        function_(function) {
    DEBUG_ASSERT(instantiator_class.IsNotTemporaryScopedHandle());
    DEBUG_ASSERT(function.IsNotTemporaryScopedHandle());
    SetInputAt(0, instantiator_type_arguments);
    SetInputAt(1, function_type_arguments);
    SetInputAt(2, type_arguments);
  }

  DECLARE_INSTRUCTION(InstantiateTypeArguments)

  Value* instantiator_type_arguments() const { return inputs_[0]; }
  Value* function_type_arguments() const { return inputs_[1]; }
  Value* type_arguments() const { return inputs_[2]; }
  const Class& instantiator_class() const { return instantiator_class_; }
  const Function& function() const { return function_; }
  virtual TokenPosition token_pos() const { return token_pos_; }

  virtual bool ComputeCanDeoptimize() const { return false; }
  virtual bool ComputeCanDeoptimizeAfterCall() const {
    return !CompilerState::Current().is_aot();
  }
  virtual intptr_t NumberOfInputsConsumedBeforeCall() const {
    return InputCount();
  }

  virtual bool HasUnknownSideEffects() const { return false; }

  virtual Definition* Canonicalize(FlowGraph* flow_graph);

  bool CanShareInstantiatorTypeArguments(
      bool* with_runtime_check = nullptr) const {
    if (instantiator_class().IsNull() || !type_arguments()->BindsToConstant() ||
        !type_arguments()->BoundConstant().IsTypeArguments()) {
      return false;
    }
    const auto& type_args =
        TypeArguments::Cast(type_arguments()->BoundConstant());
    return type_args.CanShareInstantiatorTypeArguments(instantiator_class(),
                                                       with_runtime_check);
  }

  bool CanShareFunctionTypeArguments(bool* with_runtime_check = nullptr) const {
    if (function().IsNull() || !type_arguments()->BindsToConstant() ||
        !type_arguments()->BoundConstant().IsTypeArguments()) {
      return false;
    }
    const auto& type_args =
        TypeArguments::Cast(type_arguments()->BoundConstant());
    return type_args.CanShareFunctionTypeArguments(function(),
                                                   with_runtime_check);
  }

  const Code& GetStub() const {
    bool with_runtime_check;
    if (CanShareInstantiatorTypeArguments(&with_runtime_check)) {
      ASSERT(with_runtime_check);
      return StubCode::InstantiateTypeArgumentsMayShareInstantiatorTA();
    } else if (CanShareFunctionTypeArguments(&with_runtime_check)) {
      ASSERT(with_runtime_check);
      return StubCode::InstantiateTypeArgumentsMayShareFunctionTA();
    }
    return StubCode::InstantiateTypeArguments();
  }

  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F)                                                          \
  F(const TokenPosition, token_pos_)                                           \
  F(const Class&, instantiator_class_)                                         \
  F(const Function&, function_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(InstantiateTypeArgumentsInstr,
                                          TemplateDefinition,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(InstantiateTypeArgumentsInstr);
};

// [AllocateContext] instruction allocates a new Context object with the space
// for the given [context_variables].
class AllocateContextInstr : public TemplateAllocation<0> {
 public:
  AllocateContextInstr(const InstructionSource& source,
                       const ZoneGrowableArray<const Slot*>& context_slots,
                       intptr_t deopt_id)
      : TemplateAllocation(source, deopt_id), context_slots_(context_slots) {}

  DECLARE_INSTRUCTION(AllocateContext)
  virtual CompileType ComputeType() const;

  const ZoneGrowableArray<const Slot*>& context_slots() const {
    return context_slots_;
  }

  intptr_t num_context_variables() const { return context_slots().length(); }

  virtual bool ComputeCanDeoptimize() const { return false; }

  virtual bool HasUnknownSideEffects() const { return false; }

  virtual bool WillAllocateNewOrRemembered() const {
    return compiler::target::WillAllocateNewOrRememberedContext(
        context_slots().length());
  }

  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F) F(const ZoneGrowableArray<const Slot*>&, context_slots_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(AllocateContextInstr,
                                          TemplateAllocation,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(AllocateContextInstr);
};

// [CloneContext] instruction clones the given Context object assuming that
// it contains exactly the provided [context_variables].
class CloneContextInstr : public TemplateDefinition<1, Throws> {
 public:
  CloneContextInstr(const InstructionSource& source,
                    Value* context_value,
                    const ZoneGrowableArray<const Slot*>& context_slots,
                    intptr_t deopt_id)
      : TemplateDefinition(source, deopt_id),
        token_pos_(source.token_pos),
        context_slots_(context_slots) {
    SetInputAt(0, context_value);
  }

  virtual TokenPosition token_pos() const { return token_pos_; }
  Value* context_value() const { return inputs_[0]; }

  const ZoneGrowableArray<const Slot*>& context_slots() const {
    return context_slots_;
  }

  DECLARE_INSTRUCTION(CloneContext)
  virtual CompileType ComputeType() const;

  virtual bool ComputeCanDeoptimize() const { return false; }
  virtual bool ComputeCanDeoptimizeAfterCall() const {
    // We test that allocation instructions have correct deopt environment
    // (which is needed in case OOM is thrown) by actually deoptimizing
    // optimized code in allocation slow paths.
    return !CompilerState::Current().is_aot();
  }
  virtual intptr_t NumberOfInputsConsumedBeforeCall() const {
    return InputCount();
  }

  virtual bool HasUnknownSideEffects() const { return false; }

#define FIELD_LIST(F)                                                          \
  F(const TokenPosition, token_pos_)                                           \
  F(const ZoneGrowableArray<const Slot*>&, context_slots_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(CloneContextInstr,
                                          TemplateDefinition,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(CloneContextInstr);
};

class CheckEitherNonSmiInstr : public TemplateInstruction<2, NoThrow, Pure> {
 public:
  CheckEitherNonSmiInstr(Value* left, Value* right, intptr_t deopt_id)
      : TemplateInstruction(deopt_id) {
    SetInputAt(0, left);
    SetInputAt(1, right);
  }

  Value* left() const { return inputs_[0]; }
  Value* right() const { return inputs_[1]; }

  DECLARE_INSTRUCTION(CheckEitherNonSmi)

  virtual bool ComputeCanDeoptimize() const { return true; }

  virtual Instruction* Canonicalize(FlowGraph* flow_graph);

  virtual bool AttributesEqual(const Instruction& other) const { return true; }

  DECLARE_EMPTY_SERIALIZATION(CheckEitherNonSmiInstr, TemplateInstruction)

 private:
  DISALLOW_COPY_AND_ASSIGN(CheckEitherNonSmiInstr);
};

struct Boxing : public AllStatic {
  // Whether the given representation can be boxed or unboxed.
  static bool Supports(Representation rep);

  // Whether boxing this value requires allocating a new object.
  static bool RequiresAllocation(Representation rep);

  // The offset into the Layout object for the boxed value that can store
  // the full range of values in the representation.
  // Only defined for allocated boxes (i.e., RequiresAllocation must be true).
  static intptr_t ValueOffset(Representation rep);

  // The class ID for the boxed value that can store the full range
  // of values in the representation.
  static intptr_t BoxCid(Representation rep);
};

class BoxInstr : public TemplateDefinition<1, NoThrow, Pure> {
 public:
  static BoxInstr* Create(Representation from, Value* value);

  Value* value() const { return inputs_[0]; }
  Representation from_representation() const { return from_representation_; }

  DECLARE_INSTRUCTION(Box)
  virtual CompileType ComputeType() const;

  virtual bool ComputeCanDeoptimize() const { return false; }
  virtual intptr_t DeoptimizationTarget() const { return DeoptId::kNone; }

  virtual Representation RequiredInputRepresentation(intptr_t idx) const {
    ASSERT(idx == 0);
    return from_representation();
  }

  virtual bool AttributesEqual(const Instruction& other) const {
    return other.AsBox()->from_representation() == from_representation();
  }

  Definition* Canonicalize(FlowGraph* flow_graph);

  virtual TokenPosition token_pos() const { return TokenPosition::kBox; }

  virtual SpeculativeMode SpeculativeModeOfInput(intptr_t index) const {
    return kNotSpeculative;
  }

#define FIELD_LIST(F) F(const Representation, from_representation_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(BoxInstr,
                                          TemplateDefinition,
                                          FIELD_LIST)
#undef FIELD_LIST

 protected:
  BoxInstr(Representation from_representation, Value* value)
      : from_representation_(from_representation) {
    SetInputAt(0, value);
  }

 private:
  intptr_t ValueOffset() const {
    return Boxing::ValueOffset(from_representation());
  }

  DISALLOW_COPY_AND_ASSIGN(BoxInstr);
};

class BoxIntegerInstr : public BoxInstr {
 public:
  BoxIntegerInstr(Representation representation, Value* value)
      : BoxInstr(representation, value) {}

  virtual bool ValueFitsSmi() const;

  virtual void InferRange(RangeAnalysis* analysis, Range* range);

  virtual CompileType ComputeType() const;
  virtual bool RecomputeType();

  virtual Definition* Canonicalize(FlowGraph* flow_graph);

  virtual bool CanTriggerGC() const { return !ValueFitsSmi(); }

  DECLARE_ABSTRACT_INSTRUCTION(BoxInteger)

  DECLARE_EMPTY_SERIALIZATION(BoxIntegerInstr, BoxInstr)

 private:
  DISALLOW_COPY_AND_ASSIGN(BoxIntegerInstr);
};

class BoxSmallIntInstr : public BoxIntegerInstr {
 public:
  explicit BoxSmallIntInstr(Representation rep, Value* value)
      : BoxIntegerInstr(rep, value) {
    ASSERT(RepresentationUtils::ValueSize(rep) * kBitsPerByte <=
           compiler::target::kSmiBits);
  }

  virtual bool ValueFitsSmi() const { return true; }

  DECLARE_INSTRUCTION(BoxSmallInt)

  DECLARE_EMPTY_SERIALIZATION(BoxSmallIntInstr, BoxIntegerInstr)

 private:
  DISALLOW_COPY_AND_ASSIGN(BoxSmallIntInstr);
};

class BoxInteger32Instr : public BoxIntegerInstr {
 public:
  BoxInteger32Instr(Representation representation, Value* value)
      : BoxIntegerInstr(representation, value) {}

  DECLARE_INSTRUCTION_BACKEND()

  DECLARE_EMPTY_SERIALIZATION(BoxInteger32Instr, BoxIntegerInstr)

 private:
  DISALLOW_COPY_AND_ASSIGN(BoxInteger32Instr);
};

class BoxInt32Instr : public BoxInteger32Instr {
 public:
  explicit BoxInt32Instr(Value* value)
      : BoxInteger32Instr(kUnboxedInt32, value) {}

  DECLARE_INSTRUCTION_NO_BACKEND(BoxInt32)

  DECLARE_EMPTY_SERIALIZATION(BoxInt32Instr, BoxInteger32Instr)

 private:
  DISALLOW_COPY_AND_ASSIGN(BoxInt32Instr);
};

class BoxUint32Instr : public BoxInteger32Instr {
 public:
  explicit BoxUint32Instr(Value* value)
      : BoxInteger32Instr(kUnboxedUint32, value) {}

  DECLARE_INSTRUCTION_NO_BACKEND(BoxUint32)

  DECLARE_EMPTY_SERIALIZATION(BoxUint32Instr, BoxInteger32Instr)

 private:
  DISALLOW_COPY_AND_ASSIGN(BoxUint32Instr);
};

class BoxInt64Instr : public BoxIntegerInstr {
 public:
  explicit BoxInt64Instr(Value* value)
      : BoxIntegerInstr(kUnboxedInt64, value) {}

  virtual Definition* Canonicalize(FlowGraph* flow_graph);

  DECLARE_INSTRUCTION(BoxInt64)

  DECLARE_EMPTY_SERIALIZATION(BoxInt64Instr, BoxIntegerInstr)

 private:
  DISALLOW_COPY_AND_ASSIGN(BoxInt64Instr);
};

class UnboxInstr : public TemplateDefinition<1, NoThrow, Pure> {
 public:
  static UnboxInstr* Create(Representation to,
                            Value* value,
                            intptr_t deopt_id,
                            SpeculativeMode speculative_mode = kGuardInputs);

  Value* value() const { return inputs_[0]; }

  virtual bool ComputeCanDeoptimize() const {
    if (SpeculativeModeOfInputs() == kNotSpeculative) {
      return false;
    }

    const intptr_t value_cid = value()->Type()->ToCid();
    const intptr_t box_cid = BoxCid();

    if (value_cid == box_cid) {
      return false;
    }

    if (CanConvertSmi() && (value_cid == kSmiCid)) {
      return false;
    }

    return true;
  }

  virtual SpeculativeMode SpeculativeModeOfInput(intptr_t index) const {
    return speculative_mode_;
  }

  virtual Representation representation() const { return representation_; }

  DECLARE_INSTRUCTION(Unbox)
  virtual CompileType ComputeType() const;

  virtual bool AttributesEqual(const Instruction& other) const {
    auto const other_unbox = other.AsUnbox();
    return (representation() == other_unbox->representation()) &&
           (speculative_mode_ == other_unbox->speculative_mode_);
  }

  Definition* Canonicalize(FlowGraph* flow_graph);

  virtual intptr_t DeoptimizationTarget() const { return GetDeoptId(); }

  virtual TokenPosition token_pos() const { return TokenPosition::kBox; }

#define FIELD_LIST(F)                                                          \
  F(const Representation, representation_)                                     \
  F(SpeculativeMode, speculative_mode_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(UnboxInstr,
                                          TemplateDefinition,
                                          FIELD_LIST)
#undef FIELD_LIST

 protected:
  UnboxInstr(Representation representation,
             Value* value,
             intptr_t deopt_id,
             SpeculativeMode speculative_mode)
      : TemplateDefinition(deopt_id),
        representation_(representation),
        speculative_mode_(speculative_mode) {
    SetInputAt(0, value);
  }

  void set_speculative_mode(SpeculativeMode value) {
    speculative_mode_ = value;
  }

 private:
  bool CanConvertSmi() const;
  void EmitLoadFromBox(FlowGraphCompiler* compiler);
  void EmitSmiConversion(FlowGraphCompiler* compiler);
  void EmitLoadInt32FromBoxOrSmi(FlowGraphCompiler* compiler);
  void EmitLoadInt64FromBoxOrSmi(FlowGraphCompiler* compiler);
  void EmitLoadFromBoxWithDeopt(FlowGraphCompiler* compiler);

  intptr_t BoxCid() const { return Boxing::BoxCid(representation_); }

  intptr_t ValueOffset() const { return Boxing::ValueOffset(representation_); }

  DISALLOW_COPY_AND_ASSIGN(UnboxInstr);
};

class UnboxIntegerInstr : public UnboxInstr {
 public:
  enum TruncationMode { kTruncate, kNoTruncation };

  UnboxIntegerInstr(Representation representation,
                    TruncationMode truncation_mode,
                    Value* value,
                    intptr_t deopt_id,
                    SpeculativeMode speculative_mode)
      : UnboxInstr(representation, value, deopt_id, speculative_mode),
        is_truncating_(truncation_mode == kTruncate) {}

  bool is_truncating() const { return is_truncating_; }

  void mark_truncating() { is_truncating_ = true; }

  virtual CompileType ComputeType() const;

  virtual bool AttributesEqual(const Instruction& other) const {
    auto const other_unbox = other.AsUnboxInteger();
    return UnboxInstr::AttributesEqual(other) &&
           (other_unbox->is_truncating_ == is_truncating_);
  }

  virtual Definition* Canonicalize(FlowGraph* flow_graph);

  DECLARE_ABSTRACT_INSTRUCTION(UnboxInteger)

  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F) F(bool, is_truncating_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(UnboxIntegerInstr,
                                          UnboxInstr,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(UnboxIntegerInstr);
};

class UnboxInteger32Instr : public UnboxIntegerInstr {
 public:
  UnboxInteger32Instr(Representation representation,
                      TruncationMode truncation_mode,
                      Value* value,
                      intptr_t deopt_id,
                      SpeculativeMode speculative_mode)
      : UnboxIntegerInstr(representation,
                          truncation_mode,
                          value,
                          deopt_id,
                          speculative_mode) {}

  DECLARE_INSTRUCTION_BACKEND()

  DECLARE_EMPTY_SERIALIZATION(UnboxInteger32Instr, UnboxIntegerInstr)

 private:
  DISALLOW_COPY_AND_ASSIGN(UnboxInteger32Instr);
};

class UnboxUint32Instr : public UnboxInteger32Instr {
 public:
  UnboxUint32Instr(Value* value,
                   intptr_t deopt_id,
                   SpeculativeMode speculative_mode = kGuardInputs)
      : UnboxInteger32Instr(kUnboxedUint32,
                            kTruncate,
                            value,
                            deopt_id,
                            speculative_mode) {
    ASSERT(is_truncating());
  }

  virtual bool ComputeCanDeoptimize() const;

  virtual void InferRange(RangeAnalysis* analysis, Range* range);

  DECLARE_INSTRUCTION_NO_BACKEND(UnboxUint32)

  DECLARE_EMPTY_SERIALIZATION(UnboxUint32Instr, UnboxInteger32Instr)

 private:
  DISALLOW_COPY_AND_ASSIGN(UnboxUint32Instr);
};

class UnboxInt32Instr : public UnboxInteger32Instr {
 public:
  UnboxInt32Instr(TruncationMode truncation_mode,
                  Value* value,
                  intptr_t deopt_id,
                  SpeculativeMode speculative_mode = kGuardInputs)
      : UnboxInteger32Instr(kUnboxedInt32,
                            truncation_mode,
                            value,
                            deopt_id,
                            speculative_mode) {}

  virtual bool ComputeCanDeoptimize() const;

  virtual void InferRange(RangeAnalysis* analysis, Range* range);

  virtual Definition* Canonicalize(FlowGraph* flow_graph);

  DECLARE_INSTRUCTION_NO_BACKEND(UnboxInt32)

  DECLARE_EMPTY_SERIALIZATION(UnboxInt32Instr, UnboxInteger32Instr)

 private:
  DISALLOW_COPY_AND_ASSIGN(UnboxInt32Instr);
};

class UnboxInt64Instr : public UnboxIntegerInstr {
 public:
  UnboxInt64Instr(Value* value,
                  intptr_t deopt_id,
                  SpeculativeMode speculative_mode)
      : UnboxIntegerInstr(kUnboxedInt64,
                          kNoTruncation,
                          value,
                          deopt_id,
                          speculative_mode) {}

  virtual void InferRange(RangeAnalysis* analysis, Range* range);

  virtual Definition* Canonicalize(FlowGraph* flow_graph);

  virtual bool ComputeCanDeoptimize() const {
    if (SpeculativeModeOfInputs() == kNotSpeculative) {
      return false;
    }

    return !value()->Type()->IsInt();
  }

  DECLARE_INSTRUCTION_NO_BACKEND(UnboxInt64)

  DECLARE_EMPTY_SERIALIZATION(UnboxInt64Instr, UnboxIntegerInstr)

 private:
  DISALLOW_COPY_AND_ASSIGN(UnboxInt64Instr);
};

bool Definition::IsInt64Definition() {
  return (Type()->ToCid() == kMintCid) || IsBinaryInt64Op() ||
         IsUnaryInt64Op() || IsShiftInt64Op() || IsSpeculativeShiftInt64Op() ||
         IsBoxInt64() || IsUnboxInt64();
}

class MathUnaryInstr : public TemplateDefinition<1, NoThrow, Pure> {
 public:
  enum MathUnaryKind {
    kIllegal,
    kSqrt,
    kDoubleSquare,
  };
  MathUnaryInstr(MathUnaryKind kind, Value* value, intptr_t deopt_id)
      : TemplateDefinition(deopt_id), kind_(kind) {
    SetInputAt(0, value);
  }

  Value* value() const { return inputs_[0]; }
  MathUnaryKind kind() const { return kind_; }

  virtual bool ComputeCanDeoptimize() const { return false; }

  virtual Representation representation() const { return kUnboxedDouble; }

  virtual Representation RequiredInputRepresentation(intptr_t idx) const {
    ASSERT(idx == 0);
    return kUnboxedDouble;
  }

  virtual SpeculativeMode SpeculativeModeOfInput(intptr_t idx) const {
    ASSERT(idx == 0);
    return kNotSpeculative;
  }

  virtual intptr_t DeoptimizationTarget() const {
    // Direct access since this instruction cannot deoptimize, and the deopt-id
    // was inherited from another instruction that could deoptimize.
    return GetDeoptId();
  }

  DECLARE_INSTRUCTION(MathUnary)
  virtual CompileType ComputeType() const;

  virtual bool AttributesEqual(const Instruction& other) const {
    return kind() == other.AsMathUnary()->kind();
  }

  Definition* Canonicalize(FlowGraph* flow_graph);

  static const char* KindToCString(MathUnaryKind kind);

  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F) F(const MathUnaryKind, kind_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(MathUnaryInstr,
                                          TemplateDefinition,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(MathUnaryInstr);
};

// Calls into the runtime and performs a case-insensitive comparison of the
// UTF16 strings (i.e. TwoByteString or ExternalTwoByteString) located at
// str[lhs_index:lhs_index + length] and str[rhs_index:rhs_index + length].
// Depending on [handle_surrogates], we will treat the strings as either
// UCS2 (no surrogate handling) or UTF16 (surrogates handled appropriately).
class CaseInsensitiveCompareInstr
    : public TemplateDefinition<4, NoThrow, Pure> {
 public:
  CaseInsensitiveCompareInstr(Value* str,
                              Value* lhs_index,
                              Value* rhs_index,
                              Value* length,
                              bool handle_surrogates,
                              intptr_t cid)
      : handle_surrogates_(handle_surrogates), cid_(cid) {
    ASSERT(cid == kTwoByteStringCid || cid == kExternalTwoByteStringCid);
    ASSERT(index_scale() == 2);
    SetInputAt(0, str);
    SetInputAt(1, lhs_index);
    SetInputAt(2, rhs_index);
    SetInputAt(3, length);
  }

  Value* str() const { return inputs_[0]; }
  Value* lhs_index() const { return inputs_[1]; }
  Value* rhs_index() const { return inputs_[2]; }
  Value* length() const { return inputs_[3]; }

  const RuntimeEntry& TargetFunction() const;
  bool IsExternal() const { return cid_ == kExternalTwoByteStringCid; }
  intptr_t class_id() const { return cid_; }

  intptr_t index_scale() const {
    return compiler::target::Instance::ElementSizeFor(cid_);
  }

  virtual bool ComputeCanDeoptimize() const { return false; }

  virtual Representation representation() const { return kTagged; }

  DECLARE_INSTRUCTION(CaseInsensitiveCompare)
  virtual CompileType ComputeType() const;

  virtual bool AttributesEqual(const Instruction& other) const {
    const auto* other_compare = other.AsCaseInsensitiveCompare();
    return (other_compare->handle_surrogates_ == handle_surrogates_) &&
           (other_compare->cid_ == cid_);
  }

#define FIELD_LIST(F)                                                          \
  F(const bool, handle_surrogates_)                                            \
  F(const intptr_t, cid_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(CaseInsensitiveCompareInstr,
                                          TemplateDefinition,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(CaseInsensitiveCompareInstr);
};

// Represents Math's static min and max functions.
class MathMinMaxInstr : public TemplateDefinition<2, NoThrow, Pure> {
 public:
  MathMinMaxInstr(MethodRecognizer::Kind op_kind,
                  Value* left_value,
                  Value* right_value,
                  intptr_t deopt_id,
                  intptr_t result_cid)
      : TemplateDefinition(deopt_id),
        op_kind_(op_kind),
        result_cid_(result_cid) {
    ASSERT((result_cid == kSmiCid) || (result_cid == kDoubleCid));
    SetInputAt(0, left_value);
    SetInputAt(1, right_value);
  }

  MethodRecognizer::Kind op_kind() const { return op_kind_; }

  Value* left() const { return inputs_[0]; }
  Value* right() const { return inputs_[1]; }

  intptr_t result_cid() const { return result_cid_; }

  virtual bool ComputeCanDeoptimize() const { return false; }

  virtual Representation representation() const {
    if (result_cid() == kSmiCid) {
      return kTagged;
    }
    ASSERT(result_cid() == kDoubleCid);
    return kUnboxedDouble;
  }

  virtual Representation RequiredInputRepresentation(intptr_t idx) const {
    if (result_cid() == kSmiCid) {
      return kTagged;
    }
    ASSERT(result_cid() == kDoubleCid);
    return kUnboxedDouble;
  }

  virtual intptr_t DeoptimizationTarget() const {
    // Direct access since this instruction cannot deoptimize, and the deopt-id
    // was inherited from another instruction that could deoptimize.
    return GetDeoptId();
  }

  DECLARE_INSTRUCTION(MathMinMax)
  virtual CompileType ComputeType() const;
  virtual bool AttributesEqual(const Instruction& other) const;

#define FIELD_LIST(F)                                                          \
  F(const MethodRecognizer::Kind, op_kind_)                                    \
  F(const intptr_t, result_cid_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(MathMinMaxInstr,
                                          TemplateDefinition,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(MathMinMaxInstr);
};

class BinaryDoubleOpInstr : public TemplateDefinition<2, NoThrow, Pure> {
 public:
  BinaryDoubleOpInstr(Token::Kind op_kind,
                      Value* left,
                      Value* right,
                      intptr_t deopt_id,
                      const InstructionSource& source,
                      SpeculativeMode speculative_mode = kGuardInputs)
      : TemplateDefinition(source, deopt_id),
        op_kind_(op_kind),
        token_pos_(source.token_pos),
        speculative_mode_(speculative_mode) {
    SetInputAt(0, left);
    SetInputAt(1, right);
  }

  Value* left() const { return inputs_[0]; }
  Value* right() const { return inputs_[1]; }

  Token::Kind op_kind() const { return op_kind_; }

  virtual TokenPosition token_pos() const { return token_pos_; }

  virtual bool ComputeCanDeoptimize() const { return false; }

  virtual Representation representation() const { return kUnboxedDouble; }

  virtual Representation RequiredInputRepresentation(intptr_t idx) const {
    ASSERT((idx == 0) || (idx == 1));
    return kUnboxedDouble;
  }

  virtual SpeculativeMode SpeculativeModeOfInput(intptr_t index) const {
    return speculative_mode_;
  }

  virtual intptr_t DeoptimizationTarget() const {
    // Direct access since this instruction cannot deoptimize, and the deopt-id
    // was inherited from another instruction that could deoptimize.
    return GetDeoptId();
  }

  PRINT_OPERANDS_TO_SUPPORT

  DECLARE_INSTRUCTION(BinaryDoubleOp)
  virtual CompileType ComputeType() const;

  virtual Definition* Canonicalize(FlowGraph* flow_graph);

  virtual bool AttributesEqual(const Instruction& other) const {
    auto const other_bin_op = other.AsBinaryDoubleOp();
    return (op_kind() == other_bin_op->op_kind()) &&
           (speculative_mode_ == other_bin_op->speculative_mode_);
  }

#define FIELD_LIST(F)                                                          \
  F(const Token::Kind, op_kind_)                                               \
  F(const TokenPosition, token_pos_)                                           \
  F(const SpeculativeMode, speculative_mode_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(BinaryDoubleOpInstr,
                                          TemplateDefinition,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(BinaryDoubleOpInstr);
};

class DoubleTestOpInstr : public TemplateComparison<1, NoThrow, Pure> {
 public:
  DoubleTestOpInstr(MethodRecognizer::Kind op_kind,
                    Value* value,
                    intptr_t deopt_id,
                    const InstructionSource& source)
      : TemplateComparison(source, Token::kEQ, deopt_id), op_kind_(op_kind) {
    SetInputAt(0, value);
  }

  Value* value() const { return InputAt(0); }

  MethodRecognizer::Kind op_kind() const { return op_kind_; }

  virtual bool ComputeCanDeoptimize() const { return false; }

  virtual Representation RequiredInputRepresentation(intptr_t idx) const {
    ASSERT(idx == 0);
    return kUnboxedDouble;
  }

  PRINT_OPERANDS_TO_SUPPORT

  DECLARE_COMPARISON_INSTRUCTION(DoubleTestOp)

  virtual CompileType ComputeType() const;

  virtual Definition* Canonicalize(FlowGraph* flow_graph);

  virtual bool AttributesEqual(const Instruction& other) const {
    return op_kind_ == other.AsDoubleTestOp()->op_kind() &&
           ComparisonInstr::AttributesEqual(other);
  }

  virtual ComparisonInstr* CopyWithNewOperands(Value* left, Value* right);

#define FIELD_LIST(F) F(const MethodRecognizer::Kind, op_kind_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(DoubleTestOpInstr,
                                          TemplateComparison,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(DoubleTestOpInstr);
};

class HashDoubleOpInstr : public TemplateDefinition<1, NoThrow, Pure> {
 public:
  HashDoubleOpInstr(Value* value, intptr_t deopt_id)
      : TemplateDefinition(deopt_id) {
    SetInputAt(0, value);
  }

  static HashDoubleOpInstr* Create(Value* value, intptr_t deopt_id) {
    return new HashDoubleOpInstr(value, deopt_id);
  }

  Value* value() const { return inputs_[0]; }

  virtual intptr_t DeoptimizationTarget() const {
    // Direct access since this instruction cannot deoptimize, and the deopt-id
    // was inherited from another instruction that could deoptimize.
    return GetDeoptId();
  }

  virtual Representation representation() const { return kUnboxedInt64; }

  virtual Representation RequiredInputRepresentation(intptr_t idx) const {
    ASSERT(idx == 0);
    return kUnboxedDouble;
  }

  DECLARE_INSTRUCTION(HashDoubleOp)

  virtual bool ComputeCanDeoptimize() const { return false; }

  virtual CompileType ComputeType() const { return CompileType::Smi(); }

  virtual bool AttributesEqual(const Instruction& other) const { return true; }

  DECLARE_EMPTY_SERIALIZATION(HashDoubleOpInstr, TemplateDefinition)

 private:
  DISALLOW_COPY_AND_ASSIGN(HashDoubleOpInstr);
};

class HashIntegerOpInstr : public TemplateDefinition<1, NoThrow, Pure> {
 public:
  HashIntegerOpInstr(Value* value, bool smi, intptr_t deopt_id)
      : TemplateDefinition(deopt_id), smi_(smi) {
    SetInputAt(0, value);
  }

  static HashIntegerOpInstr* Create(Value* value, bool smi, intptr_t deopt_id) {
    return new HashIntegerOpInstr(value, smi, deopt_id);
  }

  Value* value() const { return inputs_[0]; }

  virtual intptr_t DeoptimizationTarget() const {
    // Direct access since this instruction cannot deoptimize, and the deopt-id
    // was inherited from another instruction that could deoptimize.
    return GetDeoptId();
  }

  virtual Representation representation() const { return kTagged; }

  virtual Representation RequiredInputRepresentation(intptr_t idx) const {
    ASSERT(idx == 0);
    return kTagged;
  }

  DECLARE_INSTRUCTION(HashIntegerOp)

  virtual bool ComputeCanDeoptimize() const { return false; }

  virtual CompileType ComputeType() const { return CompileType::Smi(); }

  virtual bool AttributesEqual(const Instruction& other) const { return true; }

#define FIELD_LIST(F) F(const bool, smi_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(HashIntegerOpInstr,
                                          TemplateDefinition,
                                          FIELD_LIST)
#undef FIELD_LIST

  PRINT_OPERANDS_TO_SUPPORT

 private:
  DISALLOW_COPY_AND_ASSIGN(HashIntegerOpInstr);
};

class UnaryIntegerOpInstr : public TemplateDefinition<1, NoThrow, Pure> {
 public:
  UnaryIntegerOpInstr(Token::Kind op_kind, Value* value, intptr_t deopt_id)
      : TemplateDefinition(deopt_id), op_kind_(op_kind) {
    ASSERT((op_kind == Token::kNEGATE) || (op_kind == Token::kBIT_NOT));
    SetInputAt(0, value);
  }

  static UnaryIntegerOpInstr* Make(Representation representation,
                                   Token::Kind op_kind,
                                   Value* value,
                                   intptr_t deopt_id,
                                   Range* range);

  Value* value() const { return inputs_[0]; }
  Token::Kind op_kind() const { return op_kind_; }

  virtual bool AttributesEqual(const Instruction& other) const {
    return other.AsUnaryIntegerOp()->op_kind() == op_kind();
  }

  virtual intptr_t DeoptimizationTarget() const {
    // Direct access since this instruction cannot deoptimize, and the deopt-id
    // was inherited from another instruction that could deoptimize.
    return GetDeoptId();
  }

  PRINT_OPERANDS_TO_SUPPORT

  DECLARE_ABSTRACT_INSTRUCTION(UnaryIntegerOp)

#define FIELD_LIST(F) F(const Token::Kind, op_kind_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(UnaryIntegerOpInstr,
                                          TemplateDefinition,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(UnaryIntegerOpInstr);
};

// Handles both Smi operations: BIT_OR and NEGATE.
class UnarySmiOpInstr : public UnaryIntegerOpInstr {
 public:
  UnarySmiOpInstr(Token::Kind op_kind, Value* value, intptr_t deopt_id)
      : UnaryIntegerOpInstr(op_kind, value, deopt_id) {}

  virtual bool ComputeCanDeoptimize() const {
    return op_kind() == Token::kNEGATE;
  }

  virtual CompileType ComputeType() const;

  DECLARE_INSTRUCTION(UnarySmiOp)

  DECLARE_EMPTY_SERIALIZATION(UnarySmiOpInstr, UnaryIntegerOpInstr)

 private:
  DISALLOW_COPY_AND_ASSIGN(UnarySmiOpInstr);
};

class UnaryUint32OpInstr : public UnaryIntegerOpInstr {
 public:
  UnaryUint32OpInstr(Token::Kind op_kind, Value* value, intptr_t deopt_id)
      : UnaryIntegerOpInstr(op_kind, value, deopt_id) {
    ASSERT(IsSupported(op_kind));
  }

  virtual bool ComputeCanDeoptimize() const { return false; }

  virtual CompileType ComputeType() const;

  virtual Representation representation() const { return kUnboxedUint32; }

  virtual Representation RequiredInputRepresentation(intptr_t idx) const {
    ASSERT(idx == 0);
    return kUnboxedUint32;
  }

  static bool IsSupported(Token::Kind op_kind) {
    return op_kind == Token::kBIT_NOT;
  }

  DECLARE_INSTRUCTION(UnaryUint32Op)

  DECLARE_EMPTY_SERIALIZATION(UnaryUint32OpInstr, UnaryIntegerOpInstr)

 private:
  DISALLOW_COPY_AND_ASSIGN(UnaryUint32OpInstr);
};

class UnaryInt64OpInstr : public UnaryIntegerOpInstr {
 public:
  UnaryInt64OpInstr(Token::Kind op_kind,
                    Value* value,
                    intptr_t deopt_id,
                    SpeculativeMode speculative_mode = kGuardInputs)
      : UnaryIntegerOpInstr(op_kind, value, deopt_id),
        speculative_mode_(speculative_mode) {
    ASSERT(op_kind == Token::kBIT_NOT || op_kind == Token::kNEGATE);
  }

  virtual bool ComputeCanDeoptimize() const { return false; }

  virtual CompileType ComputeType() const;

  virtual Representation representation() const { return kUnboxedInt64; }

  virtual Representation RequiredInputRepresentation(intptr_t idx) const {
    ASSERT(idx == 0);
    return kUnboxedInt64;
  }

  virtual bool AttributesEqual(const Instruction& other) const {
    auto const unary_op_other = other.AsUnaryInt64Op();
    return UnaryIntegerOpInstr::AttributesEqual(other) &&
           (speculative_mode_ == unary_op_other->speculative_mode_);
  }

  virtual SpeculativeMode SpeculativeModeOfInput(intptr_t index) const {
    return speculative_mode_;
  }

  DECLARE_INSTRUCTION(UnaryInt64Op)

#define FIELD_LIST(F) F(const SpeculativeMode, speculative_mode_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(UnaryInt64OpInstr,
                                          UnaryIntegerOpInstr,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(UnaryInt64OpInstr);
};

class BinaryIntegerOpInstr : public TemplateDefinition<2, NoThrow, Pure> {
 public:
  BinaryIntegerOpInstr(Token::Kind op_kind,
                       Value* left,
                       Value* right,
                       intptr_t deopt_id)
      : TemplateDefinition(deopt_id),
        op_kind_(op_kind),
        can_overflow_(true),
        is_truncating_(false) {
    SetInputAt(0, left);
    SetInputAt(1, right);
  }

  static BinaryIntegerOpInstr* Make(
      Representation representation,
      Token::Kind op_kind,
      Value* left,
      Value* right,
      intptr_t deopt_id,
      SpeculativeMode speculative_mode = kGuardInputs);

  static BinaryIntegerOpInstr* Make(
      Representation representation,
      Token::Kind op_kind,
      Value* left,
      Value* right,
      intptr_t deopt_id,
      bool can_overflow,
      bool is_truncating,
      Range* range,
      SpeculativeMode speculative_mode = kGuardInputs);

  Token::Kind op_kind() const { return op_kind_; }
  Value* left() const { return inputs_[0]; }
  Value* right() const { return inputs_[1]; }

  bool can_overflow() const { return can_overflow_; }
  void set_can_overflow(bool overflow) {
    ASSERT(!is_truncating_ || !overflow);
    can_overflow_ = overflow;
  }

  bool is_truncating() const { return is_truncating_; }
  void mark_truncating() {
    is_truncating_ = true;
    set_can_overflow(false);
  }

  // Returns true if right is a non-zero Smi constant which absolute value is
  // a power of two.
  bool RightIsPowerOfTwoConstant() const;

  virtual Definition* Canonicalize(FlowGraph* flow_graph);

  virtual bool AttributesEqual(const Instruction& other) const;

  virtual intptr_t DeoptimizationTarget() const { return GetDeoptId(); }

  virtual void InferRange(RangeAnalysis* analysis, Range* range);

  PRINT_OPERANDS_TO_SUPPORT

  DECLARE_ABSTRACT_INSTRUCTION(BinaryIntegerOp)

#define FIELD_LIST(F)                                                          \
  F(const Token::Kind, op_kind_)                                               \
  F(bool, can_overflow_)                                                       \
  F(bool, is_truncating_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(BinaryIntegerOpInstr,
                                          TemplateDefinition,
                                          FIELD_LIST)
#undef FIELD_LIST

 protected:
  void InferRangeHelper(const Range* left_range,
                        const Range* right_range,
                        Range* range);

 private:
  Definition* CreateConstantResult(FlowGraph* graph, const Integer& result);

  DISALLOW_COPY_AND_ASSIGN(BinaryIntegerOpInstr);
};

class BinarySmiOpInstr : public BinaryIntegerOpInstr {
 public:
  BinarySmiOpInstr(Token::Kind op_kind,
                   Value* left,
                   Value* right,
                   intptr_t deopt_id,
                   // Provided by BinaryIntegerOpInstr::Make for constant RHS.
                   Range* right_range = nullptr)
      : BinaryIntegerOpInstr(op_kind, left, right, deopt_id),
        right_range_(right_range) {}

  virtual bool ComputeCanDeoptimize() const;

  virtual void InferRange(RangeAnalysis* analysis, Range* range);
  virtual CompileType ComputeType() const;

  DECLARE_INSTRUCTION(BinarySmiOp)

  Range* right_range() const { return right_range_; }

#define FIELD_LIST(F) F(Range*, right_range_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(BinarySmiOpInstr,
                                          BinaryIntegerOpInstr,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(BinarySmiOpInstr);
};

class BinaryInt32OpInstr : public BinaryIntegerOpInstr {
 public:
  BinaryInt32OpInstr(Token::Kind op_kind,
                     Value* left,
                     Value* right,
                     intptr_t deopt_id)
      : BinaryIntegerOpInstr(op_kind, left, right, deopt_id) {
    SetInputAt(0, left);
    SetInputAt(1, right);
  }

  static bool IsSupported(Token::Kind op_kind, Value* left, Value* right) {
#if defined(TARGET_ARCH_IS_32_BIT)
    switch (op_kind) {
      case Token::kADD:
      case Token::kSUB:
      case Token::kMUL:
      case Token::kBIT_AND:
      case Token::kBIT_OR:
      case Token::kBIT_XOR:
        return true;

      case Token::kSHL:
      case Token::kSHR:
      case Token::kUSHR:
        if (right->BindsToConstant() && right->BoundConstant().IsSmi()) {
          const intptr_t value = Smi::Cast(right->BoundConstant()).Value();
          return 0 <= value && value < kBitsPerWord;
        }
        return false;

      default:
        return false;
    }
#else
    return false;
#endif
  }

  virtual bool ComputeCanDeoptimize() const;

  virtual Representation representation() const { return kUnboxedInt32; }

  virtual Representation RequiredInputRepresentation(intptr_t idx) const {
    ASSERT((idx == 0) || (idx == 1));
    return kUnboxedInt32;
  }

  virtual CompileType ComputeType() const;

  DECLARE_INSTRUCTION(BinaryInt32Op)

  DECLARE_EMPTY_SERIALIZATION(BinaryInt32OpInstr, BinaryIntegerOpInstr)

 private:
  DISALLOW_COPY_AND_ASSIGN(BinaryInt32OpInstr);
};

class BinaryUint32OpInstr : public BinaryIntegerOpInstr {
 public:
  BinaryUint32OpInstr(Token::Kind op_kind,
                      Value* left,
                      Value* right,
                      intptr_t deopt_id)
      : BinaryIntegerOpInstr(op_kind, left, right, deopt_id) {
    mark_truncating();
    ASSERT(IsSupported(op_kind));
  }

  virtual bool ComputeCanDeoptimize() const { return false; }

  virtual Representation representation() const { return kUnboxedUint32; }

  virtual Representation RequiredInputRepresentation(intptr_t idx) const {
    ASSERT((idx == 0) || (idx == 1));
    return kUnboxedUint32;
  }

  virtual CompileType ComputeType() const;

  static bool IsSupported(Token::Kind op_kind) {
    switch (op_kind) {
      case Token::kADD:
      case Token::kSUB:
      case Token::kMUL:
      case Token::kBIT_AND:
      case Token::kBIT_OR:
      case Token::kBIT_XOR:
        return true;
      default:
        return false;
    }
  }

  DECLARE_INSTRUCTION(BinaryUint32Op)

  DECLARE_EMPTY_SERIALIZATION(BinaryUint32OpInstr, BinaryIntegerOpInstr)

 private:
  DISALLOW_COPY_AND_ASSIGN(BinaryUint32OpInstr);
};

class BinaryInt64OpInstr : public BinaryIntegerOpInstr {
 public:
  BinaryInt64OpInstr(Token::Kind op_kind,
                     Value* left,
                     Value* right,
                     intptr_t deopt_id,
                     SpeculativeMode speculative_mode = kGuardInputs)
      : BinaryIntegerOpInstr(op_kind, left, right, deopt_id),
        speculative_mode_(speculative_mode) {
    mark_truncating();
  }

  virtual bool ComputeCanDeoptimize() const {
    ASSERT(!can_overflow());
    return false;
  }

  virtual bool MayThrow() const {
    return op_kind() == Token::kMOD || op_kind() == Token::kTRUNCDIV;
  }

  virtual Representation representation() const { return kUnboxedInt64; }

  virtual Representation RequiredInputRepresentation(intptr_t idx) const {
    ASSERT((idx == 0) || (idx == 1));
    return kUnboxedInt64;
  }

  virtual SpeculativeMode SpeculativeModeOfInput(intptr_t index) const {
    return speculative_mode_;
  }

  virtual bool AttributesEqual(const Instruction& other) const {
    return BinaryIntegerOpInstr::AttributesEqual(other) &&
           (speculative_mode_ == other.AsBinaryInt64Op()->speculative_mode_);
  }

  virtual CompileType ComputeType() const;

  DECLARE_INSTRUCTION(BinaryInt64Op)

#define FIELD_LIST(F) F(const SpeculativeMode, speculative_mode_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(BinaryInt64OpInstr,
                                          BinaryIntegerOpInstr,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(BinaryInt64OpInstr);
};

// Base class for integer shift operations.
class ShiftIntegerOpInstr : public BinaryIntegerOpInstr {
 public:
  ShiftIntegerOpInstr(Token::Kind op_kind,
                      Value* left,
                      Value* right,
                      intptr_t deopt_id,
                      // Provided by BinaryIntegerOpInstr::Make for constant RHS
                      Range* right_range = nullptr)
      : BinaryIntegerOpInstr(op_kind, left, right, deopt_id),
        shift_range_(right_range) {
    ASSERT((op_kind == Token::kSHL) || (op_kind == Token::kSHR) ||
           (op_kind == Token::kUSHR));
    mark_truncating();
  }

  Range* shift_range() const { return shift_range_; }

  // Set the range directly (takes ownership).
  void set_shift_range(Range* shift_range) { shift_range_ = shift_range; }

  virtual void InferRange(RangeAnalysis* analysis, Range* range);

  DECLARE_ABSTRACT_INSTRUCTION(ShiftIntegerOp)

#define FIELD_LIST(F) F(Range*, shift_range_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(ShiftIntegerOpInstr,
                                          BinaryIntegerOpInstr,
                                          FIELD_LIST)
#undef FIELD_LIST

 protected:
  static constexpr intptr_t kShiftCountLimit = 63;

  // Returns true if the shift amount is guaranteed to be in
  // [0..max] range.
  bool IsShiftCountInRange(int64_t max = kShiftCountLimit) const;

 private:
  DISALLOW_COPY_AND_ASSIGN(ShiftIntegerOpInstr);
};

// Non-speculative int64 shift. Takes 2 unboxed int64.
// Throws if right operand is negative.
class ShiftInt64OpInstr : public ShiftIntegerOpInstr {
 public:
  ShiftInt64OpInstr(Token::Kind op_kind,
                    Value* left,
                    Value* right,
                    intptr_t deopt_id,
                    Range* right_range = nullptr)
      : ShiftIntegerOpInstr(op_kind, left, right, deopt_id, right_range) {}

  virtual SpeculativeMode SpeculativeModeOfInput(intptr_t index) const {
    return kNotSpeculative;
  }
  virtual bool ComputeCanDeoptimize() const { return false; }
  virtual bool MayThrow() const { return !IsShiftCountInRange(); }

  virtual Representation representation() const { return kUnboxedInt64; }

  virtual Representation RequiredInputRepresentation(intptr_t idx) const {
    ASSERT((idx == 0) || (idx == 1));
    return kUnboxedInt64;
  }

  virtual CompileType ComputeType() const;

  DECLARE_INSTRUCTION(ShiftInt64Op)

  DECLARE_EMPTY_SERIALIZATION(ShiftInt64OpInstr, ShiftIntegerOpInstr)

 private:
  DISALLOW_COPY_AND_ASSIGN(ShiftInt64OpInstr);
};

// Speculative int64 shift. Takes unboxed int64 and smi.
// Deoptimizes if right operand is negative or greater than kShiftCountLimit.
class SpeculativeShiftInt64OpInstr : public ShiftIntegerOpInstr {
 public:
  SpeculativeShiftInt64OpInstr(Token::Kind op_kind,
                               Value* left,
                               Value* right,
                               intptr_t deopt_id,
                               Range* right_range = nullptr)
      : ShiftIntegerOpInstr(op_kind, left, right, deopt_id, right_range) {}

  virtual bool ComputeCanDeoptimize() const {
    ASSERT(!can_overflow());
    return !IsShiftCountInRange();
  }

  virtual Representation representation() const { return kUnboxedInt64; }

  virtual Representation RequiredInputRepresentation(intptr_t idx) const {
    ASSERT((idx == 0) || (idx == 1));
    return (idx == 0) ? kUnboxedInt64 : kTagged;
  }

  virtual CompileType ComputeType() const;

  DECLARE_INSTRUCTION(SpeculativeShiftInt64Op)

  DECLARE_EMPTY_SERIALIZATION(SpeculativeShiftInt64OpInstr, ShiftIntegerOpInstr)

 private:
  DISALLOW_COPY_AND_ASSIGN(SpeculativeShiftInt64OpInstr);
};

// Non-speculative uint32 shift. Takes unboxed uint32 and unboxed int64.
// Throws if right operand is negative.
class ShiftUint32OpInstr : public ShiftIntegerOpInstr {
 public:
  ShiftUint32OpInstr(Token::Kind op_kind,
                     Value* left,
                     Value* right,
                     intptr_t deopt_id,
                     Range* right_range = nullptr)
      : ShiftIntegerOpInstr(op_kind, left, right, deopt_id, right_range) {}

  virtual SpeculativeMode SpeculativeModeOfInput(intptr_t index) const {
    return kNotSpeculative;
  }
  virtual bool ComputeCanDeoptimize() const { return false; }
  virtual bool MayThrow() const { return true; }

  virtual Representation representation() const { return kUnboxedUint32; }

  virtual Representation RequiredInputRepresentation(intptr_t idx) const {
    ASSERT((idx == 0) || (idx == 1));
    return (idx == 0) ? kUnboxedUint32 : kUnboxedInt64;
  }

  virtual CompileType ComputeType() const;

  DECLARE_INSTRUCTION(ShiftUint32Op)

  DECLARE_EMPTY_SERIALIZATION(ShiftUint32OpInstr, ShiftIntegerOpInstr)

 private:
  static constexpr intptr_t kUint32ShiftCountLimit = 31;

  DISALLOW_COPY_AND_ASSIGN(ShiftUint32OpInstr);
};

// Speculative uint32 shift. Takes unboxed uint32 and smi.
// Deoptimizes if right operand is negative.
class SpeculativeShiftUint32OpInstr : public ShiftIntegerOpInstr {
 public:
  SpeculativeShiftUint32OpInstr(Token::Kind op_kind,
                                Value* left,
                                Value* right,
                                intptr_t deopt_id,
                                Range* right_range = nullptr)
      : ShiftIntegerOpInstr(op_kind, left, right, deopt_id, right_range) {}

  virtual bool ComputeCanDeoptimize() const { return !IsShiftCountInRange(); }

  virtual Representation representation() const { return kUnboxedUint32; }

  virtual Representation RequiredInputRepresentation(intptr_t idx) const {
    ASSERT((idx == 0) || (idx == 1));
    return (idx == 0) ? kUnboxedUint32 : kTagged;
  }

  DECLARE_INSTRUCTION(SpeculativeShiftUint32Op)

  virtual CompileType ComputeType() const;

  DECLARE_EMPTY_SERIALIZATION(SpeculativeShiftUint32OpInstr,
                              ShiftIntegerOpInstr)

 private:
  static constexpr intptr_t kUint32ShiftCountLimit = 31;

  DISALLOW_COPY_AND_ASSIGN(SpeculativeShiftUint32OpInstr);
};

// Handles only NEGATE.
class UnaryDoubleOpInstr : public TemplateDefinition<1, NoThrow, Pure> {
 public:
  UnaryDoubleOpInstr(Token::Kind op_kind,
                     Value* value,
                     intptr_t deopt_id,
                     SpeculativeMode speculative_mode = kGuardInputs)
      : TemplateDefinition(deopt_id),
        op_kind_(op_kind),
        speculative_mode_(speculative_mode) {
    ASSERT(op_kind == Token::kNEGATE);
    SetInputAt(0, value);
  }

  Value* value() const { return inputs_[0]; }
  Token::Kind op_kind() const { return op_kind_; }

  DECLARE_INSTRUCTION(UnaryDoubleOp)
  virtual CompileType ComputeType() const;

  virtual bool ComputeCanDeoptimize() const { return false; }

  virtual intptr_t DeoptimizationTarget() const {
    // Direct access since this instruction cannot deoptimize, and the deopt-id
    // was inherited from another instruction that could deoptimize.
    return GetDeoptId();
  }

  virtual Representation representation() const { return kUnboxedDouble; }

  virtual Representation RequiredInputRepresentation(intptr_t idx) const {
    ASSERT(idx == 0);
    return kUnboxedDouble;
  }

  virtual SpeculativeMode SpeculativeModeOfInput(intptr_t index) const {
    return speculative_mode_;
  }

  virtual bool AttributesEqual(const Instruction& other) const {
    return speculative_mode_ == other.AsUnaryDoubleOp()->speculative_mode_;
  }

  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F)                                                          \
  F(const Token::Kind, op_kind_)                                               \
  F(const SpeculativeMode, speculative_mode_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(UnaryDoubleOpInstr,
                                          TemplateDefinition,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(UnaryDoubleOpInstr);
};

class CheckStackOverflowInstr : public TemplateInstruction<0, NoThrow> {
 public:
  enum Kind {
    // kOsrAndPreemption stack overflow checks are emitted in both unoptimized
    // and optimized versions of the code and they serve as both preemption and
    // OSR entry points.
    kOsrAndPreemption,

    // kOsrOnly stack overflow checks are only needed in the unoptimized code
    // because we can't OSR optimized code.
    kOsrOnly,
  };

  CheckStackOverflowInstr(const InstructionSource& source,
                          intptr_t stack_depth,
                          intptr_t loop_depth,
                          intptr_t deopt_id,
                          Kind kind)
      : TemplateInstruction(source, deopt_id),
        token_pos_(source.token_pos),
        stack_depth_(stack_depth),
        loop_depth_(loop_depth),
        kind_(kind) {
    ASSERT(kind != kOsrOnly || loop_depth > 0);
  }

  virtual TokenPosition token_pos() const { return token_pos_; }
  bool in_loop() const { return loop_depth_ > 0; }
  intptr_t stack_depth() const { return stack_depth_; }
  intptr_t loop_depth() const { return loop_depth_; }

  DECLARE_INSTRUCTION(CheckStackOverflow)

  virtual bool ComputeCanDeoptimize() const { return false; }
  virtual bool ComputeCanDeoptimizeAfterCall() const {
    return !CompilerState::Current().is_aot();
  }

  virtual Instruction* Canonicalize(FlowGraph* flow_graph);

  virtual bool HasUnknownSideEffects() const { return false; }

  virtual bool UseSharedSlowPathStub(bool is_optimizing) const {
    return SlowPathSharingSupported(is_optimizing);
  }

  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F)                                                          \
  F(const TokenPosition, token_pos_)                                           \
  F(const intptr_t, stack_depth_)                                              \
  F(const intptr_t, loop_depth_)                                               \
  F(const Kind, kind_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(CheckStackOverflowInstr,
                                          TemplateInstruction,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(CheckStackOverflowInstr);
};

// TODO(vegorov): remove this instruction in favor of Int32ToDouble.
class SmiToDoubleInstr : public TemplateDefinition<1, NoThrow, Pure> {
 public:
  SmiToDoubleInstr(Value* value, const InstructionSource& source)
      : TemplateDefinition(source), token_pos_(source.token_pos) {
    SetInputAt(0, value);
  }

  Value* value() const { return inputs_[0]; }
  virtual TokenPosition token_pos() const { return token_pos_; }

  DECLARE_INSTRUCTION(SmiToDouble)
  virtual CompileType ComputeType() const;

  virtual Representation representation() const { return kUnboxedDouble; }

  virtual bool ComputeCanDeoptimize() const { return false; }

  virtual bool AttributesEqual(const Instruction& other) const { return true; }

#define FIELD_LIST(F) F(const TokenPosition, token_pos_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(SmiToDoubleInstr,
                                          TemplateDefinition,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(SmiToDoubleInstr);
};

class Int32ToDoubleInstr : public TemplateDefinition<1, NoThrow, Pure> {
 public:
  explicit Int32ToDoubleInstr(Value* value) { SetInputAt(0, value); }

  Value* value() const { return inputs_[0]; }

  DECLARE_INSTRUCTION(Int32ToDouble)
  virtual CompileType ComputeType() const;

  virtual Representation RequiredInputRepresentation(intptr_t index) const {
    ASSERT(index == 0);
    return kUnboxedInt32;
  }

  virtual Representation representation() const { return kUnboxedDouble; }

  virtual bool ComputeCanDeoptimize() const { return false; }

  virtual bool AttributesEqual(const Instruction& other) const { return true; }

  DECLARE_EMPTY_SERIALIZATION(Int32ToDoubleInstr, TemplateDefinition)

 private:
  DISALLOW_COPY_AND_ASSIGN(Int32ToDoubleInstr);
};

class Int64ToDoubleInstr : public TemplateDefinition<1, NoThrow, Pure> {
 public:
  Int64ToDoubleInstr(Value* value,
                     intptr_t deopt_id,
                     SpeculativeMode speculative_mode = kGuardInputs)
      : TemplateDefinition(deopt_id), speculative_mode_(speculative_mode) {
    SetInputAt(0, value);
  }

  Value* value() const { return inputs_[0]; }

  DECLARE_INSTRUCTION(Int64ToDouble)
  virtual CompileType ComputeType() const;

  virtual Representation RequiredInputRepresentation(intptr_t index) const {
    ASSERT(index == 0);
    return kUnboxedInt64;
  }

  virtual Representation representation() const { return kUnboxedDouble; }

  virtual intptr_t DeoptimizationTarget() const {
    // Direct access since this instruction cannot deoptimize, and the deopt-id
    // was inherited from another instruction that could deoptimize.
    return GetDeoptId();
  }

  virtual bool ComputeCanDeoptimize() const { return false; }

  virtual SpeculativeMode SpeculativeModeOfInput(intptr_t index) const {
    return speculative_mode_;
  }

  virtual bool AttributesEqual(const Instruction& other) const {
    return speculative_mode_ == other.AsInt64ToDouble()->speculative_mode_;
  }

#define FIELD_LIST(F) F(const SpeculativeMode, speculative_mode_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(Int64ToDoubleInstr,
                                          TemplateDefinition,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(Int64ToDoubleInstr);
};

class DoubleToIntegerInstr : public TemplateDefinition<1, Throws, Pure> {
 public:
  DoubleToIntegerInstr(Value* value,
                       MethodRecognizer::Kind recognized_kind,
                       intptr_t deopt_id)
      : TemplateDefinition(deopt_id), recognized_kind_(recognized_kind) {
    ASSERT((recognized_kind == MethodRecognizer::kDoubleToInteger) ||
           (recognized_kind == MethodRecognizer::kDoubleFloorToInt) ||
           (recognized_kind == MethodRecognizer::kDoubleCeilToInt));
    SetInputAt(0, value);
  }

  Value* value() const { return inputs_[0]; }

  MethodRecognizer::Kind recognized_kind() const { return recognized_kind_; }

  DECLARE_INSTRUCTION(DoubleToInteger)
  virtual CompileType ComputeType() const;

  virtual Representation RequiredInputRepresentation(intptr_t idx) const {
    ASSERT(idx == 0);
    return kUnboxedDouble;
  }

  virtual SpeculativeMode SpeculativeModeOfInput(intptr_t idx) const {
    ASSERT(idx == 0);
    return kNotSpeculative;
  }

  virtual bool ComputeCanDeoptimize() const {
    return !CompilerState::Current().is_aot();
  }

  virtual intptr_t DeoptimizationTarget() const { return GetDeoptId(); }

  virtual bool HasUnknownSideEffects() const { return false; }

  virtual bool AttributesEqual(const Instruction& other) const {
    return other.AsDoubleToInteger()->recognized_kind() == recognized_kind();
  }

#define FIELD_LIST(F) F(const MethodRecognizer::Kind, recognized_kind_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(DoubleToIntegerInstr,
                                          TemplateDefinition,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(DoubleToIntegerInstr);
};

// Similar to 'DoubleToIntegerInstr' but expects unboxed double as input
// and creates a Smi.
class DoubleToSmiInstr : public TemplateDefinition<1, NoThrow, Pure> {
 public:
  DoubleToSmiInstr(Value* value, intptr_t deopt_id)
      : TemplateDefinition(deopt_id) {
    SetInputAt(0, value);
  }

  Value* value() const { return inputs_[0]; }

  DECLARE_INSTRUCTION(DoubleToSmi)
  virtual CompileType ComputeType() const;

  virtual bool ComputeCanDeoptimize() const { return true; }

  virtual Representation RequiredInputRepresentation(intptr_t idx) const {
    ASSERT(idx == 0);
    return kUnboxedDouble;
  }

  virtual intptr_t DeoptimizationTarget() const { return GetDeoptId(); }

  virtual bool AttributesEqual(const Instruction& other) const { return true; }

  DECLARE_EMPTY_SERIALIZATION(DoubleToSmiInstr, TemplateDefinition)

 private:
  DISALLOW_COPY_AND_ASSIGN(DoubleToSmiInstr);
};

class DoubleToDoubleInstr : public TemplateDefinition<1, NoThrow, Pure> {
 public:
  DoubleToDoubleInstr(Value* value,
                      MethodRecognizer::Kind recognized_kind,
                      intptr_t deopt_id)
      : TemplateDefinition(deopt_id), recognized_kind_(recognized_kind) {
    ASSERT((recognized_kind == MethodRecognizer::kDoubleTruncateToDouble) ||
           (recognized_kind == MethodRecognizer::kDoubleFloorToDouble) ||
           (recognized_kind == MethodRecognizer::kDoubleCeilToDouble));
    SetInputAt(0, value);
  }

  Value* value() const { return inputs_[0]; }

  MethodRecognizer::Kind recognized_kind() const { return recognized_kind_; }

  DECLARE_INSTRUCTION(DoubleToDouble)
  virtual CompileType ComputeType() const;

  virtual bool ComputeCanDeoptimize() const { return false; }

  virtual Representation representation() const { return kUnboxedDouble; }

  virtual Representation RequiredInputRepresentation(intptr_t idx) const {
    ASSERT(idx == 0);
    return kUnboxedDouble;
  }

  virtual SpeculativeMode SpeculativeModeOfInput(intptr_t idx) const {
    ASSERT(idx == 0);
    return kNotSpeculative;
  }

  virtual intptr_t DeoptimizationTarget() const { return GetDeoptId(); }

  virtual bool AttributesEqual(const Instruction& other) const {
    return other.AsDoubleToDouble()->recognized_kind() == recognized_kind();
  }

#define FIELD_LIST(F) F(const MethodRecognizer::Kind, recognized_kind_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(DoubleToDoubleInstr,
                                          TemplateDefinition,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(DoubleToDoubleInstr);
};

class DoubleToFloatInstr : public TemplateDefinition<1, NoThrow, Pure> {
 public:
  DoubleToFloatInstr(Value* value,
                     intptr_t deopt_id,
                     SpeculativeMode speculative_mode = kGuardInputs)
      : TemplateDefinition(deopt_id), speculative_mode_(speculative_mode) {
    SetInputAt(0, value);
  }

  Value* value() const { return inputs_[0]; }

  DECLARE_INSTRUCTION(DoubleToFloat)

  virtual CompileType ComputeType() const;

  virtual bool ComputeCanDeoptimize() const { return false; }

  virtual Representation representation() const { return kUnboxedFloat; }

  virtual Representation RequiredInputRepresentation(intptr_t idx) const {
    ASSERT(idx == 0);
    return kUnboxedDouble;
  }

  virtual SpeculativeMode SpeculativeModeOfInput(intptr_t index) const {
    return speculative_mode_;
  }

  virtual intptr_t DeoptimizationTarget() const { return GetDeoptId(); }

  virtual bool AttributesEqual(const Instruction& other) const { return true; }

  virtual Definition* Canonicalize(FlowGraph* flow_graph);

#define FIELD_LIST(F) F(const SpeculativeMode, speculative_mode_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(DoubleToFloatInstr,
                                          TemplateDefinition,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(DoubleToFloatInstr);
};

class FloatToDoubleInstr : public TemplateDefinition<1, NoThrow, Pure> {
 public:
  FloatToDoubleInstr(Value* value, intptr_t deopt_id)
      : TemplateDefinition(deopt_id) {
    SetInputAt(0, value);
  }

  Value* value() const { return inputs_[0]; }

  DECLARE_INSTRUCTION(FloatToDouble)

  virtual CompileType ComputeType() const;

  virtual bool ComputeCanDeoptimize() const { return false; }

  virtual Representation representation() const { return kUnboxedDouble; }

  virtual Representation RequiredInputRepresentation(intptr_t idx) const {
    ASSERT(idx == 0);
    return kUnboxedFloat;
  }

  virtual intptr_t DeoptimizationTarget() const { return GetDeoptId(); }

  virtual bool AttributesEqual(const Instruction& other) const { return true; }

  virtual Definition* Canonicalize(FlowGraph* flow_graph);

  DECLARE_EMPTY_SERIALIZATION(FloatToDoubleInstr, TemplateDefinition)

 private:
  DISALLOW_COPY_AND_ASSIGN(FloatToDoubleInstr);
};

// TODO(sjindel): Replace with FFICallInstr.
class InvokeMathCFunctionInstr : public VariadicDefinition {
 public:
  InvokeMathCFunctionInstr(InputsArray&& inputs,
                           intptr_t deopt_id,
                           MethodRecognizer::Kind recognized_kind,
                           const InstructionSource& source);

  static intptr_t ArgumentCountFor(MethodRecognizer::Kind recognized_kind_);

  const RuntimeEntry& TargetFunction() const;

  MethodRecognizer::Kind recognized_kind() const { return recognized_kind_; }

  virtual TokenPosition token_pos() const { return token_pos_; }

  DECLARE_INSTRUCTION(InvokeMathCFunction)
  virtual CompileType ComputeType() const;

  virtual bool ComputeCanDeoptimize() const { return false; }

  virtual Representation representation() const { return kUnboxedDouble; }

  virtual Representation RequiredInputRepresentation(intptr_t idx) const {
    ASSERT((0 <= idx) && (idx < InputCount()));
    return kUnboxedDouble;
  }

  virtual SpeculativeMode SpeculativeModeOfInput(intptr_t idx) const {
    ASSERT((0 <= idx) && (idx < InputCount()));
    return kNotSpeculative;
  }

  virtual intptr_t DeoptimizationTarget() const { return GetDeoptId(); }

  virtual bool AllowsCSE() const { return true; }
  virtual bool HasUnknownSideEffects() const { return false; }

  virtual bool AttributesEqual(const Instruction& other) const {
    auto const other_invoke = other.AsInvokeMathCFunction();
    return other_invoke->recognized_kind() == recognized_kind();
  }

  virtual bool MayThrow() const { return false; }

  static constexpr intptr_t kSavedSpTempIndex = 0;
  static constexpr intptr_t kObjectTempIndex = 1;
  static constexpr intptr_t kDoubleTempIndex = 2;

  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F)                                                          \
  F(const MethodRecognizer::Kind, recognized_kind_)                            \
  F(const TokenPosition, token_pos_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(InvokeMathCFunctionInstr,
                                          VariadicDefinition,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(InvokeMathCFunctionInstr);
};

class ExtractNthOutputInstr : public TemplateDefinition<1, NoThrow, Pure> {
 public:
  // Extract the Nth output register from value.
  ExtractNthOutputInstr(Value* value,
                        intptr_t n,
                        Representation definition_rep,
                        intptr_t definition_cid)
      : index_(n),
        definition_rep_(definition_rep),
        definition_cid_(definition_cid) {
    SetInputAt(0, value);
  }

  Value* value() const { return inputs_[0]; }

  DECLARE_INSTRUCTION(ExtractNthOutput)
  DECLARE_ATTRIBUTES(index())

  virtual CompileType ComputeType() const;
  virtual bool ComputeCanDeoptimize() const { return false; }

  intptr_t index() const { return index_; }

  virtual Representation representation() const { return definition_rep_; }

  virtual Representation RequiredInputRepresentation(intptr_t idx) const {
    ASSERT(idx == 0);
    if (representation() == kTagged) {
      return kPairOfTagged;
    }
    UNREACHABLE();
    return definition_rep_;
  }

  virtual bool AttributesEqual(const Instruction& other) const {
    auto const other_extract = other.AsExtractNthOutput();
    return (other_extract->representation() == representation()) &&
           (other_extract->index() == index());
  }

  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F)                                                          \
  F(const intptr_t, index_)                                                    \
  F(const Representation, definition_rep_)                                     \
  F(const intptr_t, definition_cid_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(ExtractNthOutputInstr,
                                          TemplateDefinition,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(ExtractNthOutputInstr);
};

// Combines 2 values into a pair with kPairOfTagged representation.
class MakePairInstr : public TemplateDefinition<2, NoThrow, Pure> {
 public:
  MakePairInstr(Value* x, Value* y) {
    SetInputAt(0, x);
    SetInputAt(1, y);
  }

  DECLARE_INSTRUCTION(MakePair)

  virtual CompileType ComputeType() const;
  virtual bool ComputeCanDeoptimize() const { return false; }

  virtual Representation representation() const { return kPairOfTagged; }

  virtual Representation RequiredInputRepresentation(intptr_t idx) const {
    ASSERT((0 <= idx) && (idx < InputCount()));
    return kTagged;
  }

  virtual bool AttributesEqual(const Instruction& other) const { return true; }

  DECLARE_EMPTY_SERIALIZATION(MakePairInstr, TemplateDefinition)

 private:
  DISALLOW_COPY_AND_ASSIGN(MakePairInstr);
};

class TruncDivModInstr : public TemplateDefinition<2, NoThrow, Pure> {
 public:
  TruncDivModInstr(Value* lhs, Value* rhs, intptr_t deopt_id);

  static intptr_t OutputIndexOf(Token::Kind token);

  virtual CompileType ComputeType() const;

  virtual bool ComputeCanDeoptimize() const { return true; }

  virtual Representation representation() const { return kPairOfTagged; }

  virtual Representation RequiredInputRepresentation(intptr_t idx) const {
    ASSERT((0 <= idx) && (idx < InputCount()));
    return kTagged;
  }

  virtual intptr_t DeoptimizationTarget() const { return GetDeoptId(); }

  DECLARE_INSTRUCTION(TruncDivMod)

  virtual bool AttributesEqual(const Instruction& other) const { return true; }

  PRINT_OPERANDS_TO_SUPPORT

  DECLARE_EMPTY_SERIALIZATION(TruncDivModInstr, TemplateDefinition)

 private:
  Range* divisor_range() const {
    // Note: this range is only used to remove check for zero divisor from
    // the emitted pattern. It is not used for deciding whether instruction
    // will deoptimize or not - that is why it is ok to access range of
    // the definition directly. Otherwise range analysis or another pass
    // needs to cache range of the divisor in the operation to prevent
    // bugs when range information gets out of sync with the final decision
    // whether some instruction can deoptimize or not made in
    // EliminateEnvironments().
    return InputAt(1)->definition()->range();
  }

  DISALLOW_COPY_AND_ASSIGN(TruncDivModInstr);
};

class CheckClassInstr : public TemplateInstruction<1, NoThrow> {
 public:
  CheckClassInstr(Value* value,
                  intptr_t deopt_id,
                  const Cids& cids,
                  const InstructionSource& source);

  DECLARE_INSTRUCTION(CheckClass)

  virtual bool ComputeCanDeoptimize() const { return true; }

  virtual TokenPosition token_pos() const { return token_pos_; }

  Value* value() const { return inputs_[0]; }

  const Cids& cids() const { return cids_; }

  virtual Instruction* Canonicalize(FlowGraph* flow_graph);

  bool IsNullCheck() const { return IsDeoptIfNull() || IsDeoptIfNotNull(); }

  bool IsDeoptIfNull() const;
  bool IsDeoptIfNotNull() const;

  bool IsBitTest() const;
  static bool IsCompactCidRange(const Cids& cids);
  intptr_t ComputeCidMask() const;

  virtual bool AllowsCSE() const { return true; }
  virtual bool HasUnknownSideEffects() const { return false; }

  virtual bool AttributesEqual(const Instruction& other) const;

  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F)                                                          \
  F(const Cids&, cids_)                                                        \
  F(bool, is_bit_test_)                                                        \
  F(const TokenPosition, token_pos_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(CheckClassInstr,
                                          TemplateInstruction,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  int EmitCheckCid(FlowGraphCompiler* compiler,
                   int bias,
                   intptr_t cid_start,
                   intptr_t cid_end,
                   bool is_last,
                   compiler::Label* is_ok,
                   compiler::Label* deopt,
                   bool use_near_jump);
  void EmitBitTest(FlowGraphCompiler* compiler,
                   intptr_t min,
                   intptr_t max,
                   intptr_t mask,
                   compiler::Label* deopt);
  void EmitNullCheck(FlowGraphCompiler* compiler, compiler::Label* deopt);

  DISALLOW_COPY_AND_ASSIGN(CheckClassInstr);
};

class CheckSmiInstr : public TemplateInstruction<1, NoThrow, Pure> {
 public:
  CheckSmiInstr(Value* value,
                intptr_t deopt_id,
                const InstructionSource& source)
      : TemplateInstruction(source, deopt_id), token_pos_(source.token_pos) {
    SetInputAt(0, value);
  }

  Value* value() const { return inputs_[0]; }
  virtual TokenPosition token_pos() const { return token_pos_; }

  DECLARE_INSTRUCTION(CheckSmi)

  virtual bool ComputeCanDeoptimize() const { return true; }

  virtual Instruction* Canonicalize(FlowGraph* flow_graph);

  virtual bool AttributesEqual(const Instruction& other) const { return true; }

#define FIELD_LIST(F) F(const TokenPosition, token_pos_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(CheckSmiInstr,
                                          TemplateInstruction,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(CheckSmiInstr);
};

// CheckNull instruction takes one input (`value`) and tests it for `null`.
// If `value` is `null`, then an exception is thrown according to
// `exception_type`. Otherwise, execution proceeds to the next instruction.
class CheckNullInstr : public TemplateDefinition<1, Throws, Pure> {
 public:
  enum ExceptionType {
    kNoSuchMethod,
    kArgumentError,
    kCastError,
  };

  CheckNullInstr(Value* value,
                 const String& function_name,
                 intptr_t deopt_id,
                 const InstructionSource& source,
                 ExceptionType exception_type = kNoSuchMethod)
      : TemplateDefinition(source, deopt_id),
        token_pos_(source.token_pos),
        function_name_(function_name),
        exception_type_(exception_type) {
    DEBUG_ASSERT(function_name.IsNotTemporaryScopedHandle());
    ASSERT(function_name.IsSymbol());
    SetInputAt(0, value);
  }

  Value* value() const { return inputs_[0]; }
  virtual TokenPosition token_pos() const { return token_pos_; }
  const String& function_name() const { return function_name_; }
  ExceptionType exception_type() const { return exception_type_; }

  virtual bool UseSharedSlowPathStub(bool is_optimizing) const {
    return SlowPathSharingSupported(is_optimizing);
  }

  DECLARE_INSTRUCTION(CheckNull)

  virtual CompileType ComputeType() const;
  virtual bool RecomputeType();

  // CheckNull can implicitly call Dart code (NoSuchMethodError constructor),
  // so it needs a deopt ID in optimized and unoptimized code.
  virtual bool ComputeCanDeoptimize() const { return false; }
  virtual bool ComputeCanDeoptimizeAfterCall() const {
    return !CompilerState::Current().is_aot();
  }
  virtual bool CanBecomeDeoptimizationTarget() const { return true; }

  virtual Definition* Canonicalize(FlowGraph* flow_graph);

  virtual bool AttributesEqual(const Instruction& other) const;

  static void AddMetadataForRuntimeCall(CheckNullInstr* check_null,
                                        FlowGraphCompiler* compiler);

  virtual Value* RedefinedValue() const;

  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F)                                                          \
  F(const TokenPosition, token_pos_)                                           \
  F(const String&, function_name_)                                             \
  F(const ExceptionType, exception_type_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(CheckNullInstr,
                                          TemplateDefinition,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(CheckNullInstr);
};

class CheckClassIdInstr : public TemplateInstruction<1, NoThrow> {
 public:
  CheckClassIdInstr(Value* value, CidRangeValue cids, intptr_t deopt_id)
      : TemplateInstruction(deopt_id), cids_(cids) {
    SetInputAt(0, value);
  }

  Value* value() const { return inputs_[0]; }
  const CidRangeValue& cids() const { return cids_; }

  DECLARE_INSTRUCTION(CheckClassId)

  virtual bool ComputeCanDeoptimize() const { return true; }

  virtual Instruction* Canonicalize(FlowGraph* flow_graph);

  virtual bool AllowsCSE() const { return true; }
  virtual bool HasUnknownSideEffects() const { return false; }

  virtual bool AttributesEqual(const Instruction& other) const {
    return other.Cast<CheckClassIdInstr>()->cids().Equals(cids_);
  }

  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F) F(CidRangeValue, cids_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(CheckClassIdInstr,
                                          TemplateInstruction,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  bool Contains(intptr_t cid) const;

  DISALLOW_COPY_AND_ASSIGN(CheckClassIdInstr);
};

// Base class for speculative [CheckArrayBoundInstr] and
// non-speculative [GenericCheckBoundInstr] bounds checking.
class CheckBoundBaseInstr : public TemplateDefinition<2, NoThrow, Pure> {
 public:
  CheckBoundBaseInstr(Value* length, Value* index, intptr_t deopt_id)
      : TemplateDefinition(deopt_id) {
    SetInputAt(kLengthPos, length);
    SetInputAt(kIndexPos, index);
  }

  Value* length() const { return inputs_[kLengthPos]; }
  Value* index() const { return inputs_[kIndexPos]; }

  virtual Definition* Canonicalize(FlowGraph* flow_graph);

  DECLARE_ABSTRACT_INSTRUCTION(CheckBoundBase);

  virtual Value* RedefinedValue() const;

  // Returns true if the bounds check can be eliminated without
  // changing the semantics (viz. 0 <= index < length).
  bool IsRedundant(bool use_loops = false);

  // Give a name to the location/input indices.
  enum { kLengthPos = 0, kIndexPos = 1 };

  DECLARE_EMPTY_SERIALIZATION(CheckBoundBaseInstr, TemplateDefinition)

 private:
  DISALLOW_COPY_AND_ASSIGN(CheckBoundBaseInstr);
};

// Performs an array bounds check, where
//   safe_index := CheckArrayBound(length, index)
// returns the "safe" index when
//   0 <= index < length
// or otherwise deoptimizes (viz. speculative).
class CheckArrayBoundInstr : public CheckBoundBaseInstr {
 public:
  CheckArrayBoundInstr(Value* length, Value* index, intptr_t deopt_id)
      : CheckBoundBaseInstr(length, index, deopt_id), generalized_(false) {}

  DECLARE_INSTRUCTION(CheckArrayBound)

  virtual CompileType ComputeType() const;
  virtual bool RecomputeType();

  virtual bool ComputeCanDeoptimize() const { return true; }

  void mark_generalized() { generalized_ = true; }

  // Returns the length offset for array and string types.
  static intptr_t LengthOffsetFor(intptr_t class_id);

  static bool IsFixedLengthArrayType(intptr_t class_id);

  virtual bool AttributesEqual(const Instruction& other) const { return true; }

#define FIELD_LIST(F) F(bool, generalized_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(CheckArrayBoundInstr,
                                          CheckBoundBaseInstr,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(CheckArrayBoundInstr);
};

// Performs an array bounds check, where
//   safe_index := GenericCheckBound(length, index)
// returns the "safe" index when
//   0 <= index < length
// or otherwise throws an out-of-bounds exception (viz. non-speculative).
class GenericCheckBoundInstr : public CheckBoundBaseInstr {
 public:
  // We prefer to have unboxed inputs on 64-bit where values can fit into a
  // register.
  static bool UseUnboxedRepresentation() {
    return compiler::target::kWordSize == 8;
  }

  GenericCheckBoundInstr(Value* length, Value* index, intptr_t deopt_id)
      : CheckBoundBaseInstr(length, index, deopt_id) {}

  virtual bool AttributesEqual(const Instruction& other) const { return true; }

  DECLARE_INSTRUCTION(GenericCheckBound)

  virtual CompileType ComputeType() const;
  virtual bool RecomputeType();

  virtual intptr_t DeoptimizationTarget() const { return DeoptId::kNone; }

  virtual SpeculativeMode SpeculativeModeOfInput(intptr_t index) const {
    return kNotSpeculative;
  }

  virtual Representation representation() const {
    return UseUnboxedRepresentation() ? kUnboxedInt64 : kTagged;
  }

  virtual Representation RequiredInputRepresentation(intptr_t idx) const {
    ASSERT(idx == kIndexPos || idx == kLengthPos);
    return UseUnboxedRepresentation() ? kUnboxedInt64 : kTagged;
  }

  // GenericCheckBound can implicitly call Dart code (RangeError or
  // ArgumentError constructor), so it can lazily deopt.
  virtual bool ComputeCanDeoptimize() const { return false; }
  virtual bool ComputeCanDeoptimizeAfterCall() const {
    return !CompilerState::Current().is_aot();
  }

  virtual bool MayThrow() const { return true; }

  virtual bool UseSharedSlowPathStub(bool is_optimizing) const {
    return SlowPathSharingSupported(is_optimizing);
  }

  DECLARE_EMPTY_SERIALIZATION(GenericCheckBoundInstr, CheckBoundBaseInstr)

 private:
  DISALLOW_COPY_AND_ASSIGN(GenericCheckBoundInstr);
};

class CheckWritableInstr : public TemplateDefinition<1, Throws, Pure> {
 public:
  CheckWritableInstr(Value* array,
                     intptr_t deopt_id,
                     const InstructionSource& source)
      : TemplateDefinition(source, deopt_id) {
    SetInputAt(0, array);
  }

  virtual bool AttributesEqual(const Instruction& other) const { return true; }

  DECLARE_INSTRUCTION(CheckWritable)

  Value* value() const { return inputs_[0]; }

  virtual Definition* Canonicalize(FlowGraph* flow_graph);

  virtual Value* RedefinedValue() const;

  virtual bool ComputeCanDeoptimize() const { return false; }

  DECLARE_EMPTY_SERIALIZATION(CheckWritableInstr, TemplateDefinition)

 private:
  DISALLOW_COPY_AND_ASSIGN(CheckWritableInstr);
};

// Instruction evaluates the given comparison and deoptimizes if it evaluates
// to false.
class CheckConditionInstr : public Instruction {
 public:
  CheckConditionInstr(ComparisonInstr* comparison, intptr_t deopt_id)
      : Instruction(deopt_id), comparison_(comparison) {
    ASSERT(comparison->ArgumentCount() == 0);
    ASSERT(comparison->env() == nullptr);
    for (intptr_t i = comparison->InputCount() - 1; i >= 0; --i) {
      comparison->InputAt(i)->set_instruction(this);
    }
  }

  ComparisonInstr* comparison() const { return comparison_; }

  DECLARE_INSTRUCTION(CheckCondition)

  virtual bool ComputeCanDeoptimize() const { return true; }

  virtual Instruction* Canonicalize(FlowGraph* flow_graph);

  virtual bool AllowsCSE() const { return true; }
  virtual bool HasUnknownSideEffects() const { return false; }

  virtual bool AttributesEqual(const Instruction& other) const {
    return other.AsCheckCondition()->comparison()->AttributesEqual(
        *comparison());
  }

  virtual intptr_t InputCount() const { return comparison()->InputCount(); }
  virtual Value* InputAt(intptr_t i) const { return comparison()->InputAt(i); }

  virtual bool MayThrow() const { return false; }

  virtual void CopyDeoptIdFrom(const Instruction& instr) {
    Instruction::CopyDeoptIdFrom(instr);
    comparison()->CopyDeoptIdFrom(instr);
  }

  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F) F(ComparisonInstr*, comparison_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(CheckConditionInstr,
                                          Instruction,
                                          FIELD_LIST)
#undef FIELD_LIST
  DECLARE_EXTRA_SERIALIZATION

 private:
  virtual void RawSetInputAt(intptr_t i, Value* value) {
    comparison()->RawSetInputAt(i, value);
  }

  DISALLOW_COPY_AND_ASSIGN(CheckConditionInstr);
};

class IntConverterInstr : public TemplateDefinition<1, NoThrow, Pure> {
 public:
  IntConverterInstr(Representation from,
                    Representation to,
                    Value* value,
                    intptr_t deopt_id)
      : TemplateDefinition(deopt_id),
        from_representation_(from),
        to_representation_(to),
        is_truncating_(to == kUnboxedUint32) {
    ASSERT(from != to);
    ASSERT(from == kUnboxedInt64 || from == kUnboxedUint32 ||
           from == kUnboxedInt32 || from == kUntagged);
    ASSERT(to == kUnboxedInt64 || to == kUnboxedUint32 || to == kUnboxedInt32 ||
           to == kUntagged);
    ASSERT(from != kUntagged ||
           (to == kUnboxedIntPtr || to == kUnboxedFfiIntPtr));
    ASSERT(to != kUntagged ||
           (from == kUnboxedIntPtr || from == kUnboxedFfiIntPtr));
    SetInputAt(0, value);
  }

  Value* value() const { return inputs_[0]; }

  Representation from() const { return from_representation_; }
  Representation to() const { return to_representation_; }
  bool is_truncating() const { return is_truncating_; }

  void mark_truncating() { is_truncating_ = true; }

  Definition* Canonicalize(FlowGraph* flow_graph);

  virtual bool ComputeCanDeoptimize() const;

  virtual Representation representation() const { return to(); }

  virtual Representation RequiredInputRepresentation(intptr_t idx) const {
    ASSERT(idx == 0);
    return from();
  }

  virtual bool AttributesEqual(const Instruction& other) const {
    ASSERT(other.IsIntConverter());
    auto const converter = other.AsIntConverter();
    return (converter->from() == from()) && (converter->to() == to()) &&
           (converter->is_truncating() == is_truncating());
  }

  virtual intptr_t DeoptimizationTarget() const { return GetDeoptId(); }

  virtual void InferRange(RangeAnalysis* analysis, Range* range);

  virtual CompileType ComputeType() const {
    // TODO(vegorov) use range information to improve type.
    return CompileType::Int();
  }

  DECLARE_INSTRUCTION(IntConverter);

  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F)                                                          \
  F(const Representation, from_representation_)                                \
  F(const Representation, to_representation_)                                  \
  F(bool, is_truncating_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(IntConverterInstr,
                                          TemplateDefinition,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(IntConverterInstr);
};

// Moves a floating-point value between CPU and FPU registers. Used to implement
// "softfp" calling conventions, where FPU arguments/return values are passed in
// normal CPU registers.
class BitCastInstr : public TemplateDefinition<1, NoThrow, Pure> {
 public:
  BitCastInstr(Representation from, Representation to, Value* value)
      : TemplateDefinition(DeoptId::kNone),
        from_representation_(from),
        to_representation_(to) {
    ASSERT(from != to);
    ASSERT((to == kUnboxedInt32 && from == kUnboxedFloat) ||
           (to == kUnboxedFloat && from == kUnboxedInt32) ||
           (to == kUnboxedInt64 && from == kUnboxedDouble) ||
           (to == kUnboxedDouble && from == kUnboxedInt64));
    SetInputAt(0, value);
  }

  Value* value() const { return inputs_[0]; }

  Representation from() const { return from_representation_; }
  Representation to() const { return to_representation_; }

  virtual bool ComputeCanDeoptimize() const { return false; }

  virtual Representation representation() const { return to(); }

  virtual Representation RequiredInputRepresentation(intptr_t idx) const {
    ASSERT(idx == 0);
    return from();
  }

  virtual bool AttributesEqual(const Instruction& other) const {
    ASSERT(other.IsBitCast());
    auto const converter = other.AsBitCast();
    return converter->from() == from() && converter->to() == to();
  }

  virtual CompileType ComputeType() const { return CompileType::Dynamic(); }

  DECLARE_INSTRUCTION(BitCast);

  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F)                                                          \
  F(const Representation, from_representation_)                                \
  F(const Representation, to_representation_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(BitCastInstr,
                                          TemplateDefinition,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(BitCastInstr);
};

class LoadThreadInstr : public TemplateDefinition<0, NoThrow, Pure> {
 public:
  LoadThreadInstr() : TemplateDefinition(DeoptId::kNone) {}

  virtual bool ComputeCanDeoptimize() const { return false; }

  virtual Representation representation() const { return kUntagged; }

  virtual Representation RequiredInputRepresentation(intptr_t idx) const {
    UNREACHABLE();
  }

  virtual CompileType ComputeType() const { return CompileType::Int(); }

  // CSE is allowed. The thread should always be the same value.
  virtual bool AttributesEqual(const Instruction& other) const {
    ASSERT(other.IsLoadThread());
    return true;
  }

  DECLARE_INSTRUCTION(LoadThread);

  DECLARE_EMPTY_SERIALIZATION(LoadThreadInstr, TemplateDefinition)

 private:
  DISALLOW_COPY_AND_ASSIGN(LoadThreadInstr);
};

// SimdOpInstr
//
// All SIMD intrinsics and recognized methods are represented via instances
// of SimdOpInstr, a particular type of SimdOp is selected by SimdOpInstr::Kind.
//
// Defines below are used to construct SIMD_OP_LIST - a list of all SIMD
// operations. SIMD_OP_LIST contains information such as arity, input types and
// output type for each SIMD op and is used to derive things like input
// and output representations, type of return value, etc.
//
// Lists of SIMD ops are defined using macro M, OP and BINARY_OP which are
// expected to have the following signature:
//
//          (Arity, HasMask, Name, (In_0, ..., In_Arity), Out)
//
// where:
//
//          HasMask is either _ or MASK and determines if operation has an
//          constant mask attribute
//          In_0, ..., In_Arity are input types
//          Out is output type
//

// A binary SIMD op with the given name that has signature T x T -> T.
#define SIMD_BINARY_OP(M, T, Name) M(2, _, T##Name, (T, T), T)

// List of SIMD_BINARY_OPs common for Float32x4 or Float64x2.
// Note: M for recognized methods and OP for operators.
#define SIMD_BINARY_FLOAT_OP_LIST(M, OP, T)                                    \
  SIMD_BINARY_OP(OP, T, Add)                                                   \
  SIMD_BINARY_OP(OP, T, Sub)                                                   \
  SIMD_BINARY_OP(OP, T, Mul)                                                   \
  SIMD_BINARY_OP(OP, T, Div)                                                   \
  SIMD_BINARY_OP(M, T, Min)                                                    \
  SIMD_BINARY_OP(M, T, Max)

// List of SIMD_BINARY_OP for Int32x4.
// Note: M for recognized methods and OP for operators.
#define SIMD_BINARY_INTEGER_OP_LIST(M, OP, T)                                  \
  SIMD_BINARY_OP(OP, T, Add)                                                   \
  SIMD_BINARY_OP(OP, T, Sub)                                                   \
  SIMD_BINARY_OP(OP, T, BitAnd)                                                \
  SIMD_BINARY_OP(OP, T, BitOr)                                                 \
  SIMD_BINARY_OP(OP, T, BitXor)

// Given a signature of a given SIMD op construct its per component variations.
#define SIMD_PER_COMPONENT_XYZW(M, Arity, Name, Inputs, Output)                \
  M(Arity, _, Name##X, Inputs, Output)                                         \
  M(Arity, _, Name##Y, Inputs, Output)                                         \
  M(Arity, _, Name##Z, Inputs, Output)                                         \
  M(Arity, _, Name##W, Inputs, Output)

// Define conversion between two SIMD types.
#define SIMD_CONVERSION(M, FromType, ToType)                                   \
  M(1, _, FromType##To##ToType, (FromType), ToType)

// List of all recognized SIMD operations.
// Note: except for operations that map to operators (Add, Mul, Sub, Div,
// BitXor, BitOr) all other operations must match names used by
// MethodRecognizer. This allows to autogenerate conversion from
// MethodRecognizer::Kind into SimdOpInstr::Kind (see KindForMethod helper).
// Note: M is for those SimdOp that are recognized methods and BINARY_OP
// is for operators.
#define SIMD_OP_LIST(M, BINARY_OP)                                             \
  SIMD_BINARY_FLOAT_OP_LIST(M, BINARY_OP, Float32x4)                           \
  SIMD_BINARY_FLOAT_OP_LIST(M, BINARY_OP, Float64x2)                           \
  SIMD_BINARY_INTEGER_OP_LIST(M, BINARY_OP, Int32x4)                           \
  SIMD_PER_COMPONENT_XYZW(M, 1, Float32x4Get, (Float32x4), Double)             \
  SIMD_PER_COMPONENT_XYZW(M, 2, Float32x4With, (Double, Float32x4), Float32x4) \
  SIMD_PER_COMPONENT_XYZW(M, 1, Int32x4GetFlag, (Int32x4), Bool)               \
  SIMD_PER_COMPONENT_XYZW(M, 2, Int32x4WithFlag, (Int32x4, Bool), Int32x4)     \
  M(1, MASK, Float32x4Shuffle, (Float32x4), Float32x4)                         \
  M(1, MASK, Int32x4Shuffle, (Int32x4), Int32x4)                               \
  M(2, MASK, Float32x4ShuffleMix, (Float32x4, Float32x4), Float32x4)           \
  M(2, MASK, Int32x4ShuffleMix, (Int32x4, Int32x4), Int32x4)                   \
  M(2, _, Float32x4Equal, (Float32x4, Float32x4), Int32x4)                     \
  M(2, _, Float32x4GreaterThan, (Float32x4, Float32x4), Int32x4)               \
  M(2, _, Float32x4GreaterThanOrEqual, (Float32x4, Float32x4), Int32x4)        \
  M(2, _, Float32x4LessThan, (Float32x4, Float32x4), Int32x4)                  \
  M(2, _, Float32x4LessThanOrEqual, (Float32x4, Float32x4), Int32x4)           \
  M(2, _, Float32x4NotEqual, (Float32x4, Float32x4), Int32x4)                  \
  M(4, _, Int32x4FromInts, (Int32, Int32, Int32, Int32), Int32x4)              \
  M(4, _, Int32x4FromBools, (Bool, Bool, Bool, Bool), Int32x4)                 \
  M(4, _, Float32x4FromDoubles, (Double, Double, Double, Double), Float32x4)   \
  M(2, _, Float64x2FromDoubles, (Double, Double), Float64x2)                   \
  M(0, _, Float32x4Zero, (), Float32x4)                                        \
  M(0, _, Float64x2Zero, (), Float64x2)                                        \
  M(1, _, Float32x4Splat, (Double), Float32x4)                                 \
  M(1, _, Float64x2Splat, (Double), Float64x2)                                 \
  M(1, _, Int32x4GetSignMask, (Int32x4), Int8)                                 \
  M(1, _, Float32x4GetSignMask, (Float32x4), Int8)                             \
  M(1, _, Float64x2GetSignMask, (Float64x2), Int8)                             \
  M(2, _, Float32x4Scale, (Double, Float32x4), Float32x4)                      \
  M(2, _, Float64x2Scale, (Float64x2, Double), Float64x2)                      \
  M(1, _, Float32x4Sqrt, (Float32x4), Float32x4)                               \
  M(1, _, Float64x2Sqrt, (Float64x2), Float64x2)                               \
  M(1, _, Float32x4Reciprocal, (Float32x4), Float32x4)                         \
  M(1, _, Float32x4ReciprocalSqrt, (Float32x4), Float32x4)                     \
  M(1, _, Float32x4Negate, (Float32x4), Float32x4)                             \
  M(1, _, Float64x2Negate, (Float64x2), Float64x2)                             \
  M(1, _, Float32x4Abs, (Float32x4), Float32x4)                                \
  M(1, _, Float64x2Abs, (Float64x2), Float64x2)                                \
  M(3, _, Float32x4Clamp, (Float32x4, Float32x4, Float32x4), Float32x4)        \
  M(3, _, Float64x2Clamp, (Float64x2, Float64x2, Float64x2), Float64x2)        \
  M(1, _, Float64x2GetX, (Float64x2), Double)                                  \
  M(1, _, Float64x2GetY, (Float64x2), Double)                                  \
  M(2, _, Float64x2WithX, (Float64x2, Double), Float64x2)                      \
  M(2, _, Float64x2WithY, (Float64x2, Double), Float64x2)                      \
  M(3, _, Int32x4Select, (Int32x4, Float32x4, Float32x4), Float32x4)           \
  SIMD_CONVERSION(M, Float32x4, Int32x4)                                       \
  SIMD_CONVERSION(M, Int32x4, Float32x4)                                       \
  SIMD_CONVERSION(M, Float32x4, Float64x2)                                     \
  SIMD_CONVERSION(M, Float64x2, Float32x4)

class SimdOpInstr : public Definition {
 public:
  enum Kind {
#define DECLARE_ENUM(Arity, Mask, Name, ...) k##Name,
    SIMD_OP_LIST(DECLARE_ENUM, DECLARE_ENUM)
#undef DECLARE_ENUM
        kIllegalSimdOp,
  };

  // Create SimdOp from the arguments of the given call and the given receiver.
  static SimdOpInstr* CreateFromCall(Zone* zone,
                                     MethodRecognizer::Kind kind,
                                     Definition* receiver,
                                     Instruction* call,
                                     intptr_t mask = 0);

  // Create SimdOp from the arguments of the given factory call.
  static SimdOpInstr* CreateFromFactoryCall(Zone* zone,
                                            MethodRecognizer::Kind kind,
                                            Instruction* call);

  // Create a binary SimdOp instr.
  static SimdOpInstr* Create(Kind kind,
                             Value* left,
                             Value* right,
                             intptr_t deopt_id) {
    return new SimdOpInstr(kind, left, right, deopt_id);
  }

  // Create a binary SimdOp instr.
  static SimdOpInstr* Create(MethodRecognizer::Kind kind,
                             Value* left,
                             Value* right,
                             intptr_t deopt_id) {
    return new SimdOpInstr(KindForMethod(kind), left, right, deopt_id);
  }

  // Create a unary SimdOp.
  static SimdOpInstr* Create(MethodRecognizer::Kind kind,
                             Value* left,
                             intptr_t deopt_id) {
    return new SimdOpInstr(KindForMethod(kind), left, deopt_id);
  }

  static Kind KindForOperator(MethodRecognizer::Kind kind);

  static Kind KindForMethod(MethodRecognizer::Kind method_kind);

  // Convert a combination of SIMD cid and an arithmetic token into Kind, e.g.
  // Float32x4 and Token::kADD becomes Float32x4Add.
  static Kind KindForOperator(intptr_t cid, Token::Kind op);

  virtual intptr_t InputCount() const;
  virtual Value* InputAt(intptr_t i) const {
    ASSERT(0 <= i && i < InputCount());
    return inputs_[i];
  }

  Kind kind() const { return kind_; }
  intptr_t mask() const {
    ASSERT(HasMask());
    return mask_;
  }

  virtual Representation representation() const;
  virtual Representation RequiredInputRepresentation(intptr_t idx) const;

  virtual CompileType ComputeType() const;

  virtual bool MayThrow() const { return false; }
  virtual bool ComputeCanDeoptimize() const { return false; }

  virtual intptr_t DeoptimizationTarget() const {
    // Direct access since this instruction cannot deoptimize, and the deopt-id
    // was inherited from another instruction that could deoptimize.
    return GetDeoptId();
  }

  virtual bool HasUnknownSideEffects() const { return false; }
  virtual bool AllowsCSE() const { return true; }

  virtual bool AttributesEqual(const Instruction& other) const {
    auto const other_op = other.AsSimdOp();
    return kind() == other_op->kind() &&
           (!HasMask() || mask() == other_op->mask());
  }

  DECLARE_INSTRUCTION(SimdOp)
  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F)                                                          \
  F(const Kind, kind_)                                                         \
  F(intptr_t, mask_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(SimdOpInstr, Definition, FIELD_LIST)
#undef FIELD_LIST

 private:
  SimdOpInstr(Kind kind, intptr_t deopt_id)
      : Definition(deopt_id), kind_(kind) {}

  SimdOpInstr(Kind kind, Value* left, intptr_t deopt_id)
      : Definition(deopt_id), kind_(kind) {
    SetInputAt(0, left);
  }

  SimdOpInstr(Kind kind, Value* left, Value* right, intptr_t deopt_id)
      : Definition(deopt_id), kind_(kind) {
    SetInputAt(0, left);
    SetInputAt(1, right);
  }

  bool HasMask() const;
  void set_mask(intptr_t mask) { mask_ = mask; }

  virtual void RawSetInputAt(intptr_t i, Value* value) { inputs_[i] = value; }

  // We consider SimdOpInstr to be very uncommon so we don't optimize them for
  // size. Any instance of SimdOpInstr has enough space to fit any variation.
  // TODO(dartbug.com/30949) optimize this for size.
  Value* inputs_[4];

  DISALLOW_COPY_AND_ASSIGN(SimdOpInstr);
};

// Generic instruction to call 1-argument stubs specified using [StubId].
class Call1ArgStubInstr : public TemplateDefinition<1, Throws> {
 public:
  enum class StubId {
    kCloneSuspendState,
    kInitAsync,
    kInitAsyncStar,
    kInitSyncStar,
    kFfiAsyncCallbackSend,
  };

  Call1ArgStubInstr(const InstructionSource& source,
                    StubId stub_id,
                    Value* operand,
                    intptr_t deopt_id)
      : TemplateDefinition(source, deopt_id),
        stub_id_(stub_id),
        token_pos_(source.token_pos) {
    SetInputAt(0, operand);
  }

  Value* operand() const { return inputs_[0]; }
  StubId stub_id() const { return stub_id_; }
  virtual TokenPosition token_pos() const { return token_pos_; }

  virtual bool CanCallDart() const { return true; }
  virtual bool ComputeCanDeoptimize() const { return false; }
  virtual bool ComputeCanDeoptimizeAfterCall() const { return true; }
  virtual bool HasUnknownSideEffects() const { return true; }
  virtual intptr_t NumberOfInputsConsumedBeforeCall() const {
    return InputCount();
  }

  DECLARE_INSTRUCTION(Call1ArgStub);
  PRINT_OPERANDS_TO_SUPPORT

#define FIELD_LIST(F)                                                          \
  F(const StubId, stub_id_)                                                    \
  F(const TokenPosition, token_pos_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(Call1ArgStubInstr,
                                          TemplateDefinition,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(Call1ArgStubInstr);
};

// Suspends execution using the suspend stub specified using [StubId].
class SuspendInstr : public TemplateDefinition<2, Throws> {
 public:
  enum class StubId {
    kAwait,
    kAwaitWithTypeCheck,
    kYieldAsyncStar,
    kSuspendSyncStarAtStart,
    kSuspendSyncStarAtYield,
  };

  SuspendInstr(const InstructionSource& source,
               StubId stub_id,
               Value* operand,
               Value* type_args,
               intptr_t deopt_id,
               intptr_t resume_deopt_id)
      : TemplateDefinition(source, deopt_id),
        stub_id_(stub_id),
        resume_deopt_id_(resume_deopt_id),
        token_pos_(source.token_pos) {
    SetInputAt(0, operand);
    if (has_type_args()) {
      SetInputAt(1, type_args);
    } else {
      ASSERT(type_args == nullptr);
    }
  }

  bool has_type_args() const { return stub_id_ == StubId::kAwaitWithTypeCheck; }
  virtual intptr_t InputCount() const { return has_type_args() ? 2 : 1; }

  Value* operand() const { return inputs_[0]; }
  Value* type_args() const {
    ASSERT(has_type_args());
    return inputs_[1];
  }

  StubId stub_id() const { return stub_id_; }
  intptr_t resume_deopt_id() const { return resume_deopt_id_; }
  virtual TokenPosition token_pos() const { return token_pos_; }

  virtual bool CanCallDart() const { return true; }
  virtual bool ComputeCanDeoptimize() const { return false; }
  virtual bool ComputeCanDeoptimizeAfterCall() const { return true; }
  virtual bool HasUnknownSideEffects() const { return true; }
  virtual intptr_t NumberOfInputsConsumedBeforeCall() const {
    return InputCount();
  }

  DECLARE_INSTRUCTION(Suspend);
  PRINT_OPERANDS_TO_SUPPORT

  virtual Definition* Canonicalize(FlowGraph* flow_graph);

#define FIELD_LIST(F)                                                          \
  F(StubId, stub_id_)                                                          \
  F(const intptr_t, resume_deopt_id_)                                          \
  F(const TokenPosition, token_pos_)

  DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS(SuspendInstr,
                                          TemplateDefinition,
                                          FIELD_LIST)
#undef FIELD_LIST

 private:
  DISALLOW_COPY_AND_ASSIGN(SuspendInstr);
};

#undef DECLARE_INSTRUCTION

class Environment : public ZoneAllocated {
 public:
  // Iterate the non-null values in the innermost level of an environment.
  class ShallowIterator : public ValueObject {
   public:
    explicit ShallowIterator(Environment* environment)
        : environment_(environment), index_(0) {}

    ShallowIterator(const ShallowIterator& other)
        : ValueObject(),
          environment_(other.environment_),
          index_(other.index_) {}

    ShallowIterator& operator=(const ShallowIterator& other) {
      environment_ = other.environment_;
      index_ = other.index_;
      return *this;
    }

    Environment* environment() const { return environment_; }

    void Advance() {
      ASSERT(!Done());
      ++index_;
    }

    bool Done() const {
      return (environment_ == nullptr) || (index_ >= environment_->Length());
    }

    Value* CurrentValue() const {
      ASSERT(!Done());
      ASSERT(environment_->values_[index_] != nullptr);
      return environment_->values_[index_];
    }

    void SetCurrentValue(Value* value) {
      ASSERT(!Done());
      ASSERT(value != nullptr);
      environment_->values_[index_] = value;
    }

    Location CurrentLocation() const {
      ASSERT(!Done());
      return environment_->locations_[index_];
    }

    void SetCurrentLocation(Location loc) {
      ASSERT(!Done());
      environment_->locations_[index_] = loc;
    }

   private:
    Environment* environment_;
    intptr_t index_;
  };

  // Iterate all non-null values in an environment, including outer
  // environments.  Note that the iterator skips empty environments.
  class DeepIterator : public ValueObject {
   public:
    explicit DeepIterator(Environment* environment) : iterator_(environment) {
      SkipDone();
    }

    void Advance() {
      ASSERT(!Done());
      iterator_.Advance();
      SkipDone();
    }

    bool Done() const { return iterator_.environment() == nullptr; }

    Value* CurrentValue() const {
      ASSERT(!Done());
      return iterator_.CurrentValue();
    }

    void SetCurrentValue(Value* value) {
      ASSERT(!Done());
      iterator_.SetCurrentValue(value);
    }

    Location CurrentLocation() const {
      ASSERT(!Done());
      return iterator_.CurrentLocation();
    }

    void SetCurrentLocation(Location loc) {
      ASSERT(!Done());
      iterator_.SetCurrentLocation(loc);
    }

   private:
    void SkipDone() {
      while (!Done() && iterator_.Done()) {
        iterator_ = ShallowIterator(iterator_.environment()->outer());
      }
    }

    ShallowIterator iterator_;
  };

  // Construct an environment by constructing uses from an array of definitions.
  static Environment* From(Zone* zone,
                           const GrowableArray<Definition*>& definitions,
                           intptr_t fixed_parameter_count,
                           intptr_t lazy_deopt_pruning_count,
                           const ParsedFunction& parsed_function);

  void set_locations(Location* locations) {
    ASSERT(locations_ == nullptr);
    locations_ = locations;
  }

  // Get deopt_id associated with this environment.
  // Note that only outer environments have deopt id associated with
  // them (set by DeepCopyToOuter).
  intptr_t GetDeoptId() const {
    ASSERT(DeoptIdBits::decode(bitfield_) != DeoptId::kNone);
    return DeoptIdBits::decode(bitfield_);
  }

  intptr_t LazyDeoptPruneCount() const {
    return LazyDeoptPruningBits::decode(bitfield_);
  }

  bool LazyDeoptToBeforeDeoptId() const {
    return LazyDeoptToBeforeDeoptId::decode(bitfield_);
  }

  void MarkAsLazyDeoptToBeforeDeoptId() {
    bitfield_ = LazyDeoptToBeforeDeoptId::update(true, bitfield_);
    // As eager and lazy deopts will target the before environment, we do not
    // want to prune inputs on lazy deopts.
    bitfield_ = LazyDeoptPruningBits::update(0, bitfield_);
  }

  // This environment belongs to an optimistically hoisted instruction.
  bool IsHoisted() const { return Hoisted::decode(bitfield_); }

  void MarkAsHoisted() { bitfield_ = Hoisted::update(true, bitfield_); }

  Environment* GetLazyDeoptEnv(Zone* zone) {
    if (LazyDeoptToBeforeDeoptId()) {
      ASSERT(LazyDeoptPruneCount() == 0);
    }
    const intptr_t num_args_to_prune = LazyDeoptPruneCount();
    if (num_args_to_prune == 0) return this;
    return DeepCopy(zone, Length() - num_args_to_prune);
  }

  Environment* outer() const { return outer_; }

  Environment* Outermost() {
    Environment* result = this;
    while (result->outer() != nullptr)
      result = result->outer();
    return result;
  }

  Value* ValueAt(intptr_t ix) const { return values_[ix]; }

  void PushValue(Value* value);

  intptr_t Length() const { return values_.length(); }

  Location LocationAt(intptr_t index) const {
    ASSERT((index >= 0) && (index < values_.length()));
    return locations_[index];
  }

  // The use index is the index in the flattened environment.
  Value* ValueAtUseIndex(intptr_t index) const {
    const Environment* env = this;
    while (index >= env->Length()) {
      ASSERT(env->outer_ != nullptr);
      index -= env->Length();
      env = env->outer_;
    }
    return env->ValueAt(index);
  }

  intptr_t fixed_parameter_count() const { return fixed_parameter_count_; }

  intptr_t CountArgsPushed() {
    intptr_t count = 0;
    for (Environment::DeepIterator it(this); !it.Done(); it.Advance()) {
      if (it.CurrentValue()->definition()->IsMoveArgument()) {
        count++;
      }
    }
    return count;
  }

  const Function& function() const { return function_; }

  Environment* DeepCopy(Zone* zone) const { return DeepCopy(zone, Length()); }

  void DeepCopyTo(Zone* zone, Instruction* instr) const;
  void DeepCopyToOuter(Zone* zone,
                       Instruction* instr,
                       intptr_t outer_deopt_id) const;

  void DeepCopyAfterTo(Zone* zone,
                       Instruction* instr,
                       intptr_t argc,
                       Definition* dead,
                       Definition* result) const;

  void PrintTo(BaseTextBuffer* f) const;
  const char* ToCString() const;

  // Deep copy an environment.  The 'length' parameter may be less than the
  // environment's length in order to drop values (e.g., passed arguments)
  // from the copy.
  Environment* DeepCopy(Zone* zone, intptr_t length) const;

  void Write(FlowGraphSerializer* s) const;
  explicit Environment(FlowGraphDeserializer* d);

 private:
  friend class ShallowIterator;
  friend class compiler::BlockBuilder;  // For Environment constructor.

  class LazyDeoptPruningBits : public BitField<uintptr_t, uintptr_t, 0, 8> {};
  class LazyDeoptToBeforeDeoptId
      : public BitField<uintptr_t, bool, LazyDeoptPruningBits::kNextBit, 1> {};
  class Hoisted : public BitField<uintptr_t,
                                  bool,
                                  LazyDeoptToBeforeDeoptId::kNextBit,
                                  1> {};
  class DeoptIdBits : public BitField<uintptr_t,
                                      intptr_t,
                                      Hoisted::kNextBit,
                                      kBitsPerWord - Hoisted::kNextBit,
                                      /*sign_extend=*/true> {};

  Environment(intptr_t length,
              intptr_t fixed_parameter_count,
              intptr_t lazy_deopt_pruning_count,
              const Function& function,
              Environment* outer)
      : values_(length),
        fixed_parameter_count_(fixed_parameter_count),
        bitfield_(DeoptIdBits::encode(DeoptId::kNone) |
                  LazyDeoptToBeforeDeoptId::encode(false) |
                  LazyDeoptPruningBits::encode(lazy_deopt_pruning_count)),
        function_(function),
        outer_(outer) {}

  void SetDeoptId(intptr_t deopt_id) {
    bitfield_ = DeoptIdBits::update(deopt_id, bitfield_);
  }
  void SetLazyDeoptPruneCount(intptr_t value) {
    bitfield_ = LazyDeoptPruningBits::update(value, bitfield_);
  }
  void SetLazyDeoptToBeforeDeoptId(bool value) {
    bitfield_ = LazyDeoptToBeforeDeoptId::update(value, bitfield_);
  }

  GrowableArray<Value*> values_;
  Location* locations_ = nullptr;
  const intptr_t fixed_parameter_count_;
  // Deoptimization id associated with this environment. Only set for
  // outer environments.
  uintptr_t bitfield_;
  const Function& function_;
  Environment* outer_;

  DISALLOW_COPY_AND_ASSIGN(Environment);
};

class InstructionVisitor : public ValueObject {
 public:
  InstructionVisitor() {}
  virtual ~InstructionVisitor() {}

// Visit functions for instruction classes, with an empty default
// implementation.
#define DECLARE_VISIT_INSTRUCTION(ShortName, Attrs)                            \
  virtual void Visit##ShortName(ShortName##Instr* instr) {}

  FOR_EACH_INSTRUCTION(DECLARE_VISIT_INSTRUCTION)

#undef DECLARE_VISIT_INSTRUCTION

 private:
  DISALLOW_COPY_AND_ASSIGN(InstructionVisitor);
};

// Visitor base class to visit each instruction and computation in a flow
// graph as defined by a reversed list of basic blocks.
class FlowGraphVisitor : public InstructionVisitor {
 public:
  explicit FlowGraphVisitor(const GrowableArray<BlockEntryInstr*>& block_order)
      : current_iterator_(nullptr), block_order_(&block_order) {}
  virtual ~FlowGraphVisitor() {}

  ForwardInstructionIterator* current_iterator() const {
    return current_iterator_;
  }

  // Visit each block in the block order, and for each block its
  // instructions in order from the block entry to exit.
  virtual void VisitBlocks();

 protected:
  void set_block_order(const GrowableArray<BlockEntryInstr*>& block_order) {
    block_order_ = &block_order;
  }

  ForwardInstructionIterator* current_iterator_;

 private:
  const GrowableArray<BlockEntryInstr*>* block_order_;
  DISALLOW_COPY_AND_ASSIGN(FlowGraphVisitor);
};

// Helper macros for platform ports.
#define DEFINE_UNIMPLEMENTED_INSTRUCTION(Name)                                 \
  LocationSummary* Name::MakeLocationSummary(Zone* zone, bool opt) const {     \
    UNIMPLEMENTED();                                                           \
    return nullptr;                                                            \
  }                                                                            \
  void Name::EmitNativeCode(FlowGraphCompiler* compiler) {                     \
    UNIMPLEMENTED();                                                           \
  }

template <intptr_t kExtraInputs>
StringPtr TemplateDartCall<kExtraInputs>::Selector() {
  if (auto static_call = this->AsStaticCall()) {
    return static_call->function().name();
  } else if (auto instance_call = this->AsInstanceCall()) {
    return instance_call->function_name().ptr();
  } else {
    UNREACHABLE();
  }
}

inline bool Value::CanBe(const Object& value) {
  ConstantInstr* constant = definition()->AsConstant();
  return (constant == nullptr) || constant->value().ptr() == value.ptr();
}
#undef DECLARE_INSTRUCTION_SERIALIZABLE_FIELDS
#undef DECLARE_CUSTOM_SERIALIZATION
#undef DECLARE_EMPTY_SERIALIZATION

}  // namespace dart

#endif  // RUNTIME_VM_COMPILER_BACKEND_IL_H_
