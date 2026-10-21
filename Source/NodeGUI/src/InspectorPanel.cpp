#include "InspectorPanel.h"

#include "GraphScene.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPalette>
#include <QPointer>
#include <QScrollArea>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <set>

namespace NodeGUI {

namespace {

constexpr char kNoDomainLabel[] = "(no domain)";

// Commit-flash duration: long enough to register, short enough to not be
// mistaken for a persistent state.
constexpr int kCommitFlashDurationMs = 1000;

// Qt palettes carry no error role, so red is adapted to the base luminance:
// lighter red on dark themes (incl. themeMode 2), deeper red on light ones.
QColor ErrorColor(const QPalette& palette) {
    return palette.color(QPalette::Base).lightness() < 128
               ? QColor(255, 104, 104)
               : QColor(192, 0, 0);
}

// Commit-flash tint: the editor's Base washed toward Highlight — derived
// from the live palette, so it tracks both light and dark themes instead of
// hard-coding a hue that clashes with one of them.
QColor CommitFlashColor(const QPalette& palette) {
    const QColor base = palette.color(QPalette::Base);
    const QColor highlight = palette.color(QPalette::Highlight);
    constexpr double kBaseWeight = 0.78;
    return QColor::fromRgbF(kBaseWeight * base.redF() + (1.0 - kBaseWeight) * highlight.redF(),
                            kBaseWeight * base.greenF() + (1.0 - kBaseWeight) * highlight.greenF(),
                            kBaseWeight * base.blueF() + (1.0 - kBaseWeight) * highlight.blueF());
}

// Validates a committed parameter text against the declared descriptor type.
// Returns empty when acceptable or when no validation claim can be made
// (parameter without a declared type, or a free-text (string) parameter).
// Booleans accept the literal 0/1 the codegen emits; f32 requires the whole
// trimmed text to parse as a finite float.
QString ParameterValidationError(const std::optional<NodeAPI::WireType>& declaredType,
                                 const QString& text) {
    if (!declaredType) {
        return QString{};
    }
    const QString trimmed = text.trimmed();
    if (declaredType->quantity == NodeAPI::Quantity::Boolean) {
        if (trimmed != QStringLiteral("0") && trimmed != QStringLiteral("1")) {
            return QStringLiteral("expected a boolean constant (0 or 1), got '%1'")
                .arg(trimmed);
        }
        return QString{};
    }
    if (declaredType->quantity == NodeAPI::Quantity::String) {
        return QString{};
    }
    switch (declaredType->dtype) {
        case NodeAPI::DType::F32: {
            bool ok = false;
            const float value = trimmed.toFloat(&ok);
            if (!ok || !std::isfinite(value)) {
                return QStringLiteral("expected a number (f32), got '%1'").arg(trimmed);
            }
            return QString{};
        }
    }
    return QString{};
}

}  // namespace

InspectorPanel::InspectorPanel(QWidget* parent)
    : QWidget(parent) {
    layout_ = new QVBoxLayout(this);
    layout_->setContentsMargins(8, 8, 8, 8);
    layout_->setAlignment(Qt::AlignTop);

    // Param-heavy forms overflow the dock without this wrap (clunk dossier);
    // the scroll area hosts the rebuilt form content and hides when empty.
    scrollArea_ = new QScrollArea(this);
    scrollArea_->setWidgetResizable(true);
    scrollArea_->setFrameShape(QFrame::NoFrame);
    scrollArea_->hide();
    layout_->addWidget(scrollArea_, 1);

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

    // Detach the old form from its scroll area before delete - the area
    // would otherwise point at freed memory until it's itself replaced.
    if (scrollArea_) {
        scrollArea_->takeWidget();
        scrollArea_->hide();
    }
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
        "the code emitter. Children fed by it receive a zero value."));
    formLayout->addRow(QStringLiteral("Compile"), excludeCheck_);
    connect(excludeCheck_, &QCheckBox::toggled, this, &InspectorPanel::ApplyExcluded);

    excludeChildrenCheck_ = new QCheckBox(QStringLiteral("and children"), form_);
    excludeChildrenCheck_->setChecked(node->excludeFromCompileRecursive);
    excludeChildrenCheck_->setToolTip(QStringLiteral(
        "Exclude this node and its whole downstream chain from compilation. "
        "Children are shown as excluded with this node named as the parent."));
    formLayout->addRow(QStringLiteral(""), excludeChildrenCheck_);
    connect(excludeChildrenCheck_, &QCheckBox::toggled, this,
            &InspectorPanel::ApplyExcludedRecursive);

    // Chain exclusion note: this node is excluded because a recursive-flagged
    // ancestor excludes it.
    const std::string excludedBy = scene_->ExclusionParent(nodeId);
    if (!excludedBy.empty()) {
        auto* note = new QLabel(
            QStringLiteral("Excluded by parent '%1'").arg(QString::fromStdString(excludedBy)),
            form_);
        note->setWordWrap(true);
        note->setStyleSheet(QStringLiteral("color: #ffb74d;"));
        formLayout->addRow(note);
    }

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
        // smoke-driver seam: param.<nodeId>.<paramName> for scripted edits
        edit->setObjectName(QStringLiteral("param.")
                            + QString::fromStdString(node->id)
                            + QStringLiteral(".")
                            + QString::fromStdString(name));
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

        ParamRow row;
        row.name = name;
        row.edit = edit;
        row.wireInput = wireInput;
        row.declaredType = nodeType ? nodeType->FindParameterType(name)
                                    : std::nullopt;
        row.baseToolTip = parameterToolTip;
        paramRows_.push_back(std::move(row));
        connect(edit, &QLineEdit::editingFinished, this, &InspectorPanel::ApplyParameters);
        connect(wireInput, &QCheckBox::toggled, this, &InspectorPanel::ApplyParameterInputs);
    }

    scrollArea_->setWidget(form_);
    scrollArea_->show();
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

