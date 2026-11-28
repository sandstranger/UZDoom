/*
** jit_runtime.cpp
**
** ZScript JIT for AArch64: code memory, unwind info, stack traces and the
** exceptions kept aside for JIT code (see jitintern.h)
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

#include <memory>
#include <exception>
#include <cstring>
#include <cstdlib>
#include <dlfcn.h>
#include <unwind.h>
#include <cxxabi.h>
#include "jit.h"
#include "jitintern.h"

struct JitFuncInfo
{
	FString name;
	FString filename;
	TArray<JitLineInfo> LineInfo;
	void *start;
	void *end;
};

static TArray<JitFuncInfo> JitDebugInfo;
static TArray<uint8_t*> JitBlocks;
static TArray<size_t> JitBlockSizes;
static TArray<uint8_t*> JitFrames;
static size_t JitBlockPos = 0;
static size_t JitBlockSize = 0;
static bool JitWriteXorExecute = false;

/////////////////////////////////////////////////////////////////////////////
// Exceptions caught on their way into JIT code

static thread_local std::exception_ptr JitPendingException;

// Only call this from a catch block
void JitStashException()
{
	JitPendingException = std::current_exception();
}

[[noreturn]] static void JitRethrowPendingException()
{
	std::exception_ptr e = std::move(JitPendingException);
	JitPendingException = nullptr;
	if (!e)
		I_Error("JIT code reported an error without an exception");
	std::rethrow_exception(e);
}

int JitCallNoThrow(VMFunction *func, VMValue *params, int numparams, VMReturn *ret, int numret)
{
	try
	{
		return func->ScriptCall(func, params, numparams, ret, numret);
	}
	catch (...)
	{
		JitStashException();
		return -1;
	}
}

int JitScriptCall(VMFunction *func, VMValue *params, int numparams, VMReturn *ret, int numret)
{
	int result = func->JitCall(func, params, numparams, ret, numret);
	if (result < 0)
		JitRethrowPendingException();
	return result;
}

/////////////////////////////////////////////////////////////////////////////
// Code memory

asmjit::Environment GetHostEnvironment()
{
	return asmjit::Environment::host();
}

static uint8_t *AllocJitMemory(size_t size)
{
	using namespace asmjit;

	if (JitBlocks.Size() > 0 && JitBlockPos + size <= JitBlockSize)
	{
		uint8_t *p = JitBlocks.Last() + JitBlockPos;
		JitBlockPos += size;
		return p;
	}

	const size_t bytesToAllocate = max(size_t(1024 * 1024), (size + 0xffff) & ~size_t(0xffff));
	void *p = nullptr;
	if (!JitWriteXorExecute)
	{
		if (VirtMem::alloc(&p, bytesToAllocate, VirtMem::MemoryFlags::kAccessRWX) != Error::kOk)
		{
			// No writable and executable memory (SELinux execmem): switch the blocks between RW and RX instead
			JitWriteXorExecute = true;
			p = nullptr;
		}
	}
	if (JitWriteXorExecute)
	{
		if (VirtMem::alloc(&p, bytesToAllocate, VirtMem::MemoryFlags::kAccessRW) != Error::kOk)
			return nullptr;
	}

	JitBlocks.Push((uint8_t*)p);
	JitBlockSizes.Push(bytesToAllocate);
	JitBlockSize = bytesToAllocate;
	JitBlockPos = size;
	return (uint8_t*)p;
}

// The block that AllocJitMemory returned memory from last
static void BeginWriteJitMemory()
{
	if (JitWriteXorExecute)
		asmjit::VirtMem::protect(JitBlocks.Last(), JitBlockSizes.Last(), asmjit::VirtMem::MemoryFlags::kAccessRW);
}

static void EndWriteJitMemory(void *p, size_t size)
{
	if (JitWriteXorExecute)
		asmjit::VirtMem::protect(JitBlocks.Last(), JitBlockSizes.Last(), asmjit::VirtMem::MemoryFlags::kAccessRX);
	asmjit::VirtMem::flush_instruction_cache(p, size);
}

/////////////////////////////////////////////////////////////////////////////
// Unwind info (.eh_frame) for the JIT code

extern "C"
{
	// The copy of LLVM libunwind linked into this library: it takes one FDE per call
	void __register_frame(const void*);
	void __deregister_frame(const void*);
}

static void WriteLength(TArray<uint8_t> &stream, unsigned int pos, unsigned int v)
{
	memcpy(&stream[pos], &v, sizeof(uint32_t));
}

static void WriteUInt64(TArray<uint8_t> &stream, uint64_t v)
{
	for (int i = 0; i < 8; i++)
		stream.Push((v >> (i * 8)) & 0xff);
}

static void WriteUInt32(TArray<uint8_t> &stream, uint32_t v)
{
	for (int i = 0; i < 4; i++)
		stream.Push((v >> (i * 8)) & 0xff);
}

static void WriteUInt16(TArray<uint8_t> &stream, uint16_t v)
{
	for (int i = 0; i < 2; i++)
		stream.Push((v >> (i * 8)) & 0xff);
}

static void WriteUInt8(TArray<uint8_t> &stream, uint8_t v)
{
	stream.Push(v);
}

static void WriteULEB128(TArray<uint8_t> &stream, uint32_t v)
{
	while (true)
	{
		if (v < 128)
		{
			WriteUInt8(stream, v);
			break;
		}
		else
		{
			WriteUInt8(stream, (v & 0x7f) | 0x80);
			v >>= 7;
		}
	}
}

static void WriteSLEB128(TArray<uint8_t> &stream, int32_t v)
{
	if (v >= 0)
	{
		WriteULEB128(stream, v);
	}
	else
	{
		while (true)
		{
			if (v > -128)
			{
				WriteUInt8(stream, v & 0x7f);
				break;
			}
			else
			{
				WriteUInt8(stream, v);
				v >>= 7;
			}
		}
	}
}

static void WritePadding(TArray<uint8_t> &stream)
{
	int padding = stream.Size() % 8;
	if (padding != 0)
	{
		padding = 8 - padding;
		for (int i = 0; i < padding; i++) WriteUInt8(stream, 0);
	}
}

static void WriteCIE(TArray<uint8_t> &stream, const TArray<uint8_t> &cieInstructions, uint8_t returnAddressReg)
{
	unsigned int lengthPos = stream.Size();
	WriteUInt32(stream, 0); // Length
	WriteUInt32(stream, 0); // CIE ID

	WriteUInt8(stream, 1); // CIE Version
	WriteUInt8(stream, 'z');
	WriteUInt8(stream, 'R'); // fde encoding
	WriteUInt8(stream, 0);
	WriteULEB128(stream, 1); // code alignment factor: advance_loc counts bytes
	WriteSLEB128(stream, -1); // data alignment factor: DW_CFA_offset counts bytes below the CFA
	WriteULEB128(stream, returnAddressReg);

	WriteULEB128(stream, 1); // LEB128 augmentation size
	WriteUInt8(stream, 0); // DW_EH_PE_absptr (FDE uses absolute pointers)

	for (unsigned int i = 0; i < cieInstructions.Size(); i++)
		stream.Push(cieInstructions[i]);

	WritePadding(stream);
	WriteLength(stream, lengthPos, stream.Size() - lengthPos - 4);
}

static void WriteFDE(TArray<uint8_t> &stream, const TArray<uint8_t> &fdeInstructions, uint32_t cieLocation, unsigned int &functionStart)
{
	unsigned int lengthPos = stream.Size();
	WriteUInt32(stream, 0); // Length
	uint32_t offsetToCIE = stream.Size() - cieLocation;
	WriteUInt32(stream, offsetToCIE);

	functionStart = stream.Size();
	WriteUInt64(stream, 0); // func start
	WriteUInt64(stream, 0); // func size

	WriteULEB128(stream, 0); // LEB128 augmentation size

	for (unsigned int i = 0; i < fdeInstructions.Size(); i++)
		stream.Push(fdeInstructions[i]);

	WritePadding(stream);
	WriteLength(stream, lengthPos, stream.Size() - lengthPos - 4);
}

static void WriteAdvanceLoc(TArray<uint8_t> &fdeInstructions, uint64_t offset, uint64_t &lastOffset)
{
	uint64_t delta = offset - lastOffset;
	if (delta < (1 << 6))
	{
		WriteUInt8(fdeInstructions, (1 << 6) | delta); // DW_CFA_advance_loc
	}
	else if (delta < (1 << 8))
	{
		WriteUInt8(fdeInstructions, 2); // DW_CFA_advance_loc1
		WriteUInt8(fdeInstructions, delta);
	}
	else if (delta < (1 << 16))
	{
		WriteUInt8(fdeInstructions, 3); // DW_CFA_advance_loc2
		WriteUInt16(fdeInstructions, delta);
	}
	else
	{
		WriteUInt8(fdeInstructions, 4); // DW_CFA_advance_loc4
		WriteUInt32(fdeInstructions, delta);
	}
	lastOffset = offset;
}

static void WriteDefineCFA(TArray<uint8_t> &cieInstructions, int dwarfRegId, int stackOffset)
{
	WriteUInt8(cieInstructions, 0x0c); // DW_CFA_def_cfa
	WriteULEB128(cieInstructions, dwarfRegId);
	WriteULEB128(cieInstructions, stackOffset);
}

static void WriteDefineStackOffset(TArray<uint8_t> &fdeInstructions, int stackOffset)
{
	WriteUInt8(fdeInstructions, 0x0e); // DW_CFA_def_cfa_offset
	WriteULEB128(fdeInstructions, stackOffset);
}

static void WriteRegisterStackLocation(TArray<uint8_t> &instructions, int dwarfRegId, int stackLocation)
{
	if (dwarfRegId < 64)
	{
		WriteUInt8(instructions, (2 << 6) | dwarfRegId); // DW_CFA_offset
	}
	else
	{
		WriteUInt8(instructions, 0x05); // DW_CFA_offset_extended
		WriteULEB128(instructions, dwarfRegId);
	}
	WriteULEB128(instructions, stackLocation);
}

// Builds the CFI by reading the prolog asmjit emitted (a64::EmitHelper::emit_prolog): STP/STR of the saved
// registers (the first one pre-indexed, allocating the save area), MOV x29, SP, then SUB SP, SP, #n. The CFA stays
// SP based, which is right for the whole body: the function never moves SP after the prolog.
// DWARF register numbers on AArch64: x0-x30 are 0-30, SP is 31, v0-v31 are 64-95.
static TArray<uint8_t> CreateUnwindInfoA64(const uint8_t *code, size_t codeSize, unsigned int &functionStart)
{
	const int dwarfRegSP = 31;
	const int dwarfRegV0 = 64;
	const uint8_t returnAddressReg = 30; // LR

	TArray<uint8_t> cieInstructions;
	TArray<uint8_t> fdeInstructions;

	WriteDefineCFA(cieInstructions, dwarfRegSP, 0); // On entry the CFA is SP and the return address is in LR

	int cfaOffset = 0;
	uint64_t lastOffset = 0;
	size_t pos = 0;
	while (pos + 4 <= codeSize && pos < 64 * 4)
	{
		uint32_t inst;
		memcpy(&inst, code + pos, sizeof(uint32_t));
		pos += 4;

		int rt = inst & 31;
		int rn = (inst >> 5) & 31;
		int rt2 = (inst >> 10) & 31;
		int imm7 = (int)((inst >> 15) & 0x7f);
		if (imm7 & 0x40) imm7 -= 0x80;
		int imm9 = (int)((inst >> 12) & 0x1ff);
		if (imm9 & 0x100) imm9 -= 0x200;
		int imm12 = (int)((inst >> 10) & 0xfff);

		bool pair, preindex, vec;
		int offset;
		if ((inst & 0xffc00000) == 0xa9800000) { pair = true; preindex = true; vec = false; offset = imm7 * 8; } // STP Xt1, Xt2, [SP, #imm]!
		else if ((inst & 0xffc00000) == 0xa9000000) { pair = true; preindex = false; vec = false; offset = imm7 * 8; } // STP Xt1, Xt2, [SP, #imm]
		else if ((inst & 0xffc00000) == 0x6d800000) { pair = true; preindex = true; vec = true; offset = imm7 * 8; } // STP Dt1, Dt2, [SP, #imm]!
		else if ((inst & 0xffc00000) == 0x6d000000) { pair = true; preindex = false; vec = true; offset = imm7 * 8; } // STP Dt1, Dt2, [SP, #imm]
		else if ((inst & 0xffe00c00) == 0xf8000c00) { pair = false; preindex = true; vec = false; offset = imm9; } // STR Xt, [SP, #imm]!
		else if ((inst & 0xffc00000) == 0xf9000000) { pair = false; preindex = false; vec = false; offset = imm12 * 8; } // STR Xt, [SP, #imm]
		else if ((inst & 0xffe00c00) == 0xfc000c00) { pair = false; preindex = true; vec = true; offset = imm9; } // STR Dt, [SP, #imm]!
		else if ((inst & 0xffc00000) == 0xfd000000) { pair = false; preindex = false; vec = true; offset = imm12 * 8; } // STR Dt, [SP, #imm]
		else if (inst == 0x910003fd) // MOV x29, SP
		{
			continue;
		}
		else if ((inst & 0xff8003ff) == 0xd10003ff) // SUB SP, SP, #imm{, LSL #12}
		{
			int shift = (inst >> 22) & 1;
			cfaOffset += imm12 << (shift * 12);
			WriteAdvanceLoc(fdeInstructions, pos, lastOffset);
			WriteDefineStackOffset(fdeInstructions, cfaOffset);
			continue;
		}
		else
		{
			break; // End of the prolog
		}

		if (rn != dwarfRegSP)
			break;

		int slot;
		if (preindex)
		{
			cfaOffset -= offset; // offset is negative
			slot = 0;
		}
		else
		{
			slot = offset;
		}

		int regBase = vec ? dwarfRegV0 : 0;
		WriteAdvanceLoc(fdeInstructions, pos, lastOffset);
		if (preindex)
			WriteDefineStackOffset(fdeInstructions, cfaOffset);
		WriteRegisterStackLocation(fdeInstructions, regBase + rt, cfaOffset - slot);
		if (pair)
			WriteRegisterStackLocation(fdeInstructions, regBase + rt2, cfaOffset - slot - 8);
	}

	TArray<uint8_t> stream;
	WriteCIE(stream, cieInstructions, returnAddressReg);
	WriteFDE(stream, fdeInstructions, 0, functionStart);
	WriteUInt32(stream, 0);
	return stream;
}

void *AddJitFunction(asmjit::CodeHolder* code, JitCompiler *compiler)
{
	using namespace asmjit;

	FuncNode *func = compiler->Codegen();

	code->flatten();
	code->resolve_cross_section_fixups();
	if (code->has_unresolved_fixups())
		return nullptr;

	size_t codeSize = code->code_size();
	if (codeSize == 0)
		return nullptr;

	// The function starts at the beginning of the .text section (only comments come before it)
	if (code->label_offset(func->label()) != 0)
		I_Error("JIT: function does not start at offset 0");

	Section *text = code->text_section();
	unsigned int fdeFunctionStart = 0;
	TArray<uint8_t> unwindInfo = CreateUnwindInfoA64(text->data(), text->buffer_size(), fdeFunctionStart);
	size_t unwindInfoSize = unwindInfo.Size();

	size_t unwindStart = (codeSize + 15) / 16 * 16;

	uint8_t *p = AllocJitMemory(unwindStart + unwindInfoSize);
	if (!p)
		return nullptr;

	if (code->relocate_to_base((uint64_t)(uintptr_t)p) != Error::kOk)
		return nullptr;

	size_t relocSize = code->code_size();

	BeginWriteJitMemory();

	memset(p, 0, unwindStart);
	if (code->copy_flattened_data(p, relocSize) != Error::kOk)
	{
		EndWriteJitMemory(p, unwindStart + unwindInfoSize);
		return nullptr;
	}

	uint8_t *startaddr = p;
	uint8_t *endaddr = p + relocSize;
	uint8_t *unwindptr = p + unwindStart;
	memcpy(unwindptr, &unwindInfo[0], unwindInfoSize);

	uint64_t funcaddr[2] = { (uint64_t)(uintptr_t)startaddr, (uint64_t)(endaddr - startaddr) };
	memcpy(unwindptr + fdeFunctionStart, funcaddr, sizeof(funcaddr));

	EndWriteJitMemory(p, unwindStart + unwindInfoSize);

#if defined(__APPLE__) || defined(__ANDROID__)
	// LLVM libunwind's __register_frame takes a single FDE as an argument
	uint8_t *entry = unwindptr;
	while (true)
	{
		uint32_t length;
		memcpy(&length, entry, sizeof(uint32_t));
		if (length == 0)
			break;

		uint32_t offset;
		memcpy(&offset, entry + 4, sizeof(uint32_t));
		if (offset != 0)
		{
			__register_frame(entry);
			JitFrames.Push(entry);
		}
		entry += length + 4;
	}
#else
	// libgcc's __register_frame takes a pointer to the entire .eh_frame
	__register_frame(unwindptr);
	JitFrames.Push(unwindptr);
#endif

	JitDebugInfo.Push({ compiler->GetScriptFunction()->PrintableName, compiler->GetScriptFunction()->SourceFileName, compiler->LineInfo, startaddr, endaddr });

	return p;
}

void JitRelease()
{
	for (auto p : JitFrames)
	{
		__deregister_frame(p);
	}
	for (unsigned int i = 0; i < JitBlocks.Size(); i++)
	{
		asmjit::VirtMem::release(JitBlocks[i], JitBlockSizes[i]);
	}
	JitDebugInfo.Clear();
	JitFrames.Clear();
	JitBlocks.Clear();
	JitBlockSizes.Clear();
	JitBlockPos = 0;
	JitBlockSize = 0;
}

/////////////////////////////////////////////////////////////////////////////
// Stack traces

struct UnwindBacktraceState
{
	void **current;
	void **end;
};

static _Unwind_Reason_Code UnwindBacktraceCallback(struct _Unwind_Context *context, void *arg)
{
	UnwindBacktraceState *state = static_cast<UnwindBacktraceState *>(arg);
	uintptr_t pc = _Unwind_GetIP(context);
	if (pc)
	{
		if (state->current == state->end)
			return _URC_END_OF_STACK;
		*state->current++ = reinterpret_cast<void *>(pc);
	}
	return _URC_NO_REASON;
}

// Walks JIT frames too: their FDEs are registered with this library's unwinder, which _Unwind_Backtrace uses
static int CaptureStackTrace(int max_frames, void **out_frames)
{
	memset(out_frames, 0, sizeof(void *) * max_frames);
	UnwindBacktraceState state = { out_frames, out_frames + max_frames };
	_Unwind_Backtrace(UnwindBacktraceCallback, &state);
	return (int)(state.current - out_frames);
}

class NativeSymbolResolver
{
public:
	FString GetName(void *frame)
	{
		FString s;
		Dl_info info;
		if (dladdr(frame, &info) == 0)
		{
			s.Format("Called from %p\n", frame);
			return s;
		}

		const char *filename = info.dli_fname ? info.dli_fname : "";
		const char *function = info.dli_sname ? info.dli_sname : "";

		int status = 0;
		char *new_function = info.dli_sname ? abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status) : nullptr;
		if (new_function)	// Was correctly decoded
		{
			function = new_function;
		}

		s.Format("Called from %s at %s\n", function, filename);

		if (new_function)
		{
			free(new_function);
		}
		return s;
	}
};

int JITPCToLine(uint8_t *pc, const JitFuncInfo *info)
{
	int PCIndex = int(pc - ((uint8_t *) (info->start)));
	if (info->LineInfo.Size () == 1) return info->LineInfo[0].LineNumber;
	for (unsigned i = 1; i < info->LineInfo.Size (); i++)
	{
		if (info->LineInfo[i].InstructionIndex >= PCIndex)
		{
			return info->LineInfo[i - 1].LineNumber;
		}
	}
	// After the last label: the last line (the x86 version returns -1 here)
	return info->LineInfo.Size() > 0 ? info->LineInfo.Last().LineNumber : -1;
}

FString JitGetStackFrameName(NativeSymbolResolver *nativeSymbols, void *pc)
{
	for (unsigned int i = 0; i < JitDebugInfo.Size(); i++)
	{
		const auto &info = JitDebugInfo[i];
		if (pc >= info.start && pc < info.end)
		{
			int line = JITPCToLine ((uint8_t *)pc, &info);

			FString s;

			if (line == -1)
				s.Format("Called from %s at %s\n", info.name.GetChars(), info.filename.GetChars());
			else
				s.Format("Called from %s at %s, line %d\n", info.name.GetChars(), info.filename.GetChars(), line);

			return s;
		}
	}

	return nativeSymbols ? nativeSymbols->GetName(pc) : FString();
}

FString JitCaptureStackTrace(int framesToSkip, bool includeNativeFrames, int maxFrames)
{
	void *frames[32];
	int numframes = CaptureStackTrace(32, frames);

	std::unique_ptr<NativeSymbolResolver> nativeSymbols;
	if (includeNativeFrames)
		nativeSymbols.reset(new NativeSymbolResolver());

	int total = 0;
	FString s;
	for (int i = framesToSkip + 1; i < numframes; i++)
	{
		FString name = JitGetStackFrameName(nativeSymbols.get(), frames[i]);
		if (!name.IsEmpty())
		{
			s += name;
			total++;
			if (maxFrames != -1 && maxFrames == total)
				break;
		}
	}
	return s;
}
