/*
 *  SPDX-FileCopyrightText: 2003-2009 Boudewijn Rempt <boud@valdyas.org>
 *  SPDX-FileCopyrightText: 2015 Moritz Molch <kde@moritzmolch.de>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_tool_paint.h"

#include <algorithm>

#include <QWidget>
#include <QRect>
#include <QLayout>
#include <QLabel>
#include <QPushButton>
#include <QWhatsThis>
#include <QCheckBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QKeyEvent>
#include <QEvent>
#include <QVariant>
#include <QAction>
#include <kis_debug.h>
#include <QPoint>

#include <klocalizedstring.h>
#include <kactioncollection.h>

#include <kis_icon.h>
#include <KoShape.h>
#include <KoCanvasResourceProvider.h>
#include <KoColorSpace.h>
#include <KoPointerEvent.h>
#include <KoColor.h>
#include <KoCanvasBase.h>
#include <KoCanvasController.h>

#include <kis_types.h>
#include <kis_global.h>
#include <kis_image.h>
#include <kis_paint_device.h>
#include <kis_layer.h>
#include <KisViewManager.h>
#include <kis_canvas2.h>
#include <kis_cubic_curve.h>
#include "kis_display_color_converter.h"
#include <KisDocument.h>
#include <KisReferenceImagesLayer.h>

#include "kis_config.h"
#include "kis_config_notifier.h"
#include "kis_cursor.h"
#include "kis_image_config.h"
#include "widgets/kis_cmb_composite.h"
#include "kis_slider_spin_box.h"
#include "kis_canvas_resource_provider.h"
#include "kis_tool_utils.h"
#include <brushengine/kis_paintop.h>
#include <brushengine/kis_paintop_preset.h>
#include <kis_action_manager.h>
#include <kis_action.h>
#include "strokes/kis_color_sampler_stroke_strategy.h"
#include "kis_popup_palette.h"


KisToolPaint::KisToolPaint(KoCanvasBase *canvas, const QCursor &cursor)
    : KisTool(canvas, cursor),
      m_showColorPreview(false),
      m_colorPreviewShowComparePlate(false),
      m_colorSamplerDelayTimer(),
      m_isOutlineEnabled(true)
{
    m_specialHoverModifier = false;
    m_optionsWidgetLayout = 0;

    m_opacity = OPACITY_OPAQUE_U8;

    m_supportOutline = false;

    {
        const int maxSize = KisImageConfig(true).maxBrushSize();

        int brushSize = 1;
        do {
            m_standardBrushSizes.push_back(brushSize);
            int increment = qMax(1, int(std::ceil(qreal(brushSize) / 15)));
            brushSize += increment;
        } while (brushSize < maxSize);

        m_standardBrushSizes.push_back(maxSize);
    }

    KisCanvas2 *kiscanvas = dynamic_cast<KisCanvas2*>(canvas);

    connect(this, SIGNAL(sigPaintingFinished()), kiscanvas->viewManager()->canvasResourceProvider(), SLOT(slotPainting()));

    m_colorSamplerDelayTimer.setSingleShot(true);
    connect(&m_colorSamplerDelayTimer, SIGNAL(timeout()), this, SLOT(activateSampleColorDelayed()));

    using namespace std::placeholders; // For _1 placeholder
    std::function<void(SamplingJob)> callback =
        std::bind(&KisToolPaint::addSamplerJob, this, _1);
    m_colorSamplingCompressor.reset(
        new SamplingCompressor(100, callback, KisSignalCompressor::FIRST_ACTIVE));
}


KisToolPaint::~KisToolPaint()
{
}

int KisToolPaint::flags() const
{
    return KisTool::FLAG_USES_CUSTOM_COMPOSITEOP;
}

void KisToolPaint::canvasResourceChanged(int key, const QVariant& v)
{
    KisTool::canvasResourceChanged(key, v);

    switch(key) {
    case(KoCanvasResource::Opacity):
        setOpacity(v.toDouble());
        break;
    case(KoCanvasResource::CurrentPaintOpPreset):
        requestUpdateOutline(m_outlineDocPoint, 0);
        break;
    default: //nothing
        break;
    }

    connect(KisConfigNotifier::instance(), SIGNAL(configChanged()), SLOT(resetCursorStyle()), Qt::UniqueConnection);

}


void KisToolPaint::activate(const QSet<KoShape*> &shapes)
{
    if (currentPaintOpPreset()) {
        QString formattedBrushName = currentPaintOpPreset()->name().replace("_", " ");
        emit statusTextChanged(formattedBrushName);
    }

    KisTool::activate(shapes);
    if (flags() & KisTool::FLAG_USES_CUSTOM_SIZE) {
        connect(action("increase_brush_size"), SIGNAL(triggered()), SLOT(increaseBrushSize()), Qt::UniqueConnection);
        connect(action("decrease_brush_size"), SIGNAL(triggered()), SLOT(decreaseBrushSize()), Qt::UniqueConnection);
        connect(action("increase_brush_size"), SIGNAL(triggered()), this, SLOT(showBrushSize()));
        connect(action("decrease_brush_size"), SIGNAL(triggered()), this, SLOT(showBrushSize()));

    }

    KisCanvasResourceProvider *provider = qobject_cast<KisCanvas2*>(canvas())->viewManager()->canvasResourceProvider();
    if ( provider->currentPreset() == m_localPreset ) {
        m_oldOpacity = provider->opacity();
        provider->setOpacity(m_localOpacity);
    }
}

void KisToolPaint::deactivate()
{
    if (flags() & KisTool::FLAG_USES_CUSTOM_SIZE) {
        disconnect(action("increase_brush_size"), 0, this, 0);
        disconnect(action("decrease_brush_size"), 0, this, 0);
    }

    KisCanvasResourceProvider *provider = qobject_cast<KisCanvas2*>(canvas())->viewManager()->canvasResourceProvider();
    m_localOpacity = provider->opacity();
    m_localPreset = provider->currentPreset();
    provider->setOpacity(m_oldOpacity);

    KisTool::deactivate();
}

QPainterPath KisToolPaint::tryFixBrushOutline(const QPainterPath &originalOutline)
{
    KisConfig cfg(true);
    if (cfg.newOutlineStyle() == OUTLINE_NONE) return originalOutline;

    const qreal minThresholdSize = cfg.outlineSizeMinimum();

    /**
     * If the brush outline is bigger than the canvas itself (which
     * would make it invisible for a user in most of the cases) just
     * add a cross in the center of it
     */

    QSize widgetSize = canvas()->canvasWidget()->size();
    const int maxThresholdSum = widgetSize.width() + widgetSize.height();

    QPainterPath outline = originalOutline;
    QRectF boundingRect = outline.boundingRect();
    const qreal sum = boundingRect.width() + boundingRect.height();

    QPointF center = boundingRect.center();

    if (sum > maxThresholdSum) {
        const int hairOffset = 7;

        outline.moveTo(center.x(), center.y() - hairOffset);
        outline.lineTo(center.x(), center.y() + hairOffset);

        outline.moveTo(center.x() - hairOffset, center.y());
        outline.lineTo(center.x() + hairOffset, center.y());
    } else if (sum < minThresholdSize && !outline.isEmpty()) {
        outline = QPainterPath();
        outline.addEllipse(center, 0.5 * minThresholdSize, 0.5 * minThresholdSize);
    }

    return outline;
}

