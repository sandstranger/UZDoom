/*
** main.fp
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

// Vertex outputs a variant actually reads. A tile-based GPU stores every vertex output in memory
// between the vertex and the fragment phase, used or not, so the ones a variant does not need are
// not declared (and not computed). Colors and normals are mediump: half the storage, and enough
// for values in [0, 1] / unit vectors. Texture coordinates, positions and distances keep the
// default (highp where the stage has it). User shaders get all of them.
#if defined(USER_SHADER) || (DEF_USE_GLOW_TOP_COLOR == 1) || (DEF_USE_GLOW_BOTTOM_COLOR == 1)
#define VARYING_GLOW 1
#else
#define VARYING_GLOW 0
#endif
#if defined(USER_SHADER) || (DEF_USE_OBJECT_COLOR_2 == 1)
#define VARYING_GRADIENT 1
#else
#define VARYING_GRADIENT 0
#endif
// PBR (GLSL ES 3.00): material_pbr.fp's ambient term reads the normal too, dynamic lights or not.
// Vanilla lighting: walls and planes have different colormap equations.
#if defined(USER_SHADER) || (defined(PBR) && __VERSION__ >= 300) || (DEF_VANILLA_LIGHTING == 1) || (DEF_DYNAMIC_LIGHTS_MOD == 1) || (DEF_DYNAMIC_LIGHTS_SUB == 1) || (DEF_DYNAMIC_LIGHTS_ADD == 1)
#define VARYING_NORMAL 1
#else
#define VARYING_NORMAL 0
#endif

varying vec4 vTexCoord;
varying mediump vec4 vColor;
varying vec4 pixelpos;
#if VARYING_GLOW
varying vec3 glowdist;
#endif
#if VARYING_GRADIENT
varying mediump vec3 gradientdist;
#endif
#if VARYING_NORMAL
varying mediump vec4 vWorldNormal;
#endif
#ifdef USER_SHADER
varying mediump vec4 vEyeNormal;
#endif



struct Material
{
	vec4 Base;
	vec4 Bright;
	vec4 Glow;
	vec3 Normal;
	vec3 Specular;
	float Glossiness;
	float SpecularLevel;
	// PBR materials (material_pbr.fp, GLSL ES 3.00 only)
	float Metallic;
	float Roughness;
	float AO;
};

vec4 Process(vec4 color);
vec4 ProcessTexel();
Material ProcessMaterial(); // note that this is deprecated. Use SetupMaterial!
void SetupMaterial(inout Material mat);
vec4 ProcessLight(Material mat, vec4 color);
vec3 ProcessMaterialLight(Material material, vec3 color);
vec2 GetTexCoord();

#ifdef USER_SHADER
// Mods' material shaders (GLSL ES 3.00 only) are written for the GL renderer's main.fp: what they use of
// it that the built-in shaders here have as compile-time flavour flags, with the same values as there.
uniform int uFogEnabled;	// 0 no fog, 1/2 black fog (gl_fogmode), -1/-2 colored fog, -3 2D 'fog' overlay

// These get Or'ed into uTextureMode because it only uses its 3 lowermost bits.
const int TEXF_Brightmap = 0x10000;
const int TEXF_Detailmap = 0x20000;
const int TEXF_Glowmap = 0x40000;
const int TEXF_ClampY = 0x80000;
#endif


//===========================================================================
//
// Color to grayscale
//
//===========================================================================

float grayscale(vec4 color)
{
	return dot(color.rgb, vec3(0.3, 0.56, 0.14));
}

//===========================================================================
//
// Desaturate a color
//
//===========================================================================

vec4 dodesaturate(vec4 texel, float factor)
{
	if (factor != 0.0)
	{
		float gray = grayscale(texel);
		return mix (texel, vec4(gray,gray,gray,texel.a), factor);
	}
	else
	{
		return texel;
	}
}

//===========================================================================
//
// Desaturate a color
//
//===========================================================================

vec4 desaturate(vec4 texel)
{
#if (DEF_DO_DESATURATE == 1)
	return dodesaturate(texel, uDesaturationFactor);
#else
	return texel;
#endif
}

//===========================================================================
//
// Texture tinting code originally from JFDuke but with a few more options
//
//===========================================================================

const int Tex_Blend_Alpha = 1;
const int Tex_Blend_Screen = 2;
const int Tex_Blend_Overlay = 3;
const int Tex_Blend_Hardlight = 4;

 vec4 ApplyTextureManipulation(vec4 texel)
 {
	// Step 1: desaturate according to the material's desaturation factor.
	texel = dodesaturate(texel, uTextureModulateColor.a);

	// Step 2: Invert if requested // TODO FIX
	//if ((blendflags & 8) != 0)
	//{
	//	texel.rgb = vec3(1.0 - texel.r, 1.0 - texel.g, 1.0 - texel.b);
	//}

	// Step 3: Apply additive color
	texel.rgb += uTextureAddColor.rgb;

	// Step 4: Colorization, including gradient if set.
	texel.rgb *= uTextureModulateColor.rgb;

	// Before applying the blend the value needs to be clamped to [0..1] range.
	texel.rgb = clamp(texel.rgb, 0.0, 1.0);

	// Step 5: Apply a blend. This may just be a translucent overlay or one of the blend modes present in current Build engines.
#if (DEF_BLEND_FLAGS != 0)

	vec3 tcol = texel.rgb * 255.0;	// * 255.0 to make it easier to reuse the integer math.
	vec4 tint = uTextureBlendColor * 255.0;

#if (DEF_BLEND_FLAGS == 1)

	tcol.b = tcol.b * (1.0 - uTextureBlendColor.a) + tint.b * uTextureBlendColor.a;
	tcol.g = tcol.g * (1.0 - uTextureBlendColor.a) + tint.g * uTextureBlendColor.a;
	tcol.r = tcol.r * (1.0 - uTextureBlendColor.a) + tint.r * uTextureBlendColor.a;

#elif (DEF_BLEND_FLAGS == 2) // Tex_Blend_Screen:
	tcol.b = 255.0 - (((255.0 - tcol.b) * (255.0 - tint.r)) / 256.0);
	tcol.g = 255.0 - (((255.0 - tcol.g) * (255.0 - tint.g)) / 256.0);
	tcol.r = 255.0 - (((255.0 - tcol.r) * (255.0 - tint.b)) / 256.0);

#elif (DEF_BLEND_FLAGS == 3) // Tex_Blend_Overlay:

	tcol.b = tcol.b < 128.0? (tcol.b * tint.b) / 128.0 : 255.0 - (((255.0 - tcol.b) * (255.0 - tint.b)) / 128.0);
	tcol.g = tcol.g < 128.0? (tcol.g * tint.g) / 128.0 : 255.0 - (((255.0 - tcol.g) * (255.0 - tint.g)) / 128.0);
	tcol.r = tcol.r < 128.0? (tcol.r * tint.r) / 128.0 : 255.0 - (((255.0 - tcol.r) * (255.0 - tint.r)) / 128.0);

#elif (DEF_BLEND_FLAGS == 4) // Tex_Blend_Hardlight:

	tcol.b = tint.b < 128.0 ? (tcol.b * tint.b) / 128.0 : 255.0 - (((255.0 - tcol.b) * (255.0 - tint.b)) / 128.0);
	tcol.g = tint.g < 128.0 ? (tcol.g * tint.g) / 128.0 : 255.0 - (((255.0 - tcol.g) * (255.0 - tint.g)) / 128.0);
	tcol.r = tint.r < 128.0 ? (tcol.r * tint.r) / 128.0 : 255.0 - (((255.0 - tcol.r) * (255.0 - tint.r)) / 128.0);

#endif

	texel.rgb = tcol / 255.0;

#endif

	return texel;
}

#ifdef USER_SHADER
//===========================================================================
//
// The GL renderer's forms for mods' material shaders: the blend flags
// passed in rather than chosen at compile time, and RGB to HSV
//
//===========================================================================

vec4 ApplyTextureManipulation(vec4 texel, int blendflags)
{
	// Step 1: desaturate according to the material's desaturation factor.
	texel = dodesaturate(texel, uTextureModulateColor.a);

	// Step 2: Invert if requested
	if ((blendflags & 8) != 0)
	{
		texel.rgb = vec3(1.0 - texel.r, 1.0 - texel.g, 1.0 - texel.b);
	}

	// Step 3: Apply additive color
	texel.rgb += uTextureAddColor.rgb;

	// Step 4: Colorization, including gradient if set.
	texel.rgb *= uTextureModulateColor.rgb;

	// Before applying the blend the value needs to be clamped to [0..1] range.
	texel.rgb = clamp(texel.rgb, 0.0, 1.0);

	// Step 5: Apply a blend. This may just be a translucent overlay or one of the blend modes present in current Build engines.
	if ((blendflags & 7) != 0)
	{
		vec3 tcol = texel.rgb * 255.0;	// * 255.0 to make it easier to reuse the integer math.
		vec4 tint = uTextureBlendColor * 255.0;

		switch (blendflags & 7)
		{
			default:
				tcol.b = tcol.b * (1.0 - uTextureBlendColor.a) + tint.b * uTextureBlendColor.a;
				tcol.g = tcol.g * (1.0 - uTextureBlendColor.a) + tint.g * uTextureBlendColor.a;
				tcol.r = tcol.r * (1.0 - uTextureBlendColor.a) + tint.r * uTextureBlendColor.a;
				break;

			// The following 3 are taken 1:1 from the Build engine
			case Tex_Blend_Screen:
				tcol.b = 255.0 - (((255.0 - tcol.b) * (255.0 - tint.r)) / 256.0);
				tcol.g = 255.0 - (((255.0 - tcol.g) * (255.0 - tint.g)) / 256.0);
				tcol.r = 255.0 - (((255.0 - tcol.r) * (255.0 - tint.b)) / 256.0);
				break;

			case Tex_Blend_Overlay:
				tcol.b = tcol.b < 128.0? (tcol.b * tint.b) / 128.0 : 255.0 - (((255.0 - tcol.b) * (255.0 - tint.b)) / 128.0);
				tcol.g = tcol.g < 128.0? (tcol.g * tint.g) / 128.0 : 255.0 - (((255.0 - tcol.g) * (255.0 - tint.g)) / 128.0);
				tcol.r = tcol.r < 128.0? (tcol.r * tint.r) / 128.0 : 255.0 - (((255.0 - tcol.r) * (255.0 - tint.r)) / 128.0);
				break;

			case Tex_Blend_Hardlight:
				tcol.b = tint.b < 128.0 ? (tcol.b * tint.b) / 128.0 : 255.0 - (((255.0 - tcol.b) * (255.0 - tint.b)) / 128.0);
				tcol.g = tint.g < 128.0 ? (tcol.g * tint.g) / 128.0 : 255.0 - (((255.0 - tcol.g) * (255.0 - tint.g)) / 128.0);
				tcol.r = tint.r < 128.0 ? (tcol.r * tint.r) / 128.0 : 255.0 - (((255.0 - tcol.r) * (255.0 - tint.r)) / 128.0);
				break;
		}
		texel.rgb = tcol / 255.0;
	}
	return texel;
}

vec3 rgb2hsv(vec3 c)
{
	vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
	vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
	vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));

	float d = q.x - min(q.w, q.y);
	float e = 1.0e-10;
	return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}
#endif

//===========================================================================
//
// This function is common for all (non-special-effect) fragment shaders
//
//===========================================================================

vec4 getTexel(vec2 st)
{
	vec4 texel = texture2D(tex, st);

#if (DEF_TEXTURE_MODE == 1)

	texel.rgb = vec3(1.0,1.0,1.0);

#elif (DEF_TEXTURE_MODE == 2)// TM_OPAQUE

	texel.a = 1.0;

#elif (DEF_TEXTURE_MODE == 3)// TM_INVERSE

	texel = vec4(1.0-texel.r, 1.0-texel.b, 1.0-texel.g, texel.a);

#elif (DEF_TEXTURE_MODE == 4)// TM_ALPHATEXTURE

	float gray = grayscale(texel);
	texel = vec4(1.0, 1.0, 1.0, gray*texel.a);

#elif (DEF_TEXTURE_MODE == 5)// TM_CLAMPY

	if (st.t < 0.0 || st.t > 1.0)
	{
		texel.a = 0.0;
	}

#elif (DEF_TEXTURE_MODE == 6)// TM_OPAQUEINVERSE

	texel = vec4(1.0-texel.r, 1.0-texel.b, 1.0-texel.g, 1.0);


#elif (DEF_TEXTURE_MODE == 7)//TM_FOGLAYER

	return texel;

#endif

	// Apply the texture modification colors.
#if (DEF_BLEND_FLAGS != 0)

	// only apply the texture manipulation if it contains something.
	texel = ApplyTextureManipulation(texel);

#endif

	// Apply the Doom64 style material colors on top of everything from the texture modification settings.
	// This may be a bit redundant in terms of features but the data comes from different sources so this is unavoidable.

	texel.rgb += uAddColor.rgb;

#if (DEF_USE_OBJECT_COLOR_2 == 1)
	texel *= mix(uObjectColor, uObjectColor2, gradientdist.z);
#else
	texel *= uObjectColor;
#endif

	// Last but not least apply the desaturation from the sector's light.
	return desaturate(texel);
}




//===========================================================================
//
// Doom software lighting equation
//
//===========================================================================

#define DOOMLIGHTFACTOR 232.0

float R_DoomLightingEquation_OLD(float light)
{
	// z is the depth in view space, positive going into the screen
	float z = pixelpos.w;


		/* L in the range 0 to 63 */
	float L = light * 63.0/31.0;

	float min_L = clamp(36.0/31.0 - L, 0.0, 1.0);

	// Fix objects getting totally black when close.
	if (z < 0.0001)
		z = 0.0001;

	float scale = 1.0 / z;
	float index = (59.0/31.0 - L) - (scale * DOOMLIGHTFACTOR/31.0 - DOOMLIGHTFACTOR/31.0);

	// Result is the normalized colormap index (0 bright .. 1 dark)
	return clamp(index, min_L, 1.0) / 32.0;
}


