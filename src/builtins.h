// Copyright 2011 the V8 project authors. All rights reserved.
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

#ifndef V8_BUILTINS_H_
#define V8_BUILTINS_H_

namespace v8 {
namespace internal {

// Specifies extra arguments required by a C++ builtin.
enum BuiltinExtraArguments {
  NO_EXTRA_ARGUMENTS = 0,
  NEEDS_CALLED_FUNCTION = 1
};


#define CODE_AGE_LIST_WITH_ARG(V, A)     \
  V(Quadragenarian, A)                   \
  V(Quinquagenarian, A)                  \
  V(Sexagenarian, A)                     \
  V(Septuagenarian, A)                   \
  V(Octogenarian, A)

#define CODE_AGE_LIST_IGNORE_ARG(X, V) V(X)

#define CODE_AGE_LIST(V) \
  CODE_AGE_LIST_WITH_ARG(CODE_AGE_LIST_IGNORE_ARG, V)

#define CODE_AGE_LIST_COMPLETE(V)                  \
  V(NotExecuted)                                   \
  V(ExecutedOnce)                                  \
  V(NoAge)                                         \
  CODE_AGE_LIST_WITH_ARG(CODE_AGE_LIST_IGNORE_ARG, V)

#define DECLARE_CODE_AGE_BUILTIN(C, V)             \
  V(Make##C##CodeYoungAgainOddMarking, BUILTIN,    \
    UNINITIALIZED, Code::kNoExtraICState)          \
  V(Make##C##CodeYoungAgainEvenMarking, BUILTIN,   \
    UNINITIALIZED, Code::kNoExtraICState)


// Define list of builtins implemented in C++.
#define BUILTIN_LIST_C(V)                                           \
  V(Illegal, NO_EXTRA_ARGUMENTS)                                    \
                                                                    \
  V(EmptyFunction, NO_EXTRA_ARGUMENTS)                              \
                                                                    \
  V(ArrayPush, NO_EXTRA_ARGUMENTS)                                  \
  V(ArrayPop, NO_EXTRA_ARGUMENTS)                                   \
  V(ArrayShift, NO_EXTRA_ARGUMENTS)                                 \
  V(ArrayUnshift, NO_EXTRA_ARGUMENTS)                               \
  V(ArraySlice, NO_EXTRA_ARGUMENTS)                                 \
  V(ArraySplice, NO_EXTRA_ARGUMENTS)                                \
  V(ArrayConcat, NO_EXTRA_ARGUMENTS)                                \
                                                                    \
  V(HandleApiCall, NEEDS_CALLED_FUNCTION)                           \
  V(HandleApiCallConstruct, NEEDS_CALLED_FUNCTION)                  \
  V(HandleApiCallAsFunction, NO_EXTRA_ARGUMENTS)                    \
  V(HandleApiCallAsConstructor, NO_EXTRA_ARGUMENTS)                 \
                                                                    \
  V(StrictModePoisonPill, NO_EXTRA_ARGUMENTS)

// Define list of builtins implemented in assembly.
#define BUILTIN_LIST_A(V)                                               \
  V(ArgumentsAdaptorTrampoline,     BUILTIN, UNINITIALIZED,             \
                                    Code::kNoExtraICState)              \
  V(InRecompileQueue,               BUILTIN, UNINITIALIZED,             \
                                    Code::kNoExtraICState)              \
  V(JSConstructStubCountdown,       BUILTIN, UNINITIALIZED,             \
                                    Code::kNoExtraICState)              \
  V(JSConstructStubGeneric,         BUILTIN, UNINITIALIZED,             \
                                    Code::kNoExtraICState)              \
  V(JSConstructStubApi,             BUILTIN, UNINITIALIZED,             \
                                    Code::kNoExtraICState)              \
  V(JSEntryTrampoline,              BUILTIN, UNINITIALIZED,             \
                                    Code::kNoExtraICState)              \
  V(JSConstructEntryTrampoline,     BUILTIN, UNINITIALIZED,             \
                                    Code::kNoExtraICState)              \
  V(LazyCompile,                    BUILTIN, UNINITIALIZED,             \
                                    Code::kNoExtraICState)              \
  V(LazyRecompile,                  BUILTIN, UNINITIALIZED,             \
                                    Code::kNoExtraICState)              \
  V(ConcurrentRecompile,            BUILTIN, UNINITIALIZED,             \
                                    Code::kNoExtraICState)              \
  V(NotifyDeoptimized,              BUILTIN, UNINITIALIZED,             \
                                    Code::kNoExtraICState)              \
  V(NotifySoftDeoptimized,          BUILTIN, UNINITIALIZED,             \
                                    Code::kNoExtraICState)              \
  V(NotifyLazyDeoptimized,          BUILTIN, UNINITIALIZED,             \
                                    Code::kNoExtraICState)              \
  V(NotifyStubFailure,              BUILTIN, UNINITIALIZED,             \
                                    Code::kNoExtraICState)              \
                                                                        \
  V(LoadIC_Miss,                    BUILTIN, UNINITIALIZED,             \
                                    Code::kNoExtraICState)              \
  V(KeyedLoadIC_Miss,               BUILTIN, UNINITIALIZED,             \
                                    Code::kNoExtraICState)              \
  V(KeyedLoadIC_MissForceGeneric,   BUILTIN, UNINITIALIZED,             \
                                    Code::kNoExtraICState)              \
  V(StoreIC_Miss,                   BUILTIN, UNINITIALIZED,             \
                                    Code::kNoExtraICState)              \
  V(KeyedStoreIC_Miss,              BUILTIN, UNINITIALIZED,             \
                                    Code::kNoExtraICState)              \
  V(KeyedStoreIC_MissForceGeneric,  BUILTIN, UNINITIALIZED,             \
                                    Code::kNoExtraICState)              \
  V(LoadIC_Initialize,              LOAD_IC, UNINITIALIZED,             \
                                    Code::kNoExtraICState)              \
  V(LoadIC_PreMonomorphic,          LOAD_IC, PREMONOMORPHIC,            \
                                    Code::kNoExtraICState)              \
  V(LoadIC_Megamorphic,             LOAD_IC, MEGAMORPHIC,               \
                                    Code::kNoExtraICState)              \
  V(LoadIC_Getter_ForDeopt,         LOAD_IC, MONOMORPHIC,               \
                                    Code::kNoExtraICState)              \
                                                                        \
  V(KeyedLoadIC_Initialize,         KEYED_LOAD_IC, UNINITIALIZED,       \
                                    Code::kNoExtraICState)              \
  V(KeyedLoadIC_PreMonomorphic,     KEYED_LOAD_IC, PREMONOMORPHIC,      \
                                    Code::kNoExtraICState)              \
  V(KeyedLoadIC_Generic,            KEYED_LOAD_IC, GENERIC,             \
                                    Code::kNoExtraICState)              \
  V(KeyedLoadIC_String,             KEYED_LOAD_IC, MEGAMORPHIC,         \
                                    Code::kNoExtraICState)              \
  V(KeyedLoadIC_IndexedInterceptor, KEYED_LOAD_IC, MONOMORPHIC,         \
                                    Code::kNoExtraICState)              \
  V(KeyedLoadIC_NonStrictArguments, KEYED_LOAD_IC, MONOMORPHIC,         \
                                    Code::kNoExtraICState)              \
                                                                        \
  V(StoreIC_Initialize,             STORE_IC, UNINITIALIZED,            \
                                    Code::kNoExtraICState)              \
  V(StoreIC_PreMonomorphic,         STORE_IC, PREMONOMORPHIC,           \
                                    Code::kNoExtraICState)              \
  V(StoreIC_Megamorphic,            STORE_IC, MEGAMORPHIC,              \
                                    Code::kNoExtraICState)              \
  V(StoreIC_Generic,                STORE_IC, GENERIC,                  \
                                    Code::kNoExtraICState)              \
  V(StoreIC_Generic_Strict,         STORE_IC, GENERIC,                  \
                                    kStrictMode)                        \
  V(StoreIC_GlobalProxy,            STORE_IC, GENERIC,                  \
                                    Code::kNoExtraICState)              \
  V(StoreIC_Initialize_Strict,      STORE_IC, UNINITIALIZED,            \
                                    kStrictMode)                        \
  V(StoreIC_PreMonomorphic_Strict,  STORE_IC, PREMONOMORPHIC,           \
                                    kStrictMode)                        \
  V(StoreIC_Megamorphic_Strict,     STORE_IC, MEGAMORPHIC,              \
                                    kStrictMode)                        \
  V(StoreIC_GlobalProxy_Strict,     STORE_IC, GENERIC,                  \
                                    kStrictMode)                        \
  V(StoreIC_Setter_ForDeopt,        STORE_IC, MONOMORPHIC,              \
                                    kStrictMode)                        \
                                                                        \
  V(KeyedStoreIC_Initialize,        KEYED_STORE_IC, UNINITIALIZED,      \
                                    Code::kNoExtraICState)              \
  V(KeyedStoreIC_PreMonomorphic,    KEYED_STORE_IC, PREMONOMORPHIC,     \
                                    Code::kNoExtraICState)              \
  V(KeyedStoreIC_Generic,           KEYED_STORE_IC, GENERIC,            \
                                    Code::kNoExtraICState)              \
                                                                        \
  V(KeyedStoreIC_Initialize_Strict, KEYED_STORE_IC, UNINITIALIZED,      \
                                    kStrictMode)                        \
  V(KeyedStoreIC_PreMonomorphic_Strict, KEYED_STORE_IC, PREMONOMORPHIC, \
                                    kStrictMode)                        \
  V(KeyedStoreIC_Generic_Strict,    KEYED_STORE_IC, GENERIC,            \
                                    kStrictMode)                        \
  V(KeyedStoreIC_NonStrictArguments, KEYED_STORE_IC, MONOMORPHIC,       \
                                     Code::kNoExtraICState)             \
                                                                        \
  /* Uses KeyedLoadIC_Initialize; must be after in list. */             \
  V(FunctionCall,                   BUILTIN, UNINITIALIZED,             \
                                    Code::kNoExtraICState)              \
  V(FunctionApply,                  BUILTIN, UNINITIALIZED,             \
                                    Code::kNoExtraICState)              \
                                                                        \
  V(InternalArrayCode,              BUILTIN, UNINITIALIZED,             \
                                    Code::kNoExtraICState)              \
  V(ArrayCode,                      BUILTIN, UNINITIALIZED,             \
                                    Code::kNoExtraICState)              \
                                                                        \
  V(StringConstructCode,            BUILTIN, UNINITIALIZED,             \
                                    Code::kNoExtraICState)              \
                                                                        \
  V(OnStackReplacement,             BUILTIN, UNINITIALIZED,             \
                                    Code::kNoExtraICState)              \
  V(InterruptCheck,                 BUILTIN, UNINITIALIZED,             \
                                    Code::kNoExtraICState)              \
  V(OsrAfterStackCheck,             BUILTIN, UNINITIALIZED,             \
                                    Code::kNoExtraICState)              \
  V(StackCheck,                     BUILTIN, UNINITIALIZED,             \
                                    Code::kNoExtraICState)              \
                                                                        \
  V(MarkCodeAsExecutedOnce,         BUILTIN, UNINITIALIZED,             \
                                    Code::kNoExtraICState)              \
  V(MarkCodeAsExecutedTwice,        BUILTIN, UNINITIALIZED,             \
                                    Code::kNoExtraICState)              \
  CODE_AGE_LIST_WITH_ARG(DECLARE_CODE_AGE_BUILTIN, V)

// Define list of builtin handlers implemented in assembly.
#define BUILTIN_LIST_H(V)                                                 \
  V(LoadIC_Slow,                    LOAD_IC, Code::kNoExtraICState)       \
  V(KeyedLoadIC_Slow,               KEYED_LOAD_IC, Code::kNoExtraICState) \
  V(StoreIC_Slow,                   STORE_IC, Code::kNoExtraICState)      \
  V(StoreIC_Slow_Strict,            STORE_IC, kStrictMode)                \
  V(KeyedStoreIC_Slow,              KEYED_STORE_IC, Code::kNoExtraICState)\
  V(KeyedStoreIC_Slow_Strict,       KEYED_STORE_IC, kStrictMode)          \
  V(LoadIC_Normal,                  LOAD_IC, Code::kNoExtraICState)       \
  V(StoreIC_Normal,                 STORE_IC, Code::kNoExtraICState)      \
  V(StoreIC_Normal_Strict,          STORE_IC, kStrictMode)

#ifdef ENABLE_DEBUGGER_SUPPORT
// Define list of builtins used by the debugger implemented in assembly.
#define BUILTIN_LIST_DEBUG_A(V)                                               \
  V(Return_DebugBreak,                         BUILTIN, DEBUG_STUB,           \
                                               DEBUG_BREAK)                   \
  V(CallFunctionStub_DebugBreak,               BUILTIN, DEBUG_STUB,           \
                                               DEBUG_BREAK)                   \
  V(CallFunctionStub_Recording_DebugBreak,     BUILTIN, DEBUG_STUB,           \
                                               DEBUG_BREAK)                   \
  V(CallConstructStub_DebugBreak,              BUILTIN, DEBUG_STUB,           \
                                               DEBUG_BREAK)                   \
  V(CallConstructStub_Recording_DebugBreak,    BUILTIN, DEBUG_STUB,           \
                                               DEBUG_BREAK)                   \
  V(LoadIC_DebugBreak,                         LOAD_IC, DEBUG_STUB,           \
                                               DEBUG_BREAK)                   \
  V(KeyedLoadIC_DebugBreak,                    KEYED_LOAD_IC, DEBUG_STUB,     \
                                               DEBUG_BREAK)                   \
  V(StoreIC_DebugBreak,                        STORE_IC, DEBUG_STUB,          \
                                               DEBUG_BREAK)                   \
  V(KeyedStoreIC_DebugBreak,                   KEYED_STORE_IC, DEBUG_STUB,    \
                                               DEBUG_BREAK)                   \
  V(CompareNilIC_DebugBreak,                   COMPARE_NIL_IC, DEBUG_STUB,    \
                                               DEBUG_BREAK)                   \
  V(Slot_DebugBreak,                           BUILTIN, DEBUG_STUB,           \
                                               DEBUG_BREAK)                   \
  V(PlainReturn_LiveEdit,                      BUILTIN, DEBUG_STUB,           \
                                               DEBUG_BREAK)                   \
  V(FrameDropper_LiveEdit,                     BUILTIN, DEBUG_STUB,           \
                                               DEBUG_BREAK)
#else
#define BUILTIN_LIST_DEBUG_A(V)
#endif

// Define list of builtins implemented in JavaScript.
#define BUILTINS_LIST_JS(V)              \
  V(EQUALS, 1)                           \
  V(STRICT_EQUALS, 1)                    \
  V(COMPARE, 2)                          \
  V(ADD, 1)                              \
  V(SUB, 1)                              \
  V(MUL, 1)                              \
  V(DIV, 1)                              \
  V(MOD, 1)                              \
  V(BIT_OR, 1)                           \
  V(BIT_AND, 1)                          \
  V(BIT_XOR, 1)                          \
  V(SHL, 1)                              \
  V(SAR, 1)                              \
  V(SHR, 1)                              \
  V(DELETE, 2)                           \
  V(IN, 1)                               \
  V(INSTANCE_OF, 1)                      \
  V(FILTER_KEY, 1)                       \
  V(CALL_NON_FUNCTION, 0)                \
  V(CALL_NON_FUNCTION_AS_CONSTRUCTOR, 0) \
  V(CALL_FUNCTION_PROXY, 1)                \
  V(CALL_FUNCTION_PROXY_AS_CONSTRUCTOR, 1) \
  V(TO_OBJECT, 0)                        \
  V(TO_NUMBER, 0)                        \
  V(TO_STRING, 0)                        \
  V(STRING_ADD_LEFT, 1)                  \
  V(STRING_ADD_RIGHT, 1)                 \
  V(APPLY_PREPARE, 1)                    \
  V(APPLY_OVERFLOW, 1)

class BuiltinFunctionTable;
class ObjectVisitor;


class Builtins {
 public:
  ~Builtins();