void KisToolPaint::paint(QPainter &gc, const KoViewConverter &converter)
{
    Q_UNUSED(converter);

    QPainterPath path = tryFixBrushOutline(pixelToView(m_currentOutline));
    paintToolOutline(&gc, path);

    if (m_showColorPreview) {
        const QRectF viewRect = converter.documentToView(m_oldColorPreviewRect);
        gc.fillRect(viewRect, m_colorPreviewCurrentColor);

        if (m_colorPreviewShowComparePlate) {
            const QRectF baseColorRect = converter.documentToView(m_oldColorPreviewBaseColorRect);
            gc.fillRect(baseColorRect, m_colorPreviewBaseColor);
        }
    }
}

void KisToolPaint::setMode(ToolMode mode)
{
    if(this->mode() == KisTool::PAINT_MODE &&
            mode != KisTool::PAINT_MODE) {

        // Let's add history information about recently used colors
        emit sigPaintingFinished();
    }

    KisTool::setMode(mode);
}

void KisToolPaint::activateSampleColor(AlternateAction action)
{
    m_showColorPreview = true;

    requestUpdateOutline(m_outlineDocPoint, 0);

    int resource = colorPreviewResourceId(action);
    KoColor color = canvas()->resourceManager()->koColorResource(resource);

    KisCanvas2 * kisCanvas = dynamic_cast<KisCanvas2*>(canvas());
    KIS_ASSERT_RECOVER_RETURN(kisCanvas);

    m_colorPreviewCurrentColor = kisCanvas->displayColorConverter()->toQColor(color);

    if (!m_colorPreviewBaseColor.isValid()) {
        m_colorPreviewBaseColor = m_colorPreviewCurrentColor;
    }
}