//===========================================================================
//
// zdoom colormap equation
//
//===========================================================================
float R_ZDoomColormap(float light, float z)
{
	float L = light * 255.0;
	float vis = min(uGlobVis / z, 24.0 / 32.0);
	float shade = 2.0 - (L + 12.0) / 128.0;
	float lightscale = shade - vis;
	return lightscale * 31.0;
}

#if (DEF_VANILLA_LIGHTING == 1)
//===========================================================================
//
// Vanilla Doom wall colormap equation
//
//===========================================================================
float R_WallColormap(float lightnum, float z, vec3 normal)
{
	// R_ScaleFromGlobalAngle calculation
	float projection = 160.0; // projection depends on SCREENBLOCKS!! 160 is the fullscreen value
	vec2 line_v1 = pixelpos.xz; // in vanilla this is the first curline vertex
	vec2 line_normal = normal.xz;
	float texscale = projection * clamp(dot(normalize(uCameraPos.xz - line_v1), line_normal), 0.0, 1.0) / z;

	float lightz = clamp(16.0 * texscale, 0.0, 47.0);

	// scalelight[lightnum][lightz] lookup
	float startmap = (15.0 - lightnum) * 4.0;
	return startmap - lightz * 0.5;
}

//===========================================================================
//
// Vanilla Doom plane colormap equation
//
//===========================================================================
float R_PlaneColormap(float lightnum, float z)
{
	float lightz = clamp(z / 16.0, 0.0, 127.0);

	// zlight[lightnum][lightz] lookup
	float startmap = (15.0 - lightnum) * 4.0;
	float scale = 160.0 / (lightz + 1.0);
	return startmap - scale * 0.5;
}
#endif

