#pragma once

#include <NodeAPI/WireType.h>

#include <QtNodes/Definitions>

#include <QString>
#include <QWidget>

#include <cstddef>

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

class QCheckBox;
class QScrollArea;
class QComboBox;
class QFormLayout;
class QLineEdit;
class QVBoxLayout;

namespace NodeGUI {

class GraphScene;

// Right-dock panel that inspects the currently selected node: id (rename),
// type, timing domain, and parameters (value + wire-as-input toggle). Edits
// apply immediately through the same write paths as the rest of the editor.
// Parameter commits are validated against the type descriptor when the
// parameter declares a type (f32 numeric, boolean 0/1; free text otherwise):
// invalid edits revert to the last stored value with a red outline and a
// reason tooltip, valid commits flash the field briefly so they are visible.
class InspectorPanel : public QWidget {
    Q_OBJECT

public:
    explicit InspectorPanel(QWidget* parent = nullptr);

    void Attach(GraphScene* scene);

    // Rebuilds the form for the given node. ShowNode with InvalidNodeId (or
    // Clear()) shows the placeholder.
    void ShowNode(QtNodes::NodeId qtId);
    void Clear();

    // Report apply failures (wired to MainWindow's toast).
    std::function<void(const QString&)> onError;

private:
    struct ParamRow {
        std::string name;
        QLineEdit* edit = nullptr;
        QCheckBox* wireInput = nullptr;
        // Declared type from the node-type descriptor (empty = free text,
        // no validation claim). Drives per-commit validation.
        std::optional<NodeAPI::WireType> declaredType;
        QString baseToolTip;  // Tooltip without any inline error affix.
    };

    void Rebuild();
    void ApplyParameters();
    void ApplyParameterInputs();
    void ApplyDomain();
    void ApplyRename();
    void ApplyExcluded(bool exclude);
    void ApplyExcludedRecursive(bool exclude);

    // Inline commit feedback on one parameter row: rejected edits get a red
    // outline + reason tooltip (and the text is reverted by the caller);
    // accepted edits get a ~1 s palette-tinted background flash.
    void MarkRowInvalid(std::size_t row, const QString& reason);
    void ClearRowState(std::size_t row);
    void FlashRowCommitted(std::size_t row);

    GraphScene* scene_ = nullptr;
    QtNodes::NodeId qtId_ = QtNodes::InvalidNodeId;

    QVBoxLayout* layout_ = nullptr;
    QWidget* placeholder_ = nullptr;
    QWidget* form_ = nullptr;
    QScrollArea* scrollArea_ = nullptr;
    QLineEdit* idEdit_ = nullptr;
    QComboBox* domainCombo_ = nullptr;
    QCheckBox* excludeCheck_ = nullptr;
    QCheckBox* excludeChildrenCheck_ = nullptr;
    std::vector<ParamRow> paramRows_;
};

}  // namespace NodeGUI