void KisToolPaint::deactivateSampleColor(AlternateAction action)
{
    Q_UNUSED(action);

    m_showColorPreview = false;
    m_oldColorPreviewRect = QRect();
    m_colorPreviewCurrentColor = QColor();
}

void KisToolPaint::sampleColorWasOverridden()
{
    m_colorPreviewShowComparePlate = false;
    m_colorPreviewBaseColor = QColor();
}

void KisToolPaint::activateAlternateAction(AlternateAction action)
{
    switch (action) {
    case SampleFgNode:
        Q_FALLTHROUGH();
    case SampleBgNode:
        Q_FALLTHROUGH();
    case SampleFgImage:
        Q_FALLTHROUGH();
    case SampleBgImage:
        delayedAction = action;
        m_colorSamplerDelayTimer.start(100);
        Q_FALLTHROUGH();
    default:
        sampleColorWasOverridden();
        KisTool::activateAlternateAction(action);
    };
}

void KisToolPaint::activateSampleColorDelayed()
{
    switch (delayedAction) {
        case SampleFgNode:
        useCursor(KisCursor::samplerLayerForegroundCursor());
        activateSampleColor(delayedAction);
        break;
    case SampleBgNode:
        useCursor(KisCursor::samplerLayerBackgroundCursor());
        activateSampleColor(delayedAction);
        break;
    case SampleFgImage:
        useCursor(KisCursor::samplerImageForegroundCursor());
        activateSampleColor(delayedAction);
        break;
    case SampleBgImage:
        useCursor(KisCursor::samplerImageBackgroundCursor());
        activateSampleColor(delayedAction);
        break;
    default:
        break;
    };

    repaintDecorations();
}

bool KisToolPaint::isSamplingAction(AlternateAction action) {
    return action == SampleFgNode ||
        action == SampleBgNode ||
        action == SampleFgImage ||
        action == SampleBgImage;
}

void KisToolPaint::deactivateAlternateAction(AlternateAction action)
{
    if (!isSamplingAction(action)) {
        KisTool::deactivateAlternateAction(action);
        return;
    }

    delayedAction = KisTool::NONE;
    m_colorSamplerDelayTimer.stop();

    resetCursorStyle();
    deactivateSampleColor(action);
}

void KisToolPaint::addSamplerJob(const SamplingJob &samplingJob)
{
    /**
     * The actual sampling is delayed by a compressor, so we can get this
     * event when the stroke is already closed
     */
    if (!m_samplerStrokeId) return;

    KIS_ASSERT_RECOVER_RETURN(isSamplingAction(samplingJob.action));

    const QPoint imagePoint = image()->documentToImagePixelFloored(samplingJob.documentPixel);
    const bool fromCurrentNode = samplingJob.action == SampleFgNode || samplingJob.action == SampleBgNode;
    m_samplingResource = colorPreviewResourceId(samplingJob.action);

    if (!fromCurrentNode) {
        auto *kisCanvas = dynamic_cast<KisCanvas2*>(canvas());
        KIS_SAFE_ASSERT_RECOVER_RETURN(kisCanvas);
        KisSharedPtr<KisReferenceImagesLayer> referencesLayer = kisCanvas->imageView()->document()->referenceImagesLayer();
        if (referencesLayer && kisCanvas->referenceImagesDecoration()->visible()) {
            QColor color = referencesLayer->getPixel(imagePoint);
            if (color.isValid() && color.alpha() != 0) {
                slotColorSamplingFinished(KoColor(color, image()->colorSpace()));
                return;
            }
        }
    }

    KisPaintDeviceSP device = fromCurrentNode ?
        currentNode()->colorSampleSourceDevice() : image()->projection();

    if (device) {
        // Used for color sampler blending.
        KoColor currentColor = canvas()->resourceManager()->foregroundColor();
        if( samplingJob.action == SampleBgNode || samplingJob.action == SampleBgImage ){
            currentColor = canvas()->resourceManager()->backgroundColor();
        }

        image()->addJob(m_samplerStrokeId,
                        new KisColorSamplerStrokeStrategy::Data(device, imagePoint, currentColor));
    } else {
        KisCanvas2 *kiscanvas = static_cast<KisCanvas2 *>(canvas());
        QString message = i18n("Color sampler does not work on this layer.");
        kiscanvas->viewManager()->showFloatingMessage(message, koIcon("object-locked"));
    }
}

