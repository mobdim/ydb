#pragma once

#include <util/system/hp_timer.h>

#include <ydb/core/tablet_flat/tablet_flat_executed.h>
#include <ydb/core/base/tablet_pipe.h>
#include <ydb/core/base/appdata.h>
#include <library/cpp/actors/core/hfunc.h>
#include <ydb/core/persqueue/events/global.h>
#include <ydb/core/tablet_flat/flat_dbase_scheme.h>
#include <ydb/core/tablet_flat/flat_cxx_database.h>
#include <ydb/core/tx/schemeshard/schemeshard.h>
#include <ydb/core/engine/minikql/flat_local_tx_factory.h>
#include <ydb/library/persqueue/topic_parser/topic_parser.h>

namespace NKikimr {
namespace NPQ {

using namespace NTabletFlatExecutor;

class TMetricsTimeKeeper {
public:
    TMetricsTimeKeeper(NMetrics::TResourceMetrics* metrics, const TActorContext& ctx)
        : Ctx(ctx)
        , Metrics(metrics)
    {}

    ~TMetricsTimeKeeper() {
        ui64 counter = ui64(CpuTimer.PassedReset() * 1000000.);
        if (counter && Metrics) {
            Metrics->CPU.Increment(counter);
            Metrics->TryUpdate(Ctx);
        }
    }

private:
    const TActorContext& Ctx;
    NMetrics::TResourceMetrics *Metrics;
    THPTimer CpuTimer;
};


enum EPartitionState {
    StateRegular = 0,
    StateWaitingFromSS,
};

class TPersQueueReadBalancer : public TActor<TPersQueueReadBalancer>, public TTabletExecutedFlat {

    struct Schema : NIceDb::Schema {
        struct Data : Table<32> {
            struct Key : Column<32, NScheme::NTypeIds::Uint32> {};
            struct PathId : Column<33, NScheme::NTypeIds::Uint64> {};
            struct Topic : Column<34, NScheme::NTypeIds::Utf8> {};
            struct Path : Column<35, NScheme::NTypeIds::Utf8> {};
            struct Version : Column<36, NScheme::NTypeIds::Uint32> {};
            struct Config : Column<40, NScheme::NTypeIds::Utf8> {};
            struct MaxPartsPerTablet : Column<41, NScheme::NTypeIds::Uint32> {};
            struct SchemeShardId : Column<42, NScheme::NTypeIds::Uint64> {};
            struct NextPartitionId : Column<43, NScheme::NTypeIds::Uint64> {};

            using TKey = TableKey<Key>;
            using TColumns = TableColumns<Key, PathId, Topic, Path, Version, Config, MaxPartsPerTablet, SchemeShardId, NextPartitionId>;
        };

        struct Partitions : Table<33> {
            struct Partition : Column<32, NScheme::NTypeIds::Uint32> {};
            struct TabletId : Column<33, NScheme::NTypeIds::Uint64> {};

            struct State : Column<34, NScheme::NTypeIds::Uint32> {};

            using TKey = TableKey<Partition>;
            using TColumns = TableColumns<Partition, TabletId, State>;
        };

        struct Groups : Table<34> {
            struct GroupId : Column<32, NScheme::NTypeIds::Uint32> {};
            struct Partition : Column<33, NScheme::NTypeIds::Uint32> {};

            using TKey = TableKey<GroupId, Partition>;
            using TColumns = TableColumns<GroupId, Partition>;
        };

        struct Tablets : Table<35> {
            struct Owner : Column<32, NScheme::NTypeIds::Uint64> {};
            struct Idx : Column<33, NScheme::NTypeIds::Uint64> {};
            struct TabletId : Column<34, NScheme::NTypeIds::Uint64> {};

            using TKey = TableKey<TabletId>;
            using TColumns = TableColumns<Owner, Idx, TabletId>;
        };

        struct Operations : Table<36> {
            struct Idx : Column<33, NScheme::NTypeIds::Uint64> {};
            struct State : Column<34, NScheme::NTypeIds::Utf8> {}; //serialzed protobuf

            using TKey = TableKey<Idx>;
            using TColumns = TableColumns<Idx, State>;
        };

        using TTables = SchemaTables<Data, Partitions, Groups, Tablets, Operations>;
    };


    struct TTxPreInit : public ITransaction {
        TPersQueueReadBalancer * const Self;

