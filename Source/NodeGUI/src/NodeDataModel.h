#pragma once

#include <NodeAPI/Graph.h>

#include <QtNodes/NodeData>
#include <QtNodes/NodeDelegateModel>

#include <memory>
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

    QWidget* embeddedWidget() override { return parameterWidget_; }

    // Builds (or refreshes) the read-only parameter view embedded inside the
    // node. The first call must happen before the scene constructs the node's
    // graphics object, which is when embeddedWidget() is queried; later calls
    // rebuild the rows in place. An empty map hides the panel.
    void SetParameters(const std::map<std::string, std::string>& parameters,
                       const std::map<std::string, NodeAPI::WireType>& parameterTypes);

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

    // Read-only parameter view. Ownership transfers to the scene's
    // QGraphicsProxyWidget once it is embedded.
    QWidget* parameterWidget_ = nullptr;
};

}  // namespace NodeGUI