  // Generate all builtin code objects. Should be called once during
  // isolate initialization.
  void SetUp(Isolate* isolate, bool create_heap_objects);
  void TearDown();

  // Garbage collection support.
  void IterateBuiltins(ObjectVisitor* v);

  // Disassembler support.
  const char* Lookup(byte* pc);

  enum Name {
#define DEF_ENUM_C(name, ignore) k##name,
#define DEF_ENUM_A(name, kind, state, extra) k##name,
#define DEF_ENUM_H(name, kind, extra) k##name,
    BUILTIN_LIST_C(DEF_ENUM_C)
    BUILTIN_LIST_A(DEF_ENUM_A)
    BUILTIN_LIST_H(DEF_ENUM_H)
    BUILTIN_LIST_DEBUG_A(DEF_ENUM_A)
#undef DEF_ENUM_C
#undef DEF_ENUM_A
    builtin_count
  };

  enum CFunctionId {
#define DEF_ENUM_C(name, ignore) c_##name,
    BUILTIN_LIST_C(DEF_ENUM_C)
#undef DEF_ENUM_C
    cfunction_count
  };

  enum JavaScript {
#define DEF_ENUM(name, ignore) name,
    BUILTINS_LIST_JS(DEF_ENUM)
#undef DEF_ENUM
    id_count
  };

#define DECLARE_BUILTIN_ACCESSOR_C(name, ignore) Handle<Code> name();
#define DECLARE_BUILTIN_ACCESSOR_A(name, kind, state, extra) \
  Handle<Code> name();
#define DECLARE_BUILTIN_ACCESSOR_H(name, kind, extra) Handle<Code> name();
  BUILTIN_LIST_C(DECLARE_BUILTIN_ACCESSOR_C)
  BUILTIN_LIST_A(DECLARE_BUILTIN_ACCESSOR_A)
  BUILTIN_LIST_H(DECLARE_BUILTIN_ACCESSOR_H)
  BUILTIN_LIST_DEBUG_A(DECLARE_BUILTIN_ACCESSOR_A)
#undef DECLARE_BUILTIN_ACCESSOR_C
#undef DECLARE_BUILTIN_ACCESSOR_A

