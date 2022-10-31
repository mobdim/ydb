#pragma once

#include "datashard.h"
#include "datashard_locks.h"
#include "datashard__engine_host.h"
#include "operation.h"

#include <ydb/core/tx/tx_processing.h>
#include <ydb/core/tablet_flat/flat_cxx_database.h>

#include <ydb/library/yql/public/issue/yql_issue.h>

namespace NKikimr {

class TBalanceCoverageBuilder;

namespace NDataShard {

static constexpr char MemoryLabelValidatedDataTx[] = "Datashard/TValidatedDataTx";
static constexpr char MemoryLabelActiveTransactionBody[] = "Datashard/TActiveTransaction/TxBody";

using NTabletFlatExecutor::TTransactionContext;
using NTabletFlatExecutor::TTableSnapshotContext;

class TDataShard;
class TSysLocks;
struct TReadSetKey;
class TActiveTransaction;

struct TScanState {
    TString LastKey;
    ui64 Bytes = 0;
    Ydb::StatusIds::StatusCode StatusCode = Ydb::StatusIds::STATUS_CODE_UNSPECIFIED;
    NYql::TIssues Issues;
};

struct TSchemaOperation {
    enum EType : ui32 {
        ETypeDrop = 0,
        ETypeCreate = 1,
        ETypeAlter = 2,
        ETypeBackup = 3,
        ETypeCopy = 4,
        EType_DEPRECATED_05 = 5,
        ETypeCreatePersistentSnapshot = 6,
        ETypeDropPersistentSnapshot = 7,
        ETypeInitiateBuildIndex = 8,
        ETypeFinalizeBuildIndex = 9,
        ETypeDropIndexNotice = 10,
        ETypeRestore = 11,
        ETypeMoveTable = 12,
        ETypeCreateCdcStream = 13,
        ETypeAlterCdcStream = 14,
        ETypeDropCdcStream = 15,
        ETypeMoveIndex = 16,

        ETypeUnknown = Max<ui32>()
    };

    ui64 TxId;
    EType Type;
    TActorId Source;
    ui64 TabletId;
    ui64 MinStep;
    ui64 MaxStep;
    ui64 PlanStep;
    bool ReadOnly;
    bool Done;

    bool Success;
    TString Error;
    ui64 BytesProcessed;
    ui64 RowsProcessed;

    TScanState ScanState;

    TSchemaOperation(ui64 txId, EType type, TActorId source, ui64 tabletId,
                    ui64 minStep, ui64 maxStep, ui64 planStep, bool readOnly,
                    bool success, const TString& error, ui64 bytes, ui64 rows)
        : TxId(txId)
        , Type(type)
        , Source(source)
        , TabletId(tabletId)
        , MinStep(minStep)
        , MaxStep(maxStep)
        , PlanStep(planStep)
        , ReadOnly(readOnly)
        , Done(false)
        , Success(success)
        , Error(error)
        , BytesProcessed(bytes)
        , RowsProcessed(rows)
    {}

    bool IsDrop() const { return Type == ETypeDrop; }
    bool IsCreate() const { return Type == ETypeCreate; }
    bool IsAlter() const { return Type == ETypeAlter; }
    bool IsBackup() const { return Type == ETypeBackup; }
    bool IsRestore() const { return Type == ETypeRestore; }
    bool IsCopy() const { return Type == ETypeCopy; }
    bool IsCreatePersistentSnapshot() const { return Type == ETypeCreatePersistentSnapshot; }
    bool IsDropPersistentSnapshot() const { return Type == ETypeDropPersistentSnapshot; }
    bool IsInitiateBuildIndex() const { return Type == ETypeInitiateBuildIndex; }
    bool IsFinalizeBuildIndex() const { return Type == ETypeFinalizeBuildIndex; }
    bool IsDropIndexNotice() const { return Type == ETypeDropIndexNotice; }
    bool IsMove() const { return Type == ETypeMoveTable; }
    bool IsMoveIndex() const { return Type == ETypeMoveIndex; }
    bool IsCreateCdcStream() const { return Type == ETypeCreateCdcStream; }
    bool IsAlterCdcStream() const { return Type == ETypeAlterCdcStream; }
    bool IsDropCdcStream() const { return Type == ETypeDropCdcStream; }

    bool IsReadOnly() const { return ReadOnly; }
};

/// @note This class incapsulates Engine stuff for minor needs. Do not return TEngine out of it.
class TValidatedDataTx : TNonCopyable {
public:
    using TPtr = std::shared_ptr<TValidatedDataTx>;

