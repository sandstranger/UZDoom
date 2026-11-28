/*
** jit_call.cpp
**
** ZScript JIT for AArch64 (see jitintern.h)
**
**---------------------------------------------------------------------------
**
** Copyright 2018 Magnus Norddahl
** Copyright 2018-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** Code written prior to 2026 is also licensed under:
**
** SPDX-License-Identifier: BSD-3-Clause
**
**---------------------------------------------------------------------------
**
*/

#include "jitintern.h"

/////////////////////////////////////////////////////////////////////////////
// Direct native calls
//
// JIT code writes the arguments of a direct native call into a JitNativeCallArgs block,
// already sorted the way AAPCS64 passes them, and calls one of these wrappers. A wrapper
// passes them on by calling the target through a function type with all eight X and all
// eight D argument registers (and 16 stack slots if any argument goes on the stack): the
// target only reads the registers and slots its own parameters use. Ints only need their
// low 32 bits, AAPCS64 leaves the upper half of such an argument unspecified. The C++
// try/catch here is what keeps exceptions out of the JIT frame that called us.

typedef uint64_t(*JitNativeFuncX)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
	double, double, double, double, double, double, double, double);
typedef double(*JitNativeFuncD)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
	double, double, double, double, double, double, double, double);
typedef uint64_t(*JitNativeFuncXS)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
	double, double, double, double, double, double, double, double,
	uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
	uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
typedef double(*JitNativeFuncDS)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
	double, double, double, double, double, double, double, double,
	uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
	uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

static_assert(JitNativeCallArgs::MaxStackArgs == 16, "the wrappers pass 16 stack slots");

#define JIT_NATIVE_REG_ARGS(a) \
	a->x[0], a->x[1], a->x[2], a->x[3], a->x[4], a->x[5], a->x[6], a->x[7], \
	a->d[0], a->d[1], a->d[2], a->d[3], a->d[4], a->d[5], a->d[6], a->d[7]

#define JIT_NATIVE_STACK_ARGS(a) \
	a->stack[0], a->stack[1], a->stack[2], a->stack[3], a->stack[4], a->stack[5], a->stack[6], a->stack[7], \
	a->stack[8], a->stack[9], a->stack[10], a->stack[11], a->stack[12], a->stack[13], a->stack[14], a->stack[15]

static int JitCallNativeX(JitNativeCallArgs *a)
{
	try
	{
		a->retx = reinterpret_cast<JitNativeFuncX>(a->target)(JIT_NATIVE_REG_ARGS(a));
		return 0;
	}
	catch (...)
	{
		JitStashException();
		return -1;
	}
}

static int JitCallNativeD(JitNativeCallArgs *a)
{
	try
	{
		a->retd = reinterpret_cast<JitNativeFuncD>(a->target)(JIT_NATIVE_REG_ARGS(a));
		return 0;
	}
	catch (...)
	{
		JitStashException();
		return -1;
	}
}

static int JitCallNativeXS(JitNativeCallArgs *a)
{
	try
	{
		a->retx = reinterpret_cast<JitNativeFuncXS>(a->target)(JIT_NATIVE_REG_ARGS(a), JIT_NATIVE_STACK_ARGS(a));
		return 0;
	}
	catch (...)
	{
		JitStashException();
		return -1;
	}
}

static int JitCallNativeDS(JitNativeCallArgs *a)
{
	try
	{
		a->retd = reinterpret_cast<JitNativeFuncDS>(a->target)(JIT_NATIVE_REG_ARGS(a), JIT_NATIVE_STACK_ARGS(a));
		return 0;
	}
	catch (...)
	{
		JitStashException();
		return -1;
	}
}

/////////////////////////////////////////////////////////////////////////////

void JitCompiler::EmitPARAM()
{
	ParamOpcodes.Push(pc);
}

void JitCompiler::EmitPARAMI()
{
	ParamOpcodes.Push(pc);
}

void JitCompiler::EmitRESULT()
{
	// This instruction is just a placeholder to indicate where a return
	// value should be stored. It does nothing on its own and should not
	// be executed.
}

void JitCompiler::EmitVTBL()
{
	// This instruction is handled in the CALL/CALL_K instruction following it
}

