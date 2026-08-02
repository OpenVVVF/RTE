#pragma once

#include <QtNodes/Definitions>

#include <QWidget>

#include <functional>
#include <map>
#include <string>
#include <vector>

class QCheckBox;
class QComboBox;
class QFormLayout;
class QLineEdit;
class QVBoxLayout;

namespace NodeGUI {

class GraphScene;

// Right-dock panel that inspects the currently selected node: id (rename),
// type, timing domain, and parameters (value + wire-as-input toggle). Edits
// apply immediately through the same write paths as the rest of the editor.
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
    };

    void Rebuild();
    void ApplyParameters();
    void ApplyParameterInputs();
    void ApplyDomain();
    void ApplyRename();
    void ApplyExcluded(bool exclude);

    GraphScene* scene_ = nullptr;
    QtNodes::NodeId qtId_ = QtNodes::InvalidNodeId;

    QVBoxLayout* layout_ = nullptr;
    QWidget* placeholder_ = nullptr;
    QWidget* form_ = nullptr;
    QLineEdit* idEdit_ = nullptr;
    QComboBox* domainCombo_ = nullptr;
    QCheckBox* excludeCheck_ = nullptr;
    std::vector<ParamRow> paramRows_;
};

}  // namespace NodeGUI
