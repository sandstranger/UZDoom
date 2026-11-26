// Selaco's lightmap composite: the base lightmaps and the light groups (in their colors) drawn into the master lightmaps
vec4 ProcessTexel()
{
	return uObjectColor * getTexel(vTexCoord.st);
}