void JitCompiler::EmitVtbl(const VMOP *op)
{
	int a = op->a;
	int b = op->b;
	int c = op->c;

	auto label = EmitThrowExceptionLabel(X_READ_NIL);
	cc.cbz(regA[b], label);

	cc.ldr(regA[a], MemOffset(regA[b], myoffsetof(DObject, Class), 8));
	cc.ldr(regA[a], MemOffset(regA[a], myoffsetof(PClass, Virtuals) + myoffsetof(FArray, Array), 8));
	cc.ldr(regA[a], MemOffset(regA[a], c * (int)sizeof(void*), 8));
}

void JitCompiler::EmitCALL()
{
	EmitVMCall(regA[A], nullptr);
	pc += C; // Skip RESULTs
}

void JitCompiler::EmitCALL_K()
{
	VMFunction *target = static_cast<VMFunction*>(konsta[A].v);

	VMNativeFunction *ntarget = nullptr;
	if (target && (target->VarFlags & VARF_Native))
		ntarget = static_cast<VMNativeFunction *>(target);

	if (ntarget && ntarget->DirectNativeCall && CanDirectNativeCall(ntarget))
	{
		EmitNativeCall(ntarget);
	}
	else
	{
		auto ptr = LoadConstPtr(target);
		EmitVMCall(ptr, target);
	}

	pc += C; // Skip RESULTs
}

void JitCompiler::EmitVMCall(Gp vmfunc, VMFunction *target)
{
	using namespace asmjit;

	CheckVMFrame();

	int numparams = StoreCallParams();
	if (numparams != B)
		I_Error("OP_CALL parameter count does not match the number of preceding OP_PARAM instructions");

	if (pc > sfunc->Code && (pc - 1)->op == OP_VTBL)
		EmitVtbl(pc - 1);

	FillReturns(pc + 1, C);

	Gp paramsptr = newTempIntPtr();
	EmitAddOffset(paramsptr, vmframe, offsetParams);

	auto jitcall = newTempIntPtr();
	cc.ldr(jitcall, MemOffset(vmfunc, myoffsetof(VMFunction, JitCall), 8));

	auto result = newResultInt32();
	InvokeNode *call = nullptr;
	cc.invoke(Out<InvokeNode*>(call), jitcall, FuncSignature::build<int, VMFunction *, VMValue*, int, VMReturn*, int>());
	call->set_ret(0, result);
	call->set_arg(0, vmfunc);
	call->set_arg(1, paramsptr);
	call->set_arg(2, Imm(B));
	call->set_arg(3, GetCallReturns());
	call->set_arg(4, Imm(C));
	call->set_inline_comment(target ? target->PrintableName : "VMCall");

	// A negative result means the callee failed and left its exception with JitStashException.
	// Not TBNZ: it only reaches +-32 KB, and the error exit is at the end of the function.
	cc.cmp(result, Imm(0));
	cc.b_lt(ErrorExitLabel());

	LoadInOuts();
	LoadReturns(pc + 1, C);

	ParamOpcodes.Clear();
}

