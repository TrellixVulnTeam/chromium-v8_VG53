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

#ifndef V8_DEOPTIMIZER_H_
#define V8_DEOPTIMIZER_H_

#include "v8.h"

#include "allocation.h"
#include "macro-assembler.h"
#include "zone-inl.h"


namespace v8 {
namespace internal {


static inline double read_double_value(Address p) {
#ifdef V8_HOST_CAN_READ_UNALIGNED
  return Memory::double_at(p);
#else  // V8_HOST_CAN_READ_UNALIGNED
  // Prevent gcc from using load-double (mips ldc1) on (possibly)
  // non-64-bit aligned address.
  union conversion {
    double d;
    uint32_t u[2];
  } c;
  c.u[0] = *reinterpret_cast<uint32_t*>(p);
  c.u[1] = *reinterpret_cast<uint32_t*>(p + 4);
  return c.d;
#endif  // V8_HOST_CAN_READ_UNALIGNED
}


class FrameDescription;
class TranslationIterator;
class DeoptimizedFrameInfo;

template<typename T>
class HeapNumberMaterializationDescriptor BASE_EMBEDDED {
 public:
  HeapNumberMaterializationDescriptor(T destination, double value)
      : destination_(destination), value_(value) { }

  T destination() const { return destination_; }
  double value() const { return value_; }

 private:
  T destination_;
  double value_;
};


class ObjectMaterializationDescriptor BASE_EMBEDDED {
 public:
  ObjectMaterializationDescriptor(
      Address slot_address, int frame, int length, int duplicate, bool is_args)
      : slot_address_(slot_address),
        jsframe_index_(frame),
        object_length_(length),
        duplicate_object_(duplicate),
        is_arguments_(is_args) { }

  Address slot_address() const { return slot_address_; }
  int jsframe_index() const { return jsframe_index_; }
  int object_length() const { return object_length_; }
  int duplicate_object() const { return duplicate_object_; }
  bool is_arguments() const { return is_arguments_; }

  // Only used for allocated receivers in DoComputeConstructStubFrame.
  void patch_slot_address(intptr_t slot) {
    slot_address_ = reinterpret_cast<Address>(slot);
  }

 private:
  Address slot_address_;
  int jsframe_index_;
  int object_length_;
  int duplicate_object_;
  bool is_arguments_;
};


class OptimizedFunctionVisitor BASE_EMBEDDED {
 public:
  virtual ~OptimizedFunctionVisitor() {}

  // Function which is called before iteration of any optimized functions
  // from given native context.
  virtual void EnterContext(Context* context) = 0;

  virtual void VisitFunction(JSFunction* function) = 0;

  // Function which is called after iteration of all optimized functions
  // from given native context.
  virtual void LeaveContext(Context* context) = 0;
};


class Deoptimizer : public Malloced {
 public:
  enum BailoutType {
    EAGER,
    LAZY,
    SOFT,
    // This last bailout type is not really a bailout, but used by the
    // debugger to deoptimize stack frames to allow inspection.
    DEBUGGER
  };

  static const int kBailoutTypesWithCodeEntry = SOFT + 1;

  struct JumpTableEntry {
    inline JumpTableEntry(Address entry,
                          Deoptimizer::BailoutType type,
                          bool frame)
        : label(),
          address(entry),
          bailout_type(type),
          needs_frame(frame) { }
    Label label;
    Address address;
    Deoptimizer::BailoutType bailout_type;
    bool needs_frame;
  };

  static bool TraceEnabledFor(BailoutType deopt_type,
                              StackFrame::Type frame_type);
  static const char* MessageFor(BailoutType type);

  int output_count() const { return output_count_; }

  Handle<JSFunction> function() const { return Handle<JSFunction>(function_); }
  Handle<Code> compiled_code() const { return Handle<Code>(compiled_code_); }
  BailoutType bailout_type() const { return bailout_type_; }

  // Number of created JS frames. Not all created frames are necessarily JS.
  int jsframe_count() const { return jsframe_count_; }

