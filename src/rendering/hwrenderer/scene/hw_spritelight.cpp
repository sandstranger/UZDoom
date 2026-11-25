/*
** hw_spritelight.cpp
**
** Light level / fog management / dynamic lights
**
**---------------------------------------------------------------------------
**
** Copyright 2002-2016 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#include "c_dispatch.h"
#include "a_dynlight.h"
#include "p_local.h"
#include "p_effect.h"
#include "g_level.h"
#include "g_levellocals.h"
#include "actorinlines.h"
#include "hw_dynlightdata.h"
#include "hw_shadowmap.h"
#include "hwrenderer/scene/hw_drawinfo.h"
#include "hwrenderer/scene/hw_drawstructs.h"
#include "models.h"
#include "hw_cvars.h"
#include <cmath>	// needed for std::floor on mac
#include "hw_renderstate.h"
#include "selaco.h"

EXTERN_CVAR(Bool, lm_lightprobes);
EXTERN_CVAR(Bool, lm_heavyprobes);

template<class T>
T smoothstep(const T edge0, const T edge1, const T x)
{
	auto t = clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
	return t * t * (3.0 - 2.0 * t);
}

LightProbe* FindLightProbe(FLevelLocals* level, float x, float y, float z, float floorz /* = -FLT_MAX */)
{
	LightProbe* foundprobe = nullptr;
	if (lm_lightprobes && level->LightProbes.Size() > 0)
	{
#if 1
		double rcpCellSize = 1.0 / level->LPCellSize;
		int gridCenterX = (int)std::floor(x * rcpCellSize) - level->LPMinX;
		int gridCenterY = (int)std::floor(y * rcpCellSize) - level->LPMinY;
		int gridWidth = level->LPWidth;
		int gridHeight = level->LPHeight;
		float lastdist = 0.0f;
		// (Selaco: only the probes of the cell itself)
		const int range = g_SelacoMode ? 0 : 1;
		for (int gridY = gridCenterY - range; gridY <= gridCenterY + range; gridY++)
		{
			for (int gridX = gridCenterX - range; gridX <= gridCenterX + range; gridX++)
			{
				if (gridX >= 0 && gridY >= 0 && gridX < gridWidth && gridY < gridHeight)
				{
					const LightProbeCell& cell = level->LPCells[gridX + (size_t)gridY * gridWidth];
					for (int i = 0; i < cell.NumProbes; i++)
					{
						LightProbe* probe = cell.FirstProbe + i;
						// Check the probe isn't under the floor, which can happen with thin 3D floors
						if (floorz < probe->Z)
						{
							float dx = probe->X - x;
							float dy = probe->Y - y;
							float dz = probe->Z - z;
							float dist = dx * dx + dy * dy + dz * dz;
							if (!foundprobe || dist < lastdist)
							{
								foundprobe = probe;
								lastdist = dist;
							}
						}
					}
				}
			}
		}
#else
		float lastdist = 0.0f;
		for (unsigned int i = 0; i < level->LightProbes.Size(); i++)
		{
			LightProbe *probe = &level->LightProbes[i];
			float dx = probe->X - x;
			float dy = probe->Y - y;
			float dz = probe->Z - z;
			float dist = dx * dx + dy * dy + dz * dz;
			if (i == 0 || dist < lastdist)
			{
				foundprobe = probe;
				lastdist = dist;
			}
		}
#endif
	}
	return foundprobe;
}

bool TryGetLightProbeColor(FLevelLocals* level, AActor* actor, FVector3& out)
{
	if (!actor || (lm_heavyprobes && actor->HeavyProbeIndex >= 0))
	{
		return false;
	}

	AActor* sourceActor = (actor->flags9 & MF9_MASTERLIGHTPROBES) && actor->master != nullptr ? actor->master : actor;
	DVector3 samplePos = sourceActor->GetLightProbeSamplePosition(level->LPCellSize);

	return TryGetLightProbeColor(level, samplePos.X, samplePos.Y, samplePos.Z, out, sourceActor->floorz);
}

bool TryGetLightProbeColor(FLevelLocals* level, float x, float y, float z, FVector3& out, float floorz /* = -FLT_MAX */)
{
	if (LightProbe* probe = FindLightProbe(level, x, y, z, floorz))
	{
		out = probe->CalculateColor(level);
		return true;
	}
	else
	{
		return false;
	}
}

void SetHeavyProbe(FRenderState& state, FLevelLocals* level, AActor* actor)
{
	if (lm_heavyprobes && actor->HeavyProbeIndex >= 0 && actor->HeavyProbeIndex < level->HeavyProbes.size())
	{
		HeavyProbe& heavyProbe = level->HeavyProbes[actor->HeavyProbeIndex];

		HeavyProbeSampleData samples[4];
		heavyProbe.CalculateSampleData(level, samples);
		
		state.SetHeavyProbe(heavyProbe.numSamples, samples);
	}
}

//==========================================================================
//
// Sets a single light value from all dynamic lights affecting the specified location
//
//==========================================================================