int JitCompiler::StoreCallParams()
{
	using namespace asmjit;

	Gp stackPtr = newTempIntPtr();
	Gp tmp = newTempIntPtr();
	Vec tmp2 = newTempFloat64();

	auto paramOffset = [&](int slot, size_t member) { return offsetParams + slot * (int)sizeof(VMValue) + (int)member; };

	int numparams = 0;
	for (unsigned int i = 0; i < ParamOpcodes.Size(); i++)
	{
		int slot = numparams++;

		if (ParamOpcodes[i]->op == OP_PARAMI)
		{
			int abcs = ParamOpcodes[i]->i24;
			cc.str(LoadConstInt32(abcs), MemOffset(vmframe, paramOffset(slot, myoffsetof(VMValue, i)), 4));
			continue;
		}

		int bc = ParamOpcodes[i]->i16u;

		switch (ParamOpcodes[i]->a)
		{
		case REGT_NIL:
			cc.mov(tmp, Imm(0));
			cc.str(tmp, MemOffset(vmframe, paramOffset(slot, myoffsetof(VMValue, a)), 8));
			break;
		case REGT_INT:
			cc.str(regD[bc], MemOffset(vmframe, paramOffset(slot, myoffsetof(VMValue, i)), 4));
			break;
		case REGT_INT | REGT_ADDROF:
			EmitAddOffset(stackPtr, vmframe, offsetD + (int)(bc * sizeof(int32_t)));
			cc.str(regD[bc], a64::ptr(stackPtr));
			cc.str(stackPtr, MemOffset(vmframe, paramOffset(slot, myoffsetof(VMValue, a)), 8));
			break;
		case REGT_INT | REGT_KONST:
			cc.str(LoadConstInt32(konstd[bc]), MemOffset(vmframe, paramOffset(slot, myoffsetof(VMValue, i)), 4));
			break;
		case REGT_STRING:
			cc.str(regS[bc], MemOffset(vmframe, paramOffset(slot, myoffsetof(VMValue, sp)), 8));
			break;
		case REGT_STRING | REGT_ADDROF:
			cc.str(regS[bc], MemOffset(vmframe, paramOffset(slot, myoffsetof(VMValue, a)), 8));
			break;
		case REGT_STRING | REGT_KONST:
			cc.mov(tmp, Imm((uint64_t)(uintptr_t)&konsts[bc]));
			cc.str(tmp, MemOffset(vmframe, paramOffset(slot, myoffsetof(VMValue, sp)), 8));
			break;
		case REGT_POINTER:
			cc.str(regA[bc], MemOffset(vmframe, paramOffset(slot, myoffsetof(VMValue, a)), 8));
			break;
		case REGT_POINTER | REGT_ADDROF:
			EmitAddOffset(stackPtr, vmframe, offsetA + (int)(bc * sizeof(void*)));
			cc.str(regA[bc], a64::ptr(stackPtr));
			cc.str(stackPtr, MemOffset(vmframe, paramOffset(slot, myoffsetof(VMValue, a)), 8));
			break;
		case REGT_POINTER | REGT_KONST:
			cc.mov(tmp, Imm((uint64_t)(uintptr_t)konsta[bc].v));
			cc.str(tmp, MemOffset(vmframe, paramOffset(slot, myoffsetof(VMValue, a)), 8));
			break;
		case REGT_FLOAT:
			cc.str(regF[bc], MemOffset(vmframe, paramOffset(slot, myoffsetof(VMValue, f)), 8));
			break;
		case REGT_FLOAT | REGT_MULTIREG2:
			for (int j = 0; j < 2; j++)
			{
				cc.str(regF[bc + j], MemOffset(vmframe, paramOffset(slot + j, myoffsetof(VMValue, f)), 8));
			}
			numparams++;
			break;
		case REGT_FLOAT | REGT_MULTIREG3:
			for (int j = 0; j < 3; j++)
			{
				cc.str(regF[bc + j], MemOffset(vmframe, paramOffset(slot + j, myoffsetof(VMValue, f)), 8));
			}
			numparams += 2;
			break;
		case REGT_FLOAT | REGT_MULTIREG4:
			for (int j = 0; j < 4; j++)
			{
				cc.str(regF[bc + j], MemOffset(vmframe, paramOffset(slot + j, myoffsetof(VMValue, f)), 8));
			}
			numparams += 3;
			break;
		case REGT_FLOAT | REGT_ADDROF:
			EmitAddOffset(stackPtr, vmframe, offsetF + (int)(bc * sizeof(double)));
			// When passing the address to a float we don't know if the receiving function will treat it as float, vec2 or vec3.
			for (int j = 0; j < 3; j++)
			{
				if ((unsigned int)(bc + j) < regF.Size())
					cc.str(regF[bc + j], a64::ptr(stackPtr, j * (int)sizeof(double)));
			}
			cc.str(stackPtr, MemOffset(vmframe, paramOffset(slot, myoffsetof(VMValue, a)), 8));
			break;
		case REGT_FLOAT | REGT_KONST:
			LoadConstFloat64(tmp2, konstf[bc]);
			cc.str(tmp2, MemOffset(vmframe, paramOffset(slot, myoffsetof(VMValue, f)), 8));
			break;

		default:
			I_Error("Unknown REGT value passed to EmitPARAM\n");
			break;
		}
	}

	return numparams;
}

