/*
** material_pbr.fp
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

// The GL renderer's PBR light model (GLSL ES 3.00 only). The surface's dynamic lights are in lights[]
// from 0 on: modulated ones up to uLightRange.y, subtractive ones up to uLightRange.z. Like with GL,
// additive lights are not part of this model.

const float PI = 3.14159265359;

float DistributionGGX(vec3 N, vec3 H, float roughness)
{
	float a = roughness * roughness;
	float a2 = a * a;
	float NdotH = max(dot(N, H), 0.0);
	float NdotH2 = NdotH*NdotH;

	float nom = a2;
	float denom = (NdotH2 * (a2 - 1.0) + 1.0);
	denom = PI * denom * denom;

	return nom / denom;
}

float GeometrySchlickGGX(float NdotV, float roughness)
{
	float r = (roughness + 1.0);
	float k = (r * r) / 8.0;

	float nom = NdotV;
	float denom = NdotV * (1.0 - k) + k;

	return nom / denom;
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness)
{
	float NdotV = max(dot(N, V), 0.0);
	float NdotL = max(dot(N, L), 0.0);
	float ggx2 = GeometrySchlickGGX(NdotV, roughness);
	float ggx1 = GeometrySchlickGGX(NdotL, roughness);
	return ggx1 * ggx2;
}

vec3 fresnelSchlick(float cosTheta, vec3 F0)
{
	return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
}

vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness)
{
	return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(1.0 - cosTheta, 5.0);
}

float linearDistanceAttenuation(vec4 lightpos)
{
	float lightdistance = distance(lightpos.xyz, pixelpos.xyz);
	return clamp((lightpos.w - lightdistance) / lightpos.w, 0.0, 1.0);
}

// Cook-Torrance BRDF of the light at lights[i]
vec3 lightContributionPBR(int i, vec3 N, vec3 V, vec3 albedo, vec3 F0, float metallic, float roughness)
{
	vec4 lightpos = lights[i];
	vec4 lightcolor = lights[i+1];
	vec4 lightspot1 = lights[i+2];
	vec4 lightspot2 = lights[i+3];

	vec3 L = normalize(lightpos.xyz - pixelpos.xyz);
	vec3 H = normalize(V + L);

	float attenuation = linearDistanceAttenuation(lightpos);
#if (DEF_HAS_SPOTLIGHT == 1) // Only perform test below if there are ANY spot lights on this surface.
	if (lightspot1.w == 1.0)
		attenuation *= spotLightAttenuation(lightpos, lightspot1.xyz, lightspot2.x, lightspot2.y);
#endif
	if (lightcolor.a < 0.0)
		attenuation *= clamp(dot(N, L), 0.0, 1.0); // Sign bit is the attenuated light flag

	if (attenuation <= 0.0)
		return vec3(0.0);

	attenuation *= shadowAttenuation(lightpos, lightcolor.a);

	vec3 radiance = lightcolor.rgb * attenuation;

	float NDF = DistributionGGX(N, H, roughness);
	float G = GeometrySmith(N, V, L, roughness);
	vec3 F = fresnelSchlick(clamp(dot(H, V), 0.0, 1.0), F0);

	vec3 kS = F;
	vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);

	vec3 nominator = NDF * G * F;
	float denominator = 4.0 * clamp(dot(N, V), 0.0, 1.0) * clamp(dot(N, L), 0.0, 1.0);
	vec3 specular = nominator / max(denominator, 0.001);

	return (kD * albedo / PI + specular) * radiance;
}

vec3 ProcessMaterialLight(Material material, vec3 ambientLight)
{
	vec3 albedo = pow(material.Base.rgb, vec3(2.2)); // sRGB to linear
	ambientLight = pow(ambientLight, vec3(2.2));

	float metallic = material.Metallic;
	float roughness = material.Roughness;
	float ao = material.AO;

	vec3 N = material.Normal;
	vec3 V = normalize(uCameraPos.xyz - pixelpos.xyz);

	vec3 F0 = mix(vec3(0.04), albedo, metallic);

	vec3 Lo = uDynLightColor.rgb;

#if (DEF_DYNAMIC_LIGHTS_MOD == 1)
	// modulated lights
	for(int i=uLightRange.x; i<uLightRange.y; i+=4)
	{
		Lo += lightContributionPBR(i, N, V, albedo, F0, metallic, roughness);
	}
#endif

#if (DEF_DYNAMIC_LIGHTS_SUB == 1)
	// subtractive lights
	for(int i=uLightRange.y; i<uLightRange.z; i+=4)
	{
		Lo -= lightContributionPBR(i, N, V, albedo, F0, metallic, roughness);
	}
#endif

	// Pretend we sampled the sector light level from an irradiance map

	vec3 F = fresnelSchlickRoughness(clamp(dot(N, V), 0.0, 1.0), F0, roughness);

	vec3 kS = F;
	vec3 kD = 1.0 - kS;

	vec3 irradiance = ambientLight; // texture(irradianceMap, N).rgb
	vec3 diffuse = irradiance * albedo;

	vec3 ambient = (kD * diffuse) * ao;

	vec3 color = max(ambient + Lo, vec3(0.0));

	// sRGB gamma
	return pow(color, vec3(1.0 / 2.2));
}
