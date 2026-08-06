#include <RTEAutomation/CachePaths.h>

#include <gtest/gtest.h>

TEST(CachePaths, StableProjectIdAndLayout) {
    const auto first = RTEAutomation::WorkspaceForGraph(
        "project/graph.json", "Release", "/tmp/rte-cache-test");
    const auto second = RTEAutomation::WorkspaceForGraph(
        "project/graph.json", "Release", "/tmp/rte-cache-test");
    EXPECT_EQ(first.projectId, second.projectId);
    EXPECT_EQ(first.projectId.size(), 16u);
    EXPECT_EQ(first.build.filename(), "Release");
    EXPECT_EQ(first.generated.filename(), "generated");
    EXPECT_EQ(first.manifest.filename(), "manifest.json");
}
