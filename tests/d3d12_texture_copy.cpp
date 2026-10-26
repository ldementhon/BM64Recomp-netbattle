// Run on Windows with a D3D12 adapter; no window or game ROM is required.
#include "plume_d3d12.h"
#include <cstdio>
#include <cstdlib>

static void check(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

int main() {
    using namespace plume;
    D3D12Interface renderInterface;
    check(renderInterface.isValid(), "D3D12 interface unavailable");
    auto device = renderInterface.createDevice("");
    check(device && static_cast<D3D12Device *>(device.get())->d3d, "D3D12 device unavailable");
    constexpr uint32_t width = 17, height = 3, rowWidth = 64;
    auto source = device->createTexture(RenderTextureDesc::DepthTarget(width, height, RenderFormat::D32_FLOAT));
    auto destination = device->createTexture(RenderTextureDesc::DepthTarget(width, height, RenderFormat::D32_FLOAT));
    auto readback = device->createBuffer(RenderBufferDesc::ReadbackBuffer(rowWidth * height * sizeof(float)));
    auto framebuffer = device->createFramebuffer(RenderFramebufferDesc(nullptr, 0, source.get()));
    auto queue = device->createCommandQueue(RenderCommandListType::DIRECT);
    auto commands = queue->createCommandList();
    auto fence = device->createCommandFence();
    commands->begin();
    commands->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(source.get(), RenderTextureLayout::DEPTH_WRITE));
    commands->setFramebuffer(framebuffer.get());
    commands->clearDepth(true, 0.25f);
    commands->setFramebuffer(nullptr);
    commands->barriers(RenderBarrierStage::COPY, {
        RenderTextureBarrier(source.get(), RenderTextureLayout::COPY_SOURCE),
        RenderTextureBarrier(destination.get(), RenderTextureLayout::COPY_DEST)
    });
    commands->copyTextureRegion(RenderTextureCopyLocation::Subresource(destination.get()), RenderTextureCopyLocation::Subresource(source.get()));
    commands->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(destination.get(), RenderTextureLayout::COPY_SOURCE));
    commands->barriers(RenderBarrierStage::COPY, RenderBufferBarrier(readback.get(), RenderBufferAccess::WRITE));
    commands->copyTextureRegion(
        RenderTextureCopyLocation::PlacedFootprint(readback.get(), RenderFormat::D32_FLOAT, width, height, 1, rowWidth),
        RenderTextureCopyLocation::Subresource(destination.get()));
    commands->end();
    const RenderCommandList *lists[] = {commands.get()};
    queue->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, fence.get());
    queue->waitForCommandFence(fence.get());
    auto *pixels = static_cast<const float *>(readback->map());
    check(pixels != nullptr, "readback mapping failed");
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            check(pixels[y * rowWidth + x] == 0.25f, "copied depth differs from clear value");
        }
    }
    readback->unmap();
    std::puts("PASS: D3D12 depth texture copy and padded buffer readback");
}
