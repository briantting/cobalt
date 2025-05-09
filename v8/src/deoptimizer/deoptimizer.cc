// Copyright 2013 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/deoptimizer/deoptimizer.h"

#include "src/base/memory.h"
#include "src/codegen/interface-descriptors.h"
#include "src/codegen/register-configuration.h"
#include "src/codegen/reloc-info.h"
#include "src/debug/debug.h"
#include "src/deoptimizer/deoptimized-frame-info.h"
#include "src/deoptimizer/materialized-object-store.h"
#include "src/execution/frames-inl.h"
#include "src/execution/isolate.h"
#include "src/execution/pointer-authentication.h"
#include "src/execution/v8threads.h"
#include "src/handles/handles-inl.h"
#include "src/heap/heap-inl.h"
#include "src/logging/counters.h"
#include "src/logging/log.h"
#include "src/logging/runtime-call-stats-scope.h"
#include "src/objects/js-function-inl.h"
#include "src/objects/oddball.h"
#include "src/snapshot/embedded/embedded-data.h"

#if V8_ENABLE_WEBASSEMBLY
#include "src/wasm/wasm-linkage.h"
#endif  // V8_ENABLE_WEBASSEMBLY

namespace v8 {

using base::Memory;

namespace internal {

namespace {

class DeoptimizableCodeIterator {
 public:
  explicit DeoptimizableCodeIterator(Isolate* isolate);
  DeoptimizableCodeIterator(const DeoptimizableCodeIterator&) = delete;
  DeoptimizableCodeIterator& operator=(const DeoptimizableCodeIterator&) =
      delete;
  Code Next();

 private:
  Isolate* const isolate_;
  std::unique_ptr<SafepointScope> safepoint_scope_;
  std::unique_ptr<ObjectIterator> object_iterator_;
  enum { kIteratingCodeSpace, kIteratingCodeLOSpace, kDone } state_;

  DISALLOW_GARBAGE_COLLECTION(no_gc)
};

DeoptimizableCodeIterator::DeoptimizableCodeIterator(Isolate* isolate)
    : isolate_(isolate),
      safepoint_scope_(std::make_unique<SafepointScope>(
          isolate, isolate->is_shared_space_isolate()
                       ? SafepointKind::kGlobal
                       : SafepointKind::kIsolate)),
      object_iterator_(
          isolate->heap()->code_space()->GetObjectIterator(isolate->heap())),
      state_(kIteratingCodeSpace) {}

Code DeoptimizableCodeIterator::Next() {
  while (true) {
    HeapObject object = object_iterator_->Next();
    if (object.is_null()) {
      // No objects left in the current iterator, try to move to the next space
      // based on the state.
      switch (state_) {
        case kIteratingCodeSpace: {
          object_iterator_ =
              isolate_->heap()->code_lo_space()->GetObjectIterator(
                  isolate_->heap());
          state_ = kIteratingCodeLOSpace;
          continue;
        }
        case kIteratingCodeLOSpace:
          // No other spaces to iterate, so clean up and we're done. Keep the
          // object iterator so that it keeps returning null on Next(), to avoid
          // needing to branch on state_ before the while loop, but drop the
          // safepoint scope since we no longer need to stop the heap from
          // moving.
          safepoint_scope_.reset();
          state_ = kDone;
          V8_FALLTHROUGH;
        case kDone:
          return Code();
      }
    }
    InstructionStream istream = InstructionStream::cast(object);
    Code code;
    if (!istream.TryGetCode(&code, kAcquireLoad)) continue;
    if (!CodeKindCanDeoptimize(code.kind())) continue;
    return code;
  }
}

}  // namespace

// {FrameWriter} offers a stack writer abstraction for writing
// FrameDescriptions. The main service the class provides is managing
// {top_offset_}, i.e. the offset of the next slot to write to.
//
// Note: Not in an anonymous namespace due to the friend class declaration
// in Deoptimizer.
class FrameWriter {
 public:
  static const int NO_INPUT_INDEX = -1;
  FrameWriter(Deoptimizer* deoptimizer, FrameDescription* frame,
              CodeTracer::Scope* trace_scope)
      : deoptimizer_(deoptimizer),
        frame_(frame),
        trace_scope_(trace_scope),
        top_offset_(frame->GetFrameSize()) {}

  void PushRawValue(intptr_t value, const char* debug_hint) {
    PushValue(value);
    if (trace_scope_ != nullptr) {
      DebugPrintOutputValue(value, debug_hint);
    }
  }

  void PushRawObject(Object obj, const char* debug_hint) {
    intptr_t value = obj.ptr();
    PushValue(value);
    if (trace_scope_ != nullptr) {
      DebugPrintOutputObject(obj, top_offset_, debug_hint);
    }
  }

  // There is no check against the allowed addresses for bottommost frames, as
  // the caller's pc could be anything. The caller's pc pushed here should never
  // be re-signed.
  void PushBottommostCallerPc(intptr_t pc) {
    top_offset_ -= kPCOnStackSize;
    frame_->SetFrameSlot(top_offset_, pc);
    DebugPrintOutputPc(pc, "bottommost caller's pc\n");
  }

  void PushApprovedCallerPc(intptr_t pc) {
    top_offset_ -= kPCOnStackSize;
    frame_->SetCallerPc(top_offset_, pc);
    DebugPrintOutputPc(pc, "caller's pc\n");
  }

  void PushCallerFp(intptr_t fp) {
    top_offset_ -= kFPOnStackSize;
    frame_->SetCallerFp(top_offset_, fp);
    DebugPrintOutputValue(fp, "caller's fp\n");
  }

  void PushCallerConstantPool(intptr_t cp) {
    top_offset_ -= kSystemPointerSize;
    frame_->SetCallerConstantPool(top_offset_, cp);
    DebugPrintOutputValue(cp, "caller's constant_pool\n");
  }

  void PushTranslatedValue(const TranslatedFrame::iterator& iterator,
                           const char* debug_hint = "") {
    Object obj = iterator->GetRawValue();
    PushRawObject(obj, debug_hint);
    if (trace_scope_ != nullptr) {
      PrintF(trace_scope_->file(), " (input #%d)\n", iterator.input_index());
    }
    deoptimizer_->QueueValueForMaterialization(output_address(top_offset_), obj,
                                               iterator);
  }

  void PushStackJSArguments(TranslatedFrame::iterator& iterator,
                            int parameters_count) {
    std::vector<TranslatedFrame::iterator> parameters;
    parameters.reserve(parameters_count);
    for (int i = 0; i < parameters_count; ++i, ++iterator) {
      parameters.push_back(iterator);
    }
    for (auto& parameter : base::Reversed(parameters)) {
      PushTranslatedValue(parameter, "stack parameter");
    }
  }

  unsigned top_offset() const { return top_offset_; }

  FrameDescription* frame() { return frame_; }

 private:
  void PushValue(intptr_t value) {
    CHECK_GE(top_offset_, 0);
    top_offset_ -= kSystemPointerSize;
    frame_->SetFrameSlot(top_offset_, value);
  }

  Address output_address(unsigned output_offset) {
    Address output_address =
        static_cast<Address>(frame_->GetTop()) + output_offset;
    return output_address;
  }

  void DebugPrintOutputValue(intptr_t value, const char* debug_hint = "") {
    if (trace_scope_ != nullptr) {
      PrintF(trace_scope_->file(),
             "    " V8PRIxPTR_FMT ": [top + %3d] <- " V8PRIxPTR_FMT " ;  %s",
             output_address(top_offset_), top_offset_, value, debug_hint);
    }
  }

  void DebugPrintOutputPc(intptr_t value, const char* debug_hint = "") {
#ifdef V8_ENABLE_CONTROL_FLOW_INTEGRITY
    if (trace_scope_ != nullptr) {
      PrintF(trace_scope_->file(),
             "    " V8PRIxPTR_FMT ": [top + %3d] <- " V8PRIxPTR_FMT
             " (signed) " V8PRIxPTR_FMT " (unsigned) ;  %s",
             output_address(top_offset_), top_offset_, value,
             PointerAuthentication::StripPAC(value), debug_hint);
    }
#else
    DebugPrintOutputValue(value, debug_hint);
#endif
  }

  void DebugPrintOutputObject(Object obj, unsigned output_offset,
                              const char* debug_hint = "") {
    if (trace_scope_ != nullptr) {
      PrintF(trace_scope_->file(), "    " V8PRIxPTR_FMT ": [top + %3d] <- ",
             output_address(output_offset), output_offset);
      if (obj.IsSmi()) {
        PrintF(trace_scope_->file(), V8PRIxPTR_FMT " <Smi %d>", obj.ptr(),
               Smi::cast(obj).value());
      } else {
        obj.ShortPrint(trace_scope_->file());
      }
      PrintF(trace_scope_->file(), " ;  %s", debug_hint);
    }
  }

  Deoptimizer* deoptimizer_;
  FrameDescription* frame_;
  CodeTracer::Scope* const trace_scope_;
  unsigned top_offset_;
};

// We rely on this function not causing a GC. It is called from generated code
// without having a real stack frame in place.
Deoptimizer* Deoptimizer::New(Address raw_function, DeoptimizeKind kind,
                              Address from, int fp_to_sp_delta,
                              Isolate* isolate) {
  JSFunction function = JSFunction::cast(Object(raw_function));
  Deoptimizer* deoptimizer =
      new Deoptimizer(isolate, function, kind, from, fp_to_sp_delta);
  isolate->set_current_deoptimizer(deoptimizer);
  return deoptimizer;
}

Deoptimizer* Deoptimizer::Grab(Isolate* isolate) {
  Deoptimizer* result = isolate->GetAndClearCurrentDeoptimizer();
  result->DeleteFrameDescriptions();
  return result;
}

DeoptimizedFrameInfo* Deoptimizer::DebuggerInspectableFrame(
    JavaScriptFrame* frame, int jsframe_index, Isolate* isolate) {
  CHECK(frame->is_optimized());

  TranslatedState translated_values(frame);
  translated_values.Prepare(frame->fp());

  TranslatedState::iterator frame_it = translated_values.end();
  int counter = jsframe_index;
  for (auto it = translated_values.begin(); it != translated_values.end();
       it++) {
    if (it->kind() == TranslatedFrame::kUnoptimizedFunction ||
        it->kind() == TranslatedFrame::kJavaScriptBuiltinContinuation ||
        it->kind() ==
            TranslatedFrame::kJavaScriptBuiltinContinuationWithCatch) {
      if (counter == 0) {
        frame_it = it;
        break;
      }
      counter--;
    }
  }
  CHECK(frame_it != translated_values.end());
  // We only include kJavaScriptBuiltinContinuation frames above to get the
  // counting right.
  CHECK_EQ(frame_it->kind(), TranslatedFrame::kUnoptimizedFunction);

  DeoptimizedFrameInfo* info =
      new DeoptimizedFrameInfo(&translated_values, frame_it, isolate);

  return info;
}

namespace {
class ActivationsFinder : public ThreadVisitor {
 public:
  ActivationsFinder(GcSafeCode topmost_optimized_code,
                    bool safe_to_deopt_topmost_optimized_code) {
#ifdef DEBUG
    topmost_ = topmost_optimized_code;
    safe_to_deopt_ = safe_to_deopt_topmost_optimized_code;
#endif
  }