void JitCompiler::LoadInOuts()
{
	for (unsigned int i = 0; i < ParamOpcodes.Size(); i++)
	{
		const VMOP &param = *ParamOpcodes[i];
		if (param.op == OP_PARAM && (param.a & REGT_ADDROF))
		{
			LoadCallResult(param.a, param.i16u, true);
		}
	}
}

void JitCompiler::LoadReturns(const VMOP *retval, int numret)
{
	for (int i = 0; i < numret; ++i)
	{
		if (retval[i].op != OP_RESULT)
			I_Error("Expected OP_RESULT to follow OP_CALL\n");

		LoadCallResult(retval[i].b, retval[i].c, false);
	}
}

void JitCompiler::LoadCallResult(int type, int regnum, bool addrof)
{
	switch (type & REGT_TYPE)
	{
	case REGT_INT:
		cc.ldr(regD[regnum], MemOffset(vmframe, offsetD + regnum * sizeof(int32_t), 4));
		break;
	case REGT_FLOAT:
		cc.ldr(regF[regnum], MemOffset(vmframe, offsetF + regnum * sizeof(double), 8));
		if (addrof)
		{
			// When passing the address to a float we don't know if the receiving function will treat it as float, vec2 or vec3.
			if ((unsigned int)regnum + 1 < regF.Size())
				cc.ldr(regF[regnum + 1], MemOffset(vmframe, offsetF + (regnum + 1) * sizeof(double), 8));
			if ((unsigned int)regnum + 2 < regF.Size())
				cc.ldr(regF[regnum + 2], MemOffset(vmframe, offsetF + (regnum + 2) * sizeof(double), 8));
		}
		else if (type & REGT_MULTIREG2)
		{
			cc.ldr(regF[regnum + 1], MemOffset(vmframe, offsetF + (regnum + 1) * sizeof(double), 8));
		}
		else if (type & REGT_MULTIREG3)
		{
			cc.ldr(regF[regnum + 1], MemOffset(vmframe, offsetF + (regnum + 1) * sizeof(double), 8));
			cc.ldr(regF[regnum + 2], MemOffset(vmframe, offsetF + (regnum + 2) * sizeof(double), 8));
		}
		else if (type & REGT_MULTIREG4)
		{
			cc.ldr(regF[regnum + 1], MemOffset(vmframe, offsetF + (regnum + 1) * sizeof(double), 8));
			cc.ldr(regF[regnum + 2], MemOffset(vmframe, offsetF + (regnum + 2) * sizeof(double), 8));
			cc.ldr(regF[regnum + 3], MemOffset(vmframe, offsetF + (regnum + 3) * sizeof(double), 8));
		}
		break;
	case REGT_STRING:
		// We don't have to do anything in this case. String values are never moved to virtual registers.
		break;
	case REGT_POINTER:
		cc.ldr(regA[regnum], MemOffset(vmframe, offsetA + regnum * sizeof(void*), 8));
		break;
	default:
		I_Error("Unknown OP_RESULT/OP_PARAM type encountered in LoadCallResult\n");
		break;
	}
}

// Address of the VM frame slot that a result of this type goes to
asmjit::a64::Gp JitCompiler::GetReturnLocation(int type, int regnum)
{
	auto regPtr = newTempIntPtr();

	switch (type & REGT_TYPE)
	{
	case REGT_INT:
		EmitAddOffset(regPtr, vmframe, offsetD + (int)(regnum * sizeof(int32_t)));
		break;
	case REGT_FLOAT:
		EmitAddOffset(regPtr, vmframe, offsetF + (int)(regnum * sizeof(double)));
		break;
	case REGT_STRING:
		EmitAddOffset(regPtr, vmframe, offsetS + (int)(regnum * sizeof(FString)));
		break;
	case REGT_POINTER:
		EmitAddOffset(regPtr, vmframe, offsetA + (int)(regnum * sizeof(void*)));
		break;
	default:
		I_Error("Unknown OP_RESULT type encountered\n");
		break;
	}
	return regPtr;
}

