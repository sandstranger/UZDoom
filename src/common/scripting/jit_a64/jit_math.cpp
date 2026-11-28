/*
** jit_math.cpp
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
** Float comparisons: after FCMP the EQ, MI (less), LS (less or equal), GT and GE
** conditions are false when the operands are unordered (NaN), NE, PL, HI, LE and
** LT are true. The branches below are picked to match ucomisd plus the parity
** checks of the x86 version.
*/

#include "jitintern.h"
#include "basics.h"

using asmjit::arm::CondCode;

/////////////////////////////////////////////////////////////////////////////
// String instructions.

static void ConcatString(FString* to, FString* first, FString* second)
{
	*to = *first + *second;
}

void JitCompiler::EmitCONCAT()
{
	auto rc = CheckRegS(C, A);
	auto call = CreateCall<void, FString*, FString*, FString*>(ConcatString);
	call->set_arg(0, regS[A]);
	call->set_arg(1, regS[B]);
	call->set_arg(2, rc);
}

static int StringLength(FString* str)
{
	return static_cast<int>(str->Len());
}

void JitCompiler::EmitLENS()
{
	auto result = newResultInt32();
	auto call = CreateCall<int, FString*>(StringLength);
	call->set_ret(0, result);
	call->set_arg(0, regS[B]);
	cc.mov(regD[A], result);
}

static int StringCompareNoCase(FString* first, FString* second)
{
	return first->CompareNoCase(*second);
}

static int StringCompare(FString* first, FString* second)
{
	return first->Compare(*second);
}

void JitCompiler::EmitCMPS()
{
	EmitComparisonOpcode([&](bool check, asmjit::Label& fail, asmjit::Label& success) {

		auto first = static_cast<bool>(A & CMP_BK) ? LoadConstPtr(&konsts[B]) : regS[B];
		auto second = static_cast<bool>(A & CMP_CK) ? LoadConstPtr(&konsts[C]) : regS[C];

		auto call = CreateCall<int, FString*, FString*>(static_cast<bool>(A & CMP_APPROX) ? StringCompareNoCase : StringCompare);

		auto result = newResultInt32();
		call->set_ret(0, result);
		call->set_arg(0, first);
		call->set_arg(1, second);

		int method = A & CMP_METHOD_MASK;
		if (method == CMP_EQ) {
			if (check) cc.cbz(result, fail);
			else       cc.cbnz(result, fail);
		}
		else if (method == CMP_LT) {
			// Not TBNZ/TBZ, they only reach +-32 KB
			cc.cmp(result, asmjit::Imm(0));
			if (check) cc.b_lt(fail);
			else       cc.b_ge(fail);
		}
		else {
			cc.cmp(result, asmjit::Imm(0));
			if (check) cc.b_le(fail);
			else       cc.b_gt(fail);
		}
	});
}

/////////////////////////////////////////////////////////////////////////////
// Integer math.

// Shift counts are taken modulo 32, like the x86 shift instructions do

void JitCompiler::EmitSLL_RR()
{
	cc.lsl(regD[A], regD[B], regD[C]);
}

void JitCompiler::EmitSLL_RI()
{
	cc.lsl(regD[A], regD[B], asmjit::Imm(C & 31));
}

void JitCompiler::EmitSLL_KR()
{
	cc.lsl(regD[A], LoadConstInt32(konstd[B]), regD[C]);
}

void JitCompiler::EmitSRL_RR()
{
	cc.lsr(regD[A], regD[B], regD[C]);
}

void JitCompiler::EmitSRL_RI()
{
	cc.lsr(regD[A], regD[B], asmjit::Imm(C & 31));
}

void JitCompiler::EmitSRL_KR()
{
	cc.lsr(regD[A], LoadConstInt32(konstd[B]), regD[C]);
}

void JitCompiler::EmitSRA_RR()
{
	cc.asr(regD[A], regD[B], regD[C]);
}

void JitCompiler::EmitSRA_RI()
{
	cc.asr(regD[A], regD[B], asmjit::Imm(C & 31));
}

void JitCompiler::EmitSRA_KR()
{
	cc.asr(regD[A], LoadConstInt32(konstd[B]), regD[C]);
}

void JitCompiler::EmitADD_RR()
{
	cc.add(regD[A], regD[B], regD[C]);
}

void JitCompiler::EmitADD_RK()
{
	EmitAddImm32(regD[A], regD[B], konstd[C]);
}

void JitCompiler::EmitADDI()
{
	EmitAddImm32(regD[A], regD[B], Cs);
}

void JitCompiler::EmitSUB_RR()
{
	cc.sub(regD[A], regD[B], regD[C]);
}

void JitCompiler::EmitSUB_RK()
{
	EmitSubImm32(regD[A], regD[B], konstd[C]);
}

void JitCompiler::EmitSUB_KR()
{
	cc.sub(regD[A], LoadConstInt32(konstd[B]), regD[C]);
}

void JitCompiler::EmitMUL_RR()
{
	cc.mul(regD[A], regD[B], regD[C]);
}

void JitCompiler::EmitMUL_RK()
{
	cc.mul(regD[A], regD[B], LoadConstInt32(konstd[C]));
}

void JitCompiler::EmitDIV_RR()
{
	auto label = EmitThrowExceptionLabel(X_DIVISION_BY_ZERO);
	cc.cbz(regD[C], label);
	cc.sdiv(regD[A], regD[B], regD[C]);
}

void JitCompiler::EmitDIV_RK()
{
	if (konstd[C] != 0)
	{
		cc.sdiv(regD[A], regD[B], LoadConstInt32(konstd[C]));
	}
	else
	{
		EmitThrowException(X_DIVISION_BY_ZERO);
	}
}

