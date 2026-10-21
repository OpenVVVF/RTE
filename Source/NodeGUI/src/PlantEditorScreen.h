#pragma once

#include <QtNodes/Definitions>

#include <QPointer>
#include <QWidget>

#include <functional>
#include <memory>
#include <string>
#include <vector>

class QAction;
class QLabel;
class QTimer;

namespace NodeGUI {

class GraphScene;
class GraphView;
class InspectorPanel;
class NodePalette;

// Self-contained Plant Editor screen: a first-class editor for plant
// nodegraphs (the HW side of the closed loop; sibling to the controller
// graphs handled by the Node Editor). Owns its own GraphScene (pinned to
// the single implicit domain 'plant'), GraphView, NodePalette (plant types
// only) and InspectorPanel instances; the Node Editor's singletons stay
// untouched. The MainWindow hosts this widget as the 'Plant Editor' tab and
// wraps PalettePanel()/InspectorPanelWidget() in its per-screen docks, the
// same way the runtime screen hands up its panels.
class PlantEditorScreen : public QWidget {
    Q_OBJECT

public:
    // plantTemplatesDir: Assets/PlantTemplates; templates are loaded right
    // away so the palette and node creation work before any file is opened.
    explicit PlantEditorScreen(std::string plantTemplatesDir,
                               QWidget* parent = nullptr);
    ~PlantEditorScreen() override;

    // Document lifecycle. The plant document keeps its own current path and
    // its own undo/redo history, independent of the Node Editor document.
    // Returns an empty string on success, otherwise an error message.
    QString OpenGraph(const std::string& path);
    QString NewGraph();
    QString SaveGraph(const std::string& path);
    const std::string& CurrentPath() const { return currentPath_; }

    void Undo();
    void Redo();
    bool CanUndo() const { return !undoHistory_.empty(); }
    bool CanRedo() const { return !redoHistory_.empty(); }
    void SetUndoHistoryLimit(int limit);

    QString StatusText() const;

    // Frame the canvas to the document content (mirrors the Node Editor's
    // Frame to Content behavior, invoked after every successful load).
    void FrameToContent();

    // Canvas pan mouse button (forwarded from the app preferences).
    void SetPanMouseButton(Qt::MouseButton button);

    // Surfaces an operation error on this screen's own toast (used by the
    // MainWindow glue for load/save failures).
    void ReportError(const QString& message) { ShowToast(message); }

    // Created with no parent; the MainWindow's docks take ownership via
    // QDockWidget::setWidget (same hand-up pattern as RuntimeTab).
    NodePalette* PalettePanel() const;
    InspectorPanel* InspectorPanelWidget() const;

    GraphScene* Scene() const { return graphScene_.get(); }

    // MainWindow hooks: fired after any document-identity or content change
    // (title/undo-state refresh) and to relay the screen's status text.
    std::function<void()> onDocumentChanged;
    std::function<void(const QString&)> onStatusMessage;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void ConnectSceneSignals();
    void OnSceneSelectionChanged();
    void ShowNodeContextMenu(const QPointF& globalPos, QtNodes::NodeId qtId);
    // Locks controller-side inspector rows that the plant engines reject at
    // load: the domain combo (single implicit domain), exclude-from-compile
    // and per-parameter 'wire' toggles.
    void ApplyPlantChrome();
    // Same rationale as MainWindow::StripBrokenSceneActions: QtNodes' own
    // undo/paste/duplicate actions silently desync the NodeAPI graph.
    void StripBrokenViewActions();
    void CheckForRejectionReason();
    void ShowToast(const QString& message);
    void RepositionToast();

    void ResetHistory();
    void RecordHistorySnapshot();
    bool RestoreHistorySnapshot(const std::string& snapshot);
    void NotifyDocumentChanged();
    // Drops the informational controller entry-point flags from the loaded
    // plant type database (see the comment in the .cpp).
    void SanitizeEntryPointFlags();

    std::unique_ptr<GraphScene> graphScene_;
    QPointer<GraphView> view_;
    QPointer<NodePalette> palette_;
    QPointer<InspectorPanel> inspector_;
    QPointer<QLabel> toast_;
    QTimer* toastTimer_ = nullptr;
    QAction* deleteAction_ = nullptr;
    QAction* clearSelectionAction_ = nullptr;

    std::string templatesDir_;
    std::string currentPath_;

    std::vector<std::string> undoHistory_;
    std::vector<std::string> redoHistory_;
    std::string currentHistorySnapshot_;
    bool restoringHistory_ = false;
    int undoHistoryLimit_ = 100;

    bool connectionCreatedThisDrag_ = false;
};

}  // namespace NodeGUI