//===========================================================================
//
// Colormap of the software light modes: vanilla Doom's equations with light
// mode 16, ZDoom's otherwise
//
//===========================================================================
float R_DoomColormap(float light, float z)
{
#if (DEF_VANILLA_LIGHTING == 1)
	float lightnum = clamp(light * 15.0, 0.0, 15.0);

	if (dot(vWorldNormal.xyz, vWorldNormal.xyz) > 0.5)
	{
		vec3 normal = normalize(vWorldNormal.xyz);
		return mix(R_WallColormap(lightnum, z, normal), R_PlaneColormap(lightnum, z), abs(normal.y));
	}
	else // vWorldNormal is not set on sprites
	{
		return R_PlaneColormap(lightnum, z);
	}
#else
	return R_ZDoomColormap(light, z);
#endif
}

//===========================================================================
//
// Doom software lighting equation
//
//===========================================================================
float R_DoomLightingEquation(float light)
{
	// z is the depth in view space, positive going into the screen
	float z;

#if (DEF_FOG_RADIAL == 1)
	z = distance(pixelpos.xyz, uCameraPos.xyz);
#else
	z = pixelpos.w;
#endif

#if (DEF_BUILD_LIGHTING == 1) // gl_lightmode 5: Build software lighting emulation.
	// This is a lot more primitive than Doom's lighting...
	float numShades = float(uPalLightLevels);
	float curshade = (1.0 - light) * (numShades - 1.0);
	float visibility = max(uGlobVis * uLightFactor * abs(z), 0.0);
	float shade = clamp((curshade + visibility), 0.0, numShades - 1.0);
	return clamp(shade * uLightDist, 0.0, 1.0);
#endif

	float colormap = R_DoomColormap(light, z);

#if (DEF_BANDED_SW_LIGHTING == 1)
	colormap = floor(colormap) + 0.5;
#endif

	// Result is the normalized colormap index (0 bright .. 1 dark)
	return clamp(colormap, 0.0, 31.0) / 32.0;
}


