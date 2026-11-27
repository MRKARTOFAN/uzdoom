/*
** bloomfused.fp
**
** [Karto] A bloom resample fused with the following blur.fp passes (r_pp_fused). Mode 0: resample plus both blurs; mode 1: resample
** plus horizontal blur. Taps, weights and order are blur.fp's; intermediates stay 32-bit instead of RGBA16F.
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
layout(binding=0) uniform sampler2D SourceTexture;

ivec2 TargetLast()
{
	return ivec2(TargetSize) - ivec2(1);
}

vec4 Resampled(ivec2 q)
{
	q = clamp(q, ivec2(0), TargetLast());
	return vec4(texture(SourceTexture, (vec2(q) + 0.5) / TargetSize).rgb, 0.0);
}

vec4 Horizontal(ivec2 q)
{
	q.y = clamp(q.y, 0, TargetLast().y);
	return
		Resampled(q) * SampleWeights0 +
		Resampled(q + ivec2( 1, 0)) * SampleWeights1 +
		Resampled(q + ivec2(-1, 0)) * SampleWeights2 +
		Resampled(q + ivec2( 2, 0)) * SampleWeights3 +
		Resampled(q + ivec2(-2, 0)) * SampleWeights4 +
		Resampled(q + ivec2( 3, 0)) * SampleWeights5 +
		Resampled(q + ivec2(-3, 0)) * SampleWeights6;
}

void main()
{
	ivec2 p = min(ivec2(TexCoord * TargetSize), TargetLast());
	if (Mode == 1)
	{
		FragColor = Horizontal(p);
		return;
	}
	FragColor =
		Horizontal(p) * SampleWeights0 +
		Horizontal(p + ivec2(0, 1)) * SampleWeights1 +
		Horizontal(p + ivec2(0,-1)) * SampleWeights2 +
		Horizontal(p + ivec2(0, 2)) * SampleWeights3 +
		Horizontal(p + ivec2(0,-2)) * SampleWeights4 +
		Horizontal(p + ivec2(0, 3)) * SampleWeights5 +
		Horizontal(p + ivec2(0,-3)) * SampleWeights6;
}
