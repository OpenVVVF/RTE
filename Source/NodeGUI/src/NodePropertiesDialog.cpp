#include "NodePropertiesDialog.h"

#include "NodeParameterWidget.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QVBoxLayout>

namespace NodeGUI {

NodePropertiesDialog::NodePropertiesDialog(NodeAPI::Graph& graph,
                                           const NodeAPI::Node& node,
                                           QWidget* parent)
    : QDialog(parent)
    , graph_(graph)
    , nodeId_(node.id) {
    nodeType_ = nullptr;
    if (const auto type = graph_.FindNodeType(node.type)) {
        nodeType_ = &(*type);
    }

    BuildUI();
}

void NodePropertiesDialog::BuildUI() {
    setWindowTitle(QStringLiteral("Properties: %1").arg(QString::fromStdString(nodeId_)));
    setMinimumWidth(320);

    auto* layout = new QVBoxLayout(this);

    if (nodeType_) {
        auto* header = new QLabel(QStringLiteral("Type: %1").arg(QString::fromStdString(nodeType_->id)),
                                  this);
        layout->addWidget(header);
    }

    if (const auto node = graph_.FindNode(nodeId_)) {
        currentParameters_ = node->parameters;
    }

    if (nodeType_ && !currentParameters_.empty()) {
        parameterWidget_ = new NodeParameterWidget(
            *nodeType_,
            currentParameters_,
            [this](const std::string& paramName, const std::string& value) {
                auto& nodes = const_cast<std::vector<NodeAPI::Node>&>(graph_.GetNodes());
                for (auto& node : nodes) {
                    if (node.id == nodeId_) {
                        node.parameters[paramName] = value;
                        return;
                    }
                }
            },
            this);
        layout->addWidget(parameterWidget_);
    } else {
        auto* noneLabel = new QLabel(QStringLiteral("No editable properties."), this);
        layout->addWidget(noneLabel);
    }

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

}  // namespace NodeGUI
