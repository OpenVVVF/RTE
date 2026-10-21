#pragma once

#include <QColor>
#include <QString>

class QPainter;
class QRectF;

namespace NodeGUI {

// Palette-carrying colors for node glyphs: the painter keeps them sourced
// from the node style (FontColor family), so glyphs follow the active theme
// instead of hardcoding black-on-white.
struct GlyphColors {
    QColor line;   // strokes/outlines
    QColor fill;   // subtle interior fill
    QColor text;   // glyph letters ("M", "PI", ...)
};

// True when a glyph is registered for the node type id.
bool HasTypeGlyph(const QString& typeId);

// Paints the domain glyph registered for the node type into the node's
// center area (the glyph square centers itself there). Returns false when
// the type has no glyph or the area is too small to be meaningful.
bool PaintTypeGlyph(QPainter* painter,
                    const QString& typeId,
                    const QRectF& area,
                    const GlyphColors& colors);

// ---- glyph area negotiation (shared by the node painter and geometry) ----
//
// A glyph fits either in the center column between the in/out port text
// columns (dense nodes like motors/inverters) or in the full-width strip
// below the port rows and above the parameter block (sparse nodes like a
// PI controller with long captions). ParameterNodeGeometry calls
// GlyphAreaPadding to grow a node ONLY when neither fits, so registered
// types always show their icon.
struct GlyphAreaInput {
    double nodeWidth = 0.0;
    double areaTop = 0.0;      // caption(+label) bottom + spacing
    double areaBottom = 0.0;   // parameter block top - margin, or node bottom - spacing
    unsigned inRows = 0;
    unsigned outRows = 0;
    double inColumnWidth = 0.0;   // max in-port text advance
    double outColumnWidth = 0.0;
    double spacing = 10.0;
    double rowHeight = 15.0;      // port row pitch contributor (_portSize)
};

// Where the glyph draws at the current node size; null when it cannot fit.
QRectF GlyphArea(const GlyphAreaInput& input);

// Extra node height needed for the strip to fit (0 when it fits as-is).
double GlyphAreaPadding(const GlyphAreaInput& input);

}  // namespace NodeGUI