  static Deoptimizer* New(JSFunction* function,
                          BailoutType type,
                          unsigned bailout_id,
                          Address from,
                          int fp_to_sp_delta,
                          Isolate* isolate);
  static Deoptimizer* Grab(Isolate* isolate);

#ifdef ENABLE_DEBUGGER_SUPPORT
  // The returned object with information on the optimized frame needs to be
  // freed before another one can be generated.
  static DeoptimizedFrameInfo* DebuggerInspectableFrame(JavaScriptFrame* frame,
                                                        int jsframe_index,
                                                        Isolate* isolate);
  static void DeleteDebuggerInspectableFrame(DeoptimizedFrameInfo* info,
                                             Isolate* isolate);
#endif

  // Makes sure that there is enough room in the relocation
  // information of a code object to perform lazy deoptimization
  // patching. If there is not enough room a new relocation
  // information object is allocated and comments are added until it
  // is big enough.
  static void EnsureRelocSpaceForLazyDeoptimization(Handle<Code> code);

  // Deoptimize the function now. Its current optimized code will never be run
  // again and any activations of the optimized code will get deoptimized when
  // execution returns.
  static void DeoptimizeFunction(JSFunction* function);

  // Deoptimize all code in the given isolate.
  static void DeoptimizeAll(Isolate* isolate);

  // Deoptimize code associated with the given global object.
  static void DeoptimizeGlobalObject(JSObject* object);

  // Deoptimizes all optimized code that has been previously marked
  // (via code->set_marked_for_deoptimization) and unlinks all functions that
  // refer to that code.
  static void DeoptimizeMarkedCode(Isolate* isolate);

  // Visit all the known optimized functions in a given isolate.
  static void VisitAllOptimizedFunctions(
      Isolate* isolate, OptimizedFunctionVisitor* visitor);

  // The size in bytes of the code required at a lazy deopt patch site.
  static int patch_size();

  ~Deoptimizer();

  void MaterializeHeapObjects(JavaScriptFrameIterator* it);
#ifdef ENABLE_DEBUGGER_SUPPORT
  void MaterializeHeapNumbersForDebuggerInspectableFrame(
      Address parameters_top,
      uint32_t parameters_size,
      Address expressions_top,
      uint32_t expressions_size,
      DeoptimizedFrameInfo* info);
#endif

  static void ComputeOutputFrames(Deoptimizer* deoptimizer);


  enum GetEntryMode {
    CALCULATE_ENTRY_ADDRESS,
    ENSURE_ENTRY_CODE
  };


  static Address GetDeoptimizationEntry(
      Isolate* isolate,
      int id,
      BailoutType type,
      GetEntryMode mode = ENSURE_ENTRY_CODE);
  static int GetDeoptimizationId(Isolate* isolate,
                                 Address addr,
                                 BailoutType type);
  static int GetOutputInfo(DeoptimizationOutputData* data,
                           BailoutId node_id,
                           SharedFunctionInfo* shared);

  // Code generation support.
  static int input_offset() { return OFFSET_OF(Deoptimizer, input_); }
  static int output_count_offset() {
    return OFFSET_OF(Deoptimizer, output_count_);
  }
  static int output_offset() { return OFFSET_OF(Deoptimizer, output_); }

  static int has_alignment_padding_offset() {
    return OFFSET_OF(Deoptimizer, has_alignment_padding_);
  }

  static int GetDeoptimizedCodeCount(Isolate* isolate);

  static const int kNotDeoptimizationEntry = -1;

  // Generators for the deoptimization entry code.
  class EntryGenerator BASE_EMBEDDED {
   public:
    EntryGenerator(MacroAssembler* masm, BailoutType type)
        : masm_(masm), type_(type) { }
    virtual ~EntryGenerator() { }

    void Generate();

   protected:
    MacroAssembler* masm() const { return masm_; }
    BailoutType type() const { return type_; }
    Isolate* isolate() const { return masm_->isolate(); }

    virtual void GeneratePrologue() { }

   private:
    MacroAssembler* masm_;
    Deoptimizer::BailoutType type_;
  };

  class TableEntryGenerator : public EntryGenerator {
   public:
    TableEntryGenerator(MacroAssembler* masm, BailoutType type,  int count)
        : EntryGenerator(masm, type), count_(count) { }

   protected:
    virtual void GeneratePrologue();

   private:
    int count() const { return count_; }

    int count_;
  };

  int ConvertJSFrameIndexToFrameIndex(int jsframe_index);

