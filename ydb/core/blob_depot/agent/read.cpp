#include "agent_impl.h"

namespace NKikimr::NBlobDepot {

    struct TBlobDepotAgent::TQuery::TReadContext : TRequestContext {
        TReadArg ReadArg;
        const ui64 Size;
        TString Buffer;
        bool Terminated = false;
        ui32 NumPartsPending = 0;
        TLogoBlobID BlobWithoutData;

        TReadContext(TReadArg&& readArg, ui64 size)
            : ReadArg(std::move(readArg))
            , Size(size)
        {}

        void EndWithError(TQuery *query, NKikimrProto::EReplyStatus status, TString errorReason) {
            query->OnRead(ReadArg.Tag, status, errorReason);
            Terminated = true;
        }

        void Abort() {
            Terminated = true;
        }

        void EndWithSuccess(TQuery *query) {
            query->OnRead(ReadArg.Tag, NKikimrProto::OK, std::move(Buffer));
        }

        ui64 GetTag() const {
            return ReadArg.Tag;
        }

        struct TPartContext : TRequestContext {
            std::shared_ptr<TReadContext> Read;
            std::vector<ui64> Offsets;

            TPartContext(std::shared_ptr<TReadContext> read)
                : Read(std::move(read))
            {}
        };
    };

    bool TBlobDepotAgent::TQuery::IssueRead(TReadArg&& arg, TString& error) {
        STLOG(PRI_DEBUG, BLOB_DEPOT_AGENT, BDA34, "IssueRead", (AgentId, Agent.LogId), (QueryId, GetQueryId()),
            (ReadId, arg.Tag), (Offset, arg.Offset), (Size, arg.Size), (Value, arg.Value));

        ui64 outputOffset = 0;

        struct TReadItem {
            ui32 GroupId;
            TLogoBlobID Id;
            ui32 Offset;
            ui32 Size;
            ui64 OutputOffset;
        };
        std::vector<TReadItem> items;

        ui64 offset = arg.Offset;
        ui64 size = arg.Size;

        for (const auto& value : arg.Value.Chain) {
            const ui32 groupId = value.GroupId;
            const auto& blobId = value.BlobId;
            const ui32 begin = value.SubrangeBegin;
            const ui32 end = value.SubrangeEnd;

            if (end <= begin || blobId.BlobSize() < end) {
                error = "incorrect SubrangeBegin/SubrangeEnd pair";
                STLOG(PRI_CRIT, BLOB_DEPOT_AGENT, BDA24, error, (AgentId, Agent.LogId), (QueryId, GetQueryId()),
                    (ReadId, arg.Tag), (Offset, arg.Offset), (Size, arg.Size), (Value, arg.Value));
                return false;
            }

            // calculate the whole length of current part
            ui64 partLen = end - begin;
            if (offset >= partLen) {
                // just skip this part
                offset -= partLen;
                continue;
            }

            // adjust it to fit size and offset
            partLen = Min(size ? size : Max<ui64>(), partLen - offset);
            Y_VERIFY(partLen);

            items.push_back(TReadItem{groupId, blobId, ui32(offset + begin), ui32(partLen), outputOffset});

            outputOffset += partLen;
            offset = 0;

            if (size) {
                size -= partLen;
                if (!size) {
                    break;
                }
            }
        }

        if (size) {
            error = "incorrect offset/size provided";
            STLOG(PRI_ERROR, BLOB_DEPOT_AGENT, BDA25, error, (AgentId, Agent.LogId), (QueryId, GetQueryId()),
                (ReadId, arg.Tag), (Offset, arg.Offset), (Size, arg.Size), (Value, arg.Value));
            return false;
        }

        auto context = std::make_shared<TReadContext>(std::move(arg), outputOffset);
        if (!outputOffset) {
            context->EndWithSuccess(this);
            return true;
        }

        THashMap<ui32, std::vector<std::tuple<ui64 /*offset*/, TEvBlobStorage::TEvGet::TQuery>>> queriesPerGroup;
        for (const TReadItem& item : items) {
            TEvBlobStorage::TEvGet::TQuery query;
            query.Set(item.Id, item.Offset, item.Size);
            queriesPerGroup[item.GroupId].emplace_back(item.OutputOffset, query);
        }

        for (const auto& [groupId, queries] : queriesPerGroup) {
            const ui32 sz = queries.size();
            TArrayHolder<TEvBlobStorage::TEvGet::TQuery> q(new TEvBlobStorage::TEvGet::TQuery[sz]);
            auto partContext = std::make_shared<TReadContext::TPartContext>(context);
            for (ui32 i = 0; i < sz; ++i) {
                ui64 outputOffset;
                std::tie(outputOffset, q[i]) = queries[i];
                partContext->Offsets.push_back(outputOffset);
            }

            // when the TEvGet query is sent to the underlying proxy, MustRestoreFirst must be cleared, or else it may
            // lead to ERROR due to impossibility of writes; all MustRestoreFirst should be handled by the TEvResolve
            // query
            auto event = std::make_unique<TEvBlobStorage::TEvGet>(q, sz, TInstant::Max(), context->ReadArg.GetHandleClass,
                context->ReadArg.MustRestoreFirst && groupId != Agent.DecommitGroupId);
            event->ReaderTabletData = context->ReadArg.ReaderTabletData;
            STLOG(PRI_DEBUG, BLOB_DEPOT_AGENT, BDA39, "issuing TEvGet", (AgentId, Agent.LogId), (QueryId, GetQueryId()),
                (ReadId, context->GetTag()), (GroupId, groupId), (Msg, *event));
            Agent.SendToProxy(groupId, std::move(event), this, std::move(partContext));
            ++context->NumPartsPending;
        }

        Y_VERIFY(context->NumPartsPending);

        return true;
    }

