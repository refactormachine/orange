#ifndef __GALOIS_KIWI_H__
#define __GALOIS_KIWI_H__

#include <algorithm>
#include <set>

#include "utils.h"
#include "Allocator.h"

#define KIWI_CHUNK_SIZE 1024
#define ATOMIC_CAS_MB(p, o, n) __sync_bool_compare_and_swap(p, o, n)
#define ATOMIC_FETCH_AND_INC_FULL(p) __sync_fetch_and_add(p, 1)

template <class Comparer, typename K>
class KiwiChunk;

template <class Comparer, typename K>
class KiWiRebalancedObject;

enum ChunkStatus {
    INFANT_CHUNK = 0,
    NORMAL_CHUNK = 1,
    FROZEN_CHUNK = 2,
};

enum PPA_MASK {
    IDLE = (1 << 29) - 1,
    POP = 1 << 29,
    PUSH = 1 << 30,
    FROZEN = 1 << 31,
};

/**
 * A node in a list of chunks which are going to be rebalanced
 * used to synchronize the executions of rebalance
 * @tparam Comparer - Compares keys
 * @tparam K
 */
template <class Comparer, typename K>
class KiWiRebalancedObject {
   public:
    KiwiChunk<Comparer, K>* first;
    KiwiChunk<Comparer, K>* volatile next;

    void init(KiwiChunk<Comparer, K>* f, KiwiChunk<Comparer, K>* n) {
        first = f;
        next = n;
    }
};

/**
 * Pre-allocated memory containing a sorted concurrent list
 * of nodes in which the priorities and the values are stored
 * @tparam Comparer - Compares keys
 * @tparam K
 */
template <class Comparer, typename K>
class KiwiChunk {
    using rebalance_object_t = KiWiRebalancedObject<Comparer, K>;

   public:
    // dummy field which is used by the heap when the node is freed.
    // (without it, freeing a node would corrupt a field, possibly affecting
    // a concurrent traversal.)
    void* dummy;

    volatile uint32_t i;

    typedef struct element_t {
        K key;
        volatile int deleted;
        struct element_t* volatile next;
    } Element;

    /// Two fixed sentinels keep the invariant of an element always
    /// having both a previous and a next element
    Element begin_sentinel;
    Element k[KIWI_CHUNK_SIZE];
    Element end_sentinel;

    /// The minimal key in the list, except for the first non-sentinel chunk
    K min_key;

    /// A link to the next chunk
    KiwiChunk<Comparer, K>* volatile next;

    /// The status of the chunk
    volatile uint32_t status;

    /// The parent chunk during rebalacing (while this chunk is still INFANT)
    KiwiChunk<Comparer, K>* volatile parent;

    /// Ponits to the rebalanced object that in win in the consensus at the
    /// begging of rebalnced
    rebalance_object_t* volatile ro;

    /// An array of indices to push or pop from the chunk, its size is equal to
    /// the number of threads in the system (see new_chunk() in KiWiPQ)
    uint32_t ppa_len;
    uint32_t volatile ppa[0];

    static bool is_marked(Element* j) {
        return ((uintptr_t)j & (uintptr_t)0x01) != 0;
    }

    static Element* unset_mark(Element* j) {
        return reinterpret_cast<Element*>((uintptr_t)j & ~(uintptr_t)0x01);
    }

    static Element* set_mark(Element* j) {
        return reinterpret_cast<Element*>((uintptr_t)j | (uintptr_t)0x01);
    }

    void init() {
        // Used for debugging
        element_t* const UNINITIALIZED = reinterpret_cast<element_t* const>(0xdeadf00d);
        begin_sentinel.next = unset_mark(&end_sentinel);
        end_sentinel.next = nullptr;
        status = INFANT_CHUNK;
        ppa_len = getNumOfThreads();
        this->i = 0;

        // initialize ppa entries
        for (int j = 0; j < ppa_len; j++) {
            ppa[j] = IDLE;
        }

        // initialize next pointers of all elements
        for (int j = 0; j < KIWI_CHUNK_SIZE; j++) {
            k[j].next = UNINITIALIZED;
        }
        //TODO: change it back to the next element in the array, and set the last one to end_sentinel

    }

    /// Based on lock free list from "The art of multiprocessor programming"
    void find(const Comparer& compare,
              const K& key,
              Element*& out_prev,
              Element*& out_next) {
        Element* pred = nullptr;
        Element* curr = nullptr;
        Element* succ = nullptr;

    retry:
        while (true) {
            pred = &begin_sentinel;
            curr = unset_mark(pred->next);

            if (curr == &end_sentinel) {
                out_prev = pred;
                out_next = curr;
                return;
            }

            while (true) {
                succ = unset_mark(curr->next);
                while (is_marked(curr->next)) {
                    if (!ATOMIC_CAS_MB(&(pred->next), unset_mark(curr),
                                       unset_mark(succ))) {
                        goto retry;
                    }
                    curr = succ;
                    succ = unset_mark(curr->next);
                }

                if (succ == &end_sentinel) {
                    out_prev = curr;
                    out_next = &end_sentinel;
                    return;
                }

                if (!compare(curr->key, key)) {
                    out_prev = pred;
                    out_next = curr;
                    return;
                }

                pred = curr;
                curr = succ;
            }
        }
    }

