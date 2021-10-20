/*
 * Copyright (c) 2020, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2019, Arm Limited. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

#include "precompiled.hpp"
#include "asm/macroAssembler.hpp"
#include "code/codeBlob.hpp"
#include "code/codeCache.hpp"
#include "code/vmreg.inline.hpp"
#include "compiler/oopMap.hpp"
#include "logging/logStream.hpp"
#include "memory/resourceArea.hpp"
#include "prims/universalNativeInvoker.hpp"

#define __ _masm->

void ProgrammableInvoker::Generator::generate() {
  __ enter();

  // Name registers used in the stub code. These are all caller-save so
  // may be clobbered by the call to the native function. Avoid using
  // rscratch1 here as it's r8 which is the indirect result register in
  // the standard ABI.
  Register Rctx = r10, Rstack_size = r11;
  Register Rwords = r12, Rtmp = r13;
  Register Rsrc_ptr = r14, Rdst_ptr = r15;

  assert_different_registers(Rctx, Rstack_size, rscratch1, rscratch2);

  // TODO: if the callee is not using the standard C ABI then we need to
  //       preserve more registers here.

  __ block_comment("init_and_alloc_stack");

  __ mov(Rctx, c_rarg0);
  __ str(Rctx, Address(__ pre(sp, -2 * wordSize)));

  assert(_abi->_stack_alignment_bytes % 16 == 0, "stack must be 16 byte aligned");

  __ block_comment("allocate_stack");
  __ ldr(Rstack_size, Address(Rctx, (int) _layout->stack_args_bytes));
  __ add(rscratch2, Rstack_size, _abi->_stack_alignment_bytes - 1);
  __ andr(rscratch2, rscratch2, -_abi->_stack_alignment_bytes);
  __ sub(sp, sp, rscratch2);

  __ block_comment("load_arguments");

  __ ldr(Rsrc_ptr, Address(Rctx, (int) _layout->stack_args));
  __ lsr(Rwords, Rstack_size, LogBytesPerWord);
  __ mov(Rdst_ptr, sp);

  Label Ldone, Lnext;
  __ bind(Lnext);
  __ cbz(Rwords, Ldone);
  __ ldr(Rtmp, __ post(Rsrc_ptr, wordSize));
  __ str(Rtmp, __ post(Rdst_ptr, wordSize));
  __ sub(Rwords, Rwords, 1);
  __ b(Lnext);
  __ bind(Ldone);

  for (int i = 0; i < _abi->_vector_argument_registers.length(); i++) {
    ssize_t offs = _layout->arguments_vector + i * float_reg_size;
    __ ldrq(_abi->_vector_argument_registers.at(i), Address(Rctx, offs));
  }

  for (int i = 0; i < _abi->_integer_argument_registers.length(); i++) {
    ssize_t offs = _layout->arguments_integer + i * sizeof(uintptr_t);
    __ ldr(_abi->_integer_argument_registers.at(i), Address(Rctx, offs));
  }

  assert(_abi->_shadow_space_bytes == 0, "shadow space not supported on AArch64");

  // call target function
  __ block_comment("call target function");
  __ ldr(rscratch2, Address(Rctx, (int) _layout->arguments_next_pc));
  __ blr(rscratch2);

  __ ldr(Rctx, Address(rfp, -2 * wordSize));   // Might have clobbered Rctx

  __ block_comment("store_registers");

  for (int i = 0; i < _abi->_integer_return_registers.length(); i++) {
    ssize_t offs = _layout->returns_integer + i * sizeof(uintptr_t);
    __ str(_abi->_integer_return_registers.at(i), Address(Rctx, offs));
  }

  for (int i = 0; i < _abi->_vector_return_registers.length(); i++) {
    ssize_t offs = _layout->returns_vector + i * float_reg_size;
    __ strq(_abi->_vector_return_registers.at(i), Address(Rctx, offs));
  }

  __ leave();
  __ ret(lr);

  __ flush();
}

address ProgrammableInvoker::generate_adapter(jobject jabi, jobject jlayout) {
  ResourceMark rm;
  const ABIDescriptor abi = ForeignGlobals::parse_abi_descriptor(jabi);
  const BufferLayout layout = ForeignGlobals::parse_buffer_layout(jlayout);

  BufferBlob* _invoke_native_blob = BufferBlob::create("invoke_native_blob", native_invoker_size);

  CodeBuffer code2(_invoke_native_blob);
  ProgrammableInvoker::Generator g2(&code2, &abi, &layout);
  g2.generate();
  code2.log_section_sizes("InvokeNativeBlob");

  return _invoke_native_blob->code_begin();
}

// ---------------------------------------------------------------

class NativeInvokerGenerator : public StubCodeGenerator {
  BasicType* _signature;
  int _num_args;
  BasicType _ret_bt;
  int _shadow_space_bytes;

  const GrowableArray<VMReg>& _input_registers;
  const GrowableArray<VMReg>& _output_registers;

  int _frame_complete;
  int _framesize;
  OopMapSet* _oop_maps;
public:
  NativeInvokerGenerator(CodeBuffer* buffer,
                         BasicType* signature,
                         int num_args,
                         BasicType ret_bt,
                         int shadow_space_bytes,
                         const GrowableArray<VMReg>& input_registers,
                         const GrowableArray<VMReg>& output_registers)
   : StubCodeGenerator(buffer, PrintMethodHandleStubs),
     _signature(signature),
     _num_args(num_args),
     _ret_bt(ret_bt),
     _shadow_space_bytes(shadow_space_bytes),
     _input_registers(input_registers),
     _output_registers(output_registers),
     _frame_complete(0),
     _framesize(0),
     _oop_maps(NULL) {
    assert(_output_registers.length() <= 1
           || (_output_registers.length() == 2 && !_output_registers.at(1)->is_valid()), "no multi-reg returns");
  }

  void generate();

  int frame_complete() const {
    return _frame_complete;
  }

  int framesize() const {
    return (_framesize >> (LogBytesPerWord - LogBytesPerInt));
  }

  OopMapSet* oop_maps() const {
    return _oop_maps;
  }

private:
#ifdef ASSERT
  bool target_uses_register(VMReg reg) {
    return _input_registers.contains(reg) || _output_registers.contains(reg);
  }
#endif
};

static const int native_invoker_code_size = 1024;

RuntimeStub* ProgrammableInvoker::make_native_invoker(BasicType* signature,
                                                      int num_args,
                                                      BasicType ret_bt,
                                                      int shadow_space_bytes,
                                                      const GrowableArray<VMReg>& input_registers,
                                                      const GrowableArray<VMReg>& output_registers) {
  int locs_size  = 64;
  CodeBuffer code("nep_invoker_blob", native_invoker_code_size, locs_size);
  NativeInvokerGenerator g(&code, signature, num_args, ret_bt, shadow_space_bytes, input_registers, output_registers);
  g.generate();
  code.log_section_sizes("nep_invoker_blob");

  RuntimeStub* stub =
    RuntimeStub::new_runtime_stub("nep_invoker_blob",
                                  &code,
                                  g.frame_complete(),
                                  g.framesize(),
                                  g.oop_maps(), false);

  if (TraceNativeInvokers) {
    stub->print_on(tty);
  }

  return stub;
}

void NativeInvokerGenerator::generate() {
  // we can't use rscratch1 because it is r8, and used by the ABI
  Register tmp1 = r9;
  Register tmp2 = r10;
  assert(!target_uses_register(tmp1->as_VMReg()), "conflict");
  assert(!target_uses_register(tmp2->as_VMReg()), "conflict");
  assert(!target_uses_register(rthread->as_VMReg()), "conflict");

  enum layout {
    rfp_off,
    rfp_off2,
    lr_off,
    lr_off2,
    framesize // inclusive of return address
    // The following are also computed dynamically:
    // spill area for return value
    // out arg area (e.g. for stack args)
  };

  Register input_addr_reg = tmp1;
  Register shuffle_reg = r19;
  JavaCallConv in_conv;
  DowncallNativeCallConv out_conv(_input_registers, input_addr_reg->as_VMReg());
  ArgumentShuffle arg_shuffle(_signature, _num_args, _signature, _num_args, &in_conv, &out_conv, shuffle_reg->as_VMReg());

#ifdef ASSERT
  LogTarget(Trace, panama) lt;
  if (lt.is_enabled()) {
    ResourceMark rm;
    LogStream ls(lt);
    arg_shuffle.print_on(&ls);
  }
#endif

  RegSpiller out_reg_spiller(_output_registers);
  int spill_offset = 0;

  assert(_shadow_space_bytes == 0, "not expecting shadow space on AArch64");
  _framesize = align_up(framesize
    + (out_reg_spiller.spill_size_bytes() >> LogBytesPerInt)
    + arg_shuffle.out_arg_stack_slots(), 4);
  assert(is_even(_framesize/2), "sp not 16-byte aligned");

  _oop_maps  = new OopMapSet();
  MacroAssembler* masm = _masm;

  address start = __ pc();

  __ enter();

  // lr and fp are already in place
  __ sub(sp, rfp, ((unsigned)_framesize-4) << LogBytesPerInt); // prolog

  _frame_complete = __ pc() - start;

  address the_pc = __ pc();
  __ set_last_Java_frame(sp, rfp, the_pc, tmp1);
  OopMap* map = new OopMap(_framesize, 0);
  _oop_maps->add_gc_map(the_pc - start, map);

  // State transition
  __ mov(tmp1, _thread_in_native);
  __ lea(tmp2, Address(rthread, JavaThread::thread_state_offset()));
  __ stlrw(tmp1, tmp2);

  __ block_comment("{ argument shuffle");
  arg_shuffle.generate(_masm, shuffle_reg->as_VMReg(), 0, _shadow_space_bytes);
  __ block_comment("} argument shuffle");

  __ blr(input_addr_reg);

  // Unpack native results.
  switch (_ret_bt) {
    case T_BOOLEAN: __ c2bool(r0);                     break;
    case T_CHAR   : __ ubfx(r0, r0, 0, 16);            break;
    case T_BYTE   : __ sbfx(r0, r0, 0, 8);             break;
    case T_SHORT  : __ sbfx(r0, r0, 0, 16);            break;
    case T_INT    : __ sbfx(r0, r0, 0, 32);            break;
    case T_DOUBLE :
    case T_FLOAT  :
      // Result is in v0 we'll save as needed
      break;
    case T_VOID: break;
    case T_LONG: break;
    default       : ShouldNotReachHere();
  }

  __ mov(tmp1, _thread_in_native_trans);
  __ strw(tmp1, Address(rthread, JavaThread::thread_state_offset()));

  // Force this write out before the read below
  __ membar(Assembler::LoadLoad | Assembler::LoadStore |
            Assembler::StoreLoad | Assembler::StoreStore);

  __ verify_sve_vector_length(tmp1);

  Label L_after_safepoint_poll;
  Label L_safepoint_poll_slow_path;

  __ safepoint_poll(L_safepoint_poll_slow_path, true /* at_return */, true /* acquire */, false /* in_nmethod */, tmp1);

  __ ldrw(tmp1, Address(rthread, JavaThread::suspend_flags_offset()));
  __ cbnzw(tmp1, L_safepoint_poll_slow_path);

  __ bind(L_after_safepoint_poll);

  // change thread state
  __ mov(tmp1, _thread_in_Java);
  __ lea(tmp2, Address(rthread, JavaThread::thread_state_offset()));
  __ stlrw(tmp1, tmp2);

  __ block_comment("reguard stack check");
  Label L_reguard;
  Label L_after_reguard;
  __ ldrb(tmp1, Address(rthread, JavaThread::stack_guard_state_offset()));
  __ cmpw(tmp1, StackOverflow::stack_guard_yellow_reserved_disabled);
  __ br(Assembler::EQ, L_reguard);
  __ bind(L_after_reguard);

  __ reset_last_Java_frame(true);

  __ leave(); // required for proper stackwalking of RuntimeStub frame
  __ ret(lr);

  //////////////////////////////////////////////////////////////////////////////

  __ block_comment("{ L_safepoint_poll_slow_path");
  __ bind(L_safepoint_poll_slow_path);

  // Need to save the native result registers around any runtime calls.
  out_reg_spiller.generate_spill(_masm, spill_offset);

  __ mov(c_rarg0, rthread);
  assert(frame::arg_reg_save_area_bytes == 0, "not expecting frame reg save area");
  __ lea(tmp1, RuntimeAddress(CAST_FROM_FN_PTR(address, JavaThread::check_special_condition_for_native_trans)));
  __ blr(tmp1);

  out_reg_spiller.generate_fill(_masm, spill_offset);

  __ b(L_after_safepoint_poll);
  __ block_comment("} L_safepoint_poll_slow_path");

  //////////////////////////////////////////////////////////////////////////////

  __ block_comment("{ L_reguard");
  __ bind(L_reguard);

  out_reg_spiller.generate_spill(_masm, spill_offset);

  __ rt_call(CAST_FROM_FN_PTR(address, SharedRuntime::reguard_yellow_pages), tmp1);

  out_reg_spiller.generate_fill(_masm, spill_offset);

  __ b(L_after_reguard);

  __ block_comment("} L_reguard");

  //////////////////////////////////////////////////////////////////////////////

  __ flush();
}

bool ProgrammableInvoker::supports_native_invoker() {
  return true;
}
