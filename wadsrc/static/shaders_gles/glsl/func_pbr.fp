/*
** func_pbr.fp
**
**
**
**---------------------------------------------------------------------------
**
** Copyright 2018-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

void SetupMaterial(inout Material material)
{
	SetMaterialProps(material, vTexCoord.st);
#if __VERSION__ >= 300	// material_pbr.fp (GLSL ES 1.00 lights PBR materials as normal ones)
	material.Metallic = texture2D(metallictexture, vTexCoord.st).r;
	material.Roughness = texture2D(roughnesstexture, vTexCoord.st).r;
	material.AO = texture2D(aotexture, vTexCoord.st).r;
#endif
}