void InspectorPanel::ApplyExcludedRecursive(bool exclude) {
    if (!scene_) {
        return;
    }
    const QString error = scene_->SetNodeExcludedRecursive(qtId_, exclude);
    if (!error.isEmpty() && onError) {
        onError(error);
    }
}

void InspectorPanel::ApplyParameters() {
    if (!scene_) {
        return;
    }
    const std::string nodeId = scene_->NodeApiId(qtId_);
    const auto node = nodeId.empty() ? std::nullopt : scene_->Graph().FindNode(nodeId);
    if (!node) {
        return;
    }

    // Start from the stored values; only rows whose text actually differs
    // from the stored value are validated and take part in the commit
    // (untouched rows are never flagged — fresh instances legitimately seed
    // empty fields). Invalid rows revert to the stored value, exactly like
    // the rename path reverts a rejected id. Disabled wire-driven rows are
    // not value sources.
    std::map<std::string, std::string> updated = node->parameters;
    bool anyChanged = false;
    std::vector<std::size_t> committedRows;
    for (std::size_t i = 0; i < paramRows_.size(); ++i) {
        ParamRow& row = paramRows_[i];
        if (!row.edit->isEnabled()) {
            continue;  // wire-driven row: the text is not a value source
        }
        const std::string& stored = node->parameters.at(row.name);
        const QString text = row.edit->text().trimmed();
        if (text.toStdString() == stored) {
            continue;  // untouched: no validation claim, no styling churn
        }
        const QString error = ParameterValidationError(row.declaredType, text);
        if (!error.isEmpty()) {
            row.edit->setText(QString::fromStdString(stored));
            MarkRowInvalid(i, error);
            continue;
        }
        ClearRowState(i);
        updated[row.name] = text.toStdString();
        anyChanged = true;
        committedRows.push_back(i);
    }

    // Revert-only (and re-committing the already stored values) leaves the
    // document untouched: skip the write path so undo history records
    // exactly one snapshot per real change.
    if (!anyChanged) {
        return;
    }
    const QString error = scene_->SetNodeParameters(qtId_, updated);
    if (!error.isEmpty()) {
        if (onError) onError(error);
        for (std::size_t i = 0; i < paramRows_.size(); ++i) {
            ParamRow& row = paramRows_[i];
            row.edit->setText(QString::fromStdString(node->parameters.at(row.name)));
            ClearRowState(i);
        }
        return;
    }
    for (const std::size_t row : committedRows) {
        FlashRowCommitted(row);
    }
}

void InspectorPanel::MarkRowInvalid(std::size_t row, const QString& reason) {
    ParamRow& paramRow = paramRows_[row];
    // Invalidate any pending flash restore so it cannot wipe the outline.
    paramRow.edit->setProperty("flashToken",
                               paramRow.edit->property("flashToken").toInt() + 1);
    paramRow.edit->setProperty("paramCommitted", false);
    paramRow.edit->setProperty("paramInvalid", true);
    paramRow.edit->setStyleSheet(
        QStringLiteral("QLineEdit { border: 2px solid %1; border-radius: 3px; }")
            .arg(ErrorColor(paramRow.edit->palette()).name()));
    paramRow.edit->setToolTip(reason + (paramRow.baseToolTip.isEmpty()
                                            ? QString{}
                                            : QStringLiteral("\n") + paramRow.baseToolTip));
}

void InspectorPanel::ClearRowState(std::size_t row) {
    ParamRow& paramRow = paramRows_[row];
    if (!paramRow.edit->property("paramInvalid").toBool()
        && !paramRow.edit->property("paramCommitted").toBool()) {
        return;  // nothing to clear: keep the default styling
    }
    // Invalidate any pending flash restore so it cannot fight the (re)set
    // styles below.
    paramRow.edit->setProperty("flashToken",
                               paramRow.edit->property("flashToken").toInt() + 1);
    paramRow.edit->setProperty("paramInvalid", false);
    paramRow.edit->setProperty("paramCommitted", false);
    paramRow.edit->setStyleSheet(QString());
    paramRow.edit->setToolTip(paramRow.baseToolTip);
}

void InspectorPanel::FlashRowCommitted(std::size_t row) {
    ParamRow& paramRow = paramRows_[row];
    QLineEdit* edit = paramRow.edit;
    edit->setProperty("paramInvalid", false);
    edit->setProperty("paramCommitted", true);
    const int token = edit->property("flashToken").toInt() + 1;
    edit->setProperty("flashToken", token);
    edit->setStyleSheet(QStringLiteral("QLineEdit { background-color: %1; }")
                            .arg(CommitFlashColor(edit->palette()).name()));
    QPointer<QLineEdit> guard(edit);
    QTimer::singleShot(kCommitFlashDurationMs, this, [guard, token]() {
        if (!guard || guard->property("flashToken").toInt() != token) {
            return;
        }
        guard->setProperty("paramCommitted", false);
        guard->setStyleSheet(QString());
    });
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
