/* Procedural content of the presentation demo (demo_content.c). */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#ifndef SUTEKH_DEMO_CONTENT_H
#define SUTEKH_DEMO_CONTENT_H

#include <stddef.h>
#include <stdint.h>

#include "sutekh/asset.h"

/* su_asset_id("demo/" + name) */
uint64_t demo_id(const char *name);
/* A pack with the models "model/ship", "model/gate", "model/station" and "model/rock" (two LODs), meshes under
 * "mesh/" and materials under "mat/"; malloc'd, NULL on failure. */
uint8_t *demo_build_pack(size_t *len);

#endif
