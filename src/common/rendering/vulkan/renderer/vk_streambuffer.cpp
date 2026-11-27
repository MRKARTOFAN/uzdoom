/*
** vk_streambuffer.cpp
**
** Vulkan backend
**
**---------------------------------------------------------------------------
**
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** Copyright 2016-2020 Magnus Norddahl
**
** SPDX-License-Identifier: Zlib
**
**---------------------------------------------------------------------------
**
*/

#include "vk_renderstate.h"
#include "vulkan/system/vk_renderdevice.h"
#include "zvulkan/vulkanbuilders.h"
#include "vulkan/system/vk_buffer.h"
#include "vulkan/renderer/vk_streambuffer.h"
#include "hw_clock.h"

VkStreamBufferWriter::VkStreamBufferWriter(VulkanRenderDevice* fb)
{
	mBuffer = fb->GetBufferManager()->StreamBuffer.get();
}

bool VkStreamBufferWriter::Write(const StreamData& data, bool reuse)
{
	if (SceneProfiling) ++SceneMainSurfaceProfile.UniformRequests;
	// [Karto] Reuse only an immutable live slot; a reset invalidates it before the stream memory can be overwritten.
	if (reuse && mLastDataValid && memcmp(&mLastData, &data, sizeof(data)) == 0)
	{
		if (SceneProfiling) ++SceneMainSurfaceProfile.UniformReuses;
		return true;
	}
	mDataIndex++;
	if (mDataIndex == MAX_STREAM_DATA)
	{
		mDataIndex = 0;
		mStreamDataOffset = mBuffer->NextStreamDataBlock();
		if (mStreamDataOffset == 0xffffffff)
			return false;
	}
	uint8_t* ptr = (uint8_t*)mBuffer->UniformBuffer->Memory();
	memcpy(ptr + mStreamDataOffset + sizeof(StreamData) * mDataIndex, &data, sizeof(StreamData));
	if (reuse) mLastData = data;
	mLastDataValid = reuse;
	if (SceneProfiling) ++SceneMainSurfaceProfile.UniformUploads;
	return true;
}

void VkStreamBufferWriter::Reset()
{
	mLastDataValid = false;
	mDataIndex = MAX_STREAM_DATA - 1;
	mStreamDataOffset = 0;
	mBuffer->Reset();
}

/////////////////////////////////////////////////////////////////////////////

VkMatrixBufferWriter::VkMatrixBufferWriter(VulkanRenderDevice* fb)
{
	mBuffer = fb->GetBufferManager()->MatrixBuffer.get();
	mIdentityMatrix.loadIdentity();
}

template<typename T>
static void BufferedSet(bool& modified, T& dst, const T& src)
{
	if (dst == src)
		return;
	dst = src;
	modified = true;
}

static void BufferedSet(bool& modified, VSMatrix& dst, const VSMatrix& src)
{
	if (memcmp(dst.get(), src.get(), sizeof(float) * 16) == 0)
		return;
	dst = src;
	modified = true;
}

bool VkMatrixBufferWriter::Write(const VSMatrix& modelMatrix, bool modelMatrixEnabled, const VSMatrix& textureMatrix, bool textureMatrixEnabled)
{
	bool modified = (mOffset == 0); // always modified first call

	if (modelMatrixEnabled)
	{
		BufferedSet(modified, mMatrices.ModelMatrix, modelMatrix);
		if (modified)
			mMatrices.NormalModelMatrix.computeNormalMatrix(modelMatrix);
	}
	else
	{
		BufferedSet(modified, mMatrices.ModelMatrix, mIdentityMatrix);
		BufferedSet(modified, mMatrices.NormalModelMatrix, mIdentityMatrix);
	}

	if (textureMatrixEnabled)
	{
		BufferedSet(modified, mMatrices.TextureMatrix, textureMatrix);
	}
	else
	{
		BufferedSet(modified, mMatrices.TextureMatrix, mIdentityMatrix);
	}

	if (modified)
	{
		mOffset = mBuffer->NextStreamDataBlock();
		if (mOffset == 0xffffffff)
			return false;

		uint8_t* ptr = (uint8_t*)mBuffer->UniformBuffer->Memory();
		memcpy(ptr + mOffset, &mMatrices, sizeof(MatricesUBO));
	}

	return true;
}

void VkMatrixBufferWriter::Reset()
{
	mOffset = 0;
	mBuffer->Reset();
}