    TValidatedDataTx(TDataShard *self,
                     TTransactionContext &txc,
                     const TActorContext &ctx,
                     const TStepOrder &stepTxId,
                     TInstant receivedAt,
                     const TString &txBody,
                     bool usesMvccSnapshot);

    ~TValidatedDataTx();

    static constexpr ui64 MaxReorderTxKeys() { return 100; }

    NKikimrTxDataShard::TError::EKind Code() const { return ErrCode; }
    const TString GetErrors() const { return ErrStr; }

    TStepOrder StepTxId() const { return StepTxId_; }
    ui64 TxId() const { return StepTxId_.TxId; }
    ui64 TabletId() const { return TabletId_; }
    const TString& Body() const { return TxBody; }

    ui64 LockTxId() const { return Tx.GetLockTxId(); }
    ui32 LockNodeId() const { return Tx.GetLockNodeId(); }
    ui64 ProgramSize() const { return Tx.GetMiniKQL().size(); }
    bool Immediate() const { return Tx.GetImmediate(); }
    bool ReadOnly() const { return Tx.GetReadOnly(); }
    bool NeedDiagnostics() const { return Tx.GetNeedDiagnostics(); }
    bool CollectStats() const { return Tx.GetCollectStats(); }
    TInstant ReceivedAt() const { return ReceivedAt_; }
    TInstant Deadline() const { return Deadline_; }
    TMaybe<ui64> PerShardKeysSizeLimitBytes() const { return PerShardKeysSizeLimitBytes_; }

    bool Ready() const { return ErrCode == NKikimrTxDataShard::TError::OK; }
    bool RequirePrepare() const { return ErrCode == NKikimrTxDataShard::TError::SNAPSHOT_NOT_READY_YET; }
    bool RequireWrites() const { return TxInfo().HasWrites() || !Immediate(); }
    bool HasWrites() const { return TxInfo().HasWrites(); }
    bool HasLockedWrites() const { return HasWrites() && LockTxId(); }
    bool HasDynamicWrites() const { return TxInfo().DynKeysCount != 0; }

    // TODO: It's an expensive operation (Precharge() inside). We need avoid it.
    TEngineBay::TSizes CalcReadSizes(bool needsTotalKeysSize) const { return EngineBay.CalcSizes(needsTotalKeysSize); }

    ui64 GetMemoryAllocated() const {
        if (!IsKqpDataTx()) {
            const NMiniKQL::IEngineFlat * engine = EngineBay.GetEngine();
            if (engine) {
                return EngineBay.GetEngine()->GetMemoryAllocated();
            }
        }

        return 0;
    }

    NMiniKQL::IEngineFlat *GetEngine() { return EngineBay.GetEngine(); }
    void DestroyEngine() { EngineBay.DestroyEngine(); }
    const NMiniKQL::TEngineHostCounters& GetCounters() { return EngineBay.GetCounters(); }
    void ResetCounters() { EngineBay.ResetCounters(); }

    bool CanCancel();
    bool CheckCancelled();

    void SetWriteVersion(TRowVersion writeVersion) { EngineBay.SetWriteVersion(writeVersion); }
    void SetReadVersion(TRowVersion readVersion) { EngineBay.SetReadVersion(readVersion); }

    void CommitChanges(const TTableId& tableId, ui64 lockId, const TRowVersion& writeVersion, TTransactionContext& txc) {
        EngineBay.CommitChanges(tableId, lockId, writeVersion, txc);
    }

    TVector<NMiniKQL::IChangeCollector::TChange> GetCollectedChanges() const { return EngineBay.GetCollectedChanges(); }
    void ResetCollectedChanges() { EngineBay.ResetCollectedChanges(); }

    TActorId Source() const { return Source_; }
    void SetSource(const TActorId& actorId) { Source_ = actorId; }
    void SetStep(ui64 step) { StepTxId_.Step = step; }
    bool IsProposed() const { return Source_ != TActorId(); }

    bool IsTableRead() const { return Tx.HasReadTableTransaction(); }

    bool IsKqpTx() const { return Tx.HasKqpTransaction(); }

    bool IsKqpDataTx() const {
        return IsKqpTx() && Tx.GetKqpTransaction().GetType() == NKikimrTxDataShard::KQP_TX_TYPE_DATA;
    }

    bool IsKqpScanTx() const {
        return IsKqpTx() && Tx.GetKqpTransaction().GetType() == NKikimrTxDataShard::KQP_TX_TYPE_SCAN;
    }