  // Find the frames with activations of codes marked for deoptimization, search
  // for the trampoline to the deoptimizer call respective to each code, and use
  // it to replace the current pc on the stack.
  void VisitThread(Isolate* isolate, ThreadLocalTop* top) override {
    for (StackFrameIterator it(isolate, top); !it.done(); it.Advance()) {
      if (it.frame()->is_optimized()) {
        GcSafeCode code = it.frame()->GcSafeLookupCode();
        if (CodeKindCanDeoptimize(code.kind()) &&
            code.marked_for_deoptimization()) {
          // Obtain the trampoline to the deoptimizer call.
          int trampoline_pc;
          if (code.is_maglevved()) {
            MaglevSafepointEntry safepoint = MaglevSafepointTable::FindEntry(
                isolate, code, it.frame()->pc());
            trampoline_pc = safepoint.trampoline_pc();
          } else {
            SafepointEntry safepoint =
                SafepointTable::FindEntry(isolate, code, it.frame()->pc());
            trampoline_pc = safepoint.trampoline_pc();
          }
          DCHECK_IMPLIES(code == topmost_, safe_to_deopt_);
          static_assert(SafepointEntry::kNoTrampolinePC == -1);
          CHECK_GE(trampoline_pc, 0);
          // Replace the current pc on the stack with the trampoline.
          // TODO(v8:10026): avoid replacing a signed pointer.
          Address* pc_addr = it.frame()->pc_address();
          Address new_pc = code.instruction_start() + trampoline_pc;
          PointerAuthentication::ReplacePC(pc_addr, new_pc, kSystemPointerSize);
        }
      }
    }
  }

