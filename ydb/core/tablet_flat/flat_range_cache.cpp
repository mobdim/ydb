#include "flat_range_cache.h"
#include "util_fmt_cell.h"

namespace NKikimr {
namespace NTable {

TKeyRangeCache::TKeyRangeCache(const TKeyNulls& nulls, const TKeyRangeCacheConfig& config)
    : Nulls(nulls)
    , Config(config)
    , Pool(new TSpecialMemoryPool())
    , Entries(TKeyRangeEntryCompare(Nulls.Types), TAllocator(&UsedHeapMemory))
{ }

TKeyRangeCache::~TKeyRangeCache()
{ }

TArrayRef<TCell> TKeyRangeCache::AllocateArrayCopy(TSpecialMemoryPool* pool, TArrayRef<const TCell> key) {
    if (!key) {
        return { };
    }

    TCell* rawPtr = static_cast<TCell*>(pool->Allocate(sizeof(TCell) * key.size()));
    TCell* nextCell = rawPtr;
    for (const TCell& cell : key) {
        new(nextCell++) TCell(cell);
    }

    return TArrayRef<TCell>(rawPtr, key.size());
}

TCell TKeyRangeCache::AllocateCellCopy(TSpecialMemoryPool* pool, const TCell& cell) {
    if (TCell::CanInline(cell.Size())) {
        TCell copy(cell.Data(), cell.Size());
        Y_VERIFY_DEBUG(copy.IsInline());
        return copy;
    }

    void* rawData = pool->Allocate(cell.Size());
    if (cell.Size()) {
        ::memcpy(rawData, cell.Data(), cell.Size());
    }
    TCell copy((const char*)rawData, cell.Size());
    Y_VERIFY_DEBUG(!copy.IsInline());
    return copy;
}

TArrayRef<TCell> TKeyRangeCache::AllocateKey(TArrayRef<const TCell> key) {
    if (!key) {
        return { };
    }

    ++Stats_.Allocations;

    auto copy = AllocateArrayCopy(Pool.Get(), key);
    for (TCell& cell : copy) {
        if (!cell.IsInline()) {
            cell = AllocateCellCopy(Pool.Get(), cell);
        }
    }
    return copy;
}

void TKeyRangeCache::DeallocateKey(TArrayRef<TCell> key) {
    if (!key) {
        return;
    }

    ++Stats_.Deallocations;

    size_t index = key.size() - 1;
    do {
        TCell cell = key[index];
        if (!cell.IsInline()) {
            Pool->Deallocate((void*)cell.Data(), cell.Size());
        }
    } while (index--);

    Pool->Deallocate((void*)key.data(), sizeof(TCell) * key.size());
}

void TKeyRangeCache::ExtendLeft(const_iterator it, TArrayRef<TCell> newLeft, bool leftInclusive, TRowVersion version) {
    Y_VERIFY_DEBUG(it != end());
    TKeyRangeEntryLRU& entry = const_cast<TKeyRangeEntryLRU&>(*it);
    DeallocateKey(entry.FromKey);
    entry.FromKey = newLeft;
    entry.FromInclusive = leftInclusive;
    entry.MaxVersion = ::Max(entry.MaxVersion, version);
}

void TKeyRangeCache::ExtendRight(const_iterator it, TArrayRef<TCell> newRight, bool rightInclusive, TRowVersion version) {
    Y_VERIFY_DEBUG(it != end());
    TKeyRangeEntryLRU& entry = const_cast<TKeyRangeEntryLRU&>(*it);
    DeallocateKey(entry.ToKey);
    entry.ToKey = newRight;
    entry.ToInclusive = rightInclusive;
    entry.MaxVersion = ::Max(entry.MaxVersion, version);
}

TKeyRangeCache::const_iterator TKeyRangeCache::Merge(const_iterator left, const_iterator right, TRowVersion version) {
    Y_VERIFY_DEBUG(left != end());
    Y_VERIFY_DEBUG(right != end());
    Y_VERIFY_DEBUG(left != right);
    TKeyRangeEntry rightCopy = *right;
    Entries.erase(right);
    DeallocateKey(rightCopy.FromKey);
    ExtendRight(left, rightCopy.ToKey, rightCopy.ToInclusive, ::Max(rightCopy.MaxVersion, version));
    return left;
}

TKeyRangeCache::const_iterator TKeyRangeCache::Add(TKeyRangeEntry entry) {
    auto res = Entries.emplace(entry.FromKey, entry.ToKey, entry.FromInclusive, entry.ToInclusive, entry.MaxVersion);
    Y_VERIFY_DEBUG(res.second);
    TKeyRangeEntryLRU& newEntry = const_cast<TKeyRangeEntryLRU&>(*res.first);
    Fresh.PushBack(&newEntry);
    return res.first;
}

void TKeyRangeCache::Invalidate(const_iterator it) {
    Y_VERIFY_DEBUG(it != end());
    TKeyRangeEntry entryCopy = *it;
    Entries.erase(it);
    DeallocateKey(entryCopy.FromKey);
    DeallocateKey(entryCopy.ToKey);
}

void TKeyRangeCache::Touch(const_iterator it) {
    Y_VERIFY_DEBUG(it != end());
    TKeyRangeEntryLRU& entry = const_cast<TKeyRangeEntryLRU&>(*it);
    Fresh.PushBack(&entry);
}

void TKeyRangeCache::EvictOld() {
    while (OverMemoryLimit() && LRU) {
        auto* entry = LRU.PopFront();
        auto it = Entries.find(*entry);
        Y_VERIFY_DEBUG(it != end());
        Invalidate(it);
        ++Stats_.Evictions;
    }
}

void TKeyRangeCache::CollectGarbage() {
    while (Fresh) {
        // The first entry in Fresh will become the last entry in LRU
        LRU.PushBack(Fresh.PopBack());
    }
    EvictOld();

    if (GetTotalAllocated() <= Config.MaxBytes / 2) {
        // Don't bother with garbage collection yet
        return;
    }

    if (Pool->TotalUsed() < Pool->TotalGarbage() / 2) {
        THolder<TSpecialMemoryPool> newPool = MakeHolder<TSpecialMemoryPool>(); 
        newPool->Reserve(Pool->TotalUsed());
        for (auto& constEntry : Entries) {
            auto& entry = const_cast<TKeyRangeEntryLRU&>(constEntry);
            if (entry.FromKey) {
                entry.FromKey = AllocateArrayCopy(newPool.Get(), entry.FromKey);
                for (auto& cell : entry.FromKey) {
                    if (!cell.IsInline()) {
                        cell = AllocateCellCopy(newPool.Get(), cell);
                    }
                }
            }
            if (entry.ToKey) {
                entry.ToKey = AllocateArrayCopy(newPool.Get(), entry.ToKey);
                for (auto& cell : entry.ToKey) {
                    if (!cell.IsInline()) {
                        cell = AllocateCellCopy(newPool.Get(), cell);
                    }
                }
            }
        }
        Pool = std::move(newPool);
        ++Stats_.GarbageCollections;
    }
}

void TKeyRangeCache::TDumpRanges::DumpTo(IOutputStream& out) const {
    out << "TKeyRangeCache{";
    bool first = true;
    for (const auto& entry : Self->Entries) {
        out << (first ? " " : ", ");
        out << (entry.FromInclusive ? "[" : "(");
        out << NFmt::TPrintableTypedCells(entry.FromKey, Self->Nulls.BasicTypes());
        out << ", ";
        out << NFmt::TPrintableTypedCells(entry.ToKey, Self->Nulls.BasicTypes());
        out << (entry.ToInclusive ? "]" : ")");
        first = false;
    }
    out << " }";
}

}
}

template<>
void Out<NKikimr::NTable::TKeyRangeCache::TDumpRanges>(
        IOutputStream& out,
        const NKikimr::NTable::TKeyRangeCache::TDumpRanges& value)
{
    value.DumpTo(out);
}
