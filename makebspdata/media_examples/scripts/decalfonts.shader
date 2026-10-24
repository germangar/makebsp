
//--------------------------------------------------
//
//		FONT SHADER EXAMPLE 
//		for makebsp to genate decals directly 
//		from text with misc_text_decal
//
//		The example uses a quake 3 style shader.
//		Adapt it to your own game materials.
//--------------------------------------------------

textures/decals/fonts/impact
{
	qer_editorimage textures/decals/fonts/impact
	qer_trans 0.5
	q3map_vertexcolor 255 255 255
	surfaceparm nomarks
	surfaceparm nodlight
	surfaceparm nonsolid
	surfaceparm trans
	polygonOffset
	nopicmip

	{
		detail
		map textures/decals/fonts/impact.tga
		rgbgen vertex
		alphagen vertex
		blendfunc blend
	}
}