void JitCompiler::EmitDIV_KR()
{
	auto label = EmitThrowExceptionLabel(X_DIVISION_BY_ZERO);
	cc.cbz(regD[C], label);
	cc.sdiv(regD[A], LoadConstInt32(konstd[B]), regD[C]);
}

void JitCompiler::EmitDIVU_RR()
{
	auto label = EmitThrowExceptionLabel(X_DIVISION_BY_ZERO);
	cc.cbz(regD[C], label);
	cc.udiv(regD[A], regD[B], regD[C]);
}

void JitCompiler::EmitDIVU_RK()
{
	if (konstd[C] != 0)
	{
		cc.udiv(regD[A], regD[B], LoadConstInt32(konstd[C]));
	}
	else
	{
		EmitThrowException(X_DIVISION_BY_ZERO);
	}
}

void JitCompiler::EmitDIVU_KR()
{
	auto label = EmitThrowExceptionLabel(X_DIVISION_BY_ZERO);
	cc.cbz(regD[C], label);
	cc.udiv(regD[A], LoadConstInt32(konstd[B]), regD[C]);
}

// Remainder: a - (a / b) * b, which has the sign of the dividend like the x86 idiv remainder

void JitCompiler::EmitMOD_RR()
{
	auto label = EmitThrowExceptionLabel(X_DIVISION_BY_ZERO);
	cc.cbz(regD[C], label);
	auto quotient = newTempInt32();
	cc.sdiv(quotient, regD[B], regD[C]);
	cc.msub(regD[A], quotient, regD[C], regD[B]);
}

void JitCompiler::EmitMOD_RK()
{
	if (konstd[C] != 0)
	{
		auto divisor = LoadConstInt32(konstd[C]);
		auto quotient = newTempInt32();
		cc.sdiv(quotient, regD[B], divisor);
		cc.msub(regD[A], quotient, divisor, regD[B]);
	}
	else
	{
		EmitThrowException(X_DIVISION_BY_ZERO);
	}
}

void JitCompiler::EmitMOD_KR()
{
	auto label = EmitThrowExceptionLabel(X_DIVISION_BY_ZERO);
	cc.cbz(regD[C], label);
	auto dividend = LoadConstInt32(konstd[B]);
	auto quotient = newTempInt32();
	cc.sdiv(quotient, dividend, regD[C]);
	cc.msub(regD[A], quotient, regD[C], dividend);
}

void JitCompiler::EmitMODU_RR()
{
	auto label = EmitThrowExceptionLabel(X_DIVISION_BY_ZERO);
	cc.cbz(regD[C], label);
	auto quotient = newTempInt32();
	cc.udiv(quotient, regD[B], regD[C]);
	cc.msub(regD[A], quotient, regD[C], regD[B]);
}

void JitCompiler::EmitMODU_RK()
{
	if (konstd[C] != 0)
	{
		auto divisor = LoadConstInt32(konstd[C]);
		auto quotient = newTempInt32();
		cc.udiv(quotient, regD[B], divisor);
		cc.msub(regD[A], quotient, divisor, regD[B]);
	}
	else
	{
		EmitThrowException(X_DIVISION_BY_ZERO);
	}
}

void JitCompiler::EmitMODU_KR()
{
	auto label = EmitThrowExceptionLabel(X_DIVISION_BY_ZERO);
	cc.cbz(regD[C], label);
	auto dividend = LoadConstInt32(konstd[B]);
	auto quotient = newTempInt32();
	cc.udiv(quotient, dividend, regD[C]);
	cc.msub(regD[A], quotient, regD[C], dividend);
}

void JitCompiler::EmitAND_RR()
{
	cc.and_(regD[A], regD[B], regD[C]);
}

void JitCompiler::EmitAND_RK()
{
	cc.and_(regD[A], regD[B], LoadConstInt32(konstd[C]));
}

void JitCompiler::EmitOR_RR()
{
	cc.orr(regD[A], regD[B], regD[C]);
}

void JitCompiler::EmitOR_RK()
{
	cc.orr(regD[A], regD[B], LoadConstInt32(konstd[C]));
}

void JitCompiler::EmitXOR_RR()
{
	cc.eor(regD[A], regD[B], regD[C]);
}

void JitCompiler::EmitXOR_RK()
{
	cc.eor(regD[A], regD[B], LoadConstInt32(konstd[C]));
}

// min/max: C if it beats B, else B (like the x86 version's cmov)

void JitCompiler::EmitMIN_RR()
{
	cc.cmp(regD[C], regD[B]);
	cc.csel(regD[A], regD[C], regD[B], CondCode::kLT);
}

void JitCompiler::EmitMIN_RK()
{
	auto rc = LoadConstInt32(konstd[C]);
	cc.cmp(rc, regD[B]);
	cc.csel(regD[A], rc, regD[B], CondCode::kLT);
}

void JitCompiler::EmitMAX_RR()
{
	cc.cmp(regD[C], regD[B]);
	cc.csel(regD[A], regD[C], regD[B], CondCode::kGT);
}

void JitCompiler::EmitMAX_RK()
{
	auto rc = LoadConstInt32(konstd[C]);
	cc.cmp(rc, regD[B]);
	cc.csel(regD[A], rc, regD[B], CondCode::kGT);
}

void JitCompiler::EmitMINU_RR()
{
	cc.cmp(regD[C], regD[B]);
	cc.csel(regD[A], regD[C], regD[B], CondCode::kLO);
}

void JitCompiler::EmitMINU_RK()
{
	auto rc = LoadConstInt32(konstd[C]);
	cc.cmp(rc, regD[B]);
	cc.csel(regD[A], rc, regD[B], CondCode::kLO);
}

void JitCompiler::EmitMAXU_RR()
{
	cc.cmp(regD[C], regD[B]);
	cc.csel(regD[A], regD[C], regD[B], CondCode::kHI);
}

