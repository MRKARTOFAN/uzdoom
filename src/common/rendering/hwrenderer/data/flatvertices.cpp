/*
** flatvertices.cpp
**
** Creates flat vertex data for hardware rendering.
**
**---------------------------------------------------------------------------
**
** Copyright 2010-2020 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** Code written prior to 2026 is also licensed under:
**
** SPDX-License-Identifier: BSD-3-Clause
**
**---------------------------------------------------------------------------
**
*/

#include "c_cvars.h"
#include "flatvertices.h"
#include "v_video.h"
#include "cmdlib.h"
#include "printf.h"
#include "hwrenderer/data/buffers.h"
#include "wallvertexcache.h"
#include "hw_clock.h"

//==========================================================================
//
//
//
//==========================================================================

FFlatVertexBuffer::FFlatVertexBuffer(int width, int height, int pipelineNbr):
	mPipelineNbr(pipelineNbr)
{
	vbo_shadowdata.Resize(NUM_RESERVED);

	// the first quad is reserved for handling coordinates through uniforms.
	vbo_shadowdata[0].Set(0, 0, 0, 0, 0);
	vbo_shadowdata[1].Set(1, 0, 0, 0, 0);
	vbo_shadowdata[2].Set(2, 0, 0, 0, 0);
	vbo_shadowdata[3].Set(3, 0, 0, 0, 0);

	// and the second one for the fullscreen quad used for blend overlays.
	vbo_shadowdata[4].Set(0, 0, 0, 0, 0);
	vbo_shadowdata[5].Set(0, (float)height, 0, 0, 1);
	vbo_shadowdata[6].Set((float)width, 0, 0, 1, 0);
	vbo_shadowdata[7].Set((float)width, (float)height, 0, 1, 1);

	// and this is for the postprocessing copy operation
	vbo_shadowdata[8].Set(-1.0f, -1.0f, 0, 0.0f, 0.0f);
	vbo_shadowdata[9].Set(3.0f, -1.0f, 0, 2.f, 0.0f);
	vbo_shadowdata[10].Set(-1.0f, 3.0f, 0, 0.0f, 2.f);
	vbo_shadowdata[11].Set(3.0f, 3.0f, 0, 2.f, 2.f); // Note: not used anymore

	// The next two are the stencil caps.
	vbo_shadowdata[12].Set(-32767.0f, 32767.0f, -32767.0f, 0, 0);
	vbo_shadowdata[13].Set(-32767.0f, 32767.0f, 32767.0f, 0, 0);
	vbo_shadowdata[14].Set(32767.0f, 32767.0f, 32767.0f, 0, 0);
	vbo_shadowdata[15].Set(32767.0f, 32767.0f, -32767.0f, 0, 0);

	vbo_shadowdata[16].Set(-32767.0f, -32767.0f, -32767.0f, 0, 0);
	vbo_shadowdata[17].Set(-32767.0f, -32767.0f, 32767.0f, 0, 0);
	vbo_shadowdata[18].Set(32767.0f, -32767.0f, 32767.0f, 0, 0);
	vbo_shadowdata[19].Set(32767.0f, -32767.0f, -32767.0f, 0, 0);

	mIndexBuffer = screen->CreateIndexBuffer();
	int data[4] = {};
	mIndexBuffer->SetData(4, data, BufferUsageType::Static); // On Vulkan this may not be empty, so set some dummy defaults to avoid crashes.


	for (int n = 0; n < mPipelineNbr; n++)
	{
		mVertexBufferPipeline[n] = screen->CreateVertexBuffer();

		unsigned int bytesize = BUFFER_SIZE * sizeof(FFlatVertex);
		mVertexBufferPipeline[n]->SetData(bytesize, nullptr, BufferUsageType::Persistent);

		static const FVertexBufferAttribute format[] = {
			{ 0, VATTR_VERTEX, VFmt_Float3, (int)myoffsetof(FFlatVertex, x) },
			{ 0, VATTR_TEXCOORD, VFmt_Float2, (int)myoffsetof(FFlatVertex, u) },
			{ 0, VATTR_LIGHTMAP, VFmt_Float3, (int)myoffsetof(FFlatVertex, lu) },
			{ 1, VATTR_SURFACEID, VFmt_Float, 0, sizeof(float) },
		};

		mVertexBufferPipeline[n]->SetFormat(screen->IsVulkan() ? 2 : 1, screen->IsVulkan() ? 4 : 3, sizeof(FFlatVertex), format);
	}

	mVertexBuffer = mVertexBufferPipeline[mPipelinePos];

	mIndex = mCurIndex = NUM_RESERVED;
	mNumReserved = NUM_RESERVED;
	Copy(0, NUM_RESERVED);
}

//==========================================================================
//
//
//
//==========================================================================

FFlatVertexBuffer::~FFlatVertexBuffer()
{
	for (int n = 0; n < mPipelineNbr; n++)
	{
		delete mVertexBufferPipeline[n];
	}

	delete mIndexBuffer;
	mIndexBuffer = nullptr;
	mVertexBuffer = nullptr;
}

//==========================================================================
//
//
//
//==========================================================================

void FFlatVertexBuffer::OutputResized(int width, int height)
{
	vbo_shadowdata[4].Set(0, 0, 0, 0, 0);
	vbo_shadowdata[5].Set(0, (float)height, 0, 0, 1);
	vbo_shadowdata[6].Set((float)width, 0, 0, 1, 0);
	vbo_shadowdata[7].Set((float)width, (float)height, 0, 1, 1);
	Copy(4, 4);
}

//==========================================================================
//
//
//
//==========================================================================