void KisToolPaint::beginAlternateAction(KoPointerEvent *event, AlternateAction action)
{
    if (isSamplingAction(action)) {
        KIS_ASSERT_RECOVER_RETURN(!m_samplerStrokeId);
        setMode(SECONDARY_PAINT_MODE);

        KisColorSamplerStrokeStrategy *strategy = new KisColorSamplerStrokeStrategy();
        connect(strategy, &KisColorSamplerStrokeStrategy::sigColorUpdated,
                this, &KisToolPaint::slotColorSamplingFinished);

        m_samplerStrokeId = image()->startStroke(strategy);
        m_colorSamplingCompressor->start(SamplingJob(event->point, action));
        requestUpdateOutline(event->point, event);
    } else {
        KisTool::beginAlternateAction(event, action);
    }
}

void KisToolPaint::continueAlternateAction(KoPointerEvent *event, AlternateAction action)
{
    if (isSamplingAction(action)) {
        KIS_ASSERT_RECOVER_RETURN(m_samplerStrokeId);
        m_colorSamplingCompressor->start(SamplingJob(event->point, action));
        requestUpdateOutline(event->point, event);
    } else {
        KisTool::continueAlternateAction(event, action);
    }
}

void KisToolPaint::endAlternateAction(KoPointerEvent *event, AlternateAction action)
{
    if (isSamplingAction(action)) {
        KIS_ASSERT_RECOVER_RETURN(m_samplerStrokeId);
        image()->endStroke(m_samplerStrokeId);
        m_samplerStrokeId.clear();
        requestUpdateOutline(event->point, event);
        setMode(HOVER_MODE);
    } else {
        KisTool::endAlternateAction(event, action);
    }
}

int KisToolPaint::colorPreviewResourceId(AlternateAction action)
{
    bool toForegroundColor = action == SampleFgNode || action == SampleFgImage;
    int resource = toForegroundColor ?
        KoCanvasResource::ForegroundColor : KoCanvasResource::BackgroundColor;

    return resource;
}

void KisToolPaint::slotColorSamplingFinished(KoColor color)
{
    color.setOpacity(OPACITY_OPAQUE_U8);
    canvas()->resourceManager()->setResource(m_samplingResource, color);

    if (!m_showColorPreview) return;

    KisCanvas2 * kisCanvas = dynamic_cast<KisCanvas2*>(canvas());
    KIS_ASSERT_RECOVER_RETURN(kisCanvas);
    QColor previewColor = kisCanvas->displayColorConverter()->toQColor(color);

    m_colorPreviewShowComparePlate = true;
    m_colorPreviewCurrentColor = previewColor;

    requestUpdateOutline(m_outlineDocPoint, 0);
}

void KisToolPaint::mousePressEvent(KoPointerEvent *event)
{
    KisTool::mousePressEvent(event);
    if (mode() == KisTool::HOVER_MODE) {
        requestUpdateOutline(event->point, event);
    }
}

void KisToolPaint::mouseMoveEvent(KoPointerEvent *event)
{
    KisTool::mouseMoveEvent(event);
    if (mode() == KisTool::HOVER_MODE) {
        requestUpdateOutline(event->point, event);
    }
}

KisPopupWidgetInterface *KisToolPaint::popupWidget()
{
    KisCanvas2 *kisCanvas = dynamic_cast<KisCanvas2*>(canvas());

    if (!kisCanvas) {
        return nullptr;
    }

    KisPopupWidgetInterface* popupWidget = kisCanvas->popupPalette();
    return popupWidget;
}

void KisToolPaint::mouseReleaseEvent(KoPointerEvent *event)
{
    KisTool::mouseReleaseEvent(event);
    if (mode() == KisTool::HOVER_MODE) {
        requestUpdateOutline(event->point, event);
    }
}

