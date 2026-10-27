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
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <algorithm>
#include <bit>
#include <memory>
#include <new>
#endif

// Same-process images: all 512 MiB remain covered, including future writes to
// previously untouched pages. Mach shares pages until either mapping writes.
// Windows cannot share ordinary memory that way, so for a MEM_WRITE_WATCH source
// images share reference-counted copies of pages, made once per write.
class CheckpointMemory {
    size_t size_ = 0;
    std::vector<uint8_t> copy_;
#ifdef __APPLE__
    mach_vm_address_t image_ = 0;
    static void checked(kern_return_t result) {
        if (result != KERN_SUCCESS) throw std::runtime_error(mach_error_string(result));
    }
#endif
#ifdef _WIN32
    using Pages = std::vector<uint64_t>;
    // One stored page; its bytes follow the header.
    struct Block {
        uint32_t refs;
        uint8_t* data() { return reinterpret_cast<uint8_t*>(this) + 64; }
    };
    // Page index to block, allocated in leaves of 64 pages. A null block is a zero page.
    struct Table {
        static constexpr size_t leaf = 64;
        std::vector<std::unique_ptr<Block*[]>> leaves;
        Block* get(size_t page) const { auto& l = leaves[page / leaf]; return l ? l[page % leaf] : nullptr; }
        Block*& at(size_t page) {
            auto& l = leaves[page / leaf];
            if (!l) l.reset(new Block*[leaf]());
            return l[page % leaf];
        }
    };
    bool watched_ = false;
    Table blocks_;
    // Pages whose source contents may differ from this image.
    mutable Pages stale_;
    struct Watch {
        uint8_t* source = nullptr;
        size_t size = 0, page = 0, live = 0;
        Pages written; // Since allocation.
        // fresh: the source page still holds latest's bytes, so captures share that block.
        Pages fresh;
        Table latest;
        std::vector<void*> addresses;
        std::vector<CheckpointMemory*> images;
        std::vector<Block*> spare;
        static constexpr std::align_val_t alignment{64};
        Block* hold(Block* block) { if (block) ++block->refs; return block; }
        void drop(Block* block) {
            if (!block || --block->refs) return;
            --live;
            if (spare.size() < 1024) spare.push_back(block);
            else ::operator delete(block, alignment);
        }
        Block* copy(const uint8_t* bytes) {
            Block* block;
            if (spare.empty()) block = static_cast<Block*>(::operator new(64 + page, alignment));
            else { block = spare.back(); spare.pop_back(); }
            block->refs = 0; ++live;
            std::memcpy(block->data(), bytes, page);
            return block;
        }
        // The source page now holds block's bytes.
        void point(size_t index, Block* block) {
            if (latest.get(index) != block) {
                hold(block); drop(latest.get(index));
                latest.at(index) = block;
            }
            fresh[index / 64] |= 1ull << (index % 64);
        }
        // Pages written since the previous query; resets tracking for the whole region.
        ULONG_PTR query() {
            ULONG_PTR count = addresses.size(); ULONG granularity = 0;
            if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, source, size, addresses.data(), &count, &granularity) != 0)
                throw std::runtime_error("GetWriteWatch failed");
            return count;
        }
        size_t index(ULONG_PTR i) const { return size_t(static_cast<uint8_t*>(addresses[i]) - source) / page; }
        void mark(size_t index) {
            uint64_t bit = 1ull << (index % 64);
            written[index / 64] |= bit;
            if (fresh[index / 64] & bit) {
                fresh[index / 64] &= ~bit;
                if (auto* block = latest.get(index)) { drop(block); latest.at(index) = nullptr; }
            }
            for (auto* image : images) image->stale_[index / 64] |= bit;
        }
        void poll() { for (ULONG_PTR i = 0, count = query(); i < count; ++i) mark(index(i)); }
    };
    // Never destroyed, so images released during exit can still unregister.
    static Watch& watch() { static Watch* watch = new Watch; return *watch; }
    // Each query resets tracking for the whole region, so only one source is watched.
    static bool watched(uint8_t* source, size_t size) {
        auto& w = watch();
        if (w.source) return w.source == source && w.size == size;
        SYSTEM_INFO info; GetSystemInfo(&info);
        size_t page = info.dwPageSize;
        if (!size || size % page || page < 64) return false;
        std::vector<void*> addresses(size / page);
        ULONG_PTR count = addresses.size(); ULONG granularity = 0;
        // This fails unless the allocation requested MEM_WRITE_WATCH.
        if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, source, size, addresses.data(), &count, &granularity) != 0) return false;
        if (granularity != page) throw std::runtime_error("unexpected write-watch granularity");
        const size_t pages = size / page;
        w.source = source; w.size = size; w.page = page;
        w.written.assign((pages + 63) / 64, 0); w.fresh.assign(w.written.size(), 0);
        w.latest.leaves.resize((pages + Table::leaf - 1) / Table::leaf);
        w.addresses = std::move(addresses);
        for (ULONG_PTR i = 0; i < count; ++i) { size_t index = w.index(i); w.written[index / 64] |= 1ull << (index % 64); }
        return true;
    }
    static bool test(const Pages& bits, size_t index) { return bits[index / 64] >> (index % 64) & 1; }
    static bool zero(const uint8_t* bytes, size_t size) {
        uint64_t any = 0, word;
        for (size_t i = 0; i < size; i += sizeof(word)) { std::memcpy(&word, bytes + i, sizeof(word)); any |= word; }
        return !any;
    }
    // Call f(index) for each marked page.
    template <class F> static void each(const Pages& bits, F&& f) {
        for (size_t i = 0; i < bits.size(); ++i)
            for (uint64_t word = bits[i]; word; word &= word - 1) f(i * 64 + std::countr_zero(word));
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
#ifdef _WIN32
        if (watched_) {
            auto& w = watch();
            w.images.erase(std::find(w.images.begin(), w.images.end(), this));
            for (auto& leaf : blocks_.leaves)
                if (leaf) for (size_t i = 0; i < Table::leaf; ++i) w.drop(leaf[i]);
            blocks_.leaves.clear(); stale_.clear(); watched_ = false;
        }
#endif
        copy_.clear();
    }
    void capture(uint8_t* source, size_t size, bool full_copy = false) {
        auto start=std::chrono::steady_clock::now();
#ifdef _WIN32
        if (!full_copy && watched(source, size)) {
            auto& w = watch();
            if (!watched_) {
                clear(); size_ = size; watched_ = true;
                w.poll();
                stale_ = w.written; blocks_.leaves.resize(w.latest.leaves.size());
                w.images.push_back(this);
            } else w.poll();
            auto polled=std::chrono::steady_clock::now();
            clear_ms=std::chrono::duration<double,std::milli>(polled-start).count();
            each(stale_, [&](size_t index) {
                if (!test(w.fresh, index)) {
                    const uint8_t* bytes = source + index * w.page;
                    w.point(index, zero(bytes, w.page) ? nullptr : w.copy(bytes));
                }
                Block* next = w.latest.get(index);
                if (Block* previous = blocks_.get(index); previous != next) {
                    w.hold(next); w.drop(previous);
                    blocks_.at(index) = next;
                }
            });
            std::fill(stale_.begin(), stale_.end(), 0);
            remap_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-polled).count();
            return;
        }
