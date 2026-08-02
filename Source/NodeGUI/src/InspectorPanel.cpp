#include "InspectorPanel.h"

#include "GraphScene.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>

#include <algorithm>
#include <set>

namespace NodeGUI {

namespace {

constexpr char kNoDomainLabel[] = "(no domain)";

}  // namespace

InspectorPanel::InspectorPanel(QWidget* parent)
    : QWidget(parent) {
    layout_ = new QVBoxLayout(this);
    layout_->setContentsMargins(8, 8, 8, 8);
    layout_->setAlignment(Qt::AlignTop);

    placeholder_ = new QLabel(QStringLiteral("Select a node to inspect it."), this);
    layout_->addWidget(placeholder_);

    Clear();
}

void InspectorPanel::Attach(GraphScene* scene) {
    scene_ = scene;
}

void InspectorPanel::Clear() {
    ShowNode(QtNodes::InvalidNodeId);
}

void InspectorPanel::ShowNode(QtNodes::NodeId qtId) {
    if (qtId == qtId_) {
        return;
    }
    qtId_ = qtId;
    Rebuild();
}

void InspectorPanel::Rebuild() {
    paramRows_.clear();
    idEdit_ = nullptr;
    domainCombo_ = nullptr;

    delete form_;
    form_ = nullptr;
    placeholder_->hide();

    if (!scene_ || qtId_ == QtNodes::InvalidNodeId) {
        placeholder_->show();
        return;
    }

    const std::string nodeId = scene_->NodeApiId(qtId_);
    const auto node = nodeId.empty() ? std::nullopt : scene_->Graph().FindNode(nodeId);
    if (!node) {
        placeholder_->show();
        qtId_ = QtNodes::InvalidNodeId;
        return;
    }
    const auto nodeType = scene_->Graph().FindNodeType(node->type);

    form_ = new QWidget(this);
    auto* formLayout = new QFormLayout(form_);
    formLayout->setContentsMargins(0, 0, 0, 0);

    // Id (editable: rename).
    idEdit_ = new QLineEdit(QString::fromStdString(nodeId), form_);
    formLayout->addRow(QStringLiteral("Id"), idEdit_);
    connect(idEdit_, &QLineEdit::editingFinished, this, &InspectorPanel::ApplyRename);

    // Type (read-only).
    auto* typeValue = new QLabel(QString::fromStdString(node->type), form_);
    formLayout->addRow(QStringLiteral("Type"), typeValue);

    if (nodeType && !nodeType->description.empty()) {
        auto* description =
            new QLabel(QString::fromStdString(nodeType->description), form_);
        description->setWordWrap(true);
        description->setTextInteractionFlags(Qt::TextSelectableByMouse);
        description->setStyleSheet(
            QStringLiteral("QLabel { color: palette(text); "
                           "background: palette(alternate-base); "
                           "border-radius: 4px; padding: 7px; }"));
        formLayout->addRow(description);
        typeValue->setToolTip(QString::fromStdString(nodeType->description));
    }

    // Domain (combo of existing domains + "(no domain)"; locked types show a
    // disabled row).
    domainCombo_ = new QComboBox(form_);
    domainCombo_->addItem(QString::fromStdString(kNoDomainLabel));
    {
        std::set<std::string> domains;
        for (const auto& n : scene_->Graph().GetNodes()) {
            if (!n.domain.empty()) {
                domains.insert(n.domain);
            }
        }
        for (const auto& domain : domains) {
            domainCombo_->addItem(QString::fromStdString(domain));
        }
    }
    const bool locked = nodeType && !nodeType->domain.empty();
    if (locked) {
        domainCombo_->addItem(QString::fromStdString(nodeType->domain));
        domainCombo_->setCurrentText(QString::fromStdString(nodeType->domain));
        domainCombo_->setEnabled(false);
        domainCombo_->setToolTip(QStringLiteral("Domain is locked by the node type"));
    } else {
        domainCombo_->setCurrentText(node->domain.empty()
                                         ? QString::fromStdString(kNoDomainLabel)
                                         : QString::fromStdString(node->domain));
    }
    formLayout->addRow(QStringLiteral("Domain"), domainCombo_);
    connect(domainCombo_, &QComboBox::activated, this, &InspectorPanel::ApplyDomain);

    // Exclude from compile: node stays in the graph but is skipped by codegen.
    excludeCheck_ = new QCheckBox(QStringLiteral("Exclude from compile"), form_);
    excludeCheck_->setChecked(node->excludeFromCompile);
    excludeCheck_->setToolTip(QStringLiteral(
        "The node stays visible in the graph (greyed out) but is skipped by "
        "the code emitter together with its connections."));
    formLayout->addRow(QStringLiteral("Compile"), excludeCheck_);
    connect(excludeCheck_, &QCheckBox::toggled, this, &InspectorPanel::ApplyExcluded);

    // Parameters: value editor + wire-as-input toggle per row.
    if (!node->parameters.empty()) {
        auto* header = new QLabel(QStringLiteral("Parameters"), form_);
        QFont font = header->font();
        font.setBold(true);
        header->setFont(font);
        formLayout->addRow(header);
    }
    for (const auto& [name, value] : node->parameters) {
        auto* edit = new QLineEdit(QString::fromStdString(value), form_);
        QString parameterToolTip;
        if (nodeType) {
            if (const auto type = nodeType->FindParameterType(name)) {
                const std::string typeText = NodeAPI::ToString(type->quantity) + "."
                                             + NodeAPI::ToString(type->frame) + "."
                                             + NodeAPI::ToString(type->dtype);
                parameterToolTip = QString::fromStdString(typeText);
            }
            if (const auto description =
                    nodeType->FindParameterDescription(name);
                description && !description->empty()) {
                parameterToolTip =
                    QString::fromStdString(*description)
                    + (parameterToolTip.isEmpty()
                           ? QString{}
                           : QStringLiteral("\nType: ") + parameterToolTip);
            }
        }
        edit->setToolTip(parameterToolTip);

        auto* wireInput = new QCheckBox(QStringLiteral("wire"), form_);
        const bool wired = std::find(node->parameterInputs.begin(),
                                     node->parameterInputs.end(),
                                     name) != node->parameterInputs.end();
        wireInput->setChecked(wired);
        edit->setEnabled(!wired);
        wireInput->setToolTip(parameterToolTip);

        auto* rowWidget = new QWidget(form_);
        auto* rowLayout = new QHBoxLayout(rowWidget);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->addWidget(edit);
        rowLayout->addWidget(wireInput);
        rowWidget->setToolTip(parameterToolTip);
        auto* rowLabel =
            new QLabel(QString::fromStdString(name), form_);
        rowLabel->setToolTip(parameterToolTip);
        formLayout->addRow(rowLabel, rowWidget);

        paramRows_.push_back({name, edit, wireInput});
        connect(edit, &QLineEdit::editingFinished, this, &InspectorPanel::ApplyParameters);
        connect(wireInput, &QCheckBox::toggled, this, &InspectorPanel::ApplyParameterInputs);
    }

    layout_->addWidget(form_);
}

void InspectorPanel::ApplyRename() {
    if (!scene_ || !idEdit_) {
        return;
    }
    const std::string newId = idEdit_->text().trimmed().toStdString();
    if (newId.empty() || newId == scene_->NodeApiId(qtId_)) {
        return;
    }
    const QString error = scene_->RenameNode(qtId_, newId);
    if (!error.isEmpty()) {
        if (onError) onError(error);
        // Revert the editor to the actual id.
        idEdit_->setText(QString::fromStdString(scene_->NodeApiId(qtId_)));
    }
}

void InspectorPanel::ApplyDomain() {
    if (!scene_ || !domainCombo_ || !domainCombo_->isEnabled()) {
        return;
    }
    const QString text = domainCombo_->currentText();
    const std::string domain =
        (text == kNoDomainLabel) ? std::string{} : text.toStdString();
    const QString error = scene_->SetNodeDomain(qtId_, domain);
    if (!error.isEmpty() && onError) {
        onError(error);
    }
}

void InspectorPanel::ApplyExcluded(bool exclude) {
    if (!scene_) {
        return;
    }
    const QString error = scene_->SetNodeExcluded(qtId_, exclude);
    if (!error.isEmpty() && onError) {
        onError(error);
    }
}

void InspectorPanel::ApplyParameters() {
    if (!scene_) {
        return;
    }
    std::map<std::string, std::string> updated;
    for (const auto& row : paramRows_) {
        updated[row.name] = row.edit->text().toStdString();
    }
    const QString error = scene_->SetNodeParameters(qtId_, updated);
    if (!error.isEmpty() && onError) {
        onError(error);
    }
}

void InspectorPanel::ApplyParameterInputs() {
    if (!scene_) {
        return;
    }
    std::vector<std::string> wired;
    for (const auto& row : paramRows_) {
        row.edit->setEnabled(!row.wireInput->isChecked());
        if (row.wireInput->isChecked()) {
            wired.push_back(row.name);
        }
    }
    const QString error = scene_->SetNodeParameterInputs(qtId_, wired);
    if (!error.isEmpty() && onError) {
        onError(error);
    }
}

}  // namespace NodeGUI
