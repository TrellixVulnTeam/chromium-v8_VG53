// Copyright 2012 the V8 project authors. All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//     * Neither the name of Google Inc. nor the names of its
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "v8.h"

#include "mips/lithium-codegen-mips.h"
#include "mips/lithium-gap-resolver-mips.h"
#include "code-stubs.h"
#include "stub-cache.h"
#include "hydrogen-osr.h"

namespace v8 {
namespace internal {


class SafepointGenerator V8_FINAL  : public CallWrapper {
 public:
  SafepointGenerator(LCodeGen* codegen,
                     LPointerMap* pointers,
                     Safepoint::DeoptMode mode)
      : codegen_(codegen),
        pointers_(pointers),
        deopt_mode_(mode) { }
  virtual ~SafepointGenerator() {}

  virtual void BeforeCall(int call_size) const V8_OVERRIDE {}

  virtual void AfterCall() const V8_OVERRIDE {
    codegen_->RecordSafepoint(pointers_, deopt_mode_);
  }

 private:
  LCodeGen* codegen_;
  LPointerMap* pointers_;
  Safepoint::DeoptMode deopt_mode_;
};


#define __ masm()->

bool LCodeGen::GenerateCode() {
  LPhase phase("Z_Code generation", chunk());
  ASSERT(is_unused());
  status_ = GENERATING;

  // Open a frame scope to indicate that there is a frame on the stack.  The
  // NONE indicates that the scope shouldn't actually generate code to set up
  // the frame (that is done in GeneratePrologue).
  FrameScope frame_scope(masm_, StackFrame::NONE);

  return GeneratePrologue() &&
      GenerateBody() &&
      GenerateDeferredCode() &&
      GenerateDeoptJumpTable() &&
      GenerateSafepointTable();
}


void LCodeGen::FinishCode(Handle<Code> code) {
  ASSERT(is_done());
  code->set_stack_slots(GetStackSlotCount());
  code->set_safepoint_table_offset(safepoints_.GetCodeOffset());
  if (FLAG_weak_embedded_maps_in_optimized_code) {
    RegisterDependentCodeForEmbeddedMaps(code);
  }
  PopulateDeoptimizationData(code);
  info()->CommitDependencies(code);
}


void LChunkBuilder::Abort(BailoutReason reason) {
  info()->set_bailout_reason(reason);
  status_ = ABORTED;
}


bool LCodeGen::GeneratePrologue() {
  ASSERT(is_generating());

  if (info()->IsOptimizing()) {
    ProfileEntryHookStub::MaybeCallEntryHook(masm_);

#ifdef DEBUG
    if (strlen(FLAG_stop_at) > 0 &&
        info_->function()->name()->IsUtf8EqualTo(CStrVector(FLAG_stop_at))) {
      __ stop("stop_at");
    }
#endif

    // a1: Callee's JS function.
    // cp: Callee's context.
    // fp: Caller's frame pointer.
    // lr: Caller's pc.

    // Strict mode functions and builtins need to replace the receiver
    // with undefined when called as functions (without an explicit
    // receiver object). r5 is zero for method calls and non-zero for
    // function calls.
    if (!info_->is_classic_mode() || info_->is_native()) {
      Label ok;
      __ Branch(&ok, eq, t1, Operand(zero_reg));

      int receiver_offset = scope()->num_parameters() * kPointerSize;
      __ LoadRoot(a2, Heap::kUndefinedValueRootIndex);
      __ sw(a2, MemOperand(sp, receiver_offset));
      __ bind(&ok);
    }
  }

  info()->set_prologue_offset(masm_->pc_offset());
  if (NeedsEagerFrame()) {
    __ Prologue(info()->IsStub() ? BUILD_STUB_FRAME : BUILD_FUNCTION_FRAME);
    frame_is_built_ = true;
    info_->AddNoFrameRange(0, masm_->pc_offset());
  }

  // Reserve space for the stack slots needed by the code.
  int slots = GetStackSlotCount();
  if (slots > 0) {
    if (FLAG_debug_code) {
      __ Subu(sp,  sp, Operand(slots * kPointerSize));
      __ push(a0);
      __ push(a1);
      __ Addu(a0, sp, Operand(slots *  kPointerSize));
      __ li(a1, Operand(kSlotsZapValue));
      Label loop;
      __ bind(&loop);
      __ Subu(a0, a0, Operand(kPointerSize));
      __ sw(a1, MemOperand(a0, 2 * kPointerSize));
      __ Branch(&loop, ne, a0, Operand(sp));
      __ pop(a1);
      __ pop(a0);
    } else {
      __ Subu(sp, sp, Operand(slots * kPointerSize));
    }
  }

  if (info()->saves_caller_doubles()) {
    Comment(";;; Save clobbered callee double registers");
    int count = 0;
    BitVector* doubles = chunk()->allocated_double_registers();
    BitVector::Iterator save_iterator(doubles);
    while (!save_iterator.Done()) {
      __ sdc1(DoubleRegister::FromAllocationIndex(save_iterator.Current()),
              MemOperand(sp, count * kDoubleSize));
      save_iterator.Advance();
      count++;
    }
  }

  // Possibly allocate a local context.
  int heap_slots = info()->num_heap_slots() - Context::MIN_CONTEXT_SLOTS;
  if (heap_slots > 0) {
    Comment(";;; Allocate local context");
    // Argument to NewContext is the function, which is in a1.
    __ push(a1);
    if (heap_slots <= FastNewContextStub::kMaximumSlots) {
      FastNewContextStub stub(heap_slots);
      __ CallStub(&stub);
    } else {
      __ CallRuntime(Runtime::kNewFunctionContext, 1);
    }
    RecordSafepoint(Safepoint::kNoLazyDeopt);
    // Context is returned in both v0 and cp.  It replaces the context
    // passed to us.  It's saved in the stack and kept live in cp.
    __ sw(cp, MemOperand(fp, StandardFrameConstants::kContextOffset));
    // Copy any necessary parameters into the context.
    int num_parameters = scope()->num_parameters();
    for (int i = 0; i < num_parameters; i++) {
      Variable* var = scope()->parameter(i);
      if (var->IsContextSlot()) {
        int parameter_offset = StandardFrameConstants::kCallerSPOffset +
            (num_parameters - 1 - i) * kPointerSize;
        // Load parameter from stack.
        __ lw(a0, MemOperand(fp, parameter_offset));
        // Store it in the context.
        MemOperand target = ContextOperand(cp, var->index());
        __ sw(a0, target);
        // Update the write barrier. This clobbers a3 and a0.
        __ RecordWriteContextSlot(
            cp, target.offset(), a0, a3, GetRAState(), kSaveFPRegs);
      }
    }
    Comment(";;; End allocate local context");
  }

  // Trace the call.
  if (FLAG_trace && info()->IsOptimizing()) {
    // We have not executed any compiled code yet, so cp still holds the
    // incoming context.
    __ CallRuntime(Runtime::kTraceEnter, 0);
  }
  return !is_aborted();
}


void LCodeGen::GenerateOsrPrologue() {
  // Generate the OSR entry prologue at the first unknown OSR value, or if there
  // are none, at the OSR entrypoint instruction.
  if (osr_pc_offset_ >= 0) return;

  osr_pc_offset_ = masm()->pc_offset();

  // Adjust the frame size, subsuming the unoptimized frame into the
  // optimized frame.
  int slots = GetStackSlotCount() - graph()->osr()->UnoptimizedFrameSlots();
  ASSERT(slots >= 0);
  __ Subu(sp, sp, Operand(slots * kPointerSize));
}


bool LCodeGen::GenerateDeferredCode() {
  ASSERT(is_generating());
  if (deferred_.length() > 0) {
    for (int i = 0; !is_aborted() && i < deferred_.length(); i++) {
      LDeferredCode* code = deferred_[i];

      HValue* value =
          instructions_->at(code->instruction_index())->hydrogen_value();
      RecordAndWritePosition(value->position());

      Comment(";;; <@%d,#%d> "
              "-------------------- Deferred %s --------------------",
              code->instruction_index(),
              code->instr()->hydrogen_value()->id(),
              code->instr()->Mnemonic());
      __ bind(code->entry());
      if (NeedsDeferredFrame()) {
        Comment(";;; Build frame");
        ASSERT(!frame_is_built_);
        ASSERT(info()->IsStub());
        frame_is_built_ = true;
        __ MultiPush(cp.bit() | fp.bit() | ra.bit());
        __ li(scratch0(), Operand(Smi::FromInt(StackFrame::STUB)));
        __ push(scratch0());
        __ Addu(fp, sp, Operand(2 * kPointerSize));
        Comment(";;; Deferred code");
      }
      code->Generate();
      if (NeedsDeferredFrame()) {
        Comment(";;; Destroy frame");
        ASSERT(frame_is_built_);
        __ pop(at);
        __ MultiPop(cp.bit() | fp.bit() | ra.bit());
        frame_is_built_ = false;
      }
      __ jmp(code->exit());
    }
  }
  // Deferred code is the last part of the instruction sequence. Mark
  // the generated code as done unless we bailed out.
  if (!is_aborted()) status_ = DONE;
  return !is_aborted();
}


bool LCodeGen::GenerateDeoptJumpTable() {
  if (deopt_jump_table_.length() > 0) {
    Comment(";;; -------------------- Jump table --------------------");
  }
  Assembler::BlockTrampolinePoolScope block_trampoline_pool(masm_);
  Label table_start;
  __ bind(&table_start);
  Label needs_frame;
  for (int i = 0; i < deopt_jump_table_.length(); i++) {
    __ bind(&deopt_jump_table_[i].label);
    Address entry = deopt_jump_table_[i].address;
    Deoptimizer::BailoutType type = deopt_jump_table_[i].bailout_type;
    int id = Deoptimizer::GetDeoptimizationId(isolate(), entry, type);
    if (id == Deoptimizer::kNotDeoptimizationEntry) {
      Comment(";;; jump table entry %d.", i);
    } else {
      Comment(";;; jump table entry %d: deoptimization bailout %d.", i, id);
    }
    __ li(t9, Operand(ExternalReference::ForDeoptEntry(entry)));
    if (deopt_jump_table_[i].needs_frame) {
      if (needs_frame.is_bound()) {
        __ Branch(&needs_frame);
      } else {
        __ bind(&needs_frame);
        __ MultiPush(cp.bit() | fp.bit() | ra.bit());
        // This variant of deopt can only be used with stubs. Since we don't
        // have a function pointer to install in the stack frame that we're
        // building, install a special marker there instead.
        ASSERT(info()->IsStub());
        __ li(scratch0(), Operand(Smi::FromInt(StackFrame::STUB)));
        __ push(scratch0());
        __ Addu(fp, sp, Operand(2 * kPointerSize));
        __ Call(t9);
      }
    } else {
      __ Call(t9);
    }
  }
  __ RecordComment("]");

  // The deoptimization jump table is the last part of the instruction
  // sequence. Mark the generated code as done unless we bailed out.
  if (!is_aborted()) status_ = DONE;
  return !is_aborted();
}


bool LCodeGen::GenerateSafepointTable() {
  ASSERT(is_done());
  safepoints_.Emit(masm(), GetStackSlotCount());
  return !is_aborted();
}


Register LCodeGen::ToRegister(int index) const {
  return Register::FromAllocationIndex(index);
}


DoubleRegister LCodeGen::ToDoubleRegister(int index) const {
  return DoubleRegister::FromAllocationIndex(index);
}


Register LCodeGen::ToRegister(LOperand* op) const {
  ASSERT(op->IsRegister());
  return ToRegister(op->index());
}


Register LCodeGen::EmitLoadRegister(LOperand* op, Register scratch) {
  if (op->IsRegister()) {
    return ToRegister(op->index());
  } else if (op->IsConstantOperand()) {
    LConstantOperand* const_op = LConstantOperand::cast(op);
    HConstant* constant = chunk_->LookupConstant(const_op);
    Handle<Object> literal = constant->handle(isolate());
    Representation r = chunk_->LookupLiteralRepresentation(const_op);
    if (r.IsInteger32()) {
      ASSERT(literal->IsNumber());
      __ li(scratch, Operand(static_cast<int32_t>(literal->Number())));
    } else if (r.IsSmi()) {
      ASSERT(constant->HasSmiValue());
      __ li(scratch, Operand(Smi::FromInt(constant->Integer32Value())));
    } else if (r.IsDouble()) {
      Abort(kEmitLoadRegisterUnsupportedDoubleImmediate);
    } else {
      ASSERT(r.IsSmiOrTagged());
      __ li(scratch, literal);
    }
    return scratch;
  } else if (op->IsStackSlot() || op->IsArgument()) {
    __ lw(scratch, ToMemOperand(op));
    return scratch;
  }
  UNREACHABLE();
  return scratch;
}


DoubleRegister LCodeGen::ToDoubleRegister(LOperand* op) const {
  ASSERT(op->IsDoubleRegister());
  return ToDoubleRegister(op->index());
}


DoubleRegister LCodeGen::EmitLoadDoubleRegister(LOperand* op,
                                                FloatRegister flt_scratch,
                                                DoubleRegister dbl_scratch) {
  if (op->IsDoubleRegister()) {
    return ToDoubleRegister(op->index());
  } else if (op->IsConstantOperand()) {
    LConstantOperand* const_op = LConstantOperand::cast(op);
    HConstant* constant = chunk_->LookupConstant(const_op);
    Handle<Object> literal = constant->handle(isolate());
    Representation r = chunk_->LookupLiteralRepresentation(const_op);
    if (r.IsInteger32()) {
      ASSERT(literal->IsNumber());
      __ li(at, Operand(static_cast<int32_t>(literal->Number())));
      __ mtc1(at, flt_scratch);
      __ cvt_d_w(dbl_scratch, flt_scratch);
      return dbl_scratch;
    } else if (r.IsDouble()) {
      Abort(kUnsupportedDoubleImmediate);
    } else if (r.IsTagged()) {
      Abort(kUnsupportedTaggedImmediate);
    }
  } else if (op->IsStackSlot() || op->IsArgument()) {
    MemOperand mem_op = ToMemOperand(op);
    __ ldc1(dbl_scratch, mem_op);
    return dbl_scratch;
  }
  UNREACHABLE();
  return dbl_scratch;
}


Handle<Object> LCodeGen::ToHandle(LConstantOperand* op) const {
  HConstant* constant = chunk_->LookupConstant(op);
  ASSERT(chunk_->LookupLiteralRepresentation(op).IsSmiOrTagged());
  return constant->handle(isolate());
}


bool LCodeGen::IsInteger32(LConstantOperand* op) const {
  return chunk_->LookupLiteralRepresentation(op).IsSmiOrInteger32();
}


bool LCodeGen::IsSmi(LConstantOperand* op) const {
  return chunk_->LookupLiteralRepresentation(op).IsSmi();
}


int32_t LCodeGen::ToInteger32(LConstantOperand* op) const {
  return ToRepresentation(op, Representation::Integer32());
}


int32_t LCodeGen::ToRepresentation(LConstantOperand* op,
                                   const Representation& r) const {
  HConstant* constant = chunk_->LookupConstant(op);
  int32_t value = constant->Integer32Value();
  if (r.IsInteger32()) return value;
  ASSERT(r.IsSmiOrTagged());
  return reinterpret_cast<int32_t>(Smi::FromInt(value));
}


Smi* LCodeGen::ToSmi(LConstantOperand* op) const {
  HConstant* constant = chunk_->LookupConstant(op);
  return Smi::FromInt(constant->Integer32Value());
}


double LCodeGen::ToDouble(LConstantOperand* op) const {
  HConstant* constant = chunk_->LookupConstant(op);
  ASSERT(constant->HasDoubleValue());
  return constant->DoubleValue();
}


Operand LCodeGen::ToOperand(LOperand* op) {
  if (op->IsConstantOperand()) {
    LConstantOperand* const_op = LConstantOperand::cast(op);
    HConstant* constant = chunk()->LookupConstant(const_op);
    Representation r = chunk_->LookupLiteralRepresentation(const_op);
    if (r.IsSmi()) {
      ASSERT(constant->HasSmiValue());
      return Operand(Smi::FromInt(constant->Integer32Value()));
    } else if (r.IsInteger32()) {
      ASSERT(constant->HasInteger32Value());
      return Operand(constant->Integer32Value());
    } else if (r.IsDouble()) {
      Abort(kToOperandUnsupportedDoubleImmediate);
    }
    ASSERT(r.IsTagged());
    return Operand(constant->handle(isolate()));
  } else if (op->IsRegister()) {
    return Operand(ToRegister(op));
  } else if (op->IsDoubleRegister()) {
    Abort(kToOperandIsDoubleRegisterUnimplemented);
    return Operand(0);
  }
  // Stack slots not implemented, use ToMemOperand instead.
  UNREACHABLE();
  return Operand(0);
}


MemOperand LCodeGen::ToMemOperand(LOperand* op) const {
  ASSERT(!op->IsRegister());
  ASSERT(!op->IsDoubleRegister());
  ASSERT(op->IsStackSlot() || op->IsDoubleStackSlot());
  return MemOperand(fp, StackSlotOffset(op->index()));
}


MemOperand LCodeGen::ToHighMemOperand(LOperand* op) const {
  ASSERT(op->IsDoubleStackSlot());
  return MemOperand(fp, StackSlotOffset(op->index()) + kPointerSize);
}


void LCodeGen::WriteTranslation(LEnvironment* environment,
                                Translation* translation) {
  if (environment == NULL) return;

  // The translation includes one command per value in the environment.
  int translation_size = environment->translation_size();
  // The output frame height does not include the parameters.
  int height = translation_size - environment->parameter_count();

  WriteTranslation(environment->outer(), translation);
  bool has_closure_id = !info()->closure().is_null() &&
      !info()->closure().is_identical_to(environment->closure());
  int closure_id = has_closure_id
      ? DefineDeoptimizationLiteral(environment->closure())
      : Translation::kSelfLiteralId;

  switch (environment->frame_type()) {
    case JS_FUNCTION:
      translation->BeginJSFrame(environment->ast_id(), closure_id, height);
      break;
    case JS_CONSTRUCT:
      translation->BeginConstructStubFrame(closure_id, translation_size);
      break;
    case JS_GETTER:
      ASSERT(translation_size == 1);
      ASSERT(height == 0);
      translation->BeginGetterStubFrame(closure_id);
      break;
    case JS_SETTER:
      ASSERT(translation_size == 2);
      ASSERT(height == 0);
      translation->BeginSetterStubFrame(closure_id);
      break;
    case STUB:
      translation->BeginCompiledStubFrame();
      break;
    case ARGUMENTS_ADAPTOR:
      translation->BeginArgumentsAdaptorFrame(closure_id, translation_size);
      break;
  }

  int object_index = 0;
  int dematerialized_index = 0;
  for (int i = 0; i < translation_size; ++i) {
    LOperand* value = environment->values()->at(i);
    AddToTranslation(environment,
                     translation,
                     value,
                     environment->HasTaggedValueAt(i),
                     environment->HasUint32ValueAt(i),
                     &object_index,
                     &dematerialized_index);
  }
}


void LCodeGen::AddToTranslation(LEnvironment* environment,
                                Translation* translation,
                                LOperand* op,
                                bool is_tagged,
                                bool is_uint32,
                                int* object_index_pointer,
                                int* dematerialized_index_pointer) {
  if (op == LEnvironment::materialization_marker()) {
    int object_index = (*object_index_pointer)++;
    if (environment->ObjectIsDuplicateAt(object_index)) {
      int dupe_of = environment->ObjectDuplicateOfAt(object_index);
      translation->DuplicateObject(dupe_of);
      return;
    }
    int object_length = environment->ObjectLengthAt(object_index);
    if (environment->ObjectIsArgumentsAt(object_index)) {
      translation->BeginArgumentsObject(object_length);
    } else {
      translation->BeginCapturedObject(object_length);
    }
    int dematerialized_index = *dematerialized_index_pointer;
    int env_offset = environment->translation_size() + dematerialized_index;
    *dematerialized_index_pointer += object_length;
    for (int i = 0; i < object_length; ++i) {
      LOperand* value = environment->values()->at(env_offset + i);
      AddToTranslation(environment,
                       translation,
                       value,
                       environment->HasTaggedValueAt(env_offset + i),
                       environment->HasUint32ValueAt(env_offset + i),
                       object_index_pointer,
                       dematerialized_index_pointer);
    }
    return;
  }

  if (op->IsStackSlot()) {
    if (is_tagged) {
      translation->StoreStackSlot(op->index());
    } else if (is_uint32) {
      translation->StoreUint32StackSlot(op->index());
    } else {
      translation->StoreInt32StackSlot(op->index());
    }
  } else if (op->IsDoubleStackSlot()) {
    translation->StoreDoubleStackSlot(op->index());
  } else if (op->IsArgument()) {
    ASSERT(is_tagged);
    int src_index = GetStackSlotCount() + op->index();
    translation->StoreStackSlot(src_index);
  } else if (op->IsRegister()) {
    Register reg = ToRegister(op);
    if (is_tagged) {
      translation->StoreRegister(reg);
    } else if (is_uint32) {
      translation->StoreUint32Register(reg);
    } else {
      translation->StoreInt32Register(reg);
    }
  } else if (op->IsDoubleRegister()) {
    DoubleRegister reg = ToDoubleRegister(op);
    translation->StoreDoubleRegister(reg);
  } else if (op->IsConstantOperand()) {
    HConstant* constant = chunk()->LookupConstant(LConstantOperand::cast(op));
    int src_index = DefineDeoptimizationLiteral(constant->handle(isolate()));
    translation->StoreLiteral(src_index);
  } else {
    UNREACHABLE();
  }
}


void LCodeGen::CallCode(Handle<Code> code,
                        RelocInfo::Mode mode,
                        LInstruction* instr) {
  CallCodeGeneric(code, mode, instr, RECORD_SIMPLE_SAFEPOINT);
}


void LCodeGen::CallCodeGeneric(Handle<Code> code,
                               RelocInfo::Mode mode,
                               LInstruction* instr,
                               SafepointMode safepoint_mode) {
  EnsureSpaceForLazyDeopt(Deoptimizer::patch_size());
  ASSERT(instr != NULL);
  __ Call(code, mode);
  RecordSafepointWithLazyDeopt(instr, safepoint_mode);
}


void LCodeGen::CallRuntime(const Runtime::Function* function,
                           int num_arguments,
                           LInstruction* instr,
                           SaveFPRegsMode save_doubles) {
  ASSERT(instr != NULL);

  __ CallRuntime(function, num_arguments, save_doubles);

  RecordSafepointWithLazyDeopt(instr, RECORD_SIMPLE_SAFEPOINT);
}


void LCodeGen::LoadContextFromDeferred(LOperand* context) {
  if (context->IsRegister()) {
    __ Move(cp, ToRegister(context));
  } else if (context->IsStackSlot()) {
    __ lw(cp, ToMemOperand(context));
  } else if (context->IsConstantOperand()) {
    HConstant* constant =
        chunk_->LookupConstant(LConstantOperand::cast(context));
    __ li(cp, Handle<Object>::cast(constant->handle(isolate())));
  } else {
    UNREACHABLE();
  }
}


void LCodeGen::CallRuntimeFromDeferred(Runtime::FunctionId id,
                                       int argc,
                                       LInstruction* instr,
                                       LOperand* context) {
  LoadContextFromDeferred(context);
  __ CallRuntimeSaveDoubles(id);
  RecordSafepointWithRegisters(
      instr->pointer_map(), argc, Safepoint::kNoLazyDeopt);
}


void LCodeGen::RegisterEnvironmentForDeoptimization(LEnvironment* environment,
                                                    Safepoint::DeoptMode mode) {
  if (!environment->HasBeenRegistered()) {
    // Physical stack frame layout:
    // -x ............. -4  0 ..................................... y
    // [incoming arguments] [spill slots] [pushed outgoing arguments]

    // Layout of the environment:
    // 0 ..................................................... size-1
    // [parameters] [locals] [expression stack including arguments]

    // Layout of the translation:
    // 0 ........................................................ size - 1 + 4
    // [expression stack including arguments] [locals] [4 words] [parameters]
    // |>------------  translation_size ------------<|

    int frame_count = 0;
    int jsframe_count = 0;
    for (LEnvironment* e = environment; e != NULL; e = e->outer()) {
      ++frame_count;
      if (e->frame_type() == JS_FUNCTION) {
        ++jsframe_count;
      }
    }
    Translation translation(&translations_, frame_count, jsframe_count, zone());
    WriteTranslation(environment, &translation);
    int deoptimization_index = deoptimizations_.length();
    int pc_offset = masm()->pc_offset();
    environment->Register(deoptimization_index,
                          translation.index(),
                          (mode == Safepoint::kLazyDeopt) ? pc_offset : -1);
    deoptimizations_.Add(environment, zone());
  }
}


void LCodeGen::DeoptimizeIf(Condition condition,
                            LEnvironment* environment,
                            Deoptimizer::BailoutType bailout_type,
                            Register src1,
                            const Operand& src2) {
  RegisterEnvironmentForDeoptimization(environment, Safepoint::kNoLazyDeopt);
  ASSERT(environment->HasBeenRegistered());
  int id = environment->deoptimization_index();
  ASSERT(info()->IsOptimizing() || info()->IsStub());
  Address entry =
      Deoptimizer::GetDeoptimizationEntry(isolate(), id, bailout_type);
  if (entry == NULL) {
    Abort(kBailoutWasNotPrepared);
    return;
  }

  ASSERT(FLAG_deopt_every_n_times < 2);  // Other values not supported on MIPS.
  if (FLAG_deopt_every_n_times == 1 &&
      !info()->IsStub() &&
      info()->opt_count() == id) {
    ASSERT(frame_is_built_);
    __ Call(entry, RelocInfo::RUNTIME_ENTRY);
    return;
  }

  if (info()->ShouldTrapOnDeopt()) {
    Label skip;
    if (condition != al) {
      __ Branch(&skip, NegateCondition(condition), src1, src2);
    }
    __ stop("trap_on_deopt");
    __ bind(&skip);
  }

  ASSERT(info()->IsStub() || frame_is_built_);
  if (condition == al && frame_is_built_) {
    __ Call(entry, RelocInfo::RUNTIME_ENTRY, condition, src1, src2);
  } else {
    // We often have several deopts to the same entry, reuse the last
    // jump entry if this is the case.
    if (deopt_jump_table_.is_empty() ||
        (deopt_jump_table_.last().address != entry) ||
        (deopt_jump_table_.last().bailout_type != bailout_type) ||
        (deopt_jump_table_.last().needs_frame != !frame_is_built_)) {
      Deoptimizer::JumpTableEntry table_entry(entry,
                                              bailout_type,
                                              !frame_is_built_);
      deopt_jump_table_.Add(table_entry, zone());
    }
    __ Branch(&deopt_jump_table_.last().label, condition, src1, src2);
  }
}


void LCodeGen::DeoptimizeIf(Condition condition,
                            LEnvironment* environment,
                            Register src1,
                            const Operand& src2) {
  Deoptimizer::BailoutType bailout_type = info()->IsStub()
      ? Deoptimizer::LAZY
      : Deoptimizer::EAGER;
  DeoptimizeIf(condition, environment, bailout_type, src1, src2);
}


void LCodeGen::RegisterDependentCodeForEmbeddedMaps(Handle<Code> code) {
  ZoneList<Handle<Map> > maps(1, zone());
  ZoneList<Handle<JSObject> > objects(1, zone());
  int mode_mask = RelocInfo::ModeMask(RelocInfo::EMBEDDED_OBJECT);
  for (RelocIterator it(*code, mode_mask); !it.done(); it.next()) {
    if (Code::IsWeakEmbeddedObject(code->kind(), it.rinfo()->target_object())) {
      if (it.rinfo()->target_object()->IsMap()) {
        Handle<Map> map(Map::cast(it.rinfo()->target_object()));
        maps.Add(map, zone());
      } else if (it.rinfo()->target_object()->IsJSObject()) {
        Handle<JSObject> object(JSObject::cast(it.rinfo()->target_object()));
        objects.Add(object, zone());
      }
    }
  }
#ifdef VERIFY_HEAP
  // This disables verification of weak embedded objects after full GC.
  // AddDependentCode can cause a GC, which would observe the state where
  // this code is not yet in the depended code lists of the embedded maps.
  NoWeakObjectVerificationScope disable_verification_of_embedded_objects;
#endif
  for (int i = 0; i < maps.length(); i++) {
    maps.at(i)->AddDependentCode(DependentCode::kWeaklyEmbeddedGroup, code);
  }
  for (int i = 0; i < objects.length(); i++) {
    AddWeakObjectToCodeDependency(isolate()->heap(), objects.at(i), code);
  }
}


void LCodeGen::PopulateDeoptimizationData(Handle<Code> code) {
  int length = deoptimizations_.length();
  if (length == 0) return;
  Handle<DeoptimizationInputData> data =
      factory()->NewDeoptimizationInputData(length, TENURED);

  Handle<ByteArray> translations =
      translations_.CreateByteArray(isolate()->factory());
  data->SetTranslationByteArray(*translations);
  data->SetInlinedFunctionCount(Smi::FromInt(inlined_function_count_));

  Handle<FixedArray> literals =
      factory()->NewFixedArray(deoptimization_literals_.length(), TENURED);
  { AllowDeferredHandleDereference copy_handles;
    for (int i = 0; i < deoptimization_literals_.length(); i++) {
      literals->set(i, *deoptimization_literals_[i]);
    }
    data->SetLiteralArray(*literals);
  }

  data->SetOsrAstId(Smi::FromInt(info_->osr_ast_id().ToInt()));
  data->SetOsrPcOffset(Smi::FromInt(osr_pc_offset_));

  // Populate the deoptimization entries.
  for (int i = 0; i < length; i++) {
    LEnvironment* env = deoptimizations_[i];
    data->SetAstId(i, env->ast_id());
    data->SetTranslationIndex(i, Smi::FromInt(env->translation_index()));
    data->SetArgumentsStackHeight(i,
                                  Smi::FromInt(env->arguments_stack_height()));
    data->SetPc(i, Smi::FromInt(env->pc_offset()));
  }
  code->set_deoptimization_data(*data);
}


int LCodeGen::DefineDeoptimizationLiteral(Handle<Object> literal) {
  int result = deoptimization_literals_.length();
  for (int i = 0; i < deoptimization_literals_.length(); ++i) {
    if (deoptimization_literals_[i].is_identical_to(literal)) return i;
  }
  deoptimization_literals_.Add(literal, zone());
  return result;
}


void LCodeGen::PopulateDeoptimizationLiteralsWithInlinedFunctions() {
  ASSERT(deoptimization_literals_.length() == 0);

  const ZoneList<Handle<JSFunction> >* inlined_closures =
      chunk()->inlined_closures();

  for (int i = 0, length = inlined_closures->length();
       i < length;
       i++) {
    DefineDeoptimizationLiteral(inlined_closures->at(i));
  }

  inlined_function_count_ = deoptimization_literals_.length();
}


void LCodeGen::RecordSafepointWithLazyDeopt(
    LInstruction* instr, SafepointMode safepoint_mode) {
  if (safepoint_mode == RECORD_SIMPLE_SAFEPOINT) {
    RecordSafepoint(instr->pointer_map(), Safepoint::kLazyDeopt);
  } else {
    ASSERT(safepoint_mode == RECORD_SAFEPOINT_WITH_REGISTERS_AND_NO_ARGUMENTS);
    RecordSafepointWithRegisters(
        instr->pointer_map(), 0, Safepoint::kLazyDeopt);
  }
}


void LCodeGen::RecordSafepoint(
    LPointerMap* pointers,
    Safepoint::Kind kind,
    int arguments,
    Safepoint::DeoptMode deopt_mode) {
  ASSERT(expected_safepoint_kind_ == kind);

  const ZoneList<LOperand*>* operands = pointers->GetNormalizedOperands();
  Safepoint safepoint = safepoints_.DefineSafepoint(masm(),
      kind, arguments, deopt_mode);
  for (int i = 0; i < operands->length(); i++) {
    LOperand* pointer = operands->at(i);
    if (pointer->IsStackSlot()) {
      safepoint.DefinePointerSlot(pointer->index(), zone());
    } else if (pointer->IsRegister() && (kind & Safepoint::kWithRegisters)) {
      safepoint.DefinePointerRegister(ToRegister(pointer), zone());
    }
  }
}


void LCodeGen::RecordSafepoint(LPointerMap* pointers,
                               Safepoint::DeoptMode deopt_mode) {
  RecordSafepoint(pointers, Safepoint::kSimple, 0, deopt_mode);
}


void LCodeGen::RecordSafepoint(Safepoint::DeoptMode deopt_mode) {
  LPointerMap empty_pointers(zone());
  RecordSafepoint(&empty_pointers, deopt_mode);
}


void LCodeGen::RecordSafepointWithRegisters(LPointerMap* pointers,
                                            int arguments,
                                            Safepoint::DeoptMode deopt_mode) {
  RecordSafepoint(
      pointers, Safepoint::kWithRegisters, arguments, deopt_mode);
}


void LCodeGen::RecordSafepointWithRegistersAndDoubles(
    LPointerMap* pointers,
    int arguments,
    Safepoint::DeoptMode deopt_mode) {
  RecordSafepoint(
      pointers, Safepoint::kWithRegistersAndDoubles, arguments, deopt_mode);
}


void LCodeGen::RecordAndWritePosition(int position) {
  if (position == RelocInfo::kNoPosition) return;
  masm()->positions_recorder()->RecordPosition(position);
  masm()->positions_recorder()->WriteRecordedPositions();
}


static const char* LabelType(LLabel* label) {
  if (label->is_loop_header()) return " (loop header)";
  if (label->is_osr_entry()) return " (OSR entry)";
  return "";
}


void LCodeGen::DoLabel(LLabel* label) {
  Comment(";;; <@%d,#%d> -------------------- B%d%s --------------------",
          current_instruction_,
          label->hydrogen_value()->id(),
          label->block_id(),
          LabelType(label));
  __ bind(label->label());
  current_block_ = label->block_id();
  DoGap(label);
}


void LCodeGen::DoParallelMove(LParallelMove* move) {
  resolver_.Resolve(move);
}


void LCodeGen::DoGap(LGap* gap) {
  for (int i = LGap::FIRST_INNER_POSITION;
       i <= LGap::LAST_INNER_POSITION;
       i++) {
    LGap::InnerPosition inner_pos = static_cast<LGap::InnerPosition>(i);
    LParallelMove* move = gap->GetParallelMove(inner_pos);
    if (move != NULL) DoParallelMove(move);
  }
}


void LCodeGen::DoInstructionGap(LInstructionGap* instr) {
  DoGap(instr);
}


void LCodeGen::DoParameter(LParameter* instr) {
  // Nothing to do.
}


void LCodeGen::DoCallStub(LCallStub* instr) {
  ASSERT(ToRegister(instr->context()).is(cp));
  ASSERT(ToRegister(instr->result()).is(v0));
  switch (instr->hydrogen()->major_key()) {
    case CodeStub::RegExpConstructResult: {
      RegExpConstructResultStub stub;
      CallCode(stub.GetCode(isolate()), RelocInfo::CODE_TARGET, instr);
      break;
    }
    case CodeStub::RegExpExec: {
      RegExpExecStub stub;
      CallCode(stub.GetCode(isolate()), RelocInfo::CODE_TARGET, instr);
      break;
    }
    case CodeStub::SubString: {
      SubStringStub stub;
      CallCode(stub.GetCode(isolate()), RelocInfo::CODE_TARGET, instr);
      break;
    }
    case CodeStub::StringCompare: {
      StringCompareStub stub;
      CallCode(stub.GetCode(isolate()), RelocInfo::CODE_TARGET, instr);
      break;
    }
    case CodeStub::TranscendentalCache: {
      __ lw(a0, MemOperand(sp, 0));
      TranscendentalCacheStub stub(instr->transcendental_type(),
                                   TranscendentalCacheStub::TAGGED);
      CallCode(stub.GetCode(isolate()), RelocInfo::CODE_TARGET, instr);
      break;
    }
    default:
      UNREACHABLE();
  }
}


void LCodeGen::DoUnknownOSRValue(LUnknownOSRValue* instr) {
  GenerateOsrPrologue();
}


void LCodeGen::DoModI(LModI* instr) {
  HMod* hmod = instr->hydrogen();
  HValue* left = hmod->left();
  HValue* right = hmod->right();
  if (hmod->HasPowerOf2Divisor()) {
    const Register left_reg = ToRegister(instr->left());
    const Register result_reg = ToRegister(instr->result());

    // Note: The code below even works when right contains kMinInt.
    int32_t divisor = Abs(right->GetInteger32Constant());

    Label left_is_not_negative, done;
    if (left->CanBeNegative()) {
      __ Branch(left_reg.is(result_reg) ? PROTECT : USE_DELAY_SLOT,
                &left_is_not_negative, ge, left_reg, Operand(zero_reg));
      __ subu(result_reg, zero_reg, left_reg);
      __ And(result_reg, result_reg, divisor - 1);
      if (hmod->CheckFlag(HValue::kBailoutOnMinusZero)) {
        DeoptimizeIf(eq, instr->environment(), result_reg, Operand(zero_reg));
      }
      __ Branch(USE_DELAY_SLOT, &done);
      __ subu(result_reg, zero_reg, result_reg);
    }

    __ bind(&left_is_not_negative);
    __ And(result_reg, left_reg, divisor - 1);
    __ bind(&done);

  } else if (hmod->fixed_right_arg().has_value) {
    const Register left_reg = ToRegister(instr->left());
    const Register result_reg = ToRegister(instr->result());
    const Register right_reg = ToRegister(instr->right());

    int32_t divisor = hmod->fixed_right_arg().value;
    ASSERT(IsPowerOf2(divisor));

    // Check if our assumption of a fixed right operand still holds.
    DeoptimizeIf(ne, instr->environment(), right_reg, Operand(divisor));

    Label left_is_not_negative, done;
    if (left->CanBeNegative()) {
      __ Branch(left_reg.is(result_reg) ? PROTECT : USE_DELAY_SLOT,
                &left_is_not_negative, ge, left_reg, Operand(zero_reg));
      __ subu(result_reg, zero_reg, left_reg);
      __ And(result_reg, result_reg, divisor - 1);
      if (hmod->CheckFlag(HValue::kBailoutOnMinusZero)) {
        DeoptimizeIf(eq, instr->environment(), result_reg, Operand(zero_reg));
      }
      __ Branch(USE_DELAY_SLOT, &done);
      __ subu(result_reg, zero_reg, result_reg);
    }

    __ bind(&left_is_not_negative);
    __ And(result_reg, left_reg, divisor - 1);
    __ bind(&done);

  } else {
    const Register scratch = scratch0();
    const Register left_reg = ToRegister(instr->left());
    const Register result_reg = ToRegister(instr->result());

    // div runs in the background while we check for special cases.
    Register right_reg = EmitLoadRegister(instr->right(), scratch);
    __ div(left_reg, right_reg);

    Label done;
    // Check for x % 0, we have to deopt in this case because we can't return a
    // NaN.
    if (right->CanBeZero()) {
      DeoptimizeIf(eq, instr->environment(), right_reg, Operand(zero_reg));
    }

    // Check for kMinInt % -1, we have to deopt if we care about -0, because we
    // can't return that.
    if (left->RangeCanInclude(kMinInt) && right->RangeCanInclude(-1)) {
      Label left_not_min_int;
      __ Branch(&left_not_min_int, ne, left_reg, Operand(kMinInt));
      // TODO(svenpanne) Don't deopt when we don't care about -0.
      DeoptimizeIf(eq, instr->environment(), right_reg, Operand(-1));
      __ bind(&left_not_min_int);
    }

    // TODO(svenpanne) Only emit the test/deopt if we have to.
    __ Branch(USE_DELAY_SLOT, &done, ge, left_reg, Operand(zero_reg));
    __ mfhi(result_reg);

    if (hmod->CheckFlag(HValue::kBailoutOnMinusZero)) {
      DeoptimizeIf(eq, instr->environment(), result_reg, Operand(zero_reg));
    }
    __ bind(&done);
  }
}


void LCodeGen::EmitSignedIntegerDivisionByConstant(
    Register result,
    Register dividend,
    int32_t divisor,
    Register remainder,
    Register scratch,
    LEnvironment* environment) {
  ASSERT(!AreAliased(dividend, scratch, at, no_reg));

  uint32_t divisor_abs = abs(divisor);

  int32_t power_of_2_factor =
    CompilerIntrinsics::CountTrailingZeros(divisor_abs);

  switch (divisor_abs) {
    case 0:
      DeoptimizeIf(al, environment);
      return;

    case 1:
      if (divisor > 0) {
        __ Move(result, dividend);
      } else {
        __ SubuAndCheckForOverflow(result, zero_reg, dividend, scratch);
        DeoptimizeIf(lt, environment, scratch, Operand(zero_reg));
      }
      // Compute the remainder.
      __ Move(remainder, zero_reg);
      return;

    default:
      if (IsPowerOf2(divisor_abs)) {
        // Branch and condition free code for integer division by a power
        // of two.
        int32_t power = WhichPowerOf2(divisor_abs);
        if (power > 1) {
          __ sra(scratch, dividend, power - 1);
        }
        __ srl(scratch, scratch, 32 - power);
        __ Addu(scratch, dividend, Operand(scratch));
        __ sra(result, scratch,  power);
        // Negate if necessary.
        // We don't need to check for overflow because the case '-1' is
        // handled separately.
        if (divisor < 0) {
          ASSERT(divisor != -1);
          __ Subu(result, zero_reg, Operand(result));
        }
        // Compute the remainder.
        if (divisor > 0) {
          __ sll(scratch, result, power);
          __ Subu(remainder, dividend, Operand(scratch));
        } else {
          __ sll(scratch, result, power);
          __ Addu(remainder, dividend, Operand(scratch));
        }
        return;
      } else if (LChunkBuilder::HasMagicNumberForDivisor(divisor)) {
        // Use magic numbers for a few specific divisors.
        // Details and proofs can be found in:
        // - Hacker's Delight, Henry S. Warren, Jr.
        // - The PowerPC Compiler Writer's Guide
        // and probably many others.
        //
        // We handle
        //   <divisor with magic numbers> * <power of 2>
        // but not
        //   <divisor with magic numbers> * <other divisor with magic numbers>
        DivMagicNumbers magic_numbers =
          DivMagicNumberFor(divisor_abs >> power_of_2_factor);
        // Branch and condition free code for integer division by a power
        // of two.
        const int32_t M = magic_numbers.M;
        const int32_t s = magic_numbers.s + power_of_2_factor;

        __ li(scratch, Operand(M));
        __ mult(dividend, scratch);
        __ mfhi(scratch);
        if (M < 0) {
          __ Addu(scratch, scratch, Operand(dividend));
        }
        if (s > 0) {
          __ sra(scratch, scratch, s);
          __ mov(scratch, scratch);
        }
        __ srl(at, dividend, 31);
        __ Addu(result, scratch, Operand(at));
        if (divisor < 0) __ Subu(result, zero_reg, Operand(result));
        // Compute the remainder.
        __ li(scratch, Operand(divisor));
        __ Mul(scratch, result, Operand(scratch));
        __ Subu(remainder, dividend, Operand(scratch));
      } else {
        __ li(scratch, Operand(divisor));
        __ div(dividend, scratch);
        __ mfhi(remainder);
        __ mflo(result);
      }
  }
}


void LCodeGen::DoDivI(LDivI* instr) {
  const Register left = ToRegister(instr->left());
  const Register right = ToRegister(instr->right());
  const Register result = ToRegister(instr->result());

  // On MIPS div is asynchronous - it will run in the background while we
  // check for special cases.
  __ div(left, right);

  // Check for x / 0.
  if (instr->hydrogen()->CheckFlag(HValue::kCanBeDivByZero)) {
    DeoptimizeIf(eq, instr->environment(), right, Operand(zero_reg));
  }

  // Check for (0 / -x) that will produce negative zero.
  if (instr->hydrogen()->CheckFlag(HValue::kBailoutOnMinusZero)) {
    Label left_not_zero;
    __ Branch(&left_not_zero, ne, left, Operand(zero_reg));
    DeoptimizeIf(lt, instr->environment(), right, Operand(zero_reg));
    __ bind(&left_not_zero);
  }

  // Check for (kMinInt / -1).
  if (instr->hydrogen()->CheckFlag(HValue::kCanOverflow)) {
    Label left_not_min_int;
    __ Branch(&left_not_min_int, ne, left, Operand(kMinInt));
    DeoptimizeIf(eq, instr->environment(), right, Operand(-1));
    __ bind(&left_not_min_int);
  }

  if (!instr->hydrogen()->CheckFlag(HInstruction::kAllUsesTruncatingToInt32)) {
    __ mfhi(result);
    DeoptimizeIf(ne, instr->environment(), result, Operand(zero_reg));
  }
  __ mflo(result);
}


void LCodeGen::DoMultiplyAddD(LMultiplyAddD* instr) {
  DoubleRegister addend = ToDoubleRegister(instr->addend());
  DoubleRegister multiplier = ToDoubleRegister(instr->multiplier());
  DoubleRegister multiplicand = ToDoubleRegister(instr->multiplicand());

  // This is computed in-place.
  ASSERT(addend.is(ToDoubleRegister(instr->result())));

  __ madd_d(addend, addend, multiplier, multiplicand);
}


void LCodeGen::DoMathFloorOfDiv(LMathFloorOfDiv* instr) {
  const Register result = ToRegister(instr->result());
  const Register left = ToRegister(instr->left());
  const Register remainder = ToRegister(instr->temp());
  const Register scratch = scratch0();

  if (instr->right()->IsConstantOperand()) {
    Label done;
    int32_t divisor = ToInteger32(LConstantOperand::cast(instr->right()));
    if (divisor < 0) {
      DeoptimizeIf(eq, instr->environment(), left, Operand(zero_reg));
    }
    EmitSignedIntegerDivisionByConstant(result,
                                        left,
                                        divisor,
                                        remainder,
                                        scratch,
                                        instr->environment());
    // We performed a truncating division. Correct the result if necessary.
    __ Branch(&done, eq, remainder, Operand(zero_reg), USE_DELAY_SLOT);
    __ Xor(scratch , remainder, Operand(divisor));
    __ Branch(&done, ge, scratch, Operand(zero_reg));
    __ Subu(result, result, Operand(1));
    __ bind(&done);
  } else {
    Label done;
    const Register right = ToRegister(instr->right());

    // On MIPS div is asynchronous - it will run in the background while we
    // check for special cases.
    __ div(left, right);

    // Check for x / 0.
    DeoptimizeIf(eq, instr->environment(), right, Operand(zero_reg));

    // Check for (0 / -x) that will produce negative zero.
    if (instr->hydrogen()->CheckFlag(HValue::kBailoutOnMinusZero)) {
      Label left_not_zero;
      __ Branch(&left_not_zero, ne, left, Operand(zero_reg));
      DeoptimizeIf(lt, instr->environment(), right, Operand(zero_reg));
      __ bind(&left_not_zero);
    }

    // Check for (kMinInt / -1).
    if (instr->hydrogen()->CheckFlag(HValue::kCanOverflow)) {
      Label left_not_min_int;
      __ Branch(&left_not_min_int, ne, left, Operand(kMinInt));
      DeoptimizeIf(eq, instr->environment(), right, Operand(-1));
      __ bind(&left_not_min_int);
    }

    __ mfhi(remainder);
    __ mflo(result);

    // We performed a truncating division. Correct the result if necessary.
    __ Branch(&done, eq, remainder, Operand(zero_reg), USE_DELAY_SLOT);
    __ Xor(scratch , remainder, Operand(right));
    __ Branch(&done, ge, scratch, Operand(zero_reg));
    __ Subu(result, result, Operand(1));
    __ bind(&done);
  }
}


void LCodeGen::DoMulI(LMulI* instr) {
  Register scratch = scratch0();
  Register result = ToRegister(instr->result());
  // Note that result may alias left.
  Register left = ToRegister(instr->left());
  LOperand* right_op = instr->right();

  bool bailout_on_minus_zero =
    instr->hydrogen()->CheckFlag(HValue::kBailoutOnMinusZero);
  bool overflow = instr->hydrogen()->CheckFlag(HValue::kCanOverflow);

  if (right_op->IsConstantOperand()) {
    int32_t constant = ToInteger32(LConstantOperand::cast(right_op));

    if (bailout_on_minus_zero && (constant < 0)) {
      // The case of a null constant will be handled separately.
      // If constant is negative and left is null, the result should be -0.
      DeoptimizeIf(eq, instr->environment(), left, Operand(zero_reg));
    }

    switch (constant) {
      case -1:
        if (overflow) {
          __ SubuAndCheckForOverflow(result, zero_reg, left, scratch);
          DeoptimizeIf(lt, instr->environment(), scratch, Operand(zero_reg));
        } else {
          __ Subu(result, zero_reg, left);
        }
        break;
      case 0:
        if (bailout_on_minus_zero) {
          // If left is strictly negative and the constant is null, the
          // result is -0. Deoptimize if required, otherwise return 0.
          DeoptimizeIf(lt, instr->environment(), left, Operand(zero_reg));
        }
        __ mov(result, zero_reg);
        break;
      case 1:
        // Nothing to do.
        __ Move(result, left);
        break;
      default:
        // Multiplying by powers of two and powers of two plus or minus
        // one can be done faster with shifted operands.
        // For other constants we emit standard code.
        int32_t mask = constant >> 31;
        uint32_t constant_abs = (constant + mask) ^ mask;

        if (IsPowerOf2(constant_abs)) {
          int32_t shift = WhichPowerOf2(constant_abs);
          __ sll(result, left, shift);
          // Correct the sign of the result if the constant is negative.
          if (constant < 0)  __ Subu(result, zero_reg, result);
        } else if (IsPowerOf2(constant_abs - 1)) {
          int32_t shift = WhichPowerOf2(constant_abs - 1);
          __ sll(scratch, left, shift);
          __ Addu(result, scratch, left);
          // Correct the sign of the result if the constant is negative.
          if (constant < 0)  __ Subu(result, zero_reg, result);
        } else if (IsPowerOf2(constant_abs + 1)) {
          int32_t shift = WhichPowerOf2(constant_abs + 1);
          __ sll(scratch, left, shift);
          __ Subu(result, scratch, left);
          // Correct the sign of the result if the constant is negative.
          if (constant < 0)  __ Subu(result, zero_reg, result);
        } else {
          // Generate standard code.
          __ li(at, constant);
          __ Mul(result, left, at);
        }
    }

  } else {
    ASSERT(right_op->IsRegister());
    Register right = ToRegister(right_op);

    if (overflow) {
      // hi:lo = left * right.
      if (instr->hydrogen()->representation().IsSmi()) {
        __ SmiUntag(result, left);
        __ mult(result, right);
        __ mfhi(scratch);
        __ mflo(result);
      } else {
        __ mult(left, right);
        __ mfhi(scratch);
        __ mflo(result);
      }
      __ sra(at, result, 31);
      DeoptimizeIf(ne, instr->environment(), scratch, Operand(at));
    } else {
      if (instr->hydrogen()->representation().IsSmi()) {
        __ SmiUntag(result, left);
        __ Mul(result, result, right);
      } else {
        __ Mul(result, left, right);
      }
    }

    if (bailout_on_minus_zero) {
      Label done;
      __ Xor(at, left, right);
      __ Branch(&done, ge, at, Operand(zero_reg));
      // Bail out if the result is minus zero.
      DeoptimizeIf(eq,
                   instr->environment(),
                   result,
                   Operand(zero_reg));
      __ bind(&done);
    }
  }
}


void LCodeGen::DoBitI(LBitI* instr) {
  LOperand* left_op = instr->left();
  LOperand* right_op = instr->right();
  ASSERT(left_op->IsRegister());
  Register left = ToRegister(left_op);
  Register result = ToRegister(instr->result());
  Operand right(no_reg);

  if (right_op->IsStackSlot() || right_op->IsArgument()) {
    right = Operand(EmitLoadRegister(right_op, at));
  } else {
    ASSERT(right_op->IsRegister() || right_op->IsConstantOperand());
    right = ToOperand(right_op);
  }

  switch (instr->op()) {
    case Token::BIT_AND:
      __ And(result, left, right);
      break;
    case Token::BIT_OR:
      __ Or(result, left, right);
      break;
    case Token::BIT_XOR:
      if (right_op->IsConstantOperand() && right.immediate() == int32_t(~0)) {
        __ Nor(result, zero_reg, left);
      } else {
        __ Xor(result, left, right);
      }
      break;
    default:
      UNREACHABLE();
      break;
  }
}


void LCodeGen::DoShiftI(LShiftI* instr) {
  // Both 'left' and 'right' are "used at start" (see LCodeGen::DoShift), so
  // result may alias either of them.
  LOperand* right_op = instr->right();
  Register left = ToRegister(instr->left());
  Register result = ToRegister(instr->result());
  Register scratch = scratch0();

  if (right_op->IsRegister()) {
    // No need to mask the right operand on MIPS, it is built into the variable
    // shift instructions.
    switch (instr->op()) {
      case Token::ROR:
        __ Ror(result, left, Operand(ToRegister(right_op)));
        break;
      case Token::SAR:
        __ srav(result, left, ToRegister(right_op));
        break;
      case Token::SHR:
        __ srlv(result, left, ToRegister(right_op));
        if (instr->can_deopt()) {
          DeoptimizeIf(lt, instr->environment(), result, Operand(zero_reg));
        }
        break;
      case Token::SHL:
        __ sllv(result, left, ToRegister(right_op));
        break;
      default:
        UNREACHABLE();
        break;
    }
  } else {
    // Mask the right_op operand.
    int value = ToInteger32(LConstantOperand::cast(right_op));
    uint8_t shift_count = static_cast<uint8_t>(value & 0x1F);
    switch (instr->op()) {
      case Token::ROR:
        if (shift_count != 0) {
          __ Ror(result, left, Operand(shift_count));
        } else {
          __ Move(result, left);
        }
        break;
      case Token::SAR:
        if (shift_count != 0) {
          __ sra(result, left, shift_count);
        } else {
          __ Move(result, left);
        }
        break;
      case Token::SHR:
        if (shift_count != 0) {
          __ srl(result, left, shift_count);
        } else {
          if (instr->can_deopt()) {
            __ And(at, left, Operand(0x80000000));
            DeoptimizeIf(ne, instr->environment(), at, Operand(zero_reg));
          }
          __ Move(result, left);
        }
        break;
      case Token::SHL:
        if (shift_count != 0) {
          if (instr->hydrogen_value()->representation().IsSmi() &&
              instr->can_deopt()) {
            if (shift_count != 1) {
              __ sll(result, left, shift_count - 1);
              __ SmiTagCheckOverflow(result, result, scratch);
            } else {
              __ SmiTagCheckOverflow(result, left, scratch);
            }
            DeoptimizeIf(lt, instr->environment(), scratch, Operand(zero_reg));
          } else {
            __ sll(result, left, shift_count);
          }
        } else {
          __ Move(result, left);
        }
        break;
      default:
        UNREACHABLE();
        break;
    }
  }
}


void LCodeGen::DoSubI(LSubI* instr) {
  LOperand* left = instr->left();
  LOperand* right = instr->right();
  LOperand* result = instr->result();
  bool can_overflow = instr->hydrogen()->CheckFlag(HValue::kCanOverflow);

  if (!can_overflow) {
    if (right->IsStackSlot() || right->IsArgument()) {
      Register right_reg = EmitLoadRegister(right, at);
      __ Subu(ToRegister(result), ToRegister(left), Operand(right_reg));
    } else {
      ASSERT(right->IsRegister() || right->IsConstantOperand());
      __ Subu(ToRegister(result), ToRegister(left), ToOperand(right));
    }
  } else {  // can_overflow.
    Register overflow = scratch0();
    Register scratch = scratch1();
    if (right->IsStackSlot() ||
        right->IsArgument() ||
        right->IsConstantOperand()) {
      Register right_reg = EmitLoadRegister(right, scratch);
      __ SubuAndCheckForOverflow(ToRegister(result),
                                 ToRegister(left),
                                 right_reg,
                                 overflow);  // Reg at also used as scratch.
    } else {
      ASSERT(right->IsRegister());
      // Due to overflow check macros not supporting constant operands,
      // handling the IsConstantOperand case was moved to prev if clause.
      __ SubuAndCheckForOverflow(ToRegister(result),
                                 ToRegister(left),
                                 ToRegister(right),
                                 overflow);  // Reg at also used as scratch.
    }
    DeoptimizeIf(lt, instr->environment(), overflow, Operand(zero_reg));
  }
}


void LCodeGen::DoConstantI(LConstantI* instr) {
  __ li(ToRegister(instr->result()), Operand(instr->value()));
}


void LCodeGen::DoConstantS(LConstantS* instr) {
  __ li(ToRegister(instr->result()), Operand(instr->value()));
}


void LCodeGen::DoConstantD(LConstantD* instr) {
  ASSERT(instr->result()->IsDoubleRegister());
  DoubleRegister result = ToDoubleRegister(instr->result());
  double v = instr->value();
  __ Move(result, v);
}


void LCodeGen::DoConstantE(LConstantE* instr) {
  __ li(ToRegister(instr->result()), Operand(instr->value()));
}


void LCodeGen::DoConstantT(LConstantT* instr) {
  Handle<Object> value = instr->value(isolate());
  AllowDeferredHandleDereference smi_check;
  __ li(ToRegister(instr->result()), value);
}


void LCodeGen::DoMapEnumLength(LMapEnumLength* instr) {
  Register result = ToRegister(instr->result());
  Register map = ToRegister(instr->value());
  __ EnumLength(result, map);
}


void LCodeGen::DoElementsKind(LElementsKind* instr) {
  Register result = ToRegister(instr->result());
  Register input = ToRegister(instr->value());

  // Load map into |result|.
  __ lw(result, FieldMemOperand(input, HeapObject::kMapOffset));
  // Load the map's "bit field 2" into |result|. We only need the first byte,
  // but the following bit field extraction takes care of that anyway.
  __ lbu(result, FieldMemOperand(result, Map::kBitField2Offset));
  // Retrieve elements_kind from bit field 2.
  __ Ext(result, result, Map::kElementsKindShift, Map::kElementsKindBitCount);
}


void LCodeGen::DoValueOf(LValueOf* instr) {
  Register input = ToRegister(instr->value());
  Register result = ToRegister(instr->result());
  Register map = ToRegister(instr->temp());
  Label done;

  if (!instr->hydrogen()->value()->IsHeapObject()) {
    // If the object is a smi return the object.
    __ Move(result, input);
    __ JumpIfSmi(input, &done);
  }

  // If the object is not a value type, return the object.
  __ GetObjectType(input, map, map);
  __ Branch(&done, ne, map, Operand(JS_VALUE_TYPE));
  __ lw(result, FieldMemOperand(input, JSValue::kValueOffset));

  __ bind(&done);
}


void LCodeGen::DoDateField(LDateField* instr) {
  Register object = ToRegister(instr->date());
  Register result = ToRegister(instr->result());
  Register scratch = ToRegister(instr->temp());
  Smi* index = instr->index();
  Label runtime, done;
  ASSERT(object.is(a0));
  ASSERT(result.is(v0));
  ASSERT(!scratch.is(scratch0()));
  ASSERT(!scratch.is(object));

  __ And(at, object, Operand(kSmiTagMask));
  DeoptimizeIf(eq, instr->environment(), at, Operand(zero_reg));
  __ GetObjectType(object, scratch, scratch);
  DeoptimizeIf(ne, instr->environment(), scratch, Operand(JS_DATE_TYPE));

  if (index->value() == 0) {
    __ lw(result, FieldMemOperand(object, JSDate::kValueOffset));
  } else {
    if (index->value() < JSDate::kFirstUncachedField) {
      ExternalReference stamp = ExternalReference::date_cache_stamp(isolate());
      __ li(scratch, Operand(stamp));
      __ lw(scratch, MemOperand(scratch));
      __ lw(scratch0(), FieldMemOperand(object, JSDate::kCacheStampOffset));
      __ Branch(&runtime, ne, scratch, Operand(scratch0()));
      __ lw(result, FieldMemOperand(object, JSDate::kValueOffset +
                                            kPointerSize * index->value()));
      __ jmp(&done);
    }
    __ bind(&runtime);
    __ PrepareCallCFunction(2, scratch);
    __ li(a1, Operand(index));
    __ CallCFunction(ExternalReference::get_date_field_function(isolate()), 2);
    __ bind(&done);
  }
}


void LCodeGen::DoSeqStringSetChar(LSeqStringSetChar* instr) {
  Register string = ToRegister(instr->string());
  LOperand* index_op = instr->index();
  Register value = ToRegister(instr->value());
  Register scratch = scratch0();
  String::Encoding encoding = instr->encoding();

  if (FLAG_debug_code) {
    __ lw(scratch, FieldMemOperand(string, HeapObject::kMapOffset));
    __ lbu(scratch, FieldMemOperand(scratch, Map::kInstanceTypeOffset));

    __ And(scratch, scratch,
           Operand(kStringRepresentationMask | kStringEncodingMask));
    static const uint32_t one_byte_seq_type = kSeqStringTag | kOneByteStringTag;
    static const uint32_t two_byte_seq_type = kSeqStringTag | kTwoByteStringTag;
    __ Subu(at, scratch, Operand(encoding == String::ONE_BYTE_ENCODING
                                ? one_byte_seq_type : two_byte_seq_type));
    __ Check(eq, kUnexpectedStringType, at, Operand(zero_reg));
  }

  if (index_op->IsConstantOperand()) {
    int constant_index = ToInteger32(LConstantOperand::cast(index_op));
    if (encoding == String::ONE_BYTE_ENCODING) {
      __ sb(value,
          FieldMemOperand(string, SeqString::kHeaderSize + constant_index));
    } else {
      __ sh(value,
          FieldMemOperand(string, SeqString::kHeaderSize + constant_index * 2));
    }
  } else {
    Register index = ToRegister(index_op);
    if (encoding == String::ONE_BYTE_ENCODING) {
      __ Addu(scratch, string, Operand(index));
      __ sb(value, FieldMemOperand(scratch, SeqString::kHeaderSize));
    } else {
      __ sll(scratch, index, 1);
      __ Addu(scratch, string, scratch);
      __ sh(value, FieldMemOperand(scratch, SeqString::kHeaderSize));
    }
  }
}


void LCodeGen::DoThrow(LThrow* instr) {
  Register input_reg = EmitLoadRegister(instr->value(), at);
  __ push(input_reg);
  ASSERT(ToRegister(instr->context()).is(cp));
  CallRuntime(Runtime::kThrow, 1, instr);

  if (FLAG_debug_code) {
    __ stop("Unreachable code.");
  }
}


void LCodeGen::DoAddI(LAddI* instr) {
  LOperand* left = instr->left();
  LOperand* right = instr->right();
  LOperand* result = instr->result();
  bool can_overflow = instr->hydrogen()->CheckFlag(HValue::kCanOverflow);

  if (!can_overflow) {
    if (right->IsStackSlot() || right->IsArgument()) {
      Register right_reg = EmitLoadRegister(right, at);
      __ Addu(ToRegister(result), ToRegister(left), Operand(right_reg));
    } else {
      ASSERT(right->IsRegister() || right->IsConstantOperand());
      __ Addu(ToRegister(result), ToRegister(left), ToOperand(right));
    }
  } else {  // can_overflow.
    Register overflow = scratch0();
    Register scratch = scratch1();
    if (right->IsStackSlot() ||
        right->IsArgument() ||
        right->IsConstantOperand()) {
      Register right_reg = EmitLoadRegister(right, scratch);
      __ AdduAndCheckForOverflow(ToRegister(result),
                                 ToRegister(left),
                                 right_reg,
                                 overflow);  // Reg at also used as scratch.
    } else {
      ASSERT(right->IsRegister());
      // Due to overflow check macros not supporting constant operands,
      // handling the IsConstantOperand case was moved to prev if clause.
      __ AdduAndCheckForOverflow(ToRegister(result),
                                 ToRegister(left),
                                 ToRegister(right),
                                 overflow);  // Reg at also used as scratch.
    }
    DeoptimizeIf(lt, instr->environment(), overflow, Operand(zero_reg));
  }
}


void LCodeGen::DoMathMinMax(LMathMinMax* instr) {
  LOperand* left = instr->left();
  LOperand* right = instr->right();
  HMathMinMax::Operation operation = instr->hydrogen()->operation();
  Condition condition = (operation == HMathMinMax::kMathMin) ? le : ge;
  if (instr->hydrogen()->representation().IsSmiOrInteger32()) {
    Register left_reg = ToRegister(left);
    Operand right_op = (right->IsRegister() || right->IsConstantOperand())
        ? ToOperand(right)
        : Operand(EmitLoadRegister(right, at));
    Register result_reg = ToRegister(instr->result());
    Label return_right, done;
    if (!result_reg.is(left_reg)) {
      __ Branch(&return_right, NegateCondition(condition), left_reg, right_op);
      __ mov(result_reg, left_reg);
      __ Branch(&done);
    }
    __ Branch(&done, condition, left_reg, right_op);
    __ bind(&return_right);
    __ Addu(result_reg, zero_reg, right_op);
    __ bind(&done);
  } else {
    ASSERT(instr->hydrogen()->representation().IsDouble());
    FPURegister left_reg = ToDoubleRegister(left);
    FPURegister right_reg = ToDoubleRegister(right);
    FPURegister result_reg = ToDoubleRegister(instr->result());
    Label check_nan_left, check_zero, return_left, return_right, done;
    __ BranchF(&check_zero, &check_nan_left, eq, left_reg, right_reg);
    __ BranchF(&return_left, NULL, condition, left_reg, right_reg);
    __ Branch(&return_right);

    __ bind(&check_zero);
    // left == right != 0.
    __ BranchF(&return_left, NULL, ne, left_reg, kDoubleRegZero);
    // At this point, both left and right are either 0 or -0.
    if (operation == HMathMinMax::kMathMin) {
      __ neg_d(left_reg, left_reg);
      __ sub_d(result_reg, left_reg, right_reg);
      __ neg_d(result_reg, result_reg);
    } else {
      __ add_d(result_reg, left_reg, right_reg);
    }
    __ Branch(&done);

    __ bind(&check_nan_left);
    // left == NaN.
    __ BranchF(NULL, &return_left, eq, left_reg, left_reg);
    __ bind(&return_right);
    if (!right_reg.is(result_reg)) {
      __ mov_d(result_reg, right_reg);
    }
    __ Branch(&done);

    __ bind(&return_left);
    if (!left_reg.is(result_reg)) {
      __ mov_d(result_reg, left_reg);
    }
    __ bind(&done);
  }
}


void LCodeGen::DoArithmeticD(LArithmeticD* instr) {
  DoubleRegister left = ToDoubleRegister(instr->left());
  DoubleRegister right = ToDoubleRegister(instr->right());
  DoubleRegister result = ToDoubleRegister(instr->result());
  switch (instr->op()) {
    case Token::ADD:
      __ add_d(result, left, right);
      break;
    case Token::SUB:
      __ sub_d(result, left, right);
      break;
    case Token::MUL:
      __ mul_d(result, left, right);
      break;
    case Token::DIV:
      __ div_d(result, left, right);
      break;
    case Token::MOD: {
      // Save a0-a3 on the stack.
      RegList saved_regs = a0.bit() | a1.bit() | a2.bit() | a3.bit();
      __ MultiPush(saved_regs);

      __ PrepareCallCFunction(0, 2, scratch0());
      __ SetCallCDoubleArguments(left, right);
      __ CallCFunction(
          ExternalReference::double_fp_operation(Token::MOD, isolate()),
          0, 2);
      // Move the result in the double result register.
      __ GetCFunctionDoubleResult(result);

      // Restore saved register.
      __ MultiPop(saved_regs);
      break;
    }
    default:
      UNREACHABLE();
      break;
  }
}


void LCodeGen::DoArithmeticT(LArithmeticT* instr) {
  ASSERT(ToRegister(instr->context()).is(cp));
  ASSERT(ToRegister(instr->left()).is(a1));
  ASSERT(ToRegister(instr->right()).is(a0));
  ASSERT(ToRegister(instr->result()).is(v0));

  BinaryOpStub stub(instr->op(), NO_OVERWRITE);
  CallCode(stub.GetCode(isolate()), RelocInfo::CODE_TARGET, instr);
  // Other arch use a nop here, to signal that there is no inlined
  // patchable code. Mips does not need the nop, since our marker
  // instruction (andi zero_reg) will never be used in normal code.
}


template<class InstrType>
void LCodeGen::EmitBranch(InstrType instr,
                          Condition condition,
                          Register src1,
                          const Operand& src2) {
  int left_block = instr->TrueDestination(chunk_);
  int right_block = instr->FalseDestination(chunk_);

  int next_block = GetNextEmittedBlock();
  if (right_block == left_block || condition == al) {
    EmitGoto(left_block);
  } else if (left_block == next_block) {
    __ Branch(chunk_->GetAssemblyLabel(right_block),
              NegateCondition(condition), src1, src2);
  } else if (right_block == next_block) {
    __ Branch(chunk_->GetAssemblyLabel(left_block), condition, src1, src2);
  } else {
    __ Branch(chunk_->GetAssemblyLabel(left_block), condition, src1, src2);
    __ Branch(chunk_->GetAssemblyLabel(right_block));
  }
}


template<class InstrType>
void LCodeGen::EmitBranchF(InstrType instr,
                           Condition condition,
                           FPURegister src1,
                           FPURegister src2) {
  int right_block = instr->FalseDestination(chunk_);
  int left_block = instr->TrueDestination(chunk_);

  int next_block = GetNextEmittedBlock();
  if (right_block == left_block) {
    EmitGoto(left_block);
  } else if (left_block == next_block) {
    __ BranchF(chunk_->GetAssemblyLabel(right_block), NULL,
               NegateCondition(condition), src1, src2);
  } else if (right_block == next_block) {
    __ BranchF(chunk_->GetAssemblyLabel(left_block), NULL,
               condition, src1, src2);
  } else {
    __ BranchF(chunk_->GetAssemblyLabel(left_block), NULL,
               condition, src1, src2);
    __ Branch(chunk_->GetAssemblyLabel(right_block));
  }
}


template<class InstrType>
void LCodeGen::EmitFalseBranchF(InstrType instr,
                                Condition condition,
                                FPURegister src1,
                                FPURegister src2) {
  int false_block = instr->FalseDestination(chunk_);
  __ BranchF(chunk_->GetAssemblyLabel(false_block), NULL,
             condition, src1, src2);
}


void LCodeGen::DoDebugBreak(LDebugBreak* instr) {
  __ stop("LDebugBreak");
}


void LCodeGen::DoBranch(LBranch* instr) {
  Representation r = instr->hydrogen()->value()->representation();
  if (r.IsInteger32() || r.IsSmi()) {
    ASSERT(!info()->IsStub());
    Register reg = ToRegister(instr->value());
    EmitBranch(instr, ne, reg, Operand(zero_reg));
  } else if (r.IsDouble()) {
    ASSERT(!info()->IsStub());
    DoubleRegister reg = ToDoubleRegister(instr->value());
    // Test the double value. Zero and NaN are false.
    EmitBranchF(instr, nue, reg, kDoubleRegZero);
  } else {
    ASSERT(r.IsTagged());
    Register reg = ToRegister(instr->value());
    HType type = instr->hydrogen()->value()->type();
    if (type.IsBoolean()) {
      ASSERT(!info()->IsStub());
      __ LoadRoot(at, Heap::kTrueValueRootIndex);
      EmitBranch(instr, eq, reg, Operand(at));
    } else if (type.IsSmi()) {
      ASSERT(!info()->IsStub());
      EmitBranch(instr, ne, reg, Operand(zero_reg));
    } else if (type.IsJSArray()) {
      ASSERT(!info()->IsStub());
      EmitBranch(instr, al, zero_reg, Operand(zero_reg));
    } else if (type.IsHeapNumber()) {
      ASSERT(!info()->IsStub());
      DoubleRegister dbl_scratch = double_scratch0();
      __ ldc1(dbl_scratch, FieldMemOperand(reg, HeapNumber::kValueOffset));
      // Test the double value. Zero and NaN are false.
      EmitBranchF(instr, nue, dbl_scratch, kDoubleRegZero);
    } else if (type.IsString()) {
      ASSERT(!info()->IsStub());
      __ lw(at, FieldMemOperand(reg, String::kLengthOffset));
      EmitBranch(instr, ne, at, Operand(zero_reg));
    } else {
      ToBooleanStub::Types expected = instr->hydrogen()->expected_input_types();
      // Avoid deopts in the case where we've never executed this path before.
      if (expected.IsEmpty()) expected = ToBooleanStub::Types::Generic();

      if (expected.Contains(ToBooleanStub::UNDEFINED)) {
        // undefined -> false.
        __ LoadRoot(at, Heap::kUndefinedValueRootIndex);
        __ Branch(instr->FalseLabel(chunk_), eq, reg, Operand(at));
      }
      if (expected.Contains(ToBooleanStub::BOOLEAN)) {
        // Boolean -> its value.
        __ LoadRoot(at, Heap::kTrueValueRootIndex);
        __ Branch(instr->TrueLabel(chunk_), eq, reg, Operand(at));
        __ LoadRoot(at, Heap::kFalseValueRootIndex);
        __ Branch(instr->FalseLabel(chunk_), eq, reg, Operand(at));
      }
      if (expected.Contains(ToBooleanStub::NULL_TYPE)) {
        // 'null' -> false.
        __ LoadRoot(at, Heap::kNullValueRootIndex);
        __ Branch(instr->FalseLabel(chunk_), eq, reg, Operand(at));
      }

      if (expected.Contains(ToBooleanStub::SMI)) {
        // Smis: 0 -> false, all other -> true.
        __ Branch(instr->FalseLabel(chunk_), eq, reg, Operand(zero_reg));
        __ JumpIfSmi(reg, instr->TrueLabel(chunk_));
      } else if (expected.NeedsMap()) {
        // If we need a map later and have a Smi -> deopt.
        __ And(at, reg, Operand(kSmiTagMask));
        DeoptimizeIf(eq, instr->environment(), at, Operand(zero_reg));
      }

      const Register map = scratch0();
      if (expected.NeedsMap()) {
        __ lw(map, FieldMemOperand(reg, HeapObject::kMapOffset));
        if (expected.CanBeUndetectable()) {
          // Undetectable -> false.
          __ lbu(at, FieldMemOperand(map, Map::kBitFieldOffset));
          __ And(at, at, Operand(1 << Map::kIsUndetectable));
          __ Branch(instr->FalseLabel(chunk_), ne, at, Operand(zero_reg));
        }
      }

      if (expected.Contains(ToBooleanStub::SPEC_OBJECT)) {
        // spec object -> true.
        __ lbu(at, FieldMemOperand(map, Map::kInstanceTypeOffset));
        __ Branch(instr->TrueLabel(chunk_),
                  ge, at, Operand(FIRST_SPEC_OBJECT_TYPE));
      }

      if (expected.Contains(ToBooleanStub::STRING)) {
        // String value -> false iff empty.
        Label not_string;
        __ lbu(at, FieldMemOperand(map, Map::kInstanceTypeOffset));
        __ Branch(&not_string, ge , at, Operand(FIRST_NONSTRING_TYPE));
        __ lw(at, FieldMemOperand(reg, String::kLengthOffset));
        __ Branch(instr->TrueLabel(chunk_), ne, at, Operand(zero_reg));
        __ Branch(instr->FalseLabel(chunk_));
        __ bind(&not_string);
      }

      if (expected.Contains(ToBooleanStub::SYMBOL)) {
        // Symbol value -> true.
        const Register scratch = scratch1();
        __ lbu(scratch, FieldMemOperand(map, Map::kInstanceTypeOffset));
        __ Branch(instr->TrueLabel(chunk_), eq, scratch, Operand(SYMBOL_TYPE));
      }

      if (expected.Contains(ToBooleanStub::HEAP_NUMBER)) {
        // heap number -> false iff +0, -0, or NaN.
        DoubleRegister dbl_scratch = double_scratch0();
        Label not_heap_number;
        __ LoadRoot(at, Heap::kHeapNumberMapRootIndex);
        __ Branch(&not_heap_number, ne, map, Operand(at));
        __ ldc1(dbl_scratch, FieldMemOperand(reg, HeapNumber::kValueOffset));
        __ BranchF(instr->TrueLabel(chunk_), instr->FalseLabel(chunk_),
                   ne, dbl_scratch, kDoubleRegZero);
        // Falls through if dbl_scratch == 0.
        __ Branch(instr->FalseLabel(chunk_));
        __ bind(&not_heap_number);
      }

      if (!expected.IsGeneric()) {
        // We've seen something for the first time -> deopt.
        // This can only happen if we are not generic already.
        DeoptimizeIf(al, instr->environment(), zero_reg, Operand(zero_reg));
      }
    }
  }
}


void LCodeGen::EmitGoto(int block) {
  if (!IsNextEmittedBlock(block)) {
    __ jmp(chunk_->GetAssemblyLabel(LookupDestination(block)));
  }
}


void LCodeGen::DoGoto(LGoto* instr) {
  EmitGoto(instr->block_id());
}


Condition LCodeGen::TokenToCondition(Token::Value op, bool is_unsigned) {
  Condition cond = kNoCondition;
  switch (op) {
    case Token::EQ:
    case Token::EQ_STRICT:
      cond = eq;
      break;
    case Token::NE:
    case Token::NE_STRICT:
      cond = ne;
      break;
    case Token::LT:
      cond = is_unsigned ? lo : lt;
      break;
    case Token::GT:
      cond = is_unsigned ? hi : gt;
      break;
    case Token::LTE:
      cond = is_unsigned ? ls : le;
      break;
    case Token::GTE:
      cond = is_unsigned ? hs : ge;
      break;
    case Token::IN:
    case Token::INSTANCEOF:
    default:
      UNREACHABLE();
  }
  return cond;
}


void LCodeGen::DoCompareNumericAndBranch(LCompareNumericAndBranch* instr) {
  LOperand* left = instr->left();
  LOperand* right = instr->right();
  Condition cond = TokenToCondition(instr->op(), false);

  if (left->IsConstantOperand() && right->IsConstantOperand()) {
    // We can statically evaluate the comparison.
    double left_val = ToDouble(LConstantOperand::cast(left));
    double right_val = ToDouble(LConstantOperand::cast(right));
    int next_block = EvalComparison(instr->op(), left_val, right_val) ?
        instr->TrueDestination(chunk_) : instr->FalseDestination(chunk_);
    EmitGoto(next_block);
  } else {
    if (instr->is_double()) {
      // Compare left and right as doubles and load the
      // resulting flags into the normal status register.
      FPURegister left_reg = ToDoubleRegister(left);
      FPURegister right_reg = ToDoubleRegister(right);

      // If a NaN is involved, i.e. the result is unordered,
      // jump to false block label.
      __ BranchF(NULL, instr->FalseLabel(chunk_), eq,
                 left_reg, right_reg);

      EmitBranchF(instr, cond, left_reg, right_reg);
    } else {
      Register cmp_left;
      Operand cmp_right = Operand(0);

      if (right->IsConstantOperand()) {
        int32_t value = ToInteger32(LConstantOperand::cast(right));
        if (instr->hydrogen_value()->representation().IsSmi()) {
          cmp_left = ToRegister(left);
          cmp_right = Operand(Smi::FromInt(value));
        } else {
          cmp_left = ToRegister(left);
          cmp_right = Operand(value);
        }
      } else if (left->IsConstantOperand()) {
        int32_t value = ToInteger32(LConstantOperand::cast(left));
        if (instr->hydrogen_value()->representation().IsSmi()) {
           cmp_left = ToRegister(right);
           cmp_right = Operand(Smi::FromInt(value));
        } else {
          cmp_left = ToRegister(right);
          cmp_right = Operand(value);
        }
        // We transposed the operands. Reverse the condition.
        cond = ReverseCondition(cond);
      } else {
        cmp_left = ToRegister(left);
        cmp_right = Operand(ToRegister(right));
      }

      EmitBranch(instr, cond, cmp_left, cmp_right);
    }
  }
}


void LCodeGen::DoCmpObjectEqAndBranch(LCmpObjectEqAndBranch* instr) {
  Register left = ToRegister(instr->left());
  Register right = ToRegister(instr->right());

  EmitBranch(instr, eq, left, Operand(right));
}


void LCodeGen::DoCmpHoleAndBranch(LCmpHoleAndBranch* instr) {
  if (instr->hydrogen()->representation().IsTagged()) {
    Register input_reg = ToRegister(instr->object());
    __ li(at, Operand(factory()->the_hole_value()));
    EmitBranch(instr, eq, input_reg, Operand(at));
    return;
  }

  DoubleRegister input_reg = ToDoubleRegister(instr->object());
  EmitFalseBranchF(instr, eq, input_reg, input_reg);

  Register scratch = scratch0();
  __ FmoveHigh(scratch, input_reg);
  EmitBranch(instr, eq, scratch, Operand(kHoleNanUpper32));
}


Condition LCodeGen::EmitIsObject(Register input,
                                 Register temp1,
                                 Register temp2,
                                 Label* is_not_object,
                                 Label* is_object) {
  __ JumpIfSmi(input, is_not_object);

  __ LoadRoot(temp2, Heap::kNullValueRootIndex);
  __ Branch(is_object, eq, input, Operand(temp2));

  // Load map.
  __ lw(temp1, FieldMemOperand(input, HeapObject::kMapOffset));
  // Undetectable objects behave like undefined.
  __ lbu(temp2, FieldMemOperand(temp1, Map::kBitFieldOffset));
  __ And(temp2, temp2, Operand(1 << Map::kIsUndetectable));
  __ Branch(is_not_object, ne, temp2, Operand(zero_reg));

  // Load instance type and check that it is in object type range.
  __ lbu(temp2, FieldMemOperand(temp1, Map::kInstanceTypeOffset));
  __ Branch(is_not_object,
            lt, temp2, Operand(FIRST_NONCALLABLE_SPEC_OBJECT_TYPE));

  return le;
}


void LCodeGen::DoIsObjectAndBranch(LIsObjectAndBranch* instr) {
  Register reg = ToRegister(instr->value());
  Register temp1 = ToRegister(instr->temp());
  Register temp2 = scratch0();

  Condition true_cond =
      EmitIsObject(reg, temp1, temp2,
          instr->FalseLabel(chunk_), instr->TrueLabel(chunk_));

  EmitBranch(instr, true_cond, temp2,
             Operand(LAST_NONCALLABLE_SPEC_OBJECT_TYPE));
}


Condition LCodeGen::EmitIsString(Register input,
                                 Register temp1,
                                 Label* is_not_string,
                                 SmiCheck check_needed = INLINE_SMI_CHECK) {
  if (check_needed == INLINE_SMI_CHECK) {
    __ JumpIfSmi(input, is_not_string);
  }
  __ GetObjectType(input, temp1, temp1);

  return lt;
}


void LCodeGen::DoIsStringAndBranch(LIsStringAndBranch* instr) {
  Register reg = ToRegister(instr->value());
  Register temp1 = ToRegister(instr->temp());

  SmiCheck check_needed =
      instr->hydrogen()->value()->IsHeapObject()
          ? OMIT_SMI_CHECK : INLINE_SMI_CHECK;
  Condition true_cond =
      EmitIsString(reg, temp1, instr->FalseLabel(chunk_), check_needed);

  EmitBranch(instr, true_cond, temp1,
             Operand(FIRST_NONSTRING_TYPE));
}


void LCodeGen::DoIsSmiAndBranch(LIsSmiAndBranch* instr) {
  Register input_reg = EmitLoadRegister(instr->value(), at);
  __ And(at, input_reg, kSmiTagMask);
  EmitBranch(instr, eq, at, Operand(zero_reg));
}


void LCodeGen::DoIsUndetectableAndBranch(LIsUndetectableAndBranch* instr) {
  Register input = ToRegister(instr->value());
  Register temp = ToRegister(instr->temp());

  if (!instr->hydrogen()->value()->IsHeapObject()) {
    __ JumpIfSmi(input, instr->FalseLabel(chunk_));
  }
  __ lw(temp, FieldMemOperand(input, HeapObject::kMapOffset));
  __ lbu(temp, FieldMemOperand(temp, Map::kBitFieldOffset));
  __ And(at, temp, Operand(1 << Map::kIsUndetectable));
  EmitBranch(instr, ne, at, Operand(zero_reg));
}


static Condition ComputeCompareCondition(Token::Value op) {
  switch (op) {
    case Token::EQ_STRICT:
    case Token::EQ:
      return eq;
    case Token::LT:
      return lt;
    case Token::GT:
      return gt;
    case Token::LTE:
      return le;
    case Token::GTE:
      return ge;
    default:
      UNREACHABLE();
      return kNoCondition;
  }
}


void LCodeGen::DoStringCompareAndBranch(LStringCompareAndBranch* instr) {
  ASSERT(ToRegister(instr->context()).is(cp));
  Token::Value op = instr->op();

  Handle<Code> ic = CompareIC::GetUninitialized(isolate(), op);
  CallCode(ic, RelocInfo::CODE_TARGET, instr);

  Condition condition = ComputeCompareCondition(op);

  EmitBranch(instr, condition, v0, Operand(zero_reg));
}


static InstanceType TestType(HHasInstanceTypeAndBranch* instr) {
  InstanceType from = instr->from();
  InstanceType to = instr->to();
  if (from == FIRST_TYPE) return to;
  ASSERT(from == to || to == LAST_TYPE);
  return from;
}


static Condition BranchCondition(HHasInstanceTypeAndBranch* instr) {
  InstanceType from = instr->from();
  InstanceType to = instr->to();
  if (from == to) return eq;
  if (to == LAST_TYPE) return hs;
  if (from == FIRST_TYPE) return ls;
  UNREACHABLE();
  return eq;
}


void LCodeGen::DoHasInstanceTypeAndBranch(LHasInstanceTypeAndBranch* instr) {
  Register scratch = scratch0();
  Register input = ToRegister(instr->value());

  if (!instr->hydrogen()->value()->IsHeapObject()) {
    __ JumpIfSmi(input, instr->FalseLabel(chunk_));
  }

  __ GetObjectType(input, scratch, scratch);
  EmitBranch(instr,
             BranchCondition(instr->hydrogen()),
             scratch,
             Operand(TestType(instr->hydrogen())));
}


void LCodeGen::DoGetCachedArrayIndex(LGetCachedArrayIndex* instr) {
  Register input = ToRegister(instr->value());
  Register result = ToRegister(instr->result());

  __ AssertString(input);

  __ lw(result, FieldMemOperand(input, String::kHashFieldOffset));
  __ IndexFromHash(result, result);
}


void LCodeGen::DoHasCachedArrayIndexAndBranch(
    LHasCachedArrayIndexAndBranch* instr) {
  Register input = ToRegister(instr->value());
  Register scratch = scratch0();

  __ lw(scratch,
         FieldMemOperand(input, String::kHashFieldOffset));
  __ And(at, scratch, Operand(String::kContainsCachedArrayIndexMask));
  EmitBranch(instr, eq, at, Operand(zero_reg));
}


// Branches to a label or falls through with the answer in flags.  Trashes
// the temp registers, but not the input.
void LCodeGen::EmitClassOfTest(Label* is_true,
                               Label* is_false,
                               Handle<String>class_name,
                               Register input,
                               Register temp,
                               Register temp2) {
  ASSERT(!input.is(temp));
  ASSERT(!input.is(temp2));
  ASSERT(!temp.is(temp2));

  __ JumpIfSmi(input, is_false);

  if (class_name->IsOneByteEqualTo(STATIC_ASCII_VECTOR("Function"))) {
    // Assuming the following assertions, we can use the same compares to test
    // for both being a function type and being in the object type range.
    STATIC_ASSERT(NUM_OF_CALLABLE_SPEC_OBJECT_TYPES == 2);
    STATIC_ASSERT(FIRST_NONCALLABLE_SPEC_OBJECT_TYPE ==
                  FIRST_SPEC_OBJECT_TYPE + 1);
    STATIC_ASSERT(LAST_NONCALLABLE_SPEC_OBJECT_TYPE ==
                  LAST_SPEC_OBJECT_TYPE - 1);
    STATIC_ASSERT(LAST_SPEC_OBJECT_TYPE == LAST_TYPE);

    __ GetObjectType(input, temp, temp2);
    __ Branch(is_false, lt, temp2, Operand(FIRST_SPEC_OBJECT_TYPE));
    __ Branch(is_true, eq, temp2, Operand(FIRST_SPEC_OBJECT_TYPE));
    __ Branch(is_true, eq, temp2, Operand(LAST_SPEC_OBJECT_TYPE));
  } else {
    // Faster code path to avoid two compares: subtract lower bound from the
    // actual type and do a signed compare with the width of the type range.
    __ GetObjectType(input, temp, temp2);
    __ Subu(temp2, temp2, Operand(FIRST_NONCALLABLE_SPEC_OBJECT_TYPE));
    __ Branch(is_false, gt, temp2, Operand(LAST_NONCALLABLE_SPEC_OBJECT_TYPE -
                                           FIRST_NONCALLABLE_SPEC_OBJECT_TYPE));
  }

  // Now we are in the FIRST-LAST_NONCALLABLE_SPEC_OBJECT_TYPE range.
  // Check if the constructor in the map is a function.
  __ lw(temp, FieldMemOperand(temp, Map::kConstructorOffset));

  // Objects with a non-function constructor have class 'Object'.
  __ GetObjectType(temp, temp2, temp2);
  if (class_name->IsOneByteEqualTo(STATIC_ASCII_VECTOR("Object"))) {
    __ Branch(is_true, ne, temp2, Operand(JS_FUNCTION_TYPE));
  } else {
    __ Branch(is_false, ne, temp2, Operand(JS_FUNCTION_TYPE));
  }

  // temp now contains the constructor function. Grab the
  // instance class name from there.
  __ lw(temp, FieldMemOperand(temp, JSFunction::kSharedFunctionInfoOffset));
  __ lw(temp, FieldMemOperand(temp,
                               SharedFunctionInfo::kInstanceClassNameOffset));
  // The class name we are testing against is internalized since it's a literal.
  // The name in the constructor is internalized because of the way the context
  // is booted.  This routine isn't expected to work for random API-created
  // classes and it doesn't have to because you can't access it with natives
  // syntax.  Since both sides are internalized it is sufficient to use an
  // identity comparison.

  // End with the address of this class_name instance in temp register.
  // On MIPS, the caller must do the comparison with Handle<String>class_name.
}


void LCodeGen::DoClassOfTestAndBranch(LClassOfTestAndBranch* instr) {
  Register input = ToRegister(instr->value());
  Register temp = scratch0();
  Register temp2 = ToRegister(instr->temp());
  Handle<String> class_name = instr->hydrogen()->class_name();

  EmitClassOfTest(instr->TrueLabel(chunk_), instr->FalseLabel(chunk_),
                  class_name, input, temp, temp2);

  EmitBranch(instr, eq, temp, Operand(class_name));
}


void LCodeGen::DoCmpMapAndBranch(LCmpMapAndBranch* instr) {
  Register reg = ToRegister(instr->value());
  Register temp = ToRegister(instr->temp());

  __ lw(temp, FieldMemOperand(reg, HeapObject::kMapOffset));
  EmitBranch(instr, eq, temp, Operand(instr->map()));
}


void LCodeGen::DoInstanceOf(LInstanceOf* instr) {
  ASSERT(ToRegister(instr->context()).is(cp));
  Label true_label, done;
  ASSERT(ToRegister(instr->left()).is(a0));  // Object is in a0.
  ASSERT(ToRegister(instr->right()).is(a1));  // Function is in a1.
  Register result = ToRegister(instr->result());
  ASSERT(result.is(v0));

  InstanceofStub stub(InstanceofStub::kArgsInRegisters);
  CallCode(stub.GetCode(isolate()), RelocInfo::CODE_TARGET, instr);

  __ Branch(&true_label, eq, result, Operand(zero_reg));
  __ li(result, Operand(factory()->false_value()));
  __ Branch(&done);
  __ bind(&true_label);
  __ li(result, Operand(factory()->true_value()));
  __ bind(&done);
}


void LCodeGen::DoInstanceOfKnownGlobal(LInstanceOfKnownGlobal* instr) {
  class DeferredInstanceOfKnownGlobal V8_FINAL : public LDeferredCode {
   public:
    DeferredInstanceOfKnownGlobal(LCodeGen* codegen,
                                  LInstanceOfKnownGlobal* instr)
        : LDeferredCode(codegen), instr_(instr) { }
    virtual void Generate() V8_OVERRIDE {
      codegen()->DoDeferredInstanceOfKnownGlobal(instr_, &map_check_);
    }
    virtual LInstruction* instr() V8_OVERRIDE { return instr_; }
    Label* map_check() { return &map_check_; }

   private:
    LInstanceOfKnownGlobal* instr_;
    Label map_check_;
  };

  DeferredInstanceOfKnownGlobal* deferred;
  deferred = new(zone()) DeferredInstanceOfKnownGlobal(this, instr);

  Label done, false_result;
  Register object = ToRegister(instr->value());
  Register temp = ToRegister(instr->temp());
  Register result = ToRegister(instr->result());

  ASSERT(object.is(a0));
  ASSERT(result.is(v0));

  // A Smi is not instance of anything.
  __ JumpIfSmi(object, &false_result);

  // This is the inlined call site instanceof cache. The two occurences of the
  // hole value will be patched to the last map/result pair generated by the
  // instanceof stub.
  Label cache_miss;
  Register map = temp;
  __ lw(map, FieldMemOperand(object, HeapObject::kMapOffset));

  Assembler::BlockTrampolinePoolScope block_trampoline_pool(masm_);
  __ bind(deferred->map_check());  // Label for calculating code patching.
  // We use Factory::the_hole_value() on purpose instead of loading from the
  // root array to force relocation to be able to later patch with
  // the cached map.
  Handle<Cell> cell = factory()->NewCell(factory()->the_hole_value());
  __ li(at, Operand(Handle<Object>(cell)));
  __ lw(at, FieldMemOperand(at, PropertyCell::kValueOffset));
  __ Branch(&cache_miss, ne, map, Operand(at));
  // We use Factory::the_hole_value() on purpose instead of loading from the
  // root array to force relocation to be able to later patch
  // with true or false.
  __ li(result, Operand(factory()->the_hole_value()), CONSTANT_SIZE);
  __ Branch(&done);

  // The inlined call site cache did not match. Check null and string before
  // calling the deferred code.
  __ bind(&cache_miss);
  // Null is not instance of anything.
  __ LoadRoot(temp, Heap::kNullValueRootIndex);
  __ Branch(&false_result, eq, object, Operand(temp));

  // String values is not instance of anything.
  Condition cc = __ IsObjectStringType(object, temp, temp);
  __ Branch(&false_result, cc, temp, Operand(zero_reg));

  // Go to the deferred code.
  __ Branch(deferred->entry());

  __ bind(&false_result);
  __ LoadRoot(result, Heap::kFalseValueRootIndex);

  // Here result has either true or false. Deferred code also produces true or
  // false object.
  __ bind(deferred->exit());
  __ bind(&done);
}


void LCodeGen::DoDeferredInstanceOfKnownGlobal(LInstanceOfKnownGlobal* instr,
                                               Label* map_check) {
  Register result = ToRegister(instr->result());
  ASSERT(result.is(v0));

  InstanceofStub::Flags flags = InstanceofStub::kNoFlags;
  flags = static_cast<InstanceofStub::Flags>(
      flags | InstanceofStub::kArgsInRegisters);
  flags = static_cast<InstanceofStub::Flags>(
      flags | InstanceofStub::kCallSiteInlineCheck);
  flags = static_cast<InstanceofStub::Flags>(
      flags | InstanceofStub::kReturnTrueFalseObject);
  InstanceofStub stub(flags);

  PushSafepointRegistersScope scope(this, Safepoint::kWithRegisters);
  LoadContextFromDeferred(instr->context());

  // Get the temp register reserved by the instruction. This needs to be t0 as
  // its slot of the pushing of safepoint registers is used to communicate the
  // offset to the location of the map check.
  Register temp = ToRegister(instr->temp());
  ASSERT(temp.is(t0));
  __ li(InstanceofStub::right(), instr->function());
  static const int kAdditionalDelta = 7;
  int delta = masm_->InstructionsGeneratedSince(map_check) + kAdditionalDelta;
  Label before_push_delta;
  __ bind(&before_push_delta);
  {
    Assembler::BlockTrampolinePoolScope block_trampoline_pool(masm_);
    __ li(temp, Operand(delta * kPointerSize), CONSTANT_SIZE);
    __ StoreToSafepointRegisterSlot(temp, temp);
  }
  CallCodeGeneric(stub.GetCode(isolate()),
                  RelocInfo::CODE_TARGET,
                  instr,
                  RECORD_SAFEPOINT_WITH_REGISTERS_AND_NO_ARGUMENTS);
  LEnvironment* env = instr->GetDeferredLazyDeoptimizationEnvironment();
  safepoints_.RecordLazyDeoptimizationIndex(env->deoptimization_index());
  // Put the result value into the result register slot and
  // restore all registers.
  __ StoreToSafepointRegisterSlot(result, result);
}


void LCodeGen::DoCmpT(LCmpT* instr) {
  ASSERT(ToRegister(instr->context()).is(cp));
  Token::Value op = instr->op();

  Handle<Code> ic = CompareIC::GetUninitialized(isolate(), op);
  CallCode(ic, RelocInfo::CODE_TARGET, instr);
  // On MIPS there is no need for a "no inlined smi code" marker (nop).

  Condition condition = ComputeCompareCondition(op);
  // A minor optimization that relies on LoadRoot always emitting one
  // instruction.
  Assembler::BlockTrampolinePoolScope block_trampoline_pool(masm());
  Label done, check;
  __ Branch(USE_DELAY_SLOT, &done, condition, v0, Operand(zero_reg));
  __ bind(&check);
  __ LoadRoot(ToRegister(instr->result()), Heap::kTrueValueRootIndex);
  ASSERT_EQ(1, masm()->InstructionsGeneratedSince(&check));
  __ LoadRoot(ToRegister(instr->result()), Heap::kFalseValueRootIndex);
  __ bind(&done);
}


void LCodeGen::DoReturn(LReturn* instr) {
  if (FLAG_trace && info()->IsOptimizing()) {
    // Push the return value on the stack as the parameter.
    // Runtime::TraceExit returns its parameter in v0. We're leaving the code
    // managed by the register allocator and tearing down the frame, it's
    // safe to write to the context register.
    __ push(v0);
    __ lw(cp, MemOperand(fp, StandardFrameConstants::kContextOffset));
    __ CallRuntime(Runtime::kTraceExit, 1);
  }
  if (info()->saves_caller_doubles()) {
    ASSERT(NeedsEagerFrame());
    BitVector* doubles = chunk()->allocated_double_registers();
    BitVector::Iterator save_iterator(doubles);
    int count = 0;
    while (!save_iterator.Done()) {
      __ ldc1(DoubleRegister::FromAllocationIndex(save_iterator.Current()),
              MemOperand(sp, count * kDoubleSize));
      save_iterator.Advance();
      count++;
    }
  }
  int no_frame_start = -1;
  if (NeedsEagerFrame()) {
    __ mov(sp, fp);
    no_frame_start = masm_->pc_offset();
    __ Pop(ra, fp);
  }
  if (instr->has_constant_parameter_count()) {
    int parameter_count = ToInteger32(instr->constant_parameter_count());
    int32_t sp_delta = (parameter_count + 1) * kPointerSize;
    if (sp_delta != 0) {
      __ Addu(sp, sp, Operand(sp_delta));
    }
  } else {
    Register reg = ToRegister(instr->parameter_count());
    // The argument count parameter is a smi
    __ SmiUntag(reg);
    __ sll(at, reg, kPointerSizeLog2);
    __ Addu(sp, sp, at);
  }

  __ Jump(ra);

  if (no_frame_start != -1) {
    info_->AddNoFrameRange(no_frame_start, masm_->pc_offset());
  }
}


void LCodeGen::DoLoadGlobalCell(LLoadGlobalCell* instr) {
  Register result = ToRegister(instr->result());
  __ li(at, Operand(Handle<Object>(instr->hydrogen()->cell().handle())));
  __ lw(result, FieldMemOperand(at, Cell::kValueOffset));
  if (instr->hydrogen()->RequiresHoleCheck()) {
    __ LoadRoot(at, Heap::kTheHoleValueRootIndex);
    DeoptimizeIf(eq, instr->environment(), result, Operand(at));
  }
}


void LCodeGen::DoLoadGlobalGeneric(LLoadGlobalGeneric* instr) {
  ASSERT(ToRegister(instr->context()).is(cp));
  ASSERT(ToRegister(instr->global_object()).is(a0));
  ASSERT(ToRegister(instr->result()).is(v0));

  __ li(a2, Operand(instr->name()));
  RelocInfo::Mode mode = instr->for_typeof() ? RelocInfo::CODE_TARGET
                                             : RelocInfo::CODE_TARGET_CONTEXT;
  Handle<Code> ic = isolate()->builtins()->LoadIC_Initialize();
  CallCode(ic, mode, instr);
}


void LCodeGen::DoStoreGlobalCell(LStoreGlobalCell* instr) {
  Register value = ToRegister(instr->value());
  Register cell = scratch0();

  // Load the cell.
  __ li(cell, Operand(instr->hydrogen()->cell().handle()));

  // If the cell we are storing to contains the hole it could have
  // been deleted from the property dictionary. In that case, we need
  // to update the property details in the property dictionary to mark
  // it as no longer deleted.
  if (instr->hydrogen()->RequiresHoleCheck()) {
    // We use a temp to check the payload.
    Register payload = ToRegister(instr->temp());
    __ lw(payload, FieldMemOperand(cell, Cell::kValueOffset));
    __ LoadRoot(at, Heap::kTheHoleValueRootIndex);
    DeoptimizeIf(eq, instr->environment(), payload, Operand(at));
  }

  // Store the value.
  __ sw(value, FieldMemOperand(cell, Cell::kValueOffset));
  // Cells are always rescanned, so no write barrier here.
}


void LCodeGen::DoStoreGlobalGeneric(LStoreGlobalGeneric* instr) {
  ASSERT(ToRegister(instr->context()).is(cp));
  ASSERT(ToRegister(instr->global_object()).is(a1));
  ASSERT(ToRegister(instr->value()).is(a0));

  __ li(a2, Operand(instr->name()));
  Handle<Code> ic = (instr->strict_mode_flag() == kStrictMode)
      ? isolate()->builtins()->StoreIC_Initialize_Strict()
      : isolate()->builtins()->StoreIC_Initialize();
  CallCode(ic, RelocInfo::CODE_TARGET_CONTEXT, instr);
}


void LCodeGen::DoLoadContextSlot(LLoadContextSlot* instr) {
  Register context = ToRegister(instr->context());
  Register result = ToRegister(instr->result());

  __ lw(result, ContextOperand(context, instr->slot_index()));
  if (instr->hydrogen()->RequiresHoleCheck()) {
    __ LoadRoot(at, Heap::kTheHoleValueRootIndex);

    if (instr->hydrogen()->DeoptimizesOnHole()) {
      DeoptimizeIf(eq, instr->environment(), result, Operand(at));
    } else {
      Label is_not_hole;
      __ Branch(&is_not_hole, ne, result, Operand(at));
      __ LoadRoot(result, Heap::kUndefinedValueRootIndex);
      __ bind(&is_not_hole);
    }
  }
}


void LCodeGen::DoStoreContextSlot(LStoreContextSlot* instr) {
  Register context = ToRegister(instr->context());
  Register value = ToRegister(instr->value());
  Register scratch = scratch0();
  MemOperand target = ContextOperand(context, instr->slot_index());

  Label skip_assignment;

  if (instr->hydrogen()->RequiresHoleCheck()) {
    __ lw(scratch, target);
    __ LoadRoot(at, Heap::kTheHoleValueRootIndex);

    if (instr->hydrogen()->DeoptimizesOnHole()) {
      DeoptimizeIf(eq, instr->environment(), scratch, Operand(at));
    } else {
      __ Branch(&skip_assignment, ne, scratch, Operand(at));
    }
  }

  __ sw(value, target);
  if (instr->hydrogen()->NeedsWriteBarrier()) {
    SmiCheck check_needed =
        instr->hydrogen()->value()->IsHeapObject()
            ? OMIT_SMI_CHECK : INLINE_SMI_CHECK;
    __ RecordWriteContextSlot(context,
                              target.offset(),
                              value,
                              scratch0(),
                              GetRAState(),
                              kSaveFPRegs,
                              EMIT_REMEMBERED_SET,
                              check_needed);
  }

  __ bind(&skip_assignment);
}


void LCodeGen::DoLoadNamedField(LLoadNamedField* instr) {
  HObjectAccess access = instr->hydrogen()->access();
  int offset = access.offset();
  Register object = ToRegister(instr->object());

  if (access.IsExternalMemory()) {
    Register result = ToRegister(instr->result());
    MemOperand operand = MemOperand(object, offset);
    if (access.representation().IsByte()) {
      __ lb(result, operand);
    } else {
      __ lw(result, operand);
    }
    return;
  }

  if (instr->hydrogen()->representation().IsDouble()) {
    DoubleRegister result = ToDoubleRegister(instr->result());
    __ ldc1(result, FieldMemOperand(object, offset));
    return;
  }

  Register result = ToRegister(instr->result());
  if (!access.IsInobject()) {
    __ lw(result, FieldMemOperand(object, JSObject::kPropertiesOffset));
    object = result;
  }
  MemOperand operand = FieldMemOperand(object, offset);
  if (access.representation().IsByte()) {
    __ lb(result, operand);
  } else {
    __ lw(result, operand);
  }
}


void LCodeGen::DoLoadNamedGeneric(LLoadNamedGeneric* instr) {
  ASSERT(ToRegister(instr->context()).is(cp));
  ASSERT(ToRegister(instr->object()).is(a0));
  ASSERT(ToRegister(instr->result()).is(v0));

  // Name is always in a2.
  __ li(a2, Operand(instr->name()));
  Handle<Code> ic = isolate()->builtins()->LoadIC_Initialize();
  CallCode(ic, RelocInfo::CODE_TARGET, instr);
}


void LCodeGen::DoLoadFunctionPrototype(LLoadFunctionPrototype* instr) {
  Register scratch = scratch0();
  Register function = ToRegister(instr->function());
  Register result = ToRegister(instr->result());

  // Check that the function really is a function. Load map into the
  // result register.
  __ GetObjectType(function, result, scratch);
  DeoptimizeIf(ne, instr->environment(), scratch, Operand(JS_FUNCTION_TYPE));

  // Make sure that the function has an instance prototype.
  Label non_instance;
  __ lbu(scratch, FieldMemOperand(result, Map::kBitFieldOffset));
  __ And(scratch, scratch, Operand(1 << Map::kHasNonInstancePrototype));
  __ Branch(&non_instance, ne, scratch, Operand(zero_reg));

  // Get the prototype or initial map from the function.
  __ lw(result,
         FieldMemOperand(function, JSFunction::kPrototypeOrInitialMapOffset));

  // Check that the function has a prototype or an initial map.
  __ LoadRoot(at, Heap::kTheHoleValueRootIndex);
  DeoptimizeIf(eq, instr->environment(), result, Operand(at));

  // If the function does not have an initial map, we're done.
  Label done;
  __ GetObjectType(result, scratch, scratch);
  __ Branch(&done, ne, scratch, Operand(MAP_TYPE));

  // Get the prototype from the initial map.
  __ lw(result, FieldMemOperand(result, Map::kPrototypeOffset));
  __ Branch(&done);

  // Non-instance prototype: Fetch prototype from constructor field
  // in initial map.
  __ bind(&non_instance);
  __ lw(result, FieldMemOperand(result, Map::kConstructorOffset));

  // All done.
  __ bind(&done);
}


void LCodeGen::DoLoadRoot(LLoadRoot* instr) {
  Register result = ToRegister(instr->result());
  __ LoadRoot(result, instr->index());
}


void LCodeGen::DoLoadExternalArrayPointer(
    LLoadExternalArrayPointer* instr) {
  Register to_reg = ToRegister(instr->result());
  Register from_reg  = ToRegister(instr->object());
  __ lw(to_reg, FieldMemOperand(from_reg,
                                ExternalArray::kExternalPointerOffset));
}


void LCodeGen::DoAccessArgumentsAt(LAccessArgumentsAt* instr) {
  Register arguments = ToRegister(instr->arguments());
  Register result = ToRegister(instr->result());
  if (instr->length()->IsConstantOperand() &&
      instr->index()->IsConstantOperand()) {
    int const_index = ToInteger32(LConstantOperand::cast(instr->index()));
    int const_length = ToInteger32(LConstantOperand::cast(instr->length()));
    int index = (const_length - const_index) + 1;
    __ lw(result, MemOperand(arguments, index * kPointerSize));
  } else {
    Register length = ToRegister(instr->length());
    Register index = ToRegister(instr->index());
    // There are two words between the frame pointer and the last argument.
    // Subtracting from length accounts for one of them, add one more.
    __ subu(length, length, index);
    __ Addu(length, length, Operand(1));
    __ sll(length, length, kPointerSizeLog2);
    __ Addu(at, arguments, Operand(length));
    __ lw(result, MemOperand(at, 0));
  }
}


void LCodeGen::DoLoadKeyedExternalArray(LLoadKeyed* instr) {
  Register external_pointer = ToRegister(instr->elements());
  Register key = no_reg;
  ElementsKind elements_kind = instr->elements_kind();
  bool key_is_constant = instr->key()->IsConstantOperand();
  int constant_key = 0;
  if (key_is_constant) {
    constant_key = ToInteger32(LConstantOperand::cast(instr->key()));
    if (constant_key & 0xF0000000) {
      Abort(kArrayIndexConstantValueTooBig);
    }
  } else {
    key = ToRegister(instr->key());
  }
  int element_size_shift = ElementsKindToShiftSize(elements_kind);
  int shift_size = (instr->hydrogen()->key()->representation().IsSmi())
      ? (element_size_shift - kSmiTagSize) : element_size_shift;
  int additional_offset = instr->additional_index() << element_size_shift;

  if (elements_kind == EXTERNAL_FLOAT_ELEMENTS ||
      elements_kind == EXTERNAL_DOUBLE_ELEMENTS) {
    FPURegister result = ToDoubleRegister(instr->result());
    if (key_is_constant) {
      __ Addu(scratch0(), external_pointer, constant_key << element_size_shift);
    } else {
      __ sll(scratch0(), key, shift_size);
      __ Addu(scratch0(), scratch0(), external_pointer);
    }
    if (elements_kind == EXTERNAL_FLOAT_ELEMENTS) {
      __ lwc1(result, MemOperand(scratch0(), additional_offset));
      __ cvt_d_s(result, result);
    } else  {  // i.e. elements_kind == EXTERNAL_DOUBLE_ELEMENTS
      __ ldc1(result, MemOperand(scratch0(), additional_offset));
    }
  } else {
    Register result = ToRegister(instr->result());
    MemOperand mem_operand = PrepareKeyedOperand(
        key, external_pointer, key_is_constant, constant_key,
        element_size_shift, shift_size,
        instr->additional_index(), additional_offset);
    switch (elements_kind) {
      case EXTERNAL_BYTE_ELEMENTS:
        __ lb(result, mem_operand);
        break;
      case EXTERNAL_PIXEL_ELEMENTS:
      case EXTERNAL_UNSIGNED_BYTE_ELEMENTS:
        __ lbu(result, mem_operand);
        break;
      case EXTERNAL_SHORT_ELEMENTS:
        __ lh(result, mem_operand);
        break;
      case EXTERNAL_UNSIGNED_SHORT_ELEMENTS:
        __ lhu(result, mem_operand);
        break;
      case EXTERNAL_INT_ELEMENTS:
        __ lw(result, mem_operand);
        break;
      case EXTERNAL_UNSIGNED_INT_ELEMENTS:
        __ lw(result, mem_operand);
        if (!instr->hydrogen()->CheckFlag(HInstruction::kUint32)) {
          DeoptimizeIf(Ugreater_equal, instr->environment(),
              result, Operand(0x80000000));
        }
        break;
      case EXTERNAL_FLOAT_ELEMENTS:
      case EXTERNAL_DOUBLE_ELEMENTS:
      case FAST_DOUBLE_ELEMENTS:
      case FAST_ELEMENTS:
      case FAST_SMI_ELEMENTS:
      case FAST_HOLEY_DOUBLE_ELEMENTS:
      case FAST_HOLEY_ELEMENTS:
      case FAST_HOLEY_SMI_ELEMENTS:
      case DICTIONARY_ELEMENTS:
      case NON_STRICT_ARGUMENTS_ELEMENTS:
        UNREACHABLE();
        break;
    }
  }
}


void LCodeGen::DoLoadKeyedFixedDoubleArray(LLoadKeyed* instr) {
  Register elements = ToRegister(instr->elements());
  bool key_is_constant = instr->key()->IsConstantOperand();
  Register key = no_reg;
  DoubleRegister result = ToDoubleRegister(instr->result());
  Register scratch = scratch0();

  int element_size_shift = ElementsKindToShiftSize(FAST_DOUBLE_ELEMENTS);

  int base_offset =
      FixedDoubleArray::kHeaderSize - kHeapObjectTag +
      (instr->additional_index() << element_size_shift);
  if (key_is_constant) {
    int constant_key = ToInteger32(LConstantOperand::cast(instr->key()));
    if (constant_key & 0xF0000000) {
      Abort(kArrayIndexConstantValueTooBig);
    }
    base_offset += constant_key << element_size_shift;
  }
  __ Addu(scratch, elements, Operand(base_offset));

  if (!key_is_constant) {
    key = ToRegister(instr->key());
    int shift_size = (instr->hydrogen()->key()->representation().IsSmi())
        ? (element_size_shift - kSmiTagSize) : element_size_shift;
    __ sll(at, key, shift_size);
    __ Addu(scratch, scratch, at);
  }

  __ ldc1(result, MemOperand(scratch));

  if (instr->hydrogen()->RequiresHoleCheck()) {
    __ lw(scratch, MemOperand(scratch, sizeof(kHoleNanLower32)));
    DeoptimizeIf(eq, instr->environment(), scratch, Operand(kHoleNanUpper32));
  }
}


void LCodeGen::DoLoadKeyedFixedArray(LLoadKeyed* instr) {
  Register elements = ToRegister(instr->elements());
  Register result = ToRegister(instr->result());
  Register scratch = scratch0();
  Register store_base = scratch;
  int offset = 0;

  if (instr->key()->IsConstantOperand()) {
    LConstantOperand* const_operand = LConstantOperand::cast(instr->key());
    offset = FixedArray::OffsetOfElementAt(ToInteger32(const_operand) +
                                           instr->additional_index());
    store_base = elements;
  } else {
    Register key = ToRegister(instr->key());
    // Even though the HLoadKeyed instruction forces the input
    // representation for the key to be an integer, the input gets replaced
    // during bound check elimination with the index argument to the bounds
    // check, which can be tagged, so that case must be handled here, too.
    if (instr->hydrogen()->key()->representation().IsSmi()) {
      __ sll(scratch, key, kPointerSizeLog2 - kSmiTagSize);
      __ addu(scratch, elements, scratch);
    } else {
      __ sll(scratch, key, kPointerSizeLog2);
      __ addu(scratch, elements, scratch);
    }
    offset = FixedArray::OffsetOfElementAt(instr->additional_index());
  }
  __ lw(result, FieldMemOperand(store_base, offset));

  // Check for the hole value.
  if (instr->hydrogen()->RequiresHoleCheck()) {
    if (IsFastSmiElementsKind(instr->hydrogen()->elements_kind())) {
      __ And(scratch, result, Operand(kSmiTagMask));
      DeoptimizeIf(ne, instr->environment(), scratch, Operand(zero_reg));
    } else {
      __ LoadRoot(scratch, Heap::kTheHoleValueRootIndex);
      DeoptimizeIf(eq, instr->environment(), result, Operand(scratch));
    }
  }
}


void LCodeGen::DoLoadKeyed(LLoadKeyed* instr) {
  if (instr->is_external()) {
    DoLoadKeyedExternalArray(instr);
  } else if (instr->hydrogen()->representation().IsDouble()) {
    DoLoadKeyedFixedDoubleArray(instr);
  } else {
    DoLoadKeyedFixedArray(instr);
  }
}


MemOperand LCodeGen::PrepareKeyedOperand(Register key,
                                         Register base,
                                         bool key_is_constant,
                                         int constant_key,
                                         int element_size,
                                         int shift_size,
                                         int additional_index,
                                         int additional_offset) {
  if (additional_index != 0 && !key_is_constant) {
    additional_index *= 1 << (element_size - shift_size);
    __ Addu(scratch0(), key, Operand(additional_index));
  }

  if (key_is_constant) {
    return MemOperand(base,
                      (constant_key << element_size) + additional_offset);
  }

  if (additional_index == 0) {
    if (shift_size >= 0) {
      __ sll(scratch0(), key, shift_size);
      __ Addu(scratch0(), base, scratch0());
      return MemOperand(scratch0());
    } else {
      ASSERT_EQ(-1, shift_size);
      __ srl(scratch0(), key, 1);
      __ Addu(scratch0(), base, scratch0());
      return MemOperand(scratch0());
    }
  }

  if (shift_size >= 0) {
    __ sll(scratch0(), scratch0(), shift_size);
    __ Addu(scratch0(), base, scratch0());
    return MemOperand(scratch0());
  } else {
    ASSERT_EQ(-1, shift_size);
    __ srl(scratch0(), scratch0(), 1);
    __ Addu(scratch0(), base, scratch0());
    return MemOperand(scratch0());
  }
}


void LCodeGen::DoLoadKeyedGeneric(LLoadKeyedGeneric* instr) {
  ASSERT(ToRegister(instr->context()).is(cp));
  ASSERT(ToRegister(instr->object()).is(a1));
  ASSERT(ToRegister(instr->key()).is(a0));

  Handle<Code> ic = isolate()->builtins()->KeyedLoadIC_Initialize();
  CallCode(ic, RelocInfo::CODE_TARGET, instr);
}


void LCodeGen::DoArgumentsElements(LArgumentsElements* instr) {
  Register scratch = scratch0();
  Register temp = scratch1();
  Register result = ToRegister(instr->result());

  if (instr->hydrogen()->from_inlined()) {
    __ Subu(result, sp, 2 * kPointerSize);
  } else {
    // Check if the calling frame is an arguments adaptor frame.
    Label done, adapted;
    __ lw(scratch, MemOperand(fp, StandardFrameConstants::kCallerFPOffset));
    __ lw(result, MemOperand(scratch, StandardFrameConstants::kContextOffset));
    __ Xor(temp, result, Operand(Smi::FromInt(StackFrame::ARGUMENTS_ADAPTOR)));

    // Result is the frame pointer for the frame if not adapted and for the real
    // frame below the adaptor frame if adapted.
    __ Movn(result, fp, temp);  // Move only if temp is not equal to zero (ne).
    __ Movz(result, scratch, temp);  // Move only if temp is equal to zero (eq).
  }
}


void LCodeGen::DoArgumentsLength(LArgumentsLength* instr) {
  Register elem = ToRegister(instr->elements());
  Register result = ToRegister(instr->result());

  Label done;

  // If no arguments adaptor frame the number of arguments is fixed.
  __ Addu(result, zero_reg, Operand(scope()->num_parameters()));
  __ Branch(&done, eq, fp, Operand(elem));

  // Arguments adaptor frame present. Get argument length from there.
  __ lw(result, MemOperand(fp, StandardFrameConstants::kCallerFPOffset));
  __ lw(result,
        MemOperand(result, ArgumentsAdaptorFrameConstants::kLengthOffset));
  __ SmiUntag(result);

  // Argument length is in result register.
  __ bind(&done);
}


void LCodeGen::DoWrapReceiver(LWrapReceiver* instr) {
  Register receiver = ToRegister(instr->receiver());
  Register function = ToRegister(instr->function());
  Register scratch = scratch0();

  // If the receiver is null or undefined, we have to pass the global
  // object as a receiver to normal functions. Values have to be
  // passed unchanged to builtins and strict-mode functions.
  Label global_object, receiver_ok;

  // Do not transform the receiver to object for strict mode
  // functions.
  __ lw(scratch,
         FieldMemOperand(function, JSFunction::kSharedFunctionInfoOffset));
  __ lw(scratch,
         FieldMemOperand(scratch, SharedFunctionInfo::kCompilerHintsOffset));

  // Do not transform the receiver to object for builtins.
  int32_t strict_mode_function_mask =
                  1 <<  (SharedFunctionInfo::kStrictModeFunction + kSmiTagSize);
  int32_t native_mask = 1 << (SharedFunctionInfo::kNative + kSmiTagSize);
  __ And(scratch, scratch, Operand(strict_mode_function_mask | native_mask));
  __ Branch(&receiver_ok, ne, scratch, Operand(zero_reg));

  // Normal function. Replace undefined or null with global receiver.
  __ LoadRoot(scratch, Heap::kNullValueRootIndex);
  __ Branch(&global_object, eq, receiver, Operand(scratch));
  __ LoadRoot(scratch, Heap::kUndefinedValueRootIndex);
  __ Branch(&global_object, eq, receiver, Operand(scratch));

  // Deoptimize if the receiver is not a JS object.
  __ And(scratch, receiver, Operand(kSmiTagMask));
  DeoptimizeIf(eq, instr->environment(), scratch, Operand(zero_reg));

  __ GetObjectType(receiver, scratch, scratch);
  DeoptimizeIf(lt, instr->environment(),
               scratch, Operand(FIRST_SPEC_OBJECT_TYPE));
  __ Branch(&receiver_ok);

  __ bind(&global_object);
  __ lw(receiver, GlobalObjectOperand());
  __ lw(receiver,
         FieldMemOperand(receiver, JSGlobalObject::kGlobalReceiverOffset));
  __ bind(&receiver_ok);
}


void LCodeGen::DoApplyArguments(LApplyArguments* instr) {
  Register receiver = ToRegister(instr->receiver());
  Register function = ToRegister(instr->function());
  Register length = ToRegister(instr->length());
  Register elements = ToRegister(instr->elements());
  Register scratch = scratch0();
  ASSERT(receiver.is(a0));  // Used for parameter count.
  ASSERT(function.is(a1));  // Required by InvokeFunction.
  ASSERT(ToRegister(instr->result()).is(v0));

  // Copy the arguments to this function possibly from the
  // adaptor frame below it.
  const uint32_t kArgumentsLimit = 1 * KB;
  DeoptimizeIf(hi, instr->environment(), length, Operand(kArgumentsLimit));

  // Push the receiver and use the register to keep the original
  // number of arguments.
  __ push(receiver);
  __ Move(receiver, length);
  // The arguments are at a one pointer size offset from elements.
  __ Addu(elements, elements, Operand(1 * kPointerSize));

  // Loop through the arguments pushing them onto the execution
  // stack.
  Label invoke, loop;
  // length is a small non-negative integer, due to the test above.
  __ Branch(USE_DELAY_SLOT, &invoke, eq, length, Operand(zero_reg));
  __ sll(scratch, length, 2);
  __ bind(&loop);
  __ Addu(scratch, elements, scratch);
  __ lw(scratch, MemOperand(scratch));
  __ push(scratch);
  __ Subu(length, length, Operand(1));
  __ Branch(USE_DELAY_SLOT, &loop, ne, length, Operand(zero_reg));
  __ sll(scratch, length, 2);

  __ bind(&invoke);
  ASSERT(instr->HasPointerMap());
  LPointerMap* pointers = instr->pointer_map();
  SafepointGenerator safepoint_generator(
      this, pointers, Safepoint::kLazyDeopt);
  // The number of arguments is stored in receiver which is a0, as expected
  // by InvokeFunction.
  ParameterCount actual(receiver);
  __ InvokeFunction(function, actual, CALL_FUNCTION,
                    safepoint_generator, CALL_AS_METHOD);
}


void LCodeGen::DoPushArgument(LPushArgument* instr) {
  LOperand* argument = instr->value();
  if (argument->IsDoubleRegister() || argument->IsDoubleStackSlot()) {
    Abort(kDoPushArgumentNotImplementedForDoubleType);
  } else {
    Register argument_reg = EmitLoadRegister(argument, at);
    __ push(argument_reg);
  }
}


void LCodeGen::DoDrop(LDrop* instr) {
  __ Drop(instr->count());
}


void LCodeGen::DoThisFunction(LThisFunction* instr) {
  Register result = ToRegister(instr->result());
  __ lw(result, MemOperand(fp, JavaScriptFrameConstants::kFunctionOffset));
}


void LCodeGen::DoContext(LContext* instr) {
  // If there is a non-return use, the context must be moved to a register.
  Register result = ToRegister(instr->result());
  if (info()->IsOptimizing()) {
    __ lw(result, MemOperand(fp, StandardFrameConstants::kContextOffset));
  } else {
    // If there is no frame, the context must be in cp.
    ASSERT(result.is(cp));
  }
}


void LCodeGen::DoOuterContext(LOuterContext* instr) {
  Register context = ToRegister(instr->context());
  Register result = ToRegister(instr->result());
  __ lw(result,
        MemOperand(context, Context::SlotOffset(Context::PREVIOUS_INDEX)));
}


void LCodeGen::DoDeclareGlobals(LDeclareGlobals* instr) {
  ASSERT(ToRegister(instr->context()).is(cp));
  __ li(scratch0(), instr->hydrogen()->pairs());
  __ li(scratch1(), Operand(Smi::FromInt(instr->hydrogen()->flags())));
  // The context is the first argument.
  __ Push(cp, scratch0(), scratch1());
  CallRuntime(Runtime::kDeclareGlobals, 3, instr);
}


void LCodeGen::DoGlobalObject(LGlobalObject* instr) {
  Register context = ToRegister(instr->context());
  Register result = ToRegister(instr->result());
  __ lw(result, ContextOperand(context, Context::GLOBAL_OBJECT_INDEX));
}


void LCodeGen::DoGlobalReceiver(LGlobalReceiver* instr) {
  Register global = ToRegister(instr->global_object());
  Register result = ToRegister(instr->result());
  __ lw(result, FieldMemOperand(global, GlobalObject::kGlobalReceiverOffset));
}


void LCodeGen::CallKnownFunction(Handle<JSFunction> function,
                                 int formal_parameter_count,
                                 int arity,
                                 LInstruction* instr,
                                 CallKind call_kind,
                                 A1State a1_state) {
  bool dont_adapt_arguments =
      formal_parameter_count == SharedFunctionInfo::kDontAdaptArgumentsSentinel;
  bool can_invoke_directly =
      dont_adapt_arguments || formal_parameter_count == arity;

  LPointerMap* pointers = instr->pointer_map();

  if (can_invoke_directly) {
    if (a1_state == A1_UNINITIALIZED) {
      __ li(a1, function);
    }

    // Change context.
    __ lw(cp, FieldMemOperand(a1, JSFunction::kContextOffset));

    // Set r0 to arguments count if adaption is not needed. Assumes that r0
    // is available to write to at this point.
    if (dont_adapt_arguments) {
      __ li(a0, Operand(arity));
    }

    // Invoke function.
    __ SetCallKind(t1, call_kind);
    __ lw(at, FieldMemOperand(a1, JSFunction::kCodeEntryOffset));
    __ Call(at);

    // Set up deoptimization.
    RecordSafepointWithLazyDeopt(instr, RECORD_SIMPLE_SAFEPOINT);
  } else {
    SafepointGenerator generator(this, pointers, Safepoint::kLazyDeopt);
    ParameterCount count(arity);
    ParameterCount expected(formal_parameter_count);
    __ InvokeFunction(
        function, expected, count, CALL_FUNCTION, generator, call_kind);
  }
}


void LCodeGen::DoCallConstantFunction(LCallConstantFunction* instr) {
  ASSERT(ToRegister(instr->result()).is(v0));
  __ mov(a0, v0);
  CallKnownFunction(instr->hydrogen()->function(),
                    instr->hydrogen()->formal_parameter_count(),
                    instr->arity(),
                    instr,
                    CALL_AS_METHOD,
                    A1_UNINITIALIZED);
}


void LCodeGen::DoDeferredMathAbsTaggedHeapNumber(LMathAbs* instr) {
  ASSERT(instr->context() != NULL);
  ASSERT(ToRegister(instr->context()).is(cp));
  Register input = ToRegister(instr->value());
  Register result = ToRegister(instr->result());
  Register scratch = scratch0();

  // Deoptimize if not a heap number.
  __ lw(scratch, FieldMemOperand(input, HeapObject::kMapOffset));
  __ LoadRoot(at, Heap::kHeapNumberMapRootIndex);
  DeoptimizeIf(ne, instr->environment(), scratch, Operand(at));

  Label done;
  Register exponent = scratch0();
  scratch = no_reg;
  __ lw(exponent, FieldMemOperand(input, HeapNumber::kExponentOffset));
  // Check the sign of the argument. If the argument is positive, just
  // return it.
  __ Move(result, input);
  __ And(at, exponent, Operand(HeapNumber::kSignMask));
  __ Branch(&done, eq, at, Operand(zero_reg));

  // Input is negative. Reverse its sign.
  // Preserve the value of all registers.
  {
    PushSafepointRegistersScope scope(this, Safepoint::kWithRegisters);

    // Registers were saved at the safepoint, so we can use
    // many scratch registers.
    Register tmp1 = input.is(a1) ? a0 : a1;
    Register tmp2 = input.is(a2) ? a0 : a2;
    Register tmp3 = input.is(a3) ? a0 : a3;
    Register tmp4 = input.is(t0) ? a0 : t0;

    // exponent: floating point exponent value.

    Label allocated, slow;
    __ LoadRoot(tmp4, Heap::kHeapNumberMapRootIndex);
    __ AllocateHeapNumber(tmp1, tmp2, tmp3, tmp4, &slow);
    __ Branch(&allocated);

    // Slow case: Call the runtime system to do the number allocation.
    __ bind(&slow);

    CallRuntimeFromDeferred(Runtime::kAllocateHeapNumber, 0, instr,
                            instr->context());
    // Set the pointer to the new heap number in tmp.
    if (!tmp1.is(v0))
      __ mov(tmp1, v0);
    // Restore input_reg after call to runtime.
    __ LoadFromSafepointRegisterSlot(input, input);
    __ lw(exponent, FieldMemOperand(input, HeapNumber::kExponentOffset));

    __ bind(&allocated);
    // exponent: floating point exponent value.
    // tmp1: allocated heap number.
    __ And(exponent, exponent, Operand(~HeapNumber::kSignMask));
    __ sw(exponent, FieldMemOperand(tmp1, HeapNumber::kExponentOffset));
    __ lw(tmp2, FieldMemOperand(input, HeapNumber::kMantissaOffset));
    __ sw(tmp2, FieldMemOperand(tmp1, HeapNumber::kMantissaOffset));

    __ StoreToSafepointRegisterSlot(tmp1, result);
  }

  __ bind(&done);
}


void LCodeGen::EmitIntegerMathAbs(LMathAbs* instr) {
  Register input = ToRegister(instr->value());
  Register result = ToRegister(instr->result());
  Assembler::BlockTrampolinePoolScope block_trampoline_pool(masm_);
  Label done;
  __ Branch(USE_DELAY_SLOT, &done, ge, input, Operand(zero_reg));
  __ mov(result, input);
  __ subu(result, zero_reg, input);
  // Overflow if result is still negative, i.e. 0x80000000.
  DeoptimizeIf(lt, instr->environment(), result, Operand(zero_reg));
  __ bind(&done);
}


void LCodeGen::DoMathAbs(LMathAbs* instr) {
  // Class for deferred case.
  class DeferredMathAbsTaggedHeapNumber V8_FINAL : public LDeferredCode {
   public:
    DeferredMathAbsTaggedHeapNumber(LCodeGen* codegen, LMathAbs* instr)
        : LDeferredCode(codegen), instr_(instr) { }
    virtual void Generate() V8_OVERRIDE {
      codegen()->DoDeferredMathAbsTaggedHeapNumber(instr_);
    }
    virtual LInstruction* instr() V8_OVERRIDE { return instr_; }
   private:
    LMathAbs* instr_;
  };

  Representation r = instr->hydrogen()->value()->representation();
  if (r.IsDouble()) {
    FPURegister input = ToDoubleRegister(instr->value());
    FPURegister result = ToDoubleRegister(instr->result());
    __ abs_d(result, input);
  } else if (r.IsSmiOrInteger32()) {
    EmitIntegerMathAbs(instr);
  } else {
    // Representation is tagged.
    DeferredMathAbsTaggedHeapNumber* deferred =
        new(zone()) DeferredMathAbsTaggedHeapNumber(this, instr);
    Register input = ToRegister(instr->value());
    // Smi check.
    __ JumpIfNotSmi(input, deferred->entry());
    // If smi, handle it directly.
    EmitIntegerMathAbs(instr);
    __ bind(deferred->exit());
  }
}


void LCodeGen::DoMathFloor(LMathFloor* instr) {
  DoubleRegister input = ToDoubleRegister(instr->value());
  Register result = ToRegister(instr->result());
  Register scratch1 = scratch0();
  Register except_flag = ToRegister(instr->temp());

  __ EmitFPUTruncate(kRoundToMinusInf,
                     result,
                     input,
                     scratch1,
                     double_scratch0(),
                     except_flag);

  // Deopt if the operation did not succeed.
  DeoptimizeIf(ne, instr->environment(), except_flag, Operand(zero_reg));

  if (instr->hydrogen()->CheckFlag(HValue::kBailoutOnMinusZero)) {
    // Test for -0.
    Label done;
    __ Branch(&done, ne, result, Operand(zero_reg));
    __ mfc1(scratch1, input.high());
    __ And(scratch1, scratch1, Operand(HeapNumber::kSignMask));
    DeoptimizeIf(ne, instr->environment(), scratch1, Operand(zero_reg));
    __ bind(&done);
  }
}


void LCodeGen::DoMathRound(LMathRound* instr) {
  DoubleRegister input = ToDoubleRegister(instr->value());
  Register result = ToRegister(instr->result());
  DoubleRegister double_scratch1 = ToDoubleRegister(instr->temp());
  Register scratch = scratch0();
  Label done, check_sign_on_zero;

  // Extract exponent bits.
  __ mfc1(result, input.high());
  __ Ext(scratch,
         result,
         HeapNumber::kExponentShift,
         HeapNumber::kExponentBits);

  // If the number is in ]-0.5, +0.5[, the result is +/- 0.
  Label skip1;
  __ Branch(&skip1, gt, scratch, Operand(HeapNumber::kExponentBias - 2));
  __ mov(result, zero_reg);
  if (instr->hydrogen()->CheckFlag(HValue::kBailoutOnMinusZero)) {
    __ Branch(&check_sign_on_zero);
  } else {
    __ Branch(&done);
  }
  __ bind(&skip1);

  // The following conversion will not work with numbers
  // outside of ]-2^32, 2^32[.
  DeoptimizeIf(ge, instr->environment(), scratch,
               Operand(HeapNumber::kExponentBias + 32));

  // Save the original sign for later comparison.
  __ And(scratch, result, Operand(HeapNumber::kSignMask));

  __ Move(double_scratch0(), 0.5);
  __ add_d(double_scratch0(), input, double_scratch0());

  // Check sign of the result: if the sign changed, the input
  // value was in ]0.5, 0[ and the result should be -0.
  __ mfc1(result, double_scratch0().high());
  __ Xor(result, result, Operand(scratch));
  if (instr->hydrogen()->CheckFlag(HValue::kBailoutOnMinusZero)) {
    // ARM uses 'mi' here, which is 'lt'
    DeoptimizeIf(lt, instr->environment(), result,
                 Operand(zero_reg));
  } else {
    Label skip2;
    // ARM uses 'mi' here, which is 'lt'
    // Negating it results in 'ge'
    __ Branch(&skip2, ge, result, Operand(zero_reg));
    __ mov(result, zero_reg);
    __ Branch(&done);
    __ bind(&skip2);
  }

  Register except_flag = scratch;
  __ EmitFPUTruncate(kRoundToMinusInf,
                     result,
                     double_scratch0(),
                     at,
                     double_scratch1,
                     except_flag);

  DeoptimizeIf(ne, instr->environment(), except_flag, Operand(zero_reg));

  if (instr->hydrogen()->CheckFlag(HValue::kBailoutOnMinusZero)) {
    // Test for -0.
    __ Branch(&done, ne, result, Operand(zero_reg));
    __ bind(&check_sign_on_zero);
    __ mfc1(scratch, input.high());
    __ And(scratch, scratch, Operand(HeapNumber::kSignMask));
    DeoptimizeIf(ne, instr->environment(), scratch, Operand(zero_reg));
  }
  __ bind(&done);
}


void LCodeGen::DoMathSqrt(LMathSqrt* instr) {
  DoubleRegister input = ToDoubleRegister(instr->value());
  DoubleRegister result = ToDoubleRegister(instr->result());
  __ sqrt_d(result, input);
}


void LCodeGen::DoMathPowHalf(LMathPowHalf* instr) {
  DoubleRegister input = ToDoubleRegister(instr->value());
  DoubleRegister result = ToDoubleRegister(instr->result());
  DoubleRegister temp = ToDoubleRegister(instr->temp());

  ASSERT(!input.is(result));

  // Note that according to ECMA-262 15.8.2.13:
  // Math.pow(-Infinity, 0.5) == Infinity
  // Math.sqrt(-Infinity) == NaN
  Label done;
  __ Move(temp, -V8_INFINITY);
  __ BranchF(USE_DELAY_SLOT, &done, NULL, eq, temp, input);
  // Set up Infinity in the delay slot.
  // result is overwritten if the branch is not taken.
  __ neg_d(result, temp);

  // Add +0 to convert -0 to +0.
  __ add_d(result, input, kDoubleRegZero);
  __ sqrt_d(result, result);
  __ bind(&done);
}


void LCodeGen::DoPower(LPower* instr) {
  Representation exponent_type = instr->hydrogen()->right()->representation();
  // Having marked this as a call, we can use any registers.
  // Just make sure that the input/output registers are the expected ones.
  ASSERT(!instr->right()->IsDoubleRegister() ||
         ToDoubleRegister(instr->right()).is(f4));
  ASSERT(!instr->right()->IsRegister() ||
         ToRegister(instr->right()).is(a2));
  ASSERT(ToDoubleRegister(instr->left()).is(f2));
  ASSERT(ToDoubleRegister(instr->result()).is(f0));

  if (exponent_type.IsSmi()) {
    MathPowStub stub(MathPowStub::TAGGED);
    __ CallStub(&stub);
  } else if (exponent_type.IsTagged()) {
    Label no_deopt;
    __ JumpIfSmi(a2, &no_deopt);
    __ lw(t3, FieldMemOperand(a2, HeapObject::kMapOffset));
    DeoptimizeIf(ne, instr->environment(), t3, Operand(at));
    __ bind(&no_deopt);
    MathPowStub stub(MathPowStub::TAGGED);
    __ CallStub(&stub);
  } else if (exponent_type.IsInteger32()) {
    MathPowStub stub(MathPowStub::INTEGER);
    __ CallStub(&stub);
  } else {
    ASSERT(exponent_type.IsDouble());
    MathPowStub stub(MathPowStub::DOUBLE);
    __ CallStub(&stub);
  }
}


void LCodeGen::DoRandom(LRandom* instr) {
  // Assert that the register size is indeed the size of each seed.
  static const int kSeedSize = sizeof(uint32_t);
  STATIC_ASSERT(kPointerSize == kSeedSize);

  // Load native context.
  Register global_object = ToRegister(instr->global_object());
  Register native_context = global_object;
  __ lw(native_context, FieldMemOperand(
          global_object, GlobalObject::kNativeContextOffset));

  // Load state (FixedArray of the native context's random seeds).
  static const int kRandomSeedOffset =
      FixedArray::kHeaderSize + Context::RANDOM_SEED_INDEX * kPointerSize;
  Register state = native_context;
  __ lw(state, FieldMemOperand(native_context, kRandomSeedOffset));

  // Load state[0].
  Register state0 = ToRegister(instr->scratch());
  __ lw(state0, FieldMemOperand(state, ByteArray::kHeaderSize));
  // Load state[1].
  Register state1 = ToRegister(instr->scratch2());
  __ lw(state1, FieldMemOperand(state, ByteArray::kHeaderSize + kSeedSize));

  // state[0] = 18273 * (state[0] & 0xFFFF) + (state[0] >> 16)
  Register scratch3 = ToRegister(instr->scratch3());
  Register scratch4 = scratch0();
  __ And(scratch3, state0, Operand(0xFFFF));
  __ li(scratch4, Operand(18273));
  __ Mul(scratch3, scratch3, scratch4);
  __ srl(state0, state0, 16);
  __ Addu(state0, scratch3, state0);
  // Save state[0].
  __ sw(state0, FieldMemOperand(state, ByteArray::kHeaderSize));

  // state[1] = 36969 * (state[1] & 0xFFFF) + (state[1] >> 16)
  __ And(scratch3, state1, Operand(0xFFFF));
  __ li(scratch4, Operand(36969));
  __ Mul(scratch3, scratch3, scratch4);
  __ srl(state1, state1, 16),
  __ Addu(state1, scratch3, state1);
  // Save state[1].
  __ sw(state1, FieldMemOperand(state, ByteArray::kHeaderSize + kSeedSize));

  // Random bit pattern = (state[0] << 14) + (state[1] & 0x3FFFF)
  Register random = scratch4;
  __ And(random, state1, Operand(0x3FFFF));
  __ sll(state0, state0, 14);
  __ Addu(random, random, state0);

  // 0x41300000 is the top half of 1.0 x 2^20 as a double.
  __ li(scratch3, Operand(0x41300000));
  // Move 0x41300000xxxxxxxx (x = random bits in v0) to FPU.
  DoubleRegister result = ToDoubleRegister(instr->result());
  __ Move(result, random, scratch3);
  // Move 0x4130000000000000 to FPU.
  DoubleRegister scratch5 = double_scratch0();
  __ Move(scratch5, zero_reg, scratch3);
  __ sub_d(result, result, scratch5);
}


void LCodeGen::DoMathExp(LMathExp* instr) {
  DoubleRegister input = ToDoubleRegister(instr->value());
  DoubleRegister result = ToDoubleRegister(instr->result());
  DoubleRegister double_scratch1 = ToDoubleRegister(instr->double_temp());
  DoubleRegister double_scratch2 = double_scratch0();
  Register temp1 = ToRegister(instr->temp1());
  Register temp2 = ToRegister(instr->temp2());

  MathExpGenerator::EmitMathExp(
      masm(), input, result, double_scratch1, double_scratch2,
      temp1, temp2, scratch0());
}


void LCodeGen::DoMathLog(LMathLog* instr) {
  ASSERT(ToDoubleRegister(instr->result()).is(f4));
  // Set the context register to a GC-safe fake value. Clobbering it is
  // OK because this instruction is marked as a call.
  __ mov(cp, zero_reg);
  TranscendentalCacheStub stub(TranscendentalCache::LOG,
                               TranscendentalCacheStub::UNTAGGED);
  CallCode(stub.GetCode(isolate()), RelocInfo::CODE_TARGET, instr);
}


void LCodeGen::DoMathTan(LMathTan* instr) {
  ASSERT(ToDoubleRegister(instr->result()).is(f4));
  // Set the context register to a GC-safe fake value. Clobbering it is
  // OK because this instruction is marked as a call.
  __ mov(cp, zero_reg);
  TranscendentalCacheStub stub(TranscendentalCache::TAN,
                               TranscendentalCacheStub::UNTAGGED);
  CallCode(stub.GetCode(isolate()), RelocInfo::CODE_TARGET, instr);
}


void LCodeGen::DoMathCos(LMathCos* instr) {
  ASSERT(ToDoubleRegister(instr->result()).is(f4));
  // Set the context register to a GC-safe fake value. Clobbering it is
  // OK because this instruction is marked as a call.
  __ mov(cp, zero_reg);
  TranscendentalCacheStub stub(TranscendentalCache::COS,
                               TranscendentalCacheStub::UNTAGGED);
  CallCode(stub.GetCode(isolate()), RelocInfo::CODE_TARGET, instr);
}


void LCodeGen::DoMathSin(LMathSin* instr) {
  ASSERT(ToDoubleRegister(instr->result()).is(f4));
  // Set the context register to a GC-safe fake value. Clobbering it is
  // OK because this instruction is marked as a call.
  __ mov(cp, zero_reg);
  TranscendentalCacheStub stub(TranscendentalCache::SIN,
                               TranscendentalCacheStub::UNTAGGED);
  CallCode(stub.GetCode(isolate()), RelocInfo::CODE_TARGET, instr);
}


void LCodeGen::DoInvokeFunction(LInvokeFunction* instr) {
  ASSERT(ToRegister(instr->context()).is(cp));
  ASSERT(ToRegister(instr->function()).is(a1));
  ASSERT(instr->HasPointerMap());

  Handle<JSFunction> known_function = instr->hydrogen()->known_function();
  if (known_function.is_null()) {
    LPointerMap* pointers = instr->pointer_map();
    SafepointGenerator generator(this, pointers, Safepoint::kLazyDeopt);
    ParameterCount count(instr->arity());
    __ InvokeFunction(a1, count, CALL_FUNCTION, generator, CALL_AS_METHOD);
  } else {
    CallKnownFunction(known_function,
                      instr->hydrogen()->formal_parameter_count(),
                      instr->arity(),
                      instr,
                      CALL_AS_METHOD,
                      A1_CONTAINS_TARGET);
  }
}


void LCodeGen::DoCallKeyed(LCallKeyed* instr) {
  ASSERT(ToRegister(instr->context()).is(cp));
  ASSERT(ToRegister(instr->result()).is(v0));

  int arity = instr->arity();
  Handle<Code> ic =
      isolate()->stub_cache()->ComputeKeyedCallInitialize(arity);
  CallCode(ic, RelocInfo::CODE_TARGET, instr);
}


void LCodeGen::DoCallNamed(LCallNamed* instr) {
  ASSERT(ToRegister(instr->context()).is(cp));
  ASSERT(ToRegister(instr->result()).is(v0));

  int arity = instr->arity();
  RelocInfo::Mode mode = RelocInfo::CODE_TARGET;
  Handle<Code> ic =
      isolate()->stub_cache()->ComputeCallInitialize(arity, mode);
  __ li(a2, Operand(instr->name()));
  CallCode(ic, mode, instr);
}


void LCodeGen::DoCallFunction(LCallFunction* instr) {
  ASSERT(ToRegister(instr->context()).is(cp));
  ASSERT(ToRegister(instr->function()).is(a1));
  ASSERT(ToRegister(instr->result()).is(v0));

  int arity = instr->arity();
  CallFunctionStub stub(arity, NO_CALL_FUNCTION_FLAGS);
  CallCode(stub.GetCode(isolate()), RelocInfo::CODE_TARGET, instr);
}


void LCodeGen::DoCallGlobal(LCallGlobal* instr) {
  ASSERT(ToRegister(instr->context()).is(cp));
  ASSERT(ToRegister(instr->result()).is(v0));

  int arity = instr->arity();
  RelocInfo::Mode mode = RelocInfo::CODE_TARGET_CONTEXT;
  Handle<Code> ic =
      isolate()->stub_cache()->ComputeCallInitialize(arity, mode);
  __ li(a2, Operand(instr->name()));
  CallCode(ic, mode, instr);
}


void LCodeGen::DoCallKnownGlobal(LCallKnownGlobal* instr) {
  ASSERT(ToRegister(instr->result()).is(v0));
  CallKnownFunction(instr->hydrogen()->target(),
                    instr->hydrogen()->formal_parameter_count(),
                    instr->arity(),
                    instr,
                    CALL_AS_FUNCTION,
                    A1_UNINITIALIZED);
}


void LCodeGen::DoCallNew(LCallNew* instr) {
  ASSERT(ToRegister(instr->context()).is(cp));
  ASSERT(ToRegister(instr->constructor()).is(a1));
  ASSERT(ToRegister(instr->result()).is(v0));

  __ li(a0, Operand(instr->arity()));
  // No cell in a2 for construct type feedback in optimized code
  Handle<Object> undefined_value(isolate()->factory()->undefined_value());
  __ li(a2, Operand(undefined_value));
  CallConstructStub stub(NO_CALL_FUNCTION_FLAGS);
  CallCode(stub.GetCode(isolate()), RelocInfo::CONSTRUCT_CALL, instr);
}


void LCodeGen::DoCallNewArray(LCallNewArray* instr) {
  ASSERT(ToRegister(instr->context()).is(cp));
  ASSERT(ToRegister(instr->constructor()).is(a1));
  ASSERT(ToRegister(instr->result()).is(v0));

  __ li(a0, Operand(instr->arity()));
  __ li(a2, Operand(instr->hydrogen()->property_cell()));
  ElementsKind kind = instr->hydrogen()->elements_kind();
  AllocationSiteOverrideMode override_mode =
      (AllocationSite::GetMode(kind) == TRACK_ALLOCATION_SITE)
          ? DISABLE_ALLOCATION_SITES
          : DONT_OVERRIDE;
  ContextCheckMode context_mode = CONTEXT_CHECK_NOT_REQUIRED;

  if (instr->arity() == 0) {
    ArrayNoArgumentConstructorStub stub(kind, context_mode, override_mode);
    CallCode(stub.GetCode(isolate()), RelocInfo::CONSTRUCT_CALL, instr);
  } else if (instr->arity() == 1) {
    Label done;
    if (IsFastPackedElementsKind(kind)) {
      Label packed_case;
      // We might need a change here,
      // look at the first argument.
      __ lw(t1, MemOperand(sp, 0));
      __ Branch(&packed_case, eq, t1, Operand(zero_reg));

      ElementsKind holey_kind = GetHoleyElementsKind(kind);
      ArraySingleArgumentConstructorStub stub(holey_kind, context_mode,
                                              override_mode);
      CallCode(stub.GetCode(isolate()), RelocInfo::CONSTRUCT_CALL, instr);
      __ jmp(&done);
      __ bind(&packed_case);
    }

    ArraySingleArgumentConstructorStub stub(kind, context_mode, override_mode);
    CallCode(stub.GetCode(isolate()), RelocInfo::CONSTRUCT_CALL, instr);
    __ bind(&done);
  } else {
    ArrayNArgumentsConstructorStub stub(kind, context_mode, override_mode);
    CallCode(stub.GetCode(isolate()), RelocInfo::CONSTRUCT_CALL, instr);
  }
}


void LCodeGen::DoCallRuntime(LCallRuntime* instr) {
  CallRuntime(instr->function(), instr->arity(), instr);
}


void LCodeGen::DoStoreCodeEntry(LStoreCodeEntry* instr) {
  Register function = ToRegister(instr->function());
  Register code_object = ToRegister(instr->code_object());
  __ Addu(code_object, code_object,
          Operand(Code::kHeaderSize - kHeapObjectTag));
  __ sw(code_object,
        FieldMemOperand(function, JSFunction::kCodeEntryOffset));
}


void LCodeGen::DoInnerAllocatedObject(LInnerAllocatedObject* instr) {
  Register result = ToRegister(instr->result());
  Register base = ToRegister(instr->base_object());
  __ Addu(result, base, Operand(instr->offset()));
}


void LCodeGen::DoStoreNamedField(LStoreNamedField* instr) {
  Representation representation = instr->representation();

  Register object = ToRegister(instr->object());
  Register scratch = scratch0();
  HObjectAccess access = instr->hydrogen()->access();
  int offset = access.offset();

  if (access.IsExternalMemory()) {
    Register value = ToRegister(instr->value());
    MemOperand operand = MemOperand(object, offset);
    if (representation.IsByte()) {
      __ sb(value, operand);
    } else {
      __ sw(value, operand);
    }
    return;
  }

  Handle<Map> transition = instr->transition();

  if (FLAG_track_heap_object_fields && representation.IsHeapObject()) {
    Register value = ToRegister(instr->value());
    if (!instr->hydrogen()->value()->type().IsHeapObject()) {
      __ And(scratch, value, Operand(kSmiTagMask));
      DeoptimizeIf(eq, instr->environment(), scratch, Operand(zero_reg));
    }
  } else if (FLAG_track_double_fields && representation.IsDouble()) {
    ASSERT(transition.is_null());
    ASSERT(access.IsInobject());
    ASSERT(!instr->hydrogen()->NeedsWriteBarrier());
    DoubleRegister value = ToDoubleRegister(instr->value());
    __ sdc1(value, FieldMemOperand(object, offset));
    return;
  }

  if (!transition.is_null()) {
    __ li(scratch, Operand(transition));
    __ sw(scratch, FieldMemOperand(object, HeapObject::kMapOffset));
    if (instr->hydrogen()->NeedsWriteBarrierForMap()) {
      Register temp = ToRegister(instr->temp());
      // Update the write barrier for the map field.
      __ RecordWriteField(object,
                          HeapObject::kMapOffset,
                          scratch,
                          temp,
                          GetRAState(),
                          kSaveFPRegs,
                          OMIT_REMEMBERED_SET,
                          OMIT_SMI_CHECK);
    }
  }

  // Do the store.
  Register value = ToRegister(instr->value());
  ASSERT(!object.is(value));
  SmiCheck check_needed =
      instr->hydrogen()->value()->IsHeapObject()
          ? OMIT_SMI_CHECK : INLINE_SMI_CHECK;
  if (access.IsInobject()) {
    MemOperand operand = FieldMemOperand(object, offset);
    if (representation.IsByte()) {
      __ sb(value, operand);
    } else {
      __ sw(value, operand);
    }
    if (instr->hydrogen()->NeedsWriteBarrier()) {
      // Update the write barrier for the object for in-object properties.
      __ RecordWriteField(object,
                          offset,
                          value,
                          scratch,
                          GetRAState(),
                          kSaveFPRegs,
                          EMIT_REMEMBERED_SET,
                          check_needed);
    }
  } else {
    __ lw(scratch, FieldMemOperand(object, JSObject::kPropertiesOffset));
    MemOperand operand = FieldMemOperand(scratch, offset);
    if (representation.IsByte()) {
      __ sb(value, operand);
    } else {
      __ sw(value, operand);
    }
    if (instr->hydrogen()->NeedsWriteBarrier()) {
      // Update the write barrier for the properties array.
      // object is used as a scratch register.
      __ RecordWriteField(scratch,
                          offset,
                          value,
                          object,
                          GetRAState(),
                          kSaveFPRegs,
                          EMIT_REMEMBERED_SET,
                          check_needed);
    }
  }
}


void LCodeGen::DoStoreNamedGeneric(LStoreNamedGeneric* instr) {
  ASSERT(ToRegister(instr->context()).is(cp));
  ASSERT(ToRegister(instr->object()).is(a1));
  ASSERT(ToRegister(instr->value()).is(a0));

  // Name is always in a2.
  __ li(a2, Operand(instr->name()));
  Handle<Code> ic = (instr->strict_mode_flag() == kStrictMode)
      ? isolate()->builtins()->StoreIC_Initialize_Strict()
      : isolate()->builtins()->StoreIC_Initialize();
  CallCode(ic, RelocInfo::CODE_TARGET, instr);
}


void LCodeGen::ApplyCheckIf(Condition condition,
                            LBoundsCheck* check,
                            Register src1,
                            const Operand& src2) {
  if (FLAG_debug_code && check->hydrogen()->skip_check()) {
    Label done;
    __ Branch(&done, NegateCondition(condition), src1, src2);
    __ stop("eliminated bounds check failed");
    __ bind(&done);
  } else {
    DeoptimizeIf(condition, check->environment(), src1, src2);
  }
}


void LCodeGen::DoBoundsCheck(LBoundsCheck* instr) {
  if (instr->hydrogen()->skip_check()) return;

  Condition condition = instr->hydrogen()->allow_equality() ? hi : hs;
  if (instr->index()->IsConstantOperand()) {
    int constant_index =
        ToInteger32(LConstantOperand::cast(instr->index()));
    if (instr->hydrogen()->length()->representation().IsSmi()) {
      __ li(at, Operand(Smi::FromInt(constant_index)));
    } else {
      __ li(at, Operand(constant_index));
    }
    ApplyCheckIf(condition,
                 instr,
                 at,
                 Operand(ToRegister(instr->length())));
  } else {
    ApplyCheckIf(condition,
                 instr,
                 ToRegister(instr->index()),
                 Operand(ToRegister(instr->length())));
  }
}


void LCodeGen::DoStoreKeyedExternalArray(LStoreKeyed* instr) {
  Register external_pointer = ToRegister(instr->elements());
  Register key = no_reg;
  ElementsKind elements_kind = instr->elements_kind();
  bool key_is_constant = instr->key()->IsConstantOperand();
  int constant_key = 0;
  if (key_is_constant) {
    constant_key = ToInteger32(LConstantOperand::cast(instr->key()));
    if (constant_key & 0xF0000000) {
      Abort(kArrayIndexConstantValueTooBig);
    }
  } else {
    key = ToRegister(instr->key());
  }
  int element_size_shift = ElementsKindToShiftSize(elements_kind);
  int shift_size = (instr->hydrogen()->key()->representation().IsSmi())
      ? (element_size_shift - kSmiTagSize) : element_size_shift;
  int additional_offset = instr->additional_index() << element_size_shift;

  if (elements_kind == EXTERNAL_FLOAT_ELEMENTS ||
      elements_kind == EXTERNAL_DOUBLE_ELEMENTS) {
    Register address = scratch0();
    FPURegister value(ToDoubleRegister(instr->value()));
    if (key_is_constant) {
      if (constant_key != 0) {
        __ Addu(address, external_pointer,
                Operand(constant_key << element_size_shift));
      } else {
        address = external_pointer;
      }
    } else {
      __ sll(address, key, shift_size);
      __ Addu(address, external_pointer, address);
    }

    if (elements_kind == EXTERNAL_FLOAT_ELEMENTS) {
      __ cvt_s_d(double_scratch0(), value);
      __ swc1(double_scratch0(), MemOperand(address, additional_offset));
    } else {  // i.e. elements_kind == EXTERNAL_DOUBLE_ELEMENTS
      __ sdc1(value, MemOperand(address, additional_offset));
    }
  } else {
    Register value(ToRegister(instr->value()));
    MemOperand mem_operand = PrepareKeyedOperand(
        key, external_pointer, key_is_constant, constant_key,
        element_size_shift, shift_size,
        instr->additional_index(), additional_offset);
    switch (elements_kind) {
      case EXTERNAL_PIXEL_ELEMENTS:
      case EXTERNAL_BYTE_ELEMENTS:
      case EXTERNAL_UNSIGNED_BYTE_ELEMENTS:
        __ sb(value, mem_operand);
        break;
      case EXTERNAL_SHORT_ELEMENTS:
      case EXTERNAL_UNSIGNED_SHORT_ELEMENTS:
        __ sh(value, mem_operand);
        break;
      case EXTERNAL_INT_ELEMENTS:
      case EXTERNAL_UNSIGNED_INT_ELEMENTS:
        __ sw(value, mem_operand);
        break;
      case EXTERNAL_FLOAT_ELEMENTS:
      case EXTERNAL_DOUBLE_ELEMENTS:
      case FAST_DOUBLE_ELEMENTS:
      case FAST_ELEMENTS:
      case FAST_SMI_ELEMENTS:
      case FAST_HOLEY_DOUBLE_ELEMENTS:
      case FAST_HOLEY_ELEMENTS:
      case FAST_HOLEY_SMI_ELEMENTS:
      case DICTIONARY_ELEMENTS:
      case NON_STRICT_ARGUMENTS_ELEMENTS:
        UNREACHABLE();
        break;
    }
  }
}


void LCodeGen::DoStoreKeyedFixedDoubleArray(LStoreKeyed* instr) {
  DoubleRegister value = ToDoubleRegister(instr->value());
  Register elements = ToRegister(instr->elements());
  Register scratch = scratch0();
  DoubleRegister double_scratch = double_scratch0();
  bool key_is_constant = instr->key()->IsConstantOperand();
  Label not_nan, done;

  // Calculate the effective address of the slot in the array to store the
  // double value.
  int element_size_shift = ElementsKindToShiftSize(FAST_DOUBLE_ELEMENTS);
  if (key_is_constant) {
    int constant_key = ToInteger32(LConstantOperand::cast(instr->key()));
    if (constant_key & 0xF0000000) {
      Abort(kArrayIndexConstantValueTooBig);
    }
    __ Addu(scratch, elements,
            Operand((constant_key << element_size_shift) +
                    FixedDoubleArray::kHeaderSize - kHeapObjectTag));
  } else {
    int shift_size = (instr->hydrogen()->key()->representation().IsSmi())
        ? (element_size_shift - kSmiTagSize) : element_size_shift;
    __ Addu(scratch, elements,
            Operand(FixedDoubleArray::kHeaderSize - kHeapObjectTag));
    __ sll(at, ToRegister(instr->key()), shift_size);
    __ Addu(scratch, scratch, at);
  }

  if (instr->NeedsCanonicalization()) {
    Label is_nan;
    // Check for NaN. All NaNs must be canonicalized.
    __ BranchF(NULL, &is_nan, eq, value, value);
    __ Branch(&not_nan);

    // Only load canonical NaN if the comparison above set the overflow.
    __ bind(&is_nan);
    __ Move(double_scratch,
            FixedDoubleArray::canonical_not_the_hole_nan_as_double());
    __ sdc1(double_scratch, MemOperand(scratch, instr->additional_index() <<
        element_size_shift));
    __ Branch(&done);
  }

  __ bind(&not_nan);
  __ sdc1(value, MemOperand(scratch, instr->additional_index() <<
      element_size_shift));
  __ bind(&done);
}


void LCodeGen::DoStoreKeyedFixedArray(LStoreKeyed* instr) {
  Register value = ToRegister(instr->value());
  Register elements = ToRegister(instr->elements());
  Register key = instr->key()->IsRegister() ? ToRegister(instr->key())
      : no_reg;
  Register scratch = scratch0();
  Register store_base = scratch;
  int offset = 0;

  // Do the store.
  if (instr->key()->IsConstantOperand()) {
    ASSERT(!instr->hydrogen()->NeedsWriteBarrier());
    LConstantOperand* const_operand = LConstantOperand::cast(instr->key());
    offset = FixedArray::OffsetOfElementAt(ToInteger32(const_operand) +
                                           instr->additional_index());
    store_base = elements;
  } else {
    // Even though the HLoadKeyed instruction forces the input
    // representation for the key to be an integer, the input gets replaced
    // during bound check elimination with the index argument to the bounds
    // check, which can be tagged, so that case must be handled here, too.
    if (instr->hydrogen()->key()->representation().IsSmi()) {
      __ sll(scratch, key, kPointerSizeLog2 - kSmiTagSize);
      __ addu(scratch, elements, scratch);
    } else {
      __ sll(scratch, key, kPointerSizeLog2);
      __ addu(scratch, elements, scratch);
    }
    offset = FixedArray::OffsetOfElementAt(instr->additional_index());
  }
  __ sw(value, FieldMemOperand(store_base, offset));

  if (instr->hydrogen()->NeedsWriteBarrier()) {
    SmiCheck check_needed =
        instr->hydrogen()->value()->IsHeapObject()
            ? OMIT_SMI_CHECK : INLINE_SMI_CHECK;
    // Compute address of modified element and store it into key register.
    __ Addu(key, store_base, Operand(offset - kHeapObjectTag));
    __ RecordWrite(elements,
                   key,
                   value,
                   GetRAState(),
                   kSaveFPRegs,
                   EMIT_REMEMBERED_SET,
                   check_needed);
  }
}


void LCodeGen::DoStoreKeyed(LStoreKeyed* instr) {
  // By cases: external, fast double
  if (instr->is_external()) {
    DoStoreKeyedExternalArray(instr);
  } else if (instr->hydrogen()->value()->representation().IsDouble()) {
    DoStoreKeyedFixedDoubleArray(instr);
  } else {
    DoStoreKeyedFixedArray(instr);
  }
}


void LCodeGen::DoStoreKeyedGeneric(LStoreKeyedGeneric* instr) {
  ASSERT(ToRegister(instr->context()).is(cp));
  ASSERT(ToRegister(instr->object()).is(a2));
  ASSERT(ToRegister(instr->key()).is(a1));
  ASSERT(ToRegister(instr->value()).is(a0));

  Handle<Code> ic = (instr->strict_mode_flag() == kStrictMode)
      ? isolate()->builtins()->KeyedStoreIC_Initialize_Strict()
      : isolate()->builtins()->KeyedStoreIC_Initialize();
  CallCode(ic, RelocInfo::CODE_TARGET, instr);
}


void LCodeGen::DoTransitionElementsKind(LTransitionElementsKind* instr) {
  Register object_reg = ToRegister(instr->object());
  Register scratch = scratch0();

  Handle<Map> from_map = instr->original_map();
  Handle<Map> to_map = instr->transitioned_map();
  ElementsKind from_kind = instr->from_kind();
  ElementsKind to_kind = instr->to_kind();

  Label not_applicable;
  __ lw(scratch, FieldMemOperand(object_reg, HeapObject::kMapOffset));
  __ Branch(&not_applicable, ne, scratch, Operand(from_map));

  if (IsSimpleMapChangeTransition(from_kind, to_kind)) {
    Register new_map_reg = ToRegister(instr->new_map_temp());
    __ li(new_map_reg, Operand(to_map));
    __ sw(new_map_reg, FieldMemOperand(object_reg, HeapObject::kMapOffset));
    // Write barrier.
    __ RecordWriteField(object_reg, HeapObject::kMapOffset, new_map_reg,
                        scratch, GetRAState(), kDontSaveFPRegs);
  } else {
    ASSERT(ToRegister(instr->context()).is(cp));
    PushSafepointRegistersScope scope(
        this, Safepoint::kWithRegistersAndDoubles);
    __ mov(a0, object_reg);
    __ li(a1, Operand(to_map));
    TransitionElementsKindStub stub(from_kind, to_kind);
    __ CallStub(&stub);
    RecordSafepointWithRegistersAndDoubles(
        instr->pointer_map(), 0, Safepoint::kNoLazyDeopt);
  }
  __ bind(&not_applicable);
}


void LCodeGen::DoTrapAllocationMemento(LTrapAllocationMemento* instr) {
  Register object = ToRegister(instr->object());
  Register temp = ToRegister(instr->temp());
  Label no_memento_found;
  __ TestJSArrayForAllocationMemento(object, temp, &no_memento_found,
                                     ne, &no_memento_found);
  DeoptimizeIf(al, instr->environment());
  __ bind(&no_memento_found);
}


void LCodeGen::DoStringAdd(LStringAdd* instr) {
  ASSERT(ToRegister(instr->context()).is(cp));
  __ push(ToRegister(instr->left()));
  __ push(ToRegister(instr->right()));
  StringAddStub stub(instr->hydrogen()->flags());
  CallCode(stub.GetCode(isolate()), RelocInfo::CODE_TARGET, instr);
}


void LCodeGen::DoStringCharCodeAt(LStringCharCodeAt* instr) {
  class DeferredStringCharCodeAt V8_FINAL : public LDeferredCode {
   public:
    DeferredStringCharCodeAt(LCodeGen* codegen, LStringCharCodeAt* instr)
        : LDeferredCode(codegen), instr_(instr) { }
    virtual void Generate() V8_OVERRIDE {
      codegen()->DoDeferredStringCharCodeAt(instr_);
    }
    virtual LInstruction* instr() V8_OVERRIDE { return instr_; }
   private:
    LStringCharCodeAt* instr_;
  };

  DeferredStringCharCodeAt* deferred =
      new(zone()) DeferredStringCharCodeAt(this, instr);
  StringCharLoadGenerator::Generate(masm(),
                                    ToRegister(instr->string()),
                                    ToRegister(instr->index()),
                                    ToRegister(instr->result()),
                                    deferred->entry());
  __ bind(deferred->exit());
}


void LCodeGen::DoDeferredStringCharCodeAt(LStringCharCodeAt* instr) {
  Register string = ToRegister(instr->string());
  Register result = ToRegister(instr->result());
  Register scratch = scratch0();

  // TODO(3095996): Get rid of this. For now, we need to make the
  // result register contain a valid pointer because it is already
  // contained in the register pointer map.
  __ mov(result, zero_reg);

  PushSafepointRegistersScope scope(this, Safepoint::kWithRegisters);
  __ push(string);
  // Push the index as a smi. This is safe because of the checks in
  // DoStringCharCodeAt above.
  if (instr->index()->IsConstantOperand()) {
    int const_index = ToInteger32(LConstantOperand::cast(instr->index()));
    __ Addu(scratch, zero_reg, Operand(Smi::FromInt(const_index)));
    __ push(scratch);
  } else {
    Register index = ToRegister(instr->index());
    __ SmiTag(index);
    __ push(index);
  }
  CallRuntimeFromDeferred(Runtime::kStringCharCodeAt, 2, instr,
                          instr->context());
  __ AssertSmi(v0);
  __ SmiUntag(v0);
  __ StoreToSafepointRegisterSlot(v0, result);
}


void LCodeGen::DoStringCharFromCode(LStringCharFromCode* instr) {
  class DeferredStringCharFromCode V8_FINAL : public LDeferredCode {
   public:
    DeferredStringCharFromCode(LCodeGen* codegen, LStringCharFromCode* instr)
        : LDeferredCode(codegen), instr_(instr) { }
    virtual void Generate() V8_OVERRIDE {
      codegen()->DoDeferredStringCharFromCode(instr_);
    }
    virtual LInstruction* instr() V8_OVERRIDE { return instr_; }
   private:
    LStringCharFromCode* instr_;
  };

  DeferredStringCharFromCode* deferred =
      new(zone()) DeferredStringCharFromCode(this, instr);

  ASSERT(instr->hydrogen()->value()->representation().IsInteger32());
  Register char_code = ToRegister(instr->char_code());
  Register result = ToRegister(instr->result());
  Register scratch = scratch0();
  ASSERT(!char_code.is(result));

  __ Branch(deferred->entry(), hi,
            char_code, Operand(String::kMaxOneByteCharCode));
  __ LoadRoot(result, Heap::kSingleCharacterStringCacheRootIndex);
  __ sll(scratch, char_code, kPointerSizeLog2);
  __ Addu(result, result, scratch);
  __ lw(result, FieldMemOperand(result, FixedArray::kHeaderSize));
  __ LoadRoot(scratch, Heap::kUndefinedValueRootIndex);
  __ Branch(deferred->entry(), eq, result, Operand(scratch));
  __ bind(deferred->exit());
}


void LCodeGen::DoDeferredStringCharFromCode(LStringCharFromCode* instr) {
  Register char_code = ToRegister(instr->char_code());
  Register result = ToRegister(instr->result());

  // TODO(3095996): Get rid of this. For now, we need to make the
  // result register contain a valid pointer because it is already
  // contained in the register pointer map.
  __ mov(result, zero_reg);

  PushSafepointRegistersScope scope(this, Safepoint::kWithRegisters);
  __ SmiTag(char_code);
  __ push(char_code);
  CallRuntimeFromDeferred(Runtime::kCharFromCode, 1, instr, instr->context());
  __ StoreToSafepointRegisterSlot(v0, result);
}


void LCodeGen::DoInteger32ToDouble(LInteger32ToDouble* instr) {
  LOperand* input = instr->value();
  ASSERT(input->IsRegister() || input->IsStackSlot());
  LOperand* output = instr->result();
  ASSERT(output->IsDoubleRegister());
  FPURegister single_scratch = double_scratch0().low();
  if (input->IsStackSlot()) {
    Register scratch = scratch0();
    __ lw(scratch, ToMemOperand(input));
    __ mtc1(scratch, single_scratch);
  } else {
    __ mtc1(ToRegister(input), single_scratch);
  }
  __ cvt_d_w(ToDoubleRegister(output), single_scratch);
}


void LCodeGen::DoInteger32ToSmi(LInteger32ToSmi* instr) {
  LOperand* input = instr->value();
  LOperand* output = instr->result();
  Register scratch = scratch0();

  __ SmiTagCheckOverflow(ToRegister(output), ToRegister(input), scratch);
  if (!instr->hydrogen()->value()->HasRange() ||
      !instr->hydrogen()->value()->range()->IsInSmiRange()) {
    DeoptimizeIf(lt, instr->environment(), scratch, Operand(zero_reg));
  }
}


void LCodeGen::DoUint32ToDouble(LUint32ToDouble* instr) {
  LOperand* input = instr->value();
  LOperand* output = instr->result();

  FPURegister dbl_scratch = double_scratch0();
  __ mtc1(ToRegister(input), dbl_scratch);
  __ Cvt_d_uw(ToDoubleRegister(output), dbl_scratch, f22);
}


void LCodeGen::DoUint32ToSmi(LUint32ToSmi* instr) {
  LOperand* input = instr->value();
  LOperand* output = instr->result();
  if (!instr->hydrogen()->value()->HasRange() ||
      !instr->hydrogen()->value()->range()->IsInSmiRange()) {
    Register scratch = scratch0();
    __ And(scratch, ToRegister(input), Operand(0xc0000000));
    DeoptimizeIf(ne, instr->environment(), scratch, Operand(zero_reg));
  }
  __ SmiTag(ToRegister(output), ToRegister(input));
}


void LCodeGen::DoNumberTagI(LNumberTagI* instr) {
  class DeferredNumberTagI V8_FINAL : public LDeferredCode {
   public:
    DeferredNumberTagI(LCodeGen* codegen, LNumberTagI* instr)
        : LDeferredCode(codegen), instr_(instr) { }
    virtual void Generate() V8_OVERRIDE {
      codegen()->DoDeferredNumberTagI(instr_,
                                      instr_->value(),
                                      SIGNED_INT32);
    }
    virtual LInstruction* instr() V8_OVERRIDE { return instr_; }
   private:
    LNumberTagI* instr_;
  };

  Register src = ToRegister(instr->value());
  Register dst = ToRegister(instr->result());
  Register overflow = scratch0();

  DeferredNumberTagI* deferred = new(zone()) DeferredNumberTagI(this, instr);
  __ SmiTagCheckOverflow(dst, src, overflow);
  __ BranchOnOverflow(deferred->entry(), overflow);
  __ bind(deferred->exit());
}


void LCodeGen::DoNumberTagU(LNumberTagU* instr) {
  class DeferredNumberTagU V8_FINAL : public LDeferredCode {
   public:
    DeferredNumberTagU(LCodeGen* codegen, LNumberTagU* instr)
        : LDeferredCode(codegen), instr_(instr) { }
    virtual void Generate() V8_OVERRIDE {
      codegen()->DoDeferredNumberTagI(instr_,
                                      instr_->value(),
                                      UNSIGNED_INT32);
    }
    virtual LInstruction* instr() V8_OVERRIDE { return instr_; }
   private:
    LNumberTagU* instr_;
  };

  LOperand* input = instr->value();
  ASSERT(input->IsRegister() && input->Equals(instr->result()));
  Register reg = ToRegister(input);

  DeferredNumberTagU* deferred = new(zone()) DeferredNumberTagU(this, instr);
  __ Branch(deferred->entry(), hi, reg, Operand(Smi::kMaxValue));
  __ SmiTag(reg, reg);
  __ bind(deferred->exit());
}


void LCodeGen::DoDeferredNumberTagI(LInstruction* instr,
                                    LOperand* value,
                                    IntegerSignedness signedness) {
  Label slow;
  Register src = ToRegister(value);
  Register dst = ToRegister(instr->result());
  DoubleRegister dbl_scratch = double_scratch0();

  // Preserve the value of all registers.
  PushSafepointRegistersScope scope(this, Safepoint::kWithRegisters);

  Label done;
  if (signedness == SIGNED_INT32) {
    // There was overflow, so bits 30 and 31 of the original integer
    // disagree. Try to allocate a heap number in new space and store
    // the value in there. If that fails, call the runtime system.
    if (dst.is(src)) {
      __ SmiUntag(src, dst);
      __ Xor(src, src, Operand(0x80000000));
    }
    __ mtc1(src, dbl_scratch);
    __ cvt_d_w(dbl_scratch, dbl_scratch);
  } else {
    __ mtc1(src, dbl_scratch);
    __ Cvt_d_uw(dbl_scratch, dbl_scratch, f22);
  }

  if (FLAG_inline_new) {
    __ LoadRoot(scratch0(), Heap::kHeapNumberMapRootIndex);
    __ AllocateHeapNumber(t1, a3, t0, scratch0(), &slow, DONT_TAG_RESULT);
    __ Move(dst, t1);
    __ Branch(&done);
  }

  // Slow case: Call the runtime system to do the number allocation.
  __ bind(&slow);

  // TODO(3095996): Put a valid pointer value in the stack slot where the result
  // register is stored, as this register is in the pointer map, but contains an
  // integer value.
  __ StoreToSafepointRegisterSlot(zero_reg, dst);
  // NumberTagI and NumberTagD use the context from the frame, rather than
  // the environment's HContext or HInlinedContext value.
  // They only call Runtime::kAllocateHeapNumber.
  // The corresponding HChange instructions are added in a phase that does
  // not have easy access to the local context.
  __ lw(cp, MemOperand(fp, StandardFrameConstants::kContextOffset));
  __ CallRuntimeSaveDoubles(Runtime::kAllocateHeapNumber);
  RecordSafepointWithRegisters(
      instr->pointer_map(), 0, Safepoint::kNoLazyDeopt);
  __ Move(dst, v0);
  __ Subu(dst, dst, kHeapObjectTag);

  // Done. Put the value in dbl_scratch into the value of the allocated heap
  // number.
  __ bind(&done);
  __ sdc1(dbl_scratch, MemOperand(dst, HeapNumber::kValueOffset));
  __ Addu(dst, dst, kHeapObjectTag);
  __ StoreToSafepointRegisterSlot(dst, dst);
}


void LCodeGen::DoNumberTagD(LNumberTagD* instr) {
  class DeferredNumberTagD V8_FINAL : public LDeferredCode {
   public:
    DeferredNumberTagD(LCodeGen* codegen, LNumberTagD* instr)
        : LDeferredCode(codegen), instr_(instr) { }
    virtual void Generate() V8_OVERRIDE {
      codegen()->DoDeferredNumberTagD(instr_);
    }
    virtual LInstruction* instr() V8_OVERRIDE { return instr_; }
   private:
    LNumberTagD* instr_;
  };

  DoubleRegister input_reg = ToDoubleRegister(instr->value());
  Register scratch = scratch0();
  Register reg = ToRegister(instr->result());
  Register temp1 = ToRegister(instr->temp());
  Register temp2 = ToRegister(instr->temp2());

  DeferredNumberTagD* deferred = new(zone()) DeferredNumberTagD(this, instr);
  if (FLAG_inline_new) {
    __ LoadRoot(scratch, Heap::kHeapNumberMapRootIndex);
    // We want the untagged address first for performance
    __ AllocateHeapNumber(reg, temp1, temp2, scratch, deferred->entry(),
                          DONT_TAG_RESULT);
  } else {
    __ Branch(deferred->entry());
  }
  __ bind(deferred->exit());
  __ sdc1(input_reg, MemOperand(reg, HeapNumber::kValueOffset));
  // Now that we have finished with the object's real address tag it
  __ Addu(reg, reg, kHeapObjectTag);
}


void LCodeGen::DoDeferredNumberTagD(LNumberTagD* instr) {
  // TODO(3095996): Get rid of this. For now, we need to make the
  // result register contain a valid pointer because it is already
  // contained in the register pointer map.
  Register reg = ToRegister(instr->result());
  __ mov(reg, zero_reg);

  PushSafepointRegistersScope scope(this, Safepoint::kWithRegisters);
  // NumberTagI and NumberTagD use the context from the frame, rather than
  // the environment's HContext or HInlinedContext value.
  // They only call Runtime::kAllocateHeapNumber.
  // The corresponding HChange instructions are added in a phase that does
  // not have easy access to the local context.
  __ lw(cp, MemOperand(fp, StandardFrameConstants::kContextOffset));
  __ CallRuntimeSaveDoubles(Runtime::kAllocateHeapNumber);
  RecordSafepointWithRegisters(
      instr->pointer_map(), 0, Safepoint::kNoLazyDeopt);
  __ Subu(v0, v0, kHeapObjectTag);
  __ StoreToSafepointRegisterSlot(v0, reg);
}


void LCodeGen::DoSmiTag(LSmiTag* instr) {
  ASSERT(!instr->hydrogen_value()->CheckFlag(HValue::kCanOverflow));
  __ SmiTag(ToRegister(instr->result()), ToRegister(instr->value()));
}


void LCodeGen::DoSmiUntag(LSmiUntag* instr) {
  Register scratch = scratch0();
  Register input = ToRegister(instr->value());
  Register result = ToRegister(instr->result());
  if (instr->needs_check()) {
    STATIC_ASSERT(kHeapObjectTag == 1);
    // If the input is a HeapObject, value of scratch won't be zero.
    __ And(scratch, input, Operand(kHeapObjectTag));
    __ SmiUntag(result, input);
    DeoptimizeIf(ne, instr->environment(), scratch, Operand(zero_reg));
  } else {
    __ SmiUntag(result, input);
  }
}


void LCodeGen::EmitNumberUntagD(Register input_reg,
                                DoubleRegister result_reg,
                                bool can_convert_undefined_to_nan,
                                bool deoptimize_on_minus_zero,
                                LEnvironment* env,
                                NumberUntagDMode mode) {
  Register scratch = scratch0();
  Label convert, load_smi, done;
  if (mode == NUMBER_CANDIDATE_IS_ANY_TAGGED) {
    // Smi check.
    __ UntagAndJumpIfSmi(scratch, input_reg, &load_smi);
    // Heap number map check.
    __ lw(scratch, FieldMemOperand(input_reg, HeapObject::kMapOffset));
    __ LoadRoot(at, Heap::kHeapNumberMapRootIndex);
    if (can_convert_undefined_to_nan) {
      __ Branch(&convert, ne, scratch, Operand(at));
    } else {
      DeoptimizeIf(ne, env, scratch, Operand(at));
    }
    // Load heap number.
    __ ldc1(result_reg, FieldMemOperand(input_reg, HeapNumber::kValueOffset));
    if (deoptimize_on_minus_zero) {
      __ mfc1(at, result_reg.low());
      __ Branch(&done, ne, at, Operand(zero_reg));
      __ mfc1(scratch, result_reg.high());
      DeoptimizeIf(eq, env, scratch, Operand(HeapNumber::kSignMask));
    }
    __ Branch(&done);
    if (can_convert_undefined_to_nan) {
      __ bind(&convert);
      // Convert undefined (and hole) to NaN.
      __ LoadRoot(at, Heap::kUndefinedValueRootIndex);
      DeoptimizeIf(ne, env, input_reg, Operand(at));
      __ LoadRoot(scratch, Heap::kNanValueRootIndex);
      __ ldc1(result_reg, FieldMemOperand(scratch, HeapNumber::kValueOffset));
      __ Branch(&done);
    }
  } else {
    __ SmiUntag(scratch, input_reg);
    ASSERT(mode == NUMBER_CANDIDATE_IS_SMI);
  }
  // Smi to double register conversion
  __ bind(&load_smi);
  // scratch: untagged value of input_reg
  __ mtc1(scratch, result_reg);
  __ cvt_d_w(result_reg, result_reg);
  __ bind(&done);
}


void LCodeGen::DoDeferredTaggedToI(LTaggedToI* instr) {
  Register input_reg = ToRegister(instr->value());
  Register scratch1 = scratch0();
  Register scratch2 = ToRegister(instr->temp());
  DoubleRegister double_scratch = double_scratch0();
  DoubleRegister double_scratch2 = ToDoubleRegister(instr->temp2());

  ASSERT(!scratch1.is(input_reg) && !scratch1.is(scratch2));
  ASSERT(!scratch2.is(input_reg) && !scratch2.is(scratch1));

  Label done;

  // The input is a tagged HeapObject.
  // Heap number map check.
  __ lw(scratch1, FieldMemOperand(input_reg, HeapObject::kMapOffset));
  __ LoadRoot(at, Heap::kHeapNumberMapRootIndex);
  // This 'at' value and scratch1 map value are used for tests in both clauses
  // of the if.

  if (instr->truncating()) {
    // Performs a truncating conversion of a floating point number as used by
    // the JS bitwise operations.
    Label no_heap_number, check_bools, check_false;
    __ Branch(&no_heap_number, ne, scratch1, Operand(at));  // HeapNumber map?
    __ mov(scratch2, input_reg);
    __ TruncateHeapNumberToI(input_reg, scratch2);
    __ Branch(&done);

    // Check for Oddballs. Undefined/False is converted to zero and True to one
    // for truncating conversions.
    __ bind(&no_heap_number);
    __ LoadRoot(at, Heap::kUndefinedValueRootIndex);
    __ Branch(&check_bools, ne, input_reg, Operand(at));
    ASSERT(ToRegister(instr->result()).is(input_reg));
    __ Branch(USE_DELAY_SLOT, &done);
    __ mov(input_reg, zero_reg);  // In delay slot.

    __ bind(&check_bools);
    __ LoadRoot(at, Heap::kTrueValueRootIndex);
    __ Branch(&check_false, ne, scratch2, Operand(at));
    __ Branch(USE_DELAY_SLOT, &done);
    __ li(input_reg, Operand(1));  // In delay slot.

    __ bind(&check_false);
    __ LoadRoot(at, Heap::kFalseValueRootIndex);
    DeoptimizeIf(ne, instr->environment(), scratch2, Operand(at));
    __ Branch(USE_DELAY_SLOT, &done);
    __ mov(input_reg, zero_reg);  // In delay slot.
  } else {
    // Deoptimize if we don't have a heap number.
    DeoptimizeIf(ne, instr->environment(), scratch1, Operand(at));

    // Load the double value.
    __ ldc1(double_scratch,
            FieldMemOperand(input_reg, HeapNumber::kValueOffset));

    Register except_flag = scratch2;
    __ EmitFPUTruncate(kRoundToZero,
                       input_reg,
                       double_scratch,
                       scratch1,
                       double_scratch2,
                       except_flag,
                       kCheckForInexactConversion);

    // Deopt if the operation did not succeed.
    DeoptimizeIf(ne, instr->environment(), except_flag, Operand(zero_reg));

    if (instr->hydrogen()->CheckFlag(HValue::kBailoutOnMinusZero)) {
      __ Branch(&done, ne, input_reg, Operand(zero_reg));

      __ mfc1(scratch1, double_scratch.high());
      __ And(scratch1, scratch1, Operand(HeapNumber::kSignMask));
      DeoptimizeIf(ne, instr->environment(), scratch1, Operand(zero_reg));
    }
  }
  __ bind(&done);
}


void LCodeGen::DoTaggedToI(LTaggedToI* instr) {
  class DeferredTaggedToI V8_FINAL : public LDeferredCode {
   public:
    DeferredTaggedToI(LCodeGen* codegen, LTaggedToI* instr)
        : LDeferredCode(codegen), instr_(instr) { }
    virtual void Generate() V8_OVERRIDE {
      codegen()->DoDeferredTaggedToI(instr_);
    }
    virtual LInstruction* instr() V8_OVERRIDE { return instr_; }
   private:
    LTaggedToI* instr_;
  };

  LOperand* input = instr->value();
  ASSERT(input->IsRegister());
  ASSERT(input->Equals(instr->result()));

  Register input_reg = ToRegister(input);

  if (instr->hydrogen()->value()->representation().IsSmi()) {
    __ SmiUntag(input_reg);
  } else {
    DeferredTaggedToI* deferred = new(zone()) DeferredTaggedToI(this, instr);

    // Let the deferred code handle the HeapObject case.
    __ JumpIfNotSmi(input_reg, deferred->entry());

    // Smi to int32 conversion.
    __ SmiUntag(input_reg);
    __ bind(deferred->exit());
  }
}


void LCodeGen::DoNumberUntagD(LNumberUntagD* instr) {
  LOperand* input = instr->value();
  ASSERT(input->IsRegister());
  LOperand* result = instr->result();
  ASSERT(result->IsDoubleRegister());

  Register input_reg = ToRegister(input);
  DoubleRegister result_reg = ToDoubleRegister(result);

  HValue* value = instr->hydrogen()->value();
  NumberUntagDMode mode = value->representation().IsSmi()
      ? NUMBER_CANDIDATE_IS_SMI : NUMBER_CANDIDATE_IS_ANY_TAGGED;

  EmitNumberUntagD(input_reg, result_reg,
                   instr->hydrogen()->can_convert_undefined_to_nan(),
                   instr->hydrogen()->deoptimize_on_minus_zero(),
                   instr->environment(),
                   mode);
}


void LCodeGen::DoDoubleToI(LDoubleToI* instr) {
  Register result_reg = ToRegister(instr->result());
  Register scratch1 = scratch0();
  DoubleRegister double_input = ToDoubleRegister(instr->value());

  if (instr->truncating()) {
    __ TruncateDoubleToI(result_reg, double_input);
  } else {
    Register except_flag = LCodeGen::scratch1();

    __ EmitFPUTruncate(kRoundToMinusInf,
                       result_reg,
                       double_input,
                       scratch1,
                       double_scratch0(),
                       except_flag,
                       kCheckForInexactConversion);

    // Deopt if the operation did not succeed (except_flag != 0).
    DeoptimizeIf(ne, instr->environment(), except_flag, Operand(zero_reg));

    if (instr->hydrogen()->CheckFlag(HValue::kBailoutOnMinusZero)) {
      Label done;
      __ Branch(&done, ne, result_reg, Operand(zero_reg));
      __ mfc1(scratch1, double_input.high());
      __ And(scratch1, scratch1, Operand(HeapNumber::kSignMask));
      DeoptimizeIf(ne, instr->environment(), scratch1, Operand(zero_reg));
      __ bind(&done);
    }
  }
}


void LCodeGen::DoDoubleToSmi(LDoubleToSmi* instr) {
  Register result_reg = ToRegister(instr->result());
  Register scratch1 = LCodeGen::scratch0();
  DoubleRegister double_input = ToDoubleRegister(instr->value());

  if (instr->truncating()) {
    __ TruncateDoubleToI(result_reg, double_input);
  } else {
    Register except_flag = LCodeGen::scratch1();

    __ EmitFPUTruncate(kRoundToMinusInf,
                       result_reg,
                       double_input,
                       scratch1,
                       double_scratch0(),
                       except_flag,
                       kCheckForInexactConversion);

    // Deopt if the operation did not succeed (except_flag != 0).
    DeoptimizeIf(ne, instr->environment(), except_flag, Operand(zero_reg));

    if (instr->hydrogen()->CheckFlag(HValue::kBailoutOnMinusZero)) {
      Label done;
      __ Branch(&done, ne, result_reg, Operand(zero_reg));
      __ mfc1(scratch1, double_input.high());
      __ And(scratch1, scratch1, Operand(HeapNumber::kSignMask));
      DeoptimizeIf(ne, instr->environment(), scratch1, Operand(zero_reg));
      __ bind(&done);
    }
  }
  __ SmiTagCheckOverflow(result_reg, result_reg, scratch1);
  DeoptimizeIf(lt, instr->environment(), scratch1, Operand(zero_reg));
}


void LCodeGen::DoCheckSmi(LCheckSmi* instr) {
  LOperand* input = instr->value();
  __ And(at, ToRegister(input), Operand(kSmiTagMask));
  DeoptimizeIf(ne, instr->environment(), at, Operand(zero_reg));
}


void LCodeGen::DoCheckNonSmi(LCheckNonSmi* instr) {
  if (!instr->hydrogen()->value()->IsHeapObject()) {
    LOperand* input = instr->value();
    __ And(at, ToRegister(input), Operand(kSmiTagMask));
    DeoptimizeIf(eq, instr->environment(), at, Operand(zero_reg));
  }
}


void LCodeGen::DoCheckInstanceType(LCheckInstanceType* instr) {
  Register input = ToRegister(instr->value());
  Register scratch = scratch0();

  __ GetObjectType(input, scratch, scratch);

  if (instr->hydrogen()->is_interval_check()) {
    InstanceType first;
    InstanceType last;
    instr->hydrogen()->GetCheckInterval(&first, &last);

    // If there is only one type in the interval check for equality.
    if (first == last) {
      DeoptimizeIf(ne, instr->environment(), scratch, Operand(first));
    } else {
      DeoptimizeIf(lo, instr->environment(), scratch, Operand(first));
      // Omit check for the last type.
      if (last != LAST_TYPE) {
        DeoptimizeIf(hi, instr->environment(), scratch, Operand(last));
      }
    }
  } else {
    uint8_t mask;
    uint8_t tag;
    instr->hydrogen()->GetCheckMaskAndTag(&mask, &tag);

    if (IsPowerOf2(mask)) {
      ASSERT(tag == 0 || IsPowerOf2(tag));
      __ And(at, scratch, mask);
      DeoptimizeIf(tag == 0 ? ne : eq, instr->environment(),
          at, Operand(zero_reg));
    } else {
      __ And(scratch, scratch, Operand(mask));
      DeoptimizeIf(ne, instr->environment(), scratch, Operand(tag));
    }
  }
}


void LCodeGen::DoCheckValue(LCheckValue* instr) {
  Register reg = ToRegister(instr->value());
  Handle<HeapObject> object = instr->hydrogen()->object().handle();
  AllowDeferredHandleDereference smi_check;
  if (isolate()->heap()->InNewSpace(*object)) {
    Register reg = ToRegister(instr->value());
    Handle<Cell> cell = isolate()->factory()->NewCell(object);
    __ li(at, Operand(Handle<Object>(cell)));
    __ lw(at, FieldMemOperand(at, Cell::kValueOffset));
    DeoptimizeIf(ne, instr->environment(), reg,
                 Operand(at));
  } else {
    DeoptimizeIf(ne, instr->environment(), reg,
                 Operand(object));
  }
}


void LCodeGen::DoDeferredInstanceMigration(LCheckMaps* instr, Register object) {
  {
    PushSafepointRegistersScope scope(this, Safepoint::kWithRegisters);
    __ push(object);
    __ mov(cp, zero_reg);
    __ CallRuntimeSaveDoubles(Runtime::kMigrateInstance);
    RecordSafepointWithRegisters(
        instr->pointer_map(), 1, Safepoint::kNoLazyDeopt);
    __ StoreToSafepointRegisterSlot(v0, scratch0());
  }
  __ And(at, scratch0(), Operand(kSmiTagMask));
  DeoptimizeIf(eq, instr->environment(), at, Operand(zero_reg));
}


void LCodeGen::DoCheckMaps(LCheckMaps* instr) {
  class DeferredCheckMaps V8_FINAL : public LDeferredCode {
   public:
    DeferredCheckMaps(LCodeGen* codegen, LCheckMaps* instr, Register object)
        : LDeferredCode(codegen), instr_(instr), object_(object) {
      SetExit(check_maps());
    }
    virtual void Generate() V8_OVERRIDE {
      codegen()->DoDeferredInstanceMigration(instr_, object_);
    }
    Label* check_maps() { return &check_maps_; }
    virtual LInstruction* instr() V8_OVERRIDE { return instr_; }
   private:
    LCheckMaps* instr_;
    Label check_maps_;
    Register object_;
  };

  if (instr->hydrogen()->CanOmitMapChecks()) return;
  Register map_reg = scratch0();
  LOperand* input = instr->value();
  ASSERT(input->IsRegister());
  Register reg = ToRegister(input);
  __ lw(map_reg, FieldMemOperand(reg, HeapObject::kMapOffset));

  DeferredCheckMaps* deferred = NULL;
  if (instr->hydrogen()->has_migration_target()) {
    deferred = new(zone()) DeferredCheckMaps(this, instr, reg);
    __ bind(deferred->check_maps());
  }

  UniqueSet<Map> map_set = instr->hydrogen()->map_set();
  Label success;
  for (int i = 0; i < map_set.size() - 1; i++) {
    Handle<Map> map = map_set.at(i).handle();
    __ CompareMapAndBranch(map_reg, map, &success, eq, &success);
  }
  Handle<Map> map = map_set.at(map_set.size() - 1).handle();
  // Do the CompareMap() directly within the Branch() and DeoptimizeIf().
  if (instr->hydrogen()->has_migration_target()) {
    __ Branch(deferred->entry(), ne, map_reg, Operand(map));
  } else {
    DeoptimizeIf(ne, instr->environment(), map_reg, Operand(map));
  }

  __ bind(&success);
}


void LCodeGen::DoClampDToUint8(LClampDToUint8* instr) {
  DoubleRegister value_reg = ToDoubleRegister(instr->unclamped());
  Register result_reg = ToRegister(instr->result());
  DoubleRegister temp_reg = ToDoubleRegister(instr->temp());
  __ ClampDoubleToUint8(result_reg, value_reg, temp_reg);
}


void LCodeGen::DoClampIToUint8(LClampIToUint8* instr) {
  Register unclamped_reg = ToRegister(instr->unclamped());
  Register result_reg = ToRegister(instr->result());
  __ ClampUint8(result_reg, unclamped_reg);
}


void LCodeGen::DoClampTToUint8(LClampTToUint8* instr) {
  Register scratch = scratch0();
  Register input_reg = ToRegister(instr->unclamped());
  Register result_reg = ToRegister(instr->result());
  DoubleRegister temp_reg = ToDoubleRegister(instr->temp());
  Label is_smi, done, heap_number;

  // Both smi and heap number cases are handled.
  __ UntagAndJumpIfSmi(scratch, input_reg, &is_smi);

  // Check for heap number
  __ lw(scratch, FieldMemOperand(input_reg, HeapObject::kMapOffset));
  __ Branch(&heap_number, eq, scratch, Operand(factory()->heap_number_map()));

  // Check for undefined. Undefined is converted to zero for clamping
  // conversions.
  DeoptimizeIf(ne, instr->environment(), input_reg,
               Operand(factory()->undefined_value()));
  __ mov(result_reg, zero_reg);
  __ jmp(&done);

  // Heap number
  __ bind(&heap_number);
  __ ldc1(double_scratch0(), FieldMemOperand(input_reg,
                                             HeapNumber::kValueOffset));
  __ ClampDoubleToUint8(result_reg, double_scratch0(), temp_reg);
  __ jmp(&done);

  __ bind(&is_smi);
  __ ClampUint8(result_reg, scratch);

  __ bind(&done);
}


void LCodeGen::DoAllocate(LAllocate* instr) {
  class DeferredAllocate V8_FINAL : public LDeferredCode {
   public:
    DeferredAllocate(LCodeGen* codegen, LAllocate* instr)
        : LDeferredCode(codegen), instr_(instr) { }
    virtual void Generate() V8_OVERRIDE {
      codegen()->DoDeferredAllocate(instr_);
    }
    virtual LInstruction* instr() V8_OVERRIDE { return instr_; }
   private:
    LAllocate* instr_;
  };

  DeferredAllocate* deferred =
      new(zone()) DeferredAllocate(this, instr);

  Register result = ToRegister(instr->result());
  Register scratch = ToRegister(instr->temp1());
  Register scratch2 = ToRegister(instr->temp2());

  // Allocate memory for the object.
  AllocationFlags flags = TAG_OBJECT;
  if (instr->hydrogen()->MustAllocateDoubleAligned()) {
    flags = static_cast<AllocationFlags>(flags | DOUBLE_ALIGNMENT);
  }
  if (instr->hydrogen()->IsOldPointerSpaceAllocation()) {
    ASSERT(!instr->hydrogen()->IsOldDataSpaceAllocation());
    ASSERT(!instr->hydrogen()->IsNewSpaceAllocation());
    flags = static_cast<AllocationFlags>(flags | PRETENURE_OLD_POINTER_SPACE);
  } else if (instr->hydrogen()->IsOldDataSpaceAllocation()) {
    ASSERT(!instr->hydrogen()->IsNewSpaceAllocation());
    flags = static_cast<AllocationFlags>(flags | PRETENURE_OLD_DATA_SPACE);
  }
  if (instr->size()->IsConstantOperand()) {
    int32_t size = ToInteger32(LConstantOperand::cast(instr->size()));
    __ Allocate(size, result, scratch, scratch2, deferred->entry(), flags);
  } else {
    Register size = ToRegister(instr->size());
    __ Allocate(size,
                result,
                scratch,
                scratch2,
                deferred->entry(),
                flags);
  }

  __ bind(deferred->exit());

  if (instr->hydrogen()->MustPrefillWithFiller()) {
    if (instr->size()->IsConstantOperand()) {
      int32_t size = ToInteger32(LConstantOperand::cast(instr->size()));
      __ li(scratch, Operand(size));
    } else {
      scratch = ToRegister(instr->size());
    }
    __ Subu(scratch, scratch, Operand(kPointerSize));
    __ Subu(result, result, Operand(kHeapObjectTag));
    Label loop;
    __ bind(&loop);
    __ li(scratch2, Operand(isolate()->factory()->one_pointer_filler_map()));
    __ Addu(at, result, Operand(scratch));
    __ sw(scratch2, MemOperand(at));
    __ Subu(scratch, scratch, Operand(kPointerSize));
    __ Branch(&loop, ge, scratch, Operand(zero_reg));
    __ Addu(result, result, Operand(kHeapObjectTag));
  }
}


void LCodeGen::DoDeferredAllocate(LAllocate* instr) {
  Register result = ToRegister(instr->result());

  // TODO(3095996): Get rid of this. For now, we need to make the
  // result register contain a valid pointer because it is already
  // contained in the register pointer map.
  __ mov(result, zero_reg);

  PushSafepointRegistersScope scope(this, Safepoint::kWithRegisters);
  if (instr->size()->IsRegister()) {
    Register size = ToRegister(instr->size());
    ASSERT(!size.is(result));
    __ SmiTag(size);
    __ push(size);
  } else {
    int32_t size = ToInteger32(LConstantOperand::cast(instr->size()));
    __ Push(Smi::FromInt(size));
  }

  if (instr->hydrogen()->IsOldPointerSpaceAllocation()) {
    ASSERT(!instr->hydrogen()->IsOldDataSpaceAllocation());
    ASSERT(!instr->hydrogen()->IsNewSpaceAllocation());
    CallRuntimeFromDeferred(Runtime::kAllocateInOldPointerSpace, 1, instr,
                            instr->context());
  } else if (instr->hydrogen()->IsOldDataSpaceAllocation()) {
    ASSERT(!instr->hydrogen()->IsNewSpaceAllocation());
    CallRuntimeFromDeferred(Runtime::kAllocateInOldDataSpace, 1, instr,
                            instr->context());
  } else {
    CallRuntimeFromDeferred(Runtime::kAllocateInNewSpace, 1, instr,
                            instr->context());
  }
  __ StoreToSafepointRegisterSlot(v0, result);
}


void LCodeGen::DoToFastProperties(LToFastProperties* instr) {
  ASSERT(ToRegister(instr->value()).is(a0));
  ASSERT(ToRegister(instr->result()).is(v0));
  __ push(a0);
  CallRuntime(Runtime::kToFastProperties, 1, instr);
}


void LCodeGen::DoRegExpLiteral(LRegExpLiteral* instr) {
  ASSERT(ToRegister(instr->context()).is(cp));
  Label materialized;
  // Registers will be used as follows:
  // t3 = literals array.
  // a1 = regexp literal.
  // a0 = regexp literal clone.
  // a2 and t0-t2 are used as temporaries.
  int literal_offset =
      FixedArray::OffsetOfElementAt(instr->hydrogen()->literal_index());
  __ li(t3, instr->hydrogen()->literals());
  __ lw(a1, FieldMemOperand(t3, literal_offset));
  __ LoadRoot(at, Heap::kUndefinedValueRootIndex);
  __ Branch(&materialized, ne, a1, Operand(at));

  // Create regexp literal using runtime function
  // Result will be in v0.
  __ li(t2, Operand(Smi::FromInt(instr->hydrogen()->literal_index())));
  __ li(t1, Operand(instr->hydrogen()->pattern()));
  __ li(t0, Operand(instr->hydrogen()->flags()));
  __ Push(t3, t2, t1, t0);
  CallRuntime(Runtime::kMaterializeRegExpLiteral, 4, instr);
  __ mov(a1, v0);

  __ bind(&materialized);
  int size = JSRegExp::kSize + JSRegExp::kInObjectFieldCount * kPointerSize;
  Label allocated, runtime_allocate;

  __ Allocate(size, v0, a2, a3, &runtime_allocate, TAG_OBJECT);
  __ jmp(&allocated);

  __ bind(&runtime_allocate);
  __ li(a0, Operand(Smi::FromInt(size)));
  __ Push(a1, a0);
  CallRuntime(Runtime::kAllocateInNewSpace, 1, instr);
  __ pop(a1);

  __ bind(&allocated);
  // Copy the content into the newly allocated memory.
  // (Unroll copy loop once for better throughput).
  for (int i = 0; i < size - kPointerSize; i += 2 * kPointerSize) {
    __ lw(a3, FieldMemOperand(a1, i));
    __ lw(a2, FieldMemOperand(a1, i + kPointerSize));
    __ sw(a3, FieldMemOperand(v0, i));
    __ sw(a2, FieldMemOperand(v0, i + kPointerSize));
  }
  if ((size % (2 * kPointerSize)) != 0) {
    __ lw(a3, FieldMemOperand(a1, size - kPointerSize));
    __ sw(a3, FieldMemOperand(v0, size - kPointerSize));
  }
}


void LCodeGen::DoFunctionLiteral(LFunctionLiteral* instr) {
  ASSERT(ToRegister(instr->context()).is(cp));
  // Use the fast case closure allocation code that allocates in new
  // space for nested functions that don't need literals cloning.
  bool pretenure = instr->hydrogen()->pretenure();
  if (!pretenure && instr->hydrogen()->has_no_literals()) {
    FastNewClosureStub stub(instr->hydrogen()->language_mode(),
                            instr->hydrogen()->is_generator());
    __ li(a2, Operand(instr->hydrogen()->shared_info()));
    CallCode(stub.GetCode(isolate()), RelocInfo::CODE_TARGET, instr);
  } else {
    __ li(a2, Operand(instr->hydrogen()->shared_info()));
    __ li(a1, Operand(pretenure ? factory()->true_value()
                                : factory()->false_value()));
    __ Push(cp, a2, a1);
    CallRuntime(Runtime::kNewClosure, 3, instr);
  }
}


void LCodeGen::DoTypeof(LTypeof* instr) {
  ASSERT(ToRegister(instr->result()).is(v0));
  Register input = ToRegister(instr->value());
  __ push(input);
  CallRuntime(Runtime::kTypeof, 1, instr);
}


void LCodeGen::DoTypeofIsAndBranch(LTypeofIsAndBranch* instr) {
  Register input = ToRegister(instr->value());

  Register cmp1 = no_reg;
  Operand cmp2 = Operand(no_reg);

  Condition final_branch_condition = EmitTypeofIs(instr->TrueLabel(chunk_),
                                                  instr->FalseLabel(chunk_),
                                                  input,
                                                  instr->type_literal(),
                                                  cmp1,
                                                  cmp2);

  ASSERT(cmp1.is_valid());
  ASSERT(!cmp2.is_reg() || cmp2.rm().is_valid());

  if (final_branch_condition != kNoCondition) {
    EmitBranch(instr, final_branch_condition, cmp1, cmp2);
  }
}


Condition LCodeGen::EmitTypeofIs(Label* true_label,
                                 Label* false_label,
                                 Register input,
                                 Handle<String> type_name,
                                 Register& cmp1,
                                 Operand& cmp2) {
  // This function utilizes the delay slot heavily. This is used to load
  // values that are always usable without depending on the type of the input
  // register.
  Condition final_branch_condition = kNoCondition;
  Register scratch = scratch0();
  if (type_name->Equals(heap()->number_string())) {
    __ JumpIfSmi(input, true_label);
    __ lw(input, FieldMemOperand(input, HeapObject::kMapOffset));
    __ LoadRoot(at, Heap::kHeapNumberMapRootIndex);
    cmp1 = input;
    cmp2 = Operand(at);
    final_branch_condition = eq;

  } else if (type_name->Equals(heap()->string_string())) {
    __ JumpIfSmi(input, false_label);
    __ GetObjectType(input, input, scratch);
    __ Branch(USE_DELAY_SLOT, false_label,
              ge, scratch, Operand(FIRST_NONSTRING_TYPE));
    // input is an object so we can load the BitFieldOffset even if we take the
    // other branch.
    __ lbu(at, FieldMemOperand(input, Map::kBitFieldOffset));
    __ And(at, at, 1 << Map::kIsUndetectable);
    cmp1 = at;
    cmp2 = Operand(zero_reg);
    final_branch_condition = eq;

  } else if (type_name->Equals(heap()->symbol_string())) {
    __ JumpIfSmi(input, false_label);
    __ GetObjectType(input, input, scratch);
    cmp1 = scratch;
    cmp2 = Operand(SYMBOL_TYPE);
    final_branch_condition = eq;

  } else if (type_name->Equals(heap()->boolean_string())) {
    __ LoadRoot(at, Heap::kTrueValueRootIndex);
    __ Branch(USE_DELAY_SLOT, true_label, eq, at, Operand(input));
    __ LoadRoot(at, Heap::kFalseValueRootIndex);
    cmp1 = at;
    cmp2 = Operand(input);
    final_branch_condition = eq;

  } else if (FLAG_harmony_typeof && type_name->Equals(heap()->null_string())) {
    __ LoadRoot(at, Heap::kNullValueRootIndex);
    cmp1 = at;
    cmp2 = Operand(input);
    final_branch_condition = eq;

  } else if (type_name->Equals(heap()->undefined_string())) {
    __ LoadRoot(at, Heap::kUndefinedValueRootIndex);
    __ Branch(USE_DELAY_SLOT, true_label, eq, at, Operand(input));
    // The first instruction of JumpIfSmi is an And - it is safe in the delay
    // slot.
    __ JumpIfSmi(input, false_label);
    // Check for undetectable objects => true.
    __ lw(input, FieldMemOperand(input, HeapObject::kMapOffset));
    __ lbu(at, FieldMemOperand(input, Map::kBitFieldOffset));
    __ And(at, at, 1 << Map::kIsUndetectable);
    cmp1 = at;
    cmp2 = Operand(zero_reg);
    final_branch_condition = ne;

  } else if (type_name->Equals(heap()->function_string())) {
    STATIC_ASSERT(NUM_OF_CALLABLE_SPEC_OBJECT_TYPES == 2);
    __ JumpIfSmi(input, false_label);
    __ GetObjectType(input, scratch, input);
    __ Branch(true_label, eq, input, Operand(JS_FUNCTION_TYPE));
    cmp1 = input;
    cmp2 = Operand(JS_FUNCTION_PROXY_TYPE);
    final_branch_condition = eq;

  } else if (type_name->Equals(heap()->object_string())) {
    __ JumpIfSmi(input, false_label);
    if (!FLAG_harmony_typeof) {
      __ LoadRoot(at, Heap::kNullValueRootIndex);
      __ Branch(USE_DELAY_SLOT, true_label, eq, at, Operand(input));
    }
    Register map = input;
    __ GetObjectType(input, map, scratch);
    __ Branch(false_label,
              lt, scratch, Operand(FIRST_NONCALLABLE_SPEC_OBJECT_TYPE));
    __ Branch(USE_DELAY_SLOT, false_label,
              gt, scratch, Operand(LAST_NONCALLABLE_SPEC_OBJECT_TYPE));
    // map is still valid, so the BitField can be loaded in delay slot.
    // Check for undetectable objects => false.
    __ lbu(at, FieldMemOperand(map, Map::kBitFieldOffset));
    __ And(at, at, 1 << Map::kIsUndetectable);
    cmp1 = at;
    cmp2 = Operand(zero_reg);
    final_branch_condition = eq;

  } else {
    cmp1 = at;
    cmp2 = Operand(zero_reg);  // Set to valid regs, to avoid caller assertion.
    __ Branch(false_label);
  }

  return final_branch_condition;
}


void LCodeGen::DoIsConstructCallAndBranch(LIsConstructCallAndBranch* instr) {
  Register temp1 = ToRegister(instr->temp());

  EmitIsConstructCall(temp1, scratch0());

  EmitBranch(instr, eq, temp1,
             Operand(Smi::FromInt(StackFrame::CONSTRUCT)));
}


void LCodeGen::EmitIsConstructCall(Register temp1, Register temp2) {
  ASSERT(!temp1.is(temp2));
  // Get the frame pointer for the calling frame.
  __ lw(temp1, MemOperand(fp, StandardFrameConstants::kCallerFPOffset));

  // Skip the arguments adaptor frame if it exists.
  Label check_frame_marker;
  __ lw(temp2, MemOperand(temp1, StandardFrameConstants::kContextOffset));
  __ Branch(&check_frame_marker, ne, temp2,
            Operand(Smi::FromInt(StackFrame::ARGUMENTS_ADAPTOR)));
  __ lw(temp1, MemOperand(temp1, StandardFrameConstants::kCallerFPOffset));

  // Check the marker in the calling frame.
  __ bind(&check_frame_marker);
  __ lw(temp1, MemOperand(temp1, StandardFrameConstants::kMarkerOffset));
}


void LCodeGen::EnsureSpaceForLazyDeopt(int space_needed) {
  if (info()->IsStub()) return;
  // Ensure that we have enough space after the previous lazy-bailout
  // instruction for patching the code here.
  int current_pc = masm()->pc_offset();
  if (current_pc < last_lazy_deopt_pc_ + space_needed) {
    int padding_size = last_lazy_deopt_pc_ + space_needed - current_pc;
    ASSERT_EQ(0, padding_size % Assembler::kInstrSize);
    while (padding_size > 0) {
      __ nop();
      padding_size -= Assembler::kInstrSize;
    }
  }
}


void LCodeGen::DoLazyBailout(LLazyBailout* instr) {
  EnsureSpaceForLazyDeopt(Deoptimizer::patch_size());
  last_lazy_deopt_pc_ = masm()->pc_offset();
  ASSERT(instr->HasEnvironment());
  LEnvironment* env = instr->environment();
  RegisterEnvironmentForDeoptimization(env, Safepoint::kLazyDeopt);
  safepoints_.RecordLazyDeoptimizationIndex(env->deoptimization_index());
}


void LCodeGen::DoDeoptimize(LDeoptimize* instr) {
  Deoptimizer::BailoutType type = instr->hydrogen()->type();
  // TODO(danno): Stubs expect all deopts to be lazy for historical reasons (the
  // needed return address), even though the implementation of LAZY and EAGER is
  // now identical. When LAZY is eventually completely folded into EAGER, remove
  // the special case below.
  if (info()->IsStub() && type == Deoptimizer::EAGER) {
    type = Deoptimizer::LAZY;
  }

  Comment(";;; deoptimize: %s", instr->hydrogen()->reason());
  DeoptimizeIf(al, instr->environment(), type, zero_reg, Operand(zero_reg));
}


void LCodeGen::DoDummyUse(LDummyUse* instr) {
  // Nothing to see here, move on!
}


void LCodeGen::DoDeferredStackCheck(LStackCheck* instr) {
  PushSafepointRegistersScope scope(this, Safepoint::kWithRegisters);
  LoadContextFromDeferred(instr->context());
  __ CallRuntimeSaveDoubles(Runtime::kStackGuard);
  RecordSafepointWithLazyDeopt(
      instr, RECORD_SAFEPOINT_WITH_REGISTERS_AND_NO_ARGUMENTS);
  ASSERT(instr->HasEnvironment());
  LEnvironment* env = instr->environment();
  safepoints_.RecordLazyDeoptimizationIndex(env->deoptimization_index());
}


void LCodeGen::DoStackCheck(LStackCheck* instr) {
  class DeferredStackCheck V8_FINAL : public LDeferredCode {
   public:
    DeferredStackCheck(LCodeGen* codegen, LStackCheck* instr)
        : LDeferredCode(codegen), instr_(instr) { }
    virtual void Generate() V8_OVERRIDE {
      codegen()->DoDeferredStackCheck(instr_);
    }
    virtual LInstruction* instr() V8_OVERRIDE { return instr_; }
   private:
    LStackCheck* instr_;
  };

  ASSERT(instr->HasEnvironment());
  LEnvironment* env = instr->environment();
  // There is no LLazyBailout instruction for stack-checks. We have to
  // prepare for lazy deoptimization explicitly here.
  if (instr->hydrogen()->is_function_entry()) {
    // Perform stack overflow check.
    Label done;
    __ LoadRoot(at, Heap::kStackLimitRootIndex);
    __ Branch(&done, hs, sp, Operand(at));
    ASSERT(instr->context()->IsRegister());
    ASSERT(ToRegister(instr->context()).is(cp));
    CallCode(isolate()->builtins()->StackCheck(),
             RelocInfo::CODE_TARGET,
             instr);
    EnsureSpaceForLazyDeopt(Deoptimizer::patch_size());
    last_lazy_deopt_pc_ = masm()->pc_offset();
    __ bind(&done);
    RegisterEnvironmentForDeoptimization(env, Safepoint::kLazyDeopt);
    safepoints_.RecordLazyDeoptimizationIndex(env->deoptimization_index());
  } else {
    ASSERT(instr->hydrogen()->is_backwards_branch());
    // Perform stack overflow check if this goto needs it before jumping.
    DeferredStackCheck* deferred_stack_check =
        new(zone()) DeferredStackCheck(this, instr);
    __ LoadRoot(at, Heap::kStackLimitRootIndex);
    __ Branch(deferred_stack_check->entry(), lo, sp, Operand(at));
    EnsureSpaceForLazyDeopt(Deoptimizer::patch_size());
    last_lazy_deopt_pc_ = masm()->pc_offset();
    __ bind(instr->done_label());
    deferred_stack_check->SetExit(instr->done_label());
    RegisterEnvironmentForDeoptimization(env, Safepoint::kLazyDeopt);
    // Don't record a deoptimization index for the safepoint here.
    // This will be done explicitly when emitting call and the safepoint in
    // the deferred code.
  }
}


void LCodeGen::DoOsrEntry(LOsrEntry* instr) {
  // This is a pseudo-instruction that ensures that the environment here is
  // properly registered for deoptimization and records the assembler's PC
  // offset.
  LEnvironment* environment = instr->environment();

  // If the environment were already registered, we would have no way of
  // backpatching it with the spill slot operands.
  ASSERT(!environment->HasBeenRegistered());
  RegisterEnvironmentForDeoptimization(environment, Safepoint::kNoLazyDeopt);

  GenerateOsrPrologue();
}


void LCodeGen::DoForInPrepareMap(LForInPrepareMap* instr) {
  Register result = ToRegister(instr->result());
  Register object = ToRegister(instr->object());
  __ LoadRoot(at, Heap::kUndefinedValueRootIndex);
  DeoptimizeIf(eq, instr->environment(), object, Operand(at));

  Register null_value = t1;
  __ LoadRoot(null_value, Heap::kNullValueRootIndex);
  DeoptimizeIf(eq, instr->environment(), object, Operand(null_value));

  __ And(at, object, kSmiTagMask);
  DeoptimizeIf(eq, instr->environment(), at, Operand(zero_reg));

  STATIC_ASSERT(FIRST_JS_PROXY_TYPE == FIRST_SPEC_OBJECT_TYPE);
  __ GetObjectType(object, a1, a1);
  DeoptimizeIf(le, instr->environment(), a1, Operand(LAST_JS_PROXY_TYPE));

  Label use_cache, call_runtime;
  ASSERT(object.is(a0));
  __ CheckEnumCache(null_value, &call_runtime);

  __ lw(result, FieldMemOperand(object, HeapObject::kMapOffset));
  __ Branch(&use_cache);

  // Get the set of properties to enumerate.
  __ bind(&call_runtime);
  __ push(object);
  CallRuntime(Runtime::kGetPropertyNamesFast, 1, instr);

  __ lw(a1, FieldMemOperand(v0, HeapObject::kMapOffset));
  ASSERT(result.is(v0));
  __ LoadRoot(at, Heap::kMetaMapRootIndex);
  DeoptimizeIf(ne, instr->environment(), a1, Operand(at));
  __ bind(&use_cache);
}


void LCodeGen::DoForInCacheArray(LForInCacheArray* instr) {
  Register map = ToRegister(instr->map());
  Register result = ToRegister(instr->result());
  Label load_cache, done;
  __ EnumLength(result, map);
  __ Branch(&load_cache, ne, result, Operand(Smi::FromInt(0)));
  __ li(result, Operand(isolate()->factory()->empty_fixed_array()));
  __ jmp(&done);

  __ bind(&load_cache);
  __ LoadInstanceDescriptors(map, result);
  __ lw(result,
        FieldMemOperand(result, DescriptorArray::kEnumCacheOffset));
  __ lw(result,
        FieldMemOperand(result, FixedArray::SizeFor(instr->idx())));
  DeoptimizeIf(eq, instr->environment(), result, Operand(zero_reg));

  __ bind(&done);
}


void LCodeGen::DoCheckMapValue(LCheckMapValue* instr) {
  Register object = ToRegister(instr->value());
  Register map = ToRegister(instr->map());
  __ lw(scratch0(), FieldMemOperand(object, HeapObject::kMapOffset));
  DeoptimizeIf(ne, instr->environment(), map, Operand(scratch0()));
}


void LCodeGen::DoLoadFieldByIndex(LLoadFieldByIndex* instr) {
  Register object = ToRegister(instr->object());
  Register index = ToRegister(instr->index());
  Register result = ToRegister(instr->result());
  Register scratch = scratch0();

  Label out_of_object, done;
  __ Branch(USE_DELAY_SLOT, &out_of_object, lt, index, Operand(zero_reg));
  __ sll(scratch, index, kPointerSizeLog2 - kSmiTagSize);  // In delay slot.

  STATIC_ASSERT(kPointerSizeLog2 > kSmiTagSize);
  __ Addu(scratch, object, scratch);
  __ lw(result, FieldMemOperand(scratch, JSObject::kHeaderSize));

  __ Branch(&done);

  __ bind(&out_of_object);
  __ lw(result, FieldMemOperand(object, JSObject::kPropertiesOffset));
  // Index is equal to negated out of object property index plus 1.
  __ Subu(scratch, result, scratch);
  __ lw(result, FieldMemOperand(scratch,
                                FixedArray::kHeaderSize - kPointerSize));
  __ bind(&done);
}


#undef __

} }  // namespace v8::internal