        TTxPreInit(TPersQueueReadBalancer *self)
            : Self(self)
        {}

        bool Execute(TTransactionContext& txc, const TActorContext& ctx) override;

        void Complete(const TActorContext& ctx) override;
    };

    friend struct TTxPreInit;


    struct TTxInit : public ITransaction {
        TPersQueueReadBalancer * const Self;

        TTxInit(TPersQueueReadBalancer *self)
            : Self(self)
        {}

        bool Execute(TTransactionContext& txc, const TActorContext& ctx) override;

        void Complete(const TActorContext& ctx) override;
    };

    friend struct TTxInit;

    struct TPartInfo {
        ui64 TabletId;
        ui32 Group;
        TPartInfo(const ui64 tabletId, const ui32 group)
            : TabletId(tabletId)
            , Group(group)
        {}
    };

    struct TTabletInfo {
        ui64 Owner;
        ui64 Idx;
    };

    struct TTxWrite : public ITransaction {
        TPersQueueReadBalancer * const Self;
        TVector<ui32> DeletedPartitions;
        TVector<std::pair<ui32, TPartInfo>> NewPartitions;
        TVector<std::pair<ui64, TTabletInfo>> NewTablets;
        TVector<std::pair<ui32, ui32>> NewGroups;
        TVector<std::pair<ui64, TTabletInfo>> ReallocatedTablets;

        TTxWrite(TPersQueueReadBalancer *self, TVector<ui32>&& deletedPartitions, TVector<std::pair<ui32, TPartInfo>>&& newPartitions,
                 TVector<std::pair<ui64, TTabletInfo>>&& newTablets, TVector<std::pair<ui32, ui32>>&& newGroups,
                 TVector<std::pair<ui64, TTabletInfo>>&& reallocatedTablets)
            : Self(self)
            , DeletedPartitions(std::move(deletedPartitions))
            , NewPartitions(std::move(newPartitions))
            , NewTablets(std::move(newTablets))
            , NewGroups(std::move(newGroups))
            , ReallocatedTablets(std::move(reallocatedTablets))
        {}

        bool Execute(TTransactionContext& txc, const TActorContext& ctx) override;

        void Complete(const TActorContext &ctx) override;
    };


    friend struct TTxWrite;

    void Handle(TEvents::TEvPoisonPill::TPtr&, const TActorContext &ctx) {
        Become(&TThis::StateBroken);
        ctx.Send(Tablet(), new TEvents::TEvPoisonPill);
    }

    void HandleWakeup(TEvents::TEvWakeup::TPtr&, const TActorContext &ctx) {
        GetStat(ctx); //TODO: do it only on signals from outerspace right now

        ctx.Schedule(TDuration::Seconds(30), new TEvents::TEvWakeup()); //TODO: remove it
    }

    void HandleUpdateACL(TEvPersQueue::TEvUpdateACL::TPtr&, const TActorContext &ctx) {
        GetACL(ctx);
    }

    void Die(const TActorContext& ctx) override {
        for (auto& pipe : TabletPipes) {
            NTabletPipe::CloseClient(ctx, pipe.second);
        }
        TabletPipes.clear();
        TActor<TPersQueueReadBalancer>::Die(ctx);
    }

    void OnActivateExecutor(const TActorContext &ctx) override {
        ResourceMetrics = Executor()->GetResourceMetrics();
        Become(&TThis::StateWork);
        if (Executor()->GetStats().IsFollower)
            Y_FAIL("is follower works well with Balancer?");
        else
            Execute(new TTxPreInit(this), ctx);
    }

    void OnDetach(const TActorContext &ctx) override {
        Die(ctx);
    }

    void OnTabletDead(TEvTablet::TEvTabletDead::TPtr&, const TActorContext &ctx) override {
        Die(ctx);
    }

    void DefaultSignalTabletActive(const TActorContext &ctx) override {
        Y_UNUSED(ctx); //TODO: this is signal that tablet is ready for work
    }

