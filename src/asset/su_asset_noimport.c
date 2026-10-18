/* Builds without the pinned cgltf/stb (tools/bootstrap_asset_deps.py): the importer entry points exist and say so. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include <stdio.h>

#include "sutekh/asset.h"

int su_asset_import_available(void) {
    return 0;
}

uint32_t su_asset_cook_gltf(const char *path, const SuAssetCookOptions *options, uint8_t **pak, size_t *pak_len,
                            SuAssetCookReport *report, char message[256]) {
    (void)path;
    (void)options;
    (void)report;
    if (pak != NULL) {
        *pak = NULL;
    }
    if (pak_len != NULL) {
        *pak_len = 0;
    }
    if (message != NULL) {
        (void)snprintf(message, 256, "built without the glTF importer (run tools/bootstrap_asset_deps.py)");
    }
    return SU_E_UNSUPPORTED_METHOD;
}
