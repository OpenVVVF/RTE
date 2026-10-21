#pragma once

#include <QPointF>
#include <QPolygonF>

#include <vector>

namespace NodeGUI::WireRoute {

// Grid pitch for waypoint snapping (scene units).
inline constexpr double kGridPitch = 10.0;

QPointF SnapToGrid(const QPointF& p);

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

}  // namespace NodeGUI::WireRoute
