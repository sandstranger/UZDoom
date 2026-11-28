/*
** jit_flow.cpp
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

void JitCompiler::EmitTEST()
{
	int i = (int)(ptrdiff_t)(pc - sfunc->Code);
	EmitCmpImm32(regD[A], BC);
	cc.b_ne(GetLabel(i + 2));
}

void JitCompiler::EmitTESTN()
{
	int bc = BC;
	int i = (int)(ptrdiff_t)(pc - sfunc->Code);
	EmitCmpImm32(regD[A], -bc);
	cc.b_ne(GetLabel(i + 2));
}

void JitCompiler::EmitJMP()
{
	auto dest = pc + JMPOFS(pc) + 1;
	int i = (int)(ptrdiff_t)(dest - sfunc->Code);
	cc.b(GetLabel(i));
}

void JitCompiler::EmitIJMP()
{
	int base = (int)(ptrdiff_t)(pc - sfunc->Code) + 1;
	auto val = newTempInt32();
	cc.mov(val, regD[A]);

	for (int i = 0; i < (int)BCs; i++)
	{
		if (sfunc->Code[base +i].op == OP_JMP)
		{
			int target = base + i + JMPOFS(&sfunc->Code[base + i]) + 1;

			EmitCmpImm32(val, i);
			cc.b_eq(GetLabel(target));
		}
	}
	pc += BCs;

	// This should never happen. It means we are jumping to something that is not a JMP instruction!
	EmitThrowException(X_OTHER);
}

// FScopeBarrier::ValidateCall throws when the call crosses scopes
static int ValidateCall(DObject *o, VMFunction *f, int b)
{
	try
	{
		FScopeBarrier::ValidateCall(o->GetClass(), f, b - 1);
		return 0;
	}
	catch (...)
	{
		JitStashException();
		return -1;
	}
}

void JitCompiler::EmitSCOPE()
{
	auto label = EmitThrowExceptionLabel(X_READ_NIL);
	cc.cbz(regA[A], label);

	auto f = LoadConstPtr(konsta[C].v);

	auto status = newResultInt32();
	auto call = CreateCall<int, DObject*, VMFunction*, int>(ValidateCall);
	call->set_ret(0, status);
	call->set_arg(0, regA[A]);
	call->set_arg(1, f);
	call->set_arg(2, asmjit::Imm(B));
	EmitCheckStatus(status);
}

static void SetString(VMReturn* ret, FString* str)
{
	ret->SetString(*str);
}

void JitCompiler::EmitRET()
{
	using namespace asmjit;
	if (B == REGT_NIL)
	{
		EmitPopFrame();
		Gp vReg = newTempInt32();
		cc.mov(vReg, Imm(0));
		cc.ret(vReg);
	}
	else
	{
		int a = A;
		int retnum = a & ~RET_FINAL;

		Gp location = newTempIntPtr();
		Label L_endif = cc.new_label();

		// if (retnum >= numret) skip the store
		EmitCmpImm32(numret, retnum);
		cc.b_le(L_endif);

		cc.ldr(location, MemOffset(ret, retnum * sizeof(VMReturn) + myoffsetof(VMReturn, Location), 8));

		int regtype = B;
		int regnum = C;
		switch (regtype & REGT_TYPE)
		{
		case REGT_INT:
			if (regtype & REGT_KONST)
				cc.str(LoadConstInt32(konstd[regnum]), a64::ptr(location));
			else
				cc.str(regD[regnum], a64::ptr(location));
			break;
		case REGT_FLOAT:
		{
			int count = 1;
			if (regtype & REGT_MULTIREG4) count = 4;
			else if (regtype & REGT_MULTIREG3) count = 3;
			else if (regtype & REGT_MULTIREG2) count = 2;

			if (regtype & REGT_KONST)
			{
				auto tmp = newTempInt64();
				for (int j = 0; j < count; j++)
				{
					cc.mov(tmp, Imm(((const int64_t *)konstf)[regnum + j]));
					cc.str(tmp, a64::ptr(location, j * 8));
				}
			}
			else
			{
				for (int j = 0; j < count; j++)
				{
					cc.str(regF[regnum + j], a64::ptr(location, j * 8));
				}
			}
			break;
		}
		case REGT_STRING:
		{
			auto ptr = newTempIntPtr();
			EmitAddOffset(ptr, ret, retnum * sizeof(VMReturn));
			auto str = (regtype & REGT_KONST) ? LoadConstPtr(&konsts[regnum]) : regS[regnum];
			auto call = CreateCall<void, VMReturn*, FString*>(SetString);
			call->set_arg(0, ptr);
			call->set_arg(1, str);
			break;
		}
		case REGT_POINTER:
			if (regtype & REGT_KONST)
			{
				cc.str(LoadConstPtr(konsta[regnum].v), a64::ptr(location));
			}
			else
			{
				cc.str(regA[regnum], a64::ptr(location));
			}
			break;
		}

		if (a & RET_FINAL)
		{
			EmitPopFrame();
			cc.ret(LoadConstInt32(retnum + 1));
		}

		cc.bind(L_endif);
		if (a & RET_FINAL)
		{
			EmitPopFrame();
			cc.ret(numret);
		}
	}
}

void JitCompiler::EmitRETI()
{
	using namespace asmjit;

	int a = A;
	int retnum = a & ~RET_FINAL;

	Gp location = newTempIntPtr();
	Label L_endif = cc.new_label();

	// if (retnum >= numret) skip the store
	EmitCmpImm32(numret, retnum);
	cc.b_le(L_endif);

	cc.ldr(location, MemOffset(ret, retnum * sizeof(VMReturn) + myoffsetof(VMReturn, Location), 8));
	cc.str(LoadConstInt32(BCs), a64::ptr(location));

	if (a & RET_FINAL)
	{
		EmitPopFrame();
		cc.ret(LoadConstInt32(retnum + 1));
	}

	cc.bind(L_endif);
	if (a & RET_FINAL)
	{
		EmitPopFrame();
		cc.ret(numret);
	}
}

void JitCompiler::EmitTHROW()
{
	EmitThrowException(EVMAbortException(BC));
}

void JitCompiler::EmitBOUND()
{
	auto cursor = cc.cursor();
	auto label = cc.new_label();
	cc.bind(label);
	auto call = CreateCall<void, int, int>(&JitCompiler::ThrowArrayOutOfBounds);
	call->set_arg(0, regD[A]);
	call->set_arg(1, asmjit::Imm((int)BC));
	cc.b(ErrorExitLabel());
	cc.set_cursor(cursor);

	EmitCmpImm32(regD[A], (int)BC);
	cc.b_hs(label);

	JitLineInfo info;
	info.Label = label;
	info.LineNumber = sfunc->PCToLine(pc);
	LineInfo.Push(info);
}

void JitCompiler::EmitBOUND_K()
{
	auto cursor = cc.cursor();
	auto label = cc.new_label();
	cc.bind(label);
	auto call = CreateCall<void, int, int>(&JitCompiler::ThrowArrayOutOfBounds);
	call->set_arg(0, regD[A]);
	call->set_arg(1, asmjit::Imm(konstd[BC]));
	cc.b(ErrorExitLabel());
	cc.set_cursor(cursor);

	EmitCmpImm32(regD[A], konstd[BC]);
	cc.b_hs(label);

	JitLineInfo info;
	info.Label = label;
	info.LineNumber = sfunc->PCToLine(pc);
	LineInfo.Push(info);
}

void JitCompiler::EmitBOUND_R()
{
	auto cursor = cc.cursor();
	auto label = cc.new_label();
	cc.bind(label);
	auto call = CreateCall<void, int, int>(&JitCompiler::ThrowArrayOutOfBounds);
	call->set_arg(0, regD[A]);
	call->set_arg(1, regD[B]);
	cc.b(ErrorExitLabel());
	cc.set_cursor(cursor);

	cc.cmp(regD[A], regD[B]);
	cc.b_hs(label);

	JitLineInfo info;
	info.Label = label;
	info.LineNumber = sfunc->PCToLine(pc);
	LineInfo.Push(info);
}

void JitCompiler::ThrowArrayOutOfBounds(int index, int size)
{
	try
	{
		if (index >= size)
		{
			ThrowAbortException(X_ARRAY_OUT_OF_BOUNDS, "Size = %u, current index = %u\n", size, index);
		}
		else
		{
			ThrowAbortException(X_ARRAY_OUT_OF_BOUNDS, "Negative current index = %i\n", index);
		}
	}
	catch (...)
	{
		JitStashException();
	}
}
