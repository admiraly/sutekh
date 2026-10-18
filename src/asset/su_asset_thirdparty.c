/* Implementation unit of the pinned single-file libraries fetched by tools/bootstrap_asset_deps.py (cgltf 1.15,
 * stb_image 2.30). Their code is not project-authored and is compiled without the project's warning flags. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_FAILURE_USERMSG
#include "stb_image.h"