  static size_t GetMaxDeoptTableSize();

  static void EnsureCodeForDeoptimizationEntry(Isolate* isolate,
                                               BailoutType type,
                                               int max_entry_id);

  Isolate* isolate() const { return isolate_; }

 private:
  static const int kMinNumberOfEntries = 64;
  static const int kMaxNumberOfEntries = 16384;

  Deoptimizer(Isolate* isolate,
              JSFunction* function,
              BailoutType type,
              unsigned bailout_id,
              Address from,
              int fp_to_sp_delta,
              Code* optimized_code);
  Code* FindOptimizedCode(JSFunction* function, Code* optimized_code);
  void PrintFunctionName();
  void DeleteFrameDescriptions();

  void DoComputeOutputFrames();
  void DoComputeJSFrame(TranslationIterator* iterator, int frame_index);
  void DoComputeArgumentsAdaptorFrame(TranslationIterator* iterator,
                                      int frame_index);
  void DoComputeConstructStubFrame(TranslationIterator* iterator,
                                   int frame_index);
  void DoComputeAccessorStubFrame(TranslationIterator* iterator,
                                  int frame_index,
                                  bool is_setter_stub_frame);
  void DoComputeCompiledStubFrame(TranslationIterator* iterator,
                                  int frame_index);

  void DoTranslateObject(TranslationIterator* iterator,
                         int object_index,
                         int field_index);

  enum DeoptimizerTranslatedValueType {
    TRANSLATED_VALUE_IS_NATIVE,
    TRANSLATED_VALUE_IS_TAGGED
  };

  void DoTranslateCommand(TranslationIterator* iterator,
      int frame_index,
      unsigned output_offset,
      DeoptimizerTranslatedValueType value_type = TRANSLATED_VALUE_IS_TAGGED);

  unsigned ComputeInputFrameSize() const;
  unsigned ComputeFixedSize(JSFunction* function) const;

  unsigned ComputeIncomingArgumentSize(JSFunction* function) const;
  unsigned ComputeOutgoingArgumentSize() const;

  Object* ComputeLiteral(int index) const;

  void AddObjectStart(intptr_t slot_address, int argc, bool is_arguments);
  void AddObjectDuplication(intptr_t slot, int object_index);
  void AddObjectTaggedValue(intptr_t value);
  void AddObjectDoubleValue(double value);
  void AddDoubleValue(intptr_t slot_address, double value);

  bool ArgumentsObjectIsAdapted(int object_index) {
    ObjectMaterializationDescriptor desc = deferred_objects_.at(object_index);
    int reverse_jsframe_index = jsframe_count_ - desc.jsframe_index() - 1;
    return jsframe_has_adapted_arguments_[reverse_jsframe_index];
  }

  Handle<JSFunction> ArgumentsObjectFunction(int object_index) {
    ObjectMaterializationDescriptor desc = deferred_objects_.at(object_index);
    int reverse_jsframe_index = jsframe_count_ - desc.jsframe_index() - 1;
    return jsframe_functions_[reverse_jsframe_index];
  }

  // Helper function for heap object materialization.
  Handle<Object> MaterializeNextHeapObject();
  Handle<Object> MaterializeNextValue();

  static void GenerateDeoptimizationEntries(
      MacroAssembler* masm, int count, BailoutType type);

  // Marks all the code in the given context for deoptimization.
  static void MarkAllCodeForContext(Context* native_context);

  // Visit all the known optimized functions in a given context.
  static void VisitAllOptimizedFunctionsForContext(
      Context* context, OptimizedFunctionVisitor* visitor);

  // Deoptimizes all code marked in the given context.
  static void DeoptimizeMarkedCodeForContext(Context* native_context);

  // Patch the given code so that it will deoptimize itself.
  static void PatchCodeForDeoptimization(Isolate* isolate, Code* code);

  // Searches the list of known deoptimizing code for a Code object
  // containing the given address (which is supposedly faster than
  // searching all code objects).
  Code* FindDeoptimizingCode(Address addr);

  // Fill the input from from a JavaScript frame. This is used when
  // the debugger needs to inspect an optimized frame. For normal
  // deoptimizations the input frame is filled in generated code.
  void FillInputFrame(Address tos, JavaScriptFrame* frame);