void JitCompiler::EmitMAXU_RK()
{
	auto rc = LoadConstInt32(konstd[C]);
	cc.cmp(rc, regD[B]);
	cc.csel(regD[A], rc, regD[B], CondCode::kHI);
}

void JitCompiler::EmitABS()
{
	cc.cmp(regD[B], asmjit::Imm(0));
	cc.cneg(regD[A], regD[B], CondCode::kLT);
}

void JitCompiler::EmitNEG()
{
	cc.neg(regD[A], regD[B]);
}

void JitCompiler::EmitNOT()
{
	cc.mvn(regD[A], regD[B]);
}

void JitCompiler::EmitEQ_R()
{
	EmitComparisonOpcode([&](bool check, asmjit::Label& fail, asmjit::Label& success) {
		cc.cmp(regD[B], regD[C]);
		if (check) cc.b_eq(fail);
		else       cc.b_ne(fail);
	});
}

void JitCompiler::EmitEQ_K()
{
	EmitComparisonOpcode([&](bool check, asmjit::Label& fail, asmjit::Label& success) {
		EmitCmpImm32(regD[B], konstd[C]);
		if (check) cc.b_eq(fail);
		else       cc.b_ne(fail);
	});
}

void JitCompiler::EmitLT_RR()
{
	EmitComparisonOpcode([&](bool check, asmjit::Label& fail, asmjit::Label& success) {
		cc.cmp(regD[B], regD[C]);
		if (check) cc.b_lt(fail);
		else       cc.b_ge(fail);
	});
}

void JitCompiler::EmitLT_RK()
{
	EmitComparisonOpcode([&](bool check, asmjit::Label& fail, asmjit::Label& success) {
		EmitCmpImm32(regD[B], konstd[C]);
		if (check) cc.b_lt(fail);
		else       cc.b_ge(fail);
	});
}

void JitCompiler::EmitLT_KR()
{
	EmitComparisonOpcode([&](bool check, asmjit::Label& fail, asmjit::Label& success) {
		cc.cmp(LoadConstInt32(konstd[B]), regD[C]);
		if (check) cc.b_lt(fail);
		else       cc.b_ge(fail);
	});
}

void JitCompiler::EmitLE_RR()
{
	EmitComparisonOpcode([&](bool check, asmjit::Label& fail, asmjit::Label& success) {
		cc.cmp(regD[B], regD[C]);
		if (check) cc.b_le(fail);
		else       cc.b_gt(fail);
	});
}

void JitCompiler::EmitLE_RK()
{
	EmitComparisonOpcode([&](bool check, asmjit::Label& fail, asmjit::Label& success) {
		EmitCmpImm32(regD[B], konstd[C]);
		if (check) cc.b_le(fail);
		else       cc.b_gt(fail);
	});
}

void JitCompiler::EmitLE_KR()
{
	EmitComparisonOpcode([&](bool check, asmjit::Label& fail, asmjit::Label& success) {
		cc.cmp(LoadConstInt32(konstd[B]), regD[C]);
		if (check) cc.b_le(fail);
		else       cc.b_gt(fail);
	});
}

void JitCompiler::EmitLTU_RR()
{
	EmitComparisonOpcode([&](bool check, asmjit::Label& fail, asmjit::Label& success) {
		cc.cmp(regD[B], regD[C]);
		if (check) cc.b_lo(fail);
		else       cc.b_hs(fail);
	});
}

void JitCompiler::EmitLTU_RK()
{
	EmitComparisonOpcode([&](bool check, asmjit::Label& fail, asmjit::Label& success) {
		EmitCmpImm32(regD[B], konstd[C]);
		if (check) cc.b_lo(fail);
		else       cc.b_hs(fail);
	});
}

void JitCompiler::EmitLTU_KR()
{
	EmitComparisonOpcode([&](bool check, asmjit::Label& fail, asmjit::Label& success) {
		cc.cmp(LoadConstInt32(konstd[B]), regD[C]);
		if (check) cc.b_lo(fail);
		else       cc.b_hs(fail);
	});
}

void JitCompiler::EmitLEU_RR()
{
	EmitComparisonOpcode([&](bool check, asmjit::Label& fail, asmjit::Label& success) {
		cc.cmp(regD[B], regD[C]);
		if (check) cc.b_ls(fail);
		else       cc.b_hi(fail);
	});
}

void JitCompiler::EmitLEU_RK()
{
	EmitComparisonOpcode([&](bool check, asmjit::Label& fail, asmjit::Label& success) {
		EmitCmpImm32(regD[B], konstd[C]);
		if (check) cc.b_ls(fail);
		else       cc.b_hi(fail);
	});
}

void JitCompiler::EmitLEU_KR()
{
	EmitComparisonOpcode([&](bool check, asmjit::Label& fail, asmjit::Label& success) {
		cc.cmp(LoadConstInt32(konstd[B]), regD[C]);
		if (check) cc.b_ls(fail);
		else       cc.b_hi(fail);
	});
}

/////////////////////////////////////////////////////////////////////////////
// Double-precision floating point math.

void JitCompiler::EmitADDF_RR()
{
	cc.fadd(regF[A], regF[B], regF[C]);
}

void JitCompiler::EmitADDF_RK()
{
	cc.fadd(regF[A], regF[B], LoadConstFloat64(konstf[C]));
}

void JitCompiler::EmitSUBF_RR()
{
	cc.fsub(regF[A], regF[B], regF[C]);
}

void JitCompiler::EmitSUBF_RK()
{
	cc.fsub(regF[A], regF[B], LoadConstFloat64(konstf[C]));
}

void JitCompiler::EmitSUBF_KR()
{
	cc.fsub(regF[A], LoadConstFloat64(konstf[B]), regF[C]);
}

