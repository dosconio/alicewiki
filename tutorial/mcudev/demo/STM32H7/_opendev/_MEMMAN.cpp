#include <c/mempool.h>


uni::Mempool mempool;


void* operator new(size_t size) { return mempool.allocate(size, 3, 0); }
void* operator new[](size_t size)            { return mempool.allocate(size, 3, 0); }
void  operator delete(void* p)        noexcept { if (p) mempool.deallocate(p, 0); }
void  operator delete[](void* p)      noexcept { if (p) mempool.deallocate(p, 0); }
void  operator delete(void* p, size_t) noexcept { if (p) mempool.deallocate(p, 0); }
void  operator delete[](void* p, size_t) noexcept { if (p) mempool.deallocate(p, 0); }

extern "C" void* malloc(size_t size)              { return mempool.allocate(size, 3, 0); }
extern "C" void  free(void* p)                    { if (p) mempool.deallocate(p, 0); }
extern "C" void* calloc(size_t n, size_t size)    { void* p = mempool.allocate(n * size, 3, 0); if (p) MemSet(p, 0, n * size); return p; }
extern "C" void* realloc(void* p, size_t size)    { return mempool.reallocate(p, 0, size, 3); }