 private:
#ifdef DEBUG
  GcSafeCode topmost_;
  bool safe_to_deopt_;
#endif
};
}  // namespace

// Replace pc on the stack for codes marked for deoptimization.
// static
void Deoptimizer::DeoptimizeMarkedCode(Isolate* isolate) {
  DisallowGarbageCollection no_gc;

  GcSafeCode topmost_optimized_code;
  bool safe_to_deopt_topmost_optimized_code = false;
#ifdef DEBUG
  // Make sure all activations of optimized code can deopt at their current PC.
  // The topmost optimized code has special handling because it cannot be
  // deoptimized due to weak object dependency.
  for (StackFrameIterator it(isolate, isolate->thread_local_top()); !it.done();
       it.Advance()) {
    if (it.frame()->is_optimized()) {
      GcSafeCode code = it.frame()->GcSafeLookupCode();
      JSFunction function =
          static_cast<OptimizedFrame*>(it.frame())->function();
      TraceFoundActivation(isolate, function);
      bool safe_if_deopt_triggered;
      if (code.is_maglevved()) {
        MaglevSafepointEntry safepoint =
            MaglevSafepointTable::FindEntry(isolate, code, it.frame()->pc());
        safe_if_deopt_triggered = safepoint.has_deoptimization_index();
      } else {
        SafepointEntry safepoint =
            SafepointTable::FindEntry(isolate, code, it.frame()->pc());
        safe_if_deopt_triggered = safepoint.has_deoptimization_index();
      }

      // Deopt is checked when we are patching addresses on stack.
      bool is_builtin_code = code.kind() == CodeKind::BUILTIN;
      DCHECK(topmost_optimized_code.is_null() || safe_if_deopt_triggered ||
             is_builtin_code);
      if (topmost_optimized_code.is_null()) {
        topmost_optimized_code = code;
        safe_to_deopt_topmost_optimized_code = safe_if_deopt_triggered;
      }
    }
  }
#endif

  ActivationsFinder visitor(topmost_optimized_code,
                            safe_to_deopt_topmost_optimized_code);
  // Iterate over the stack of this thread.
  visitor.VisitThread(isolate, isolate->thread_local_top());
  // In addition to iterate over the stack of this thread, we also
  // need to consider all the other threads as they may also use
  // the code currently beings deoptimized.
  isolate->thread_manager()->IterateArchivedThreads(&visitor);
}

void Deoptimizer::DeoptimizeAll(Isolate* isolate) {
  RCS_SCOPE(isolate, RuntimeCallCounterId::kDeoptimizeCode);
  TimerEventScope<TimerEventDeoptimizeCode> timer(isolate);
  TRACE_EVENT0("v8", "V8.DeoptimizeCode");
  TraceDeoptAll(isolate);
  isolate->AbortConcurrentOptimization(BlockingBehavior::kBlock);

  // Mark all code, then deoptimize.
  {
    DeoptimizableCodeIterator it(isolate);
    for (Code code = it.Next(); !code.is_null(); code = it.Next()) {
      code.set_marked_for_deoptimization(true);
    }
  }

  DeoptimizeMarkedCode(isolate);
}

// static
void Deoptimizer::DeoptimizeFunction(JSFunction function, Code code) {
  Isolate* isolate = function.GetIsolate();
  RCS_SCOPE(isolate, RuntimeCallCounterId::kDeoptimizeCode);
  TimerEventScope<TimerEventDeoptimizeCode> timer(isolate);
  TRACE_EVENT0("v8", "V8.DeoptimizeCode");
  function.ResetIfCodeFlushed();
  if (code.is_null()) code = function.code();

  if (CodeKindCanDeoptimize(code.kind())) {
    // Mark the code for deoptimization and unlink any functions that also
    // refer to that code. The code cannot be shared across native contexts,
    // so we only need to search one.
    code.set_marked_for_deoptimization(true);
    // The code in the function's optimized code feedback vector slot might
    // be different from the code on the function - evict it if necessary.
    function.feedback_vector().EvictOptimizedCodeMarkedForDeoptimization(
        isolate, function.shared(), "unlinking code marked for deopt");

    DeoptimizeMarkedCode(isolate);
  }
}

// static
void Deoptimizer::DeoptimizeAllOptimizedCodeWithFunction(
    Isolate* isolate, Handle<SharedFunctionInfo> function) {
  RCS_SCOPE(isolate, RuntimeCallCounterId::kDeoptimizeCode);
  TimerEventScope<TimerEventDeoptimizeCode> timer(isolate);
  TRACE_EVENT0("v8", "V8.DeoptimizeAllOptimizedCodeWithFunction");

  // Make sure no new code is compiled with the function.
  isolate->AbortConcurrentOptimization(BlockingBehavior::kBlock);

  // Mark all code that inlines this function, then deoptimize.
  bool any_marked = false;
  {
    DeoptimizableCodeIterator it(isolate);
    for (Code code = it.Next(); !code.is_null(); code = it.Next()) {
      if (code.Inlines(*function)) {
        code.set_marked_for_deoptimization(true);
        any_marked = true;
      }
    }
  }
  if (any_marked) {
    DeoptimizeMarkedCode(isolate);
  }
}

void Deoptimizer::ComputeOutputFrames(Deoptimizer* deoptimizer) {
  deoptimizer->DoComputeOutputFrames();
}

const char* Deoptimizer::MessageFor(DeoptimizeKind kind) {
  switch (kind) {
    case DeoptimizeKind::kEager:
      return "deopt-eager";
    case DeoptimizeKind::kLazy:
      return "deopt-lazy";
  }
}

Deoptimizer::Deoptimizer(Isolate* isolate, JSFunction function,
                         DeoptimizeKind kind, Address from, int fp_to_sp_delta)
    : isolate_(isolate),
      function_(function),
      deopt_exit_index_(kFixedExitSizeMarker),
      deopt_kind_(kind),
      from_(from),
      fp_to_sp_delta_(fp_to_sp_delta),
      deoptimizing_throw_(false),
      catch_handler_data_(-1),
      catch_handler_pc_offset_(-1),
      restart_frame_index_(-1),
      input_(nullptr),
      output_count_(0),
      output_(nullptr),
      caller_frame_top_(0),
      caller_fp_(0),
      caller_pc_(0),
      caller_constant_pool_(0),
      actual_argument_count_(0),
      stack_fp_(0),
      trace_scope_(v8_flags.trace_deopt || v8_flags.log_deopt
                       ? new CodeTracer::Scope(isolate->GetCodeTracer())
                       : nullptr) {
  if (isolate->deoptimizer_lazy_throw()) {
    CHECK_EQ(kind, DeoptimizeKind::kLazy);
    isolate->set_deoptimizer_lazy_throw(false);
    deoptimizing_throw_ = true;
  }

  if (isolate->debug()->IsRestartFrameScheduled()) {
    CHECK(deoptimizing_throw_);
    restart_frame_index_ = isolate->debug()->restart_inline_frame_index();
    CHECK_GE(restart_frame_index_, 0);
    isolate->debug()->clear_restart_frame();
  }

  DCHECK_NE(from, kNullAddress);
  compiled_code_ = isolate_->heap()->FindCodeForInnerPointer(from);
  DCHECK(!compiled_code_.is_null());
  DCHECK(compiled_code_.IsCode());

  DCHECK(function.IsJSFunction());
#ifdef DEBUG
  DCHECK(AllowGarbageCollection::IsAllowed());
  disallow_garbage_collection_ = new DisallowGarbageCollection();
#endif  // DEBUG
  CHECK(CodeKindCanDeoptimize(compiled_code_.kind()));
  {
    HandleScope scope(isolate_);
    PROFILE(isolate_, CodeDeoptEvent(handle(compiled_code_, isolate_), kind,
                                     from_, fp_to_sp_delta_));
  }
  unsigned size = ComputeInputFrameSize();
  const int parameter_count =
      function.shared().internal_formal_parameter_count_with_receiver();
  input_ = new (size) FrameDescription(size, parameter_count, isolate_);

  DCHECK_EQ(deopt_exit_index_, kFixedExitSizeMarker);
  // Calculate the deopt exit index from return address.
  DCHECK_GT(kEagerDeoptExitSize, 0);
  DCHECK_GT(kLazyDeoptExitSize, 0);
  DeoptimizationData deopt_data =
      DeoptimizationData::cast(compiled_code_.deoptimization_data());
  Address deopt_start =
      compiled_code_.instruction_start() + deopt_data.DeoptExitStart().value();
  int eager_deopt_count = deopt_data.EagerDeoptCount().value();
  Address lazy_deopt_start =
      deopt_start + eager_deopt_count * kEagerDeoptExitSize;
  // The deoptimization exits are sorted so that lazy deopt exits appear after
  // eager deopts.
  static_assert(static_cast<int>(DeoptimizeKind::kLazy) ==
                    static_cast<int>(kLastDeoptimizeKind),
                "lazy deopts are expected to be emitted last");
  // from_ is the value of the link register after the call to the
  // deoptimizer, so for the last lazy deopt, from_ points to the first
  // non-lazy deopt, so we use <=, similarly for the last non-lazy deopt and
  // the first deopt with resume entry.
  if (from_ <= lazy_deopt_start) {
    int offset = static_cast<int>(from_ - kEagerDeoptExitSize - deopt_start);
    DCHECK_EQ(0, offset % kEagerDeoptExitSize);
    deopt_exit_index_ = offset / kEagerDeoptExitSize;
  } else {
    int offset =
        static_cast<int>(from_ - kLazyDeoptExitSize - lazy_deopt_start);
    DCHECK_EQ(0, offset % kLazyDeoptExitSize);
    deopt_exit_index_ = eager_deopt_count + (offset / kLazyDeoptExitSize);
  }
}

Handle<JSFunction> Deoptimizer::function() const {
  return Handle<JSFunction>(function_, isolate());
}

Handle<Code> Deoptimizer::compiled_code() const {
  return Handle<Code>(compiled_code_, isolate());
}

Deoptimizer::~Deoptimizer() {
  DCHECK(input_ == nullptr && output_ == nullptr);
  DCHECK_NULL(disallow_garbage_collection_);
  delete trace_scope_;
}

void Deoptimizer::DeleteFrameDescriptions() {
  delete input_;
  for (int i = 0; i < output_count_; ++i) {
    if (output_[i] != input_) delete output_[i];
  }
  delete[] output_;
  input_ = nullptr;
  output_ = nullptr;
#ifdef DEBUG
  DCHECK(!AllowGarbageCollection::IsAllowed());
  DCHECK_NOT_NULL(disallow_garbage_collection_);
  delete disallow_garbage_collection_;
  disallow_garbage_collection_ = nullptr;
#endif  // DEBUG
}

Builtin Deoptimizer::GetDeoptimizationEntry(DeoptimizeKind kind) {
  switch (kind) {
    case DeoptimizeKind::kEager:
      return Builtin::kDeoptimizationEntry_Eager;
    case DeoptimizeKind::kLazy:
      return Builtin::kDeoptimizationEntry_Lazy;
  }
}

namespace {

int LookupCatchHandler(Isolate* isolate, TranslatedFrame* translated_frame,
                       int* data_out) {
  switch (translated_frame->kind()) {
    case TranslatedFrame::kUnoptimizedFunction: {
      int bytecode_offset = translated_frame->bytecode_offset().ToInt();
      HandlerTable table(
          translated_frame->raw_shared_info().GetBytecodeArray(isolate));
      return table.LookupRange(bytecode_offset, data_out, nullptr);
    }
    case TranslatedFrame::kJavaScriptBuiltinContinuationWithCatch: {
      return 0;
    }
    default:
      break;
  }
  return -1;
}

}  // namespace

void Deoptimizer::TraceDeoptBegin(int optimization_id,
                                  BytecodeOffset bytecode_offset) {
  DCHECK(tracing_enabled());
  FILE* file = trace_scope()->file();
  Deoptimizer::DeoptInfo info = Deoptimizer::GetDeoptInfo();
  PrintF(file, "[bailout (kind: %s, reason: %s): begin. deoptimizing ",
         MessageFor(deopt_kind_), DeoptimizeReasonToString(info.deopt_reason));
  if (function_.IsJSFunction()) {
    function_.ShortPrint(file);
    PrintF(file, ", ");
  }
  compiled_code_.ShortPrint(file);
  PrintF(file,
         ", opt id %d, "
#ifdef DEBUG
         "node id %d, "
#endif  // DEBUG
         "bytecode offset %d, deopt exit %d, FP to SP "
         "delta %d, "
         "caller SP " V8PRIxPTR_FMT ", pc " V8PRIxPTR_FMT "]\n",
         optimization_id,
#ifdef DEBUG
         info.node_id,
#endif  // DEBUG
         bytecode_offset.ToInt(), deopt_exit_index_, fp_to_sp_delta_,
         caller_frame_top_, PointerAuthentication::StripPAC(from_));
  if (verbose_tracing_enabled() && deopt_kind_ != DeoptimizeKind::kLazy) {
    PrintF(file, "            ;;; deoptimize at ");
    OFStream outstr(file);
    info.position.Print(outstr, compiled_code_);
    PrintF(file, "\n");
  }
}

void Deoptimizer::TraceDeoptEnd(double deopt_duration) {
  DCHECK(verbose_tracing_enabled());
  PrintF(trace_scope()->file(), "[bailout end. took %0.3f ms]\n",
         deopt_duration);
}

// static
void Deoptimizer::TraceMarkForDeoptimization(Isolate* isolate, Code code,
                                             const char* reason) {
  if (!v8_flags.trace_deopt && !v8_flags.log_deopt) return;

  DisallowGarbageCollection no_gc;
  Object maybe_data = code.deoptimization_data();
  if (maybe_data == ReadOnlyRoots(isolate).empty_fixed_array()) return;

  DeoptimizationData deopt_data = DeoptimizationData::cast(maybe_data);
  CodeTracer::Scope scope(isolate->GetCodeTracer());
  if (v8_flags.trace_deopt) {
    PrintF(scope.file(), "[marking dependent code ");
    code.ShortPrint(scope.file());
    PrintF(scope.file(), " (");
    deopt_data.SharedFunctionInfo().ShortPrint(scope.file());
    PrintF(") (opt id %d) for deoptimization, reason: %s]\n",
           deopt_data.OptimizationId().value(), reason);
  }
  if (!v8_flags.log_deopt) return;
  no_gc.Release();
  {
    HandleScope handle_scope(isolate);
    PROFILE(
        isolate,
        CodeDependencyChangeEvent(
            handle(code, isolate),
            handle(SharedFunctionInfo::cast(deopt_data.SharedFunctionInfo()),
                   isolate),
            reason));
  }
}

// static
void Deoptimizer::TraceEvictFromOptimizedCodeCache(Isolate* isolate,
                                                   SharedFunctionInfo sfi,
                                                   const char* reason) {
  if (!v8_flags.trace_deopt_verbose) return;

  DisallowGarbageCollection no_gc;
  CodeTracer::Scope scope(isolate->GetCodeTracer());
  PrintF(scope.file(),
         "[evicting optimized code marked for deoptimization (%s) for ",
         reason);
  sfi.ShortPrint(scope.file());
  PrintF(scope.file(), "]\n");
}

#ifdef DEBUG
// static
void Deoptimizer::TraceFoundActivation(Isolate* isolate, JSFunction function) {
  if (!v8_flags.trace_deopt_verbose) return;
  CodeTracer::Scope scope(isolate->GetCodeTracer());
  PrintF(scope.file(), "[deoptimizer found activation of function: ");
  function.PrintName(scope.file());
  PrintF(scope.file(), " / %" V8PRIxPTR "]\n", function.ptr());
}
#endif  // DEBUG

// static
void Deoptimizer::TraceDeoptAll(Isolate* isolate) {
  if (!v8_flags.trace_deopt_verbose) return;
  CodeTracer::Scope scope(isolate->GetCodeTracer());
  PrintF(scope.file(), "[deoptimize all code in all contexts]\n");
}

// We rely on this function not causing a GC.  It is called from generated code
// without having a real stack frame in place.
void Deoptimizer::DoComputeOutputFrames() {
  // When we call this function, the return address of the previous frame has
  // been removed from the stack by the DeoptimizationEntry builtin, so the
  // stack is not iterable by the StackFrameIteratorForProfiler.
#if V8_TARGET_ARCH_STORES_RETURN_ADDRESS_ON_STACK
  DCHECK_EQ(0, isolate()->isolate_data()->stack_is_iterable());
#endif
  base::ElapsedTimer timer;

  // Determine basic deoptimization information.  The optimized frame is
  // described by the input data.
  DeoptimizationData input_data =
      DeoptimizationData::cast(compiled_code_.deoptimization_data());

  {
    // Read caller's PC, caller's FP and caller's constant pool values
    // from input frame. Compute caller's frame top address.

    Register fp_reg = JavaScriptFrame::fp_register();
    stack_fp_ = input_->GetRegister(fp_reg.code());

    caller_frame_top_ = stack_fp_ + ComputeInputFrameAboveFpFixedSize();

    Address fp_address = input_->GetFramePointerAddress();
    caller_fp_ = Memory<intptr_t>(fp_address);
    caller_pc_ =
        Memory<intptr_t>(fp_address + CommonFrameConstants::kCallerPCOffset);
    actual_argument_count_ = static_cast<int>(
        Memory<intptr_t>(fp_address + StandardFrameConstants::kArgCOffset));

    if (V8_EMBEDDED_CONSTANT_POOL_BOOL) {
      caller_constant_pool_ = Memory<intptr_t>(
          fp_address + CommonFrameConstants::kConstantPoolOffset);
    }
  }

  StackGuard* const stack_guard = isolate()->stack_guard();
  CHECK_GT(static_cast<uintptr_t>(caller_frame_top_),
           stack_guard->real_jslimit());

  BytecodeOffset bytecode_offset =
      input_data.GetBytecodeOffset(deopt_exit_index_);
  ByteArray translations = input_data.TranslationByteArray();
  unsigned translation_index =
      input_data.TranslationIndex(deopt_exit_index_).value();

  if (tracing_enabled()) {
    timer.Start();
    TraceDeoptBegin(input_data.OptimizationId().value(), bytecode_offset);
  }

  FILE* trace_file =
      verbose_tracing_enabled() ? trace_scope()->file() : nullptr;
  TranslationArrayIterator state_iterator(translations, translation_index);
  translated_state_.Init(
      isolate_, input_->GetFramePointerAddress(), stack_fp_, &state_iterator,
      input_data.LiteralArray(), input_->GetRegisterValues(), trace_file,
      function_.IsHeapObject()
          ? function_.shared()
                .internal_formal_parameter_count_without_receiver()
          : 0,
      actual_argument_count_ - kJSArgcReceiverSlots);

  bytecode_offset_in_outermost_frame_ =
      translated_state_.frames()[0].bytecode_offset();

  // Do the input frame to output frame(s) translation.
  size_t count = translated_state_.frames().size();
  if (is_restart_frame()) {
    // If the debugger requested to restart a particular frame, only materialize
    // up to that frame.
    count = restart_frame_index_ + 1;
  } else if (deoptimizing_throw_) {
    // If we are supposed to go to the catch handler, find the catching frame
    // for the catch and make sure we only deoptimize up to that frame.
    size_t catch_handler_frame_index = count;
    for (size_t i = count; i-- > 0;) {
      catch_handler_pc_offset_ = LookupCatchHandler(
          isolate(), &(translated_state_.frames()[i]), &catch_handler_data_);
      if (catch_handler_pc_offset_ >= 0) {
        catch_handler_frame_index = i;
        break;
      }
    }
    CHECK_LT(catch_handler_frame_index, count);
    count = catch_handler_frame_index + 1;
  }

  DCHECK_NULL(output_);
  output_ = new FrameDescription*[count];
  for (size_t i = 0; i < count; ++i) {
    output_[i] = nullptr;
  }
  output_count_ = static_cast<int>(count);

  // Translate each output frame.
  int frame_index = 0;
  size_t total_output_frame_size = 0;
  for (size_t i = 0; i < count; ++i, ++frame_index) {
    TranslatedFrame* translated_frame = &(translated_state_.frames()[i]);
    const bool handle_exception = deoptimizing_throw_ && i == count - 1;
    switch (translated_frame->kind()) {
      case TranslatedFrame::kUnoptimizedFunction:
        DoComputeUnoptimizedFrame(translated_frame, frame_index,
                                  handle_exception);
        break;
      case TranslatedFrame::kInlinedExtraArguments:
        DoComputeInlinedExtraArguments(translated_frame, frame_index);
        break;
      case TranslatedFrame::kConstructStub:
        DoComputeConstructStubFrame(translated_frame, frame_index);
        break;
      case TranslatedFrame::kBuiltinContinuation:
#if V8_ENABLE_WEBASSEMBLY
      case TranslatedFrame::kJSToWasmBuiltinContinuation:
#endif  // V8_ENABLE_WEBASSEMBLY
        DoComputeBuiltinContinuation(translated_frame, frame_index,
                                     BuiltinContinuationMode::STUB);
        break;
      case TranslatedFrame::kJavaScriptBuiltinContinuation:
        DoComputeBuiltinContinuation(translated_frame, frame_index,
                                     BuiltinContinuationMode::JAVASCRIPT);
        break;
      case TranslatedFrame::kJavaScriptBuiltinContinuationWithCatch:
        DoComputeBuiltinContinuation(
            translated_frame, frame_index,
            handle_exception
                ? BuiltinContinuationMode::JAVASCRIPT_HANDLE_EXCEPTION
                : BuiltinContinuationMode::JAVASCRIPT_WITH_CATCH);
        break;
      case TranslatedFrame::kInvalid:
        FATAL("invalid frame");
    }
    total_output_frame_size += output_[frame_index]->GetFrameSize();
  }

  FrameDescription* topmost = output_[count - 1];
  topmost->GetRegisterValues()->SetRegister(kRootRegister.code(),
                                            isolate()->isolate_root());
#ifdef V8_COMPRESS_POINTERS
  topmost->GetRegisterValues()->SetRegister(kPtrComprCageBaseRegister.code(),
                                            isolate()->cage_base());
#endif

  // Print some helpful diagnostic information.
  if (verbose_tracing_enabled()) {
    TraceDeoptEnd(timer.Elapsed().InMillisecondsF());
  }

  // The following invariant is fairly tricky to guarantee, since the size of
  // an optimized frame and its deoptimized counterparts usually differs. We
  // thus need to consider the case in which deoptimized frames are larger than
  // the optimized frame in stack checks in optimized code. We do this by
  // applying an offset to stack checks (see kArchStackPointerGreaterThan in the
  // code generator).
  // Note that we explicitly allow deopts to exceed the limit by a certain
  // number of slack bytes.
  CHECK_GT(
      static_cast<uintptr_t>(caller_frame_top_) - total_output_frame_size,
      stack_guard->real_jslimit() - kStackLimitSlackForDeoptimizationInBytes);
}

namespace {

// Get the dispatch builtin for unoptimized frames.
Builtin DispatchBuiltinFor(bool deopt_to_baseline, bool advance_bc,
                           bool is_restart_frame) {
  if (is_restart_frame) return Builtin::kRestartFrameTrampoline;

  if (deopt_to_baseline) {
    return advance_bc ? Builtin::kBaselineOrInterpreterEnterAtNextBytecode
                      : Builtin::kBaselineOrInterpreterEnterAtBytecode;
  } else {
    return advance_bc ? Builtin::kInterpreterEnterAtNextBytecode
                      : Builtin::kInterpreterEnterAtBytecode;
  }
}

}  // namespace

void Deoptimizer::DoComputeUnoptimizedFrame(TranslatedFrame* translated_frame,
                                            int frame_index,
                                            bool goto_catch_handler) {
  SharedFunctionInfo shared = translated_frame->raw_shared_info();
  TranslatedFrame::iterator value_iterator = translated_frame->begin();
  const bool is_bottommost = (0 == frame_index);
  const bool is_topmost = (output_count_ - 1 == frame_index);

  const int real_bytecode_offset = translated_frame->bytecode_offset().ToInt();
  const int bytecode_offset =
      goto_catch_handler ? catch_handler_pc_offset_ : real_bytecode_offset;

  const int parameters_count =
      shared.internal_formal_parameter_count_with_receiver();

  // If this is the bottom most frame or the previous frame was the inlined
  // extra arguments frame, then we already have extra arguments in the stack
  // (including any extra padding). Therefore we should not try to add any
  // padding.
  bool should_pad_arguments =
      !is_bottommost && (translated_state_.frames()[frame_index - 1]).kind() !=
                            TranslatedFrame::kInlinedExtraArguments;

  const int locals_count = translated_frame->height();
  UnoptimizedFrameInfo frame_info = UnoptimizedFrameInfo::Precise(
      parameters_count, locals_count, is_topmost, should_pad_arguments);
  const uint32_t output_frame_size = frame_info.frame_size_in_bytes();

  TranslatedFrame::iterator function_iterator = value_iterator++;

  BytecodeArray bytecode_array =
      shared.HasBreakInfo() ? shared.GetDebugInfo().DebugBytecodeArray()
                            : shared.GetBytecodeArray(isolate());

  // Allocate and store the output frame description.
  FrameDescription* output_frame = new (output_frame_size)
      FrameDescription(output_frame_size, parameters_count, isolate());
  FrameWriter frame_writer(this, output_frame, verbose_trace_scope());

  CHECK(frame_index >= 0 && frame_index < output_count_);
  CHECK_NULL(output_[frame_index]);
  output_[frame_index] = output_frame;

  // Compute this frame's PC and state.
  // For interpreted frames, the PC will be a special builtin that
  // continues the bytecode dispatch. Note that non-topmost and lazy-style
  // bailout handlers also advance the bytecode offset before dispatch, hence
  // simulating what normal handlers do upon completion of the operation.
  // For baseline frames, the PC will be a builtin to convert the interpreter
  // frame to a baseline frame before continuing execution of baseline code.
  // We can't directly continue into baseline code, because of CFI.
  Builtins* builtins = isolate_->builtins();
  const bool advance_bc =
      (!is_topmost || (deopt_kind_ == DeoptimizeKind::kLazy)) &&
      !goto_catch_handler;
  const bool deopt_to_baseline =
      shared.HasBaselineCode() && v8_flags.deopt_to_baseline;
  const bool restart_frame = goto_catch_handler && is_restart_frame();
  Code dispatch_builtin = builtins->code(
      DispatchBuiltinFor(deopt_to_baseline, advance_bc, restart_frame));

  if (verbose_tracing_enabled()) {
    PrintF(trace_scope()->file(), "  translating %s frame ",
           deopt_to_baseline ? "baseline" : "interpreted");
    std::unique_ptr<char[]> name = shared.DebugNameCStr();
    PrintF(trace_scope()->file(), "%s", name.get());
    PrintF(trace_scope()->file(), " => bytecode_offset=%d, ",
           real_bytecode_offset);
    PrintF(trace_scope()->file(), "variable_frame_size=%d, frame_size=%d%s\n",
           frame_info.frame_size_in_bytes_without_fixed(), output_frame_size,
           goto_catch_handler ? " (throw)" : "");
  }

  // The top address of the frame is computed from the previous frame's top and
  // this frame's size.
  const intptr_t top_address =
      is_bottommost ? caller_frame_top_ - output_frame_size
                    : output_[frame_index - 1]->GetTop() - output_frame_size;
  output_frame->SetTop(top_address);

  // Compute the incoming parameter translation.
  ReadOnlyRoots roots(isolate());
  if (should_pad_arguments) {
    for (int i = 0; i < ArgumentPaddingSlots(parameters_count); ++i) {
      frame_writer.PushRawObject(roots.the_hole_value(), "padding\n");
    }
  }

  if (verbose_tracing_enabled() && is_bottommost &&
      actual_argument_count_ > parameters_count) {
    PrintF(trace_scope_->file(),
           "    -- %d extra argument(s) already in the stack --\n",
           actual_argument_count_ - parameters_count);
  }
  frame_writer.PushStackJSArguments(value_iterator, parameters_count);

  DCHECK_EQ(output_frame->GetLastArgumentSlotOffset(should_pad_arguments),
            frame_writer.top_offset());
  if (verbose_tracing_enabled()) {
    PrintF(trace_scope()->file(), "    -------------------------\n");
  }

  // There are no translation commands for the caller's pc and fp, the
  // context, the function and the bytecode offset.  Synthesize
  // their values and set them up
  // explicitly.
  //
  // The caller's pc for the bottommost output frame is the same as in the
  // input frame. For all subsequent output frames, it can be read from the
  // previous one. This frame's pc can be computed from the non-optimized
  // function code and bytecode offset of the bailout.
  if (is_bottommost) {
    frame_writer.PushBottommostCallerPc(caller_pc_);
  } else {
    frame_writer.PushApprovedCallerPc(output_[frame_index - 1]->GetPc());
  }

  // The caller's frame pointer for the bottommost output frame is the same
  // as in the input frame.  For all subsequent output frames, it can be
  // read from the previous one.  Also compute and set this frame's frame
  // pointer.
  const intptr_t caller_fp =
      is_bottommost ? caller_fp_ : output_[frame_index - 1]->GetFp();
  frame_writer.PushCallerFp(caller_fp);

  const intptr_t fp_value = top_address + frame_writer.top_offset();
  output_frame->SetFp(fp_value);
  if (is_topmost) {
    Register fp_reg = UnoptimizedFrame::fp_register();
    output_frame->SetRegister(fp_reg.code(), fp_value);
  }

  if (V8_EMBEDDED_CONSTANT_POOL_BOOL) {
    // For the bottommost output frame the constant pool pointer can be gotten
    // from the input frame. For subsequent output frames, it can be read from
    // the previous frame.
    const intptr_t caller_cp =
        is_bottommost ? caller_constant_pool_
                      : output_[frame_index - 1]->GetConstantPool();
    frame_writer.PushCallerConstantPool(caller_cp);
  }

  // For the bottommost output frame the context can be gotten from the input
  // frame. For all subsequent output frames it can be gotten from the function
  // so long as we don't inline functions that need local contexts.

  // When deoptimizing into a catch block, we need to take the context
  // from a register that was specified in the handler table.
  TranslatedFrame::iterator context_pos = value_iterator++;
  if (goto_catch_handler) {
    // Skip to the translated value of the register specified
    // in the handler table.
    for (int i = 0; i < catch_handler_data_ + 1; ++i) {
      context_pos++;
    }
  }
  // Read the context from the translations.
  Object context = context_pos->GetRawValue();
  output_frame->SetContext(static_cast<intptr_t>(context.ptr()));
  frame_writer.PushTranslatedValue(context_pos, "context");

  // The function was mentioned explicitly in the BEGIN_FRAME.
  frame_writer.PushTranslatedValue(function_iterator, "function");

  // Actual argument count.
  int argc;
  if (is_bottommost) {
    argc = actual_argument_count_;
  } else {
    TranslatedFrame::Kind previous_frame_kind =
        (translated_state_.frames()[frame_index - 1]).kind();
    argc = previous_frame_kind == TranslatedFrame::kInlinedExtraArguments
               ? output_[frame_index - 1]->parameter_count()
               : parameters_count;
  }
  frame_writer.PushRawValue(argc, "actual argument count\n");

  // Set the bytecode array pointer.
  frame_writer.PushRawObject(bytecode_array, "bytecode array\n");

  // The bytecode offset was mentioned explicitly in the BEGIN_FRAME.
  const int raw_bytecode_offset =
      BytecodeArray::kHeaderSize - kHeapObjectTag + bytecode_offset;
  Smi smi_bytecode_offset = Smi::FromInt(raw_bytecode_offset);
  frame_writer.PushRawObject(smi_bytecode_offset, "bytecode offset\n");

  if (verbose_tracing_enabled()) {
    PrintF(trace_scope()->file(), "    -------------------------\n");
  }

  // Translate the rest of the interpreter registers in the frame.
  // The return_value_offset is counted from the top. Here, we compute the
  // register index (counted from the start).
  const int return_value_first_reg =
      locals_count - translated_frame->return_value_offset();
  const int return_value_count = translated_frame->return_value_count();
  for (int i = 0; i < locals_count; ++i, ++value_iterator) {
    // Ensure we write the return value if we have one and we are returning
    // normally to a lazy deopt point.
    if (is_topmost && !goto_catch_handler &&
        deopt_kind_ == DeoptimizeKind::kLazy && i >= return_value_first_reg &&
        i < return_value_first_reg + return_value_count) {
      const int return_index = i - return_value_first_reg;
      if (return_index == 0) {
        frame_writer.PushRawValue(input_->GetRegister(kReturnRegister0.code()),
                                  "return value 0\n");
        // We do not handle the situation when one return value should go into
        // the accumulator and another one into an ordinary register. Since
        // the interpreter should never create such situation, just assert
        // this does not happen.
        CHECK_LE(return_value_first_reg + return_value_count, locals_count);
      } else {
        CHECK_EQ(return_index, 1);
        frame_writer.PushRawValue(input_->GetRegister(kReturnRegister1.code()),
                                  "return value 1\n");
      }
    } else {
      // This is not return value, just write the value from the translations.
      frame_writer.PushTranslatedValue(value_iterator, "stack parameter");
    }
  }

  uint32_t register_slots_written = static_cast<uint32_t>(locals_count);
  DCHECK_LE(register_slots_written, frame_info.register_stack_slot_count());
  // Some architectures must pad the stack frame with extra stack slots
  // to ensure the stack frame is aligned. Do this now.
  while (register_slots_written < frame_info.register_stack_slot_count()) {
    register_slots_written++;
    frame_writer.PushRawObject(roots.the_hole_value(), "padding\n");
  }

  // Translate the accumulator register (depending on frame position).
  if (is_topmost) {
    for (int i = 0; i < ArgumentPaddingSlots(1); ++i) {
      frame_writer.PushRawObject(roots.the_hole_value(), "padding\n");
    }
    // For topmost frame, put the accumulator on the stack. The
    // {NotifyDeoptimized} builtin pops it off the topmost frame (possibly
    // after materialization).
    if (goto_catch_handler) {
      // If we are lazy deopting to a catch handler, we set the accumulator to
      // the exception (which lives in the result register).
      intptr_t accumulator_value =
          input_->GetRegister(kInterpreterAccumulatorRegister.code());
      frame_writer.PushRawObject(Object(accumulator_value), "accumulator\n");
    } else {
      // If we are lazily deoptimizing make sure we store the deopt
      // return value into the appropriate slot.
      if (deopt_kind_ == DeoptimizeKind::kLazy &&
          translated_frame->return_value_offset() == 0 &&
          translated_frame->return_value_count() > 0) {
        CHECK_EQ(translated_frame->return_value_count(), 1);
        frame_writer.PushRawValue(input_->GetRegister(kReturnRegister0.code()),
                                  "return value 0\n");
      } else {
        frame_writer.PushTranslatedValue(value_iterator, "accumulator");
      }
    }
    ++value_iterator;  // Move over the accumulator.
  } else {
    // For non-topmost frames, skip the accumulator translation. For those
    // frames, the return value from the callee will become the accumulator.
    ++value_iterator;
  }
  CHECK_EQ(translated_frame->end(), value_iterator);
  CHECK_EQ(0u, frame_writer.top_offset());

  const intptr_t pc =
      static_cast<intptr_t>(dispatch_builtin.instruction_start());
  if (is_topmost) {
    // Only the pc of the topmost frame needs to be signed since it is
    // authenticated at the end of the DeoptimizationEntry builtin.
    const intptr_t top_most_pc = PointerAuthentication::SignAndCheckPC(
        isolate(), pc, frame_writer.frame()->GetTop());
    output_frame->SetPc(top_most_pc);
  } else {
    output_frame->SetPc(pc);
  }

  // Update constant pool.
  if (V8_EMBEDDED_CONSTANT_POOL_BOOL) {
    intptr_t constant_pool_value =
        static_cast<intptr_t>(dispatch_builtin.constant_pool());
    output_frame->SetConstantPool(constant_pool_value);
    if (is_topmost) {
      Register constant_pool_reg =
          UnoptimizedFrame::constant_pool_pointer_register();
      output_frame->SetRegister(constant_pool_reg.code(), constant_pool_value);
    }
  }

  // Clear the context register. The context might be a de-materialized object
  // and will be materialized by {Runtime_NotifyDeoptimized}. For additional
  // safety we use Smi(0) instead of the potential {arguments_marker} here.
  if (is_topmost) {
    intptr_t context_value = static_cast<intptr_t>(Smi::zero().ptr());
    Register context_reg = JavaScriptFrame::context_register();
    output_frame->SetRegister(context_reg.code(), context_value);
    // Set the continuation for the topmost frame.
    Code continuation = builtins->code(Builtin::kNotifyDeoptimized);
    output_frame->SetContinuation(
        static_cast<intptr_t>(continuation.instruction_start()));
  }
}

void Deoptimizer::DoComputeInlinedExtraArguments(
    TranslatedFrame* translated_frame, int frame_index) {
  // Inlined arguments frame can not be the topmost, nor the bottom most frame.
  CHECK(frame_index < output_count_ - 1);
  CHECK_GT(frame_index, 0);
  CHECK_NULL(output_[frame_index]);

  // During deoptimization we need push the extra arguments of inlined functions
  // (arguments with index greater than the formal parameter count).
  // For more info, see the design document:
  // https://docs.google.com/document/d/150wGaUREaZI6YWqOQFD5l2mWQXaPbbZjcAIJLOFrzMs

  TranslatedFrame::iterator value_iterator = translated_frame->begin();
  const int argument_count_without_receiver = translated_frame->height() - 1;
  const int formal_parameter_count =
      translated_frame->raw_shared_info()
          .internal_formal_parameter_count_without_receiver();
  const int extra_argument_count =
      argument_count_without_receiver - formal_parameter_count;
  // The number of pushed arguments is the maximum of the actual argument count
  // and the formal parameter count + the receiver.
  const int padding = ArgumentPaddingSlots(
      std::max(argument_count_without_receiver, formal_parameter_count) + 1);
  const int output_frame_size =
      (std::max(0, extra_argument_count) + padding) * kSystemPointerSize;
  if (verbose_tracing_enabled()) {
    PrintF(trace_scope_->file(),
           "  translating inlined arguments frame => variable_size=%d\n",
           output_frame_size);
  }

  // Allocate and store the output frame description.
  FrameDescription* output_frame = new (output_frame_size) FrameDescription(
      output_frame_size, JSParameterCount(argument_count_without_receiver),
      isolate());
  // The top address of the frame is computed from the previous frame's top and
  // this frame's size.
  const intptr_t top_address =
      output_[frame_index - 1]->GetTop() - output_frame_size;
  output_frame->SetTop(top_address);
  // This is not a real frame, we take PC and FP values from the parent frame.
  output_frame->SetPc(output_[frame_index - 1]->GetPc());
  output_frame->SetFp(output_[frame_index - 1]->GetFp());
  output_[frame_index] = output_frame;

  FrameWriter frame_writer(this, output_frame, verbose_trace_scope());

  ReadOnlyRoots roots(isolate());
  for (int i = 0; i < padding; ++i) {
    frame_writer.PushRawObject(roots.the_hole_value(), "padding\n");
  }

  if (extra_argument_count > 0) {
    // The receiver and arguments with index below the formal parameter
    // count are in the fake adaptor frame, because they are used to create the
    // arguments object. We should however not push them, since the interpreter
    // frame with do that.
    value_iterator++;  // Skip function.
    value_iterator++;  // Skip receiver.
    for (int i = 0; i < formal_parameter_count; i++) value_iterator++;
    frame_writer.PushStackJSArguments(value_iterator, extra_argument_count);
  }
}

void Deoptimizer::DoComputeConstructStubFrame(TranslatedFrame* translated_frame,
                                              int frame_index) {
  TranslatedFrame::iterator value_iterator = translated_frame->begin();
  const bool is_topmost = (output_count_ - 1 == frame_index);
  // The construct frame could become topmost only if we inlined a constructor
  // call which does a tail call (otherwise the tail callee's frame would be
  // the topmost one). So it could only be the DeoptimizeKind::kLazy case.
  CHECK(!is_topmost || deopt_kind_ == DeoptimizeKind::kLazy);

  Builtins* builtins = isolate_->builtins();
  Code construct_stub = builtins->code(Builtin::kJSConstructStubGeneric);
  BytecodeOffset bytecode_offset = translated_frame->bytecode_offset();

  const int parameters_count = translated_frame->height();
  ConstructStubFrameInfo frame_info =
      ConstructStubFrameInfo::Precise(parameters_count, is_topmost);
  const uint32_t output_frame_size = frame_info.frame_size_in_bytes();

  TranslatedFrame::iterator function_iterator = value_iterator++;
  if (verbose_tracing_enabled()) {
    PrintF(trace_scope()->file(),
           "  translating construct stub => bytecode_offset=%d (%s), "
           "variable_frame_size=%d, frame_size=%d\n",
           bytecode_offset.ToInt(),
           bytecode_offset == BytecodeOffset::ConstructStubCreate() ? "create"
                                                                    : "invoke",
           frame_info.frame_size_in_bytes_without_fixed(), output_frame_size);
  }

  // Allocate and store the output frame description.
  FrameDescription* output_frame = new (output_frame_size)
      FrameDescription(output_frame_size, parameters_count, isolate());
  FrameWriter frame_writer(this, output_frame, verbose_trace_scope());

  // Construct stub can not be topmost.
  DCHECK(frame_index > 0 && frame_index < output_count_);
  DCHECK_NULL(output_[frame_index]);
  output_[frame_index] = output_frame;

  // The top address of the frame is computed from the previous frame's top and
  // this frame's size.
  const intptr_t top_address =
      output_[frame_index - 1]->GetTop() - output_frame_size;
  output_frame->SetTop(top_address);

  ReadOnlyRoots roots(isolate());
  for (int i = 0; i < ArgumentPaddingSlots(parameters_count); ++i) {
    frame_writer.PushRawObject(roots.the_hole_value(), "padding\n");
  }

  // The allocated receiver of a construct stub frame is passed as the
  // receiver parameter through the translation. It might be encoding
  // a captured object, so we need save it for later.
  TranslatedFrame::iterator receiver_iterator = value_iterator;

  // Compute the incoming parameter translation.
  frame_writer.PushStackJSArguments(value_iterator, parameters_count);

  DCHECK_EQ(output_frame->GetLastArgumentSlotOffset(),
            frame_writer.top_offset());

  // Read caller's PC from the previous frame.
  const intptr_t caller_pc = output_[frame_index - 1]->GetPc();
  frame_writer.PushApprovedCallerPc(caller_pc);

  // Read caller's FP from the previous frame, and set this frame's FP.
  const intptr_t caller_fp = output_[frame_index - 1]->GetFp();
  frame_writer.PushCallerFp(caller_fp);

  const intptr_t fp_value = top_address + frame_writer.top_offset();
  output_frame->SetFp(fp_value);
  if (is_topmost) {
    Register fp_reg = JavaScriptFrame::fp_register();
    output_frame->SetRegister(fp_reg.code(), fp_value);
  }

  if (V8_EMBEDDED_CONSTANT_POOL_BOOL) {
    // Read the caller's constant pool from the previous frame.
    const intptr_t caller_cp = output_[frame_index - 1]->GetConstantPool();
    frame_writer.PushCallerConstantPool(caller_cp);
  }

  // A marker value is used to mark the frame.
  intptr_t marker = StackFrame::TypeToMarker(StackFrame::CONSTRUCT);
  frame_writer.PushRawValue(marker, "context (construct stub sentinel)\n");

  frame_writer.PushTranslatedValue(value_iterator++, "context");

  // Number of incoming arguments.
  const uint32_t argc = parameters_count;
  frame_writer.PushRawObject(Smi::FromInt(argc), "argc\n");

  // The constructor function was mentioned explicitly in the
  // CONSTRUCT_STUB_FRAME.
  frame_writer.PushTranslatedValue(function_iterator, "constructor function\n");

  // The deopt info contains the implicit receiver or the new target at the
  // position of the receiver. Copy it to the top of stack, with the hole value
  // as padding to maintain alignment.

  frame_writer.PushRawObject(roots.the_hole_value(), "padding\n");

  CHECK(bytecode_offset == BytecodeOffset::ConstructStubCreate() ||
        bytecode_offset == BytecodeOffset::ConstructStubInvoke());
  const char* debug_hint =
      bytecode_offset == BytecodeOffset::ConstructStubCreate()
          ? "new target\n"
          : "allocated receiver\n";
  frame_writer.PushTranslatedValue(receiver_iterator, debug_hint);

  if (is_topmost) {
    for (int i = 0; i < ArgumentPaddingSlots(1); ++i) {
      frame_writer.PushRawObject(roots.the_hole_value(), "padding\n");
    }
    // Ensure the result is restored back when we return to the stub.
    Register result_reg = kReturnRegister0;
    intptr_t result = input_->GetRegister(result_reg.code());
    frame_writer.PushRawValue(result, "subcall result\n");
  }

  CHECK_EQ(translated_frame->end(), value_iterator);
  CHECK_EQ(0u, frame_writer.top_offset());

  // Compute this frame's PC.
  DCHECK(bytecode_offset.IsValidForConstructStub());
  Address start = construct_stub.instruction_start();
  const int pc_offset =
      bytecode_offset == BytecodeOffset::ConstructStubCreate()
          ? isolate_->heap()->construct_stub_create_deopt_pc_offset().value()
          : isolate_->heap()->construct_stub_invoke_deopt_pc_offset().value();
  intptr_t pc_value = static_cast<intptr_t>(start + pc_offset);
  if (is_topmost) {
    // Only the pc of the topmost frame needs to be signed since it is
    // authenticated at the end of the DeoptimizationEntry builtin.
    output_frame->SetPc(PointerAuthentication::SignAndCheckPC(
        isolate(), pc_value, frame_writer.frame()->GetTop()));
  } else {
    output_frame->SetPc(pc_value);
  }

  // Update constant pool.
  if (V8_EMBEDDED_CONSTANT_POOL_BOOL) {
    intptr_t constant_pool_value =
        static_cast<intptr_t>(construct_stub.constant_pool());
    output_frame->SetConstantPool(constant_pool_value);
    if (is_topmost) {
      Register constant_pool_reg =
          JavaScriptFrame::constant_pool_pointer_register();
      output_frame->SetRegister(constant_pool_reg.code(), constant_pool_value);
    }
  }

  // Clear the context register. The context might be a de-materialized object
  // and will be materialized by {Runtime_NotifyDeoptimized}. For additional
  // safety we use Smi(0) instead of the potential {arguments_marker} here.
  if (is_topmost) {
    intptr_t context_value = static_cast<intptr_t>(Smi::zero().ptr());
    Register context_reg = JavaScriptFrame::context_register();
    output_frame->SetRegister(context_reg.code(), context_value);
  }

  // Set the continuation for the topmost frame.
  if (is_topmost) {
    DCHECK_EQ(DeoptimizeKind::kLazy, deopt_kind_);
    Code continuation = builtins->code(Builtin::kNotifyDeoptimized);
    output_frame->SetContinuation(
        static_cast<intptr_t>(continuation.instruction_start()));
  }
}

namespace {

bool BuiltinContinuationModeIsJavaScript(BuiltinContinuationMode mode) {
  switch (mode) {
    case BuiltinContinuationMode::STUB:
      return false;
    case BuiltinContinuationMode::JAVASCRIPT:
    case BuiltinContinuationMode::JAVASCRIPT_WITH_CATCH:
    case BuiltinContinuationMode::JAVASCRIPT_HANDLE_EXCEPTION:
      return true;
  }
  UNREACHABLE();
}

StackFrame::Type BuiltinContinuationModeToFrameType(
    BuiltinContinuationMode mode) {
  switch (mode) {
    case BuiltinContinuationMode::STUB:
      return StackFrame::BUILTIN_CONTINUATION;
    case BuiltinContinuationMode::JAVASCRIPT:
      return StackFrame::JAVA_SCRIPT_BUILTIN_CONTINUATION;
    case BuiltinContinuationMode::JAVASCRIPT_WITH_CATCH:
      return StackFrame::JAVA_SCRIPT_BUILTIN_CONTINUATION_WITH_CATCH;
    case BuiltinContinuationMode::JAVASCRIPT_HANDLE_EXCEPTION:
      return StackFrame::JAVA_SCRIPT_BUILTIN_CONTINUATION_WITH_CATCH;
  }
  UNREACHABLE();
}

}  // namespace

Builtin Deoptimizer::TrampolineForBuiltinContinuation(
    BuiltinContinuationMode mode, bool must_handle_result) {
  switch (mode) {
    case BuiltinContinuationMode::STUB:
      return must_handle_result ? Builtin::kContinueToCodeStubBuiltinWithResult
                                : Builtin::kContinueToCodeStubBuiltin;
    case BuiltinContinuationMode::JAVASCRIPT:
    case BuiltinContinuationMode::JAVASCRIPT_WITH_CATCH:
    case BuiltinContinuationMode::JAVASCRIPT_HANDLE_EXCEPTION:
      return must_handle_result
                 ? Builtin::kContinueToJavaScriptBuiltinWithResult
                 : Builtin::kContinueToJavaScriptBuiltin;
  }
  UNREACHABLE();
}

#if V8_ENABLE_WEBASSEMBLY
TranslatedValue Deoptimizer::TranslatedValueForWasmReturnKind(
    base::Optional<wasm::ValueKind> wasm_call_return_kind) {
  if (wasm_call_return_kind) {
    switch (wasm_call_return_kind.value()) {
      case wasm::kI32:
        return TranslatedValue::NewInt32(
            &translated_state_,
            (int32_t)input_->GetRegister(kReturnRegister0.code()));
      case wasm::kI64:
        return TranslatedValue::NewInt64ToBigInt(
            &translated_state_,
            (int64_t)input_->GetRegister(kReturnRegister0.code()));
      case wasm::kF32:
        return TranslatedValue::NewFloat(
            &translated_state_,
            Float32(*reinterpret_cast<float*>(
                input_->GetDoubleRegister(wasm::kFpReturnRegisters[0].code())
                    .get_bits_address())));
      case wasm::kF64:
        return TranslatedValue::NewDouble(
            &translated_state_,
            input_->GetDoubleRegister(wasm::kFpReturnRegisters[0].code()));
      default:
        UNREACHABLE();
    }
  }
  return TranslatedValue::NewTagged(&translated_state_,
                                    ReadOnlyRoots(isolate()).undefined_value());
}
#endif  // V8_ENABLE_WEBASSEMBLY

// BuiltinContinuationFrames capture the machine state that is expected as input
// to a builtin, including both input register values and stack parameters. When
// the frame is reactivated (i.e. the frame below it returns), a
// ContinueToBuiltin stub restores the register state from the frame and tail
// calls to the actual target builtin, making it appear that the stub had been
// directly called by the frame above it. The input values to populate the frame
// are taken from the deopt's FrameState.
//
// Frame translation happens in two modes, EAGER and LAZY. In EAGER mode, all of
// the parameters to the Builtin are explicitly specified in the TurboFan
// FrameState node. In LAZY mode, there is always one fewer parameters specified
// in the FrameState than expected by the Builtin. In that case, construction of
// BuiltinContinuationFrame adds the final missing parameter during
// deoptimization, and that parameter is always on the stack and contains the
// value returned from the callee of the call site triggering the LAZY deopt
// (e.g. rax on x64). This requires that continuation Builtins for LAZY deopts
// must have at least one stack parameter.
//
//                TO
//    |          ....           |
//    +-------------------------+
//    | arg padding (arch dept) |<- at most 1*kSystemPointerSize
//    +-------------------------+
//    |     builtin param 0     |<- FrameState input value n becomes
//    +-------------------------+
//    |           ...           |
//    +-------------------------+
//    |     builtin param m     |<- FrameState input value n+m-1, or in
//    +-----needs-alignment-----+   the LAZY case, return LAZY result value
//    | ContinueToBuiltin entry |
//    +-------------------------+
// |  |    saved frame (FP)     |
// |  +=====needs=alignment=====+<- fpreg
// |  |constant pool (if ool_cp)|
// v  +-------------------------+
//    |BUILTIN_CONTINUATION mark|
//    +-------------------------+
//    |  JSFunction (or zero)   |<- only if JavaScript builtin
//    +-------------------------+
//    |  frame height above FP  |
//    +-------------------------+
//    |         context         |<- this non-standard context slot contains
//    +-------------------------+   the context, even for non-JS builtins.
//    |      builtin index      |
//    +-------------------------+
//    | builtin input GPR reg0  |<- populated from deopt FrameState using
//    +-------------------------+   the builtin's CallInterfaceDescriptor
//    |          ...            |   to map a FrameState's 0..n-1 inputs to
//    +-------------------------+   the builtin's n input register params.
//    | builtin input GPR regn  |
//    +-------------------------+
//    | reg padding (arch dept) |
//    +-----needs--alignment----+
//    | res padding (arch dept) |<- only if {is_topmost}; result is pop'd by
//    +-------------------------+<- kNotifyDeopt ASM stub and moved to acc
//    |      result  value      |<- reg, as ContinueToBuiltin stub expects.
//    +-----needs-alignment-----+<- spreg
//
void Deoptimizer::DoComputeBuiltinContinuation(
    TranslatedFrame* translated_frame, int frame_index,
    BuiltinContinuationMode mode) {
  TranslatedFrame::iterator result_iterator = translated_frame->end();

  bool is_js_to_wasm_builtin_continuation = false;
#if V8_ENABLE_WEBASSEMBLY
  is_js_to_wasm_builtin_continuation =
      translated_frame->kind() == TranslatedFrame::kJSToWasmBuiltinContinuation;
  if (is_js_to_wasm_builtin_continuation) {
    // For JSToWasmBuiltinContinuations, add a TranslatedValue with the result
    // of the Wasm call, extracted from the input FrameDescription.
    // This TranslatedValue will be written in the output frame in place of the
    // hole and we'll use ContinueToCodeStubBuiltin in place of
    // ContinueToCodeStubBuiltinWithResult.
    TranslatedValue result = TranslatedValueForWasmReturnKind(
        translated_frame->wasm_call_return_kind());
    translated_frame->Add(result);
  }
#endif  // V8_ENABLE_WEBASSEMBLY

  TranslatedFrame::iterator value_iterator = translated_frame->begin();

  const BytecodeOffset bytecode_offset = translated_frame->bytecode_offset();
  Builtin builtin = Builtins::GetBuiltinFromBytecodeOffset(bytecode_offset);
  CallInterfaceDescriptor continuation_descriptor =
      Builtins::CallInterfaceDescriptorFor(builtin);

  const RegisterConfiguration* config = RegisterConfiguration::Default();

  const bool is_bottommost = (0 == frame_index);
  const bool is_topmost = (output_count_ - 1 == frame_index);

  const int parameters_count = translated_frame->height();
  BuiltinContinuationFrameInfo frame_info =
      BuiltinContinuationFrameInfo::Precise(parameters_count,
                                            continuation_descriptor, config,
                                            is_topmost, deopt_kind_, mode);

  const unsigned output_frame_size = frame_info.frame_size_in_bytes();
  const unsigned output_frame_size_above_fp =
      frame_info.frame_size_in_bytes_above_fp();

  // Validate types of parameters. They must all be tagged except for argc for
  // JS builtins.
  bool has_argc = false;
  const int register_parameter_count =
      continuation_descriptor.GetRegisterParameterCount();
  for (int i = 0; i < register_parameter_count; ++i) {
    MachineType type = continuation_descriptor.GetParameterType(i);
    int code = continuation_descriptor.GetRegisterParameter(i).code();
    // Only tagged and int32 arguments are supported, and int32 only for the
    // arguments count on JavaScript builtins.
    if (type == MachineType::Int32()) {
      CHECK_EQ(code, kJavaScriptCallArgCountRegister.code());
      has_argc = true;
    } else {
      // Any other argument must be a tagged value.
      CHECK(IsAnyTagged(type.representation()));
    }
  }
  CHECK_EQ(BuiltinContinuationModeIsJavaScript(mode), has_argc);

  if (verbose_tracing_enabled()) {
    PrintF(trace_scope()->file(),
           "  translating BuiltinContinuation to %s,"
           " => register_param_count=%d,"
           " stack_param_count=%d, frame_size=%d\n",
           Builtins::name(builtin), register_parameter_count,
           frame_info.stack_parameter_count(), output_frame_size);
  }

  FrameDescription* output_frame = new (output_frame_size) FrameDescription(
      output_frame_size, frame_info.stack_parameter_count(), isolate());
  output_[frame_index] = output_frame;
  FrameWriter frame_writer(this, output_frame, verbose_trace_scope());

  // The top address of the frame is computed from the previous frame's top and
  // this frame's size.
  const intptr_t top_address =
      is_bottommost ? caller_frame_top_ - output_frame_size
                    : output_[frame_index - 1]->GetTop() - output_frame_size;
  output_frame->SetTop(top_address);

  // Get the possible JSFunction for the case that this is a
  // JavaScriptBuiltinContinuationFrame, which needs the JSFunction pointer
  // like a normal JavaScriptFrame.
  const intptr_t maybe_function = value_iterator->GetRawValue().ptr();
  ++value_iterator;

  ReadOnlyRoots roots(isolate());
  const int padding = ArgumentPaddingSlots(frame_info.stack_parameter_count());
  for (int i = 0; i < padding; ++i) {
    frame_writer.PushRawObject(roots.the_hole_value(), "padding\n");
  }

  if (mode == BuiltinContinuationMode::STUB) {
    DCHECK_EQ(
        Builtins::CallInterfaceDescriptorFor(builtin).GetStackArgumentOrder(),
        StackArgumentOrder::kDefault);
    for (uint32_t i = 0; i < frame_info.translated_stack_parameter_count();
         ++i, ++value_iterator) {
      frame_writer.PushTranslatedValue(value_iterator, "stack parameter");
    }
    if (frame_info.frame_has_result_stack_slot()) {
      if (is_js_to_wasm_builtin_continuation) {
        frame_writer.PushTranslatedValue(result_iterator,
                                         "return result on lazy deopt\n");
      } else {
        DCHECK_EQ(result_iterator, translated_frame->end());
        frame_writer.PushRawObject(
            roots.the_hole_value(),
            "placeholder for return result on lazy deopt\n");
      }
    }
  } else {
    // JavaScript builtin.
    if (frame_info.frame_has_result_stack_slot()) {
      frame_writer.PushRawObject(
          roots.the_hole_value(),
          "placeholder for return result on lazy deopt\n");
    }
    switch (mode) {
      case BuiltinContinuationMode::STUB:
        UNREACHABLE();
      case BuiltinContinuationMode::JAVASCRIPT:
        break;
      case BuiltinContinuationMode::JAVASCRIPT_WITH_CATCH: {
        frame_writer.PushRawObject(roots.the_hole_value(),
                                   "placeholder for exception on lazy deopt\n");
      } break;
      case BuiltinContinuationMode::JAVASCRIPT_HANDLE_EXCEPTION: {
        intptr_t accumulator_value =
            input_->GetRegister(kInterpreterAccumulatorRegister.code());
        frame_writer.PushRawObject(Object(accumulator_value),
                                   "exception (from accumulator)\n");
      } break;
    }
    frame_writer.PushStackJSArguments(
        value_iterator, frame_info.translated_stack_parameter_count());
  }

  DCHECK_EQ(output_frame->GetLastArgumentSlotOffset(),
            frame_writer.top_offset());

  std::vector<TranslatedFrame::iterator> register_values;
  int total_registers = config->num_general_registers();
  register_values.resize(total_registers, {value_iterator});

  for (int i = 0; i < register_parameter_count; ++i, ++value_iterator) {
    int code = continuation_descriptor.GetRegisterParameter(i).code();
    register_values[code] = value_iterator;
  }

  // The context register is always implicit in the CallInterfaceDescriptor but
  // its register must be explicitly set when continuing to the builtin. Make
  // sure that it's harvested from the translation and copied into the register
  // set (it was automatically added at the end of the FrameState by the
  // instruction selector).
  Object context = value_iterator->GetRawValue();
  const intptr_t value = context.ptr();
  TranslatedFrame::iterator context_register_value = value_iterator++;
  register_values[kContextRegister.code()] = context_register_value;
  output_frame->SetContext(value);
  output_frame->SetRegister(kContextRegister.code(), value);

  // Set caller's PC (JSFunction continuation).
  if (is_bottommost) {
    frame_writer.PushBottommostCallerPc(caller_pc_);
  } else {
    frame_writer.PushApprovedCallerPc(output_[frame_index - 1]->GetPc());
  }

  // Read caller's FP from the previous frame, and set this frame's FP.
  const intptr_t caller_fp =
      is_bottommost ? caller_fp_ : output_[frame_index - 1]->GetFp();
  frame_writer.PushCallerFp(caller_fp);

  const intptr_t fp_value = top_address + frame_writer.top_offset();
  output_frame->SetFp(fp_value);

  DCHECK_EQ(output_frame_size_above_fp, frame_writer.top_offset());

  if (V8_EMBEDDED_CONSTANT_POOL_BOOL) {
    // Read the caller's constant pool from the previous frame.
    const intptr_t caller_cp =
        is_bottommost ? caller_constant_pool_
                      : output_[frame_index - 1]->GetConstantPool();
    frame_writer.PushCallerConstantPool(caller_cp);
  }

  // A marker value is used in place of the context.
  const intptr_t marker =
      StackFrame::TypeToMarker(BuiltinContinuationModeToFrameType(mode));
  frame_writer.PushRawValue(marker,
                            "context (builtin continuation sentinel)\n");

  if (BuiltinContinuationModeIsJavaScript(mode)) {
    frame_writer.PushRawValue(maybe_function, "JSFunction\n");
  } else {
    frame_writer.PushRawValue(0, "unused\n");
  }

  // The delta from the SP to the FP; used to reconstruct SP in
  // Isolate::UnwindAndFindHandler.
  frame_writer.PushRawObject(Smi::FromInt(output_frame_size_above_fp),
                             "frame height at deoptimization\n");

  // The context even if this is a stub continuation frame. We can't use the
  // usual context slot, because we must store the frame marker there.
  frame_writer.PushTranslatedValue(context_register_value,
                                   "builtin JavaScript context\n");

  // The builtin to continue to.
  frame_writer.PushRawObject(Smi::FromInt(static_cast<int>(builtin)),
                             "builtin index\n");

  const int allocatable_register_count =
      config->num_allocatable_general_registers();
  for (int i = 0; i < allocatable_register_count; ++i) {
    int code = config->GetAllocatableGeneralCode(i);
    base::ScopedVector<char> str(128);
    if (verbose_tracing_enabled()) {
      if (BuiltinContinuationModeIsJavaScript(mode) &&
          code == kJavaScriptCallArgCountRegister.code()) {
        SNPrintF(
            str,
            "tagged argument count %s (will be untagged by continuation)\n",
            RegisterName(Register::from_code(code)));
      } else {
        SNPrintF(str, "builtin register argument %s\n",
                 RegisterName(Register::from_code(code)));
      }
    }
    frame_writer.PushTranslatedValue(
        register_values[code], verbose_tracing_enabled() ? str.begin() : "");
  }

  // Some architectures must pad the stack frame with extra stack slots
  // to ensure the stack frame is aligned.
  const int padding_slot_count =
      BuiltinContinuationFrameConstants::PaddingSlotCount(
          allocatable_register_count);
  for (int i = 0; i < padding_slot_count; ++i) {
    frame_writer.PushRawObject(roots.the_hole_value(), "padding\n");
  }

  if (is_topmost) {
    for (int i = 0; i < ArgumentPaddingSlots(1); ++i) {
      frame_writer.PushRawObject(roots.the_hole_value(), "padding\n");
    }

    // Ensure the result is restored back when we return to the stub.
    if (frame_info.frame_has_result_stack_slot()) {
      Register result_reg = kReturnRegister0;
      frame_writer.PushRawValue(input_->GetRegister(result_reg.code()),
                                "callback result\n");
    } else {
      frame_writer.PushRawObject(roots.undefined_value(), "callback result\n");
    }
  }

  CHECK_EQ(result_iterator, value_iterator);
  CHECK_EQ(0u, frame_writer.top_offset());

  // Clear the context register. The context might be a de-materialized object
  // and will be materialized by {Runtime_NotifyDeoptimized}. For additional
  // safety we use Smi(0) instead of the potential {arguments_marker} here.
  if (is_topmost) {
    intptr_t context_value = static_cast<intptr_t>(Smi::zero().ptr());
    Register context_reg = JavaScriptFrame::context_register();
    output_frame->SetRegister(context_reg.code(), context_value);
  }

  // Ensure the frame pointer register points to the callee's frame. The builtin
  // will build its own frame once we continue to it.
  Register fp_reg = JavaScriptFrame::fp_register();
  output_frame->SetRegister(fp_reg.code(), fp_value);
  // For JSToWasmBuiltinContinuations use ContinueToCodeStubBuiltin, and not
  // ContinueToCodeStubBuiltinWithResult because we don't want to overwrite the
  // return value that we have already set.
  Code continue_to_builtin =
      isolate()->builtins()->code(TrampolineForBuiltinContinuation(
          mode, frame_info.frame_has_result_stack_slot() &&
                    !is_js_to_wasm_builtin_continuation));
  if (is_topmost) {
    // Only the pc of the topmost frame needs to be signed since it is
    // authenticated at the end of the DeoptimizationEntry builtin.
    const intptr_t top_most_pc = PointerAuthentication::SignAndCheckPC(
        isolate(),
        static_cast<intptr_t>(continue_to_builtin.instruction_start()),
        frame_writer.frame()->GetTop());
    output_frame->SetPc(top_most_pc);
  } else {
    output_frame->SetPc(
        static_cast<intptr_t>(continue_to_builtin.instruction_start()));
  }

  Code continuation = isolate()->builtins()->code(Builtin::kNotifyDeoptimized);
  output_frame->SetContinuation(
      static_cast<intptr_t>(continuation.instruction_start()));
}

void Deoptimizer::MaterializeHeapObjects() {
  translated_state_.Prepare(static_cast<Address>(stack_fp_));
  if (v8_flags.deopt_every_n_times > 0) {
    // Doing a GC here will find problems with the deoptimized frames.
    isolate_->heap()->CollectAllGarbage(Heap::kNoGCFlags,
                                        GarbageCollectionReason::kTesting);
  }

  for (auto& materialization : values_to_materialize_) {
    Handle<Object> value = materialization.value_->GetValue();

    if (verbose_tracing_enabled()) {
      PrintF(trace_scope()->file(),
             "Materialization [" V8PRIxPTR_FMT "] <- " V8PRIxPTR_FMT " ;  ",
             static_cast<intptr_t>(materialization.output_slot_address_),
             value->ptr());
      value->ShortPrint(trace_scope()->file());
      PrintF(trace_scope()->file(), "\n");
    }

    *(reinterpret_cast<Address*>(materialization.output_slot_address_)) =
        value->ptr();
  }

  translated_state_.VerifyMaterializedObjects();

  bool feedback_updated = translated_state_.DoUpdateFeedback();
  if (verbose_tracing_enabled() && feedback_updated) {
    FILE* file = trace_scope()->file();
    Deoptimizer::DeoptInfo info = Deoptimizer::GetDeoptInfo();
    PrintF(file, "Feedback updated from deoptimization at ");
    OFStream outstr(file);
    info.position.Print(outstr, compiled_code_);
    PrintF(file, ", %s\n", DeoptimizeReasonToString(info.deopt_reason));
  }

  isolate_->materialized_object_store()->Remove(
      static_cast<Address>(stack_fp_));
}

void Deoptimizer::QueueValueForMaterialization(
    Address output_address, Object obj,
    const TranslatedFrame::iterator& iterator) {
  if (obj == ReadOnlyRoots(isolate_).arguments_marker()) {
    values_to_materialize_.push_back({output_address, iterator});
  }
}

unsigned Deoptimizer::ComputeInputFrameAboveFpFixedSize() const {
  unsigned fixed_size = CommonFrameConstants::kFixedFrameSizeAboveFp;
  // TODO(jkummerow): If {function_->IsSmi()} can indeed be true, then
  // {function_} should not have type {JSFunction}.
  if (!function_.IsSmi()) {
    fixed_size += ComputeIncomingArgumentSize(function_.shared());
  }
  return fixed_size;
}

namespace {

// Get the actual deopt call PC from the return address of the deopt, which
// points to immediately after the deopt call).
//
// See also the Deoptimizer constructor.
Address GetDeoptCallPCFromReturnPC(Address return_pc, Code code) {
  DCHECK_GT(Deoptimizer::kEagerDeoptExitSize, 0);
  DCHECK_GT(Deoptimizer::kLazyDeoptExitSize, 0);
  DeoptimizationData deopt_data =
      DeoptimizationData::cast(code.deoptimization_data());
  Address deopt_start =
      code.instruction_start() + deopt_data.DeoptExitStart().value();
  int eager_deopt_count = deopt_data.EagerDeoptCount().value();
  Address lazy_deopt_start =
      deopt_start + eager_deopt_count * Deoptimizer::kEagerDeoptExitSize;
  // The deoptimization exits are sorted so that lazy deopt exits appear
  // after eager deopts.
  static_assert(static_cast<int>(DeoptimizeKind::kLazy) ==
                    static_cast<int>(kLastDeoptimizeKind),
                "lazy deopts are expected to be emitted last");
  if (return_pc <= lazy_deopt_start) {
    return return_pc - Deoptimizer::kEagerDeoptExitSize;
  } else {
    return return_pc - Deoptimizer::kLazyDeoptExitSize;
  }
}

}  // namespace

unsigned Deoptimizer::ComputeInputFrameSize() const {
  // The fp-to-sp delta already takes the context, constant pool pointer and the
  // function into account so we have to avoid double counting them.
  unsigned fixed_size_above_fp = ComputeInputFrameAboveFpFixedSize();
  unsigned result = fixed_size_above_fp + fp_to_sp_delta_;
  DCHECK(CodeKindCanDeoptimize(compiled_code_.kind()));
  unsigned stack_slots = compiled_code_.stack_slots();
  if (compiled_code_.is_maglevved()) {
    // Maglev code can deopt in deferred code which has spilled registers across
    // the call. These will be included in the fp_to_sp_delta, but the expected
    // frame size won't include them, so we need to check for less-equal rather
    // than equal.
    CHECK_LE(fixed_size_above_fp + (stack_slots * kSystemPointerSize) -
                 CommonFrameConstants::kFixedFrameSizeAboveFp,
             result);
    // With slow asserts we can check this exactly, by looking up the safepoint.
    if (v8_flags.enable_slow_asserts) {
      Address deopt_call_pc = GetDeoptCallPCFromReturnPC(from_, compiled_code_);
      MaglevSafepointTable table(isolate_, deopt_call_pc, compiled_code_);
      MaglevSafepointEntry safepoint = table.FindEntry(deopt_call_pc);
      unsigned extra_spills = safepoint.num_pushed_registers();
      CHECK_EQ(fixed_size_above_fp + (stack_slots * kSystemPointerSize) -
                   CommonFrameConstants::kFixedFrameSizeAboveFp +
                   extra_spills * kSystemPointerSize,
               result);
    }
  } else {
    unsigned outgoing_size = 0;
    CHECK_EQ(fixed_size_above_fp + (stack_slots * kSystemPointerSize) -
                 CommonFrameConstants::kFixedFrameSizeAboveFp + outgoing_size,
             result);
  }
  return result;
}

// static
unsigned Deoptimizer::ComputeIncomingArgumentSize(SharedFunctionInfo shared) {
  int parameter_slots = shared.internal_formal_parameter_count_with_receiver();
  return parameter_slots * kSystemPointerSize;
}

Deoptimizer::DeoptInfo Deoptimizer::GetDeoptInfo(Code code, Address pc) {
  CHECK(code.instruction_start() <= pc && pc <= code.instruction_end());
  SourcePosition last_position = SourcePosition::Unknown();
  DeoptimizeReason last_reason = DeoptimizeReason::kUnknown;
  uint32_t last_node_id = 0;
  int last_deopt_id = kNoDeoptimizationId;
  int mask = RelocInfo::ModeMask(RelocInfo::DEOPT_REASON) |
             RelocInfo::ModeMask(RelocInfo::DEOPT_ID) |
             RelocInfo::ModeMask(RelocInfo::DEOPT_SCRIPT_OFFSET) |
             RelocInfo::ModeMask(RelocInfo::DEOPT_INLINING_ID) |
             RelocInfo::ModeMask(RelocInfo::DEOPT_NODE_ID);
  for (RelocIterator it(code, mask); !it.done(); it.next()) {
    RelocInfo* info = it.rinfo();
    if (info->pc() >= pc) break;
    if (info->rmode() == RelocInfo::DEOPT_SCRIPT_OFFSET) {
      int script_offset = static_cast<int>(info->data());
      it.next();
      DCHECK(it.rinfo()->rmode() == RelocInfo::DEOPT_INLINING_ID);
      int inlining_id = static_cast<int>(it.rinfo()->data());
      last_position = SourcePosition(script_offset, inlining_id);
    } else if (info->rmode() == RelocInfo::DEOPT_ID) {
      last_deopt_id = static_cast<int>(info->data());
    } else if (info->rmode() == RelocInfo::DEOPT_REASON) {
      last_reason = static_cast<DeoptimizeReason>(info->data());
    } else if (info->rmode() == RelocInfo::DEOPT_NODE_ID) {
      last_node_id = static_cast<uint32_t>(info->data());
    }
  }
  return DeoptInfo(last_position, last_reason, last_node_id, last_deopt_id);
}

}  // namespace internal
}  // namespace v8