float shadowAttenuation(vec4 lightpos, float lightcolorA)
{
	return 1.0;
}


float spotLightAttenuation(vec4 lightpos, vec3 spotdir, float lightCosInnerAngle, float lightCosOuterAngle)
{
	vec3 lightDirection = normalize(lightpos.xyz - pixelpos.xyz);
	float cosDir = dot(lightDirection, spotdir);
	return smoothstep(lightCosOuterAngle, lightCosInnerAngle, cosDir);
}

#if defined(NORMALMAP) && __VERSION__ >= 300 && VARYING_NORMAL
//===========================================================================
//
// Adjust normal vector according to the normal map (specular and PBR
// materials, GLSL ES 3.00), as the GL renderer does
//
//===========================================================================

mat3 cotangent_frame(vec3 n, vec3 p, vec2 uv)
{
	// get edge vectors of the pixel triangle
	vec3 dp1 = dFdx(p);
	vec3 dp2 = dFdy(p);
	vec2 duv1 = dFdx(uv);
	vec2 duv2 = dFdy(uv);

	// solve the linear system
	vec3 dp2perp = cross(n, dp2); // cross(dp2, n);
	vec3 dp1perp = cross(dp1, n); // cross(n, dp1);
	vec3 t = dp2perp * duv1.x + dp1perp * duv2.x;
	vec3 b = dp2perp * duv1.y + dp1perp * duv2.y;

	// construct a scale-invariant frame
	float invmax = inversesqrt(max(dot(t,t), dot(b,b)));
	return mat3(t * invmax, b * invmax, n);
}

