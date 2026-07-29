#pragma once

#include "ParameterBlock.h"

#include <NodeAPI/Graph.h>

#include <QtNodes/NodeData>
#include <QtNodes/NodeDelegateModel>

#include <memory>
#include <set>
#include <vector>

namespace NodeGUI {

// Minimal NodeData payload used only so QtNodes can compare port types.
class TypedNodeData : public QtNodes::NodeData {
public:
    explicit TypedNodeData(QString typeId, QString typeName);

    QtNodes::NodeDataType type() const override;

private:
    QtNodes::NodeDataType type_;
};

// The input ports a node instance actually shows: the type's input ports,
// except optional ports that share a name with a parameter and are neither
// wired nor connected (those fall back to the parameter), then synthesized
// parameter input ports not already covered by a type port.
// GraphScene::FindPortIndex and NodeGraphModel::MakePortRef use this same
// ordering.
std::vector<NodeAPI::Port> VisibleInputPorts(
    const NodeAPI::NodeType& nodeType,
    const std::vector<std::string>& parameterInputs,
    const std::set<std::string>& connectedInputs);

// Names of optional, parameter-backed input ports of this node that are the
// consumer of a connection or bridge.
std::set<std::string> ConnectedOptionalInputs(const NodeAPI::Graph& graph,
                                              const NodeAPI::Node& node,
                                              const NodeAPI::NodeType& nodeType);

// QtNodes delegate model that represents one NodeAPI node instance.
// The node type determines the port layout; the instance supplies the caption.
class NodeInstanceModel : public QtNodes::NodeDelegateModel {
    Q_OBJECT

public:
    NodeInstanceModel(NodeAPI::Node node, NodeAPI::NodeType nodeType);

    QString name() const override;
    QString caption() const override;

    unsigned int nPorts(QtNodes::PortType portType) const override;
    QtNodes::NodeDataType dataType(QtNodes::PortType portType,
                                   QtNodes::PortIndex portIndex) const override;

    void setInData(std::shared_ptr<QtNodes::NodeData> nodeData,
                   QtNodes::PortIndex const portIndex) override;

    std::shared_ptr<QtNodes::NodeData> outData(QtNodes::PortIndex const port) override;

    // No embedded widget: the parameter block is painted directly by
    // TypedNodePainter, which is far cheaper than a QGraphicsProxyWidget
    // per node.
    QWidget* embeddedWidget() override { return nullptr; }

    // Stores the node's parameters as pre-laid-out paint data. The block is
    // painted directly by TypedNodePainter (no embedded widget, for
    // performance) and ParameterNodeGeometry reserves the space for it.
    // Emits requestNodeUpdate() so the scene refreshes geometry and repaints.
    void SetParameters(std::map<std::string, std::string> parameters);

    const ParameterBlockData& ParameterBlock() const { return parameterBlock_; }

    // Sets the timing domain shown in the caption ("[domain]" second line).
    // Emits requestNodeUpdate() so the scene repaints.
    void SetDomain(std::string domain);

    // Marks parameters as wireable input ports (or back to constants). The
    // synthesized ports are appended after the type's input ports; wired
    // parameters leave the painted parameter panel. Emits portsInserted()/
    // portsDeleted() and requestNodeUpdate() so the scene re-lays out.
    void SetParameterInputs(std::vector<std::string> parameterInputs);

    const std::vector<std::string>& ParameterInputs() const { return parameterInputs_; }

    // Updates which optional, parameter-backed input ports currently have a
    // connection. Those stay visible even when their parameter is not wired;
    // unwired and unconnected ones are hidden (the parameter is used
    // instead). No-ops when unchanged.
    void SetConnectedInputs(std::set<std::string> connectedInputs);

    QString portCaption(QtNodes::PortType portType,
                        QtNodes::PortIndex portIndex) const override;

    bool portCaptionVisible(QtNodes::PortType portType,
                            QtNodes::PortIndex portIndex) const override;

    QtNodes::ConnectionPolicy portConnectionPolicy(QtNodes::PortType portType,
                                                   QtNodes::PortIndex portIndex) const override;

private:
    const NodeAPI::Node node_;
    const NodeAPI::NodeType nodeType_;

    // Cached order of ports so port indices are stable.
    std::vector<NodeAPI::Port> inputPorts_;
    std::vector<NodeAPI::Port> outputPorts_;

    std::shared_ptr<QtNodes::NodeData> outputData_;

    ParameterBlockData parameterBlock_;
    std::string domain_;

    std::map<std::string, std::string> parameters_;
    std::vector<std::string> parameterInputs_;
    std::set<std::string> connectedInputs_;

    // Rebuilds inputPorts_ from the type's ports plus the synthesized
    // parameter input ports, and refreshes the painted parameter block.
    void RebuildPortsAndBlock();
};

}  // namespace NodeGUI
