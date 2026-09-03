#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>

#include "tinystore/arena.h"

namespace tinystore {

// ===========================================================================
// SkipList —— 概率平衡的有序链表（LevelDB 风格，Arena 支撑、无锁读）
// ===========================================================================
//
// 【为什么 MemTable 用跳表而不是红黑树 / std::map】
//   1. 并发读无需锁。跳表的多层指针天然适合"快照式"遍历：读者顺着指针走，
//      即便写者正在插入新节点，只要每个 next 指针的发布是原子的（release），
//      读者用 acquire 读就不会看到撕裂的指针。红黑树的旋转会同时改写多个
//      指针，几乎无法做到无锁并发读。
//   2. 插入无旋转、无再平衡。红黑树的旋转要改一堆指针、还要上锁；跳表插入
//      只是"在每一层把新节点链进去"，可以用一条 CAS-less 的 release 写完成。
//   3. 节点由 Arena 一次性分配、随 MemTable 整体释放（见 Arena 的设计说明），
//      写入热点上完全没有 per-node 的 malloc/free 开销。
//
// 【随机层高（为什么是概率平衡）】
// 每个新节点以 1/4 的概率再升一层，期望层高约 1/(1-1/4) ≈ 1.33，最大层数
// kMaxHeight=12。这样整张表的高度是 O(log_{4} n)，查找/插入/删除都是
// O(log n)，但完全不需要维护平衡信息。对随机键这不是问题；若键有序插入跳表
// 会退化，真实系统常在 key 上做 hash 或限制 max_height 来兜底（本项目用后者）。
//
// 【线程模型（重要）】
//   * 写：本实现假设"同时只有一个写者"（LevelDB 由 db_mutex 保证）。插入时对
//     next 指针的发布用 release，搜索路径上的读取用 relaxed 即可（写者自己看到
//     自己的写，无需同步）。
//   * 读：可多个读者并发，且与写者并发时安全，因为所有 next 读取走 atomic acquire。
//   * 因此本跳表本身不加锁；MemTable 在 W2 用一把 mutex 把"写 + 读"护住（最简单
//     且正确），W3 会放开为"只锁写、读走无锁路径"。
//
// 【内存布局：柔性数组 next_[1]】
// 节点的层数是运行时决定的，但 C++ 没有变长成员的标准写法。这里用
// `std::atomic<Node*> next_[1]` 作柔性数组，实际分配时多要 (height-1) 个指针
// 的空间。这样短节点（绝大多数 height<=3）只占很少内存，而不是为 kMaxHeight
// 层都预留指针。std::atomic 析构为空操作，满足 Arena"不调用析构"的约束。
template <typename Key, class Comparator>
class SkipList {
private:
  enum { kMaxHeight = 12 };

  // 柔性数组 next_[1] 是 C 的扩展（ISO C++ 不允许），GCC/Clang 作为扩展支持。
  // 用 pragma 抑制 -Wpedantic（及 Clang 的 -Wgnu-flexible-array-extensions）对该
  // 处的警告；它带来的内存收益（短节点只占少量指针）对跳表很关键。
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
#if defined(__clang__)
#pragma GCC diagnostic ignored "-Wgnu-flexible-array-extensions"
#endif
  struct Node {
    Key const key;

    explicit Node(const Key& k) : key(k) {}

    // 带内存序的读写（发布/消费路径）
    Node* Next(int n) { return next_[n].load(std::memory_order_acquire); }
    void SetNext(int n, Node* x) {
      next_[n].store(x, std::memory_order_release);
    }
    // 无屏障读写：仅在"只有当前写者自己看得到"的路径上用（如写者搜索插入点）
    Node* NoBarrier_Next(int n) {
      return next_[n].load(std::memory_order_relaxed);
    }
    void NoBarrier_SetNext(int n, Node* x) {
      next_[n].store(x, std::memory_order_relaxed);
    }

    std::atomic<Node*> next_[1];  // 柔性数组，实际层数由分配大小决定
  };
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

public:
  explicit SkipList(Comparator cmp, Arena* arena)
      : comparator_(std::move(cmp)),
        arena_(arena),
        head_(NewNode(Key(), kMaxHeight)),
        max_height_(kMaxHeight) {
    for (int i = 0; i < kMaxHeight; ++i) {
      head_->SetNext(i, nullptr);
    }
  }

  SkipList(const SkipList&) = delete;
  SkipList& operator=(const SkipList&) = delete;

  // 插入一个 key（调用方必须保证 key 在当前表中不存在，否则触发断言）。
  void Insert(const Key& key);

  // 是否存在等于 key 的节点
  bool Contains(const Key& key) const;

  // 当前已用层数
  int GetMaxHeight() const {
    return max_height_.load(std::memory_order_relaxed);
  }