  // Fill the given output frame's registers to contain the failure handler
  // address and the number of parameters for a stub failure trampoline.
  void SetPlatformCompiledStubRegisters(FrameDescription* output_frame,
                                        CodeStubInterfaceDescriptor* desc);

  // Fill the given output frame's double registers with the original values
  // from the input frame's double registers.
  void CopyDoubleRegisters(FrameDescription* output_frame);

  // Determines whether the input frame contains alignment padding by looking
  // at the dynamic alignment state slot inside the frame.
  bool HasAlignmentPadding(JSFunction* function);

  Isolate* isolate_;
  JSFunction* function_;
  Code* compiled_code_;
  unsigned bailout_id_;
  BailoutType bailout_type_;
  Address from_;
  int fp_to_sp_delta_;
  int has_alignment_padding_;

  // Input frame description.
  FrameDescription* input_;
  // Number of output frames.
  int output_count_;
  // Number of output js frames.
  int jsframe_count_;
  // Array of output frame descriptions.
  FrameDescription** output_;

  // Deferred values to be materialized.
  List<Object*> deferred_objects_tagged_values_;
  List<HeapNumberMaterializationDescriptor<int> >
      deferred_objects_double_values_;
  List<ObjectMaterializationDescriptor> deferred_objects_;
  List<HeapNumberMaterializationDescriptor<Address> > deferred_heap_numbers_;

  // Output frame information. Only used during heap object materialization.
  List<Handle<JSFunction> > jsframe_functions_;
  List<bool> jsframe_has_adapted_arguments_;

  // Materialized objects. Only used during heap object materialization.
  List<Handle<Object> >* materialized_values_;
  List<Handle<Object> >* materialized_objects_;
  int materialization_value_index_;
  int materialization_object_index_;

#ifdef DEBUG
  DisallowHeapAllocation* disallow_heap_allocation_;
#endif  // DEBUG

  bool trace_;

  static const int table_entry_size_;

  friend class FrameDescription;
  friend class DeoptimizedFrameInfo;
};


class FrameDescription {
 public:
  FrameDescription(uint32_t frame_size,
                   JSFunction* function);

  void* operator new(size_t size, uint32_t frame_size) {
    // Subtracts kPointerSize, as the member frame_content_ already supplies
    // the first element of the area to store the frame.
    return malloc(size + frame_size - kPointerSize);
  }

  void operator delete(void* pointer, uint32_t frame_size) {
    free(pointer);
  }

  void operator delete(void* description) {
    free(description);
  }

  uint32_t GetFrameSize() const {
    ASSERT(static_cast<uint32_t>(frame_size_) == frame_size_);
    return static_cast<uint32_t>(frame_size_);
  }

  JSFunction* GetFunction() const { return function_; }

  unsigned GetOffsetFromSlotIndex(int slot_index);

  intptr_t GetFrameSlot(unsigned offset) {
    return *GetFrameSlotPointer(offset);
  }

  double GetDoubleFrameSlot(unsigned offset) {
    intptr_t* ptr = GetFrameSlotPointer(offset);
    return read_double_value(reinterpret_cast<Address>(ptr));
  }

  void SetFrameSlot(unsigned offset, intptr_t value) {
    *GetFrameSlotPointer(offset) = value;
  }

  void SetCallerPc(unsigned offset, intptr_t value);

  void SetCallerFp(unsigned offset, intptr_t value);

  intptr_t GetRegister(unsigned n) const {
#if DEBUG
    // This convoluted ASSERT is needed to work around a gcc problem that
    // improperly detects an array bounds overflow in optimized debug builds
    // when using a plain ASSERT.
    if (n >= ARRAY_SIZE(registers_)) {
      ASSERT(false);
      return 0;
    }
#endif
    return registers_[n];
  }

  double GetDoubleRegister(unsigned n) const {
    ASSERT(n < ARRAY_SIZE(double_registers_));
    return double_registers_[n];
  }

  void SetRegister(unsigned n, intptr_t value) {
    ASSERT(n < ARRAY_SIZE(registers_));
    registers_[n] = value;
  }

  void SetDoubleRegister(unsigned n, double value) {
    ASSERT(n < ARRAY_SIZE(double_registers_));
    double_registers_[n] = value;
  }

