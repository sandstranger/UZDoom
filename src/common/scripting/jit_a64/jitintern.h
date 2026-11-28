/*
** jitintern.h
**
** ZScript JIT for AArch64, a port of the x86-64 JIT in common/scripting/jit to the
** a64::Compiler of a current asmjit (libraries/asmjit_a64).
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
** How errors work here (the main difference to the x86 version):
**
** C++ exceptions never unwind through JIT code. With c++_shared, exceptions are
** thrown and unwound by libc++_shared.so's own copy of libunwind, and unwind info
** registered with __register_frame from this library goes to this library's copy,
** so the throwing unwinder could never step over a JIT frame.
**
** So every call from JIT code into C++ that can throw goes through a wrapper that
** catches the exception, keeps it aside (jit_runtime.cpp) and returns a status.
** The JIT code then leaves through its error exit: it pops its VM frame and
** returns -1. Every JIT caller checks for a negative result of VMFunction::JitCall
** and does the same, and VMScriptFunction::ScriptCall of a compiled function is a
** wrapper that rethrows the kept exception for its C++ caller.
**
** Unwind info is still generated and registered for the JIT code, for this
** library's own unwinder, so that JitCaptureStackTrace can walk JIT frames
** for the script call stack of VM abort messages.
*/

#pragma once
#include "jit.h"

#include "types.h"
#include "stats.h"

#include <asmjit/core.h>
#include <asmjit/a64.h>
#include <functional>
#include <vector>

extern cycle_t VMCycles[10];
extern int VMCalls[10];

#define A				(pc[0].a)
#define B				(pc[0].b)
#define C				(pc[0].c)
#define Cs				(pc[0].cs)
#define BC				(pc[0].i16u)
#define BCs				(pc[0].i16)
#define ABCs			(pc[0].i24)
#define JMPOFS(x)		((x)->i24)

struct JitLineInfo
{
	ptrdiff_t InstructionIndex = 0;
	int32_t LineNumber = -1;
	asmjit::Label Label;
};

// Arguments of a direct native call, written by JIT code and passed to one of the JitCallNative* wrappers
// (jit_call.cpp), which load them into the AAPCS64 argument registers and stack slots and call the target.
struct JitNativeCallArgs
{
	enum { MaxStackArgs = 16 };

	void *target;
	uint64_t x[8];					// x0-x7: int (low 32 bits) and pointer arguments
	double d[8];					// d0-d7: double arguments
	uint64_t stack[MaxStackArgs];	// arguments that did not fit, in parameter order, 8 bytes each
	union
	{
		uint64_t retx;				// int (low 32 bits) or pointer result
		double retd;				// double result
	};
};

// Exceptions caught by the wrappers that JIT code calls (jit_runtime.cpp)
void JitStashException();

class JitCompiler
{
public:
	JitCompiler(asmjit::CodeHolder *code, VMScriptFunction *sfunc) : cc(code), sfunc(sfunc) { }

	asmjit::FuncNode *Codegen();
	VMScriptFunction *GetScriptFunction() { return sfunc; }

	TArray<JitLineInfo> LineInfo;

private:
	using Gp = asmjit::a64::Gp;
	using Vec = asmjit::a64::Vec;
	using Mem = asmjit::a64::Mem;

	// Declare EmitXX functions for the opcodes:
	#define xx(op, name, mode, alt, kreg, ktype)	void Emit##op();
	#include "vmops.h"
	#undef xx

	void Setup();
	void CreateRegisters();
	void IncrementVMCalls();
	void SetupFrame();
	void SetupSimpleFrame();
	void SetupFullVMFrame();
	void BindLabels();
	void EmitOpcode();
	void EmitPopFrame();
	void EmitErrorExit();

	bool CanDirectNativeCall(VMNativeFunction *target);
	void EmitNativeCall(VMNativeFunction *target);
	void EmitVMCall(Gp ptr, VMFunction *target);
	void EmitVtbl(const VMOP *op);

	int StoreCallParams();
	void LoadInOuts();
	void LoadReturns(const VMOP *retval, int numret);
	void FillReturns(const VMOP *retval, int numret);
	void LoadCallResult(int type, int regnum, bool addrof);
	Gp GetReturnLocation(int type, int regnum);

	template <typename Func>
	void EmitComparisonOpcode(Func jmpFunc)
	{
		using namespace asmjit;

		int i = (int)(ptrdiff_t)(pc - sfunc->Code);

		auto successLabel = cc.new_label();

		auto failLabel = GetLabel(i + 2 + JMPOFS(pc + 1));

		jmpFunc(static_cast<bool>(A & CMP_CHECK), failLabel, successLabel);

		cc.bind(successLabel);
		pc++; // This instruction uses two instruction slots - skip the next one
	}