  Code* builtin(Name name) {
    // Code::cast cannot be used here since we access builtins
    // during the marking phase of mark sweep. See IC::Clear.
    return reinterpret_cast<Code*>(builtins_[name]);
  }

  Address builtin_address(Name name) {
    return reinterpret_cast<Address>(&builtins_[name]);
  }

  static Address c_function_address(CFunctionId id) {
    return c_functions_[id];
  }

  static const char* GetName(JavaScript id) { return javascript_names_[id]; }
  const char* name(int index) {
    ASSERT(index >= 0);
    ASSERT(index < builtin_count);
    return names_[index];
  }
  static int GetArgumentsCount(JavaScript id) { return javascript_argc_[id]; }
  Handle<Code> GetCode(JavaScript id, bool* resolved);
  static int NumberOfJavaScriptBuiltins() { return id_count; }

  bool is_initialized() const { return initialized_; }

 private:
  Builtins();

  // The external C++ functions called from the code.
  static Address const c_functions_[cfunction_count];

  // Note: These are always Code objects, but to conform with
  // IterateBuiltins() above which assumes Object**'s for the callback
  // function f, we use an Object* array here.
  Object* builtins_[builtin_count];
  const char* names_[builtin_count];
  static const char* const javascript_names_[id_count];
  static int const javascript_argc_[id_count];

