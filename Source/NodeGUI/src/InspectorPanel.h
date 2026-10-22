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
class QLabel;
class QLineEdit;
class QPushButton;
class QVBoxLayout;

namespace NodeGUI {

class GraphScene;

// Right-dock panel that inspects the current selection. A selected node
// shows: id (rename), type, timing domain, and parameters (value +
// wire-as-input toggle). A selected CONNECTION shows its display-only net
// label (KiCad-style: the long wire collapses into two named stubs).
// Edits apply immediately through the same write paths as the rest of the
// editor. Parameter commits are validated against the type descriptor when
// the parameter declares a type (f32 numeric, boolean 0/1; free text
// otherwise): invalid edits revert to the last stored value with a red
// outline and a reason tooltip, valid commits flash the field briefly so they
// are visible.
class InspectorPanel : public QWidget {
    Q_OBJECT

public:
    explicit InspectorPanel(QWidget* parent = nullptr);

    void Attach(GraphScene* scene);

    // Rebuilds the form for the given node. ShowNode with InvalidNodeId (or
    // Clear()) shows the placeholder.
    void ShowNode(QtNodes::NodeId qtId);
    // Rebuilds the form for the given connection (net label editor).
    void ShowConnection(QtNodes::ConnectionId connId);
    // Updates the net-label field when an OUTSIDE path (Ctrl+double-click
    // dialog, smoke verb) committed a label while this connection is shown,
    // so the field never displays a stale name.
    void RefreshNetLabel(QtNodes::ConnectionId connId, const QString& text);
    // Same outside-path refresh for the "Reset label position" button's
    // enabled state (an outside chip drag/reset committed while shown).
    void RefreshChipReset(QtNodes::ConnectionId connId, bool hasOffset);
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
    void RebuildConnectionForm();
    void ApplyParameters();
    void ApplyParameterInputs();
    void ApplyDomain();
    void ApplyRename();
    void ApplyExcluded(bool exclude);
    void ApplyExcludedRecursive(bool exclude);
    void ApplyNetLabel();

    // Inline commit feedback on one parameter row: rejected edits get a red
    // outline + reason tooltip (and the text is reverted by the caller);
    // accepted edits get a ~1 s palette-tinted background flash.
    void MarkRowInvalid(std::size_t row, const QString& reason);
    void ClearRowState(std::size_t row);
    void FlashRowCommitted(std::size_t row);

    GraphScene* scene_ = nullptr;
    QtNodes::NodeId qtId_ = QtNodes::InvalidNodeId;
    // When connectionMode_ is set the form inspects connId_ (net label),
    // not qtId_ (which stays InvalidNodeId in that mode).
    bool connectionMode_ = false;
    QtNodes::ConnectionId connId_{QtNodes::InvalidNodeId,
                                  QtNodes::InvalidPortIndex,
                                  QtNodes::InvalidNodeId,
                                  QtNodes::InvalidPortIndex};

    QVBoxLayout* layout_ = nullptr;
    QWidget* placeholder_ = nullptr;
    QWidget* form_ = nullptr;
    QScrollArea* scrollArea_ = nullptr;
    QLineEdit* idEdit_ = nullptr;
    QLineEdit* netLabelEdit_ = nullptr;
    QPushButton* chipResetButton_ = nullptr;
    QComboBox* domainCombo_ = nullptr;
    QCheckBox* excludeCheck_ = nullptr;
    QCheckBox* excludeChildrenCheck_ = nullptr;
    std::vector<ParamRow> paramRows_;
};

}  // namespace NodeGUI
