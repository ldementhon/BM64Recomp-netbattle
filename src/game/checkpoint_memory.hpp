#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
#include <stdexcept>
#include <chrono>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

// Same-process images: all 512 MiB remain covered, including future writes to
// previously untouched pages. Mach shares pages until either mapping writes.
class CheckpointMemory {
    size_t size_ = 0;
    std::vector<uint8_t> copy_;
#ifdef __APPLE__
    mach_vm_address_t image_ = 0;
    static void checked(kern_return_t result) {
        if (result != KERN_SUCCESS) throw std::runtime_error(mach_error_string(result));
    }
#endif
public:
    mutable double clear_ms=0, remap_ms=0;
    CheckpointMemory() = default;
    CheckpointMemory(const CheckpointMemory&) = delete;
    CheckpointMemory& operator=(const CheckpointMemory&) = delete;
    ~CheckpointMemory() { clear(); }
    void clear() {
#ifdef __APPLE__
        if (image_) { mach_vm_deallocate(mach_task_self(), image_, size_); image_ = 0; }
#endif
        copy_.clear();
    }
    void capture(uint8_t* source, size_t size, bool full_copy = false) {
        auto start=std::chrono::steady_clock::now();
        clear(); size_ = size;
        auto cleared=std::chrono::steady_clock::now();
        clear_ms=std::chrono::duration<double,std::milli>(cleared-start).count(); remap_ms=0;
#ifdef __APPLE__
        if (!full_copy) {
            vm_prot_t current, maximum;
            checked(mach_vm_remap(mach_task_self(), &image_, size_, 0, VM_FLAGS_ANYWHERE,
                mach_task_self(), reinterpret_cast<mach_vm_address_t>(source), true,
                &current, &maximum, VM_INHERIT_NONE));
            remap_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-cleared).count();
            return;
        }
#endif
        copy_.assign(source, source + size_);
    }
    void restore(uint8_t* target) const {
        auto start=std::chrono::steady_clock::now(); clear_ms=remap_ms=0;
#ifdef __APPLE__
        if (image_) {
            auto address = reinterpret_cast<mach_vm_address_t>(target);
            vm_prot_t current, maximum;
            checked(mach_vm_remap(mach_task_self(), &address, size_, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
                mach_task_self(), image_, true, &current, &maximum, VM_INHERIT_NONE));
            remap_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            return;
        }
#endif
        std::memcpy(target, copy_.data(), size_);
    }
    size_t copied_bytes() const { return copy_.size(); }
};