	// The flags after FCMP: EQ/MI/GT/LS/GE are false when unordered (NaN), NE/PL/LE/HI/LT true.
	// That matches the x86 version's ucomisd and parity checks.
	template<int N>
	void EmitVectorComparison(bool check, asmjit::Label& fail, asmjit::Label& success)
	{
		using namespace asmjit;

		bool approx = static_cast<bool>(A & CMP_APPROX);
		if (!approx)
		{
			for (int i = 0; i < N; i++)
			{
				cc.fcmp(regF[B + i], regF[C + i]);
				if (check)
				{
					if (i == (N - 1))
						cc.b_eq(fail);
					else
						cc.b_ne(success);
				}
				else
				{
					cc.b_ne(fail);
				}
			}
		}
		else
		{
			auto tmp = newTempFloat64();
			auto epsilon = newTempFloat64();
			LoadConstFloat64(epsilon, EQUAL_EPSILON);

			for (int i = 0; i < N; i++)
			{
				cc.fsub(tmp, regF[B + i], regF[C + i]);
				cc.fabs(tmp, tmp);
				cc.fcmp(epsilon, tmp);

				if (check)
				{
					if (i == (N - 1))
						cc.b_gt(fail);
					else
						cc.b_le(success);
				}
				else
				{
					cc.b_le(fail);
				}
			}
		}
	}

	static void CallAssignString(FString* to, FString* from) {
		*to = *from;
	}

	// Call to a C++ function with a fixed signature. The target goes through a register: BL only reaches +-128 MB.
	template<typename RetType, typename... Args>
	asmjit::InvokeNode *CreateCall(RetType(*func)(Args...))
	{
		auto target = newTempIntPtr();
		cc.mov(target, asmjit::Imm((uint64_t)(uintptr_t)reinterpret_cast<void*>(func)));
		asmjit::InvokeNode *invokeNode = nullptr;
		cc.invoke(asmjit::Out<asmjit::InvokeNode*>(invokeNode), target, asmjit::FuncSignature::build<RetType, Args...>());
		return invokeNode;
	}

	// Branches to the error exit if a status returned by a JitCallNoThrow-style wrapper is not 0
	void EmitCheckStatus(const Gp &status)
	{
		cc.cbnz(status, ErrorExitLabel());
	}

	asmjit::Label ErrorExitLabel()
	{
		errorExitUsed = true;
		return errorExit;
	}

	// Immediates and addresses: AArch64 instructions only encode small immediates and offsets.
	static bool IsEncodableOffset(int64_t offset, int accessSize)
	{
		if (offset >= -256 && offset <= 255)
			return true; // LDUR/STUR
		return offset >= 0 && (offset % accessSize) == 0 && (offset / accessSize) <= 4095;
	}
	Mem MemOffset(const Gp &base, int64_t offset, int accessSize);
	Mem MemIndex(const Gp &base, const Gp &index32, int shift = 0) { return asmjit::a64::ptr(base, index32, asmjit::a64::sxtw(shift)); }
	void EmitAddOffset(const Gp &dst, const Gp &base, int64_t offset);
	void EmitAddImm32(const Gp &dst, const Gp &src, int32_t imm);
	void EmitSubImm32(const Gp &dst, const Gp &src, int32_t imm);
	void EmitCmpImm32(const Gp &reg, int32_t imm);
	Gp LoadConstInt32(int32_t value);
	Gp LoadConstPtr(const void *ptr);
	Vec LoadConstFloat64(double value);
	void LoadConstFloat64(const Vec &dst, double value);

	FString regname;
	bool logging = false;
	size_t tmpPosInt32, tmpPosInt64, tmpPosIntPtr, tmpPosFloat64, tmpPosFloat32, resultPosInt32, resultPosIntPtr, resultPosFloat64;
	std::vector<Gp> regTmpInt32, regTmpInt64, regTmpIntPtr, regResultInt32, regResultIntPtr;
	std::vector<Vec> regTmpFloat64, regTmpFloat32, regResultFloat64;

	void ResetTemp()
	{
		tmpPosInt32 = 0;
		tmpPosInt64 = 0;
		tmpPosIntPtr = 0;
		tmpPosFloat64 = 0;
		tmpPosFloat32 = 0;
		resultPosInt32 = 0;
		resultPosIntPtr = 0;
		resultPosFloat64 = 0;
	}

	template<typename T, typename NewFunc>
	T newTempRegister(std::vector<T> &tmpVector, size_t &tmpPos, const char *name, NewFunc newCallback)
	{
		if (tmpPos == tmpVector.size())
		{
			const char *regName = nullptr;
			if (logging)
			{
				regname.Format("%s%d", name, (int)tmpVector.size());
				regName = regname.GetChars();
			}
			tmpVector.push_back(newCallback(regName));
		}
		return tmpVector[tmpPos++];
	}

	Gp newTempInt32() { return newTempRegister(regTmpInt32, tmpPosInt32, "tmpW", [&](const char *name) { return cc.new_gpw(name); }); }
	Gp newTempInt64() { return newTempRegister(regTmpInt64, tmpPosInt64, "tmpX", [&](const char *name) { return cc.new_gpx(name); }); }
	Gp newTempIntPtr() { return newTempRegister(regTmpIntPtr, tmpPosIntPtr, "tmpPtr", [&](const char *name) { return cc.new_gp_ptr(name); }); }
	Vec newTempFloat64() { return newTempRegister(regTmpFloat64, tmpPosFloat64, "tmpD", [&](const char *name) { return cc.new_vec_d(name); }); }
	Vec newTempFloat32() { return newTempRegister(regTmpFloat32, tmpPosFloat32, "tmpS", [&](const char *name) { return cc.new_vec_s(name); }); }

