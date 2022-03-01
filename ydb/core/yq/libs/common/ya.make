OWNER(g:yq)

LIBRARY()

SRCS(
    entity_id.cpp
    rows_proto_splitter.cpp
)

PEERDIR(
    ydb/core/yq/libs/control_plane_storage/events
    ydb/core/yq/libs/events
    ydb/library/yql/providers/common/structured_token
    ydb/library/yql/public/issue
    ydb/public/api/protos
)

YQL_LAST_ABI_VERSION()

END()

RECURSE_FOR_TESTS(
    ut
)