    void push(const Comparer& compare, Element& element) {
        const K& key = element.key;
        while (true) {
            Element* left;
            Element* right;
            find(compare, key, left, right);

            element.next = right;
            if (ATOMIC_CAS_MB(&(left->next), unset_mark(right),
                              unset_mark(&element))) {
                return;
            }
        }
    }

    void freeze() {
        status = ChunkStatus::FROZEN_CHUNK;
        for (uint32_t j = 0; j < ppa_len; j++) {
            uint32_t ppa_j;
            do {
                ppa_j = ppa[j];
            } while (!(ppa_j & FROZEN) &&
                     !ATOMIC_CAS_MB(&ppa[j], ppa_j, ppa_j | FROZEN));
        }
    }

    bool publish_push(uint32_t index) {
        uint32_t thread_id = getThreadId();
        uint32_t ppa_t = ppa[thread_id];
        if (!(ppa_t & FROZEN)) {
            return ATOMIC_CAS_MB(&ppa[thread_id], ppa_t, PUSH | index);
        }
        return false;
    }

    bool publish_pop(uint32_t index) {
        uint32_t thread_id = getThreadId();
        uint32_t ppa_t = ppa[thread_id];
        if (!(ppa_t & FROZEN)) {
            return ATOMIC_CAS_MB(&ppa[thread_id], ppa_t, POP | index);
        }
        return false;
    }

    bool unpublish_index() {
        uint32_t thread_id = getThreadId();
        uint32_t ppa_t = ppa[thread_id];
        if (!(ppa_t & FROZEN)) {
            return ATOMIC_CAS_MB(&ppa[thread_id], ppa_t, IDLE);
        }
        return false;
    }

    // TODO: should be improved
    uint32_t get_keys(K (&arr)[KIWI_CHUNK_SIZE]) {
        if (status != FROZEN_CHUNK) {
            // invalid call
            return 0;
        }

        std::set<Element*> set;

        // add all list elements
        Element* element = begin_sentinel.next;
        while (element != &end_sentinel) {
            if (!element->deleted) {
                set.insert(element);
            }
            element = unset_mark(element->next);
        }

        // add pending push
        for (int j = 0; j < ppa_len; j++) {
            uint32_t ppa_j = ppa[j];
            if (ppa_j & PUSH) {
                uint32_t index = ppa_j & IDLE;
                if (index < KIWI_CHUNK_SIZE) {
                    set.insert(&k[index]);
                }
            }
        }

        // remove pending pop
        for (int j = 0; j < ppa_len; j++) {
            uint32_t ppa_j = ppa[j];
            if (ppa_j & POP) {
                uint32_t index = ppa_j & IDLE;
                if (index < KIWI_CHUNK_SIZE) {
                    set.erase(&k[index]);
                }
            }
        }

        uint32_t count = 0;
        for (auto& setElement : set) {
            arr[count++] = setElement->key;
        }
        return count;
    }

    bool try_pop(K& key) {
        if (status == FROZEN_CHUNK) {
            return false;
        }

        Element* currElem = &begin_sentinel;

        while (true) {
            // 1. find not deleted element
            do {
                currElem = unset_mark(currElem->next);
            } while ((currElem != &end_sentinel) && (currElem->deleted));

            if (currElem == &end_sentinel) {
                // end of the list
                return false;
            }

            // 2. publish pop
            if (!publish_pop((uint32_t)(currElem - k))) {
                // chunk is being rebalanced
                return false;
            }

            // 3. try to mark element as deleted
            if (!ATOMIC_CAS_MB(&(currElem->deleted), 0, 1)) {
                // someone else deleted the element before us, continue
                continue;
            }

            // 4. deleted - pop from elements list
            if (currElem == &end_sentinel) {
                return false;
            }

            key = currElem->key;
            Element* nextElem;
            do {
                nextElem = currElem->next;
                if (is_marked(nextElem)) {
                    break;
                }
            } while (!ATOMIC_CAS_MB(&currElem->next, nextElem, set_mark(nextElem)));

            return true;
        }
    }
};

#ifdef GALOIS
#include "GaloisAllocator.h"
#endif