    void InitDone(const TActorContext &ctx) {

        StartPartitionIdForWrite = NextPartitionIdForWrite = rand() % TotalGroups;

        TStringBuilder s;
        s << "BALANCER INIT DONE for " << Topic << ": ";
        for (auto& p : PartitionsInfo) {
            s << "(" << p.first << ", " << p.second.TabletId << ") ";
        }
        LOG_DEBUG_S(ctx, NKikimrServices::PERSQUEUE_READ_BALANCER, s);
        for (auto& p : ClientsInfo) {
            for (auto& c : p.second.ClientGroupsInfo) {
                c.second.Balance(ctx);
            }
        }

        for (auto &ev : UpdateEvents) {
            ctx.Send(ctx.SelfID, ev.Release());
        }
        UpdateEvents.clear();

        for (auto &ev : RegisterEvents) {
            ctx.Send(ctx.SelfID, ev.Release());
        }
        RegisterEvents.clear();

        ctx.Schedule(TDuration::Seconds(30), new TEvents::TEvWakeup()); //TODO: remove it
        ctx.Send(ctx.SelfID, new TEvPersQueue::TEvUpdateACL());
    }

    bool OnRenderAppHtmlPage(NMon::TEvRemoteHttpInfo::TPtr ev, const TActorContext& ctx) override;
    TString GenerateStat();

    void Handle(TEvPersQueue::TEvWakeupClient::TPtr &ev, const TActorContext& ctx);
    void Handle(TEvPersQueue::TEvDescribe::TPtr &ev, const TActorContext& ctx);

    void HandleOnInit(TEvPersQueue::TEvUpdateBalancerConfig::TPtr &ev, const TActorContext& ctx);
    void Handle(TEvPersQueue::TEvUpdateBalancerConfig::TPtr &ev, const TActorContext& ctx);

    void HandleOnInit(TEvPersQueue::TEvRegisterReadSession::TPtr &ev, const TActorContext& ctx);
    void Handle(TEvPersQueue::TEvRegisterReadSession::TPtr &ev, const TActorContext& ctx);

    void Handle(TEvPersQueue::TEvGetReadSessionsInfo::TPtr &ev, const TActorContext& ctx);
    void Handle(TEvPersQueue::TEvCheckACL::TPtr&, const TActorContext&);
    void Handle(TEvPersQueue::TEvGetPartitionIdForWrite::TPtr&, const TActorContext&);

    void Handle(TEvTabletPipe::TEvServerConnected::TPtr& ev, const TActorContext&);
    void Handle(TEvTabletPipe::TEvServerDisconnected::TPtr& ev, const TActorContext&);

    void Handle(TEvTabletPipe::TEvClientConnected::TPtr& ev, const TActorContext&);
    void Handle(TEvTabletPipe::TEvClientDestroyed::TPtr& ev, const TActorContext&);

    TStringBuilder GetPrefix() const;

    void RequestTabletIfNeeded(const ui64 tabletId, const TActorContext&);
    void RestartPipe(const ui64 tabletId, const TActorContext&);
    void CheckStat(const TActorContext&);
    void RespondWithACL(
        const TEvPersQueue::TEvCheckACL::TPtr &request,
        const NKikimrPQ::EAccess &access,
        const TString &error,
        const TActorContext &ctx);
    void CheckACL(const TEvPersQueue::TEvCheckACL::TPtr &request, const NACLib::TUserToken& token, const TActorContext &ctx);
    void GetStat(const TActorContext&);
    void GetACL(const TActorContext&);
    void AnswerWaitingRequests(const TActorContext& ctx);

    void Handle(TEvPersQueue::TEvPartitionReleased::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvents::TEvPoisonPill &ev, const TActorContext& ctx);

    void Handle(TEvPersQueue::TEvStatusResponse::TPtr& ev, const TActorContext& ctx);
    void Handle(NSchemeShard::TEvSchemeShard::TEvDescribeSchemeResult::TPtr& ev, const TActorContext& ctx);

    void RegisterSession(const TActorId& pipe, const TActorContext& ctx);
    struct TPipeInfo;
    void UnregisterSession(const TActorId& pipe, const TActorContext& ctx);
    void RebuildStructs();

    bool Inited;
    ui64 PathId;
    TString Topic;
    TString Path;
    ui32 Generation;
    int Version;
    ui32 MaxPartsPerTablet;
    ui64 SchemeShardId;
    NKikimrPQ::TPQTabletConfig TabletConfig;
    NACLib::TSecurityObject ACL;
    TInstant LastACLUpdate;

    THashSet<TString> Consumers;

    ui64 TxId;
    ui32 NumActiveParts;

    TVector<TActorId> WaitingResponse;
    TVector<TEvPersQueue::TEvCheckACL::TPtr> WaitingACLRequests;
    TVector<TEvPersQueue::TEvDescribe::TPtr> WaitingDescribeRequests;