void HWDrawInfo::GetDynSpriteLight(AActor *self, float x, float y, float z, FSection *sec, int portalgroup, float *out)
{
	float frac, lr, lg, lb;
	float radius;

	out[0] = out[1] = out[2] = 0.f;

	if (!g_SelacoMode)
	{
		LightProbe* probe = FindLightProbe(Level, x, y, z);
		if (probe)
		{
			out[0] = probe->Red;
			out[1] = probe->Green;
			out[2] = probe->Blue;
		}
	}

	for(FDynamicLight * light : sec->dlist)
	{
		if (light->ShouldLightActor(self))
		{
			float dist;
			FVector3 L;

			// This is a performance critical section of code where we cannot afford to let the compiler decide whether to inline the function or not.
			// This will do the calculations explicitly rather than calling one of AActor's utility functions.
			if (Level->Displacements.size > 0)
			{
				int fromgroup = light->Sector->PortalGroup;
				int togroup = portalgroup;
				if (fromgroup == togroup || fromgroup == 0 || togroup == 0) goto direct;

				DVector2 offset = Level->Displacements.getOffset(fromgroup, togroup);
				L = FVector3(x - (float)(light->X() + offset.X), y - (float)(light->Y() + offset.Y), z - (float)light->Z());
			}
			else
			{
			direct:
				L = FVector3(x - (float)light->X(), y - (float)light->Y(), z - (float)light->Z());
			}

			dist = (float)L.LengthSquared();
			radius = light->GetRadius();

			if (dist < radius * radius)
			{
				dist = sqrtf(dist);	// only calculate the square root if we really need it.

				frac = 1.0f - (dist / radius);

				if (light->IsSpot())
				{
					L *= -1.0f / dist;
					DAngle negPitch = -light->Pitch;
					DAngle Angle = light->Yaw;
					double xyLen = negPitch.Cos();
					double spotDirX = -Angle.Cos() * xyLen;
					double spotDirY = -Angle.Sin() * xyLen;
					double spotDirZ = -negPitch.Sin();
					double cosDir = L.X * spotDirX + L.Y * spotDirY + L.Z * spotDirZ;
					frac *= (float)smoothstep(light->pSpotOuterAngle->Cos(), light->pSpotInnerAngle->Cos(), cosDir);
				}

				if (frac > 0 && (!light->IsShadowMapped() || (light->GetRadius() > 0 && screen->mShadowMap.ShadowTest(light->Pos, { x, y, z }))))
				{
					lr = light->GetRed() / 255.0f;
					lg = light->GetGreen() / 255.0f;
					lb = light->GetBlue() / 255.0f;

					if (light->target && (light->target->renderflags2 & RF2_LIGHTMULTALPHA))
					{
						float alpha = (float)light->target->Alpha;
						lr *= alpha;
						lg *= alpha;
						lb *= alpha;
					}

					// Get GLDEFS intensity
					lr *= light->GetLightDefIntensity();
					lg *= light->GetLightDefIntensity();
					lb *= light->GetLightDefIntensity();

					if (light->IsSubtractive())
					{
						float bright = (float)FVector3(lr, lg, lb).Length();
						FVector3 lightColor(lr, lg, lb);
						lr = (bright - lr) * -1;
						lg = (bright - lg) * -1;
						lb = (bright - lb) * -1;
					}

					out[0] += lr * frac;
					out[1] += lg * frac;
					out[2] += lb * frac;
				}
			}
		}
	}

	// Add player lights
	// @Cockatrice - Add any player lights to every sprite. This is a hack to prevent the big hit to CPU power by constantly linking and unlinking moving spotlights
	AActor* mo = players[consoleplayer].mo;
	if (mo) {
		for (auto* light : Level->playerLights) {
			if (light->ShouldLightActor(self))
			{
				float dist;
				FVector3 L;

				// This is a performance critical section of code where we cannot afford to let the compiler decide whether to inline the function or not.
				// This will do the calculations explicitly rather than calling one of AActor's utility functions.
				if (Level->Displacements.size > 0)
				{
					int fromgroup = light->Sector->PortalGroup;
					int togroup = portalgroup;
					if (fromgroup == togroup || fromgroup == 0 || togroup == 0) goto direct2;

					DVector2 offset = Level->Displacements.getOffset(fromgroup, togroup);
					L = FVector3(x - (float)(light->X() + offset.X), y - (float)(light->Y() + offset.Y), z - (float)light->Z());
				}
				else
				{
				direct2:
					L = FVector3(x - (float)light->X(), y - (float)light->Y(), z - (float)light->Z());
				}

				dist = (float)L.LengthSquared();
				radius = light->GetRadius();

				if (dist < radius * radius)
				{
					dist = sqrtf(dist);	// only calculate the square root if we really need it.

					frac = 1.0f - (dist / radius);

					if (light->IsSpot())
					{
						L *= -1.0f / dist;
						DAngle negPitch = -light->Pitch;
						DAngle Angle = light->Yaw;
						double xyLen = negPitch.Cos();
						double spotDirX = -Angle.Cos() * xyLen;
						double spotDirY = -Angle.Sin() * xyLen;
						double spotDirZ = -negPitch.Sin();
						double cosDir = L.X * spotDirX + L.Y * spotDirY + L.Z * spotDirZ;
						frac *= (float)smoothstep(light->pSpotOuterAngle->Cos(), light->pSpotInnerAngle->Cos(), cosDir);
					}

					if (frac > 0 && (!light->IsShadowMapped() || (light->GetRadius() > 0 && screen->mShadowMap.ShadowTest(light->Pos, { x, y, z }))))
					{
						lr = light->GetRed() / 255.0f;
						lg = light->GetGreen() / 255.0f;
						lb = light->GetBlue() / 255.0f;
						if (light->IsSubtractive())
						{
							float bright = (float)FVector3(lr, lg, lb).Length();
							FVector3 lightColor(lr, lg, lb);
							lr = (bright - lr) * -1;
							lg = (bright - lg) * -1;
							lb = (bright - lb) * -1;
						}

						out[0] += lr * frac;
						out[1] += lg * frac;
						out[2] += lb * frac;
					}
				}
			}
		}
	}
}