  static void Generate_Adaptor(MacroAssembler* masm,
                               CFunctionId id,
                               BuiltinExtraArguments extra_args);
  static void Generate_InRecompileQueue(MacroAssembler* masm);
  static void Generate_ConcurrentRecompile(MacroAssembler* masm);
  static void Generate_JSConstructStubCountdown(MacroAssembler* masm);
  static void Generate_JSConstructStubGeneric(MacroAssembler* masm);
  static void Generate_JSConstructStubApi(MacroAssembler* masm);
  static void Generate_JSEntryTrampoline(MacroAssembler* masm);
  static void Generate_JSConstructEntryTrampoline(MacroAssembler* masm);
  static void Generate_LazyCompile(MacroAssembler* masm);
  static void Generate_LazyRecompile(MacroAssembler* masm);
  static void Generate_NotifyDeoptimized(MacroAssembler* masm);
  static void Generate_NotifySoftDeoptimized(MacroAssembler* masm);
  static void Generate_NotifyLazyDeoptimized(MacroAssembler* masm);
  static void Generate_NotifyStubFailure(MacroAssembler* masm);
  static void Generate_ArgumentsAdaptorTrampoline(MacroAssembler* masm);

  static void Generate_FunctionCall(MacroAssembler* masm);
  static void Generate_FunctionApply(MacroAssembler* masm);