vec3 ApplyNormalMap(vec2 texcoord)
{
	vec3 interpolatedNormal = normalize(vWorldNormal.xyz);

	vec3 map = texture2D(normaltexture, texcoord).xyz;
	map = map * 255./127. - 128./127.; // Math so "odd" because 0.5 cannot be precisely described in an unsigned format
	map.y = -map.y;	// green up

	mat3 tbn = cotangent_frame(interpolatedNormal, pixelpos.xyz, vTexCoord.st);
	return normalize(tbn * map);
}
#else
vec3 ApplyNormalMap(vec2 texcoord)
{
#if VARYING_NORMAL
	return normalize(vWorldNormal.xyz);
#else
	return vec3(0.0, 0.0, 1.0);	// only read by the dynamic lights, which this variant has none of
#endif
}
#endif

//===========================================================================
//
// Sets the common material properties.
//
//===========================================================================

void SetMaterialProps(inout Material material, vec2 texCoord)
{

#ifdef NPOT_EMULATION

#if (DEF_NPOT_EMULATION == 1)
		float period = floor(texCoord.t / uNpotEmulation.y);
		texCoord.s += uNpotEmulation.x * floor(mod(texCoord.t, uNpotEmulation.y));
		texCoord.t = period + mod(texCoord.t, uNpotEmulation.y);
#endif

#endif

	material.Base = getTexel(texCoord.st);
	material.Normal = ApplyNormalMap(texCoord.st);

	#if (DEF_TEXTURE_FLAGS & 0x1)
		material.Bright = texture2D(brighttexture, texCoord.st);
	#endif

	#if (DEF_TEXTURE_FLAGS & 0x2)
	{
		vec4 Detail = texture2D(detailtexture, texCoord.st * uDetailParms.xy) * uDetailParms.z;
		material.Base *= Detail;
	}
	#endif

	#if (DEF_TEXTURE_FLAGS & 0x4)
	{
		material.Glow = texture2D(glowtexture, texCoord.st);
	}
	#endif

}