  // -------------------------------------------------------------------------
  // 迭代器：对"单写者 + 多读者"安全（next 指针走 atomic acquire）
  // -------------------------------------------------------------------------
  class Iterator {
  public:
    explicit Iterator(const SkipList* list) : list_(list), node_(nullptr) {}

    bool Valid() const { return node_ != nullptr; }

    const Key& key() const {
      assert(Valid());
      return node_->key;
    }

    void Next() {
      assert(Valid());
      node_ = node_->Next(0);
    }

    void Prev() {
      assert(Valid());
      node_ = list_->FindLessThan(node_->key);
      if (node_ == list_->head_) node_ = nullptr;
    }

    void Seek(const Key& target) { node_ = list_->FindGreaterOrEqual(target, nullptr); }

    void SeekToFirst() { node_ = list_->head_->Next(0); }

    void SeekToLast() { node_ = list_->FindLast(); }

  private:
    const SkipList* list_;
    Node* node_;
  };

private:
  Node* NewNode(const Key& key, int height) {
    // 多分配 (height-1) 个指针宽度的空间给柔性数组
    const size_t sz = sizeof(Node) + sizeof(std::atomic<Node*>) * (height - 1);
    char* const mem = arena_->AllocateAligned(sz);
    return new (mem) Node(key);
  }

  // 1/4 概率升层；返回 [1, kMaxHeight]
  int RandomHeight() {
    int height = 1;
    while (height < kMaxHeight && ((rng_() & 3) == 0)) {
      ++height;
    }
    assert(height <= kMaxHeight);
    return height;
  }

  bool Equal(const Key& a, const Key& b) const {
    return comparator_(a, b) == 0;
  }

  // key 是否严格大于节点 n 的 key（n == nullptr 视为 +∞，即 key 不大）
  bool KeyIsAfterNode(const Key& key, Node* n) const {
    return (n != nullptr) && (comparator_(key, n->key) > 0);
  }

  // 找到第一个 key' 使得 comparator_(key', key) >= 0；
  // 若 prev != nullptr，则填回每一层的"前驱"节点（用于插入时链接）。
  Node* FindGreaterOrEqual(const Key& key, Node** prev) const {
    Node* x = head_;
    int level = GetMaxHeight() - 1;
    while (true) {
      Node* next = x->Next(level);
      if (KeyIsAfterNode(key, next)) {
        x = next;  // key 还在 next 之后，继续沿这一层前进
      } else {
        if (prev != nullptr) prev[level] = x;
        if (level == 0) return next;
        level--;
      }
    }
  }

  // 找到最后一个 key' 使得 comparator_(key', key) < 0
  Node* FindLessThan(const Key& key) const {
    Node* x = head_;
    int level = GetMaxHeight() - 1;
    while (true) {
      Node* next = x->Next(level);
      if (next == nullptr || comparator_(next->key, key) >= 0) {
        if (level == 0) return x;
        level--;
      } else {
        x = next;
      }
    }
  }

  // 返回表中最后一个节点（可能为空）
  Node* FindLast() const {
    Node* x = head_;
    int level = GetMaxHeight() - 1;
    while (true) {
      Node* next = x->Next(level);
      if (next == nullptr) {
        if (level == 0) return x;
        level--;
      } else {
        x = next;
      }
    }
  }

  // 简单的线性同余 PRNG（确定性，便于测试复现）
  struct Lcg {
    uint32_t state = 0x9e3779b9u;
    uint32_t operator()() {
      state = state * 1664525u + 1013904223u;
      return state;
    }
  };

  Comparator const comparator_;
  Arena* const arena_;
  Node* const head_;
  std::atomic<int> max_height_;
  Lcg rng_;
};

// ===========================================================================
// 模板成员实现
// ===========================================================================
template <typename Key, class Comparator>
void SkipList<Key, Comparator>::Insert(const Key& key) {
  Node* prev[kMaxHeight];
  Node* x = FindGreaterOrEqual(key, prev);

  // 不允许重复 key（MemTable 里每个 internal key 由唯一 seq 保证唯一）
  assert(x == nullptr || !Equal(key, x->key));

  const int height = RandomHeight();

  // 若新节点比当前已用层数更高，把多出来的那几层前驱设为 head_，并提升 max_height_。
  if (height > GetMaxHeight()) {
    for (int i = GetMaxHeight(); i < height; ++i) {
      prev[i] = head_;
    }
    max_height_.store(height, std::memory_order_relaxed);
  }

  x = NewNode(key, height);
  for (int i = 0; i < height; ++i) {
    x->NoBarrier_SetNext(i, prev[i]->NoBarrier_Next(i));
    prev[i]->SetNext(i, x);
  }
}

template <typename Key, class Comparator>
bool SkipList<Key, Comparator>::Contains(const Key& key) const {
  Node* x = FindGreaterOrEqual(key, nullptr);
  return (x != nullptr) && Equal(key, x->key);
}

}  // namespace tinystore