QWidget *KisToolPaint::createOptionWidget()
{
    QWidget *optionWidget = new QWidget();
    optionWidget->setObjectName(toolId());

    QVBoxLayout *verticalLayout = new QVBoxLayout(optionWidget);
    verticalLayout->setObjectName("KisToolPaint::OptionWidget::VerticalLayout");
    verticalLayout->setContentsMargins(0,0,0,0);
    verticalLayout->setSpacing(5);

    // See https://bugs.kde.org/show_bug.cgi?id=316896
    QWidget *specialSpacer = new QWidget(optionWidget);
    specialSpacer->setObjectName("SpecialSpacer");
    specialSpacer->setFixedSize(0, 0);
    verticalLayout->addWidget(specialSpacer);
    verticalLayout->addWidget(specialSpacer);

    m_optionsWidgetLayout = new QGridLayout();
    m_optionsWidgetLayout->setColumnStretch(1, 1);
    verticalLayout->addLayout(m_optionsWidgetLayout);
    m_optionsWidgetLayout->setContentsMargins(0,0,0,0);
    m_optionsWidgetLayout->setSpacing(5);

    if (!quickHelp().isEmpty()) {
        QPushButton *push = new QPushButton(KisIconUtils::loadIcon("help-contents"), QString(), optionWidget);
        connect(push, SIGNAL(clicked()), this, SLOT(slotPopupQuickHelp()));
        QHBoxLayout *hLayout = new QHBoxLayout();
        hLayout->addWidget(push);
        hLayout->addItem(new QSpacerItem(0, 0, QSizePolicy::Expanding, QSizePolicy::Fixed));
        verticalLayout->addLayout(hLayout);
    }

    return optionWidget;
}

QWidget* findLabelWidget(QGridLayout *layout, QWidget *control)
{
    QWidget *result = 0;

    int index = layout->indexOf(control);

    int row, col, rowSpan, colSpan;
    layout->getItemPosition(index, &row, &col, &rowSpan, &colSpan);

    if (col > 0) {
        QLayoutItem *item = layout->itemAtPosition(row, col - 1);

        if (item) {
            result = item->widget();
        }
    } else {
        QLayoutItem *item = layout->itemAtPosition(row, col + 1);
        if (item) {
            result = item->widget();
        }
    }

    return result;
}

void KisToolPaint::showControl(QWidget *control, bool value)
{
    control->setVisible(value);
    QWidget *label = findLabelWidget(m_optionsWidgetLayout, control);
    if (label) {
        label->setVisible(value);
    }
}

void KisToolPaint::enableControl(QWidget *control, bool value)
{
    control->setEnabled(value);
    QWidget *label = findLabelWidget(m_optionsWidgetLayout, control);
    if (label) {
        label->setEnabled(value);
    }
}

void KisToolPaint::addOptionWidgetLayout(QLayout *layout)
{
    Q_ASSERT(m_optionsWidgetLayout != 0);
    int rowCount = m_optionsWidgetLayout->rowCount();
    m_optionsWidgetLayout->addLayout(layout, rowCount, 0, 1, 2);
}


void KisToolPaint::addOptionWidgetOption(QWidget *control, QWidget *label)
{
    Q_ASSERT(m_optionsWidgetLayout != 0);
    if (label) {
        m_optionsWidgetLayout->addWidget(label, m_optionsWidgetLayout->rowCount(), 0);
        m_optionsWidgetLayout->addWidget(control, m_optionsWidgetLayout->rowCount() - 1, 1);
    }
    else {
        m_optionsWidgetLayout->addWidget(control, m_optionsWidgetLayout->rowCount(), 0, 1, 2);
    }
}


void KisToolPaint::setOpacity(qreal opacity)
{
    m_opacity = quint8(opacity * OPACITY_OPAQUE_U8);
}

const KoCompositeOp* KisToolPaint::compositeOp()
{
    if (currentNode()) {
        KisPaintDeviceSP device = currentNode()->paintDevice();
        if (device) {
            QString op = canvas()->resourceManager()->resource(KoCanvasResource::CurrentCompositeOp).toString();
            return device->colorSpace()->compositeOp(op);
        }
    }
    return 0;
}

void KisToolPaint::slotPopupQuickHelp()
{
    QWhatsThis::showText(QCursor::pos(), quickHelp());
}