//===========================================================================
//
// Calculate light
//
// It is important to note that the light color is not desaturated
// due to ZDoom's implementation weirdness. Everything that's added
// on top of it, e.g. dynamic lights and glows are, though, because
// the objects emitting these lights are also.
//
// This is making this a bit more complicated than it needs to
// because we can't just desaturate the final fragment color.
//
//===========================================================================

vec4 getLightColor(Material material, float fogdist, float fogfactor)
{
	vec4 color = vColor;

#if (DEF_USE_U_LIGHT_LEVEL == 1)
	{
		float newlightlevel = 1.0 - R_DoomLightingEquation(uLightLevel);
		color.rgb *= newlightlevel;
	}
#else
	{

		#if (DEF_FOG_ENABLED == 1) && (DEF_FOG_COLOURED == 0)
		{
			// brightening around the player for light mode 2
			if (fogdist < uLightDist)
			{
				color.rgb *= uLightFactor - (fogdist / uLightDist) * (uLightFactor - 1.0);
			}

			//
			// apply light diminishing through fog equation
			//
			color.rgb = mix(vec3(0.0, 0.0, 0.0), color.rgb, fogfactor);
		}
		#endif
	}
#endif


	//
	// handle glowing walls
	//
#if (DEF_USE_GLOW_TOP_COLOR)
	if (glowdist.x < uGlowTopColor.a)
	{
		color.rgb += desaturate(uGlowTopColor * (1.0 - glowdist.x / uGlowTopColor.a)).rgb;
	}
#endif


#if (DEF_USE_GLOW_BOTTOM_COLOR)
	if (glowdist.y < uGlowBottomColor.a)
	{
		color.rgb += desaturate(uGlowBottomColor * (1.0 - glowdist.y / uGlowBottomColor.a)).rgb;
	}
#endif

	color = min(color, 1.0);

	// these cannot be safely applied by the legacy format where the implementation cannot guarantee that the values are set.
#ifndef LEGACY_USER_SHADER
	//
	// apply glow
	//
	color.rgb = mix(color.rgb, material.Glow.rgb, material.Glow.a);

	//
	// apply brightmaps
	//
	color.rgb = min(color.rgb + material.Bright.rgb, 1.0);
#endif

	//
	// apply other light manipulation by custom shaders, default is a NOP.
	//
	color = ProcessLight(material, color);

	//
	// apply dynamic lights
	//
	return vec4(ProcessMaterialLight(material, color.rgb), material.Base.a * vColor.a);
}