void HWDrawInfo::GetDynSpriteLight(AActor *thing, HWSprite *particleSprite, float *out)
{
	if (thing && !(thing->renderflags2 & RF2_NODYNAMICLIGHTING))
	{
		GetDynSpriteLight(thing, (float)thing->X(), (float)thing->Y(), (float)thing->Center(), thing->section, thing->Sector->PortalGroup, out);
	}
	else if (particleSprite && particleSprite->isparticle && particleSprite->particlesubsector)
	{
		const particle_t* particle = particleSprite->particle;
		subsector_t* subsector = particleSprite->particlesubsector;
		if (particle)
		{
			if (!(particle->flags & SPF_NODYNAMICLIGHTING))
				GetDynSpriteLight(NULL, (float)particle->Pos.X, (float)particle->Pos.Y, (float)particle->Pos.Z, subsector->section, subsector->sector->PortalGroup, out);
		}
		else
		{
			// (Selaco: pooled particles have no particle_t, the sprite has their interpolated position)
			GetDynSpriteLight(NULL, particleSprite->x, particleSprite->y, particleSprite->z, subsector->section, subsector->sector->PortalGroup, out);
		}
	}
}

// static so that we build up a reserve (memory allocations stop)
// For multithread processing each worker thread needs its own copy, though.
static thread_local TArray<FDynamicLight*> addedLightsArray;

void hw_GetDynModelLight(AActor *self, FDynLightData &modellightdata, double ticFrac)
{
	modellightdata.Clear();

	if (self && self->Sector)
	{
		float actorradius = (float)self->RenderRadius();

		if(actorradius <= gl_model_light_optimization_radius)
		{
			int group = self->Sector->PortalGroup;
			FSection *sec = self->section;

			for(FDynamicLight * light : sec->dlist)
			{
				if (light->ShouldLightActor(self))
				{
					AddLightToList(modellightdata, group, light, true, ticFrac);
				}
			}
		}
		else
		{
			auto &addedLights = addedLightsArray;	// avoid going through the thread local storage for each use.

			addedLights.Clear();

			float x = (float)self->X();
			float y = (float)self->Y();
			float z = (float)self->Center();
			float radiusSquared = actorradius * actorradius;
			dl_validcount++;

			//TODO replace with blockmap search
			BSPWalkCircle(self->Level, x, y, radiusSquared, [&](subsector_t *subsector) // Iterate through all subsectors potentially touched by actor
			{
				auto section = subsector->section;
				if (section->validcount == dl_validcount) return;	// already done from a previous subsector.

				for(FDynamicLight * light : subsector->section->dlist)
				{
					if (light->ShouldLightActor(self))
					{
						int group = subsector->sector->PortalGroup;
						DVector3 pos = light->PosRelative(group);
						float radius = (float)(light->GetRadius() + actorradius);
						double dx = pos.X - x;
						double dy = pos.Y - y;
						double dz = pos.Z - z;
						double distSquared = dx * dx + dy * dy + dz * dz;
						if (distSquared < radius * radius) // Light and actor touches
						{
							if (std::find(addedLights.begin(), addedLights.end(), light) == addedLights.end()) // Check if we already added this light from a different subsector
							{
								AddLightToList(modellightdata, group, light, true, ticFrac);
								addedLights.Push(light);
							}
						}
					}
				}
			});
		}

		// @Cockatrice - Add player lights (Selaco: the list is only filled for Selaco)
		AActor* mo = players[consoleplayer].mo;
		if (mo) {
			for (auto* light : self->Level->playerLights) {
				if (light->ShouldLightActor(self))
				{
					AddLightToList(modellightdata, light->Sector->PortalGroup, light, true, ticFrac);
				}
			}
		}
	}
}