void KisToolPaint::activatePrimaryAction()
{
    sampleColorWasOverridden();
    setOutlineEnabled(true);
    KisTool::activatePrimaryAction();
}

void KisToolPaint::deactivatePrimaryAction()
{
    setOutlineEnabled(false);
    KisTool::deactivatePrimaryAction();
}

bool KisToolPaint::isOutlineEnabled() const
{
    return m_isOutlineEnabled;
}

void KisToolPaint::setOutlineEnabled(bool value)
{
    m_isOutlineEnabled = value;
    requestUpdateOutline(m_outlineDocPoint, lastDeliveredPointerEvent());
}

void KisToolPaint::increaseBrushSize()
{
    qreal paintopSize = currentPaintOpPreset()->settings()->paintOpSize();

    std::vector<int>::iterator result =
        std::upper_bound(m_standardBrushSizes.begin(),
                         m_standardBrushSizes.end(),
                         qRound(paintopSize));

    int newValue = result != m_standardBrushSizes.end() ? *result : m_standardBrushSizes.back();

    currentPaintOpPreset()->settings()->setPaintOpSize(newValue);
    requestUpdateOutline(m_outlineDocPoint, 0);
}

void KisToolPaint::decreaseBrushSize()
{
    qreal paintopSize = currentPaintOpPreset()->settings()->paintOpSize();

    std::vector<int>::reverse_iterator result =
        std::upper_bound(m_standardBrushSizes.rbegin(),
                         m_standardBrushSizes.rend(),
                         (int)paintopSize,
                         std::greater<int>());

    int newValue = result != m_standardBrushSizes.rend() ? *result : m_standardBrushSizes.front();

    currentPaintOpPreset()->settings()->setPaintOpSize(newValue);
    requestUpdateOutline(m_outlineDocPoint, 0);
}

void KisToolPaint::showBrushSize()
{
     KisCanvas2 *kisCanvas =dynamic_cast<KisCanvas2*>(canvas());
     kisCanvas->viewManager()->showFloatingMessage(i18n("%1 %2 px", QString("Brush Size:"), currentPaintOpPreset()->settings()->paintOpSize())
                                                                   , QIcon(), 1000, KisFloatingMessage::High,  Qt::AlignLeft | Qt::TextWordWrap | Qt::AlignVCenter);
}

std::pair<QRectF,QRectF> KisToolPaint::colorPreviewDocRect(const QPointF &outlineDocPoint)
{
    if (!m_showColorPreview) return std::make_pair(QRectF(), QRectF());

    KisConfig cfg(true);

    const QRectF colorPreviewViewRect = cfg.colorPreviewRect();

    const QRectF colorPreviewBaseColorViewRect =
        m_colorPreviewShowComparePlate ?
            colorPreviewViewRect.translated(colorPreviewViewRect.width(), 0) :
            QRectF();

    const QRectF colorPreviewDocumentRect = canvas()->viewConverter()->viewToDocument(colorPreviewViewRect);
    const QRectF colorPreviewBaseColorDocumentRect =
        canvas()->viewConverter()->viewToDocument(colorPreviewBaseColorViewRect);

    return std::make_pair(colorPreviewDocumentRect.translated(outlineDocPoint),
                          colorPreviewBaseColorDocumentRect.translated(outlineDocPoint));
}

