/* Sutekh DDS texture loading (headless; no GPU needed).
 *
 * Parses a DirectDraw Surface file already in memory: 2D textures and cube maps, with the mip chain, in the block
 * formats BC1 to BC7 (including BC6H HDR), RGBA8 and RGBA16F. The parser does not copy: SuDdsImage points into the
 * caller's buffer, which has to outlive it (and the GPU upload, unless the upload is told to copy).
 *
 *   - Legacy FourCC files (DXT1, DXT3, DXT5, ATI1/BC4U, ATI2/BC5U) and "DX10" extended headers are read; cube maps need
 *     all six faces. Texture arrays (array size > 1), volumes, and formats outside the list are refused with a
 *     message.
 *   - Colour space is explicit. SU_DDS_CS_FILE follows the file (a DXGI *_SRGB format is sRGB; legacy files, which
 *     cannot say, are linear); SU_DDS_CS_SRGB and SU_DDS_CS_LINEAR override it for formats that have both variants
 *     (RGBA8, BC1, BC2, BC3, BC7). HDR and one/two channel formats are always linear; asking for sRGB there is refused.
 *   - Trailing bytes after the last pixel are ignored (some tools append metadata): they are counted in
 *     trailing_bytes, never read.
 *   - Faces are +X -X +Y -Y +Z -Z, each with its mips, the same face order as the environment cubes of sutekh/render.h.
 *
 * Also here: a CPU BC6H decoder (bit-exact to the D3D11 / OpenGL BPTC float decode: 16-bit half-float outputs) and
 * CPU decoders for the LDR block formats BC1..BC5 and BC7, for devices that cannot sample BCn, for building
 * environment cubes, and for tests. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#ifndef SUTEKH_DDS_H
#define SUTEKH_DDS_H

#include <stddef.h>
#include <stdint.h>

#include "sutekh/asset.h"
#include "sutekh/status.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SU_DDS_CS_FILE 0u
#define SU_DDS_CS_SRGB 1u
#define SU_DDS_CS_LINEAR 2u

typedef struct SuDdsImage {
    uint32_t format;                   /* SU_TEX_* */
    uint32_t width, height;
    uint32_t mip_count;                /* 1..16 */
    uint32_t faces;                    /* 1 (2D) or 6 (cube) */
    uint32_t dxgi_format;              /* the file's DXGI_FORMAT, 0 for a legacy header */
    uint64_t pixel_bytes;              /* bytes of pixel data used, from the end of the headers */
    uint64_t trailing_bytes;           /* bytes after the pixel data that were ignored */
    const uint8_t *level[6][SU_ASSET_MAX_MIPS]; /* [face][mip], su_asset_texture_level_size bytes each */
} SuDdsImage;

/* Parses data. SU_OK, or SU_E_PARSE (not a DDS file, truncated pixel data, inconsistent header),
 * SU_E_UNSUPPORTED_METHOD (a valid file of a kind not handled), SU_E_INVALID_ARGUMENT (null pointers, or an sRGB
 * override of a format without one). message (optional, 160 bytes) says why. */
uint32_t su_dds_parse(const uint8_t *data, size_t size, uint32_t color_space, SuDdsImage *out, char message[160]);

/* Pointer table for su_render_texture_create: ptrs[mip * faces + face], img->mip_count * img->faces entries. */
void su_dds_level_table(const SuDdsImage *img, const uint8_t *ptrs[SU_ASSET_MAX_MIPS * 6u]);

/* ---- CPU decoding ---- */

/* Decodes one BC6H block (16 bytes) to 16 texels (row-major) of RGB half floats, as raw IEEE binary16 bits: out[texel
 * * 3 + channel]. is_signed selects BC6H_SF16 (sign-magnitude output, as the GPU produces it). Returns 0 for a reserved
 * mode (all outputs zero, as the specification asks), else 1. */
uint32_t su_bc6h_decode_block(const uint8_t block[16], int is_signed, uint16_t out[16 * 3]);
float su_half_to_float(uint16_t h);
uint16_t su_float_to_half(float f);

/* Decodes one 4x4 block of an LDR block format to 16 RGBA8 texels (row-major, out[texel * 4 + channel]) as stored,
 * without any sRGB conversion. BC1 (with its three-colour + transparent mode), BC2, BC3, BC4 (r, 0, 0, 255), BC5
 * (r, g, 0, 255) and BC7 (all eight modes; a reserved mode decodes to zeros, as the specification asks). BC1..BC3
 * interpolate with exact integer thirds (GPUs may round the thirds one or two codes differently); BC7 is bit-exact.
 * SU_E_UNSUPPORTED_METHOD for another format. */
uint32_t su_bc_decode_block(uint32_t format, const uint8_t *block, uint8_t out[64]);

/* Decodes a whole face level to linear RGBA32F (4 floats per texel, width * height * 4). Handles BC6H, RGBA16F, RGBA8
 * and BC1..BC5, BC7 (sRGB formats, the colour space chosen at su_dds_parse, are converted to linear; alpha 1 for BC6H).
 * SU_E_UNSUPPORTED_METHOD for other formats, SU_E_INVALID_ARGUMENT for a bad face or mip. */
uint32_t su_dds_decode_rgba32f(const SuDdsImage *img, uint32_t face, uint32_t mip, float *out);

#ifdef __cplusplus
}
#endif

#endif
