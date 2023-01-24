#pragma once

#include <library/cpp/actors/core/actor.h>

#include <ydb/core/base/pathid.h>
#include <ydb/core/scheme/scheme_tablecell.h>
#include <ydb/core/tablet_flat/flat_cxx_database.h>

#include <util/generic/hash.h>
#include <util/generic/maybe.h>

namespace NKikimr::NDataShard {

class TCdcStreamScanManager {
    struct TScanInfo {
        TRowVersion SnapshotVersion;
        ui64 TxId = 0;
        ui64 ScanId = 0;
        NActors::TActorId ActorId;
        TMaybe<TSerializedCellVec> LastKey;
    };

public:
    void Reset();
    bool Load(NIceDb::TNiceDb& db);
    void Add(NTable::TDatabase& db, const TPathId& tablePathId, const TPathId& streamPathId, const TRowVersion& snapshotVersion);
    void Forget(NTable::TDatabase& db, const TPathId& tablePathId, const TPathId& streamPathId);

    void Enqueue(const TPathId& streamPathId, ui64 txId, ui64 scanId);
    void Register(ui64 txId, const NActors::TActorId& actorId);

    void Complete(const TPathId& streamPathId);
    void Complete(ui64 txId);

    TScanInfo* Get(const TPathId& streamPathId);
    const TScanInfo* Get(const TPathId& streamPathId) const;

    bool Has(const TPathId& streamPathId) const;
    bool Has(ui64 txId) const;

    ui32 Size() const;

    void PersistAdd(NIceDb::TNiceDb& db,
        const TPathId& tablePathId, const TPathId& streamPathId, const TScanInfo& info);
    void PersistRemove(NIceDb::TNiceDb& db,
        const TPathId& tablePathId, const TPathId& streamPathId);
    void PersistLastKey(NIceDb::TNiceDb& db,
        const TPathId& tablePathId, const TPathId& streamPathId, const TSerializedCellVec& value);

private:
    THashMap<TPathId, TScanInfo> Scans;
    THashMap<ui64, TPathId> TxIdToPathId;
};

}