void JitCompiler::FillReturns(const VMOP *retval, int numret)
{
	using namespace asmjit;

	for (int i = 0; i < numret; ++i)
	{
		if (retval[i].op != OP_RESULT)
		{
			I_Error("Expected OP_RESULT to follow OP_CALL\n");
		}

		int type = retval[i].b;
		int regnum = retval[i].c;

		if (type & REGT_KONST)
		{
			I_Error("OP_RESULT with REGT_KONST is not allowed\n");
		}

		auto regPtr = GetReturnLocation(type, regnum);
		cc.str(regPtr, MemOffset(GetCallReturns(), i * sizeof(VMReturn) + myoffsetof(VMReturn, Location), 8));
		cc.strb(LoadConstInt32(type), MemOffset(GetCallReturns(), i * sizeof(VMReturn) + myoffsetof(VMReturn, RegType), 1));
	}
}

// What EmitNativeCall can pass: everything that fits the JitNativeCallArgs block, except REGT_ADDROF of an int,
// pointer or float register (the x86 version can't either). Other calls go through VMNativeFunction::NativeCall.
bool JitCompiler::CanDirectNativeCall(VMNativeFunction *target)
{
	if (pc > sfunc->Code && (pc - 1)->op == OP_VTBL)
		return false;

	int numx = 0, numd = 0;
	for (unsigned int i = 0; i < ParamOpcodes.Size(); i++)
	{
		if (ParamOpcodes[i]->op == OP_PARAMI)
		{
			numx++;
			continue;
		}

		switch (ParamOpcodes[i]->a)
		{
		case REGT_NIL:
		case REGT_INT:
		case REGT_INT | REGT_KONST:
		case REGT_STRING:
		case REGT_STRING | REGT_ADDROF:
		case REGT_STRING | REGT_KONST:
		case REGT_POINTER:
		case REGT_POINTER | REGT_KONST:
			numx++;
			break;
		case REGT_FLOAT:
		case REGT_FLOAT | REGT_KONST:
			numd++;
			break;
		case REGT_FLOAT | REGT_MULTIREG2:
			numd += 2;
			break;
		case REGT_FLOAT | REGT_MULTIREG3:
			numd += 3;
			break;
		case REGT_FLOAT | REGT_MULTIREG4:
			numd += 4;
			break;
		default:
			return false;
		}
	}

	// Results after the first one are returned through pointer arguments
	const VMOP *retval = pc + 1;
	int numret = C;
	for (int i = 0; i < numret; i++)
	{
		if (retval[i].op != OP_RESULT || (retval[i].b & REGT_KONST))
			return false;
		bool inRegister = i == 0 && (retval[i].b == REGT_INT || retval[i].b == REGT_FLOAT || retval[i].b == REGT_POINTER);
		if (!inRegister)
			numx++;
	}

	int numstack = max(numx - 8, 0) + max(numd - 8, 0);
	return numstack <= JitNativeCallArgs::MaxStackArgs;
}