void JitCompiler::EmitMULF_RR()
{
	cc.fmul(regF[A], regF[B], regF[C]);
}

void JitCompiler::EmitMULF_RK()
{
	cc.fmul(regF[A], regF[B], LoadConstFloat64(konstf[C]));
}

void JitCompiler::EmitDIVF_RR()
{
	auto label = EmitThrowExceptionLabel(X_DIVISION_BY_ZERO);
	cc.fcmp(regF[C], asmjit::Imm(0));
	cc.b_eq(label);
	cc.fdiv(regF[A], regF[B], regF[C]);
}

void JitCompiler::EmitDIVF_RK()
{
	if (konstf[C] == 0.)
	{
		EmitThrowException(X_DIVISION_BY_ZERO);
	}
	else
	{
		cc.fdiv(regF[A], regF[B], LoadConstFloat64(konstf[C]));
	}
}

void JitCompiler::EmitDIVF_KR()
{
	// No division by zero check here, same as the x86 version
	cc.fdiv(regF[A], LoadConstFloat64(konstf[B]), regF[C]);
}

static double DoubleModF(double a, double b)
{
	return a - floor(a / b) * b;
}

void JitCompiler::EmitMODF_RR()
{
	auto label = EmitThrowExceptionLabel(X_DIVISION_BY_ZERO);
	cc.fcmp(regF[C], asmjit::Imm(0));
	cc.b_eq(label);

	auto result = newResultFloat64();
	auto call = CreateCall<double, double, double>(DoubleModF);
	call->set_ret(0, result);
	call->set_arg(0, regF[B]);
	call->set_arg(1, regF[C]);
	cc.fmov(regF[A], result);
}

void JitCompiler::EmitMODF_RK()
{
	if (konstf[C] == 0.)
	{
		EmitThrowException(X_DIVISION_BY_ZERO);
	}
	else
	{
		auto tmp = LoadConstFloat64(konstf[C]);

		auto result = newResultFloat64();
		auto call = CreateCall<double, double, double>(DoubleModF);
		call->set_ret(0, result);
		call->set_arg(0, regF[B]);
		call->set_arg(1, tmp);
		cc.fmov(regF[A], result);
	}
}

void JitCompiler::EmitMODF_KR()
{
	auto label = EmitThrowExceptionLabel(X_DIVISION_BY_ZERO);
	cc.fcmp(regF[C], asmjit::Imm(0));
	cc.b_eq(label);

	auto tmp = LoadConstFloat64(konstf[B]);

	auto result = newResultFloat64();
	auto call = CreateCall<double, double, double>(DoubleModF);
	call->set_ret(0, result);
	call->set_arg(0, tmp);
	call->set_arg(1, regF[C]);
	cc.fmov(regF[A], result);
}

void JitCompiler::EmitPOWF_RR()
{
	auto result = newResultFloat64();
	auto call = CreateCall<double, double, double>(g_pow);
	call->set_ret(0, result);
	call->set_arg(0, regF[B]);
	call->set_arg(1, regF[C]);
	cc.fmov(regF[A], result);
}

void JitCompiler::EmitPOWF_RK()
{
	auto tmp = LoadConstFloat64(konstf[C]);

	auto result = newResultFloat64();
	auto call = CreateCall<double, double, double>(g_pow);
	call->set_ret(0, result);
	call->set_arg(0, regF[B]);
	call->set_arg(1, tmp);
	cc.fmov(regF[A], result);
}

void JitCompiler::EmitPOWF_KR()
{
	auto tmp = LoadConstFloat64(konstf[B]);

	auto result = newResultFloat64();
	auto call = CreateCall<double, double, double>(g_pow);
	call->set_ret(0, result);
	call->set_arg(0, tmp);
	call->set_arg(1, regF[C]);
	cc.fmov(regF[A], result);
}

// min/max like the interpreter: B < C ? B : C and B > C ? B : C

void JitCompiler::EmitMINF_RR()
{
	cc.fcmp(regF[B], regF[C]);
	cc.fcsel(regF[A], regF[B], regF[C], CondCode::kMI);
}

void JitCompiler::EmitMINF_RK()
{
	auto rc = LoadConstFloat64(konstf[C]);
	cc.fcmp(regF[B], rc);
	cc.fcsel(regF[A], regF[B], rc, CondCode::kMI);
}

void JitCompiler::EmitMAXF_RR()
{
	cc.fcmp(regF[B], regF[C]);
	cc.fcsel(regF[A], regF[B], regF[C], CondCode::kGT);
}

void JitCompiler::EmitMAXF_RK()
{
	auto rc = LoadConstFloat64(konstf[C]);
	cc.fcmp(regF[B], rc);
	cc.fcsel(regF[A], regF[B], rc, CondCode::kGT);
}

void JitCompiler::EmitATAN2()
{
	auto result = newResultFloat64();
	auto call = CreateCall<double, double, double>(g_atan2);
	call->set_ret(0, result);
	call->set_arg(0, regF[B]);
	call->set_arg(1, regF[C]);

	static const double constant = 180 / M_PI;
	cc.fmul(regF[A], result, LoadConstFloat64(constant));
}

