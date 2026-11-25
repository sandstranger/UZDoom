/*
** vk_framebuffer.cpp
**
** Vulkan backend
**
**---------------------------------------------------------------------------
**
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** Copyright 2016-2020 Magnus Norddahl
**
** SPDX-License-Identifier: Zlib
**
**---------------------------------------------------------------------------
**
*/

#include <zvulkan/vulkanobjects.h>
#include <zvulkan/vulkandevice.h>
#include <zvulkan/vulkanbuilders.h>
#include <zvulkan/vulkanswapchain.h>
#include "vulkan/system/vk_renderdevice.h"
#include "vulkan/renderer/vk_postprocess.h"
#include "vk_framebuffer.h"

CVAR(Bool, vk_hdr, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
#ifdef ANDROID
#include <atomic>
static bool vk_exclusivefullscreen = true;
// (atomics: the launcher sets them through JNA from its own thread)
static std::atomic<bool> isApplicationPaused{false};
static std::atomic<bool> needToRecreaseVulkanSurfaces{false};
#else
CVAR(Bool, vk_exclusivefullscreen, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
#endif

VkFramebufferManager::VkFramebufferManager(VulkanRenderDevice* fb) : fb(fb)
{
	SwapChain = VulkanSwapChainBuilder()
		.Create(fb->device.get());

	SwapChainImageAvailableSemaphore = SemaphoreBuilder()
		.DebugName("SwapChainImageAvailableSemaphore")
		.Create(fb->device.get());
}

VkFramebufferManager::~VkFramebufferManager()
{
}

#ifdef ANDROID
extern "C" {
__attribute__((used)) __attribute__((visibility("default")))
void DestroyVulkanSwapChain() {
    isApplicationPaused = true;
}

__attribute__((used)) __attribute__((visibility("default")))
void RecreateVulkanSwapChain() {
    // The recreation is asked before the pause ends: a game thread that sees the pause over also sees it
    if (isApplicationPaused) {
        needToRecreaseVulkanSurfaces = true;
        isApplicationPaused = false;
    }
}
}
#endif

void VkFramebufferManager::AcquireImage()
{
#ifdef ANDROID
    if (isApplicationPaused){
        return;
    }
#endif
	bool exclusiveFullscreen = true;
#ifdef ANDROID
	if (SwapChain->Lost() || needToRecreaseVulkanSurfaces  || fb->GetClientWidth() != CurrentWidth || fb->GetClientHeight() != CurrentHeight || fb->GetVSync() != CurrentVSync || CurrentHdr != vk_hdr || CurrentExclusiveFullscreen != exclusiveFullscreen)
#else
    bool exclusiveFullscreen = fb->IsFullscreen() && vk_exclusivefullscreen;
	if (SwapChain->Lost() || fb->GetClientWidth() != CurrentWidth || fb->GetClientHeight() != CurrentHeight || fb->GetVSync() != CurrentVSync || CurrentHdr != vk_hdr || CurrentExclusiveFullscreen != exclusiveFullscreen)
#endif
	{
		Framebuffers.clear();

		CurrentWidth = fb->GetClientWidth();
		CurrentHeight = fb->GetClientHeight();
		CurrentVSync = fb->GetVSync();
		CurrentHdr = vk_hdr;
		CurrentExclusiveFullscreen = exclusiveFullscreen;

#ifdef ANDROID
        if (needToRecreaseVulkanSurfaces.exchange(false)){
            fb->RecreateSurface();
        }
#endif
		SwapChain->Create(CurrentWidth, CurrentHeight, CurrentVSync ? 2 : 3, CurrentVSync, CurrentHdr, CurrentExclusiveFullscreen);

		RenderFinishedSemaphores.clear();
		for (int i = 0; i < SwapChain->ImageCount(); i++)
		{
			RenderFinishedSemaphores.push_back(SemaphoreBuilder()
				.DebugName("RenderFinishedSemaphore")
				.Create(fb->device.get()));
		}
	}

	PresentImageIndex = SwapChain->AcquireImage(SwapChainImageAvailableSemaphore.get());
	if (PresentImageIndex != -1)
	{
		fb->GetPostprocess()->DrawPresentTexture(fb->mOutputLetterbox, true, false);
	}
}

void VkFramebufferManager::QueuePresent()
{
	if (PresentImageIndex != -1)
		SwapChain->QueuePresent(PresentImageIndex, RenderFinishedSemaphores[PresentImageIndex].get());
}
