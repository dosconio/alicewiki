#include <c/mempool.h>
#include <new>


uni::Mempool mempool;

// AKA the allocator the USB/xHCI code allocates through: on this board the host environment
// is the mempool. It is assigned, not defined: the library already defines the pointer.
namespace {
	struct HostenvAllocatorBinder {
		HostenvAllocatorBinder() { uni_hostenv_allocator = &mempool; }
	};
	HostenvAllocatorBinder hostenv_allocator_binder;
}


void* operator new(size_t size) { return mempool.allocate(size, 3, 0); }
void* operator new[](size_t size)            { return mempool.allocate(size, 3, 0); }
void  operator delete(void* p)        noexcept { if (p) mempool.deallocate(p, 0); }
void  operator delete[](void* p)      noexcept { if (p) mempool.deallocate(p, 0); }
void  operator delete(void* p, size_t) noexcept { if (p) mempool.deallocate(p, 0); }
void  operator delete[](void* p, size_t) noexcept { if (p) mempool.deallocate(p, 0); }

// std::align_val_t 使用字节数，Mempool 使用对齐指数；分配与释放统一走同一个池。
static void* AllocateAligned(size_t size, std::align_val_t alignment) {
	stduint bytes = (stduint)alignment;
	if (!bytes || (bytes & (bytes - 1))) return nullptr;
	stduint shift = 0;
	while (bytes > 1) {
		bytes >>= 1;
		++shift;
	}
	if (shift < 3) shift = 3;
	return mempool.allocate(size, shift, 0);
}
void* operator new(size_t size, std::align_val_t alignment) {
	return AllocateAligned(size, alignment);
}
void* operator new[](size_t size, std::align_val_t alignment) {
	return AllocateAligned(size, alignment);
}
void* operator new(size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
	return AllocateAligned(size, alignment);
}
void* operator new[](size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
	return AllocateAligned(size, alignment);
}
void operator delete(void* p, std::align_val_t) noexcept { if (p) mempool.deallocate(p, 0); }
void operator delete[](void* p, std::align_val_t) noexcept { if (p) mempool.deallocate(p, 0); }
void operator delete(void* p, size_t, std::align_val_t) noexcept { if (p) mempool.deallocate(p, 0); }
void operator delete[](void* p, size_t, std::align_val_t) noexcept { if (p) mempool.deallocate(p, 0); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { if (p) mempool.deallocate(p, 0); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { if (p) mempool.deallocate(p, 0); }

extern "C" void* malloc(size_t size)              { return mempool.allocate(size, 3, 0); }
extern "C" void  free(void* p)                    { if (p) mempool.deallocate(p, 0); }
extern "C" void* calloc(size_t n, size_t size)    { void* p = mempool.allocate(n * size, 3, 0); if (p) MemSet(p, 0, n * size); return p; }
extern "C" void* realloc(void* p, size_t size)    { return mempool.reallocate(p, 0, size, 3); }