	Gp newResultInt32() { return newTempRegister(regResultInt32, resultPosInt32, "resultW", [&](const char *name) { return cc.new_gpw(name); }); }
	Gp newResultIntPtr() { return newTempRegister(regResultIntPtr, resultPosIntPtr, "resultPtr", [&](const char *name) { return cc.new_gp_ptr(name); }); }
	Vec newResultFloat64() { return newTempRegister(regResultFloat64, resultPosFloat64, "resultD", [&](const char *name) { return cc.new_vec_d(name); }); }

	void EmitReadBarrier();

	void EmitNullPointerThrow(int index, EVMAbortException reason);
	void EmitThrowException(EVMAbortException reason);
	asmjit::Label EmitThrowExceptionLabel(EVMAbortException reason);
	void EmitThrowCall(EVMAbortException reason);

	// Called by JIT code; they never throw, see the top of the file
	static void ThrowArrayOutOfBounds(int index, int size);
	static void ThrowException(int reason);

	Gp CheckRegD(int r0, int r1);
	Vec CheckRegF(int r0, int r1);
	Vec CheckRegF(int r0, int r1, int r2);
	Vec CheckRegF(int r0, int r1, int r2, int r3);
	Vec CheckRegF(int r0, int r1, int r2, int r3, int r4);
	Gp CheckRegS(int r0, int r1);
	Gp CheckRegA(int r0, int r1);

	void CallSqrt(const Vec &a, const Vec &b);

	asmjit::a64::Compiler cc;
	VMScriptFunction *sfunc;

	asmjit::FuncNode *func = nullptr;
	Gp args;
	Gp numargs;
	Gp ret;
	Gp numret;
	Gp stack;

	asmjit::Label errorExit;
	bool errorExitUsed = false;

	int offsetParams;
	int offsetF;
	int offsetS;
	int offsetA;
	int offsetD;
	int offsetExtra;

	TArray<const VMOP *> ParamOpcodes;

	void CheckVMFrame();
	Gp GetCallReturns();

	bool vmframeAllocated = false;
	asmjit::BaseNode *vmframeCursor = nullptr;
	Gp vmframe;

	bool callReturnsAllocated = false;
	asmjit::BaseNode *callReturnsCursor = nullptr;
	Gp callReturns;

	const int *konstd;
	const double *konstf;
	const FString *konsts;
	const FVoidObj *konsta;

	TArray<Gp> regD;
	TArray<Vec> regF;
	TArray<Gp> regA;
	TArray<Gp> regS;

	struct OpcodeLabel
	{
		asmjit::BaseNode *cursor = nullptr; // null for instructions that the one before consumed
		asmjit::Label label;
		bool inUse = false;
		bool stubEmitted = false;
	};

	asmjit::Label GetLabel(size_t pos)
	{
		auto &label = labels[pos];
		if (!label.inUse)
		{
			label.label = cc.new_label();
			label.inUse = true;
		}
		return label.label;
	}

	TArray<OpcodeLabel> labels;

	// Get temporary storage enough for DVector4 which is required by operation such as MULQQ and MULQV3
	bool vectorStackAllocated = false;
	Mem vectorStack;
	Mem GetTemporaryVectorStackStorage()
	{
		if (!vectorStackAllocated)
		{
			vectorStack = cc.new_stack(sizeof(DVector4), alignof(DVector4), logging ? "tmpDVector4" : nullptr);
			vectorStackAllocated = true;
		}
		return vectorStack;
	}

	// Argument block for the direct native call wrappers
	bool nativeArgsStackAllocated = false;
	Mem nativeArgsStack;
	Mem GetNativeCallArgsStorage()
	{
		if (!nativeArgsStackAllocated)
		{
			nativeArgsStack = cc.new_stack(sizeof(JitNativeCallArgs), 16, logging ? "nativeCallArgs" : nullptr);
			nativeArgsStackAllocated = true;
		}
		return nativeArgsStack;
	}

	const VMOP *pc;
	VM_UBYTE op;
};

class AsmJitException : public std::exception
{
public:
	AsmJitException(asmjit::Error error, const char *message) noexcept : error(error), message(message)
	{
	}

	const char* what() const noexcept override
	{
		return message.GetChars();
	}

	asmjit::Error error;
	FString message;
};

class ThrowingErrorHandler : public asmjit::ErrorHandler
{
public:
	void handle_error(asmjit::Error err, const char *message, asmjit::BaseEmitter *origin) override
	{
		throw AsmJitException(err, message);
	}
};

void *AddJitFunction(asmjit::CodeHolder* code, JitCompiler *compiler);
asmjit::Environment GetHostEnvironment();

// ScriptCall of JIT compiled functions: calls JitCall (the raw code) and rethrows a kept exception
int JitScriptCall(VMFunction *func, VMValue *params, int numparams, VMReturn *ret, int numret);