void JitCompiler::EmitFLOP()
{
	switch (C)
	{
	// Exact operations: the instructions give the same results as the C library
	case FLOP_NEG:		cc.fneg(regF[A], regF[B]); return;
	case FLOP_ABS:		cc.fabs(regF[A], regF[B]); return;
	case FLOP_SQRT:		cc.fsqrt(regF[A], regF[B]); return;
	case FLOP_CEIL:		cc.frintp(regF[A], regF[B]); return;
	case FLOP_FLOOR:	cc.frintm(regF[A], regF[B]); return;
	case FLOP_ROUND:	cc.frinta(regF[A], regF[B]); return; // halfway cases away from zero, like round()
	default: break;
	}

	auto v = newTempFloat64();
	cc.fmov(v, regF[B]);

	if (C == FLOP_TAN_DEG)
	{
		static const double constant = M_PI / 180;
		cc.fmul(v, v, LoadConstFloat64(constant));
	}

	typedef double(*FuncPtr)(double);
	FuncPtr func = nullptr;
	switch (C)
	{
	default: I_Error("Unknown OP_FLOP subfunction"); break;
	case FLOP_EXP:		func = g_exp; break;
	case FLOP_LOG:		func = g_log; break;
	case FLOP_LOG10:	func = g_log10; break;
	case FLOP_ACOS:		func = g_acos; break;
	case FLOP_ASIN:		func = g_asin; break;
	case FLOP_ATAN:		func = g_atan; break;
	case FLOP_COS:		func = g_cos; break;
	case FLOP_SIN:		func = g_sin; break;
	case FLOP_TAN:		func = g_tan; break;
	case FLOP_ACOS_DEG:	func = g_acos; break;
	case FLOP_ASIN_DEG:	func = g_asin; break;
	case FLOP_ATAN_DEG:	func = g_atan; break;
	case FLOP_COS_DEG:	func = g_cosdeg; break;
	case FLOP_SIN_DEG:	func = g_sindeg; break;
	case FLOP_TAN_DEG:	func = g_tan; break;
	case FLOP_COSH:		func = g_cosh; break;
	case FLOP_SINH:		func = g_sinh; break;
	case FLOP_TANH:		func = g_tanh; break;
	}

	auto result = newResultFloat64();
	auto call = CreateCall<double, double>(func);
	call->set_ret(0, result);
	call->set_arg(0, v);

	if (C == FLOP_ACOS_DEG || C == FLOP_ASIN_DEG || C == FLOP_ATAN_DEG)
	{
		static const double constant = 180 / M_PI;
		cc.fmul(regF[A], result, LoadConstFloat64(constant));
	}
	else
	{
		cc.fmov(regF[A], result);
	}
}

void JitCompiler::EmitEQF_R()
{
	EmitComparisonOpcode([&](bool check, asmjit::Label& fail, asmjit::Label& success) {
		bool approx = static_cast<bool>(A & CMP_APPROX);
		if (!approx)
		{
			cc.fcmp(regF[B], regF[C]);
			if (check) cc.b_eq(fail);
			else       cc.b_ne(fail);
		}
		else
		{
			auto tmp = newTempFloat64();
			cc.fsub(tmp, regF[B], regF[C]);
			cc.fabs(tmp, tmp);
			cc.fcmp(LoadConstFloat64(EQUAL_EPSILON), tmp);
			if (check) cc.b_gt(fail);
			else       cc.b_le(fail);
		}
	});
}

void JitCompiler::EmitEQF_K()
{
	EmitComparisonOpcode([&](bool check, asmjit::Label& fail, asmjit::Label& success) {
		bool approx = static_cast<bool>(A & CMP_APPROX);
		auto konst = LoadConstFloat64(konstf[C]);
		if (!approx)
		{
			cc.fcmp(regF[B], konst);
			if (check) cc.b_eq(fail);
			else       cc.b_ne(fail);
		}
		else
		{
			auto tmp = newTempFloat64();
			cc.fsub(tmp, regF[B], konst);
			cc.fabs(tmp, tmp);
			cc.fcmp(LoadConstFloat64(EQUAL_EPSILON), tmp);
			if (check) cc.b_gt(fail);
			else       cc.b_le(fail);
		}
	});
}

void JitCompiler::EmitLTF_RR()
{
	EmitComparisonOpcode([&](bool check, asmjit::Label& fail, asmjit::Label& success) {
		if (static_cast<bool>(A & CMP_APPROX)) I_Error("CMP_APPROX not implemented for LTF_RR.\n");

		cc.fcmp(regF[B], regF[C]);
		if (check) cc.b_mi(fail);
		else       cc.b_pl(fail);
	});
}

void JitCompiler::EmitLTF_RK()
{
	EmitComparisonOpcode([&](bool check, asmjit::Label& fail, asmjit::Label& success) {
		if (static_cast<bool>(A & CMP_APPROX)) I_Error("CMP_APPROX not implemented for LTF_RK.\n");

		cc.fcmp(regF[B], LoadConstFloat64(konstf[C]));
		if (check) cc.b_mi(fail);
		else       cc.b_pl(fail);
	});
}

void JitCompiler::EmitLTF_KR()
{
	EmitComparisonOpcode([&](bool check, asmjit::Label& fail, asmjit::Label& success) {
		if (static_cast<bool>(A & CMP_APPROX)) I_Error("CMP_APPROX not implemented for LTF_KR.\n");

		cc.fcmp(LoadConstFloat64(konstf[B]), regF[C]);
		if (check) cc.b_mi(fail);
		else       cc.b_pl(fail);
	});
}

void JitCompiler::EmitLEF_RR()
{
	EmitComparisonOpcode([&](bool check, asmjit::Label& fail, asmjit::Label& success) {
		if (static_cast<bool>(A & CMP_APPROX)) I_Error("CMP_APPROX not implemented for LEF_RR.\n");

		cc.fcmp(regF[B], regF[C]);
		if (check) cc.b_ls(fail);
		else       cc.b_hi(fail);
	});
}

void JitCompiler::EmitLEF_RK()
{
	EmitComparisonOpcode([&](bool check, asmjit::Label& fail, asmjit::Label& success) {
		if (static_cast<bool>(A & CMP_APPROX)) I_Error("CMP_APPROX not implemented for LEF_RK.\n");

		cc.fcmp(regF[B], LoadConstFloat64(konstf[C]));
		if (check) cc.b_ls(fail);
		else       cc.b_hi(fail);
	});
}

