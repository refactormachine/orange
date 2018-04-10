//
// Created by Ynon on 26/03/2018.
//

#include <gtest/gtest.h>
#include "../Kiwi.inl"
#include "../MockAllocator.h"

template <typename T>
class MockComparer {
public:
    bool operator()(const T& t1, const T& t2) const {
        return t1 < t2;
    }
};

using kiwipq_t = KiWiPQ<MockComparer<int>, int, MockAllocator>;

class QueueTest : public testing::Test {
public:

    QueueTest() : m_allocator(nullptr), m_pq(nullptr) {

    }

    virtual void SetUp() {
        m_allocator = new MockAllocator();
        m_pq = new kiwipq_t(*m_allocator, 0, 13371337);
    }

    virtual void TearDown() {
        delete m_allocator;
        delete m_pq;
    }

protected:
    kiwipq_t &getQueue() {
        return *m_pq;
    }

private:
    MockAllocator *m_allocator;
    kiwipq_t *m_pq;
};


TEST_F(QueueTest, TestOnePushOnePop) {
    const int num_to_push = 1;
    int num_to_pop = -1;
    auto &pq = getQueue();

    pq.push(num_to_push);
    pq.try_pop(num_to_pop);
    EXPECT_EQ(num_to_pop, num_to_push);
}

TEST_F(QueueTest, TestMultiPushAscendingOnePopOneChunk) {
    const int first_num_to_push = 10;
    auto &pq = getQueue();
    int num_to_pop = -1;

    for (int i = first_num_to_push; i < KIWI_CHUNK_SIZE - 10; i++) {
        pq.push(i);
    }

    pq.try_pop(num_to_pop);
    EXPECT_EQ(num_to_pop, first_num_to_push);
}

TEST_F(QueueTest, TestMultiPushDecendingOnePopOneChunk) {
    const int expected_pop = 10;
    auto &pq = getQueue();
    int num_to_pop = -1;

    for (int i = KIWI_CHUNK_SIZE - 10; i >= expected_pop; i--) {
        pq.push(i);
    }

    pq.try_pop(num_to_pop);
    EXPECT_EQ(num_to_pop, expected_pop);
}