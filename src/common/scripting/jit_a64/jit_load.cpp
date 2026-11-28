/*
** jit_load.cpp
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

static_assert(sizeof(FString) == 8, "LKS_R scales the index by 8");
static_assert(sizeof(FVoidObj) == 8, "LKP_R scales the index by 8");

/////////////////////////////////////////////////////////////////////////////
// Load constants.

void JitCompiler::EmitLI()
{
	cc.mov(regD[A], asmjit::Imm((int)BCs));
}

void JitCompiler::EmitLK()
{
	cc.mov(regD[A], asmjit::Imm(konstd[BC]));
}

void JitCompiler::EmitLKF()
{
	LoadConstFloat64(regF[A], konstf[BC]);
}

void JitCompiler::EmitLKS()
{
	// Everything that emits instructions has to come before CreateCall, the call is emitted right away
	auto from = LoadConstPtr(konsts + BC);
	auto call = CreateCall<void, FString*, FString*>(&JitCompiler::CallAssignString);
	call->set_arg(0, regS[A]);
	call->set_arg(1, from);
}

void JitCompiler::EmitLKP()
{
	cc.mov(regA[A], asmjit::Imm((uint64_t)(uintptr_t)konsta[BC].v));
}

void JitCompiler::EmitLK_R()
{
	auto base = LoadConstPtr(konstd + C);
	cc.ldr(regD[A], MemIndex(base, regD[B], 2));
}

void JitCompiler::EmitLKF_R()
{
	auto base = LoadConstPtr(konstf + C);
	cc.ldr(regF[A], MemIndex(base, regD[B], 3));
}

void JitCompiler::EmitLKS_R()
{
	auto base = LoadConstPtr(konsts + C);
	auto ptr = newTempIntPtr();
	cc.add(ptr, base, regD[B], asmjit::a64::sxtw(3));
	auto call = CreateCall<void, FString*, FString*>(&JitCompiler::CallAssignString);
	call->set_arg(0, regS[A]);
	call->set_arg(1, ptr);
}

void JitCompiler::EmitLKP_R()
{
	auto base = LoadConstPtr(konsta + C);
	cc.ldr(regA[A], MemIndex(base, regD[B], 3));
}

void JitCompiler::EmitLFP()
{
	CheckVMFrame();
	EmitAddOffset(regA[A], vmframe, offsetExtra);
}

void JitCompiler::EmitMETA()
{
	auto label = EmitThrowExceptionLabel(X_READ_NIL);
	cc.cbz(regA[B], label);

	cc.ldr(regA[A], MemOffset(regA[B], myoffsetof(DObject, Class), 8));
	cc.ldr(regA[A], MemOffset(regA[A], myoffsetof(PClass, Meta), 8));
}

void JitCompiler::EmitCLSS()
{
	auto label = EmitThrowExceptionLabel(X_READ_NIL);
	cc.cbz(regA[B], label);
	cc.ldr(regA[A], MemOffset(regA[B], myoffsetof(DObject, Class), 8));
}

/////////////////////////////////////////////////////////////////////////////
// Load from memory. rA = *(rB + rkC)

void JitCompiler::EmitLB()
{
	EmitNullPointerThrow(B, X_READ_NIL);
	cc.ldrsb(regD[A], MemOffset(regA[B], konstd[C], 1));
}

void JitCompiler::EmitLB_R()
{
	EmitNullPointerThrow(B, X_READ_NIL);
	cc.ldrsb(regD[A], MemIndex(regA[B], regD[C]));
}

void JitCompiler::EmitLH()
{
	EmitNullPointerThrow(B, X_READ_NIL);
	cc.ldrsh(regD[A], MemOffset(regA[B], konstd[C], 2));
}

void JitCompiler::EmitLH_R()
{
	EmitNullPointerThrow(B, X_READ_NIL);
	cc.ldrsh(regD[A], MemIndex(regA[B], regD[C]));
}

void JitCompiler::EmitLW()
{
	EmitNullPointerThrow(B, X_READ_NIL);
	cc.ldr(regD[A], MemOffset(regA[B], konstd[C], 4));
}

void JitCompiler::EmitLW_R()
{
	EmitNullPointerThrow(B, X_READ_NIL);
	cc.ldr(regD[A], MemIndex(regA[B], regD[C]));
}

void JitCompiler::EmitLBU()
{
	EmitNullPointerThrow(B, X_READ_NIL);
	cc.ldrb(regD[A], MemOffset(regA[B], konstd[C], 1));
}

void JitCompiler::EmitLBU_R()
{
	EmitNullPointerThrow(B, X_READ_NIL);
	cc.ldrb(regD[A], MemIndex(regA[B], regD[C]));
}

void JitCompiler::EmitLHU()
{
	EmitNullPointerThrow(B, X_READ_NIL);
	cc.ldrh(regD[A], MemOffset(regA[B], konstd[C], 2));
}

void JitCompiler::EmitLHU_R()
{
	EmitNullPointerThrow(B, X_READ_NIL);
	cc.ldrh(regD[A], MemIndex(regA[B], regD[C]));
}

void JitCompiler::EmitLSP()
{
	EmitNullPointerThrow(B, X_READ_NIL);
	auto tmp = newTempFloat32();
	cc.ldr(tmp, MemOffset(regA[B], konstd[C], 4));
	cc.fcvt(regF[A], tmp);
}

void JitCompiler::EmitLSP_R()
{
	EmitNullPointerThrow(B, X_READ_NIL);
	auto tmp = newTempFloat32();
	cc.ldr(tmp, MemIndex(regA[B], regD[C]));
	cc.fcvt(regF[A], tmp);
}

void JitCompiler::EmitLDP()
{
	EmitNullPointerThrow(B, X_READ_NIL);
	cc.ldr(regF[A], MemOffset(regA[B], konstd[C], 8));
}

void JitCompiler::EmitLDP_R()
{
	EmitNullPointerThrow(B, X_READ_NIL);
	cc.ldr(regF[A], MemIndex(regA[B], regD[C]));
}

void JitCompiler::EmitLS()
{
	EmitNullPointerThrow(B, X_READ_NIL);
	auto ptr = newTempIntPtr();
	EmitAddOffset(ptr, regA[B], konstd[C]);
	auto call = CreateCall<void, FString*, FString*>(&JitCompiler::CallAssignString);
	call->set_arg(0, regS[A]);
	call->set_arg(1, ptr);
}

void JitCompiler::EmitLS_R()
{
	EmitNullPointerThrow(B, X_READ_NIL);
	auto ptr = newTempIntPtr();
	cc.add(ptr, regA[B], regD[C], asmjit::a64::sxtw(0));
	auto call = CreateCall<void, FString*, FString*>(&JitCompiler::CallAssignString);
	call->set_arg(0, regS[A]);
	call->set_arg(1, ptr);
}

// Inline read barrier: a pointer to an object with OF_EuthanizeMe reads as null
void JitCompiler::EmitReadBarrier()
{
	static_assert(OF_EuthanizeMe == (1 << 5), "the read barrier tests bit 5");

	auto isnull = cc.new_label();
	cc.cbz(regA[A], isnull);

	auto mask = newTempIntPtr();
	cc.ldr(mask.w(), MemOffset(regA[A], myoffsetof(DObject, ObjectFlags), 4));
	cc.sbfx(mask, mask, asmjit::Imm(5), asmjit::Imm(1)); // all bits set if OF_EuthanizeMe was set
	cc.bic(regA[A], regA[A], mask);

	cc.bind(isnull);
}

void JitCompiler::EmitLO()
{
	EmitNullPointerThrow(B, X_READ_NIL);

	cc.ldr(regA[A], MemOffset(regA[B], konstd[C], 8));
	EmitReadBarrier();
}

void JitCompiler::EmitLO_R()
{
	EmitNullPointerThrow(B, X_READ_NIL);

	cc.ldr(regA[A], MemIndex(regA[B], regD[C]));
	EmitReadBarrier();
}

void JitCompiler::EmitLP()
{
	EmitNullPointerThrow(B, X_READ_NIL);
	cc.ldr(regA[A], MemOffset(regA[B], konstd[C], 8));
}

void JitCompiler::EmitLP_R()
{
	EmitNullPointerThrow(B, X_READ_NIL);
	cc.ldr(regA[A], MemIndex(regA[B], regD[C]));
}

void JitCompiler::EmitLV2()
{
	EmitNullPointerThrow(B, X_READ_NIL);
	auto tmp = newTempIntPtr();
	EmitAddOffset(tmp, regA[B], konstd[C]);
	cc.ldr(regF[A], asmjit::a64::ptr(tmp));
	cc.ldr(regF[A + 1], asmjit::a64::ptr(tmp, 8));
}

void JitCompiler::EmitLV2_R()
{
	EmitNullPointerThrow(B, X_READ_NIL);
	auto tmp = newTempIntPtr();
	cc.add(tmp, regA[B], regD[C], asmjit::a64::sxtw(0));
	cc.ldr(regF[A], asmjit::a64::ptr(tmp));
	cc.ldr(regF[A + 1], asmjit::a64::ptr(tmp, 8));
}

void JitCompiler::EmitLV3()
{
	EmitNullPointerThrow(B, X_READ_NIL);
	auto tmp = newTempIntPtr();
	EmitAddOffset(tmp, regA[B], konstd[C]);
	cc.ldr(regF[A], asmjit::a64::ptr(tmp));
	cc.ldr(regF[A + 1], asmjit::a64::ptr(tmp, 8));
	cc.ldr(regF[A + 2], asmjit::a64::ptr(tmp, 16));
}

void JitCompiler::EmitLV3_R()
{
	EmitNullPointerThrow(B, X_READ_NIL);
	auto tmp = newTempIntPtr();
	cc.add(tmp, regA[B], regD[C], asmjit::a64::sxtw(0));
	cc.ldr(regF[A], asmjit::a64::ptr(tmp));
	cc.ldr(regF[A + 1], asmjit::a64::ptr(tmp, 8));
	cc.ldr(regF[A + 2], asmjit::a64::ptr(tmp, 16));
}

void JitCompiler::EmitLV4()
{
	EmitNullPointerThrow(B, X_READ_NIL);
	auto tmp = newTempIntPtr();
	EmitAddOffset(tmp, regA[B], konstd[C]);
	cc.ldr(regF[A], asmjit::a64::ptr(tmp));
	cc.ldr(regF[A + 1], asmjit::a64::ptr(tmp, 8));
	cc.ldr(regF[A + 2], asmjit::a64::ptr(tmp, 16));
	cc.ldr(regF[A + 3], asmjit::a64::ptr(tmp, 24));
}

void JitCompiler::EmitLV4_R()
{
	EmitNullPointerThrow(B, X_READ_NIL);
	auto tmp = newTempIntPtr();
	cc.add(tmp, regA[B], regD[C], asmjit::a64::sxtw(0));
	cc.ldr(regF[A], asmjit::a64::ptr(tmp));
	cc.ldr(regF[A + 1], asmjit::a64::ptr(tmp, 8));
	cc.ldr(regF[A + 2], asmjit::a64::ptr(tmp, 16));
	cc.ldr(regF[A + 3], asmjit::a64::ptr(tmp, 24));
}

void JitCompiler::EmitLFV2()
{
	EmitNullPointerThrow(B, X_READ_NIL);
	auto tmp = newTempIntPtr();
	EmitAddOffset(tmp, regA[B], konstd[C]);
	auto tmpF = newTempFloat32();
	for (int i = 0; i < 2; i++)
	{
		cc.ldr(tmpF, asmjit::a64::ptr(tmp, i * 4));
		cc.fcvt(regF[A + i], tmpF);
	}
}

void JitCompiler::EmitLFV2_R()
{
	EmitNullPointerThrow(B, X_READ_NIL);
	auto tmp = newTempIntPtr();
	cc.add(tmp, regA[B], regD[C], asmjit::a64::sxtw(0));
	auto tmpF = newTempFloat32();
	for (int i = 0; i < 2; i++)
	{
		cc.ldr(tmpF, asmjit::a64::ptr(tmp, i * 4));
		cc.fcvt(regF[A + i], tmpF);
	}
}

void JitCompiler::EmitLFV3()
{
	EmitNullPointerThrow(B, X_READ_NIL);
	auto tmp = newTempIntPtr();
	EmitAddOffset(tmp, regA[B], konstd[C]);
	auto tmpF = newTempFloat32();
	for (int i = 0; i < 3; i++)
	{
		cc.ldr(tmpF, asmjit::a64::ptr(tmp, i * 4));
		cc.fcvt(regF[A + i], tmpF);
	}
}

void JitCompiler::EmitLFV3_R()
{
	EmitNullPointerThrow(B, X_READ_NIL);
	auto tmp = newTempIntPtr();
	cc.add(tmp, regA[B], regD[C], asmjit::a64::sxtw(0));
	auto tmpF = newTempFloat32();
	for (int i = 0; i < 3; i++)
	{
		cc.ldr(tmpF, asmjit::a64::ptr(tmp, i * 4));
		cc.fcvt(regF[A + i], tmpF);
	}
}

void JitCompiler::EmitLFV4()
{
	EmitNullPointerThrow(B, X_READ_NIL);
	auto tmp = newTempIntPtr();
	EmitAddOffset(tmp, regA[B], konstd[C]);
	auto tmpF = newTempFloat32();
	for (int i = 0; i < 4; i++)
	{
		cc.ldr(tmpF, asmjit::a64::ptr(tmp, i * 4));
		cc.fcvt(regF[A + i], tmpF);
	}
}

void JitCompiler::EmitLFV4_R()
{
	EmitNullPointerThrow(B, X_READ_NIL);
	auto tmp = newTempIntPtr();
	cc.add(tmp, regA[B], regD[C], asmjit::a64::sxtw(0));
	auto tmpF = newTempFloat32();
	for (int i = 0; i < 4; i++)
	{
		cc.ldr(tmpF, asmjit::a64::ptr(tmp, i * 4));
		cc.fcvt(regF[A + i], tmpF);
	}
}

static void SetString(FString *to, char **from)
{
	*to = *from;
}

void JitCompiler::EmitLCS()
{
	EmitNullPointerThrow(B, X_READ_NIL);
	auto ptr = newTempIntPtr();
	EmitAddOffset(ptr, regA[B], konstd[C]);
	auto call = CreateCall<void, FString*, char**>(SetString);
	call->set_arg(0, regS[A]);
	call->set_arg(1, ptr);
}

void JitCompiler::EmitLCS_R()
{
	EmitNullPointerThrow(B, X_READ_NIL);
	auto ptr = newTempIntPtr();
	cc.add(ptr, regA[B], regD[C], asmjit::a64::sxtw(0));
	auto call = CreateCall<void, FString*, char**>(SetString);
	call->set_arg(0, regS[A]);
	call->set_arg(1, ptr);
}

void JitCompiler::EmitLBIT()
{
	EmitNullPointerThrow(B, X_READ_NIL);
	auto tmp = newTempInt32();
	cc.ldrb(tmp, asmjit::a64::ptr(regA[B]));
	cc.tst(tmp, LoadConstInt32(C));
	cc.cset(regD[A], asmjit::arm::CondCode::kNE);
}