  intptr_t GetTop() const { return top_; }
  void SetTop(intptr_t top) { top_ = top; }

  intptr_t GetPc() const { return pc_; }
  void SetPc(intptr_t pc) { pc_ = pc; }

  intptr_t GetFp() const { return fp_; }
  void SetFp(intptr_t fp) { fp_ = fp; }

  intptr_t GetContext() const { return context_; }
  void SetContext(intptr_t context) { context_ = context; }

  Smi* GetState() const { return state_; }
  void SetState(Smi* state) { state_ = state; }

  void SetContinuation(intptr_t pc) { continuation_ = pc; }

  StackFrame::Type GetFrameType() const { return type_; }
  void SetFrameType(StackFrame::Type type) { type_ = type; }

  // Get the incoming arguments count.
  int ComputeParametersCount();

  // Get a parameter value for an unoptimized frame.
  Object* GetParameter(int index);

  // Get the expression stack height for a unoptimized frame.
  unsigned GetExpressionCount();

  // Get the expression stack value for an unoptimized frame.
  Object* GetExpression(int index);

  static int registers_offset() {
    return OFFSET_OF(FrameDescription, registers_);
  }

  static int double_registers_offset() {
    return OFFSET_OF(FrameDescription, double_registers_);
  }

  static int frame_size_offset() {
    return OFFSET_OF(FrameDescription, frame_size_);
  }

  static int pc_offset() {
    return OFFSET_OF(FrameDescription, pc_);
  }

  static int state_offset() {
    return OFFSET_OF(FrameDescription, state_);
  }

  static int continuation_offset() {
    return OFFSET_OF(FrameDescription, continuation_);
  }

  static int frame_content_offset() {
    return OFFSET_OF(FrameDescription, frame_content_);
  }

 private:
  static const uint32_t kZapUint32 = 0xbeeddead;

  // Frame_size_ must hold a uint32_t value.  It is only a uintptr_t to
  // keep the variable-size array frame_content_ of type intptr_t at
  // the end of the structure aligned.
  uintptr_t frame_size_;  // Number of bytes.
  JSFunction* function_;
  intptr_t registers_[Register::kNumRegisters];
  double double_registers_[DoubleRegister::kMaxNumRegisters];
  intptr_t top_;
  intptr_t pc_;
  intptr_t fp_;
  intptr_t context_;
  StackFrame::Type type_;
  Smi* state_;

  // Continuation is the PC where the execution continues after
  // deoptimizing.
  intptr_t continuation_;

  // This must be at the end of the object as the object is allocated larger
  // than it's definition indicate to extend this array.
  intptr_t frame_content_[1];

  intptr_t* GetFrameSlotPointer(unsigned offset) {
    ASSERT(offset < frame_size_);
    return reinterpret_cast<intptr_t*>(
        reinterpret_cast<Address>(this) + frame_content_offset() + offset);
  }

  int ComputeFixedSize();
};


class DeoptimizerData {
 public:
  explicit DeoptimizerData(MemoryAllocator* allocator);
  ~DeoptimizerData();

#ifdef ENABLE_DEBUGGER_SUPPORT
  void Iterate(ObjectVisitor* v);
#endif

 private:
  MemoryAllocator* allocator_;
  int deopt_entry_code_entries_[Deoptimizer::kBailoutTypesWithCodeEntry];
  MemoryChunk* deopt_entry_code_[Deoptimizer::kBailoutTypesWithCodeEntry];

#ifdef ENABLE_DEBUGGER_SUPPORT
  DeoptimizedFrameInfo* deoptimized_frame_info_;
#endif

  Deoptimizer* current_;

  friend class Deoptimizer;

  DISALLOW_COPY_AND_ASSIGN(DeoptimizerData);
};


class TranslationBuffer BASE_EMBEDDED {
 public:
  explicit TranslationBuffer(Zone* zone) : contents_(256, zone) { }

  int CurrentIndex() const { return contents_.length(); }
  void Add(int32_t value, Zone* zone);

  Handle<ByteArray> CreateByteArray(Factory* factory);

 private:
  ZoneList<uint8_t> contents_;
};


class TranslationIterator BASE_EMBEDDED {
 public:
  TranslationIterator(ByteArray* buffer, int index)
      : buffer_(buffer), index_(index) {
    ASSERT(index >= 0 && index < buffer->length());
  }

