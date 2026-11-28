/*
** jit_store.cpp
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

void JitCompiler::EmitSB()
{
	EmitNullPointerThrow(A, X_WRITE_NIL);
	cc.strb(regD[B], MemOffset(regA[A], konstd[C], 1));
}

void JitCompiler::EmitSB_R()
{
	EmitNullPointerThrow(A, X_WRITE_NIL);
	cc.strb(regD[B], MemIndex(regA[A], regD[C]));
}

void JitCompiler::EmitSH()
{
	EmitNullPointerThrow(A, X_WRITE_NIL);
	cc.strh(regD[B], MemOffset(regA[A], konstd[C], 2));
}

void JitCompiler::EmitSH_R()
{
	EmitNullPointerThrow(A, X_WRITE_NIL);
	cc.strh(regD[B], MemIndex(regA[A], regD[C]));
}

void JitCompiler::EmitSW()
{
	EmitNullPointerThrow(A, X_WRITE_NIL);
	cc.str(regD[B], MemOffset(regA[A], konstd[C], 4));
}

void JitCompiler::EmitSW_R()
{
	EmitNullPointerThrow(A, X_WRITE_NIL);
	cc.str(regD[B], MemIndex(regA[A], regD[C]));
}

void JitCompiler::EmitSSP()
{
	EmitNullPointerThrow(A, X_WRITE_NIL);
	auto tmp = newTempFloat32();
	cc.fcvt(tmp, regF[B]);
	cc.str(tmp, MemOffset(regA[A], konstd[C], 4));
}

void JitCompiler::EmitSSP_R()
{
	EmitNullPointerThrow(A, X_WRITE_NIL);
	auto tmp = newTempFloat32();
	cc.fcvt(tmp, regF[B]);
	cc.str(tmp, MemIndex(regA[A], regD[C]));
}

void JitCompiler::EmitSDP()
{
	EmitNullPointerThrow(A, X_WRITE_NIL);
	cc.str(regF[B], MemOffset(regA[A], konstd[C], 8));
}

void JitCompiler::EmitSDP_R()
{
	EmitNullPointerThrow(A, X_WRITE_NIL);
	cc.str(regF[B], MemIndex(regA[A], regD[C]));
}

void JitCompiler::EmitSS()
{
	EmitNullPointerThrow(A, X_WRITE_NIL);
	auto ptr = newTempIntPtr();
	EmitAddOffset(ptr, regA[A], konstd[C]);
	auto call = CreateCall<void, FString*, FString*>(&JitCompiler::CallAssignString);
	call->set_arg(0, ptr);
	call->set_arg(1, regS[B]);
}

void JitCompiler::EmitSS_R()
{
	EmitNullPointerThrow(A, X_WRITE_NIL);
	auto ptr = newTempIntPtr();
	cc.add(ptr, regA[A], regD[C], asmjit::a64::sxtw(0));
	auto call = CreateCall<void, FString*, FString*>(&JitCompiler::CallAssignString);
	call->set_arg(0, ptr);
	call->set_arg(1, regS[B]);
}

void JitCompiler::EmitSO()
{
	EmitNullPointerThrow(A, X_WRITE_NIL);
	cc.str(regA[B], MemOffset(regA[A], konstd[C], 8));

	typedef void(*FuncPtr)(DObject*);
	auto call = CreateCall<void, DObject*>(static_cast<FuncPtr>(GC::WriteBarrier));
	call->set_arg(0, regA[B]);
}

void JitCompiler::EmitSO_R()
{
	EmitNullPointerThrow(A, X_WRITE_NIL);
	cc.str(regA[B], MemIndex(regA[A], regD[C]));

	typedef void(*FuncPtr)(DObject*);
	auto call = CreateCall<void, DObject*>(static_cast<FuncPtr>(GC::WriteBarrier));
	call->set_arg(0, regA[B]);
}

void JitCompiler::EmitSP()
{
	EmitNullPointerThrow(A, X_WRITE_NIL);
	cc.str(regA[B], MemOffset(regA[A], konstd[C], 8));
}

void JitCompiler::EmitSP_R()
{
	EmitNullPointerThrow(A, X_WRITE_NIL);
	cc.str(regA[B], MemIndex(regA[A], regD[C]));
}

void JitCompiler::EmitSV2()
{
	EmitNullPointerThrow(A, X_WRITE_NIL);
	auto tmp = newTempIntPtr();
	EmitAddOffset(tmp, regA[A], konstd[C]);
	cc.str(regF[B], asmjit::a64::ptr(tmp));
	cc.str(regF[B + 1], asmjit::a64::ptr(tmp, 8));
}

void JitCompiler::EmitSV2_R()
{
	EmitNullPointerThrow(A, X_WRITE_NIL);
	auto tmp = newTempIntPtr();
	cc.add(tmp, regA[A], regD[C], asmjit::a64::sxtw(0));
	cc.str(regF[B], asmjit::a64::ptr(tmp));
	cc.str(regF[B + 1], asmjit::a64::ptr(tmp, 8));
}

void JitCompiler::EmitSV3()
{
	EmitNullPointerThrow(A, X_WRITE_NIL);
	auto tmp = newTempIntPtr();
	EmitAddOffset(tmp, regA[A], konstd[C]);
	cc.str(regF[B], asmjit::a64::ptr(tmp));
	cc.str(regF[B + 1], asmjit::a64::ptr(tmp, 8));
	cc.str(regF[B + 2], asmjit::a64::ptr(tmp, 16));
}

void JitCompiler::EmitSV3_R()
{
	EmitNullPointerThrow(A, X_WRITE_NIL);
	auto tmp = newTempIntPtr();
	cc.add(tmp, regA[A], regD[C], asmjit::a64::sxtw(0));
	cc.str(regF[B], asmjit::a64::ptr(tmp));
	cc.str(regF[B + 1], asmjit::a64::ptr(tmp, 8));
	cc.str(regF[B + 2], asmjit::a64::ptr(tmp, 16));
}

void JitCompiler::EmitSV4()
{
	EmitNullPointerThrow(A, X_WRITE_NIL);
	auto tmp = newTempIntPtr();
	EmitAddOffset(tmp, regA[A], konstd[C]);
	cc.str(regF[B], asmjit::a64::ptr(tmp));
	cc.str(regF[B + 1], asmjit::a64::ptr(tmp, 8));
	cc.str(regF[B + 2], asmjit::a64::ptr(tmp, 16));
	cc.str(regF[B + 3], asmjit::a64::ptr(tmp, 24));
}

void JitCompiler::EmitSV4_R()
{
	EmitNullPointerThrow(A, X_WRITE_NIL);
	auto tmp = newTempIntPtr();
	cc.add(tmp, regA[A], regD[C], asmjit::a64::sxtw(0));
	cc.str(regF[B], asmjit::a64::ptr(tmp));
	cc.str(regF[B + 1], asmjit::a64::ptr(tmp, 8));
	cc.str(regF[B + 2], asmjit::a64::ptr(tmp, 16));
	cc.str(regF[B + 3], asmjit::a64::ptr(tmp, 24));
}

void JitCompiler::EmitSFV2()
{
	EmitNullPointerThrow(A, X_WRITE_NIL);
	auto tmp = newTempIntPtr();
	EmitAddOffset(tmp, regA[A], konstd[C]);
	auto tmpF = newTempFloat32();
	for (int i = 0; i < 2; i++)
	{
		cc.fcvt(tmpF, regF[B + i]);
		cc.str(tmpF, asmjit::a64::ptr(tmp, i * 4));
	}
}

void JitCompiler::EmitSFV2_R()
{
	EmitNullPointerThrow(A, X_WRITE_NIL);
	auto tmp = newTempIntPtr();
	cc.add(tmp, regA[A], regD[C], asmjit::a64::sxtw(0));
	auto tmpF = newTempFloat32();
	for (int i = 0; i < 2; i++)
	{
		cc.fcvt(tmpF, regF[B + i]);
		cc.str(tmpF, asmjit::a64::ptr(tmp, i * 4));
	}
}

void JitCompiler::EmitSFV3()
{
	EmitNullPointerThrow(A, X_WRITE_NIL);
	auto tmp = newTempIntPtr();
	EmitAddOffset(tmp, regA[A], konstd[C]);
	auto tmpF = newTempFloat32();
	for (int i = 0; i < 3; i++)
	{
		cc.fcvt(tmpF, regF[B + i]);
		cc.str(tmpF, asmjit::a64::ptr(tmp, i * 4));
	}
}

void JitCompiler::EmitSFV3_R()
{
	EmitNullPointerThrow(A, X_WRITE_NIL);
	auto tmp = newTempIntPtr();
	cc.add(tmp, regA[A], regD[C], asmjit::a64::sxtw(0));
	auto tmpF = newTempFloat32();
	for (int i = 0; i < 3; i++)
	{
		cc.fcvt(tmpF, regF[B + i]);
		cc.str(tmpF, asmjit::a64::ptr(tmp, i * 4));
	}
}

void JitCompiler::EmitSFV4()
{
	EmitNullPointerThrow(A, X_WRITE_NIL);
	auto tmp = newTempIntPtr();
	EmitAddOffset(tmp, regA[A], konstd[C]);
	auto tmpF = newTempFloat32();
	for (int i = 0; i < 4; i++)
	{
		cc.fcvt(tmpF, regF[B + i]);
		cc.str(tmpF, asmjit::a64::ptr(tmp, i * 4));
	}
}

void JitCompiler::EmitSFV4_R()
{
	EmitNullPointerThrow(A, X_WRITE_NIL);
	auto tmp = newTempIntPtr();
	cc.add(tmp, regA[A], regD[C], asmjit::a64::sxtw(0));
	auto tmpF = newTempFloat32();
	for (int i = 0; i < 4; i++)
	{
		cc.fcvt(tmpF, regF[B + i]);
		cc.str(tmpF, asmjit::a64::ptr(tmp, i * 4));
	}
}

void JitCompiler::EmitSBIT()
{
	EmitNullPointerThrow(A, X_WRITE_NIL);
	auto bits = LoadConstInt32(C);
	auto value = newTempInt32();
	auto set = newTempInt32();
	auto cleared = newTempInt32();
	cc.ldrb(value, asmjit::a64::ptr(regA[A]));
	cc.orr(set, value, bits);
	cc.bic(cleared, value, bits);
	cc.cmp(regD[B], asmjit::Imm(0));
	cc.csel(value, set, cleared, asmjit::arm::CondCode::kNE);
	cc.strb(value, asmjit::a64::ptr(regA[A]));
}
