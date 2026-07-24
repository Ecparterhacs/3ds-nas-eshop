#pragma once

#include <3ds.h>
#include <citro2d.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * Decodes an uncompressed 24/32-bit BMP into a GPU texture suitable for C2D.
 * The caller owns the returned texture and must call C3D_TexDelete.
 */
bool bmp_to_texture(const void *data, size_t size, C3D_Tex *texture,
                    Tex3DS_SubTexture *subtexture);

