// Run on Windows with a D3D12 adapter; no window or game ROM is required.
#include "plume_d3d12.h"
#include "plume_render_interface_builders.h"
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
    check(device != nullptr, "D3D12 device unavailable");
    auto *native = static_cast<D3D12Device *>(device.get());
    check(native->d3d != nullptr, "D3D12 device initialization failed");
    auto sampler = device->createSampler(RenderSamplerDesc{});
    const RenderSampler *samplers[] = {sampler.get(), sampler.get()};

    for (bool boundless : {false, true}) {
        RenderDescriptorSetBuilder builder;
        builder.begin();
        auto first = builder.addByteAddressBuffer(0, 2);
        builder.addImmutableSampler(2, sampler.get());
        builder.addImmutableSampler(3, samplers, 2);
        auto dynamic = builder.addSampler(5, 2);
        auto last = builder.addByteAddressBuffer(7, boundless ? 1 : 3);
        builder.end(boundless, boundless ? 5 : 0);
        D3D12DescriptorSet set(native, builder.descriptorSetDesc);
        check(set.descriptorHeapIndices[first] == 0 && set.descriptorHeapIndices[first + 1] == 1,
              "initial buffer array offsets");
        check(set.descriptorHeapIndices[dynamic] == 0 && set.descriptorHeapIndices[dynamic + 1] == 1,
              "dynamic samplers use a separate heap");
        check(set.descriptorHeapIndices[last] == 2, "static samplers must not shift subsequent buffers");
        check(set.viewAllocation.count == (boundless ? 7 : 5), "view heap excludes all samplers");
        check(set.samplerAllocation.count == 2, "sampler heap excludes static samplers");
        if (!boundless) {
            check(set.descriptorHeapIndices[last + 2] == 4, "trailing buffer array offsets");
        }
    }

    RenderDescriptorSetBuilder builder;
    builder.begin();
    builder.addImmutableSampler(0, samplers, 2);
    builder.end();
    D3D12DescriptorSet staticOnly(native, builder.descriptorSetDesc);
    check(staticOnly.viewAllocation.count == 0 && staticOnly.samplerAllocation.count == 0,
          "static-only set needs no descriptor heap allocation");
    std::puts("PASS: D3D12 static/dynamic sampler and buffer descriptor layouts");
}
