#pragma once

#include <QPointF>
#include <QPolygonF>
#include <QRectF>

#include <vector>

// Route precedence in the scene: user waypoints > (future net stubs) >
// PlanAutoRoute > legacy cubic (BridgeConnectionPainter::wirePath).
// Grid contract: kGridPitch is THE scene grid — node origins, port rows and
// sockets, wire bends and router tracks all snap to its multiples, so the
// whole canvas composites on one orthogonal lattice.

namespace NodeGUI::WireRoute {

// Grid pitch for waypoint snapping (scene units).
inline constexpr double kGridPitch = 10.0;

QPointF SnapToGrid(const QPointF& p);
// Rounds a measurement (port pitch, node height, ...) UP to a grid multiple.
// Nodes may grow, never shrink.
double RoundUpToGrid(double v);

// Rectilinear route through out -> waypoints -> in. Port stubs stay
// horizontal (ports enter/leave sideways); legs between consecutive control
// points get a single elbow whose orientation continues the previous leg's
// axis. Axis-aligned legs pass through unchanged.
QPolygonF BuildRoute(const QPointF& out,
                     const QPointF& in,
                     const std::vector<QPointF>& waypoints);

// Index of the control polyline leg (0-based; leg i spans control point i to
// i+1, where the control polyline is out + waypoints + in) whose RENDERED
// sub-route (straight or elbow) comes closest to p. Distance to that leg is
// returned via distance. Returns -1 when the route cannot be split
// (fewer than 2 control points).
int NearestRouteLeg(const QPointF& out,
                    const QPointF& in,
                    const std::vector<QPointF>& waypoints,
                    const QPointF& p,
                    double* distance);

// ---- default (waypoint-free) routing ------------------------------------

// Cubic-bezier sampled to a polyline for crossing checks (the legacy default
// renders as a bezier, not these samples).
QPolygonF SampleCubic(const QPointF& from,
                      const QPointF& ctrl1,
                      const QPointF& ctrl2,
                      const QPointF& to,
                      int segments);

// What the mini-router decided for one connection: legacy = draw today's
// cubic (line holds its samples for wire-crossing checks); otherwise draw
// line (an orthogonal polyline in scene coordinates).
struct AutoRouteChoice {
    bool legacy = true;
    QPolygonF line;
};

// Environment the router checks against: node bodies (inflated by the
// caller) it must never cross, and already-routed wire polylines from the
// same pass (waypoints first, then route order).
struct AutoRouteContext {
    std::vector<QRectF> obstacles;
    std::vector<QPolygonF> routed;
};

// Deterministic mini-router for one connection. Fast path: the legacy cubic
// with zero node crossings AND zero wire crossings survives verbatim (clean
// graphs stay bit-identical). Else the first candidate from straight/L/S/
// wrap corridors that clears every node body wins; ties break on fewer
// crossings with previously routed wires, then candidate order. Falls back
// to legacy when nothing clears — never worse than today.
AutoRouteChoice PlanAutoRoute(const QPointF& out,
                              const QPointF& in,
                              const QPolygonF& legacySamples,
                              const AutoRouteContext& environment);

}  // namespace NodeGUI::WireRoute
