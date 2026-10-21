#include "WireRoute.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace NodeGUI::WireRoute {

namespace {

enum class Axis { Horizontal, Vertical };

QPolygonF ControlPolyline(const QPointF& out,
                          const QPointF& in,
                          const std::vector<QPointF>& waypoints) {
    QPolygonF points;
    points.reserve(static_cast<int>(waypoints.size()) + 2);
    points << out;
    for (const QPointF& p : waypoints) {
        points << p;
    }
    points << in;
    return points;
}

// A leg A -> B rendered rectilinear: straight when already axis-aligned,
// otherwise a single elbow. The elbow leaves/continues on `leaveOn` when
// that axis is free; `mustArriveHorizontal` (the in-port stub) forces the
// vertical-first bend. Sets `arrival` to the axis the route enters B on.
void AppendLeg(QPolygonF& route,
               const QPointF& a,
               const QPointF& b,
               Axis leaveOn,
               bool mustArriveHorizontal,
               Axis& arrival) {
    constexpr double eps = 0.75;
    const double dx = b.x() - a.x();
    const double dy = b.y() - a.y();
    if (std::abs(dx) <= eps || std::abs(dy) <= eps) {
        route << b;
        arrival = std::abs(dx) > eps ? Axis::Horizontal : Axis::Vertical;
        return;
    }
    bool horizontalFirst = leaveOn == Axis::Horizontal;
    if (mustArriveHorizontal) {
        horizontalFirst = false;
    }
    if (horizontalFirst) {
        route << QPointF(b.x(), a.y()) << b;
        arrival = Axis::Vertical;
    } else {
        route << QPointF(a.x(), b.y()) << b;
        arrival = Axis::Horizontal;
    }
}

void DropNearDuplicates(QPolygonF& route) {
    QPolygonF clean;
    clean.reserve(route.size());
    for (const QPointF& p : route) {
        if (!clean.isEmpty()) {
            const QPointF d = p - clean.constLast();
            if (std::abs(d.x()) < 1e-3 && std::abs(d.y()) < 1e-3) {
                continue;
            }
        }
        clean << p;
    }
    route = clean;
}

double DistanceToSegment(const QPointF& p, const QPointF& a, const QPointF& b) {
    const QPointF ab = b - a;
    const double len2 = QPointF::dotProduct(ab, ab);
    double t = 0.0;
    if (len2 > 1e-9) {
        t = QPointF::dotProduct(p - a, ab) / len2;
        t = std::max(0.0, std::min(1.0, t));
    }
    const QPointF proj = a + t * ab;
    const QPointF d = p - proj;
    return std::sqrt(QPointF::dotProduct(d, d));
}

}  // namespace

QPointF SnapToGrid(const QPointF& p) {
    return QPointF(std::round(p.x() / kGridPitch) * kGridPitch,
                   std::round(p.y() / kGridPitch) * kGridPitch);
}

QPolygonF BuildRoute(const QPointF& out,
                     const QPointF& in,
                     const std::vector<QPointF>& waypoints) {
    const QPolygonF control = ControlPolyline(out, in, waypoints);
    QPolygonF route;
    route.reserve(control.size() * 2);
    route << control.constFirst();

    Axis arrival = Axis::Horizontal;
    Axis leaveOn = Axis::Horizontal;  // out-port stub leaves sideways
    for (int i = 0; i + 1 < control.size(); ++i) {
        const bool lastLeg = i + 2 == control.size();
        AppendLeg(route, control[i], control[i + 1], leaveOn, lastLeg, arrival);
        leaveOn = arrival;
    }

    DropNearDuplicates(route);
    return route;
}

int NearestRouteLeg(const QPointF& out,
                    const QPointF& in,
                    const std::vector<QPointF>& waypoints,
                    const QPointF& p,
                    double* distance) {
    const QPolygonF control = ControlPolyline(out, in, waypoints);
    if (control.size() < 2) {
        return -1;
    }

    int bestLeg = -1;
    double bestDistance = std::numeric_limits<double>::max();

    Axis arrival = Axis::Horizontal;
    Axis leaveOn = Axis::Horizontal;
    for (int i = 0; i + 1 < control.size(); ++i) {
        QPolygonF leg;
        leg << control[i];
        const bool lastLeg = i + 2 == control.size();
        AppendLeg(leg, control[i], control[i + 1], leaveOn, lastLeg, arrival);
        leaveOn = arrival;
        DropNearDuplicates(leg);

        double legDistance = std::numeric_limits<double>::max();
        for (int s = 0; s + 1 < leg.size(); ++s) {
            legDistance = std::min(legDistance, DistanceToSegment(p, leg[s], leg[s + 1]));
        }
        if (legDistance < bestDistance) {
            bestDistance = legDistance;
            bestLeg = i;
        }
    }

    if (distance) {
        *distance = bestDistance;
    }
    return bestLeg;
}

}  // namespace NodeGUI::WireRoute