  int32_t Next();

  bool HasNext() const { return index_ < buffer_->length(); }

  void Skip(int n) {
    for (int i = 0; i < n; i++) Next();
  }

 private:
  ByteArray* buffer_;
  int index_;
};


#define TRANSLATION_OPCODE_LIST(V)                                             \
  V(BEGIN)                                                                     \
  V(JS_FRAME)                                                                  \
  V(CONSTRUCT_STUB_FRAME)                                                      \
  V(GETTER_STUB_FRAME)                                                         \
  V(SETTER_STUB_FRAME)                                                         \
  V(ARGUMENTS_ADAPTOR_FRAME)                                                   \
  V(COMPILED_STUB_FRAME)                                                       \
  V(DUPLICATED_OBJECT)                                                         \
  V(ARGUMENTS_OBJECT)                                                          \
  V(CAPTURED_OBJECT)                                                           \
  V(REGISTER)                                                                  \
  V(INT32_REGISTER)                                                            \
  V(UINT32_REGISTER)                                                           \
  V(DOUBLE_REGISTER)                                                           \
  V(STACK_SLOT)                                                                \
  V(INT32_STACK_SLOT)                                                          \
  V(UINT32_STACK_SLOT)                                                         \
  V(DOUBLE_STACK_SLOT)                                                         \
  V(LITERAL)


class Translation BASE_EMBEDDED {
 public:
#define DECLARE_TRANSLATION_OPCODE_ENUM(item) item,
  enum Opcode {
    TRANSLATION_OPCODE_LIST(DECLARE_TRANSLATION_OPCODE_ENUM)
    LAST = LITERAL
  };
#undef DECLARE_TRANSLATION_OPCODE_ENUM

  Translation(TranslationBuffer* buffer, int frame_count, int jsframe_count,
              Zone* zone)
      : buffer_(buffer),
        index_(buffer->CurrentIndex()),
        zone_(zone) {
    buffer_->Add(BEGIN, zone);
    buffer_->Add(frame_count, zone);
    buffer_->Add(jsframe_count, zone);
  }

  int index() const { return index_; }

  // Commands.
  void BeginJSFrame(BailoutId node_id, int literal_id, unsigned height);
  void BeginCompiledStubFrame();
  void BeginArgumentsAdaptorFrame(int literal_id, unsigned height);
  void BeginConstructStubFrame(int literal_id, unsigned height);
  void BeginGetterStubFrame(int literal_id);
  void BeginSetterStubFrame(int literal_id);
  void BeginArgumentsObject(int args_length);
  void BeginCapturedObject(int length);
  void DuplicateObject(int object_index);
  void StoreRegister(Register reg);
  void StoreInt32Register(Register reg);
  void StoreUint32Register(Register reg);
  void StoreDoubleRegister(DoubleRegister reg);
  void StoreStackSlot(int index);
  void StoreInt32StackSlot(int index);
  void StoreUint32StackSlot(int index);
  void StoreDoubleStackSlot(int index);
  void StoreLiteral(int literal_id);
  void StoreArgumentsObject(bool args_known, int args_index, int args_length);

  Zone* zone() const { return zone_; }

  static int NumberOfOperandsFor(Opcode opcode);

#if defined(OBJECT_PRINT) || defined(ENABLE_DISASSEMBLER)
  static const char* StringFor(Opcode opcode);
#endif

  // A literal id which refers to the JSFunction itself.
  static const int kSelfLiteralId = -239;

 private:
  TranslationBuffer* buffer_;
  int index_;
  Zone* zone_;
};


class SlotRef BASE_EMBEDDED {
 public:
  enum SlotRepresentation {
    UNKNOWN,
    TAGGED,
    INT32,
    UINT32,
    DOUBLE,
    LITERAL
  };

  SlotRef()
      : addr_(NULL), representation_(UNKNOWN) { }

  SlotRef(Address addr, SlotRepresentation representation)
      : addr_(addr), representation_(representation) { }

  SlotRef(Isolate* isolate, Object* literal)
      : literal_(literal, isolate), representation_(LITERAL) { }