void JitCompiler::EmitNativeCall(VMNativeFunction *target)
{
	using namespace asmjit;

	if (target->ImplicitArgs > 0)
	{
		auto label = EmitThrowExceptionLabel(X_READ_NIL);

		assert(ParamOpcodes.Size() > 0);
		const VMOP *param = ParamOpcodes[0];
		const int bc = param->i16u;

		if (param->op != OP_PARAM)
		{
			I_Error("Unexpected register type for self pointer\n");
		}
		else if (param->a == (REGT_POINTER | REGT_KONST))
		{
			if (konsta[bc].v == nullptr)
				cc.b(label);
		}
		else if (param->a == (REGT_STRING | REGT_KONST))
		{
			// Never null
		}
		else
		{
			switch (param->a & REGT_TYPE)
			{
			case REGT_STRING:  cc.cbz(regS[bc], label); break;
			case REGT_POINTER: cc.cbz(regA[bc], label); break;
			default:
				I_Error("Unexpected register type for self pointer\n");
				break;
			}
		}
	}

	const VMOP *retval = pc + 1;
	int numret = C;

	// Check if first return value was placed in the function's real return value slot
	int startret = 1;
	if (numret > 0)
	{
		int type = retval[0].b;
		switch (type)
		{
		case REGT_INT:
		case REGT_FLOAT:
		case REGT_POINTER:
			break;
		default:
			startret = 0;
			break;
		}
	}

	if (startret < numret)
		CheckVMFrame();

	// Not one of the temporaries: it lives until the results are read back
	auto block = cc.new_gp_ptr(logging ? "nativeArgs" : nullptr);
	cc.load_address_of(block, GetNativeCallArgsStorage());

	cc.str(LoadConstPtr(target->DirectNativeCall), a64::ptr(block, (int32_t)offsetof(JitNativeCallArgs, target)));

	// Sort the arguments the AAPCS64 way: the first 8 ints and pointers in x0-x7, the first 8 doubles in d0-d7,
	// the rest on the stack in parameter order.
	int numx = 0, numd = 0, numstack = 0;
	auto intArgSlot = [&]() -> int32_t
	{
		if (numx < 8)
			return (int32_t)(offsetof(JitNativeCallArgs, x) + sizeof(uint64_t) * numx++);
		return (int32_t)(offsetof(JitNativeCallArgs, stack) + sizeof(uint64_t) * numstack++);
	};
	auto floatArgSlot = [&]() -> int32_t
	{
		if (numd < 8)
			return (int32_t)(offsetof(JitNativeCallArgs, d) + sizeof(double) * numd++);
		return (int32_t)(offsetof(JitNativeCallArgs, stack) + sizeof(uint64_t) * numstack++);
	};

	int numparams = 0;
	for (unsigned int i = 0; i < ParamOpcodes.Size(); i++)
	{
		numparams++;

		if (ParamOpcodes[i]->op == OP_PARAMI)
		{
			int abcs = ParamOpcodes[i]->i24;
			cc.str(LoadConstInt32(abcs), a64::ptr(block, intArgSlot()));
			continue;
		}

		int bc = ParamOpcodes[i]->i16u;
		switch (ParamOpcodes[i]->a)
		{
		case REGT_NIL:
			cc.str(LoadConstPtr(nullptr), a64::ptr(block, intArgSlot()));
			break;
		case REGT_INT:
			cc.str(regD[bc], a64::ptr(block, intArgSlot()));
			break;
		case REGT_INT | REGT_KONST:
			cc.str(LoadConstInt32(konstd[bc]), a64::ptr(block, intArgSlot()));
			break;
		case REGT_STRING | REGT_ADDROF:	// AddrOf string is essentially the same - a reference to the register, just not constant on the receiving side.
		case REGT_STRING:
			cc.str(regS[bc], a64::ptr(block, intArgSlot()));
			break;
		case REGT_STRING | REGT_KONST:
			cc.str(LoadConstPtr(&konsts[bc]), a64::ptr(block, intArgSlot()));
			break;
		case REGT_POINTER:
			cc.str(regA[bc], a64::ptr(block, intArgSlot()));
			break;
		case REGT_POINTER | REGT_KONST:
			cc.str(LoadConstPtr(konsta[bc].v), a64::ptr(block, intArgSlot()));
			break;
		case REGT_FLOAT:
			cc.str(regF[bc], a64::ptr(block, floatArgSlot()));
			break;
		case REGT_FLOAT | REGT_MULTIREG2:
			for (int j = 0; j < 2; j++)
				cc.str(regF[bc + j], a64::ptr(block, floatArgSlot()));
			numparams++;
			break;
		case REGT_FLOAT | REGT_MULTIREG3:
			for (int j = 0; j < 3; j++)
				cc.str(regF[bc + j], a64::ptr(block, floatArgSlot()));
			numparams += 2;
			break;
		case REGT_FLOAT | REGT_MULTIREG4:
			for (int j = 0; j < 4; j++)
				cc.str(regF[bc + j], a64::ptr(block, floatArgSlot()));
			numparams += 3;
			break;
		case REGT_FLOAT | REGT_KONST:
			cc.str(LoadConstFloat64(konstf[bc]), a64::ptr(block, floatArgSlot()));
			break;

		default:
			// CanDirectNativeCall let it through
			I_Error("Unknown REGT value passed to EmitPARAM\n");
			break;
		}
	}

	if (numparams != B)
		I_Error("OP_CALL parameter count does not match the number of preceding OP_PARAM instructions\n");

	// Pass return pointers as arguments
	for (int i = startret; i < numret; ++i)
	{
		int type = retval[i].b;
		int regnum = retval[i].c;

		if ((type & REGT_TYPE) == REGT_STRING)
		{
			// For strings we already have them on the stack and got named registers for them.
			cc.str(regS[regnum], a64::ptr(block, intArgSlot()));
		}
		else
		{
			cc.str(GetReturnLocation(type, regnum), a64::ptr(block, intArgSlot()));
		}
	}

	bool doubleResult = startret == 1 && numret > 0 && retval[0].b == REGT_FLOAT;
	int (*wrapper)(JitNativeCallArgs *);
	if (numstack > 0)
		wrapper = doubleResult ? JitCallNativeDS : JitCallNativeXS;
	else
		wrapper = doubleResult ? JitCallNativeD : JitCallNativeX;

	auto status = newResultInt32();
	auto call = CreateCall<int, JitNativeCallArgs *>(wrapper);
	call->set_ret(0, status);
	call->set_arg(0, block);
	call->set_inline_comment(target->PrintableName);
	EmitCheckStatus(status);

	if (startret == 1 && numret > 0)
	{
		int type = retval[0].b;
		int regnum = retval[0].c;
		const int32_t retOffset = (int32_t)offsetof(JitNativeCallArgs, retx);

		switch (type)
		{
		case REGT_INT:
			cc.ldr(regD[regnum], a64::ptr(block, retOffset));
			break;
		case REGT_FLOAT:
			cc.ldr(regF[regnum], a64::ptr(block, retOffset));
			break;
		case REGT_POINTER:
			cc.ldr(regA[regnum], a64::ptr(block, retOffset));
			break;
		}
	}

	// Move the result into virtual registers
	for (int i = startret; i < numret; ++i)
	{
		int type = retval[i].b;
		int regnum = retval[i].c;

		switch (type)
		{
		case REGT_INT:
			cc.ldr(regD[regnum], MemOffset(vmframe, offsetD + regnum * sizeof(int32_t), 4));
			break;
		case REGT_FLOAT:
			cc.ldr(regF[regnum], MemOffset(vmframe, offsetF + regnum * sizeof(double), 8));
			break;
		case REGT_FLOAT | REGT_MULTIREG2:
			cc.ldr(regF[regnum], MemOffset(vmframe, offsetF + regnum * sizeof(double), 8));
			cc.ldr(regF[regnum + 1], MemOffset(vmframe, offsetF + (regnum + 1) * sizeof(double), 8));
			break;
		case REGT_FLOAT | REGT_MULTIREG3:
			cc.ldr(regF[regnum], MemOffset(vmframe, offsetF + regnum * sizeof(double), 8));
			cc.ldr(regF[regnum + 1], MemOffset(vmframe, offsetF + (regnum + 1) * sizeof(double), 8));
			cc.ldr(regF[regnum + 2], MemOffset(vmframe, offsetF + (regnum + 2) * sizeof(double), 8));
			break;
		case REGT_FLOAT | REGT_MULTIREG4:
			cc.ldr(regF[regnum], MemOffset(vmframe, offsetF + regnum * sizeof(double), 8));
			cc.ldr(regF[regnum + 1], MemOffset(vmframe, offsetF + (regnum + 1) * sizeof(double), 8));
			cc.ldr(regF[regnum + 2], MemOffset(vmframe, offsetF + (regnum + 2) * sizeof(double), 8));
			cc.ldr(regF[regnum + 3], MemOffset(vmframe, offsetF + (regnum + 3) * sizeof(double), 8));
			break;
		case REGT_STRING:
			// We don't have to do anything in this case. String values are never moved to virtual registers.
			break;
		case REGT_POINTER:
			cc.ldr(regA[regnum], MemOffset(vmframe, offsetA + regnum * sizeof(void*), 8));
			break;
		default:
			I_Error("Unknown OP_RESULT type encountered\n");
			break;
		}
	}

	ParamOpcodes.Clear();
}

void JitCompiler::EmitNULLCHECK()
{
	EmitNullPointerThrow(A, X_READ_NIL);
}
