#include "SignalTablePanel.h"

#include "RuntimeController.h"
#include "SignalRateFormat.h"

#include <QCheckBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QSizePolicy>
#include <QSlider>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace NodeGUI::runtime {

namespace {
constexpr int kRateColumnWidth = 62;
constexpr int kSignalTextPadding = 14;
}

SignalTablePanel::SignalTablePanel(RuntimeController* controller, QWidget* parent)
    : QWidget(parent)
    , controller_(controller) {
    // At narrow widths the rate disappears and signal names elide; graph
    // selectors and the current value remain visible.
    setMinimumWidth(300);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    auto* controlsRow = new QHBoxLayout;
    controlsRow->addWidget(new QLabel(QStringLiteral("Plot view (sec)"), this));
    viewSlider_ = new QSlider(Qt::Horizontal, this);
    viewSlider_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    viewSlider_->setRange(1, 1200);  // 0.05 .. 60.0 s in 0.05 s steps
    viewSlider_->setValue(100);
    connect(viewSlider_, &QSlider::valueChanged, this, &SignalTablePanel::OnViewSecondsChanged);
    controlsRow->addWidget(viewSlider_, 1);
    filterEdit_ = new QLineEdit(this);
    filterEdit_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    filterEdit_->setPlaceholderText(QStringLiteral("Filter"));
    connect(filterEdit_, &QLineEdit::textChanged, this, &SignalTablePanel::OnFilterChanged);
    controlsRow->addWidget(filterEdit_, 1);
    layout->addLayout(controlsRow);

    signalTable_ = new QTableWidget(0, 6, this);
    signalTable_->setHorizontalHeaderLabels(
        {QStringLiteral("G1"), QStringLiteral("G2"), QStringLiteral("G3"),
         QStringLiteral("Signal"), QStringLiteral("Value"), QStringLiteral("Rate")});
    signalTable_->horizontalHeaderItem(5)->setToolTip(
        QStringLiteral("Numeric value changes per second; repeated telemetry values do not count."));
    // User-adjustable columns with sensible starting widths; the Signal column
    // soaks up extra width when the dock is resized.
    signalTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    signalTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    signalTable_->setColumnWidth(0, 23);
    signalTable_->setColumnWidth(1, 23);
    signalTable_->setColumnWidth(2, 23);
    signalTable_->setColumnWidth(3, 100);
    signalTable_->setColumnWidth(4, 112);
    signalTable_->setColumnWidth(5, kRateColumnWidth);
    fullNameWidth_ = signalTable_->horizontalHeader()->fontMetrics().horizontalAdvance(
        QStringLiteral("Signal")) + kSignalTextPadding;
    signalTable_->verticalHeader()->setVisible(false);
    signalTable_->setSelectionMode(QAbstractItemView::NoSelection);
    signalTable_->viewport()->installEventFilter(this);
    layout->addWidget(signalTable_, 1);

    connect(controller_, &RuntimeController::storeChanged,
            this, &SignalTablePanel::OnStoreChanged);
}

std::array<QStringList, 3> SignalTablePanel::GraphSignalSets() const {
    return graphSignals_;
}

void SignalTablePanel::SetGraphSignalSets(const std::array<QStringList, 3>& sets) {
    graphSignals_ = sets;
    RebuildSignalTable();
    emit graphSignalsChanged(graphSignals_);
}

void SignalTablePanel::SetViewSeconds(double seconds) {
    // Slider is in twentieths of a second.
    const int pos = qBound(viewSlider_->minimum(),
                           static_cast<int>(seconds * 20.0 + 0.5),
                           viewSlider_->maximum());
    if (viewSlider_->value() != pos) {
        viewSlider_->setValue(pos);  // fires OnViewSecondsChanged -> viewSecondsChanged
    } else {
        emit viewSecondsChanged(seconds);
    }
}

void SignalTablePanel::OnStoreChanged() {
    RebuildSignalTable();

    // Refresh the value and its measured value-change rate in place.
    const auto displays = controller_->Store().SignalDisplays();
    for (int row = 0; row < signalTable_->rowCount(); ++row) {
        const auto key = signalTable_->item(row, 3)->data(Qt::UserRole).toString().toStdString();
        const auto it = displays.find(key);
        if (it != displays.end()) {
            signalTable_->item(row, 4)->setText(
                QString::asprintf("% .6f", it->second.value));
            signalTable_->item(row, 5)->setText(FormatSignalRate(it->second.updateHz));
        }
    }
}

void SignalTablePanel::OnFilterChanged(const QString& /*text*/) {
    RebuildSignalTable();
}

void SignalTablePanel::OnViewSecondsChanged(int value) {
    emit viewSecondsChanged(value / 20.0);
}

bool SignalTablePanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == signalTable_->viewport() && event->type() == QEvent::Resize) {
        UpdateResponsiveColumns();
    }
    return QWidget::eventFilter(watched, event);
}

