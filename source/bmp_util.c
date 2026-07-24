#include "bmp_util.h"

#include <stdlib.h>
#include <string.h>

static u16 read_le16(const u8 *p)
{
    return (u16)p[0] | ((u16)p[1] << 8);
}

static u32 read_le32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) |
           ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u16 next_power_of_two(u32 value)
{
    u32 result = 8;
    while (result < value && result < 1024) result <<= 1;
    return (u16)result;
}

bool bmp_to_texture(const void *data, size_t size, C3D_Tex *texture,
                    Tex3DS_SubTexture *subtexture)
{
    const u8 *bmp = data;
    u32 pixel_offset;
    s32 width;
    s32 signed_height;
    u32 height;
    u16 bits_per_pixel;
    u32 compression;
    size_t row_size;
    u16 texture_width;
    u16 texture_height;
    u32 *linear_pixels;
    size_t linear_size;
    u32 transfer_flags;

    if (!bmp || !texture || !subtexture || size < 54) return false;
    if (bmp[0] != 'B' || bmp[1] != 'M') return false;

    pixel_offset = read_le32(bmp + 10);
    width = (s32)read_le32(bmp + 18);
    signed_height = (s32)read_le32(bmp + 22);
    bits_per_pixel = read_le16(bmp + 28);
    compression = read_le32(bmp + 30);
    if (width <= 0 || signed_height == 0 || width > 512 ||
        signed_height > 512 || signed_height < -512 ||
        read_le16(bmp + 26) != 1 || compression != 0 ||
        (bits_per_pixel != 24 && bits_per_pixel != 32)) {
        return false;
    }

    height = signed_height < 0 ? (u32)-signed_height : (u32)signed_height;
    row_size = (((size_t)width * bits_per_pixel + 31) / 32) * 4;
    if (pixel_offset > size || height > (size - pixel_offset) / row_size) {
        return false;
    }

    texture_width = next_power_of_two((u32)width);
    texture_height = next_power_of_two(height);
    if (texture_width < width || texture_height < height) return false;

    linear_size = (size_t)texture_width * texture_height * sizeof(u32);
    linear_pixels = linearMemAlign(linear_size, 0x80);
    if (!linear_pixels) return false;
    memset(linear_pixels, 0, linear_size);

    for (u32 y = 0; y < height; ++y) {
        u32 source_y = signed_height > 0 ? height - 1 - y : y;
        u32 texture_y = texture_height - 1 - y;
        const u8 *source = bmp + pixel_offset + (size_t)source_y * row_size;
        u32 *destination = linear_pixels + (size_t)texture_y * texture_width;
        for (s32 x = 0; x < width; ++x) {
            const u8 *pixel = source + (size_t)x * (bits_per_pixel / 8);
            u8 alpha = bits_per_pixel == 32 ? pixel[3] : 255;
            destination[x] = C2D_Color32(pixel[2], pixel[1], pixel[0], alpha);
        }
    }

    if (!C3D_TexInit(texture, texture_width, texture_height, GPU_RGBA8)) {
        linearFree(linear_pixels);
        return false;
    }
    C3D_TexSetFilter(texture, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(texture, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

    GSPGPU_FlushDataCache(linear_pixels, linear_size);
    transfer_flags =
        GX_TRANSFER_FLIP_VERT(0) |
        GX_TRANSFER_OUT_TILED(1) |
        GX_TRANSFER_RAW_COPY(0) |
        GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
        GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
        GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);
    C3D_SyncDisplayTransfer(
        linear_pixels, GX_BUFFER_DIM(texture_width, texture_height),
        texture->data, GX_BUFFER_DIM(texture_width, texture_height),
        transfer_flags);
    linearFree(linear_pixels);

    subtexture->width = (u16)width;
    subtexture->height = (u16)height;
    subtexture->left = 0.0f;
    subtexture->right = (float)width / texture_width;
    subtexture->top = 1.0f;
    subtexture->bottom = 1.0f - (float)height / texture_height;
    return true;
}

