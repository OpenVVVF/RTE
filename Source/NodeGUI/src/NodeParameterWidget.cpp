#include "NodeParameterWidget.h"

#include <NodeAPI/WireType.h>

#include <QFormLayout>
#include <QLineEdit>

namespace NodeGUI {

NodeParameterWidget::NodeParameterWidget(const NodeAPI::NodeType& nodeType,
                                         const std::map<std::string, std::string>& currentParameters,
                                         ParameterChangedCallback onChanged,
                                         QWidget* parent)
    : QWidget(parent)
    , nodeType_(nodeType)
    , currentParameters_(currentParameters)
    , onChanged_(std::move(onChanged)) {
    BuildForm();
}

void NodeParameterWidget::BuildForm() {
    auto* layout = new QFormLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(4);

    for (const auto& [paramName, paramType] : nodeType_.parameterTypes) {
        auto* editor = new QLineEdit(this);
        editor->setPlaceholderText(
            QStringLiteral("<%1>").arg(QString::fromStdString(NodeAPI::ToString(paramType.dtype))));

        const auto it = currentParameters_.find(paramName);
        if (it != currentParameters_.end()) {
            editor->setText(QString::fromStdString(it->second));
        }

        connect(editor, &QLineEdit::editingFinished, [this, paramName, editor]() {
            OnParameterChanged(paramName, editor->text());
        });

        layout->addRow(QString::fromStdString(paramName), editor);
        editors_[paramName] = editor;
    }

    // Also show instance parameters that are not declared by the type, if any.
    for (const auto& [paramName, value] : currentParameters_) {
        if (editors_.count(paramName)) {
            continue;
        }

        auto* editor = new QLineEdit(this);
        editor->setText(QString::fromStdString(value));

        connect(editor, &QLineEdit::editingFinished, [this, paramName, editor]() {
            OnParameterChanged(paramName, editor->text());
        });

        layout->addRow(QString::fromStdString(paramName + " *"), editor);
        editors_[paramName] = editor;
    }
}

void NodeParameterWidget::OnParameterChanged(const std::string& paramName, const QString& value) {
    if (onChanged_) {
        onChanged_(paramName, value.toStdString());
    }
}

}  // namespace NodeGUI
