//
// Makebsp can automatically generate light halos
// to use with autosprite2 shaders.
// This is an example of one.
//
// The generation of halos is enabled by defining
// a halo shader in the game profile or by adding
// the keyword "haloshader" to a light entity or
// the worldspawn.

halo
{
	qer_editorimage textures/halos/qer_halo_pants.tga
	qer_trans 0.25
	surfaceparm nomarks
	surfaceparm trans
	surfaceparm nonsolid
	surfaceparm nolightmap
	deformVertexes autosprite2
	nopicmip
	cull none
	
	{
		detail
		clampmap textures/halos/halo.tga
		blendFunc GL_SRC_ALPHA GL_ONE
		rgbGen vertex
		alphaGen wave distanceramp 0 1.0 80 400
	}
}