    const NKikimrTxDataShard::TKqpTransaction &GetKqpTransaction() const { return Tx.GetKqpTransaction(); }
    const google::protobuf::RepeatedPtrField<NYql::NDqProto::TDqTask>& GetKqpTasks() const;
    NKqp::TKqpTasksRunner& GetKqpTasksRunner() { Y_VERIFY(IsKqpDataTx()); return EngineBay.GetKqpTasksRunner(Tx.GetKqpTransaction()); }
    NMiniKQL::TKqpDatashardComputeContext& GetKqpComputeCtx() { Y_VERIFY(IsKqpDataTx()); return EngineBay.GetKqpComputeCtx(); }

    bool HasStreamResponse() const { return Tx.GetStreamResponse(); }
    TActorId GetSink() const { return ActorIdFromProto(Tx.GetSink()); }
    const NKikimrTxDataShard::TReadTableTransaction &GetReadTableTransaction() const { return Tx.GetReadTableTransaction(); }

    ui32 ExtractKeys(bool allowErrors);
    bool ReValidateKeys();

    ui64 GetTxSize() const { return TxSize; }
    ui32 KeysCount() const { return TxInfo().ReadsCount + TxInfo().WritesCount; }

    void SetTxCacheUsage(ui64 val) { TxCacheUsage = val; }
    ui64 GetTxCacheUsage() const { return TxCacheUsage; }

    void ReleaseTxData();
    bool IsTxDataReleased() const { return IsReleased; }

    bool IsTxInfoLoaded() const { return TxInfo().Loaded; }

    bool IsTxReadOnly() const { return IsReadOnly; }

    bool HasOutReadsets() const { return TxInfo().HasOutReadsets; }
    bool HasInReadsets() const { return TxInfo().HasInReadsets; }

    const NMiniKQL::IEngineFlat::TValidationInfo& TxInfo() const { return EngineBay.TxInfo(); }

private:
    TStepOrder StepTxId_;
    ui64 TabletId_;
    TString TxBody;
    TActorId Source_;
    TEngineBay EngineBay;
    NKikimrTxDataShard::TDataTransaction Tx;
    NKikimrTxDataShard::TError::EKind ErrCode;
    TString ErrStr;
    ui64 TxSize;
    ui64 TxCacheUsage;
    bool IsReleased;
    TMaybe<ui64> PerShardKeysSizeLimitBytes_;
    bool IsReadOnly;
    bool AllowCancelROwithReadsets;
    bool Cancelled;
    const TInstant ReceivedAt_; // For local timeout tracking
    TInstant Deadline_;

    void ComputeTxSize();
    void ComputeDeadline();
};

enum class ERestoreDataStatus {
    Ok,
    Restart,
    Error,
};

///
class TDistributedEraseTx {
public:
    using TPtr = THolder<TDistributedEraseTx>;
    using TProto = NKikimrTxDataShard::TDistributedEraseTransaction;

public:
    bool TryParse(const TString& serialized);

    const TProto& GetBody() const { return Body; }

    bool HasDependents() const { return Body.DependentsSize(); }
    const auto& GetDependents() const { return Body.GetDependents(); }

    bool HasDependencies() const { return Body.DependenciesSize(); }
    const auto& GetDependencies() const { return Body.GetDependencies(); }

    const auto& GetRequest() const { return Body.GetEraseRowsRequest(); }

    const auto& GetIndexColumnIds() const { return Body.GetIndexColumnIds(); }
    const auto& GetIndexColumns() const { return Body.GetIndexColumns(); }

    void SetConfirmedRows(TDynBitMap&& confirmedRows) { ConfirmedRows = std::move(confirmedRows); }
    const TDynBitMap& GetConfirmedRows() const { return ConfirmedRows; }

private:
    TProto Body;
    TDynBitMap ConfirmedRows; // available on shard of main table

}; // TDistributedEraseTx

///
class TCommitWritesTx {
public:
    using TPtr = THolder<TCommitWritesTx>;
    using TProto = NKikimrTxDataShard::TCommitWritesTransaction;

public:
    bool TryParse(const TString& serialized);

    const TProto& GetBody() const { return Body; }

private:
    TProto Body;
};

///
class TActiveTransaction : public TOperation {
public:
    enum EArtifactFlags {
        OUT_RS_STORED = (1 << 0),
        LOCKS_STORED = (1 << 1),
    };

    using TPtr = TIntrusivePtr<TActiveTransaction>;

    explicit TActiveTransaction(const TBasicOpInfo &op)
        : TOperation(op)
        , ArtifactFlags(0)
        , TxCacheUsage(0)
        , ReleasedTxDataSize(0)
        , SchemeShardId(0)
        , SubDomainPathId(0)
        , SchemeTxType(TSchemaOperation::ETypeUnknown)
        , ScanSnapshotId(0)
        , ScanTask(0)
    {
        TrackMemory();
    }

