/*
** jit_move.cpp
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
#include "v_video.h"
#include "s_soundinternal.h"
#include "texturemanager.h"
#include "palutil.h"

void JitCompiler::EmitMOVE()
{
	cc.mov(regD[A], regD[B]);
}
void JitCompiler::EmitMOVEF()
{
	cc.fmov(regF[A], regF[B]);
}

void JitCompiler::EmitMOVES()
{
	auto call = CreateCall<void, FString*, FString*>(&JitCompiler::CallAssignString);
	call->set_arg(0, regS[A]);
	call->set_arg(1, regS[B]);
}

void JitCompiler::EmitMOVEA()
{
	cc.mov(regA[A], regA[B]);
}

void JitCompiler::EmitMOVEV2()
{
	cc.fmov(regF[A], regF[B]);
	cc.fmov(regF[A + 1], regF[B + 1]);
}

void JitCompiler::EmitMOVEV3()
{
	cc.fmov(regF[A], regF[B]);
	cc.fmov(regF[A + 1], regF[B + 1]);
	cc.fmov(regF[A + 2], regF[B + 2]);
}

void JitCompiler::EmitMOVEV4()
{
	cc.fmov(regF[A], regF[B]);
	cc.fmov(regF[A + 1], regF[B + 1]);
	cc.fmov(regF[A + 2], regF[B + 2]);
	cc.fmov(regF[A + 3], regF[B + 3]);
}

static void CastI2S(FString *a, int b) { a->Format("%d", b); }
static void CastU2S(FString *a, int b) { a->Format("%u", b); }
static void CastF2S(FString *a, double b) { a->Format("%.5f", b); }
static void CastV22S(FString *a, double b, double b1) { a->Format("(%.5f, %.5f)", b, b1); }
static void CastV32S(FString *a, double b, double b1, double b2) { a->Format("(%.5f, %.5f, %.5f)", b, b1, b2); }
static void CastV42S(FString *a, double b, double b1, double b2, double b3) { a->Format("(%.5f, %.5f, %.5f, %.5f)", b, b1, b2, b3); }
static void CastP2S(FString *a, void *b) { if (b == nullptr) *a = "null"; else a->Format("%p", b); }
static int CastS2I(FString *b) { return (int)b->ToLong(); }
static double CastS2F(FString *b) { return b->ToDouble(); }
static int CastS2N(FString *b) { return b->Len() == 0 ? NAME_None : FName(*b).GetIndex(); }
static void CastN2S(FString *a, int b) { FName name = FName(ENamedName(b)); *a = name.IsValidName() ? name.GetChars() : ""; }
static int CastS2Co(FString *b) { return V_GetColor(b->GetChars()); }
static void CastCo2S(FString *a, int b) { PalEntry c(b); a->Format("%02x %02x %02x", c.r, c.g, c.b); }
static int CastS2So(FString *b) { return S_FindSound(*b).index(); }
static void CastSo2S(FString* a, int b) { *a = soundEngine->GetSoundName(FSoundID::fromInt(b)); }
static void CastSID2S(FString* a, unsigned int b) { VM_CastSpriteIDToString(a, b); }
static void CastTID2S(FString *a, int b) { auto tex = TexMan.GetGameTexture(*(FTextureID*)&b); *a = (tex == nullptr) ? "(null)" : tex->GetName().GetChars(); }

void JitCompiler::EmitCAST()
{
	asmjit::a64::Gp tmp, resultD;
	asmjit::a64::Vec resultF;
	asmjit::InvokeNode *call = nullptr;

	switch (C)
	{
	case CAST_I2F:
		cc.scvtf(regF[A], regD[B]);
		break;
	case CAST_U2F:
		cc.ucvtf(regF[A], regD[B]);
		break;
	case CAST_F2I:
		cc.fcvtzs(regD[A], regF[B]);
		break;
	case CAST_F2U:
		// Through a 64-bit integer like the x86 version: negative values wrap instead of becoming 0
		tmp = newTempInt64();
		cc.fcvtzs(tmp, regF[B]);
		cc.mov(regD[A], tmp.w());
		break;
	case CAST_I2S:
		call = CreateCall<void, FString*, int>(CastI2S);
		call->set_arg(0, regS[A]);
		call->set_arg(1, regD[B]);
		break;
	case CAST_U2S:
		call = CreateCall<void, FString*, int>(CastU2S);
		call->set_arg(0, regS[A]);
		call->set_arg(1, regD[B]);
		break;
	case CAST_F2S:
		call = CreateCall<void, FString*, double>(CastF2S);
		call->set_arg(0, regS[A]);
		call->set_arg(1, regF[B]);
		break;
	case CAST_V22S:
		call = CreateCall<void, FString*, double, double>(CastV22S);
		call->set_arg(0, regS[A]);
		call->set_arg(1, regF[B]);
		call->set_arg(2, regF[B + 1]);
		break;
	case CAST_V32S:
		call = CreateCall<void, FString*, double, double, double>(CastV32S);
		call->set_arg(0, regS[A]);
		call->set_arg(1, regF[B]);
		call->set_arg(2, regF[B + 1]);
		call->set_arg(3, regF[B + 2]);
		break;
	case CAST_V42S:
		call = CreateCall<void, FString*, double, double, double, double>(CastV42S);
		call->set_arg(0, regS[A]);
		call->set_arg(1, regF[B]);
		call->set_arg(2, regF[B + 1]);
		call->set_arg(3, regF[B + 2]);
		call->set_arg(4, regF[B + 3]);
		break;
	case CAST_P2S:
		call = CreateCall<void, FString*, void*>(CastP2S);
		call->set_arg(0, regS[A]);
		call->set_arg(1, regA[B]);
		break;
	case CAST_S2I:
		resultD = newResultInt32();
		call = CreateCall<int, FString*>(CastS2I);
		call->set_ret(0, resultD);
		call->set_arg(0, regS[B]);
		cc.mov(regD[A], resultD);
		break;
	case CAST_S2F:
		resultF = newResultFloat64();
		call = CreateCall<double, FString*>(CastS2F);
		call->set_ret(0, resultF);
		call->set_arg(0, regS[B]);
		cc.fmov(regF[A], resultF);
		break;
	case CAST_S2N:
		resultD = newResultInt32();
		call = CreateCall<int, FString*>(CastS2N);
		call->set_ret(0, resultD);
		call->set_arg(0, regS[B]);
		cc.mov(regD[A], resultD);
		break;
	case CAST_N2S:
		call = CreateCall<void, FString*, int>(CastN2S);
		call->set_arg(0, regS[A]);
		call->set_arg(1, regD[B]);
		break;
	case CAST_S2Co:
		resultD = newResultInt32();
		call = CreateCall<int, FString*>(CastS2Co);
		call->set_ret(0, resultD);
		call->set_arg(0, regS[B]);
		cc.mov(regD[A], resultD);
		break;
	case CAST_Co2S:
		call = CreateCall<void, FString*, int>(CastCo2S);
		call->set_arg(0, regS[A]);
		call->set_arg(1, regD[B]);
		break;
	case CAST_S2So:
		resultD = newResultInt32();
		call = CreateCall<int, FString*>(CastS2So);
		call->set_ret(0, resultD);
		call->set_arg(0, regS[B]);
		cc.mov(regD[A], resultD);
		break;
	case CAST_So2S:
		call = CreateCall<void, FString*, int>(CastSo2S);
		call->set_arg(0, regS[A]);
		call->set_arg(1, regD[B]);
		break;
	case CAST_SID2S:
		call = CreateCall<void, FString*, unsigned int>(CastSID2S);
		call->set_arg(0, regS[A]);
		call->set_arg(1, regD[B]);
		break;
	case CAST_TID2S:
		call = CreateCall<void, FString*, int>(CastTID2S);
		call->set_arg(0, regS[A]);
		call->set_arg(1, regD[B]);
		break;
	default:
		I_Error("Unknown OP_CAST type\n");
	}
}

static int CastB_S(FString *s) { return s->Len() > 0; }

void JitCompiler::EmitCASTB()
{
	if (C == CASTB_I)
	{
		cc.cmp(regD[B], asmjit::Imm(0));
		cc.cset(regD[A], asmjit::arm::CondCode::kNE);
	}
	else if (C == CASTB_F)
	{
		// NaN is true, like the x86 version (NE is set for unordered)
		cc.fcmp(regF[B], asmjit::Imm(0));
		cc.cset(regD[A], asmjit::arm::CondCode::kNE);
	}
	else if (C == CASTB_A)
	{
		cc.cmp(regA[B], asmjit::Imm(0));
		cc.cset(regD[A], asmjit::arm::CondCode::kNE);
	}
	else
	{
		auto result = newResultInt32();
		auto call = CreateCall<int, FString*>(CastB_S);
		call->set_ret(0, result);
		call->set_arg(0, regS[B]);
		cc.mov(regD[A], result);
	}
}

static DObject *DynCast(DObject *obj, PClass *cls)
{
	return (obj && obj->IsKindOf(cls)) ? obj : nullptr;
}

void JitCompiler::EmitDYNCAST_R()
{
	auto result = newResultIntPtr();
	auto call = CreateCall<DObject*, DObject*, PClass*>(DynCast);
	call->set_ret(0, result);
	call->set_arg(0, regA[B]);
	call->set_arg(1, regA[C]);
	cc.mov(regA[A], result);
}

void JitCompiler::EmitDYNCAST_K()
{
	auto result = newResultIntPtr();
	auto c = LoadConstPtr(konsta[C].o);
	auto call = CreateCall<DObject*, DObject*, PClass*>(DynCast);
	call->set_ret(0, result);
	call->set_arg(0, regA[B]);
	call->set_arg(1, c);
	cc.mov(regA[A], result);
}

static PClass *DynCastC(PClass *cls1, PClass *cls2)
{
	return (cls1 && cls1->IsDescendantOf(cls2)) ? cls1 : nullptr;
}

void JitCompiler::EmitDYNCASTC_R()
{
	auto result = newResultIntPtr();
	auto call = CreateCall<PClass*, PClass*, PClass*>(DynCastC);
	call->set_ret(0, result);
	call->set_arg(0, regA[B]);
	call->set_arg(1, regA[C]);
	cc.mov(regA[A], result);
}

void JitCompiler::EmitDYNCASTC_K()
{
	auto result = newResultIntPtr();
	auto c = LoadConstPtr(konsta[C].o);
	auto call = CreateCall<PClass*, PClass*, PClass*>(DynCastC);
	call->set_ret(0, result);
	call->set_arg(0, regA[B]);
	call->set_arg(1, c);
	cc.mov(regA[A], result);
}