#endif
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
#ifdef _WIN32
        if (watched_) {
            auto& w = watch();
            if (target != w.source) throw std::runtime_error("watched checkpoint restored to another region");
            w.poll();
            each(stale_, [&](size_t index) {
                Block* block = blocks_.get(index);
                if (block) std::memcpy(target + index * w.page, block->data(), w.page);
                else std::memset(target + index * w.page, 0, w.page);
                w.point(index, block);
            });
            // Checkpoints run between game updates, so the copies above are the only
            // writes since the poll; consume them so the restored pages stay shared.
            for (ULONG_PTR i = 0, count = w.query(); i < count; ++i)
                if (size_t index = w.index(i); !test(stale_, index)) w.mark(index);
            // The restored pages may now differ from every other image.
            for (auto* image : w.images)
                if (image != this) for (size_t i = 0; i < stale_.size(); ++i) image->stale_[i] |= stale_[i];
            std::fill(stale_.begin(), stale_.end(), 0);
            remap_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            return;
        }
#endif
        std::memcpy(target, copy_.data(), size_);
    }
    size_t copied_bytes() const {
#ifdef _WIN32
        // Images share blocks, so each reports an equal share of the stored pages.
        if (watched_) return watch().live * watch().page / watch().images.size();
#endif
        return copy_.size();
    }
};
