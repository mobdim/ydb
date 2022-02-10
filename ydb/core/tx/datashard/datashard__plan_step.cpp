#include "datashard_txs.h"

#include <util/string/vector.h>

namespace NKikimr {
namespace NDataShard {

using namespace NTabletFlatExecutor;

TDataShard::TTxPlanStep::TTxPlanStep(TDataShard *self, TEvTxProcessing::TEvPlanStep::TPtr ev)
    : TBase(self)
    , Ev(ev)
    , IsAccepted(false)
    , RequestStartTime(TAppData::TimeProvider->Now())
{
    Y_VERIFY(Ev);
}

bool TDataShard::TTxPlanStep::Execute(TTransactionContext &txc, const TActorContext &ctx) {
    Y_VERIFY(Ev);

    // TEvPlanStep are strictly ordered by mediator so this Tx must not be retried not to break this ordering!
    txc.DB.NoMoreReadsForTx();

    TxByAck.clear();
    IsAccepted = false;

    const ui64 step = Ev->Get()->Record.GetStep();
    Self->LastKnownMediator = Ev->Get()->Record.GetMediatorID();

    TVector<ui64> txIds;
    txIds.reserve(Ev->Get()->Record.TransactionsSize());
    for (const auto& tx : Ev->Get()->Record.GetTransactions()) {
        Y_VERIFY(tx.HasTxId());
        Y_VERIFY(tx.HasAckTo());

        txIds.push_back(tx.GetTxId());

        TActorId txOwner = ActorIdFromProto(tx.GetAckTo());
        TxByAck[txOwner].push_back(tx.GetTxId());
    }

    if (Self->State != TShardState::Offline && Self->State != TShardState::PreOffline) {
        // The DS is completing Drop, so we just ignore PlanStep assuming that it might only contain
        // transactions that have already been executed.
        // NOTE: There is a scenario when because of retries the Coordinator might send some old Tx with
        // a new Step.
        IsAccepted = Self->Pipeline.PlanTxs(step, txIds, txc, ctx);
    }

    if (! IsAccepted) {
        LOG_ERROR_S(ctx, NKikimrServices::TX_DATASHARD,
            "Ignore old txIds [" << JoinStrings(txIds.begin(), txIds.end(), ", ")
            << "] for step " << step << " outdated step " << Self->Pipeline.OutdatedCleanupStep()
            << " at tablet " << Self->TabletID());
        Self->IncCounter(COUNTER_PLAN_STEP_IGNORED);
        return true;
    }

    for (ui64 txId : txIds) {
        LOG_DEBUG_S(ctx, NKikimrServices::TX_DATASHARD,
                    "Planned transaction txId " << txId << " at step " << step
                    << " at tablet " << Self->TabletID() << " " << Ev->Get()->Record);
    }

    Self->PlanQueue.Progress(ctx);
    Self->IncCounter(COUNTER_PLAN_STEP_ACCEPTED);
    return true;
}

void TDataShard::TTxPlanStep::Complete(const TActorContext &ctx) {
    Y_VERIFY(Ev);
    ui64 step = Ev->Get()->Record.GetStep();

    for (auto& kv : TxByAck) {
        THolder<TEvTxProcessing::TEvPlanStepAck> ack =
            MakeHolder<TEvTxProcessing::TEvPlanStepAck>(Self->TabletID(), step, kv.second.begin(), kv.second.end()); 
        LOG_DEBUG_S(ctx, NKikimrServices::TX_DATASHARD, "Sending '" << ack->ToString());

        ctx.Send(kv.first, ack.Release()); // Ack to Tx coordinator
    }

    THolder<TEvTxProcessing::TEvPlanStepAccepted> accepted =
        MakeHolder<TEvTxProcessing::TEvPlanStepAccepted>(Self->TabletID(), step); 
    LOG_DEBUG_S(ctx, NKikimrServices::TX_DATASHARD, "Sending '" << accepted->ToString());

    ctx.Send(Ev->Sender, accepted.Release()); // Reply to the mediator

    if (IsAccepted) {
        TDuration duration = TAppData::TimeProvider->Now() - RequestStartTime;
        Self->IncCounter(COUNTER_ACCEPTED_PLAN_STEP_COMPLETE_LATENCY, duration);
    }
}

}}
