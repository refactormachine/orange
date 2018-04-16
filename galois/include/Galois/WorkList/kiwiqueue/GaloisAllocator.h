#ifndef __KIWI_GALOIS_ALLOCATOR_H__
#define __KIWI_GALOIS_ALLOCATOR_H__

#include "Allocator.h"

class GaloisAllocator : public Allocator {
public:
    GaloisAllocator() : term(Runtime::getSystemTermination()) {}

    inline void* allocate(unsigned int numOfBytes, unsigned int listIndex) {
        return reinterpret_cast<void*>(heaps[term.getEpoch() % 3].allocate(numOfBytes, listIndex));
    }

    inline void deallocate(void* ptr, unsigned int listIndex) {
        heaps[term.getEpoch() % 3].deallocate(ptr, listIndex);
    }

    inline void reclaim(void* ptr, unsigned int listIndex) {
        heaps[(term.getEpoch() + 2)% 3].deallocate(ptr, listIndex);
    }

private:
    // memory reclamation mechanism

    Runtime::MM::ListNodeHeap heaps[3];
    Runtime::TerminationDetection& term;
};

#endif //__KIWI_GALOIS_ALLOCATOR_H__
