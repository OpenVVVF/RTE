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

// Foldback cleansing (last-resort render contract: every BuildRoute caller —
// painter, hit test, mini-router traffic — sees the same spike-free shape).
// A foldback is three consecutive points A, B, C on ONE axis where the leg
// B->C turns back along the track A->B just walked (dot(B-A, C-B) < 0) and C
// lands inside that walked span (|C-B| <= |B-A|): a zero-area U-turn micro
// segment whose pixels only ever re-tread drawn ones. Excursions the user
// placed on purpose return LONGER than the track they fold over
// (|C-B| > |B-A|) and are kept — clean routing is the drag-clip's job first.
// Dropping the tip vertex collapses the spike; cascades settle in one fixpoint
// pass, and a follow-up duplicate drop absorbs annihilated pairs (C == A).
void DropFoldbacks(QPolygonF& route) {
    constexpr double eps = 0.75;
    bool changed = true;
    while (changed) {
        changed = false;
        // Foldback eliminations feed duplicate runs (a collapsed spur leaves
        // its two junction points equal): absorb those first so the triple
        // scan below always walks real legs.
        DropNearDuplicates(route);
        for (int i = 0; i + 2 < route.size(); ++i) {
            const QPointF a = route[i];
            const QPointF b = route[i + 1];
            const QPointF c = route[i + 2];
            const QPointF d1 = b - a;
            const QPointF d2 = c - b;
            const bool horizontal =
                std::abs(d1.y()) <= eps && std::abs(d2.y()) <= eps;
            const bool vertical =
                std::abs(d1.x()) <= eps && std::abs(d2.x()) <= eps;
            if ((!horizontal && !vertical) ||
                (std::abs(d1.x()) <= eps && std::abs(d1.y()) <= eps) ||
                (std::abs(d2.x()) <= eps && std::abs(d2.y()) <= eps)) {
                continue;
            }
            const double dot = d1.x() * d2.x() + d1.y() * d2.y();
            if (dot >= 0.0) {
                continue;
            }
            const double len1 = std::abs(horizontal ? d1.x() : d1.y());
            const double len2 = std::abs(horizontal ? d2.x() : d2.y());
            if (len2 > len1 + eps) {
                // The return leg OUTRUNS the outgoing one: b's excursion is
                // apparently intentional geometry, not a dead-end spike.
                continue;
            }
            route.remove(i + 1);
            changed = true;
            break;
        }
    }
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

// True when the segment passes through the interior of rect (boundary
// touches count as clear; manually routed tracks hug node edges).
bool SegmentCrossesRect(const QPointF& a, const QPointF& b, const QRectF& rect) {
    constexpr double eps = 1e-6;
    auto strictlyInside = [&](const QPointF& p) {
        return p.x() > rect.left() + eps && p.x() < rect.right() - eps &&
               p.y() > rect.top() + eps && p.y() < rect.bottom() - eps;
    };
    if (strictlyInside(a) || strictlyInside(b)) {
        return true;
    }

    const double dx = b.x() - a.x();
    const double dy = b.y() - a.y();
    // Parametric slab test; intersect strictly against the rect walls.
    double t0 = 0.0, t1 = 1.0;
    auto clipAxis = [&](double p, double d, double lo, double hi) {
        if (std::abs(d) < eps) {
            return p > lo + eps && p < hi - eps;
        }
        double ta = (lo - p) / d;
        double tb = (hi - p) / d;
        if (ta > tb) {
            std::swap(ta, tb);
        }
        t0 = std::max(t0, ta);
        t1 = std::min(t1, tb);
        return t1 > t0 + 1e-9;
    };
    return clipAxis(a.x(), dx, rect.left(), rect.right()) &&
           clipAxis(a.y(), dy, rect.top(), rect.bottom());
}

int PolylineNodeCrossings(const QPolygonF& poly, const std::vector<QRectF>& obstacles) {
    int crossings = 0;
    for (int s = 0; s + 1 < poly.size(); ++s) {
        for (const QRectF& rect : obstacles) {
            if (SegmentCrossesRect(poly[s], poly[s + 1], rect)) {
                ++crossings;
            }
        }
    }
    return crossings;
}

// Proper segment intersection (crossing interiors; shared/parallel endpoint
// touches do not count).
bool SegmentsCross(const QPointF& a1, const QPointF& a2,
                   const QPointF& b1, const QPointF& b2) {
    auto orient = [](const QPointF& p, const QPointF& q, const QPointF& r) {
        return (q.x() - p.x()) * (r.y() - p.y()) - (q.y() - p.y()) * (r.x() - p.x());
    };
    const double d1 = orient(b1, b2, a1);
    const double d2 = orient(b1, b2, a2);
    const double d3 = orient(a1, a2, b1);
    const double d4 = orient(a1, a2, b2);
    return ((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) &&
           ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0));
}

int PolylineWireCrossings(const QPolygonF& poly, const std::vector<QPolygonF>& routed) {
    int crossings = 0;
    for (const QPolygonF& other : routed) {
        for (int s = 0; s + 1 < poly.size(); ++s) {
            const QRectF segBox = QRectF(poly[s], poly[s + 1]).normalized();
            for (int o = 0; o + 1 < other.size(); ++o) {
                if (!segBox.intersects(QRectF(other[o], other[o + 1]).normalized())) {
                    continue;
                }
                if (SegmentsCross(poly[s], poly[s + 1], other[o], other[o + 1])) {
                    ++crossings;
                }
            }
        }
    }
    return crossings;
}

}  // namespace

