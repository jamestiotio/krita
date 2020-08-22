/*
 * SPDX-FileCopyrightText: 2016 Wolthera van Hovell tot Westerflier <griffinvalley@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISVISUALCOLORSELECTOR_H
#define KISVISUALCOLORSELECTOR_H

#include "KisVisualColorSelectorShape.h"

class KisVisualEllipticalSelectorShape : public KisVisualColorSelectorShape
{
    Q_OBJECT
public:
    enum singelDTypes{border, borderMirrored};
    explicit KisVisualEllipticalSelectorShape(KisVisualColorSelector *parent,
                                              Dimensions dimension,
                                              int channel1, int channel2,
                                              int barWidth=20,
                                              KisVisualEllipticalSelectorShape::singelDTypes d = KisVisualEllipticalSelectorShape::border
            );
    ~KisVisualEllipticalSelectorShape() override;

    void setBorderWidth(int width) override;

    /**
     * @brief getSpaceForSquare
     * @param geom the full widget rectangle
     * @return rectangle with enough space for second widget
     */
    QRect getSpaceForSquare(QRect geom) override;
    QRect getSpaceForCircle(QRect geom) override;
    QRect getSpaceForTriangle(QRect geom) override;
protected:
    QImage renderAlphaMask() const override;
    QPointF mousePositionToShapeCoordinate(const QPointF &pos, const QPointF &dragStart) const override;

private:
    QPointF convertShapeCoordinateToWidgetCoordinate(QPointF coordinate) const override;
    QPointF convertWidgetCoordinateToShapeCoordinate(QPointF coordinate) const override;

    singelDTypes m_type;
    int m_barWidth;
    QRegion getMaskMap() override;
    void drawCursor(QPainter &painter) override;
    QSize sizeHint() const override;
};

#endif // KISVISUALCOLORSELECTOR_H