void JitCompiler::EmitLEF_KR()
{
	EmitComparisonOpcode([&](bool check, asmjit::Label& fail, asmjit::Label& success) {
		if (static_cast<bool>(A & CMP_APPROX)) I_Error("CMP_APPROX not implemented for LEF_KR.\n");

		cc.fcmp(LoadConstFloat64(konstf[B]), regF[C]);
		if (check) cc.b_ls(fail);
		else       cc.b_hi(fail);
	});
}

/////////////////////////////////////////////////////////////////////////////
// Vector math. (2D)
//
// Vector registers of an instruction are either the same or don't overlap, so the
// three operand instructions below can work component by component.

void JitCompiler::EmitNEGV2()
{
	cc.fneg(regF[A], regF[B]);
	cc.fneg(regF[A + 1], regF[B + 1]);
}

void JitCompiler::EmitADDV2_RR()
{
	cc.fadd(regF[A], regF[B], regF[C]);
	cc.fadd(regF[A + 1], regF[B + 1], regF[C + 1]);
}

void JitCompiler::EmitSUBV2_RR()
{
	cc.fsub(regF[A], regF[B], regF[C]);
	cc.fsub(regF[A + 1], regF[B + 1], regF[C + 1]);
}

void JitCompiler::EmitDOTV2_RR()
{
	auto tmp0 = newTempFloat64();
	auto tmp1 = newTempFloat64();
	cc.fmul(tmp0, regF[B], regF[C]);
	cc.fmul(tmp1, regF[B + 1], regF[C + 1]);
	cc.fadd(regF[A], tmp0, tmp1);
}

void JitCompiler::EmitMULVF2_RR()
{
	auto rc = CheckRegF(C, A, A + 1);
	cc.fmul(regF[A], regF[B], rc);
	cc.fmul(regF[A + 1], regF[B + 1], rc);
}

void JitCompiler::EmitMULVF2_RK()
{
	auto rc = LoadConstFloat64(konstf[C]);
	cc.fmul(regF[A], regF[B], rc);
	cc.fmul(regF[A + 1], regF[B + 1], rc);
}

void JitCompiler::EmitDIVVF2_RR()
{
	auto rc = CheckRegF(C, A, A + 1);
	cc.fdiv(regF[A], regF[B], rc);
	cc.fdiv(regF[A + 1], regF[B + 1], rc);
}

void JitCompiler::EmitDIVVF2_RK()
{
	auto rc = LoadConstFloat64(konstf[C]);
	cc.fdiv(regF[A], regF[B], rc);
	cc.fdiv(regF[A + 1], regF[B + 1], rc);
}

void JitCompiler::EmitLENV2()
{
	auto tmp0 = newTempFloat64();
	auto tmp1 = newTempFloat64();
	cc.fmul(tmp0, regF[B], regF[B]);
	cc.fmul(tmp1, regF[B + 1], regF[B + 1]);
	cc.fadd(tmp0, tmp0, tmp1);
	CallSqrt(regF[A], tmp0);
}

void JitCompiler::EmitEQV2_R()
{
	EmitComparisonOpcode([&](bool check, asmjit::Label& fail, asmjit::Label& success) {
		EmitVectorComparison<2> (check, fail, success);
	});
}

void JitCompiler::EmitEQV2_K()
{
	I_Error("EQV2_K is not used.");
}

/////////////////////////////////////////////////////////////////////////////
// Vector math. (3D)

void JitCompiler::EmitNEGV3()
{
	cc.fneg(regF[A], regF[B]);
	cc.fneg(regF[A + 1], regF[B + 1]);
	cc.fneg(regF[A + 2], regF[B + 2]);
}

void JitCompiler::EmitADDV3_RR()
{
	cc.fadd(regF[A], regF[B], regF[C]);
	cc.fadd(regF[A + 1], regF[B + 1], regF[C + 1]);
	cc.fadd(regF[A + 2], regF[B + 2], regF[C + 2]);
}

void JitCompiler::EmitSUBV3_RR()
{
	cc.fsub(regF[A], regF[B], regF[C]);
	cc.fsub(regF[A + 1], regF[B + 1], regF[C + 1]);
	cc.fsub(regF[A + 2], regF[B + 2], regF[C + 2]);
}

void JitCompiler::EmitDOTV3_RR()
{
	auto tmp0 = newTempFloat64();
	auto tmp1 = newTempFloat64();
	cc.fmul(tmp0, regF[B], regF[C]);
	cc.fmul(tmp1, regF[B + 1], regF[C + 1]);
	cc.fadd(tmp0, tmp0, tmp1);
	cc.fmul(tmp1, regF[B + 2], regF[C + 2]);
	cc.fadd(regF[A], tmp0, tmp1);
}

void JitCompiler::EmitCROSSV_RR()
{
	auto r0 = newTempFloat64();
	auto r1 = newTempFloat64();
	auto r2 = newTempFloat64();
	auto tmp = newTempFloat64();

	auto a0 = regF[B];
	auto a1 = regF[B + 1];
	auto a2 = regF[B + 2];
	auto b0 = regF[C];
	auto b1 = regF[C + 1];
	auto b2 = regF[C + 2];

	// r0 = a1b2 - a2b1
	cc.fmul(r0, a1, b2);
	cc.fmul(tmp, a2, b1);
	cc.fsub(r0, r0, tmp);

	// r1 = a2b0 - a0b2
	cc.fmul(r1, a2, b0);
	cc.fmul(tmp, a0, b2);
	cc.fsub(r1, r1, tmp);

	// r2 = a0b1 - a1b0
	cc.fmul(r2, a0, b1);
	cc.fmul(tmp, a1, b0);
	cc.fsub(r2, r2, tmp);

	cc.fmov(regF[A], r0);
	cc.fmov(regF[A + 1], r1);
	cc.fmov(regF[A + 2], r2);
}

