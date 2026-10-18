# Handoff 2026-10-19: textured effect sprites, DDS and BC6H, cube-map sky (branch `feat/textured-fx-hdr-env`, from b05ff7d)

Machine: i5-12400F, Windows 11, clang 23.1.3 (Ninja, Release, `build-win`), Quadro P600 over RDP (observations only). glslang 16.6.0, validation layer built earlier (`VK_ADD_LAYER_PATH`; sync validation via `VK_KHRONOS_VALIDATION_VALIDATE_SYNC`). Amendment: item 21 to 23 of `planning/amendments/2026-10-18-downstream-host-work.md`; docs in `docs/RENDERING_AND_ASSETS.md` 2.3.

## New API
- Effects (`include/sutekh/render.h`): `SuFxBatch`, `su_fx_batch`, `su_fx_sprite`, `su_fx_sprite_axis`, `su_fx_atlas_rect`, `SuTrailTex`, `su_trail_build_tex`; `SuRenderFrameDesc.fx_tex / fx_tex_vertex_count / fx_batches / fx_batch_count`; `SuRenderDesc.max_fx_tex_vertices / max_fx_batches`; stats `fx_tex_vertices`, `fx_batches_drawn`, `fx_batches_skipped`. Blends `SU_FX_BLEND_ADD` and `SU_FX_BLEND_PREMUL` (formulas in the header), wrap `SU_FX_WRAP_*` per axis.
- Sky: `SuRenderFrameDesc.sky_cube / sky_intensity / sky_rot (quaternion, zero = identity) / sky_lod`.
- Textures: formats `SU_TEX_BC2_UNORM/SRGB`, `BC6H_UF16/SF16`, `RGBA16F` (pack level sizes and names updated); `SuRenderTextureDesc.layers` (6 = cube with a cube view; `mip_data[mip * layers + layer]`); `SuRenderDeviceInfo.bc_textures`.
- DDS (`include/sutekh/dds.h`, in `su_asset`, headless): `su_dds_parse` (colour space explicit: `SU_DDS_CS_FILE/SRGB/LINEAR`), `su_dds_level_table`, `su_render_texture_create_dds`, `su_render_env_from_dds`, `su_bc6h_decode_block`, `su_dds_decode_rgba32f`, `su_half_to_float`, `su_float_to_half`. `SU_RENDER_API_VERSION` 3.

## Design notes
- Textured fx are a second vertex stream (`fx_tex`) with their own pipelines (`fx_tex_add.frag`, `fx_tex_pm.frag`; vertex shader shared with fx) and layout (frame + one sampler). Sampler sets come from a per-frame pool reset each frame (up to `max_fx_batches`), so no per-texture descriptor state. The untextured fx path, trails and glare are untouched.
- Cube sky: `sky_cube.frag` (own pipeline and layout: frame + cube set + 64 bytes of push constants) replaces the procedural sky draw only when a READY cube is given; the scene layout's frame set is bound again afterwards (the push ranges differ). Any 6-layer texture owns a cube sampler set now (`make_cube_set`, shared with env creation), so a raw BC6H cube can be the sky without prefiltering.
- Axis sprite: centre `pos` (camera-relative), `side = normalize(cross(axis, pos))`, U across, V along (tail 0, head 1); end-on it picks any perpendicular.
- BC6H decoder: table-driven per mode (14 field lists), written from the specification's layout; verified (below). The test file holds 14 block vectors (one per mode, unsigned and signed) whose output hashes came from the MIT/Unlicense `bcdec` reference decoder, run offline from a scratch directory (not committed, no code copied).
- CPU path for env: `su_render_env_from_dds` decodes the first power-of-two mip (8..1024) and reuses the existing CPU prefilter; GPU prefiltering was not added.

## Tests (this machine)
| Command | Result |
| --- | --- |
| `build-win\test_render_texfx.exe`, sync validation false and true | 75732 checks, 0 failures; validation 0 errors, 0 warnings both |
| `build-win\test_asset_dds.exe` (also built with MSVC `cl` /W4 /WX, `-DSUTEKH_NATIVE=OFF -DSUTEKH_RENDER=OFF`, same result) | 9790 checks, 0 failures; 30000 fuzzed headers, 8615 accepted, all inside their buffers |
| `test_render_util` (clang and MSVC), `test_render_portable` (MSVC) | 3767 checks / 84 checks, 0 failures |
| `test_render_bloom`, `_paint`, `_offscreen`, `_assets` (both sync settings), `test_asset_pak`, `test_asset_bc` | 287 / 579 / 23 / 114 checks, unchanged, 0 failures; validation clean; goldens unchanged |
| scratch: my decoder vs `bcdec` on 4,000,000 random blocks (2M unsigned, 2M signed, all 14 modes and the reserved ones, mode-biased) | 0 mismatches |
| `python -I tools/compile_shaders.py --check --compiler <glslang>` | ok (20 sources) |
| `ctest --test-dir build-win -j 8` | 108 tests 100% passed (path_fifo and containment skipped, POSIX only) |

`test_render_texfx` covers: quadrant texture at the right corners and its rotation by 90 degrees; per-sprite colour (0.5, HDR x4 clamps, alpha scales); atlas frames incl. modulo wrap; sRGB vs linear (same bytes: 128 vs 188 out); premultiplied vs additive over a blue clear, colour alpha on premultiplied; RGBA16F HDR texture feeding bloom; **axis billboards: 106 camera positions (3 axes x 3 elevations x 12 azimuths) lit area matches the projected quad (edge error at most 1 px, area within 6%)** and vertical-axis height/symmetry constant while orbiting; ribbon U by age and by distance with scroll offset and wrap, V across (flips with v0/v1), same geometry as `su_trail_build`; DDS BC1 (UNORM and sRGB, trailing metadata) and BC7 (UNORM and sRGB) drawn; BC6H cube (generated, 2 mips) as sky for all six axes, intensity 2 and 0.5, rotation by 0/90/180/270 degrees, lod, both as raw cube texture and as env from DDS; released cube falls back to the procedural sky byte for byte; **BC6H CPU decoder vs GPU sampling: 73728 channel values over 24 random textures, all modes, unsigned and signed, worst difference 1 level of 255**; skipped batches counted (stale handle, cube texture), invalid layer counts refused.

## Not done / next
- GPU prefilter of environments; CPU decoders for BC1 to BC5 and BC7 (BC6H only, as asked); automatic CPU BC6H fallback inside `texture_create` when `bc_textures` is 0 (the host can call the decoder and upload RGBA16F; untested on a device without BCn because this one has it).
- Atlas frames bleed with bilinear filtering unless padded; no flipbook frame blending, soft-particle depth fade, or sorting of batches (draw order is the batch order).
- GCC `-pedantic` not run here (clang -Wall -Wextra -Werror -pedantic and MSVC /W4 /WX are clean); CI will tell.
- The demo (`examples/presentation`) does not use the new fx yet.
- `PACK_VALIDATION.json`: regenerated, no change.
