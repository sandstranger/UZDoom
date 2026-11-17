/*
** material_specular.fp
**
**
**
**---------------------------------------------------------------------------
**
** Copyright 2013-2016 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

// The GL renderer's specular (Blinn-Phong) light model (GLSL ES 3.00 only). The surface's dynamic lights
// are in lights[] from 0 on: modulated ones up to uLightRange.y, subtractive ones up to uLightRange.z,
// additive ones up to uLightRange.w. Lights are clamped as GL's default light blend mode does.

vec2 lightAttenuation(int i, vec3 normal, vec3 viewdir, float lightcolorA, float glossiness, float specularLevel)
{
	vec4 lightpos = lights[i];
	vec4 lightspot1 = lights[i+2];
	vec4 lightspot2 = lights[i+3];

	float lightdistance = distance(lightpos.xyz, pixelpos.xyz);
	if (lightpos.w < lightdistance)
		return vec2(0.0); // Early out lights touching surface but not this fragment

	float attenuation = clamp((lightpos.w - lightdistance) / lightpos.w, 0.0, 1.0);

#if (DEF_HAS_SPOTLIGHT == 1) // Only perform test below if there are ANY spot lights on this surface.
	if (lightspot1.w == 1.0)
		attenuation *= spotLightAttenuation(lightpos, lightspot1.xyz, lightspot2.x, lightspot2.y);
#endif

	vec3 lightdir = normalize(lightpos.xyz - pixelpos.xyz);

	if (lightcolorA < 0.0) // Sign bit is the attenuated light flag
		attenuation *= clamp(dot(normal, lightdir), 0.0, 1.0);

	if (attenuation > 0.0) // Skip shadow map test if possible
		attenuation *= shadowAttenuation(lightpos, lightcolorA);

	if (attenuation <= 0.0)
		return vec2(0.0);

	vec3 halfdir = normalize(viewdir + lightdir);
	float specAngle = clamp(dot(halfdir, normal), 0.0, 1.0);
	float phExp = glossiness * 4.0;
	return vec2(attenuation, attenuation * specularLevel * pow(specAngle, phExp));
}

vec3 ProcessMaterialLight(Material material, vec3 color)
{
	vec4 dynlight = uDynLightColor;
	vec4 specular = vec4(0.0, 0.0, 0.0, 1.0);

	vec3 normal = material.Normal;
	vec3 viewdir = normalize(uCameraPos.xyz - pixelpos.xyz);

#if (DEF_DYNAMIC_LIGHTS_MOD == 1)
	// modulated lights
	for(int i=uLightRange.x; i<uLightRange.y; i+=4)
	{
		vec4 lightcolor = lights[i+1];
		vec2 attenuation = lightAttenuation(i, normal, viewdir, lightcolor.a, material.Glossiness, material.SpecularLevel);
		dynlight.rgb += lightcolor.rgb * attenuation.x;
		specular.rgb += lightcolor.rgb * attenuation.y;
	}
#endif

#if (DEF_DYNAMIC_LIGHTS_SUB == 1)
	// subtractive lights
	for(int i=uLightRange.y; i<uLightRange.z; i+=4)
	{
		vec4 lightcolor = lights[i+1];
		vec2 attenuation = lightAttenuation(i, normal, viewdir, lightcolor.a, material.Glossiness, material.SpecularLevel);
		dynlight.rgb -= lightcolor.rgb * attenuation.x;
		specular.rgb -= lightcolor.rgb * attenuation.y;
	}
#endif

	dynlight.rgb = clamp(color + desaturate(dynlight).rgb, 0.0, 1.4);
	specular.rgb = clamp(desaturate(specular).rgb, 0.0, 1.4);

	vec3 frag = material.Base.rgb * dynlight.rgb + material.Specular * specular.rgb;

#if (DEF_DYNAMIC_LIGHTS_ADD == 1)
	vec4 addlight = vec4(0.0,0.0,0.0,0.0);

	// additive lights
	for(int i=uLightRange.z; i<uLightRange.w; i+=4)
	{
		vec4 lightcolor = lights[i+1];
		vec2 attenuation = lightAttenuation(i, normal, viewdir, lightcolor.a, material.Glossiness, material.SpecularLevel);
		addlight.rgb += lightcolor.rgb * attenuation.x;
	}

	frag = clamp(frag + desaturate(addlight).rgb, 0.0, 1.0);
#endif

	return frag;
}
