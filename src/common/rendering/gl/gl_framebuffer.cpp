/*
** gl_framebuffer.cpp
**
** Implementation of non-hardware specific parts of the OpenGL frame buffer
**
**---------------------------------------------------------------------------
**
** Copyright 2010-2020 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
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

#include "gl_system.h"
#include "v_video.h"
#include "m_png.h"

#include "i_time.h"

#include "gl_interface.h"
#include "gl_framebuffer.h"
#include "gl_renderer.h"
#include "gl_renderbuffers.h"
#include "gl_samplers.h"
#include "hw_clock.h"
#include "hw_vrmodes.h"
#include "hw_skydome.h"
#include "hw_viewpointbuffer.h"
#include "hw_lightbuffer.h"
#include "hw_bonebuffer.h"
#include "gl_shaderprogram.h"
#include "gl_debug.h"
#include "r_videoscale.h"
#include "gl_buffers.h"
#include "gl_postprocessstate.h"
#include "v_draw.h"
#include "printf.h"
#include "gl_hwtexture.h"

#include "flatvertices.h"
#include "hw_cvars.h"
#include "hw_batching.h"
#include "gl_shader.h"
#ifdef __ANDROID__
#include <malloc.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#endif
#include "hw_programcache.h"
#include "gl_renderbuffers.h"

EXTERN_CVAR (Bool, vid_vsync)
EXTERN_CVAR(Int, gl_tonemap)
EXTERN_CVAR(Bool, cl_capfps)
EXTERN_CVAR(Int, gl_pipeline_depth);

void gl_LoadExtensions();
void gl_PrintStartupLog();

extern bool vid_hdr_active;

namespace OpenGLRenderer
{
	FGLRenderer *GLRenderer;

//==========================================================================
//
//
//
//==========================================================================

OpenGLFrameBuffer::OpenGLFrameBuffer(void *hMonitor, bool fullscreen) :
	Super(hMonitor, fullscreen)
{
	// SetVSync needs to be at the very top to workaround a bug in Nvidia's OpenGL driver.
	// If wglSwapIntervalEXT is called after glBindFramebuffer in a frame the setting is not changed!
	Super::SetVSync(vid_vsync);
	FHardwareTexture::InitGlobalState();

	// Make sure all global variables tracking OpenGL context state are reset..
	gl_RenderState.Reset();

	GLRenderer = nullptr;
}

OpenGLFrameBuffer::~OpenGLFrameBuffer()
{
	PPResource::ResetAll();
	gl_RenderState.ReleaseBatchResources();
	GLRenderTargetPool::Trim(true);

	if (mVertexData != nullptr) delete mVertexData;
	if (mSkyData != nullptr) delete mSkyData;
	if (mViewpoints != nullptr) delete mViewpoints;
	if (mLights != nullptr) delete mLights;
	if (mBones != nullptr) delete mBones;
	mShadowMap.Reset();

	if (GLRenderer)
	{
		delete GLRenderer;
		GLRenderer = nullptr;
	}
}

//==========================================================================
//
// Initializes the GL renderer
//
//==========================================================================

void OpenGLFrameBuffer::InitializeState()
{
	static bool first=true;

	if (first)
	{
		if (ogl_LoadFunctions() == ogl_LOAD_FAILED)
		{
			I_FatalError("Failed to load OpenGL functions.");
		}
	}

	gl_LoadExtensions();

#ifdef __ANDROID__
	// Swap() doesn't glFinish() here, so the buffers must always be pipelined and guarded by fences.
	// Otherwise a depth of 0 makes every frame rewrite the vertex and light data the GPU is still reading.
	mPipelineNbr = gl_pipeline_depth < 1 ? HW_MAX_PIPELINE_BUFFERS : clamp(*gl_pipeline_depth, 1, HW_MAX_PIPELINE_BUFFERS);
	mPipelineType = 1;
#else
	mPipelineNbr = clamp(*gl_pipeline_depth, 1, HW_MAX_PIPELINE_BUFFERS);
	mPipelineType = gl_pipeline_depth > 0;
#endif

	// Move some state to the framebuffer object for easier access.
	hwcaps = gl.flags;
	glslversion = gl.glslversion;
	uniformblockalignment = gl.uniformblockalignment;
	maxuniformblock = gl.maxuniformblock;
	vendorstring = gl.vendorstring;

	if (first)
	{
		first=false;
		gl_PrintStartupLog();
	}

	// The MSAA menu only offers the sample counts this device renders the scene with.
	V_SetMultisampleLimit(FGLRenderBuffers::MultisampleLimit(), FGLRenderBuffers::MultisamplePath());

	glDepthFunc(GL_LESS);

	glEnable(GL_DITHER);
	glDisable(GL_CULL_FACE);
	glDisable(GL_POLYGON_OFFSET_FILL);
	if (gl.esversion == 0) glEnable(GL_POLYGON_OFFSET_LINE);	// no polygon modes in OpenGL ES (GL_INVALID_ENUM)
	glEnable(GL_BLEND);
	if (gl.esversion == 0) glEnable(GL_DEPTH_CLAMP);
	glDisable(GL_DEPTH_TEST);
	if (gl.esversion == 0) glDisable(GL_LINE_SMOOTH);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClearDepth(1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	SetViewportRects(nullptr);

	mVertexData = new FFlatVertexBuffer(GetWidth(), GetHeight(), screen->mPipelineNbr);
	mSkyData = new FSkyVertexBuffer;
	mViewpoints = new HWViewpointBuffer(screen->mPipelineNbr);
	mLights = new FLightBuffer(screen->mPipelineNbr);
	mBones = new BoneBuffer(screen->mPipelineNbr);
	GLRenderer = new FGLRenderer(this);
	GLRenderer->Initialize(GetWidth(), GetHeight());

	static_cast<GLDataBuffer*>(mLights->GetBuffer())->BindBase();
	static_cast<GLDataBuffer*>(mBones->GetBuffer())->BindBase();

#ifdef __ANDROID__ 	// This is needed to stop Ardeno 530 from crashing on the first drawer
	static_cast<GLDataBuffer*>(mLights->GetBuffer())->Map();
	static_cast<GLDataBuffer*>(mLights->GetBuffer())->Unmap();

	static_cast<GLDataBuffer*>(mBones->GetBuffer())->Map();
	static_cast<GLDataBuffer*>(mBones->GetBuffer())->Unmap();
#endif

	mDebug = std::make_unique<FGLDebug>();
	mDebug->Update();
}

//==========================================================================
//
// Updates the screen
//
//==========================================================================

void OpenGLFrameBuffer::Update()
{
	twoD.Reset();
	Flush3D.Reset();

	Flush3D.Clock();
	GLRenderer->Flush();
	Flush3D.Unclock();

	Swap();
	Super::Update();
}
#ifdef ANDROID
uint8_t * gles_convertRGB(uint8_t* src, uint8_t * dst, int width, int height)
{
	for (int i=0; i<width*height; i++) {
		for (int j=0; j<3; j++)
			*(dst++) = *(src++);
		src++;
	}

	return dst;
}
#endif
void OpenGLFrameBuffer::CopyScreenToBuffer(int width, int height, uint8_t* scr)
{
	IntRect bounds;
	bounds.left = 0;
	bounds.top = 0;
	bounds.width = width;
	bounds.height = height;
	GLRenderer->CopyToBackbuffer(&bounds, true);

	// strictly speaking not needed as the glReadPixels should block until the scene is rendered, but this is to safeguard against shitty drivers
#ifndef ANDROID
	glFinish();
#endif	
#ifdef ANDROID
	uint8_t* tmp = (uint8_t *)M_Malloc(width * height * 4);
	glReadPixels(0, 0, width, height, GL_RGBA,GL_UNSIGNED_BYTE, tmp);
	gles_convertRGB( tmp, scr, width, height);
	M_Free(tmp);
#else
	glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, scr);
#endif
}

//===========================================================================
//
// Camera texture rendering
//
//===========================================================================

void OpenGLFrameBuffer::RenderTextureView(FCanvasTexture* tex, std::function<void(IntRect &)> renderFunc)
{
	GLRenderer->StartOffscreen();
	GLRenderer->BindToFrameBuffer(tex);

	IntRect bounds;
	bounds.left = bounds.top = 0;
	bounds.width = FHardwareTexture::GetTexDimension(tex->GetWidth());
	bounds.height = FHardwareTexture::GetTexDimension(tex->GetHeight());

	renderFunc(bounds);
	GLRenderer->EndOffscreen();

	tex->SetUpdated(true);
	static_cast<OpenGLFrameBuffer*>(screen)->camtexcount++;
}

//===========================================================================
//
//
//
//===========================================================================

const char* OpenGLFrameBuffer::DeviceName() const
{
	return gl.modelstring;
}

//==========================================================================
//
// Swap the buffers
//
//==========================================================================

CVAR(Bool, gl_finishbeforeswap, false, CVAR_ARCHIVE|CVAR_GLOBALCONFIG);

#ifdef __ANDROID__
//==========================================================================
//
// Memory report (Android): every gl_mem_report seconds one line in the log with the
// process memory, what this renderer holds and, under ANGLE, ANGLE's own caches
// (GL_AMD_performance_monitor exposes its Vulkan counters). Two lines a minute apart
// show which of them grows.
//
//==========================================================================

CVAR(Int, gl_mem_report, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)	// seconds, 0: off

namespace
{
	typedef void (APIENTRY *PFN_BeginPerfMonitorAMD)(GLuint monitor);
	typedef void (APIENTRY *PFN_GetPerfMonitorCountersAMD)(GLuint group, GLint *numCounters, GLint *maxActiveCounters, GLsizei counterSize, GLuint *counters);
	typedef void (APIENTRY *PFN_GetPerfMonitorCounterStringAMD)(GLuint group, GLuint counter, GLsizei bufSize, GLsizei *length, GLchar *counterString);
	typedef void (APIENTRY *PFN_GetPerfMonitorCounterDataAMD)(GLuint monitor, GLenum pname, GLsizei dataSize, GLuint *data, GLint *bytesWritten);

	const GLenum PERFMON_RESULT_SIZE_AMD = 0x8BC5;
	const GLenum PERFMON_RESULT_AMD = 0x8BC6;

	// ANGLE's Vulkan counters (ANGLE_VK_PERF_COUNTERS_X): the ones that are sizes are shown as they
	// are, of all others only those that changed since the last line, with the change
	const char *const AngleGauges[] = { "descriptorSetCacheTotalSize", "framebufferCacheSize", "pendingSubmissionGarbageObjects" };

	struct Mappings
	{
		unsigned maliCount = 0;
		uint64_t maliBytes = 0;
		uint64_t total = 0;
	};

	struct MemReportState
	{
		bool initialized = false;
		PFN_GetPerfMonitorCounterDataAMD getData = nullptr;
		uint64_t lastTime = 0;
		uint64_t lastRss = 0;
		Mappings lastMappings;
		size_t lastHeap = 0, lastTextures = 0, lastBuffers = 0, lastUploads = 0;
		unsigned lastRtCreated = 0, lastRtDeleted = 0;
		TArray<FString> names;
		TArray<uint64_t> last;
		TArray<uint8_t> results;
	};
	MemReportState MemReport;

	long long MB(uint64_t bytes)
	{
		return (long long)(bytes / (1024 * 1024));
	}

	// 64 bits: on Mali the driver counts the GPU memory of the process in its RSS, beyond 4 GB on ARMv7
	uint64_t ProcessRss()
	{
		unsigned long pages = 0, resident = 0;
		if (FILE *f = fopen("/proc/self/statm", "r"))
		{
			if (fscanf(f, "%lu %lu", &pages, &resident) != 2) resident = 0;
			fclose(f);
		}
		return uint64_t(resident) * uint64_t(sysconf(_SC_PAGESIZE));
	}

	// The address space of the process and what of it the Mali driver mapped (/dev/mali0): GPU memory
	// the CPU can reach (host visible buffers, staging, mapped images). GPU memory that grows while
	// these stay flat is GPU-only (images, pipelines, command memory).
	Mappings ProcessMappings()
	{
		Mappings m;
		FILE *f = fopen("/proc/self/maps", "r");
		if (!f) return m;
		char line[512];
		while (fgets(line, sizeof(line), f))
		{
			unsigned long long lo = 0, hi = 0;
			if (sscanf(line, "%llx-%llx", &lo, &hi) != 2 || hi < lo) continue;
			m.total += hi - lo;
			if (strstr(line, "mali") != nullptr)
			{
				m.maliCount++;
				m.maliBytes += hi - lo;
			}
		}
		fclose(f);
		return m;
	}

	// MediaTek's Mali driver lists the GPU memory of each process; not readable everywhere
	FString MaliProcessMemory()
	{
		static bool denied = false;
		FString result;
		if (denied) return result;
		FILE *f = fopen("/proc/mtk_mali/gpu_memory", "r");
		if (!f)
		{
			denied = true;
			return result;
		}
		char line[256];
		char pid[16];
		snprintf(pid, sizeof(pid), "%d", (int)getpid());
		const size_t pidLength = strlen(pid);
		while (fgets(line, sizeof(line), f))
		{
			// the line with our pid as a word of its own
			for (char *p = strstr(line, pid); p != nullptr; p = strstr(p + 1, pid))
			{
				const bool start = p == line || p[-1] == ' ' || p[-1] == '\t';
				const char end = p[pidLength];
				if (start && (end == ' ' || end == '\t' || end == '\n' || end == '\0'))
				{
					result = line;
					result.StripLeftRight();
					break;
				}
			}
			if (result.IsNotEmpty()) break;
		}
		fclose(f);
		return result;
	}

	void InitAngleCounters()
	{
		if (!gl.modelstring || strstr(gl.modelstring, "ANGLE") == nullptr) return;
		auto begin = (PFN_BeginPerfMonitorAMD)gl_GetESProcAddress("glBeginPerfMonitorAMD");
		auto counters = (PFN_GetPerfMonitorCountersAMD)gl_GetESProcAddress("glGetPerfMonitorCountersAMD");
		auto counterString = (PFN_GetPerfMonitorCounterStringAMD)gl_GetESProcAddress("glGetPerfMonitorCounterStringAMD");
		auto getData = (PFN_GetPerfMonitorCounterDataAMD)gl_GetESProcAddress("glGetPerfMonitorCounterDataAMD");
		if (!begin || !counters || !counterString || !getData) return;

		// ANGLE: one group ("vulkan"), monitor 0 only; while a monitor is begun the counters are kept up to date
		GLint numCounters = 0, maxActive = 0;
		counters(0, &numCounters, &maxActive, 0, nullptr);
		if (numCounters <= 0 || numCounters > 1024) return;
		for (GLint i = 0; i < numCounters; i++)
		{
			char name[96] = {};
			GLsizei length = 0;
			counterString(0, (GLuint)i, (GLsizei)sizeof(name) - 1, &length, name);
			MemReport.names.Push(name);
			MemReport.last.Push(0);
		}
		begin(0);
		while (glGetError() != GL_NO_ERROR) {}
		MemReport.getData = getData;
	}

	FString AngleCountersText()
	{
		FString gauges, changed;
		if (!MemReport.getData) return gauges;
		GLuint size = 0;
		GLint written = 0;
		MemReport.getData(0, PERFMON_RESULT_SIZE_AMD, (GLsizei)sizeof(size), &size, &written);
		if (size == 0 || size > (1u << 20)) return gauges;
		MemReport.results.Resize(size);
		written = 0;
		MemReport.getData(0, PERFMON_RESULT_AMD, (GLsizei)size, (GLuint *)MemReport.results.Data(), &written);
		struct Triplet { uint32_t group, counter; uint64_t value; };
		const Triplet *t = (const Triplet *)MemReport.results.Data();
		const int count = written > 0 ? int(size_t(written) / sizeof(Triplet)) : 0;
		for (int i = 0; i < count; i++)
		{
			const unsigned index = t[i].counter;
			if (t[i].group != 0 || index >= MemReport.names.Size()) continue;
			const uint64_t v = t[i].value;
			const long long delta = (long long)(v - MemReport.last[index]);
			MemReport.last[index] = v;
			const char *name = MemReport.names[index].GetChars();
			bool gauge = false;
			for (const char *g : AngleGauges) gauge |= strcmp(g, name) == 0;
			char item[128];
			if (gauge)
			{
				snprintf(item, sizeof(item), "%s%s %llu (%+lld)", gauges.IsEmpty() ? "" : ", ", name, (unsigned long long)v, delta);
				gauges += item;
			}
			else if (delta != 0)
			{
				snprintf(item, sizeof(item), "%s%s %+lld", changed.IsEmpty() ? "" : ", ", name, delta);
				changed += item;
			}
		}
		while (glGetError() != GL_NO_ERROR) {}
		if (changed.IsNotEmpty())
		{
			gauges += " | changed: ";
			gauges += changed;
		}
		return gauges;
	}
}


static void ReportMemory()
{
	if (gl_mem_report <= 0) return;
	if (!MemReport.initialized)
	{
		MemReport.initialized = true;
		InitAngleCounters();
	}
	const uint64_t now = I_msTime();
	if (MemReport.lastTime != 0 && now - MemReport.lastTime < uint64_t(gl_mem_report) * 1000) return;
	MemReport.lastTime = now;

	const uint64_t rss = ProcessRss();
	const size_t heap = mallinfo().uordblks;
	const size_t textures = FHardwareTexture::sTextureBytes;
	const size_t buffers = GLBuffer::sBufferBytes;

	char text[512];
	snprintf(text, sizeof(text), "Memory: RSS %lld MB (%+lld), native heap %lld MB (%+lld)",
		MB(rss), MB(rss) - MB(MemReport.lastRss), MB(heap), MB(heap) - MB(MemReport.lastHeap));
	FString line = text;
	const Mappings maps = ProcessMappings();
	snprintf(text, sizeof(text), ", address space %lld MB (%+lld), mali mappings %u (%+d), %lld MB (%+lld)",
		MB(maps.total), MB(maps.total) - MB(MemReport.lastMappings.total),
		maps.maliCount, int(maps.maliCount) - int(MemReport.lastMappings.maliCount),
		MB(maps.maliBytes), MB(maps.maliBytes) - MB(MemReport.lastMappings.maliBytes));
	line += text;
	FString mali = MaliProcessMemory();
	if (mali.IsNotEmpty())
	{
		line += ", GPU [";
		line += mali;
		line += "]";
	}
	snprintf(text, sizeof(text), " | textures %llu, %lld MB (%+lld), uploads %+lld, buffers %llu, %lld MB (%+lld), programs %d, render targets %lld MB (pooled %lld MB), created %+d, deleted %+d",
		(unsigned long long)FHardwareTexture::sLiveTextures, MB(textures), MB(textures) - MB(MemReport.lastTextures),
		(long long)(FHardwareTexture::sTextureUploads - MemReport.lastUploads),
		(unsigned long long)GLBuffer::sLiveBuffers, MB(buffers), MB(buffers) - MB(MemReport.lastBuffers),
		FShader::sLivePrograms, MB(rtStatsGL.bytes), MB(rtStatsGL.pooledFreeBytes),
		int(rtStatsGL.created - MemReport.lastRtCreated), int(rtStatsGL.deleted - MemReport.lastRtDeleted));
	line += text;
	FString angle = AngleCountersText();
	if (angle.IsNotEmpty())
	{
		line += " | ANGLE: ";
		line += angle;
	}
	Printf("%s\n", line.GetChars());

	MemReport.lastRss = rss;
	MemReport.lastMappings = maps;
	MemReport.lastHeap = heap;
	MemReport.lastTextures = textures;
	MemReport.lastBuffers = buffers;
	MemReport.lastUploads = FHardwareTexture::sTextureUploads;
	MemReport.lastRtCreated = rtStatsGL.created;
	MemReport.lastRtDeleted = rtStatsGL.deleted;
}
#endif

void OpenGLFrameBuffer::Swap()
{
	BatchStatsEndFrame();
	GLRenderTargetPool::FrameEnd();
	ProgramBinaryCache::ProcessPendingTrim();
#ifdef ANDROID
	if (gl.flags & RFL_INVALIDATE_BUFFER)
	{
		// The window's depth/stencil are never read: don't let a tiler write them back.
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		static const GLenum attachments[] = { GL_DEPTH, GL_STENCIL };
		glInvalidateFramebuffer(GL_FRAMEBUFFER, 2, attachments);
	}
#endif
	bool swapbefore = gl_finishbeforeswap && camtexcount == 0;
	Finish.Reset();
	Finish.Clock();
	gl_RenderState.EndStreamFrame();	// fence of the instance/index stream region this frame wrote
	if (!mPipelineType)	// not the live cvar: the buffers were set up for the value at startup
	{
#ifndef ANDROID		
		if (swapbefore) glFinish();
#endif		
		FPSLimit();
		SwapBuffers();
#ifndef ANDROID		
		if (!swapbefore) glFinish();
#endif		
	}
	else
	{
		mVertexData->DropSync();
		FPSLimit();
		SwapBuffers();

		mVertexData->NextPipelineBuffer();
		mVertexData->WaitSync();
	}
	gl_RenderState.BeginStreamFrame();

#ifdef ANDROID
	GLRenderer->mShaderManager->SetActiveShader(0);
#endif


	Finish.Unclock();
	camtexcount = 0;
	FHardwareTexture::UnbindAll();
	gl_RenderState.ClearLastMaterial();
	mDebug->Update();
#ifdef __ANDROID__
	ReportMemory();
#endif
}

//==========================================================================
//
// Enable/disable vertical sync
//
//==========================================================================

void OpenGLFrameBuffer::SetVSync(bool vsync)
{
	// Switch to the default frame buffer because some drivers associate the vsync state with the bound FB object.
	GLint oldDrawFramebufferBinding = 0, oldReadFramebufferBinding = 0;
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &oldDrawFramebufferBinding);
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &oldReadFramebufferBinding);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

	Super::SetVSync(vsync);

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, oldDrawFramebufferBinding);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, oldReadFramebufferBinding);
}

//===========================================================================
//
//
//===========================================================================

void OpenGLFrameBuffer::SetTextureFilterMode()
{
	if (GLRenderer != nullptr && GLRenderer->mSamplerManager != nullptr) GLRenderer->mSamplerManager->SetTextureFilterMode();
}

IHardwareTexture *OpenGLFrameBuffer::CreateHardwareTexture(int numchannels)
{
	return new FHardwareTexture(numchannels);
}

void OpenGLFrameBuffer::PrecacheMaterial(FMaterial *mat, int translation)
{
	if (mat->Source()->GetUseType() == ETextureType::SWCanvas) return;

	int numLayers = mat->NumLayers();
	MaterialLayerInfo* layer;
	auto base = static_cast<FHardwareTexture*>(mat->GetLayer(0, translation, &layer));

	if (base->BindOrCreate(layer->layerTexture, 0, CLAMP_NONE, translation, layer->scaleFlags))
	{
		for (int i = 1; i < numLayers; i++)
		{
			auto systex = static_cast<FHardwareTexture*>(mat->GetLayer(i, 0, &layer));
			systex->BindOrCreate(layer->layerTexture, i, CLAMP_NONE, 0, layer->scaleFlags);
		}
	}
	// unbind everything.
	FHardwareTexture::UnbindAll();
	gl_RenderState.ClearLastMaterial();
}

IVertexBuffer *OpenGLFrameBuffer::CreateVertexBuffer()
{
	return new GLVertexBuffer;
}

IIndexBuffer *OpenGLFrameBuffer::CreateIndexBuffer()
{
	return new GLIndexBuffer;
}

IDataBuffer *OpenGLFrameBuffer::CreateDataBuffer(int bindingpoint, bool ssbo, bool needsresize)
{
	return new GLDataBuffer(bindingpoint, ssbo);
}

void OpenGLFrameBuffer::BlurScene(float amount)
{
	GLRenderer->BlurScene(amount);
}

void OpenGLFrameBuffer::InitLightmap(int LMTextureSize, int LMTextureCount, TArray<uint16_t>& LMTextureData)
{
	if (LMTextureData.Size() > 0)
	{
		GLint activeTex = 0;
		glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTex);
		glActiveTexture(GL_TEXTURE0 + 17);

		if (GLRenderer->mLightMapID == 0)
			glGenTextures(1, (GLuint*)&GLRenderer->mLightMapID);

		glBindTexture(GL_TEXTURE_2D_ARRAY, GLRenderer->mLightMapID);
		glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGB16F, LMTextureSize, LMTextureSize, LMTextureCount, 0, GL_RGB, GL_HALF_FLOAT, &LMTextureData[0]);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glGenerateMipmap(GL_TEXTURE_2D_ARRAY);

		glActiveTexture(activeTex);

		LMTextureData.Reset(); // We no longer need this, release the memory
	}
}

void OpenGLFrameBuffer::SetViewportRects(IntRect *bounds)
{
	Super::SetViewportRects(bounds);
	if (!bounds)
	{
		auto vrmode = VRMode::GetVRMode(true);
		vrmode->AdjustViewport(this);
	}
}

void OpenGLFrameBuffer::UpdatePalette()
{
	if (GLRenderer)
		GLRenderer->ClearTonemapPalette();
}

FRenderState* OpenGLFrameBuffer::RenderState()
{
	return &gl_RenderState;
}

void OpenGLFrameBuffer::AmbientOccludeScene(float m5)
{
	gl_RenderState.EnableDrawBuffers(1);
	GLRenderer->AmbientOccludeScene(m5);
	glViewport(screen->mSceneViewport.left, mSceneViewport.top, mSceneViewport.width, mSceneViewport.height);
	GLRenderer->mBuffers->BindSceneFB(true);
	gl_RenderState.EnableDrawBuffers(gl_RenderState.GetPassDrawBufferCount());
	gl_RenderState.Apply();
}

void OpenGLFrameBuffer::FirstEye()
{
	GLRenderer->mBuffers->CurrentEye() = 0;  // always begin at zero, in case eye count changed
}

void OpenGLFrameBuffer::NextEye(int eyecount)
{
	GLRenderer->mBuffers->NextEye(eyecount);
}

void OpenGLFrameBuffer::SetSceneRenderTarget(bool useSSAO)
{
	GLRenderer->mBuffers->BindSceneFB(useSSAO);
}

void OpenGLFrameBuffer::UpdateShadowMap()
{
	if (mShadowMap.PerformUpdate())
	{
		FGLDebug::PushGroup("ShadowMap");

		FGLPostProcessState savedState;

		static_cast<GLDataBuffer*>(screen->mShadowMap.mLightList)->BindBase();
		static_cast<GLDataBuffer*>(screen->mShadowMap.mNodesBuffer)->BindBase();
		static_cast<GLDataBuffer*>(screen->mShadowMap.mLinesBuffer)->BindBase();

		GLRenderer->mBuffers->BindShadowMapFB();

		GLRenderer->mShadowMapShader->Bind();
		GLRenderer->mShadowMapShader->Uniforms->ShadowmapQuality = gl_shadowmap_quality;
		GLRenderer->mShadowMapShader->Uniforms->NodesCount = screen->mShadowMap.NodesCount();
		GLRenderer->mShadowMapShader->Uniforms.SetData();
		static_cast<GLDataBuffer*>(GLRenderer->mShadowMapShader->Uniforms.GetBuffer())->BindBase();

		glViewport(0, 0, gl_shadowmap_quality, 1024);
		GLRenderer->RenderScreenQuad();

		const auto& viewport = screen->mScreenViewport;
		glViewport(viewport.left, viewport.top, viewport.width, viewport.height);

		GLRenderer->mBuffers->BindShadowMapTexture(16);
		FGLDebug::PopGroup();
		screen->mShadowMap.FinishUpdate();
	}
}

void OpenGLFrameBuffer::WaitForCommands(bool finish)
{
	glFinish();
}

void OpenGLFrameBuffer::SetSaveBuffers(bool yes)
{
	if (!GLRenderer) return;
	if (yes)
	{
		// Created only for the savegame picture and released right after it.
		GLRenderer->mSaveBuffers->Setup(SAVEPICWIDTH, SAVEPICHEIGHT, SAVEPICWIDTH, SAVEPICHEIGHT);
		GLRenderer->mBuffers = GLRenderer->mSaveBuffers;
	}
	else
	{
		GLRenderer->mBuffers = GLRenderer->mScreenBuffers;
		delete GLRenderer->mSaveBuffers;
		GLRenderer->mSaveBuffers = new FGLRenderBuffers();
	}
}

bool OpenGLFrameBuffer::SceneDataMultisampled()
{
	return GLRenderer && GLRenderer->mBuffers ? GLRenderer->mBuffers->SceneDataMultisampled() : DFrameBuffer::SceneDataMultisampled();
}

int OpenGLFrameBuffer::SceneSampleCount()
{
	return GLRenderer && GLRenderer->mBuffers ? GLRenderer->mBuffers->GetSceneSamples() : DFrameBuffer::SceneSampleCount();
}

//===========================================================================
//
//
//
//===========================================================================

void OpenGLFrameBuffer::BeginFrame()
{
	SetViewportRects(nullptr);
	mViewpoints->Clear();
	if (GLRenderer != nullptr)
		GLRenderer->BeginFrame();
}

//===========================================================================
//
//	Takes a screenshot
//
//===========================================================================

TArray<uint8_t> OpenGLFrameBuffer::GetScreenshotBuffer(int &pitch, ESSType &color_type, float &gamma)
{
	const auto &viewport = mOutputLetterbox;

	// Grab what is in the back buffer.
	// We cannot rely on SCREENWIDTH/HEIGHT here because the output may have been scaled.
	TArray<uint8_t> pixels;
	pixels.Resize(viewport.width * viewport.height * 3);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(viewport.left, viewport.top, viewport.width, viewport.height, GL_RGB, GL_UNSIGNED_BYTE, &pixels[0]);
	glPixelStorei(GL_PACK_ALIGNMENT, 4);

	// Copy to screenshot buffer:
	int w = SCREENWIDTH;
	int h = SCREENHEIGHT;

	TArray<uint8_t> ScreenshotBuffer(w * h * 3, true);

	float rcpWidth = 1.0f / w;
	float rcpHeight = 1.0f / h;
	for (int y = 0; y < h; y++)
	{
		for (int x = 0; x < w; x++)
		{
			float u = (x + 0.5f) * rcpWidth;
			float v = (y + 0.5f) * rcpHeight;
			int sx = u * viewport.width;
			int sy = v * viewport.height;
			int sindex = (sx + sy * viewport.width) * 3;
			int dindex = (x + (h - y - 1) * w) * 3;
			ScreenshotBuffer[dindex] = pixels[sindex];
			ScreenshotBuffer[dindex + 1] = pixels[sindex + 1];
			ScreenshotBuffer[dindex + 2] = pixels[sindex + 2];
		}
	}

	pitch = w * 3;
	color_type = SS_RGB;

	// Screenshot should not use gamma correction if it was already applied to rendered image
	gamma = 1;
	if (vid_hdr_active && vid_fullscreen)
		gamma *= 2.2f;
	return ScreenshotBuffer;
}

//===========================================================================
//
// 2D drawing
//
//===========================================================================

void OpenGLFrameBuffer::Draw2D()
{
	if (GLRenderer != nullptr)
	{
		GLRenderer->mBuffers->BindCurrentFB();
		::Draw2D(twod, gl_RenderState);
	}
}

void OpenGLFrameBuffer::PostProcessScene(bool swscene, int fixedcm, float flash, const std::function<void()> &afterBloomDrawEndScene2D)
{
	if (!swscene) GLRenderer->mBuffers->BlitSceneToTexture(); // Copy the resulting scene to the current post process texture
	GLRenderer->PostProcessScene(fixedcm, flash, afterBloomDrawEndScene2D);
}

bool OpenGLFrameBuffer::CompileNextShader()
{
	return GLRenderer->mShaderManager->CompileNextShader();
}

int OpenGLFrameBuffer::GetShaderCount()
{
	return GLRenderer && GLRenderer->mShaderManager ? GLRenderer->mShaderManager->StartupProgramCount() : 0;
}

//==========================================================================
//
// OpenGLFrameBuffer :: WipeStartScreen
//
// Called before the current screen has started rendering. This needs to
// save what was drawn the previous frame so that it can be animated into
// what gets drawn this frame.
//
//==========================================================================

FTexture *OpenGLFrameBuffer::WipeStartScreen()
{
	const auto &viewport = screen->mScreenViewport;

	auto tex = new FWrapperTexture(viewport.width, viewport.height, 1);
	tex->GetSystemTexture()->CreateTexture(nullptr, viewport.width, viewport.height, 0, false, "WipeStartScreen");
#ifndef ANDROID
	glFinish();
#endif
	static_cast<FHardwareTexture*>(tex->GetSystemTexture())->Bind(0, false);

	GLRenderer->mBuffers->BindCurrentFB();
	glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, viewport.left, viewport.top, viewport.width, viewport.height);
	return tex;
}

//==========================================================================
//
// OpenGLFrameBuffer :: WipeEndScreen
//
// The screen we want to animate to has just been drawn.
//
//==========================================================================

FTexture *OpenGLFrameBuffer::WipeEndScreen()
{
	GLRenderer->Flush();
	const auto &viewport = screen->mScreenViewport;
	auto tex = new FWrapperTexture(viewport.width, viewport.height, 1);
	tex->GetSystemTexture()->CreateTexture(NULL, viewport.width, viewport.height, 0, false, "WipeEndScreen");
#ifndef ANDROID	
	glFinish();
#endif
	static_cast<FHardwareTexture*>(tex->GetSystemTexture())->Bind(0, false);
	GLRenderer->mBuffers->BindCurrentFB();
	glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, viewport.left, viewport.top, viewport.width, viewport.height);
	return tex;
}

bool OpenGLFrameBuffer::HasNVidiaVRAMExt() { return gl.flags & RFL_NV_MEM; }
bool OpenGLFrameBuffer::HasATIVRAMExt() { return gl.flags & RFL_ATI_MEM; }

}
