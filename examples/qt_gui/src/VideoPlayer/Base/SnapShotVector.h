#pragma once

#include <algorithm>
#include <atomic>
#include <memory>
#include <vector>

// SnapshotVector 是一个面向读多写少场景的快照容器。
// 读线程通过 getSnapshot() 获取当前列表的只读快照，随后可在无锁条件下稳定遍历，
// 不会受到后续写操作的影响。写线程每次修改都会复制当前列表、完成变更后再原子发布。
//
// 使用约束：
// 1. 该类按“单线程写、多线程读”设计。
// 2. 如果多个线程可能同时写，必须由调用方在外部串行化写操作，例如通过 Qt 队列连接信号槽。
// 3. 该类保证的是列表结构的快照一致性，不保证 T 对象内部状态的线程安全。
//
// 适用场景：
// 1. 渲染对象列表、观察者列表、处理器链等需要稳定遍历的场景。
// 2. 写操作频率不高，且可以接受写时复制成本的场景。
template <typename T>
class SnapshotVector {
public:
    using ValueType = T;
    using Ptr = std::shared_ptr<T>;
    using List = std::vector<Ptr>;
    using Snapshot = std::shared_ptr<const List>;

public:
    SnapshotVector()
        : list_(emptySnapshot())
    {
    }

public:
    Snapshot getSnapshot() const
    {
        return std::atomic_load_explicit(
            &list_,
            std::memory_order_acquire);
    }

    size_t size() const
    {
        return load()->size();
    }

    bool empty() const
    {
        return load()->empty();
    }

    template <typename Predicate>
    bool containsIf(Predicate pred) const
    {
        auto current = load();
        return std::any_of(
            current->begin(),
            current->end(),
            [&](const Ptr &item) {
                return pred(item);
            });
    }

public:
    void add(const Ptr &value)
    {
        auto newList = cloneCurrent();

        newList->push_back(value);

        store(newList);
    }

    template <typename Predicate>
    bool addIf(const Ptr &value, Predicate pred)
    {
        auto current = load();
        if (std::any_of(
                current->begin(),
                current->end(),
                [&](const Ptr &item) {
                    return pred(item);
                })) {
            return false;
        }

        auto newList = std::make_shared<List>(*current);

        newList->push_back(value);

        store(newList);
        return true;
    }

    bool addIfNotExists(const Ptr &value)
    {
        return addIf(
            value,
            [&](const Ptr &item) {
                return item == value;
            });
    }

    void remove(const Ptr &value)
    {
        removeIf(
            [&](const Ptr &item) {
                return item == value;
            });
    }

    void clear()
    {
        if (load()->empty()) {
            return;
        }

        store(emptySnapshot());
    }

    template <typename Predicate>
    void removeIf(Predicate pred)
    {
        auto newList = cloneCurrent();
        auto newEnd = std::remove_if(
            newList->begin(),
            newList->end(),
            [&](const Ptr &item) {
                return pred(item);
            });
        if (newEnd == newList->end()) {
            return;
        }
        newList->erase(newEnd, newList->end());

        if (newList->empty()) {
            store(emptySnapshot());
            return;
        }

        store(newList);
    }

private:
    static Snapshot emptySnapshot()
    {
        static const Snapshot empty = std::make_shared<const List>();
        return empty;
    }

    std::shared_ptr<const List> load() const
    {
        return std::atomic_load_explicit(
            &list_,
            std::memory_order_acquire);
    }

    std::shared_ptr<List> cloneCurrent() const
    {
        auto current = load();

        return std::make_shared<List>(*current);
    }

    void store(const std::shared_ptr<List> &newList)
    {
        std::atomic_store_explicit(
            &list_,
            std::static_pointer_cast<const List>(newList),
            std::memory_order_release);
    }

    void store(const Snapshot &newSnapshot)
    {
        std::atomic_store_explicit(
            &list_,
            newSnapshot,
            std::memory_order_release);
    }

private:
    mutable std::shared_ptr<const List> list_;
};