std::pair<FFlatVertex *, unsigned int> FFlatVertexBuffer::AllocVertices(unsigned int count)
{
	// [Karto] A run must not straddle the end of the capture scratch; past it, allocations fall back to the GPU buffer.
	if (Capturing && mCurIndex >= CaptureBase && mCurIndex - CaptureBase < CAPTURE_SCRATCH_SIZE && mCurIndex - CaptureBase + count > CAPTURE_SCRATCH_SIZE)
		mCurIndex = CaptureBase + CAPTURE_SCRATCH_SIZE;
	FFlatVertex *p = GetBuffer();
	auto index = mCurIndex.fetch_add(count);
	if (index + count >= BUFFER_SIZE_TO_USE)
	{
		// If a single scene needs 2'000'000 vertices there must be something very wrong.
		I_FatalError("Out of vertex memory. Tried to allocate more than %u vertices for a single frame", index + count);
	}
	return std::make_pair(p, index);
}

//==========================================================================
//
//
//
//==========================================================================

void FFlatVertexBuffer::Copy(int start, int count)
{
	IVertexBuffer* old = mVertexBuffer;

	for (int n = 0; n < mPipelineNbr; n++)
	{
		mVertexBuffer = mVertexBufferPipeline[n];
		Map();
		memcpy(GetBuffer(start), &vbo_shadowdata[0], count * sizeof(FFlatVertex));
		Unmap();
		mVertexBuffer->Upload(start * sizeof(FFlatVertex), count * sizeof(FFlatVertex));
	}

	mVertexBuffer = old;
}

IVertexBuffer *FFlatVertexBuffer::CacheWallVertices(const void *side, uint8_t part, const FFlatVertex *vertices, unsigned &index, bool verify)
{
	static_assert(sizeof(FFlatVertex) * 4 == sizeof(FWallVertexCache::Quad));
	if (!WallVertexCache)
	{
		WallVertexCache = std::make_unique<FWallVertexCache>();
		WallVertexBuffer.reset(screen->CreateVertexBuffer());
		WallVertexBuffer->SetData(size_t(WallVertexCache->GetCapacity()) * sizeof(FWallVertexCache::Quad), nullptr, BufferUsageType::Persistent);
		static const FVertexBufferAttribute format[] = {
			{ 0, VATTR_VERTEX, VFmt_Float3, (int)myoffsetof(FFlatVertex, x) },
			{ 0, VATTR_TEXCOORD, VFmt_Float2, (int)myoffsetof(FFlatVertex, u) },
			{ 0, VATTR_LIGHTMAP, VFmt_Float3, (int)myoffsetof(FFlatVertex, lu) },
		};
		WallVertexBuffer->SetFormat(1, 3, sizeof(FFlatVertex), format);
	}
	FWallVertexCache::Quad quad;
	memcpy(quad.data(), vertices, quad.size());
	auto match = WallVertexCache->Acquire(side, part, quad);
	if (match.State == FWallVertexCache::Status::Full)
	{
		if (SceneProfiling) ++GetSceneSurfaceProfile().CacheFull;
		return nullptr;
	}
	index = match.Index;
	auto destination = static_cast<FFlatVertex *>(WallVertexBuffer->Memory()) + index;
	if (match.State == FWallVertexCache::Status::Miss)
	{
		memcpy(destination, vertices, quad.size());
		if (SceneProfiling)
		{
			auto &profile = GetSceneSurfaceProfile();
			++profile.CacheMisses;
			profile.CacheWrittenBytes += quad.size();
		}
	}
	else
	{
		if (SceneProfiling) ++GetSceneSurfaceProfile().CacheHits;
		if (verify && memcmp(destination, vertices, quad.size()) != 0)
		{
			I_FatalError("Wall vertex cache differs from the original quad at index %u", index);
		}
		if (verify && SceneProfiling) ++GetSceneSurfaceProfile().CacheVerified;
	}
	return WallVertexBuffer.get();
}

void FFlatVertexBuffer::ClearWallVertexCache()
{
	PreparedWallVertexBuffer.reset();
	PreparedWallSlots = 0;
	WallVertexCache.reset();
	WallVertexBuffer.reset();
}

IVertexBuffer* FFlatVertexBuffer::RetainPreparedWallVertices(const FFlatVertex* vertices, unsigned& index)
{
	struct PreparedVertex
	{
		FFlatVertex Vertex;
		float SurfaceID;
	};
	if (PreparedWallSlots == 32768) return nullptr;
	if (!PreparedWallVertexBuffer)
	{
		PreparedWallVertexBuffer.reset(screen->CreateVertexBuffer());
		PreparedWallVertexBuffer->SetData(32768 * 4 * sizeof(PreparedVertex), nullptr, BufferUsageType::Persistent);
		const FVertexBufferAttribute format[] = {
			{ 0, VATTR_VERTEX, VFmt_Float3, (int)myoffsetof(FFlatVertex, x) },
			{ 0, VATTR_TEXCOORD, VFmt_Float2, (int)myoffsetof(FFlatVertex, u) },
			{ 0, VATTR_LIGHTMAP, VFmt_Float3, (int)myoffsetof(FFlatVertex, lu) },
			{ 0, VATTR_SURFACEID, VFmt_Float, (int)myoffsetof(PreparedVertex, SurfaceID) }
		};
		PreparedWallVertexBuffer->SetFormat(1, 4, sizeof(PreparedVertex), format);
	}
	index = PreparedWallSlots * 4;
	auto destination = static_cast<PreparedVertex*>(PreparedWallVertexBuffer->Memory()) + index;
	for (unsigned i = 0; i < 4; ++i) destination[i] = { vertices[i], float(PreparedWallSlots) };
	++PreparedWallSlots;
	if (SceneProfiling) GetSceneSurfaceProfile().PreparedUploadBytes += sizeof(PreparedVertex) * 4;
	return PreparedWallVertexBuffer.get();
}
