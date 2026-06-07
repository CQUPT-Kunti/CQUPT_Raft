#pragma once

#include "view/view_registry.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

#include <grpcpp/grpcpp.h>

#include "view.grpc.pb.h"

namespace viewdemo
{
    struct ViewNodeServiceImplConfig
    {
        // T019 默认应使用系统时间；测试可注入确定性时间源。
        std::function<std::uint64_t()> now_unix_ms;
    };

    // ViewNodeServiceImpl 只负责 proto/view.proto 与 ViewNodeRegistry 之间的
    // gRPC 适配边界。它接收 RegisterNode / HeartbeatNode / DiscoverMetadata /
    // DiscoverStorage / GetClusterView 请求，并在 T019 中把 proto 字段映射到
    // registry 类型，再把 registry 结果映射回 proto 响应。
    //
    // 该 adapter 不是 Raft membership authority，不决定对象 COMMITTED 可见性，
    // 不读写 StorageNode payload，也不承载 view_node_app 启动逻辑。
    class ViewNodeServiceImpl final : public view::ViewNodeService::Service
    {
    public:
        using NowUnixMsFn = std::function<std::uint64_t()>;

        explicit ViewNodeServiceImpl(
            std::shared_ptr<ViewNodeRegistry> registry,
            ViewNodeServiceImplConfig config = {});
        ~ViewNodeServiceImpl() override = default;

        ViewNodeServiceImpl(const ViewNodeServiceImpl &) = delete;
        ViewNodeServiceImpl &operator=(const ViewNodeServiceImpl &) = delete;
        ViewNodeServiceImpl(ViewNodeServiceImpl &&) = delete;
        ViewNodeServiceImpl &operator=(ViewNodeServiceImpl &&) = delete;

        // 注册只接收 discovery / observation facts，不授予 membership authority。
        ::grpc::Status RegisterNode(::grpc::ServerContext *context,
                                    const ::view::RegisterNodeRequest *request,
                                    ::view::RegisterNodeResponse *response) override;

        // heartbeat 只刷新观测事实与 liveness，不改变 Raft quorum 或 leader 安全。
        ::grpc::Status HeartbeatNode(::grpc::ServerContext *context,
                                     const ::view::HeartbeatNodeRequest *request,
                                     ::view::HeartbeatNodeResponse *response) override;

        // 返回 MetadataNode 候选地址和 leader hint；Client 仍必须处理 NOT_LEADER。
        ::grpc::Status DiscoverMetadata(
            ::grpc::ServerContext *context,
            const ::view::DiscoverMetadataRequest *request,
            ::view::DiscoverMetadataResponse *response) override;

        // 返回 StorageNode endpoint / capacity / health 观测事实，不返回 payload。
        ::grpc::Status DiscoverStorage(
            ::grpc::ServerContext *context,
            const ::view::DiscoverStorageRequest *request,
            ::view::DiscoverStorageResponse *response) override;

        // 返回 cluster view 观测快照，供 status / diagnostics / tests 使用。
        ::grpc::Status GetClusterView(
            ::grpc::ServerContext *context,
            const ::view::GetClusterViewRequest *request,
            ::view::GetClusterViewResponse *response) override;

        [[nodiscard]] const std::shared_ptr<ViewNodeRegistry> &registry() const;
        [[nodiscard]] const ViewNodeServiceImplConfig &config() const;

    private:
        std::shared_ptr<ViewNodeRegistry> registry_;
        ViewNodeServiceImplConfig config_;
    };

} // namespace viewdemo
