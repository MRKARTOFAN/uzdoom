/*
** bloomextract.fp
**
**
**
**---------------------------------------------------------------------------
**
** Copyright 2016 Magnus Norddahl
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;
layout(binding=0) uniform sampler2D SceneTexture;
layout(binding=1) uniform sampler2D ExposureTexture;

void main()
{
	float exposureAdjustment = texture(ExposureTexture, vec2(0.5)).x;
	vec4 color = texture(SceneTexture, Offset + TexCoord * Scale);
#ifdef LUMA_ALPHA
	FragColor = vec4(max((color.rgb + vec3(0.001)) * exposureAdjustment - 1, vec3(0)), max(max(color.r, color.g), color.b));
#else
	FragColor = max(vec4((color.rgb + vec3(0.001)) * exposureAdjustment - 1, 1), vec4(0));
#endif
}