QPointF SnapToGrid(const QPointF& p) {
    return QPointF(std::round(p.x() / kGridPitch) * kGridPitch,
                   std::round(p.y() / kGridPitch) * kGridPitch);
}

double RoundUpToGrid(double v) {
    return std::ceil(v / kGridPitch) * kGridPitch;
}

QPolygonF SampleCubic(const QPointF& from,
                      const QPointF& ctrl1,
                      const QPointF& ctrl2,
                      const QPointF& to,
                      int segments) {
    QPolygonF samples;
    samples.reserve(segments + 1);
    samples << from;
    for (int i = 1; i < segments; ++i) {
        const double t = static_cast<double>(i) / segments;
        const double u = 1.0 - t;
        samples << u * u * u * from +
                     3.0 * u * u * t * ctrl1 +
                     3.0 * u * t * t * ctrl2 +
                     t * t * t * to;
    }
    samples << to;
    return samples;
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
    DropFoldbacks(route);
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

AutoRouteChoice PlanAutoRoute(const QPointF& rawOut,
                              const QPointF& rawIn,
                              const QPolygonF& legacySamples,
                              const AutoRouteContext& environment) {
    const QPointF out = SnapToGrid(rawOut);
    const QPointF in = SnapToGrid(rawIn);

    // Fast path: a clean default corridor survives verbatim (no explicit
    // polyline emitted; the painter draws the rigid default).
    if (PolylineNodeCrossings(legacySamples, environment.obstacles) == 0 &&
        PolylineWireCrossings(legacySamples, environment.routed) == 0) {
        return {true, legacySamples};
    }

    // Ordered candidates; later = more aggressive shape (ties lose on index).
    std::vector<QPolygonF> candidates;
    const bool axisAligned =
        std::abs(out.y() - in.y()) < 0.5 || std::abs(out.x() - in.x()) < 0.5;
    if (axisAligned) {
        candidates.push_back(QPolygonF{out, in});
    } else {
        // The two single-bend Ls (leave horizontally, or arrive horizontally).
        candidates.push_back(QPolygonF{out, QPointF(in.x(), out.y()), in});
        candidates.push_back(QPolygonF{out, QPointF(out.x(), in.y()), in});

        // S corridors: one vertical run toward the input, horizontal port
        // stubs at both ends (measured from the out side, then the in side).
        const double track = in.x() >= out.x() ? kGridPitch : -kGridPitch;
        auto distinct = [](const QPointF& a, const QPointF& b) {
            return std::abs(a.x() - b.x()) > 0.5 || std::abs(a.y() - b.y()) > 0.5;
        };
        for (const bool fromIn : {false, true}) {
            for (int k = 1; k <= 3; ++k) {
                const double midX = fromIn ? in.x() - track * k : out.x() + track * k;
                if (distinct(QPointF(midX, out.y()), out) &&
                    distinct(QPointF(midX, in.y()), in)) {
                    candidates.push_back(QPolygonF{out,
                                                   QPointF(midX, out.y()),
                                                   QPointF(midX, in.y()),
                                                   in});
                }
            }
        }

        // Wrap corridors above/below the obstacles blocking the endpoint
        // span (tightest clearance first).
        QRectF span = QRectF(out, in).normalized();
        span = span.adjusted(-kGridPitch, -kGridPitch, kGridPitch, kGridPitch);
        double minTop = std::numeric_limits<double>::max();
        double maxBottom = std::numeric_limits<double>::lowest();
        for (const QRectF& rect : environment.obstacles) {
            if (rect.intersects(span)) {
                minTop = std::min(minTop, rect.top());
                maxBottom = std::max(maxBottom, rect.bottom());
            }
        }
        if (minTop != std::numeric_limits<double>::max()) {
            for (int k = 1; k <= 3; ++k) {
                const double clearY = minTop - k * kGridPitch;
                candidates.push_back(QPolygonF{out,
                                               QPointF(out.x(), clearY),
                                               QPointF(in.x(), clearY),
                                               in});
            }
            for (int k = 1; k <= 3; ++k) {
                const double clearY = maxBottom + k * kGridPitch;
                candidates.push_back(QPolygonF{out,
                                               QPointF(out.x(), clearY),
                                               QPointF(in.x(), clearY),
                                               in});
            }
        }
    }

    int bestIndex = -1;
    int bestWireCrossings = std::numeric_limits<int>::max();
    for (int i = 0; i < static_cast<int>(candidates.size()); ++i) {
        const QPolygonF& candidate = candidates[static_cast<std::size_t>(i)];
        if (PolylineNodeCrossings(candidate, environment.obstacles) > 0) {
            continue;
        }
        const int wireCrossings = PolylineWireCrossings(candidate, environment.routed);
        if (wireCrossings < bestWireCrossings) {
            bestWireCrossings = wireCrossings;
            bestIndex = i;
        }
    }

    if (bestIndex < 0) {
        return {true, legacySamples};
    }
    return {false, candidates[static_cast<std::size_t>(bestIndex)]};
}

}  // namespace NodeGUI::WireRoute
