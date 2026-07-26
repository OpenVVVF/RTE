#pragma once

#include <NodeAPI/NodeType.h>

#include <QWidget>

#include <functional>
#include <map>
#include <memory>
#include <string>

class QFormLayout;
class QLineEdit;

namespace NodeGUI {

// Editable form for a single node's parameters. Each parameter type defined by
// the node's type gets a labeled line edit. Changes are reported through the
// OnParameterChanged callback.
class NodeParameterWidget : public QWidget {
    Q_OBJECT

public:
    using ParameterChangedCallback =
        std::function<void(const std::string& paramName, const std::string& value)>;

    NodeParameterWidget(const NodeAPI::NodeType& nodeType,
                        const std::map<std::string, std::string>& currentParameters,
                        ParameterChangedCallback onChanged,
                        QWidget* parent = nullptr);

private:
    void BuildForm();
    void OnParameterChanged(const std::string& paramName, const QString& value);

    const NodeAPI::NodeType& nodeType_;
    const std::map<std::string, std::string>& currentParameters_;
    ParameterChangedCallback onChanged_;

    // Keep parameter name -> editor mapping for possible future use.
    std::map<std::string, QLineEdit*> editors_;
};

}  // namespace NodeGUI