void KisToolPaint::requestUpdateOutline(const QPointF &outlineDocPoint, const KoPointerEvent *event)
{
    QRectF outlinePixelRect;
    QRectF outlineDocRect;

    QRectF colorPreviewDocRect;
    QRectF colorPreviewBaseColorDocRect;
    QRectF colorPreviewDocUpdateRect;

    if (m_supportOutline) {
        KisConfig cfg(true);
        KisPaintOpSettings::OutlineMode outlineMode;

        if (isOutlineEnabled() &&
                (mode() == KisTool::GESTURE_MODE ||
                 ((cfg.newOutlineStyle() == OUTLINE_FULL ||
                   cfg.newOutlineStyle() == OUTLINE_CIRCLE ||
                   cfg.newOutlineStyle() == OUTLINE_TILT) &&
                  ((mode() == HOVER_MODE) ||
                   (mode() == PAINT_MODE && cfg.showOutlineWhilePainting()))))) { // lisp forever!

            outlineMode.isVisible = true;

            if (cfg.newOutlineStyle() == OUTLINE_CIRCLE) {
                outlineMode.forceCircle = true;
            } else if(cfg.newOutlineStyle() == OUTLINE_TILT) {
                outlineMode.forceCircle = true;
                outlineMode.showTiltDecoration = true;
            } else {
                // noop
            }
        }

        outlineMode.forceFullSize = cfg.forceAlwaysFullSizedOutline();

        m_outlineDocPoint = outlineDocPoint;
        m_currentOutline = getOutlinePath(m_outlineDocPoint, event, outlineMode);

        outlinePixelRect = tryFixBrushOutline(m_currentOutline).boundingRect();
        outlineDocRect = currentImage()->pixelToDocument(outlinePixelRect);

        // This adjusted call is needed as we paint with a 3 pixel wide brush and the pen is outside the bounds of the path
        // Pen uses view coordinates so we have to zoom the document value to match 2 pixel in view coordinates
        // See BUG 275829
        qreal zoomX;
        qreal zoomY;
        canvas()->viewConverter()->zoom(&zoomX, &zoomY);
        qreal xoffset = 2.0/zoomX;
        qreal yoffset = 2.0/zoomY;

        if (!outlineDocRect.isEmpty()) {
            outlineDocRect.adjust(-xoffset,-yoffset,xoffset,yoffset);
        }

        std::tie(colorPreviewDocRect, colorPreviewBaseColorDocRect) =
                this->colorPreviewDocRect(m_outlineDocPoint);

        colorPreviewDocUpdateRect = colorPreviewDocRect | colorPreviewBaseColorDocRect;

        if (!colorPreviewDocUpdateRect.isEmpty()) {
            colorPreviewDocUpdateRect = colorPreviewDocUpdateRect.adjusted(-xoffset,-yoffset,xoffset,yoffset);
        }

    }

    // DIRTY HACK ALERT: we should fetch the assistant's dirty rect when requesting
    //                   the update, instead of just dumbly update the entire canvas!

    // WARNING: assistants code is also duplicated in KisDelegatedSelectPathWrapper::mouseMoveEvent

    KisCanvas2 * kiscanvas = dynamic_cast<KisCanvas2*>(canvas());
    KisPaintingAssistantsDecorationSP decoration = kiscanvas->paintingAssistantsDecoration();
    if (decoration && decoration->visible() && decoration->hasPaintableAssistants()) {
        kiscanvas->updateCanvas();
    } else {
        // TODO: only this branch should be present!
        if (!m_oldColorPreviewUpdateRect.isEmpty()) {
            canvas()->updateCanvas(m_oldColorPreviewUpdateRect);
        }

        if (!m_oldOutlineRect.isEmpty()) {
            canvas()->updateCanvas(m_oldOutlineRect);
        }

        if (!outlineDocRect.isEmpty()) {
            canvas()->updateCanvas(outlineDocRect);
        }

        if (!colorPreviewDocUpdateRect.isEmpty()) {
            canvas()->updateCanvas(colorPreviewDocUpdateRect);
        }
    }

    m_oldOutlineRect = outlineDocRect;
    m_oldColorPreviewRect = colorPreviewDocRect;
    m_oldColorPreviewBaseColorRect = colorPreviewBaseColorDocRect;
    m_oldColorPreviewUpdateRect = colorPreviewDocUpdateRect;
}

QPainterPath KisToolPaint::getOutlinePath(const QPointF &documentPos,
                                          const KoPointerEvent *event,
                                          KisPaintOpSettings::OutlineMode outlineMode)
{
    Q_UNUSED(event);

    KisCanvas2 *canvas2 = dynamic_cast<KisCanvas2 *>(canvas());
    const KisCoordinatesConverter *converter = canvas2->coordinatesConverter();

    KisPaintInformation info(convertToPixelCoord(documentPos));
    info.setCanvasMirroredH(canvas2->coordinatesConverter()->xAxisMirrored());
    info.setCanvasMirroredV(canvas2->coordinatesConverter()->yAxisMirrored());
    info.setCanvasRotation(canvas2->coordinatesConverter()->rotationAngle());
    info.setRandomSource(new KisRandomSource());
    info.setPerStrokeRandomSource(new KisPerStrokeRandomSource());

    QPainterPath path = currentPaintOpPreset()->settings()->
        brushOutline(info,
                     outlineMode, converter->effectiveZoom());

    return path;
}

