#pragma once

#include <QString>

#include <cmath>
#include <optional>

namespace NodeGUI::runtime {

inline QString FormatSignalRate(std::optional<double> hz) {
    if (!hz) return QStringLiteral("—");
    if (!std::isfinite(*hz) || *hz <= 0.0) return QStringLiteral("0Hz");
    const int decimals = *hz < 1.0 ? 2 : (*hz < 10.0 ? 1 : 0);
    return QString::number(*hz, 'f', decimals) + QStringLiteral("Hz");
}

}  // namespace NodeGUI::runtime