    TActiveTransaction(const TBasicOpInfo &op,
                       TValidatedDataTx::TPtr savedTx);
    TActiveTransaction(TDataShard *self,
                       TTransactionContext &txc,
                       const TActorContext &ctx,
                       const TBasicOpInfo &op,
                       const TActorId &target,
                       const TString &txBody,
                       const TVector<TSysTables::TLocksTable::TLock> &locks,
                       ui64 artifactFlags);

    ~TActiveTransaction();

    void FillTxData(TValidatedDataTx::TPtr dataTx);
    void FillTxData(TDataShard *self,
                    TTransactionContext &txc,
                    const TActorContext &ctx,
                    const TActorId &target,
                    const TString &txBody,
                    const TVector<TSysTables::TLocksTable::TLock> &locks,
                    ui64 artifactFlags);

    const TString &GetTxBody() const { return TxBody; }
    void SetTxBody(const TString &txBody) {
        UntrackMemory();
        TxBody = txBody;
        TrackMemory();
    }
    void ClearTxBody() {
        UntrackMemory();
        TxBody.clear();
        TrackMemory();
    }

    ui64 GetSchemeShardId() const { return SchemeShardId; }
    void SetSchemeShardId(ui64 id) { SchemeShardId = id; }
    ui64 GetSubDomainPathId() const { return SubDomainPathId; }
    void SetSubDomainPathId(ui64 pathId) { SubDomainPathId = pathId; }

    const NKikimrSubDomains::TProcessingParams &GetProcessingParams() const
    {
        return ProcessingParams;
    }
    void SetProcessingParams(const NKikimrSubDomains::TProcessingParams &params)
    {
        ProcessingParams.CopyFrom(params);
    }

    void Deactivate() override {
        ClearSchemeTx();
        ClearTxBody();

        TOperation::Deactivate();
    }

    const TValidatedDataTx::TPtr& GetDataTx() const { return DataTx; }
    TValidatedDataTx::TPtr BuildDataTx(TDataShard *self,
                                       TTransactionContext &txc,
                                       const TActorContext &ctx);
    void ClearDataTx() { DataTx = nullptr; }

    const NKikimrTxDataShard::TFlatSchemeTransaction &GetSchemeTx() const
    {
        Y_VERIFY_S(SchemeTx, "No ptr");
        return *SchemeTx;
    }
    bool BuildSchemeTx();
    void ClearSchemeTx() { SchemeTx = nullptr; }
    TSchemaOperation::EType GetSchemeTxType() const { return SchemeTxType; }

    const NKikimrTxDataShard::TSnapshotTransaction& GetSnapshotTx() const {
        Y_VERIFY_DEBUG(SnapshotTx);
        return *SnapshotTx;
    }
    bool BuildSnapshotTx();
    void ClearSnapshotTx() { SnapshotTx = nullptr; }

    const TDistributedEraseTx::TPtr& GetDistributedEraseTx() const {
        Y_VERIFY_DEBUG(DistributedEraseTx);
        return DistributedEraseTx;
    }
    bool BuildDistributedEraseTx();
    void ClearDistributedEraseTx() { DistributedEraseTx = nullptr; }

    const TCommitWritesTx::TPtr& GetCommitWritesTx() const {
        Y_VERIFY_DEBUG(CommitWritesTx);
        return CommitWritesTx;
    }
    bool BuildCommitWritesTx();
    void ClearCommitWritesTx() { CommitWritesTx = nullptr; }

    // out-of-order stuff

    ui32 ExtractKeys() {
        if (DataTx && (DataTx->ProgramSize() || DataTx->IsKqpDataTx()))
            return DataTx->ExtractKeys(false);
        return 0;
    }

    bool ReValidateKeys() {
        if (DataTx && (DataTx->ProgramSize() || DataTx->IsKqpDataTx()))
            return DataTx->ReValidateKeys();
        return true;
    }

    void MarkAsUsingSnapshot() {
        SetUsingSnapshotFlag();
    }

    void SetTxCacheUsage(ui64 val) { TxCacheUsage = val; }
    ui64 GetTxCacheUsage() const { return TxCacheUsage; }

    ui64 GetReleasedTxDataSize() const { return ReleasedTxDataSize; }
    bool IsTxDataReleased() const { return ReleasedTxDataSize > 0; }

    void MarkOutRSStored()
    {
        ArtifactFlags |= OUT_RS_STORED;
    }