    struct TPipeInfo {
        TString ClientId;
        TString Session;
        TActorId Sender;
        bool WithGroups;
        ui32 ServerActors;
    };

    enum EPartitionState {
        EPS_FREE = 0,
        EPS_ACTIVE = 1
    };

    struct TPartitionInfo {
        ui64 TabletId;
        EPartitionState State;
        TActorId Session;
        ui32 GroupId;
    };

    struct TClientGroupInfo {
        struct TSessionInfo {
            TSessionInfo(const TString& session, const TActorId sender, const TString& clientNode, ui32 proxyNodeId, TInstant ts)
                : Session(session)
                , Sender(sender)
                , NumSuspended(0)
                , NumActive(0)
                , ClientNode(clientNode)
                , ProxyNodeId(proxyNodeId)
                , Timestamp(ts)
            {}

            TString Session;
            TActorId Sender;
            ui32 NumSuspended;
            ui32 NumActive;

            TString ClientNode;
            ui32 ProxyNodeId;
            TInstant Timestamp;
        };

        TString ClientId;
        TString Topic;
        ui64 TabletId;
        TString Path;
        ui32 Generation = 0;
        ui64 RandomNumber = 0;
        ui32* Step = nullptr;

        ui32 Group = 0;

        THashMap<ui32, TPartitionInfo> PartitionsInfo; // partitionId -> info
        std::deque<ui32> FreePartitions;
        THashMap<std::pair<TActorId, ui64>, TSessionInfo> SessionsInfo; //map from ActorID and random value - need for reordering sessions in different topics

        void ScheduleBalance(const TActorContext& ctx);
        void Balance(const TActorContext& ctx);
        void LockPartition(const TActorId pipe, ui32 partition, const TActorContext& ctx);
        void ReleasePartition(const TActorId pipe, const ui32 group, const ui32 count, const TActorContext& ctx);
        TStringBuilder GetPrefix() const;

        bool WakeupScheduled = false;
    };

    THashMap<ui32, TPartitionInfo> PartitionsInfo;
    THashMap<ui32, TVector<ui32>> GroupsInfo;

    THashMap<ui64, TTabletInfo> TabletsInfo;
    ui64 MaxIdx;

    ui32 NextPartitionId;
    ui32 NextPartitionIdForWrite;
    ui32 StartPartitionIdForWrite;
    ui32 TotalGroups;
    bool NoGroupsInBase;

    struct TClientInfo {
        THashMap<ui32, TClientGroupInfo> ClientGroupsInfo; //map from group to info
        ui32 SessionsWithGroup = 0;

        TString ClientId;
        TString Topic;
        ui64 TabletId;
        TString Path;
        ui32 Generation = 0;
        ui32 Step = 0;

        void KillGroup(const ui32 group, const TActorContext& ctx);
        void MergeGroups(const TActorContext& ctx);
        TClientGroupInfo& AddGroup(const ui32 group);
        void FillEmptyGroup(const ui32 group, const THashMap<ui32, TPartitionInfo>& partitionsInfo);
        void AddSession(const ui32 group, const THashMap<ui32, TPartitionInfo>& partitionsInfo,
                        const TActorId& sender, const NKikimrPQ::TRegisterReadSession& record);
        TStringBuilder GetPrefix() const;

    };

    THashMap<TString, TClientInfo> ClientsInfo; //map from userId -> to info

    THashMap<TActorId, TPipeInfo> PipesInfo;

    NMetrics::TResourceMetrics *ResourceMetrics;

    THashMap<ui64, TActorId> TabletPipes;

    THashSet<ui64> WaitingForStat;
    bool WaitingForACL;
    ui64 TotalAvgSpeedSec;
    ui64 MaxAvgSpeedSec;
    ui64 TotalAvgSpeedMin;
    ui64 MaxAvgSpeedMin;
    ui64 TotalAvgSpeedHour;
    ui64 MaxAvgSpeedHour;
    ui64 TotalAvgSpeedDay;
    ui64 MaxAvgSpeedDay;

    std::deque<TAutoPtr<TEvPersQueue::TEvRegisterReadSession>> RegisterEvents;
    std::deque<TAutoPtr<TEvPersQueue::TEvPersQueue::TEvUpdateBalancerConfig>> UpdateEvents;
public:
    static constexpr NKikimrServices::TActivity::EType ActorActivityType() {
        return NKikimrServices::TActivity::PERSQUEUE_READ_BALANCER_ACTOR;
    }