void JitCompiler::EmitMULVF3_RR()
{
	auto rc = CheckRegF(C, A, A + 1, A + 2);
	cc.fmul(regF[A], regF[B], rc);
	cc.fmul(regF[A + 1], regF[B + 1], rc);
	cc.fmul(regF[A + 2], regF[B + 2], rc);
}

void JitCompiler::EmitMULVF3_RK()
{
	auto rc = LoadConstFloat64(konstf[C]);
	cc.fmul(regF[A], regF[B], rc);
	cc.fmul(regF[A + 1], regF[B + 1], rc);
	cc.fmul(regF[A + 2], regF[B + 2], rc);
}

void JitCompiler::EmitDIVVF3_RR()
{
	auto rc = CheckRegF(C, A, A + 1, A + 2);
	cc.fdiv(regF[A], regF[B], rc);
	cc.fdiv(regF[A + 1], regF[B + 1], rc);
	cc.fdiv(regF[A + 2], regF[B + 2], rc);
}

void JitCompiler::EmitDIVVF3_RK()
{
	auto rc = LoadConstFloat64(konstf[C]);
	cc.fdiv(regF[A], regF[B], rc);
	cc.fdiv(regF[A + 1], regF[B + 1], rc);
	cc.fdiv(regF[A + 2], regF[B + 2], rc);
}

void JitCompiler::EmitLENV3()
{
	auto tmp0 = newTempFloat64();
	auto tmp1 = newTempFloat64();
	cc.fmul(tmp0, regF[B], regF[B]);
	cc.fmul(tmp1, regF[B + 1], regF[B + 1]);
	cc.fadd(tmp0, tmp0, tmp1);
	cc.fmul(tmp1, regF[B + 2], regF[B + 2]);
	cc.fadd(tmp0, tmp0, tmp1);
	CallSqrt(regF[A], tmp0);
}

void JitCompiler::EmitEQV3_R()
{
	EmitComparisonOpcode([&](bool check, asmjit::Label& fail, asmjit::Label& success) {
		EmitVectorComparison<3> (check, fail, success);
	});
}

void JitCompiler::EmitEQV3_K()
{
	I_Error("EQV3_K is not used.");
}

/////////////////////////////////////////////////////////////////////////////
// Vector math. (4D/Quaternion)

void JitCompiler::EmitNEGV4()
{
	cc.fneg(regF[A], regF[B]);
	cc.fneg(regF[A + 1], regF[B + 1]);
	cc.fneg(regF[A + 2], regF[B + 2]);
	cc.fneg(regF[A + 3], regF[B + 3]);
}

void JitCompiler::EmitADDV4_RR()
{
	cc.fadd(regF[A], regF[B], regF[C]);
	cc.fadd(regF[A + 1], regF[B + 1], regF[C + 1]);
	cc.fadd(regF[A + 2], regF[B + 2], regF[C + 2]);
	cc.fadd(regF[A + 3], regF[B + 3], regF[C + 3]);
}

void JitCompiler::EmitSUBV4_RR()
{
	cc.fsub(regF[A], regF[B], regF[C]);
	cc.fsub(regF[A + 1], regF[B + 1], regF[C + 1]);
	cc.fsub(regF[A + 2], regF[B + 2], regF[C + 2]);
	cc.fsub(regF[A + 3], regF[B + 3], regF[C + 3]);
}

void JitCompiler::EmitDOTV4_RR()
{
	auto tmp0 = newTempFloat64();
	auto tmp1 = newTempFloat64();
	cc.fmul(tmp0, regF[B], regF[C]);
	cc.fmul(tmp1, regF[B + 1], regF[C + 1]);
	cc.fadd(tmp0, tmp0, tmp1);
	cc.fmul(tmp1, regF[B + 2], regF[C + 2]);
	cc.fadd(tmp0, tmp0, tmp1);
	cc.fmul(tmp1, regF[B + 3], regF[C + 3]);
	cc.fadd(regF[A], tmp0, tmp1);
}

void JitCompiler::EmitMULVF4_RR()
{
	auto rc = CheckRegF(C, A, A + 1, A + 2, A + 3);
	cc.fmul(regF[A], regF[B], rc);
	cc.fmul(regF[A + 1], regF[B + 1], rc);
	cc.fmul(regF[A + 2], regF[B + 2], rc);
	cc.fmul(regF[A + 3], regF[B + 3], rc);
}

void JitCompiler::EmitMULVF4_RK()
{
	auto rc = LoadConstFloat64(konstf[C]);
	cc.fmul(regF[A], regF[B], rc);
	cc.fmul(regF[A + 1], regF[B + 1], rc);
	cc.fmul(regF[A + 2], regF[B + 2], rc);
	cc.fmul(regF[A + 3], regF[B + 3], rc);
}

void JitCompiler::EmitDIVVF4_RR()
{
	auto rc = CheckRegF(C, A, A + 1, A + 2, A + 3);
	cc.fdiv(regF[A], regF[B], rc);
	cc.fdiv(regF[A + 1], regF[B + 1], rc);
	cc.fdiv(regF[A + 2], regF[B + 2], rc);
	cc.fdiv(regF[A + 3], regF[B + 3], rc);
}

void JitCompiler::EmitDIVVF4_RK()
{
	auto rc = LoadConstFloat64(konstf[C]);
	cc.fdiv(regF[A], regF[B], rc);
	cc.fdiv(regF[A + 1], regF[B + 1], rc);
	cc.fdiv(regF[A + 2], regF[B + 2], rc);
	cc.fdiv(regF[A + 3], regF[B + 3], rc);
}