    bool IsOutRSStored()
    {
        return ArtifactFlags & OUT_RS_STORED;
    }

    void MarkLocksStored()
    {
        ArtifactFlags |= LOCKS_STORED;
    }

    bool IsLocksStored()
    {
        return ArtifactFlags & LOCKS_STORED;
    }

    void DbStoreLocksAccessLog(TDataShard * self,
                               TTransactionContext &txc,
                               const TActorContext &ctx);
    void DbStoreArtifactFlags(TDataShard * self,
                              TTransactionContext &txc,
                              const TActorContext &ctx);

    ui64 GetMemoryConsumption() const;

    ui64 GetRequiredMemory() const {
        Y_VERIFY(!GetTxCacheUsage() || !IsTxDataReleased());
        ui64 requiredMem = GetTxCacheUsage() + GetReleasedTxDataSize();
        if (!requiredMem)
            requiredMem = GetMemoryConsumption();
        return requiredMem;
    }

    void ReleaseTxData(NTabletFlatExecutor::TTxMemoryProviderBase &provider, const TActorContext &ctx);
    ERestoreDataStatus RestoreTxData(TDataShard * self, TTransactionContext &txc, const TActorContext &ctx);
    void FinalizeDataTxPlan();

    // TOperation iface.
    void BuildExecutionPlan(bool loaded) override;

    const NMiniKQL::IEngineFlat::TValidationInfo &GetKeysInfo() const override
    {
        if (DataTx) {
            Y_VERIFY(DataTx->TxInfo().Loaded);
            return DataTx->TxInfo();
        }
        Y_VERIFY_DEBUG(IsSchemeTx() || IsSnapshotTx() || IsDistributedEraseTx() || IsCommitWritesTx(),
            "Unexpected access to invalidated keys: non-scheme tx %" PRIu64, GetTxId());
        // For scheme tx global reader and writer flags should
        // result in all required dependencies.
        return TOperation::GetKeysInfo();
    }

    ui64 LockTxId() const override
    {
        if (DataTx)
            return DataTx->LockTxId();
        return 0;
    }

    ui32 LockNodeId() const override
    {
        if (DataTx)
            return DataTx->LockNodeId();
        return 0;
    }

    bool HasLockedWrites() const  override
    {
        if (DataTx)
            return DataTx->HasLockedWrites();
        return false;
    }

    void FillState(NKikimrTxDataShard::TEvGetOperationResponse &resp) const;

    void SetScanSnapshotId(ui64 id) { ScanSnapshotId = id; }
    ui64 GetScanSnapshotId() const { return ScanSnapshotId; }

    void SetScanTask(ui64 id) { ScanTask = id; }
    ui64 GetScanTask() const { return ScanTask; }

    void SetAsyncJobActor(TActorId aid) { AsyncJobActor = aid; }
    TActorId GetAsyncJobActor() const { return AsyncJobActor; }
    void KillAsyncJobActor(const TActorContext& ctx);

    void SetStreamSink(TActorId sink) { StreamSink = sink; }
    TActorId GetStreamSink() const { return StreamSink; }

    void SetScanActor(TActorId aid) { ScanActor = aid; }
    TActorId GetScanActor() const { return ScanActor; }

    ui64 IncrementPageFaultCount() {
        return ++PageFaultCount;
    }

private:
    void TrackMemory() const;
    void UntrackMemory() const;

private:
    TValidatedDataTx::TPtr DataTx;
    THolder<NKikimrTxDataShard::TFlatSchemeTransaction> SchemeTx;
    THolder<NKikimrTxDataShard::TSnapshotTransaction> SnapshotTx;
    TDistributedEraseTx::TPtr DistributedEraseTx;
    TCommitWritesTx::TPtr CommitWritesTx;
    TString TxBody;

    // TODO: move to persistent part of operation's flags
    ui64 ArtifactFlags;
    ui64 TxCacheUsage;
    ui64 ReleasedTxDataSize;
    ui64 SchemeShardId;
    ui64 SubDomainPathId;
    NKikimrSubDomains::TProcessingParams ProcessingParams;
    TSchemaOperation::EType SchemeTxType;
    ui64 ScanSnapshotId;
    ui64 ScanTask;
    TActorId AsyncJobActor;
    TActorId StreamSink;
    TActorId ScanActor;
    ui64 PageFaultCount = 0;
};

inline IOutputStream& operator << (IOutputStream& out, const TActiveTransaction& tx) {
    out << '[' << tx.GetStep() << ':' << tx.GetTxId() << ']';
    return out;
}

}}
