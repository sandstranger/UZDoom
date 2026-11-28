/*
** jit.cpp
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

#include "jit.h"
#include "jitintern.h"
#include "printf.h"

extern PString *TypeString;
extern PStruct *TypeVector2;
extern PStruct *TypeVector3;
extern PStruct* TypeVector4;
extern PStruct* TypeQuaternion;
extern PStruct* TypeFQuaternion;

static void OutputJitLog(const asmjit::StringLogger &logger);

JitFuncPtr JitCompile(VMScriptFunction *sfunc)
{
#if 0
	if (strcmp(sfunc->PrintableName, "StatusScreen.drawNum") != 0)
		return nullptr;
#endif

	using namespace asmjit;
	try
	{
		ThrowingErrorHandler errorHandler;
		CodeHolder code;
		code.init(GetHostEnvironment());
		code.set_error_handler(&errorHandler);

		JitCompiler compiler(&code, sfunc);
		void *rawcode = AddJitFunction(&code, &compiler);
		if (!rawcode)
			return nullptr;

		// JIT code calls JitCall, which reports errors with a negative result. C++ code calls ScriptCall.
		sfunc->JitCall = reinterpret_cast<JitFuncPtr>(rawcode);
		return &JitScriptCall;
	}
	catch (const std::exception &e)
	{
		// The interpreter runs this function instead
		Printf(TEXTCOLOR_ORANGE "%s: Unexpected JIT error: %s\n", sfunc->PrintableName, e.what());
		return nullptr;
	}
}

void JitDumpLog(FILE *file, VMScriptFunction *sfunc)
{
	using namespace asmjit;
	StringLogger logger;

	if(sfunc->VarFlags & VARF_Abstract)
	{
		// Printf(TEXTCOLOR_ORANGE "Skipping abstract function during JIT dump: %s\n", sfunc->PrintableName.GetChars());
		return;
	}

	try
	{
		ThrowingErrorHandler errorHandler;
		CodeHolder code;
		code.init(GetHostEnvironment());
		code.set_error_handler(&errorHandler);
		code.set_logger(&logger);

		JitCompiler compiler(&code, sfunc);
		compiler.Codegen();

		fwrite(logger.data(), logger.data_size(), 1, file);
	}
	catch (const std::exception &e)
	{
		fwrite(logger.data(), logger.data_size(), 1, file);

		FString err;
		err.Format("Unexpected JIT error: %s\n", e.what());
		fwrite(err.GetChars(), err.Len(), 1, file);
		fclose(file);

		I_FatalError("Unexpected JIT error: %s\n", e.what());
	}
}

[[maybe_unused]] static void OutputJitLog(const asmjit::StringLogger &logger)
{
	// Write line by line since I_FatalError seems to cut off long strings
	const char *pos = logger.data();
	const char *end = pos;
	while (*end)
	{
		if (*end == '\n')
		{
			FString substr(pos, (int)(ptrdiff_t)(end - pos));
			Printf("%s\n", substr.GetChars());
			pos = end + 1;
		}
		end++;
	}
	if (pos != end)
		Printf("%s\n", pos);
}

/////////////////////////////////////////////////////////////////////////////

static const char *OpNames[NUM_OPS] =
{
#define xx(op, name, mode, alt, kreg, ktype)	#op,
#include "vmops.h"
#undef xx
};

asmjit::FuncNode *JitCompiler::Codegen()
{
	logging = cc.logger() != nullptr;

	Setup();

	int lastLine = -1;

	pc = sfunc->Code;
	auto end = pc + sfunc->CodeSize;
	while (pc != end)
	{
		int i = (int)(ptrdiff_t)(pc - sfunc->Code);
		op = pc->op;

		int curLine = sfunc->PCToLine(pc);
		if (curLine != lastLine)
		{
			lastLine = curLine;

			auto label = cc.new_label();
			cc.bind(label);

			JitLineInfo info;
			info.Label = label;
			info.LineNumber = curLine;
			LineInfo.Push(info);
		}

		if (logging && op != OP_PARAM && op != OP_PARAMI && op != OP_VTBL)
		{
			FString lineinfo;
			lineinfo.Format("; line %d: %02x%02x%02x%02x %s", curLine, pc->op, pc->a, pc->b, pc->c, OpNames[op]);
			cc.comment("", 0);
			cc.comment(lineinfo.GetChars(), lineinfo.Len());
		}

		labels[i].cursor = cc.cursor();
		ResetTemp();
		EmitOpcode();

		pc++;
	}

	BindLabels();

	ResetTemp();
	EmitErrorExit();

	cc.end_func();
	cc.finalize();

	auto code = cc.code();
	for (unsigned int j = 0; j < LineInfo.Size(); j++)
	{
		auto info = LineInfo[j];

		if (!code->is_label_valid(info.Label) || !code->is_label_bound(info.Label))
		{
			continue;
		}

		info.InstructionIndex = (ptrdiff_t)code->label_offset(info.Label);

		LineInfo[j] = info;
	}

	std::stable_sort(LineInfo.begin(), LineInfo.end(), [](const JitLineInfo &a, const JitLineInfo &b) { return a.InstructionIndex < b.InstructionIndex; });

	return func;
}

void JitCompiler::EmitOpcode()
{
	switch (op)
	{
		#define xx(op, name, mode, alt, kreg, ktype)	case OP_##op: Emit##op(); break;
		#include "vmops.h"
		#undef xx

	default:
		I_Error("JIT error: Unknown VM opcode %d\n", op);
		break;
	}
}

void JitCompiler::BindLabels()
{
	asmjit::BaseNode *cursor = cc.cursor();
	unsigned int size = labels.Size();
	for (unsigned int i = 0; i < size; i++)
	{
		const OpcodeLabel &label = labels[i];
		if (label.inUse)
		{
			cc.set_cursor(label.cursor);
			cc.bind(label.label);
		}
	}
	cc.set_cursor(cursor);
}

// Reached from every place that found a kept exception (see jitintern.h). The deeper frames have popped
// their VM frames already (the interpreter does it in a catch block), so this one is on top of the stack.
void JitCompiler::EmitErrorExit()
{
	if (!errorExitUsed)
		return;

	cc.bind(errorExit);
	EmitPopFrame();
	auto result = newTempInt32();
	cc.mov(result, asmjit::Imm(-1));
	cc.ret(result);
}

void JitCompiler::CheckVMFrame()
{
	if (!vmframeAllocated)
	{
		auto cursor = cc.cursor();
		cc.set_cursor(vmframeCursor);

		auto vmstack = cc.new_stack(sfunc->StackSize, 16, logging ? "vmstack" : nullptr);
		vmframe = cc.new_gp_ptr(logging ? "vmframe" : nullptr);
		cc.load_address_of(vmframe, vmstack);

		cc.set_cursor(cursor);
		vmframeAllocated = true;
	}
}

asmjit::a64::Gp JitCompiler::GetCallReturns()
{
	if (!callReturnsAllocated)
	{
		auto cursor = cc.cursor();
		cc.set_cursor(callReturnsCursor);
		auto stackalloc = cc.new_stack(sizeof(VMReturn) * MAX_RETURNS, alignof(VMReturn), logging ? "stackalloc" : nullptr);
		callReturns = cc.new_gp_ptr(logging ? "callReturns" : nullptr);
		cc.load_address_of(callReturns, stackalloc);
		cc.set_cursor(cursor);
		callReturnsAllocated = true;
	}
	return callReturns;
}

void JitCompiler::Setup()
{
	using namespace asmjit;

	ResetTemp();

	if (logging)
	{
		static const char *marks = "=======================================================";
		cc.comment("", 0);
		cc.comment(marks, 56);

		FString funcname;
		funcname.Format("Function: %s", sfunc->PrintableName);
		cc.comment(funcname.GetChars(), funcname.Len());

		cc.comment(marks, 56);
		cc.comment("", 0);
	}

	func = cc.add_func(FuncSignature::build<int, VMFunction *, void *, int, void *, int>());

	// A frame record (x29/x30) in every function: profilers and debuggerd can walk the stack with it
	func->frame().set_preserved_fp();

	auto unusedFunc = cc.new_gp_ptr(logging ? "func" : nullptr); // VMFunction*
	args = cc.new_gp_ptr(logging ? "args" : nullptr); // VMValue *params
	numargs = cc.new_gpw(logging ? "numargs" : nullptr); // int numargs
	ret = cc.new_gp_ptr(logging ? "ret" : nullptr); // VMReturn *ret
	numret = cc.new_gpw(logging ? "numret" : nullptr); // int numret

	func->set_arg(0, unusedFunc);
	func->set_arg(1, args);
	func->set_arg(2, numargs);
	func->set_arg(3, ret);
	func->set_arg(4, numret);

	errorExit = cc.new_label();

	callReturnsCursor = cc.cursor();

	konstd = sfunc->KonstD;
	konstf = sfunc->KonstF;
	konsts = sfunc->KonstS;
	konsta = sfunc->KonstA;

	labels.Resize(sfunc->CodeSize);

	CreateRegisters();
	IncrementVMCalls();
	SetupFrame();
}

void JitCompiler::SetupFrame()
{
	// the VM version reads this from the stack, but it is constant data
	offsetParams = ((int)sizeof(VMFrame) + 15) & ~15;
	offsetF = offsetParams + (int)(sfunc->MaxParam * sizeof(VMValue));
	offsetS = offsetF + (int)(sfunc->NumRegF * sizeof(double));
	offsetA = offsetS + (int)(sfunc->NumRegS * sizeof(FString));
	offsetD = offsetA + (int)(sfunc->NumRegA * sizeof(void*));
	offsetExtra = (offsetD + (int)(sfunc->NumRegD * sizeof(int32_t)) + 15) & ~15;

	if (sfunc->SpecialInits.Size() == 0 && sfunc->NumRegS == 0 && sfunc->ExtraSpace == 0)
	{
		SetupSimpleFrame();
	}
	else
	{
		SetupFullVMFrame();
	}
}

void JitCompiler::SetupSimpleFrame()
{
	using namespace asmjit;

	// This is a simple frame with no constructors or destructors. Allocate it on the stack ourselves.

	vmframeCursor = cc.cursor();

	int argsPos = 0;
	int regd = 0, regf = 0, rega = 0;
	for (unsigned int i = 0; i < sfunc->Proto->ArgumentTypes.Size(); i++)
	{
		const PType *type = sfunc->Proto->ArgumentTypes[i];
		if (sfunc->ArgFlags.Size() && sfunc->ArgFlags[i] & (VARF_Out | VARF_Ref))
		{
			cc.ldr(regA[rega++], MemOffset(args, argsPos++ * sizeof(VMValue) + offsetof(VMValue, a), 8));
		}
		else if (type == TypeVector2 || type == TypeFVector2)
		{
			cc.ldr(regF[regf++], MemOffset(args, argsPos++ * sizeof(VMValue) + offsetof(VMValue, f), 8));
			cc.ldr(regF[regf++], MemOffset(args, argsPos++ * sizeof(VMValue) + offsetof(VMValue, f), 8));
		}
		else if (type == TypeVector3 || type == TypeFVector3)
		{
			cc.ldr(regF[regf++], MemOffset(args, argsPos++ * sizeof(VMValue) + offsetof(VMValue, f), 8));
			cc.ldr(regF[regf++], MemOffset(args, argsPos++ * sizeof(VMValue) + offsetof(VMValue, f), 8));
			cc.ldr(regF[regf++], MemOffset(args, argsPos++ * sizeof(VMValue) + offsetof(VMValue, f), 8));
		}
		else if (type == TypeVector4 || type == TypeFVector4 || type == TypeQuaternion || type == TypeFQuaternion)
		{
			cc.ldr(regF[regf++], MemOffset(args, argsPos++ * sizeof(VMValue) + offsetof(VMValue, f), 8));
			cc.ldr(regF[regf++], MemOffset(args, argsPos++ * sizeof(VMValue) + offsetof(VMValue, f), 8));
			cc.ldr(regF[regf++], MemOffset(args, argsPos++ * sizeof(VMValue) + offsetof(VMValue, f), 8));
			cc.ldr(regF[regf++], MemOffset(args, argsPos++ * sizeof(VMValue) + offsetof(VMValue, f), 8));
		}
		else if (type == TypeFloat64)
		{
			cc.ldr(regF[regf++], MemOffset(args, argsPos++ * sizeof(VMValue) + offsetof(VMValue, f), 8));
		}
		else if (type == TypeString)
		{
			I_FatalError("JIT: Strings are not supported yet for simple frames");
		}
		else if (type->isIntCompatible())
		{
			cc.ldr(regD[regd++], MemOffset(args, argsPos++ * sizeof(VMValue) + offsetof(VMValue, i), 4));
		}
		else
		{
			cc.ldr(regA[rega++], MemOffset(args, argsPos++ * sizeof(VMValue) + offsetof(VMValue, a), 8));
		}
	}

	const char *errorDetails = nullptr;

	if (sfunc->NumArgs != argsPos)
	{
		errorDetails = "arguments";
	}
	else if (regd > sfunc->NumRegD)
	{
		errorDetails = "integer registers";
	}
	else if (regf > sfunc->NumRegF)
	{
		errorDetails = "floating point registers";
	}
	else if (rega > sfunc->NumRegA)
	{
		errorDetails = "address registers";
	}

	if (errorDetails)
	{
		I_FatalError("JIT: inconsistent number of %s for function %s", errorDetails, sfunc->PrintableName);
	}

	for (int i = regd; i < sfunc->NumRegD; i++)
		cc.mov(regD[i], Imm(0));

	if (regf < sfunc->NumRegF)
	{
		auto zero = newTempInt64();
		cc.mov(zero, Imm(0));
		for (int i = regf; i < sfunc->NumRegF; i++)
			cc.fmov(regF[i], zero);
	}

	for (int i = rega; i < sfunc->NumRegA; i++)
		cc.mov(regA[i], Imm(0));
}

static VMFrameStack *CreateFullVMFrame(VMScriptFunction *func, VMValue *args, int numargs)
{
	VMFrameStack *stack = &GlobalVMStack;
	VMFrame *newf = stack->AllocFrame(func);
	VMFillParams(args, newf, numargs);
	return stack;
}

void JitCompiler::SetupFullVMFrame()
{
	using namespace asmjit;

	stack = cc.new_gp_ptr(logging ? "stack" : nullptr);
	auto sfuncptr = LoadConstPtr(sfunc);
	auto allocFrame = CreateCall<VMFrameStack *, VMScriptFunction *, VMValue *, int>(CreateFullVMFrame);
	allocFrame->set_ret(0, stack);
	allocFrame->set_arg(0, sfuncptr);
	allocFrame->set_arg(1, args);
	allocFrame->set_arg(2, numargs);

	vmframe = cc.new_gp_ptr(logging ? "vmframe" : nullptr);
	cc.ldr(vmframe, a64::ptr(stack)); // stack->Blocks
	cc.ldr(vmframe, MemOffset(vmframe, VMFrameStack::OffsetLastFrame(), 8)); // Blocks->LastFrame
	vmframeAllocated = true;

	for (int i = 0; i < sfunc->NumRegD; i++)
		cc.ldr(regD[i], MemOffset(vmframe, offsetD + i * sizeof(int32_t), 4));

	for (int i = 0; i < sfunc->NumRegF; i++)
		cc.ldr(regF[i], MemOffset(vmframe, offsetF + i * sizeof(double), 8));

	for (int i = 0; i < sfunc->NumRegS; i++)
		EmitAddOffset(regS[i], vmframe, offsetS + i * sizeof(FString));

	for (int i = 0; i < sfunc->NumRegA; i++)
		cc.ldr(regA[i], MemOffset(vmframe, offsetA + i * sizeof(void*), 8));
}

static void PopFullVMFrame(VMFrameStack *stack)
{
	stack->PopFrame();
}

void JitCompiler::EmitPopFrame()
{
	if (sfunc->SpecialInits.Size() != 0 || sfunc->NumRegS != 0 || sfunc->ExtraSpace != 0)
	{
		auto popFrame = CreateCall<void, VMFrameStack *>(PopFullVMFrame);
		popFrame->set_arg(0, stack);
	}
}

void JitCompiler::IncrementVMCalls()
{
	// VMCalls[0]++
	auto vmcallsptr = LoadConstPtr(VMCalls);
	auto vmcalls = newTempInt32();
	cc.ldr(vmcalls, asmjit::a64::ptr(vmcallsptr));
	cc.add(vmcalls, vmcalls, asmjit::Imm(1));
	cc.str(vmcalls, asmjit::a64::ptr(vmcallsptr));
}

void JitCompiler::CreateRegisters()
{
	regD.Resize(sfunc->NumRegD);
	regF.Resize(sfunc->NumRegF);
	regA.Resize(sfunc->NumRegA);
	regS.Resize(sfunc->NumRegS);

	for (int i = 0; i < sfunc->NumRegD; i++)
	{
		regD[i] = logging ? cc.new_gpw("regD%d", i) : cc.new_gpw();
	}

	for (int i = 0; i < sfunc->NumRegF; i++)
	{
		regF[i] = logging ? cc.new_vec_d("regF%d", i) : cc.new_vec_d();
	}

	for (int i = 0; i < sfunc->NumRegS; i++)
	{
		regS[i] = logging ? cc.new_gp_ptr("regS%d", i) : cc.new_gp_ptr();
	}

	for (int i = 0; i < sfunc->NumRegA; i++)
	{
		regA[i] = logging ? cc.new_gp_ptr("regA%d", i) : cc.new_gp_ptr();
	}
}

/////////////////////////////////////////////////////////////////////////////
// Errors

void JitCompiler::EmitNullPointerThrow(int index, EVMAbortException reason)
{
	auto label = EmitThrowExceptionLabel(reason);
	cc.cbz(regA[index], label);
}

void JitCompiler::ThrowException(int reason)
{
	try
	{
		ThrowAbortException((EVMAbortException)reason, nullptr);
	}
	catch (...)
	{
		JitStashException();
	}
}

void JitCompiler::EmitThrowCall(EVMAbortException reason)
{
	auto call = CreateCall<void, int>(&JitCompiler::ThrowException);
	call->set_arg(0, asmjit::Imm((int)reason));
	cc.b(ErrorExitLabel());
}

void JitCompiler::EmitThrowException(EVMAbortException reason)
{
	EmitThrowCall(reason);
}

asmjit::Label JitCompiler::EmitThrowExceptionLabel(EVMAbortException reason)
{
	auto label = cc.new_label();
	auto cursor = cc.cursor();
	cc.bind(label);
	EmitThrowCall(reason);
	cc.set_cursor(cursor);

	JitLineInfo info;
	info.Label = label;
	info.LineNumber = sfunc->PCToLine(pc);
	LineInfo.Push(info);

	return label;
}

/////////////////////////////////////////////////////////////////////////////
// Immediates and addresses

asmjit::a64::Mem JitCompiler::MemOffset(const Gp &base, int64_t offset, int accessSize)
{
	if (IsEncodableOffset(offset, accessSize))
		return asmjit::a64::ptr(base, (int32_t)offset);

	auto tmp = newTempIntPtr();
	cc.mov(tmp, asmjit::Imm(offset));
	return asmjit::a64::ptr(base, tmp);
}

void JitCompiler::EmitAddOffset(const Gp &dst, const Gp &base, int64_t offset)
{
	if (offset >= 0 && offset <= 4095)
	{
		cc.add(dst, base, asmjit::Imm(offset));
	}
	else if (offset < 0 && offset >= -4095)
	{
		cc.sub(dst, base, asmjit::Imm(-offset));
	}
	else
	{
		auto tmp = newTempIntPtr();
		cc.mov(tmp, asmjit::Imm(offset));
		cc.add(dst, base, tmp);
	}
}

void JitCompiler::EmitAddImm32(const Gp &dst, const Gp &src, int32_t imm)
{
	if (imm >= 0 && imm <= 4095)
	{
		cc.add(dst, src, asmjit::Imm(imm));
	}
	else if (imm < 0 && imm >= -4095)
	{
		cc.sub(dst, src, asmjit::Imm(-imm));
	}
	else
	{
		cc.add(dst, src, LoadConstInt32(imm));
	}
}

void JitCompiler::EmitSubImm32(const Gp &dst, const Gp &src, int32_t imm)
{
	if (imm >= 0 && imm <= 4095)
	{
		cc.sub(dst, src, asmjit::Imm(imm));
	}
	else if (imm < 0 && imm >= -4095)
	{
		cc.add(dst, src, asmjit::Imm(-imm));
	}
	else
	{
		cc.sub(dst, src, LoadConstInt32(imm));
	}
}

void JitCompiler::EmitCmpImm32(const Gp &reg, int32_t imm)
{
	if (imm >= 0 && imm <= 4095)
	{
		cc.cmp(reg, asmjit::Imm(imm));
	}
	else if (imm < 0 && imm >= -4095)
	{
		cc.cmn(reg, asmjit::Imm(-imm));
	}
	else
	{
		cc.cmp(reg, LoadConstInt32(imm));
	}
}

asmjit::a64::Gp JitCompiler::LoadConstInt32(int32_t value)
{
	auto tmp = newTempInt32();
	cc.mov(tmp, asmjit::Imm(value));
	return tmp;
}

asmjit::a64::Gp JitCompiler::LoadConstPtr(const void *ptr)
{
	auto tmp = newTempIntPtr();
	cc.mov(tmp, asmjit::Imm((uint64_t)(uintptr_t)ptr));
	return tmp;
}

void JitCompiler::LoadConstFloat64(const Vec &dst, double value)
{
	uint64_t bits;
	memcpy(&bits, &value, sizeof(bits));
	auto tmp = newTempInt64();
	cc.mov(tmp, asmjit::Imm(bits));
	cc.fmov(dst, tmp);
}

asmjit::a64::Vec JitCompiler::LoadConstFloat64(double value)
{
	auto tmp = newTempFloat64();
	LoadConstFloat64(tmp, value);
	return tmp;
}

/////////////////////////////////////////////////////////////////////////////

asmjit::a64::Gp JitCompiler::CheckRegD(int r0, int r1)
{
	if (r0 != r1)
	{
		return regD[r0];
	}
	else
	{
		auto copy = newTempInt32();
		cc.mov(copy, regD[r0]);
		return copy;
	}
}

asmjit::a64::Vec JitCompiler::CheckRegF(int r0, int r1)
{
	if (r0 != r1)
	{
		return regF[r0];
	}
	else
	{
		auto copy = newTempFloat64();
		cc.fmov(copy, regF[r0]);
		return copy;
	}
}

asmjit::a64::Vec JitCompiler::CheckRegF(int r0, int r1, int r2)
{
	if (r0 != r1 && r0 != r2)
	{
		return regF[r0];
	}
	else
	{
		auto copy = newTempFloat64();
		cc.fmov(copy, regF[r0]);
		return copy;
	}
}

asmjit::a64::Vec JitCompiler::CheckRegF(int r0, int r1, int r2, int r3)
{
	if (r0 != r1 && r0 != r2 && r0 != r3)
	{
		return regF[r0];
	}
	else
	{
		auto copy = newTempFloat64();
		cc.fmov(copy, regF[r0]);
		return copy;
	}
}

asmjit::a64::Vec JitCompiler::CheckRegF(int r0, int r1, int r2, int r3, int r4)
{
	if (r0 != r1 && r0 != r2 && r0 != r3 && r0 != r4)
	{
		return regF[r0];
	}
	else
	{
		auto copy = newTempFloat64();
		cc.fmov(copy, regF[r0]);
		return copy;
	}
}

asmjit::a64::Gp JitCompiler::CheckRegS(int r0, int r1)
{
	if (r0 != r1)
	{
		return regS[r0];
	}
	else
	{
		auto copy = newTempIntPtr();
		cc.mov(copy, regS[r0]);
		return copy;
	}
}

asmjit::a64::Gp JitCompiler::CheckRegA(int r0, int r1)
{
	if (r0 != r1)
	{
		return regA[r0];
	}
	else
	{
		auto copy = newTempIntPtr();
		cc.mov(copy, regA[r0]);
		return copy;
	}
}

void JitCompiler::EmitNOP()
{
	cc.nop();
}