void JitCompiler::EmitLENV4()
{
	auto tmp0 = newTempFloat64();
	auto tmp1 = newTempFloat64();
	cc.fmul(tmp0, regF[B], regF[B]);
	cc.fmul(tmp1, regF[B + 1], regF[B + 1]);
	cc.fadd(tmp0, tmp0, tmp1);
	cc.fmul(tmp1, regF[B + 2], regF[B + 2]);
	cc.fadd(tmp0, tmp0, tmp1);
	cc.fmul(tmp1, regF[B + 3], regF[B + 3]);
	cc.fadd(tmp0, tmp0, tmp1);
	CallSqrt(regF[A], tmp0);
}

void JitCompiler::EmitEQV4_R()
{
	EmitComparisonOpcode([&](bool check, asmjit::Label& fail, asmjit::Label& success) {
		EmitVectorComparison<4> (check, fail, success);
	});
}

void JitCompiler::EmitEQV4_K()
{
	I_Error("EQV4_K is not used.");
}

// Quaternion ops
static void FuncMULQQ(void *result, double ax, double ay, double az, double aw, double bx, double by, double bz, double bw)
{
	*reinterpret_cast<DQuaternion*>(result) = DQuaternion(ax, ay, az, aw) * DQuaternion(bx, by, bz, bw);
}

static void FuncMULQV3(void *result, double ax, double ay, double az, double aw, double bx, double by, double bz)
{
	*reinterpret_cast<DVector3*>(result) = DQuaternion(ax, ay, az, aw) * DVector3(bx, by, bz);
}

void JitCompiler::EmitMULQQ_RR()
{
	auto stack = GetTemporaryVectorStackStorage();
	auto tmp = newTempIntPtr();
	cc.load_address_of(tmp, stack);

	auto call = CreateCall<void, void*, double, double, double, double, double, double, double, double>(FuncMULQQ);
	call->set_arg(0, tmp);
	call->set_arg(1, regF[B + 0]);
	call->set_arg(2, regF[B + 1]);
	call->set_arg(3, regF[B + 2]);
	call->set_arg(4, regF[B + 3]);
	call->set_arg(5, regF[C + 0]);
	call->set_arg(6, regF[C + 1]);
	call->set_arg(7, regF[C + 2]);
	call->set_arg(8, regF[C + 3]);

	cc.ldr(regF[A + 0], asmjit::a64::ptr(tmp, 0));
	cc.ldr(regF[A + 1], asmjit::a64::ptr(tmp, 8));
	cc.ldr(regF[A + 2], asmjit::a64::ptr(tmp, 16));
	cc.ldr(regF[A + 3], asmjit::a64::ptr(tmp, 24));
}

void JitCompiler::EmitMULQV3_RR()
{
	auto stack = GetTemporaryVectorStackStorage();
	auto tmp = newTempIntPtr();
	cc.load_address_of(tmp, stack);

	auto call = CreateCall<void, void*, double, double, double, double, double, double, double>(FuncMULQV3);
	call->set_arg(0, tmp);
	call->set_arg(1, regF[B + 0]);
	call->set_arg(2, regF[B + 1]);
	call->set_arg(3, regF[B + 2]);
	call->set_arg(4, regF[B + 3]);
	call->set_arg(5, regF[C + 0]);
	call->set_arg(6, regF[C + 1]);
	call->set_arg(7, regF[C + 2]);

	cc.ldr(regF[A + 0], asmjit::a64::ptr(tmp, 0));
	cc.ldr(regF[A + 1], asmjit::a64::ptr(tmp, 8));
	cc.ldr(regF[A + 2], asmjit::a64::ptr(tmp, 16));
}

void JitCompiler::EmitCONJQ()
{
	// DQuaternion(-x, -y, -z, w)
	cc.fneg(regF[A + 0], regF[B + 0]);
	cc.fneg(regF[A + 1], regF[B + 1]);
	cc.fneg(regF[A + 2], regF[B + 2]);
	cc.fmov(regF[A + 3], regF[B + 3]);
}


/////////////////////////////////////////////////////////////////////////////
// Pointer math.

void JitCompiler::EmitADDA_RR()
{
	// A null pointer stays null
	auto tmp = newTempIntPtr();
	cc.add(tmp, regA[B], regD[C], asmjit::a64::sxtw(0));
	cc.cmp(regA[B], asmjit::Imm(0));
	cc.csel(regA[A], tmp, regA[B], CondCode::kNE);
}

void JitCompiler::EmitADDA_RK()
{
	// A null pointer stays null
	auto tmp = newTempIntPtr();
	EmitAddOffset(tmp, regA[B], konstd[C]);
	cc.cmp(regA[B], asmjit::Imm(0));
	cc.csel(regA[A], tmp, regA[B], CondCode::kNE);
}

void JitCompiler::EmitSUBA()
{
	// dA = pB - pC, as the interpreter does it (the x86 version computes pB - dC into pA)
	auto tmp = newTempIntPtr();
	cc.sub(tmp, regA[B], regA[C]);
	cc.mov(regD[A], tmp.w());
}

void JitCompiler::EmitEQA_R()
{
	EmitComparisonOpcode([&](bool check, asmjit::Label& fail, asmjit::Label& success) {
		cc.cmp(regA[B], regA[C]);
		if (check) cc.b_eq(fail);
		else       cc.b_ne(fail);
	});
}

void JitCompiler::EmitEQA_K()
{
	EmitComparisonOpcode([&](bool check, asmjit::Label& fail, asmjit::Label& success) {
		cc.cmp(regA[B], LoadConstPtr(konsta[C].v));
		if (check) cc.b_eq(fail);
		else       cc.b_ne(fail);
	});
}

void JitCompiler::CallSqrt(const Vec &a, const Vec &b)
{
	// g_sqrt is sqrt, and FSQRT is correctly rounded like it
	cc.fsqrt(a, b);
}
