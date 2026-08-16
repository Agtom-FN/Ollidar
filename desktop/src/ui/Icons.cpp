#include "ui/Icons.h"

#include <QPainter>
#include <QPainterPath>
#include <QRectF>

namespace lidarscan {
namespace icons {
namespace {

// Lucide's own geometry, transcribed. Every helper works in the 24x24 box.

void folder(QPainter& p) {
  QPainterPath path;
  path.moveTo(3.0, 7.0);
  path.lineTo(3.0, 18.0);
  path.arcTo(QRectF(3.0, 16.0, 4.0, 4.0), 180.0, 90.0);
  path.lineTo(19.0, 20.0);
  path.arcTo(QRectF(17.0, 16.0, 4.0, 4.0), 270.0, 90.0);
  path.lineTo(21.0, 10.0);
  path.arcTo(QRectF(17.0, 8.0, 4.0, 4.0), 0.0, 90.0);
  path.lineTo(11.5, 10.0);
  path.lineTo(9.5, 6.6);
  path.arcTo(QRectF(5.0, 4.6, 4.0, 4.0), 300.0, 60.0);
  path.lineTo(5.0, 5.0);
  path.arcTo(QRectF(3.0, 5.0, 4.0, 4.0), 90.0, 90.0);
  path.closeSubpath();
  p.drawPath(path);
}

void radar(QPainter& p) {
  // Concentric sweep arcs + the ember-lit return blip: the capture workspace.
  p.drawArc(QRectF(2.5, 2.5, 19.0, 19.0), 16 * 20, 16 * 320);
  p.drawArc(QRectF(6.5, 6.5, 11.0, 11.0), 16 * 20, 16 * 260);
  p.drawLine(QPointF(12.0, 12.0), QPointF(19.6, 8.6));
  QPainterPath dot;
  dot.addEllipse(QPointF(12.0, 12.0), 1.35, 1.35);
  p.fillPath(dot, p.pen().color());
}

void rotate3d(QPainter& p) {
  // An orbit ring around a cube corner — the review/inspect workspace.
  p.save();
  p.translate(12.0, 12.5);
  p.rotate(-28.0);
  p.drawEllipse(QRectF(-10.0, -4.6, 20.0, 9.2));
  p.restore();
  QPainterPath v;
  v.moveTo(12.0, 4.6);
  v.lineTo(12.0, 12.2);
  v.moveTo(12.0, 12.2);
  v.lineTo(17.4, 15.4);
  v.moveTo(12.0, 12.2);
  v.lineTo(6.6, 15.4);
  p.drawPath(v);
}

void layout(QPainter& p) {
  p.drawRoundedRect(QRectF(3.0, 3.0, 18.0, 18.0), 2.6, 2.6);
  p.drawLine(QPointF(3.0, 10.0), QPointF(21.0, 10.0));
  p.drawLine(QPointF(10.0, 10.0), QPointF(10.0, 21.0));
}

void gitMerge(QPainter& p) {
  p.drawEllipse(QPointF(6.5, 18.0), 2.6, 2.6);
  p.drawEllipse(QPointF(6.5, 6.0), 2.6, 2.6);
  p.drawEllipse(QPointF(18.0, 12.0), 2.6, 2.6);
  p.drawLine(QPointF(6.5, 8.6), QPointF(6.5, 15.4));
  QPainterPath c;
  c.moveTo(9.1, 6.4);
  c.cubicTo(13.5, 6.4, 12.6, 12.0, 15.4, 12.0);
  p.drawPath(c);
}

void layers(QPainter& p) {
  QPainterPath top;
  top.moveTo(12.0, 3.0);
  top.lineTo(21.0, 7.6);
  top.lineTo(12.0, 12.2);
  top.lineTo(3.0, 7.6);
  top.closeSubpath();
  p.drawPath(top);
  p.drawPolyline(QPolygonF() << QPointF(3.0, 12.2) << QPointF(12.0, 16.8) << QPointF(21.0, 12.2));
  p.drawPolyline(QPolygonF() << QPointF(3.0, 16.4) << QPointF(12.0, 21.0) << QPointF(21.0, 16.4));
}

void sliders(QPainter& p) {
  p.drawLine(QPointF(4.0, 7.0), QPointF(20.0, 7.0));
  p.drawLine(QPointF(4.0, 12.0), QPointF(20.0, 12.0));
  p.drawLine(QPointF(4.0, 17.0), QPointF(20.0, 17.0));
  const QColor c = p.pen().color();
  auto knob = [&](qreal x, qreal y) {
    QPainterPath k;
    k.addEllipse(QPointF(x, y), 2.2, 2.2);
    p.fillPath(k, c);
  };
  knob(9.0, 7.0);
  knob(15.5, 12.0);
  knob(7.0, 17.0);
}

void package(QPainter& p) {
  QPainterPath body;
  body.moveTo(12.0, 2.6);
  body.lineTo(20.6, 7.2);
  body.lineTo(20.6, 16.8);
  body.lineTo(12.0, 21.4);
  body.lineTo(3.4, 16.8);
  body.lineTo(3.4, 7.2);
  body.closeSubpath();
  p.drawPath(body);
  p.drawLine(QPointF(3.4, 7.2), QPointF(12.0, 11.9));
  p.drawLine(QPointF(20.6, 7.2), QPointF(12.0, 11.9));
  p.drawLine(QPointF(12.0, 11.9), QPointF(12.0, 21.4));
}

void fileDown(QPainter& p) {
  QPainterPath sheet;
  sheet.moveTo(14.0, 2.8);
  sheet.lineTo(6.6, 2.8);
  sheet.lineTo(6.6, 21.2);
  sheet.lineTo(17.4, 21.2);
  sheet.lineTo(17.4, 6.2);
  sheet.closeSubpath();
  p.drawPath(sheet);
  p.drawPolyline(QPolygonF() << QPointF(14.0, 2.8) << QPointF(14.0, 6.2) << QPointF(17.4, 6.2));
  p.drawLine(QPointF(12.0, 10.6), QPointF(12.0, 17.0));
  p.drawPolyline(QPolygonF() << QPointF(9.2, 14.2) << QPointF(12.0, 17.0) << QPointF(14.8, 14.2));
}

void share(QPainter& p) {
  p.drawPolyline(QPolygonF() << QPointF(4.0, 12.6) << QPointF(4.0, 20.0) << QPointF(20.0, 20.0)
                             << QPointF(20.0, 12.6));
  p.drawLine(QPointF(12.0, 3.4), QPointF(12.0, 15.2));
  p.drawPolyline(QPolygonF() << QPointF(8.2, 7.2) << QPointF(12.0, 3.4) << QPointF(15.8, 7.2));
}

}  // namespace

void paint(QPainter& p, Glyph g) {
  switch (g) {
    case Glyph::kFolder: folder(p); break;
    case Glyph::kRadar: radar(p); break;
    case Glyph::kRotate3d: rotate3d(p); break;
    case Glyph::kLayout: layout(p); break;
    case Glyph::kGitMerge: gitMerge(p); break;
    case Glyph::kLayers: layers(p); break;
    case Glyph::kSliders: sliders(p); break;
    case Glyph::kPackage: package(p); break;
    case Glyph::kFileDown: fileDown(p); break;
    case Glyph::kShare: share(p); break;
  }
}

QPixmap pixmap(Glyph g, const QColor& color, int px, qreal dpr) {
  if (dpr <= 0.0) dpr = 1.0;
  QPixmap pm(int(px * dpr), int(px * dpr));
  pm.setDevicePixelRatio(dpr);
  pm.fill(Qt::transparent);

  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing, true);
  p.scale(px / 24.0, px / 24.0);
  QPen pen(color);
  // 1.8 is Lucide's stroke width in the 24-unit space; keep it in that space
  // so the stroke scales with the glyph rather than staying 1.8 device px.
  pen.setWidthF(1.8);
  pen.setCapStyle(Qt::RoundCap);
  pen.setJoinStyle(Qt::RoundJoin);
  p.setPen(pen);
  p.setBrush(Qt::NoBrush);
  paint(p, g);
  p.end();
  return pm;
}

QIcon icon(Glyph g, const QColor& color, const QColor& active, int px, qreal dpr) {
  QIcon ic;
  ic.addPixmap(pixmap(g, color, px, dpr), QIcon::Normal, QIcon::Off);
  ic.addPixmap(pixmap(g, active, px, dpr), QIcon::Normal, QIcon::On);
  ic.addPixmap(pixmap(g, active, px, dpr), QIcon::Active, QIcon::Off);
  ic.addPixmap(pixmap(g, active, px, dpr), QIcon::Selected, QIcon::On);
  return ic;
}

}  // namespace icons
}  // namespace lidarscan
