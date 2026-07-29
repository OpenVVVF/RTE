#pragma once

#include <QWidget>

#include <QStringList>

class QCheckBox;
class QLineEdit;
class QPlainTextEdit;

namespace NodeGUI::runtime {

class RuntimeController;

// Reusable device-console panel: scrollback view with Clear/Autoscroll and a
// command row with Up/Down history. Bound to the RuntimeController; multiple
// instances can coexist (e.g. one on the Runtime screen, one docked on the
// Node Editor screen) — each keeps its own view state.
class ConsolePanel : public QWidget {
    Q_OBJECT

public:
    explicit ConsolePanel(RuntimeController* controller, QWidget* parent = nullptr);

private slots:
    void OnStoreChanged();
    void OnSendCommand();

private:
    RuntimeController* controller_;

    QPlainTextEdit* consoleView_ = nullptr;
    QLineEdit* commandEdit_ = nullptr;
    QCheckBox* autoscrollCheck_ = nullptr;

    // Console drain position (store console seq).
    uint64_t lastConsoleSeq_ = 0;

    QStringList commandHistory_;
    int historyIndex_ = -1;
};

}  // namespace NodeGUI::runtime
