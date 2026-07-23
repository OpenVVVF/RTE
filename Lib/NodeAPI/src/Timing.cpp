#include "NodeAPI/Timing.h"

#include <algorithm>
#include <queue>
#include <sstream>
#include <unordered_map>

namespace NodeAPI::Timing {

ValidationResult Validator::Validate(const Graph& graph) const {
    ValidationResult result;

    const auto domainResult = CheckDomains(graph);
    if (!domainResult.ok) {
        result.ok = false;
        result.errors.insert(result.errors.end(), domainResult.errors.begin(), domainResult.errors.end());
    }

    // Entry points must not have incoming connections — they are the roots of
    // their timing domain.
    if (result.ok) {
        const auto entryResult = CheckEntryPoints(graph);
        if (!entryResult.ok) {
            result.ok = false;
            result.errors.insert(result.errors.end(), entryResult.errors.begin(), entryResult.errors.end());
        }
    }

    // Only check cycles if domains are consistent — cross-domain connections
    // would otherwise pollute the topological ordering.
    if (result.ok) {
        const auto cycleResult = CheckCycles(graph);
        if (!cycleResult.ok) {
            result.ok = false;
            result.errors.insert(result.errors.end(), cycleResult.errors.begin(), cycleResult.errors.end());
        }
    }

    return result;
}

ValidationResult Validator::CheckDomains(const Graph& graph) const {
    ValidationResult result;

    for (const auto& node : graph.GetNodes()) {
        if (node.domain.empty()) {
            result.AddError("node '" + node.id + "' has no timing domain assigned");
        }
    }

    for (const auto& connection : graph.GetConnections()) {
        const auto fromNode = graph.FindNode(connection.from.nodeId);
        const auto toNode = graph.FindNode(connection.to.nodeId);
        if (!fromNode || !toNode) continue;  // Structural problem; Graph already rejects this.

        if (!fromNode->domain.empty() && !toNode->domain.empty() &&
            fromNode->domain != toNode->domain) {
            result.AddError("connection '" + connection.id + "' connects domain '" +
                            fromNode->domain + "' (node '" + connection.from.nodeId +
                            "') to domain '" + toNode->domain + "' (node '" + connection.to.nodeId +
                            "'); cross-timestep connections are not allowed");
        }
    }

    for (const auto& bridge : graph.GetBridges()) {
        const auto producerNode = graph.FindNode(bridge.producer.nodeId);
        const auto consumerNode = graph.FindNode(bridge.consumer.nodeId);
        if (!producerNode || !consumerNode) continue;  // Structural problem; Graph already rejects this.

        if (!producerNode->domain.empty() && !consumerNode->domain.empty() &&
            producerNode->domain == consumerNode->domain) {
            result.AddError("bridge '" + bridge.id + "' connects node '" + bridge.producer.nodeId +
                            "' to node '" + bridge.consumer.nodeId + "' within domain '" +
                            producerNode->domain + "'; use a connection for same-domain links");
        }
    }

    return result;
}

ValidationResult Validator::CheckEntryPoints(const Graph& graph) const {
    ValidationResult result;

    std::unordered_map<std::string, std::size_t> inDegree;
    for (const auto& node : graph.GetNodes()) {
        inDegree[node.id] = 0;
    }
    for (const auto& connection : graph.GetConnections()) {
        ++inDegree[connection.to.nodeId];
    }
    for (const auto& bridge : graph.GetBridges()) {
        ++inDegree[bridge.consumer.nodeId];
    }

    for (const auto& node : graph.GetNodes()) {
        const auto nodeType = graph.FindNodeType(node.type);
        if (!nodeType) continue;

        if (nodeType->isEntryPoint && inDegree[node.id] > 0) {
            result.AddError("entry-point node '" + node.id + "' (type '" + node.type +
                            "') has incoming connections; entry points must be roots of their timing domain");
        }
    }

    return result;
}

ValidationResult Validator::CheckCycles(const Graph& graph) const {
    ValidationResult result;

    std::unordered_map<std::string, std::vector<std::string>> adjacency;
    std::unordered_map<std::string, std::size_t> inDegree;

    for (const auto& node : graph.GetNodes()) {
        inDegree[node.id] = 0;
    }

    for (const auto& connection : graph.GetConnections()) {
        adjacency[connection.from.nodeId].push_back(connection.to.nodeId);
        ++inDegree[connection.to.nodeId];
    }
    for (const auto& bridge : graph.GetBridges()) {
        adjacency[bridge.producer.nodeId].push_back(bridge.consumer.nodeId);
        ++inDegree[bridge.consumer.nodeId];
    }

    std::queue<std::string> queue;
    for (const auto& [id, degree] : inDegree) {
        if (degree == 0) queue.push(id);
    }

    std::size_t processed = 0;
    while (!queue.empty()) {
        const auto current = queue.front();
        queue.pop();
        ++processed;

        for (const auto& next : adjacency[current]) {
            if (--inDegree[next] == 0) {
                queue.push(next);
            }
        }
    }

    if (processed != inDegree.size()) {
        std::vector<std::string> cycleNodes;
        for (const auto& [id, degree] : inDegree) {
            if (degree > 0) cycleNodes.push_back(id);
        }
        std::sort(cycleNodes.begin(), cycleNodes.end());

        std::ostringstream message;
        message << "graph contains a directed cycle involving nodes:";
        for (const auto& id : cycleNodes) message << " " << id;
        result.AddError(message.str());
    }

    return result;
}

}  // namespace NodeAPI::Timing