void SignalTablePanel::UpdateResponsiveColumns() {
    if (updatingColumns_) {
        return;
    }
    updatingColumns_ = true;
    const int viewportWidth = signalTable_->viewport()->width();
    const int fixedWidth = signalTable_->columnWidth(0)
        + signalTable_->columnWidth(1) + signalTable_->columnWidth(2)
        + signalTable_->columnWidth(4);
    if (!signalTable_->isColumnHidden(5)) {
        rateColumnWidth_ = signalTable_->columnWidth(5);
    }
    // Show Rate at the first width where every visible signal name fits in
    // the remaining Signal column without elision.
    const bool hideRate = viewportWidth < fixedWidth + rateColumnWidth_ + fullNameWidth_;
    if (signalTable_->isColumnHidden(5) != hideRate) {
        signalTable_->setColumnHidden(5, hideRate);
    }
    UpdateDisplayNames(std::max(0, viewportWidth - fixedWidth
        - (hideRate ? 0 : rateColumnWidth_)));
    updatingColumns_ = false;
}

void SignalTablePanel::UpdateDisplayNames(int signalWidth) {
    const int textWidth = std::max(0, signalWidth - kSignalTextPadding);
    const auto font = signalTable_->fontMetrics();
    for (int row = 0; row < signalTable_->rowCount(); ++row) {
        auto* item = signalTable_->item(row, 3);
        if (!item) {
            continue;
        }
        const QString name = item->data(Qt::UserRole).toString();
        const QString display = font.elidedText(name, Qt::ElideMiddle, textWidth);
        if (item->text() != display) {
            item->setText(display);
        }
    }
}

void SignalTablePanel::RebuildSignalTable() {
    // Only rebuild when the visible name set changes; otherwise rows would
    // reset their scroll position and checkboxes every refresh.
    const auto namesStd = controller_->Store().SignalNames();
    QStringList names;
    names.reserve(static_cast<qsizetype>(namesStd.size()));
    const QString filter = filterEdit_->text();
    for (const auto& name : namesStd) {
        const QString qname = QString::fromStdString(name);
        if (filter.isEmpty() || qname.contains(filter, Qt::CaseInsensitive)) {
            names.push_back(qname);
        }
    }

    QStringList current;
    current.reserve(signalTable_->rowCount());
    for (int row = 0; row < signalTable_->rowCount(); ++row) {
        current.push_back(signalTable_->item(row, 3)->data(Qt::UserRole).toString());
    }
    if (current == names) {
        return;
    }

    rebuildingTable_ = true;
    fullNameWidth_ = signalTable_->horizontalHeader()->fontMetrics().horizontalAdvance(
        QStringLiteral("Signal")) + kSignalTextPadding;
    const auto font = signalTable_->fontMetrics();
    for (const QString& name : names) {
        fullNameWidth_ = std::max(fullNameWidth_,
                                  font.horizontalAdvance(name) + kSignalTextPadding);
    }
    signalTable_->setRowCount(0);
    signalTable_->setRowCount(static_cast<int>(names.size()));
    for (int row = 0; row < names.size(); ++row) {
        for (int col = 0; col < 3; ++col) {
            // A real centered QCheckBox: a QTableWidgetItem checkbox would
            // leave an empty ghost text element next to the box.
            auto* check = new QCheckBox(signalTable_);
            check->setChecked(graphSignals_[col].contains(names[row]));
            auto* cell = new QWidget(signalTable_);
            auto* cellLayout = new QHBoxLayout(cell);
            cellLayout->setContentsMargins(0, 0, 0, 0);
            cellLayout->setAlignment(Qt::AlignCenter);
            cellLayout->addWidget(check);
            signalTable_->setCellWidget(row, col, cell);

            connect(check, &QCheckBox::toggled, this,
                    [this, row, col](bool checked) {
                        if (rebuildingTable_) {
                            return;
                        }
                        const QString name = signalTable_->item(row, 3)->data(Qt::UserRole).toString();
                        auto& set = graphSignals_[col];
                        if (checked) {
                            if (!set.contains(name)) {
                                set.push_back(name);
                            }
                        } else {
                            set.removeAll(name);
                        }
                        emit graphSignalsChanged(graphSignals_);
                    });
        }
        auto* nameItem = new QTableWidgetItem(names[row]);
        nameItem->setFlags(Qt::ItemIsEnabled);
        nameItem->setToolTip(names[row]);
        nameItem->setData(Qt::UserRole, names[row]);
        signalTable_->setItem(row, 3, nameItem);
        auto* valueItem = new QTableWidgetItem;
        valueItem->setFlags(Qt::ItemIsEnabled);
        signalTable_->setItem(row, 4, valueItem);
        auto* rateItem = new QTableWidgetItem;
        rateItem->setFlags(Qt::ItemIsEnabled);
        signalTable_->setItem(row, 5, rateItem);
    }
    rebuildingTable_ = false;
    UpdateResponsiveColumns();
}

}  // namespace NodeGUI::runtime
