// M0 主机侧测试入口（M0-A / M0-B1 / M0-B2 / M0-C）。
#include <cstdio>

#include "test_util.h"

void runCrcTests();
void runPacketTests();
void runEncoderTests();
void runDecoderTests();
void runFrameAssemblerTests();
void runPipelineTests();
void runFramePipelineTests();
void runProtocolEndpointTests();
void runEndpointConcurrencyTests();
void runCapabilitiesTests();
void runPhysicalPreviewTests();
void runOledPreviewTests();
void runStreamingEncoderTests();
void runInputTests();
void runLvglAdapterTests();
void runRuntimeStatsTests();
void runRemoteDisplayTests();
void runDisplayRouterTests();
void runDisplayUiStateTests();
void runPhysicalStatusTests();
void runPhysicalCapabilitySnapshotTests();
void runSplitStateTests();
void runTransportManagerTests();
void runTransportSinkTests();
void runTransportPipelineTests();
void runOledTests();
void runOledStatusTests();
void runPhysicalRendererTests();
void runPhysicalPreviewStateTests();

int main() {
    // 无缓冲：测试名实时输出，便于定位挂起/超时位置（与 pc 测试工具一致）。
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("== ESPView shared/protocol host tests ==\n");
    std::printf("[crc]\n");
    runCrcTests();
    std::printf("[packet]\n");
    runPacketTests();
    std::printf("[encoder]\n");
    runEncoderTests();
    std::printf("[decoder]\n");
    runDecoderTests();
    std::printf("[frame_assembler]\n");
    runFrameAssemblerTests();
    std::printf("[pipeline]\n");
    runPipelineTests();
    std::printf("[frame_pipeline]\n");
    runFramePipelineTests();
    std::printf("[protocol_endpoint]\n");
    runProtocolEndpointTests();
    std::printf("[endpoint_concurrency]\n");
    runEndpointConcurrencyTests();
    std::printf("[capabilities]\n");
    runCapabilitiesTests();
    std::printf("[physical_preview]\n");
    runPhysicalPreviewTests();
    std::printf("[oled_preview]\n");
    runOledPreviewTests();
    std::printf("[streaming_encoder]\n");
    runStreamingEncoderTests();
    std::printf("[input]\n");
    runInputTests();
    runLvglAdapterTests();
    std::printf("[runtime_stats]\n");
    runRuntimeStatsTests();
    std::printf("[remote_display]\n");
    runRemoteDisplayTests();
    std::printf("[display_router]\n");
    runDisplayRouterTests();
    std::printf("[display_ui_state]\n");
    runDisplayUiStateTests();
    std::printf("[physical_status]\n");
    runPhysicalStatusTests();
    runPhysicalCapabilitySnapshotTests();
    std::printf("[split_state]\n");
    runSplitStateTests();
    std::printf("[transport_manager]\n");
    runTransportManagerTests();
    std::printf("[transport_sink]\n");
    runTransportSinkTests();
    std::printf("[transport_pipeline]\n");
    runTransportPipelineTests();
    std::printf("[oled]\n");
    runOledTests();
    std::printf("[oled_status]\n");
    runOledStatusTests();
    std::printf("[physical_renderer]\n");
    runPhysicalRendererTests();
    std::printf("[physical_preview_state]\n");
    runPhysicalPreviewStateTests();
    std::printf("----\nchecks: %d, failures: %d\n", espview::proto::test::gChecks,
                espview::proto::test::gFailures);
    return espview::proto::test::gFailures == 0 ? 0 : 1;
}
