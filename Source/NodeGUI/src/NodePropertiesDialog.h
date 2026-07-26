#pragma once

#include <NodeAPI/Graph.h>

#include <QDialog>

#include <map>
#include <string>

class QLabel;

namespace NodeGUI {

class NodeParameterWidget;

// Modal dialog for editing a node's parameters. Changes are applied to the
// graph immediately as the user edits each field.
class NodePropertiesDialog : public QDialog {
    Q_OBJECT

public:
    NodePropertiesDialog(NodeAPI::Graph& graph,
                         const NodeAPI::Node& node,
                         QWidget* parent = nullptr);

private:
    void BuildUI();

    NodeAPI::Graph& graph_;
    std::string nodeId_;
    const NodeAPI::NodeType* nodeType_ = nullptr;
    std::map<std::string, std::string> currentParameters_;

    NodeParameterWidget* parameterWidget_ = nullptr;
};

}  // namespace NodeGUI