template <typename Comparer, typename K, typename Allocator_t>
class KiWiPQ {
    using chunk_t = KiwiChunk<Comparer, K>;
    using rebalance_object_t = KiWiRebalancedObject<Comparer, K>;

   protected:
    // keys comparator
    Comparer compare;

    Allocator_t* allocator;

    // chunks
    chunk_t begin_sentinel;
    chunk_t end_sentinel;
    // LockFreeSkipListSet<Comparer, K, chunk_t*> index;

    static bool is_marked(chunk_t* j) {
        return ((uintptr_t)j & (uintptr_t)0x01) != 0;
    }

    static chunk_t* unset_mark(chunk_t* j) {
        return reinterpret_cast<chunk_t*>((uintptr_t)j & ~(uintptr_t)0x01);
    }

    static chunk_t* set_mark(chunk_t* j) {
        return reinterpret_cast<chunk_t*>((uintptr_t)j | (uintptr_t)0x01);
    }

    chunk_t* new_chunk() {
        // Second argument is an index of a freelist to use to reclaim
        chunk_t* chunk = reinterpret_cast<chunk_t*>(allocator->allocate(
            sizeof(chunk_t) + sizeof(uint32_t) * getNumOfThreads(), 0));
        chunk->init();
        return chunk;
    }

    void delete_chunk(chunk_t* chunk) { allocator->deallocate(chunk, 0); }

    rebalance_object_t* new_ro(chunk_t* f, chunk_t* n) {
        rebalance_object_t* ro = reinterpret_cast<rebalance_object_t*>(
            allocator->allocate(sizeof(rebalance_object_t), 1));
        ro->init(f, n);
        return ro;
    }

    void delete_ro(rebalance_object_t* ro) { allocator->deallocate(ro, 1); }

    bool check_rebalance(chunk_t* chunk, const K& key) {
        if (chunk->status == INFANT_CHUNK) {
            // TODO: it is clear why they think it is enough to normalize at
            // that point, but we don't have the required information (Cn, Cf,
            // last are all nullptr...) normalize(chunk->parent);
            ATOMIC_CAS_MB(&(chunk->status), INFANT_CHUNK, NORMAL_CHUNK);
            return true;
        }
        if (chunk->i >= KIWI_CHUNK_SIZE || chunk->status == FROZEN_CHUNK ||
            policy(chunk)) {
            rebalance(chunk);
            return true;
        }
        return false;
    }

    void rebalance(chunk_t* chunk) {
        // 1. engage
        rebalance_object_t* tmp = new_ro(chunk, unset_mark(chunk->next));
        if (!ATOMIC_CAS_MB(&(chunk->ro), nullptr, tmp)) {
            delete_ro(tmp);
        }
        rebalance_object_t* ro = chunk->ro;
        volatile chunk_t* last = chunk;
        while (true) {
            volatile chunk_t* next = unset_mark(ro->next);
            if (next == nullptr) {
                break;
            }
            if (policy(next)) {
                ATOMIC_CAS_MB(&next, nullptr, ro);

                if (next->ro == ro) {
                    ATOMIC_CAS_MB(&(ro->next), next, unset_mark(next->next));
                    last = next;
                } else {
                    ATOMIC_CAS_MB(&(ro->next), next, nullptr);
                }
            } else {
                ATOMIC_CAS_MB(&(ro->next), next, nullptr);
            }
        }

        // search for last concurrently engaged chunk
        while (unset_mark(last->next) != nullptr &&
               unset_mark(last->next)->ro == ro) {
            last = unset_mark(last->next);
        }

        // 2. freeze
        chunk_t* t = ro->first;
        do {
            t->freeze();
        } while ((t != last) && (t = unset_mark(t->next)));

        // 3. pick minimal version
        // ... we don't have scans so we don't need this part

        // 4. build:
        chunk_t* c = ro->first;
        chunk_t* Cn = new_chunk();
        chunk_t* Cf = Cn;

        do {
            K arr[KIWI_CHUNK_SIZE];
            uint32_t count = c->get_keys(arr);
            std::sort(arr, arr + count, compare);
            for (uint32_t j = 0; j < count; j++) {
                if (Cn->i > (KIWI_CHUNK_SIZE / 2)) {
                    // Cn is more than half full - create new chunk

                    Cn->k[Cn->i - 1].next =
                        &Cn->end_sentinel;   // close Cn inner list
                    Cn->next = new_chunk();  // create a new chunk and set
                                             // Cn->next points to it
                    Cn = Cn->next;           // Cn points to the new chunk
                    Cn->parent = chunk;      // set chunk as rebalance parent of the
                                             // new chunk
                    Cn->min_key = arr[j];    // set chunk min key - this value
                                             // won't be change

                    // TODO: delete it as soon as we use index again
                    Cn->status = NORMAL_CHUNK;
                }
                uint32_t i = Cn->i;
                Cn->k[i].key = arr[j];
                Cn->k[i].next = &(Cn->k[i + 1]);
                Cn->i++;
            }
        } while ((c != last) && (c = unset_mark(c->next)));

        // 5. replace
        do {
            Cn->next = last->next;
        } while (!is_marked(Cn->next) &&
                 !ATOMIC_CAS_MB(&(last->next), unset_mark(Cn->next),
                                set_mark(Cn->next)));

        do {
            chunk_t* pred = load_prev(chunk);
            if (ATOMIC_CAS_MB(&(pred->next), unset_mark(c), unset_mark(Cf))) {
                // success - normalize chunk and free old chunks
                // normalize(chunk);

                chunk_t* curr = ro->first;
                chunk_t* next;
                do {
                    next = unset_mark(curr->next);
                    delete_chunk(curr);
                } while ((curr != last) && (curr = next));

                return;
            }

            if (unset_mark(pred->next)->parent == chunk) {
                // someone else succeeded - delete the chunks we just created
                // and normalize
                chunk_t* curr = Cf;
                chunk_t* next;
                do {
                    next = unset_mark(curr->next);
                    delete_chunk(curr);
                } while ((curr != Cn) && (curr = next));

                // normalize(chunk);
                return;
            }

            // insertion failed, help predecessor and retry
            if (pred->ro != nullptr) {
                // TODO: ...
                rebalance(pred);
            }
        } while (true);
    }

