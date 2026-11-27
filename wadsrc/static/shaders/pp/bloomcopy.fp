/*
** bloomcopy.fp
**
** [Karto] The scene copied into the post-process chain with the bloom added in the same pass (r_pp_bloomcopy), replacing the
** blit plus bloomcombine.fp's additive pass. The alpha is the scene's.
**
**---------------------------------------------------------------------------
**
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
layout(binding=1) uniform sampler2D Bloom;

void main()
{
	vec4 scene = texture(SceneTexture, TexCoord);
	FragColor = vec4(scene.rgb + texture(Bloom, TexCoord).rgb, scene.a);
}