  static void Generate_InternalArrayCode(MacroAssembler* masm);
  static void Generate_ArrayCode(MacroAssembler* masm);

  static void Generate_StringConstructCode(MacroAssembler* masm);
  static void Generate_OnStackReplacement(MacroAssembler* masm);
  static void Generate_OsrAfterStackCheck(MacroAssembler* masm);
  static void Generate_InterruptCheck(MacroAssembler* masm);
  static void Generate_StackCheck(MacroAssembler* masm);

#define DECLARE_CODE_AGE_BUILTIN_GENERATOR(C)                \
  static void Generate_Make##C##CodeYoungAgainEvenMarking(   \
      MacroAssembler* masm);                                 \
  static void Generate_Make##C##CodeYoungAgainOddMarking(    \
      MacroAssembler* masm);
  CODE_AGE_LIST(DECLARE_CODE_AGE_BUILTIN_GENERATOR)
#undef DECLARE_CODE_AGE_BUILTIN_GENERATOR

  static void Generate_MarkCodeAsExecutedOnce(MacroAssembler* masm);
  static void Generate_MarkCodeAsExecutedTwice(MacroAssembler* masm);

  static void InitBuiltinFunctionTable();

  bool initialized_;

  friend class BuiltinFunctionTable;
  friend class Isolate;

  DISALLOW_COPY_AND_ASSIGN(Builtins);
};

} }  // namespace v8::internal

#endif  // V8_BUILTINS_H_