    chunk_t* locate_target_chunk(const K& key) {
        chunk_t* c = &begin_sentinel;  // index.get(key);
        chunk_t* next = unset_mark(c->next);
        if (next == &end_sentinel) {
            // the chunk list is empty, we need to create one
            chunk_t* chunk = new_chunk();
            chunk->next = &end_sentinel;
            if (!ATOMIC_CAS_MB(&(begin_sentinel.next),
                               unset_mark(&end_sentinel), unset_mark(chunk))) {
                // add failed - delete chunk.
                delete_chunk(chunk);
            }
            return locate_target_chunk(key);
        }

        while (next != &end_sentinel && !compare(next->min_key, key)) {
            c = next;
            next = unset_mark(c->next);
        }

        if (c == &begin_sentinel) {
            // we never add any key to the sentinels
            return begin_sentinel.next;
        }

        return c;
    }

    chunk_t* load_prev(chunk_t* chunk) {
        // TODO: should use index instead of traversing the list
        chunk_t* prev = &begin_sentinel;
        chunk_t* curr = unset_mark(prev->next);
        while (curr != &end_sentinel && curr != chunk) {
            prev = curr;
            curr = unset_mark(prev->next);
        }

        if (curr == &end_sentinel) {
            return nullptr;
        }

        return prev;
    }

    void normalize(chunk_t* chunk) {
        // TODO
    }

    bool policy(volatile chunk_t* chunk) {
        // TODO ....
        return false;  // chunk->i > (KIWI_CHUNK_SIZE * 3 / 4) || chunk->i <
        // (KIWI_CHUNK_SIZE / 4);
    }

   public:

#ifdef GALOIS
    KiWiPQ() : allocator(new GaloisAllocator()), begin_sentinel(), end_sentinel() {
        begin_sentinel.next = &end_sentinel;
    }
#endif

    KiWiPQ(Allocator_t* alloc, const K& begin_key, const K& end_key)
        : allocator(alloc), begin_sentinel(), end_sentinel() {
        begin_sentinel.next = &end_sentinel;
        begin_sentinel.min_key = begin_key;
        end_sentinel.min_key = end_key;
    }

    bool push(const K& key) {
        chunk_t* chunk = locate_target_chunk(key);

        if (check_rebalance(chunk, key)) {
            return push(key);
        }

        // allocate cell in linked list
        uint32_t i = ATOMIC_FETCH_AND_INC_FULL(&chunk->i);

        if (i >= KIWI_CHUNK_SIZE) {
            // no more free space - trigger rebalance
            rebalance(chunk);
            return push(key);
        }

        chunk->k[i].key = key;

        if (!chunk->publish_push(i)) {
            // chunk is being rebalanced
            rebalance(chunk);
            return push(key);
        }

        chunk->push(compare, chunk->k[i]);
        chunk->unpublish_index();
        return true;
    }

    bool try_pop(K& key) {
        chunk_t* chunk = unset_mark(begin_sentinel.next);
        while (chunk != &end_sentinel) {
            if (chunk->try_pop(key)) {
                return true;
            }

            if (chunk->status == FROZEN) {
                // chunk is being rebalanced
                rebalance(chunk);
                return try_pop(key);
            }

            chunk = unset_mark(chunk->next);
        }
        return false;
    }
};

#endif  // __GALOIS_KIWI_H__