    TPersQueueReadBalancer(const TActorId &tablet, TTabletStorageInfo *info)
        : TActor(&TThis::StateInit)
        , TTabletExecutedFlat(info, tablet, new NMiniKQL::TMiniKQLFactory)
        , Inited(false)
        , PathId(0)
        , Generation(0)
        , Version(-1)
        , MaxPartsPerTablet(0)
        , SchemeShardId(0)
        , LastACLUpdate(TInstant::Zero())
        , TxId(0)
        , NumActiveParts(0)
        , MaxIdx(0)
        , NextPartitionId(0)
        , NextPartitionIdForWrite(0)
        , StartPartitionIdForWrite(0)
        , TotalGroups(0)
        , NoGroupsInBase(true)
        , ResourceMetrics(nullptr)
        , WaitingForACL(false)
        , TotalAvgSpeedSec(0)
        , MaxAvgSpeedSec(0)
        , TotalAvgSpeedMin(0)
        , MaxAvgSpeedMin(0)
        , TotalAvgSpeedHour(0)
        , MaxAvgSpeedHour(0)
        , TotalAvgSpeedDay(0)
        , MaxAvgSpeedDay(0)
    {}

    STFUNC(StateInit) {
        TMetricsTimeKeeper keeper(ResourceMetrics, ctx);

        switch (ev->GetTypeRewrite()) {
            HFunc(TEvents::TEvPoisonPill, Handle);
            HFunc(TEvPersQueue::TEvUpdateBalancerConfig, HandleOnInit);
            HFunc(TEvPersQueue::TEvWakeupClient, Handle);
            HFunc(TEvPersQueue::TEvDescribe, Handle);
            HFunc(TEvPersQueue::TEvRegisterReadSession, HandleOnInit);
            HFunc(TEvPersQueue::TEvGetReadSessionsInfo, Handle);
            HFunc(TEvTabletPipe::TEvServerConnected, Handle);
            HFunc(TEvTabletPipe::TEvServerDisconnected, Handle);
            HFunc(TEvPersQueue::TEvCheckACL, Handle);
            HFunc(TEvPersQueue::TEvGetPartitionIdForWrite, Handle);
            default:
                StateInitImpl(ev, ctx);
                break;
        }
    }

    STFUNC(StateWork) {
        TMetricsTimeKeeper keeper(ResourceMetrics, ctx);

        switch (ev->GetTypeRewrite()) {
            HFunc(TEvents::TEvPoisonPill, Handle);
            HFunc(TEvents::TEvWakeup, HandleWakeup);
            HFunc(TEvPersQueue::TEvUpdateACL, HandleUpdateACL);
            HFunc(TEvPersQueue::TEvCheckACL, Handle);
            HFunc(TEvPersQueue::TEvGetPartitionIdForWrite, Handle);
            HFunc(TEvPersQueue::TEvUpdateBalancerConfig, Handle);
            HFunc(TEvPersQueue::TEvWakeupClient, Handle);
            HFunc(TEvPersQueue::TEvDescribe, Handle);
            HFunc(TEvPersQueue::TEvRegisterReadSession, Handle);
            HFunc(TEvPersQueue::TEvGetReadSessionsInfo, Handle);
            HFunc(TEvPersQueue::TEvPartitionReleased, Handle);
            HFunc(TEvTabletPipe::TEvServerConnected, Handle);
            HFunc(TEvTabletPipe::TEvServerDisconnected, Handle);
            HFunc(TEvTabletPipe::TEvClientConnected, Handle);
            HFunc(TEvTabletPipe::TEvClientDestroyed, Handle);
            HFunc(TEvPersQueue::TEvStatusResponse, Handle);
            HFunc(NSchemeShard::TEvSchemeShard::TEvDescribeSchemeResult, Handle);

            default:
                HandleDefaultEvents(ev, ctx);
                break;
        }
    }

    STFUNC(StateBroken) {
        TMetricsTimeKeeper keeper(ResourceMetrics, ctx);

        switch (ev->GetTypeRewrite()) {
            HFunc(TEvTablet::TEvTabletDead, HandleTabletDead)
        }
    }

};

}
}