  Handle<Object> GetValue(Isolate* isolate) {
    switch (representation_) {
      case TAGGED:
        return Handle<Object>(Memory::Object_at(addr_), isolate);

      case INT32: {
        int value = Memory::int32_at(addr_);
        if (Smi::IsValid(value)) {
          return Handle<Object>(Smi::FromInt(value), isolate);
        } else {
          return isolate->factory()->NewNumberFromInt(value);
        }
      }

      case UINT32: {
        uint32_t value = Memory::uint32_at(addr_);
        if (value <= static_cast<uint32_t>(Smi::kMaxValue)) {
          return Handle<Object>(Smi::FromInt(static_cast<int>(value)), isolate);
        } else {
          return isolate->factory()->NewNumber(static_cast<double>(value));
        }
      }

      case DOUBLE: {
        double value = read_double_value(addr_);
        return isolate->factory()->NewNumber(value);
      }

      case LITERAL:
        return literal_;

      default:
        UNREACHABLE();
        return Handle<Object>::null();
    }
  }

  static Vector<SlotRef> ComputeSlotMappingForArguments(
      JavaScriptFrame* frame,
      int inlined_frame_index,
      int formal_parameter_count);

 private:
  Address addr_;
  Handle<Object> literal_;
  SlotRepresentation representation_;

  static Address SlotAddress(JavaScriptFrame* frame, int slot_index) {
    if (slot_index >= 0) {
      const int offset = JavaScriptFrameConstants::kLocal0Offset;
      return frame->fp() + offset - (slot_index * kPointerSize);
    } else {
      const int offset = JavaScriptFrameConstants::kLastParameterOffset;
      return frame->fp() + offset - ((slot_index + 1) * kPointerSize);
    }
  }

  static SlotRef ComputeSlotForNextArgument(TranslationIterator* iterator,
                                            DeoptimizationInputData* data,
                                            JavaScriptFrame* frame);

  static void ComputeSlotsForArguments(
      Vector<SlotRef>* args_slots,
      TranslationIterator* iterator,
      DeoptimizationInputData* data,
      JavaScriptFrame* frame);
};


#ifdef ENABLE_DEBUGGER_SUPPORT
// Class used to represent an unoptimized frame when the debugger
// needs to inspect a frame that is part of an optimized frame. The
// internally used FrameDescription objects are not GC safe so for use
// by the debugger frame information is copied to an object of this type.
// Represents parameters in unadapted form so their number might mismatch
// formal parameter count.
class DeoptimizedFrameInfo : public Malloced {
 public:
  DeoptimizedFrameInfo(Deoptimizer* deoptimizer,
                       int frame_index,
                       bool has_arguments_adaptor,
                       bool has_construct_stub);
  virtual ~DeoptimizedFrameInfo();

  // GC support.
  void Iterate(ObjectVisitor* v);

  // Return the number of incoming arguments.
  int parameters_count() { return parameters_count_; }

  // Return the height of the expression stack.
  int expression_count() { return expression_count_; }

  // Get the frame function.
  JSFunction* GetFunction() {
    return function_;
  }

  // Check if this frame is preceded by construct stub frame.  The bottom-most
  // inlined frame might still be called by an uninlined construct stub.
  bool HasConstructStub() {
    return has_construct_stub_;
  }

  // Get an incoming argument.
  Object* GetParameter(int index) {
    ASSERT(0 <= index && index < parameters_count());
    return parameters_[index];
  }

  // Get an expression from the expression stack.
  Object* GetExpression(int index) {
    ASSERT(0 <= index && index < expression_count());
    return expression_stack_[index];
  }

  int GetSourcePosition() {
    return source_position_;
  }

 private:
  // Set an incoming argument.
  void SetParameter(int index, Object* obj) {
    ASSERT(0 <= index && index < parameters_count());
    parameters_[index] = obj;
  }

  // Set an expression on the expression stack.
  void SetExpression(int index, Object* obj) {
    ASSERT(0 <= index && index < expression_count());
    expression_stack_[index] = obj;
  }

  JSFunction* function_;
  bool has_construct_stub_;
  int parameters_count_;
  int expression_count_;
  Object** parameters_;
  Object** expression_stack_;
  int source_position_;

  friend class Deoptimizer;
};
#endif

} }  // namespace v8::internal

#endif  // V8_DEOPTIMIZER_H_