//===========================================================================
//
// Applies colored fog
//
//===========================================================================

vec4 applyFog(vec4 frag, float fogfactor)
{
	return vec4(mix(uFogColor.rgb, frag.rgb, fogfactor), frag.a);
}

//===========================================================================
//
// Main shader routine
//
//===========================================================================

void main()
{

#ifndef LEGACY_USER_SHADER
	Material material;

	material.Base = vec4(0.0);
	material.Bright = vec4(0.0);
	material.Glow = vec4(0.0);
	material.Normal = vec3(0.0);
	material.Specular = vec3(0.0);
	material.Glossiness = 0.0;
	material.SpecularLevel = 0.0;
	material.Metallic = 0.0;
	material.Roughness = 0.0;
	material.AO = 0.0;
	SetupMaterial(material);
#else
	Material material = ProcessMaterial();
#endif
	vec4 frag = material.Base;

#ifndef NO_ALPHATEST
	if (frag.a <= uAlphaThreshold) discard;
#endif

#ifdef DITHERTRANS
	int index = (int(pixelpos.x) % 2) * 2 + int(pixelpos.y) % 2;
	if (index != 2) discard;
#endif

	#if (DEF_FOG_2D == 0)	// check for special 2D 'fog' mode.
	{
		float fogdist = 0.0;
		float fogfactor = 0.0;

		//
		// calculate fog factor
		//
		#if (DEF_FOG_ENABLED == 1)
		{
			#if (DEF_FOG_RADIAL == 0)
				fogdist = max(16.0, pixelpos.w);
			#else
				fogdist = max(16.0, distance(pixelpos.xyz, uCameraPos.xyz));
			#endif

			if (uThickFogDistance > 0.0)
			{
				if (fogdist > uThickFogDistance)
				{
					fogdist = fogdist + uThickFogMultiplier * (fogdist - uThickFogDistance);
				}
			}
			fogfactor = exp2 (uFogDensity * fogdist);
		}
		#endif

		#if (DEF_TEXTURE_MODE != 7)
		{
			frag = getLightColor(material, fogdist, fogfactor);

			//
			// colored fog
			//
			#if (DEF_FOG_ENABLED == 1) && (DEF_FOG_COLOURED == 1)
			{
				frag = applyFog(frag, fogfactor);
			}
			#endif
		}
		#else
		{
			frag = vec4(uFogColor.rgb, (1.0 - fogfactor) * frag.a * 0.75 * vColor.a);
		}
		#endif
	}
	#else
	{
		#if (DEF_TEXTURE_MODE == 7)
		{
			float gray = grayscale(frag);
			vec4 cm = (uObjectColor + gray * (uAddColor - uObjectColor)) * 2.0;
			frag = vec4(clamp(cm.rgb, 0.0, 1.0), frag.a);
		}
		#endif

		frag = frag * ProcessLight(material, vColor);
		frag.rgb = frag.rgb + uFogColor.rgb;
	}
	#endif  // (DEF_2D_FOG == 0)

#if (DEF_USE_COLOR_MAP == 1) // This mostly works but doesn't look great because of the blending.
	{
		frag.rgb = clamp(pow(frag.rgb, vec3(uFixedColormapStart.a)), 0.0, 1.0);
		if (uFixedColormapRange.a == 0.0)
		{
			float gray = (frag.r * 0.3 + frag.g * 0.56 + frag.b * 0.14);
			vec4 cm = uFixedColormapStart + gray * uFixedColormapRange;
			frag.rgb = clamp(cm.rgb, 0.0, 1.0);
		}
	}
#endif

	gl_FragColor = frag;

	//gl_FragColor = vec4(0.8, 0.2, 0.5, 1);

}