    void TBlobDepotAgent::TQuery::HandleGetResult(const TRequestContext::TPtr& context, TEvBlobStorage::TEvGetResult& msg) {
        auto& partContext = context->Obtain<TReadContext::TPartContext>();
        auto& readContext = *partContext.Read;
        STLOG(PRI_DEBUG, BLOB_DEPOT_AGENT, BDA41, "HandleGetResult", (AgentId, Agent.LogId), (ReadId, readContext.GetTag()),
            (Msg, msg));
        if (readContext.Terminated) {
            return; // just ignore this read
        }

        if (msg.Status != NKikimrProto::OK) {
            readContext.EndWithError(this, msg.Status, std::move(msg.ErrorReason));
        } else {
            Y_VERIFY(msg.ResponseSz == partContext.Offsets.size());

            for (ui32 i = 0; i < msg.ResponseSz; ++i) {
                auto& blob = msg.Responses[i];
                if (blob.Status == NKikimrProto::NODATA) {
                    NKikimrBlobDepot::TEvResolve resolve;
                    auto *item = resolve.AddItems();
                    item->SetExactKey(readContext.ReadArg.Key);
                    Agent.Issue(std::move(resolve), this, partContext.Read);
                    readContext.Abort();
                    readContext.BlobWithoutData = blob.Id;
                    return;
                } else if (blob.Status != NKikimrProto::OK) {
                    return readContext.EndWithError(this, blob.Status, TStringBuilder() << "failed to read BlobId# " << blob.Id);
                }

                auto& buffer = readContext.Buffer;
                const ui64 offset = partContext.Offsets[i];

                Y_VERIFY(offset < readContext.Size && blob.Buffer.size() <= readContext.Size - offset);

                if (!buffer && !offset) {
                    buffer = std::move(blob.Buffer);
                    buffer.resize(readContext.Size);
                } else {
                    if (!buffer) {
                        buffer = TString::Uninitialized(readContext.Size);
                    }
                    memcpy(buffer.Detach() + offset, blob.Buffer.data(), blob.Buffer.size());
                }
            }

            if (!--readContext.NumPartsPending) {
                readContext.EndWithSuccess(this);
            }
        }
    }

    void TBlobDepotAgent::TQuery::HandleResolveResult(const TRequestContext::TPtr& context, TEvBlobDepot::TEvResolveResult& msg) {
        auto& readContext = context->Obtain<TReadContext>();
        STLOG(PRI_DEBUG, BLOB_DEPOT_AGENT, BDA42, "HandleResolveResult", (AgentId, Agent.LogId), (QueryId, GetQueryId()),
            (ReadId, readContext.GetTag()), (Msg, msg.Record));
        if (msg.Record.GetStatus() != NKikimrProto::OK) {
            readContext.EndWithError(this, msg.Record.GetStatus(), msg.Record.GetErrorReason());
        } else if (msg.Record.ResolvedKeysSize() == 1) {
            const auto& item = msg.Record.GetResolvedKeys(0);
            if (TResolvedValue value(item); value.Supersedes(readContext.ReadArg.Value)) { // value chain has changed, we have to try again
                readContext.ReadArg.Value = std::move(value);
                TString error;
                if (!IssueRead(std::move(readContext.ReadArg), error)) {
                    readContext.EndWithError(this, NKikimrProto::ERROR, TStringBuilder() << "failed to restart read Error# " << error);
                }
            } else if (!item.GetReliablyWritten()) { // this was unassimilated value and we got NODATA for it
                readContext.EndWithError(this, NKikimrProto::NODATA, {});
            } else {
                Y_VERIFY_DEBUG_S(false, "data is lost AgentId# " << Agent.LogId << " QueryId# " << GetQueryId()
                    << " ReadId# " << readContext.GetTag() << " BlobId# " << readContext.BlobWithoutData);
                STLOG(PRI_CRIT, BLOB_DEPOT_AGENT, BDA40, "failed to read blob -- data is lost", (AgentId, Agent.LogId),
                    (QueryId, GetQueryId()), (ReadId, readContext.GetTag()), (BlobId, readContext.BlobWithoutData));
                readContext.EndWithError(this, NKikimrProto::ERROR, TStringBuilder() << "failed to read BlobId# "
                    << readContext.BlobWithoutData << ": data is lost");
            }
        } else {
            Y_VERIFY(!msg.Record.ResolvedKeysSize());
            readContext.EndWithError(this, NKikimrProto::NODATA, {});
        }
    }

} // NKikimr::NBlobDepot
