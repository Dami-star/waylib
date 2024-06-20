// Copyright (C) 2023 JiDe Zhang <zhangjide@deepin.org>.
// SPDX-License-Identifier: Apache-2.0 OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "helper.h"

#include <WServer>
#include <WOutput>
#include <WSurfaceItem>
#include <wxdgsurface.h>
#include <wrenderhelper.h>
#include <WBackend>
#include <wxdgshell.h>
#include <wxwayland.h>
#include <woutputitem.h>
#include <wquickcursor.h>
#include <woutputrenderwindow.h>
#include <wqmldynamiccreator.h>
#include <WXdgOutput>

#include <qwbackend.h>
#include <qwdisplay.h>
#include <qwoutput.h>
#include <qwlogging.h>
#include <qwallocator.h>
#include <qwrenderer.h>
#include <qwcompositor.h>
#include <qwsubcompositor.h>

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QProcess>
#include <QMouseEvent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QLoggingCategory>
#include <QKeySequence>
#include <QQmlComponent>

extern "C" {
#define static
#include <wlr/types/wlr_output.h>
#undef static
}

inline QPointF getItemGlobalPosition(QQuickItem *item)
{
    auto parent = item->parentItem();
    return parent ? parent->mapToGlobal(item->position()) : item->position();
}

Helper::Helper(QObject *parent)
    : WSeatEventFilter(parent)
    , m_outputLayout(new WQuickOutputLayout(this))
    , m_cursor(new WQuickCursor(this))
    , m_seat(new WSeat())
    , m_outputCreator(new WQmlCreator(this))
    , m_xdgoutputmanager(new WXdgOutputManager())
    , m_xwayland_xdgoutputmanager(new WXdgOutputManager())
{
    m_seat->setEventFilter(this);
    m_seat->setCursor(m_cursor);
    m_cursor->setThemeName(getenv("XCURSOR_THEME"));
    m_cursor->setLayout(m_outputLayout);

    m_xwayland_xdgoutputmanager->setLayout(m_outputLayout);
    m_xwayland_xdgoutputmanager->setScaleOverride(1.0);
    m_xdgoutputmanager->setTargetClients(m_xwayland_xdgoutputmanager->targetClients(), false);

    m_xdgoutputmanager->setLayout(m_outputLayout);
    m_xdgoutputmanager->setTargetClients(m_xwayland_xdgoutputmanager->targetClients(),true);
}

void Helper::initProtocols(WServer *server, WOutputRenderWindow *window, QQmlEngine *qmlEngine)
{
    auto backend = server->attach<WBackend>();
    m_renderer = WRenderHelper::createRenderer(backend->handle());

    if (!m_renderer) {
        qFatal("Failed to create renderer");
    }

    m_allocator = QWAllocator::autoCreate(backend->handle(), m_renderer);
    m_renderer->initWlDisplay(server->handle());

    // free follow display
    m_compositor = QWCompositor::create(server->handle(), m_renderer, 6);
    QWSubcompositor::create(server->handle());

    server->attach<WXdgShell>();
    server->attach(m_seat);

    connect(backend, &WBackend::outputAdded, this, [backend, this, window, qmlEngine] (WOutput *output) {
        if (!backend->hasDrm())
            output->setForceSoftwareCursor(true); // Test
        allowNonDrmOutputAutoChangeMode(output);

        auto initProperties = qmlEngine->newObject();
        initProperties.setProperty("waylandOutput", qmlEngine->toScriptValue(output));
        initProperties.setProperty("waylandCursor", qmlEngine->toScriptValue(m_cursor));
        initProperties.setProperty("layout", qmlEngine->toScriptValue(outputLayout()));
        initProperties.setProperty("x", qmlEngine->toScriptValue(outputLayout()->implicitWidth()));

        m_outputCreator->add(output, initProperties);
    });

    connect(backend, &WBackend::outputRemoved, this, [this] (WOutput *output) {
        m_outputCreator->removeByOwner(output);
    });

    connect(backend, &WBackend::inputAdded, this, [this] (WInputDevice *device) {
        m_seat->attachInputDevice(device);
    });

    connect(backend, &WBackend::inputRemoved, this, [this] (WInputDevice *device) {
        m_seat->detachInputDevice(device);
    });

    Q_EMIT compositorChanged();

    window->init(m_renderer, m_allocator);
    backend->handle()->start();
}

WQuickOutputLayout *Helper::outputLayout() const
{
    return m_outputLayout;
}

WSeat *Helper::seat() const
{
    return m_seat;
}

QWCompositor *Helper::compositor() const
{
    return m_compositor;
}

WQmlCreator *Helper::outputCreator() const
{
    return m_outputCreator;
}

WSurfaceItem *Helper::resizingItem() const
{
    return moveReiszeState.resizingItem;
}

void Helper::setResizingItem(WSurfaceItem *newResizingItem)
{
    if (moveReiszeState.resizingItem == newResizingItem)
        return;
    moveReiszeState.resizingItem = newResizingItem;
    emit resizingItemChanged();
}

WSurfaceItem *Helper::movingItem() const
{
    return moveReiszeState.movingItem;
}

bool Helper::registerExclusiveZone(WLayerSurface *layerSurface)
{
    auto [ output, infoPtr ] = getFirstOutputOfSurface(layerSurface);
    if (!output)
        return 0;

    auto exclusiveZone = layerSurface->exclusiveZone();
    auto exclusiveEdge = layerSurface->getExclusiveZoneEdge();

    if (exclusiveZone <= 0 || exclusiveEdge == WLayerSurface::AnchorType::None)
        return false;

    QListIterator<std::tuple<WLayerSurface*, uint32_t, WLayerSurface::AnchorType>> listIter(infoPtr->registeredSurfaceList);
    while (listIter.hasNext()) {
        if (std::get<WLayerSurface*>(listIter.next()) == layerSurface)
            return false;
    }

    infoPtr->registeredSurfaceList.append(std::make_tuple(layerSurface, exclusiveZone, exclusiveEdge));
    switch(exclusiveEdge) {
        using enum WLayerSurface::AnchorType;
    case Top:
        infoPtr->m_topExclusiveMargin += exclusiveZone;
        Q_EMIT topExclusiveMarginChanged();
        break;
    case Bottom:
        infoPtr->m_bottomExclusiveMargin += exclusiveZone;
        Q_EMIT bottomExclusiveMarginChanged();
        break;
    case Left:
        infoPtr->m_leftExclusiveMargin += exclusiveZone;
        Q_EMIT leftExclusiveMarginChanged();
        break;
    case Right:
        infoPtr->m_rightExclusiveMargin += exclusiveZone;
        Q_EMIT rightExclusiveMarginChanged();
        break;
    default:
        Q_UNREACHABLE();
    }
    return true;
}

bool Helper::unregisterExclusiveZone(WLayerSurface *layerSurface)
{
    auto [ output, infoPtr ] = getFirstOutputOfSurface(layerSurface);
    if (!output)
        return 0;

    QMutableListIterator<std::tuple<WLayerSurface*, uint32_t, WLayerSurface::AnchorType>> listIter(infoPtr->registeredSurfaceList);
    while (listIter.hasNext()) {
        auto [ registeredSurface, exclusiveZone, exclusiveEdge ] = listIter.next();
        if (registeredSurface == layerSurface) {
            listIter.remove();

            switch(exclusiveEdge) {
                using enum WLayerSurface::AnchorType;
            case Top:
                infoPtr->m_topExclusiveMargin -= exclusiveZone;
                Q_EMIT topExclusiveMarginChanged();
                break;
            case Bottom:
                infoPtr->m_bottomExclusiveMargin -= exclusiveZone;
                Q_EMIT bottomExclusiveMarginChanged();
                break;
            case Left:
                infoPtr->m_leftExclusiveMargin -= exclusiveZone;
                Q_EMIT leftExclusiveMarginChanged();
                break;
            case Right:
                infoPtr->m_rightExclusiveMargin -= exclusiveZone;
                Q_EMIT rightExclusiveMarginChanged();
                break;
            default:
                Q_UNREACHABLE();
            }
            return true;
        }
    }

    return false;
}

QJSValue Helper::getExclusiveMargins(WLayerSurface *layerSurface)
{
    auto [ output, infoPtr ] = getFirstOutputOfSurface(layerSurface);
    QMargins margins{0, 0, 0, 0};

    if (output) {
        QMutableListIterator<std::tuple<WLayerSurface*, uint32_t, WLayerSurface::AnchorType>> listIter(infoPtr->registeredSurfaceList);
        while (listIter.hasNext()) {
            auto [ registeredSurface, exclusiveZone, exclusiveEdge ] = listIter.next();
            if (registeredSurface == layerSurface)
                break;
            switch(exclusiveEdge) {
                using enum WLayerSurface::AnchorType;
            case Top:
                margins.setTop(margins.top() + exclusiveZone);
                break;
            case Bottom:
                margins.setBottom(margins.bottom() + exclusiveZone);
                break;
            case Left:
                margins.setLeft(margins.left() + exclusiveZone);
                break;
            case Right:
                margins.setRight(margins.right() + exclusiveZone);
                break;
            default:
                Q_UNREACHABLE();
            }
        }
    }

    QJSValue jsMargins = qmlEngine(this)->newObject(); // Can't use QMargins in QML
    jsMargins.setProperty("top" , margins.top());
    jsMargins.setProperty("bottom", margins.bottom());
    jsMargins.setProperty("left", margins.left());
    jsMargins.setProperty("right", margins.right());
    return jsMargins;
}

quint32 Helper::getTopExclusiveMargin(WToplevelSurface *layerSurface)
{
    auto [ _, infoPtr ] = getFirstOutputOfSurface(layerSurface);
    if (!infoPtr)
        return 0;
    return infoPtr->m_topExclusiveMargin;
}

quint32 Helper::getBottomExclusiveMargin(WToplevelSurface *layerSurface)
{
    auto [ _, infoPtr ] = getFirstOutputOfSurface(layerSurface);
    if (!infoPtr)
        return 0;
    return infoPtr->m_bottomExclusiveMargin;
}

quint32 Helper::getLeftExclusiveMargin(WToplevelSurface *layerSurface)
{
    auto [ _, infoPtr ] = getFirstOutputOfSurface(layerSurface);
    if (!infoPtr)
        return 0;
    return infoPtr->m_leftExclusiveMargin;
}

quint32 Helper::getRightExclusiveMargin(WToplevelSurface *layerSurface)
{
    auto [ _, infoPtr ] = getFirstOutputOfSurface(layerSurface);
    if (!infoPtr)
        return 0;
    return infoPtr->m_rightExclusiveMargin;
}

void Helper::onSurfaceEnterOutput(WToplevelSurface *surface, WSurfaceItem *surfaceItem, WOutput *output)
{
    auto *info = getOutputInfo(output);
    info->surfaceList.append(surface);
    info->surfaceItemList.append(surfaceItem);
}

void Helper::onSurfaceLeaveOutput(WToplevelSurface *surface, WSurfaceItem *surfaceItem, WOutput *output)
{
    auto *info = getOutputInfo(output);
    info->surfaceList.removeOne(surface);
    info->surfaceItemList.removeOne(surfaceItem);
    // should delete OutputInfo if no surface?
}

std::pair<WOutput*,OutputInfo*> Helper::getFirstOutputOfSurface(WToplevelSurface *surface)
{
    for (auto zoneInfo: m_outputExclusiveZoneInfo) {
        if (std::get<OutputInfo*>(zoneInfo)->surfaceList.contains(surface))
            return zoneInfo;
    }
    return std::make_pair(nullptr, nullptr);
}


void Helper::setMovingItem(WSurfaceItem *newMovingItem)
{
    if (moveReiszeState.movingItem == newMovingItem)
        return;
    moveReiszeState.movingItem = newMovingItem;
    emit movingItemChanged();
}

void Helper::stopMoveResize()
{
    if (moveReiszeState.surface)
        moveReiszeState.surface->setResizeing(false);

    setResizingItem(nullptr);
    setMovingItem(nullptr);

    moveReiszeState.surfaceItem = nullptr;
    moveReiszeState.surface = nullptr;
    moveReiszeState.seat = nullptr;
    moveReiszeState.resizeEdgets = {0};
}

void Helper::startMove(WToplevelSurface *surface, WSurfaceItem *shell, WSeat *seat, int serial)
{
    stopMoveResize();

    Q_UNUSED(serial)

    moveReiszeState.surfaceItem = shell;
    moveReiszeState.surface = surface;
    moveReiszeState.seat = seat;
    moveReiszeState.resizeEdgets = {0};
    moveReiszeState.surfacePosOfStartMoveResize = getItemGlobalPosition(moveReiszeState.surfaceItem);

    setMovingItem(shell);
}

void Helper::startResize(WToplevelSurface *surface, WSurfaceItem *shell, WSeat *seat, Qt::Edges edge, int serial)
{
    stopMoveResize();

    Q_UNUSED(serial)
    Q_ASSERT(edge != 0);

    moveReiszeState.surfaceItem = shell;
    moveReiszeState.surface = surface;
    moveReiszeState.seat = seat;
    moveReiszeState.surfacePosOfStartMoveResize = getItemGlobalPosition(moveReiszeState.surfaceItem);
    moveReiszeState.surfaceSizeOfStartMoveResize = moveReiszeState.surfaceItem->size();
    moveReiszeState.resizeEdgets = edge;

    surface->setResizeing(true);
    setResizingItem(shell);
}

void Helper::cancelMoveResize(WSurfaceItem *shell)
{
    if (moveReiszeState.surfaceItem != shell)
        return;
    stopMoveResize();
}

bool Helper::startDemoClient(const QString &socket)
{
#ifdef START_DEMO
    QProcess waylandClientDemo;

    waylandClientDemo.setProgram("qml");
    waylandClientDemo.setArguments({"-a", "widget", SOURCE_DIR"/ClientWindow.qml", "-platform", "wayland"});
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("WAYLAND_DISPLAY", socket);

    waylandClientDemo.setProcessEnvironment(env);
    return waylandClientDemo.startDetached();
#endif
    return false;
}

WSurface *Helper::getFocusSurfaceFrom(QObject *object)
{
    auto item = WSurfaceItem::fromFocusObject(object);
    return item ? item->surface() : nullptr;
}

void Helper::allowNonDrmOutputAutoChangeMode(WOutput *output)
{
    output->safeConnect(&QWOutput::requestState, this, &Helper::onOutputRequeseState);
}

void Helper::enableOutput(WOutput *output)
{
    // Enable on default
    auto qwoutput = output->handle();
    // Don't care for WOutput::isEnabled, must do WOutput::commit here,
    // In order to ensure trigger QWOutput::frame signal, WOutputRenderWindow
    // needs this signal to render next frmae. Because QWOutput::frame signal
    // maybe emit before WOutputRenderWindow::attach, if no commit here,
    // WOutputRenderWindow will ignore this ouptut on render.
    if (!qwoutput->property("_Enabled").toBool()) {
        qwoutput->setProperty("_Enabled", true);

        if (!qwoutput->handle()->current_mode) {
            auto mode = qwoutput->preferredMode();
            if (mode)
                output->setMode(mode);
        }
        output->enable(true);
        bool ok = output->commit();
        Q_ASSERT(ok);
    }
}

bool Helper::beforeDisposeEvent(WSeat *seat, QWindow *watched, QInputEvent *event)
{
    if (event->type() == QEvent::KeyPress) {
        auto kevent = static_cast<QKeyEvent*>(event);
        if (QKeySequence(kevent->keyCombination()) == QKeySequence::Quit) {
            qApp->quit();
            return true;
        }
    }

    if (watched) {
        if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::TouchBegin) {
            seat->setKeyboardFocusWindow(watched);
        } else if (event->type() == QEvent::MouseMove && !seat->keyboardFocusWindow()) {
            // TouchMove keep focus on first window
            seat->setKeyboardFocusWindow(watched);
        }
    }

    if (event->type() == QEvent::MouseMove || event->type() == QEvent::MouseButtonPress) {
        seat->cursor()->setVisible(true);
    } else if (event->type() == QEvent::TouchBegin) {
        seat->cursor()->setVisible(false);
    }

    if (moveReiszeState.surfaceItem && (seat == moveReiszeState.seat || moveReiszeState.seat == nullptr)) {
        // for move resize
        if (Q_LIKELY(event->type() == QEvent::MouseMove || event->type() == QEvent::TouchUpdate)) {
            auto cursor = seat->cursor();
            Q_ASSERT(cursor);
            QMouseEvent *ev = static_cast<QMouseEvent*>(event);

            if (moveReiszeState.resizeEdgets == 0) {
                auto increment_pos = ev->globalPosition() - cursor->lastPressedOrTouchDownPosition();
                auto new_pos = moveReiszeState.surfacePosOfStartMoveResize + moveReiszeState.surfaceItem->parentItem()->mapFromGlobal(increment_pos);
                moveReiszeState.surfaceItem->setPosition(new_pos);
            } else {
                auto increment_pos = moveReiszeState.surfaceItem->parentItem()->mapFromGlobal(ev->globalPosition() - cursor->lastPressedOrTouchDownPosition());
                QRectF geo(moveReiszeState.surfacePosOfStartMoveResize, moveReiszeState.surfaceSizeOfStartMoveResize);

                if (moveReiszeState.resizeEdgets & Qt::LeftEdge)
                    geo.setLeft(geo.left() + increment_pos.x());
                if (moveReiszeState.resizeEdgets & Qt::TopEdge)
                    geo.setTop(geo.top() + increment_pos.y());

                if (moveReiszeState.resizeEdgets & Qt::RightEdge)
                    geo.setRight(geo.right() + increment_pos.x());
                if (moveReiszeState.resizeEdgets & Qt::BottomEdge)
                    geo.setBottom(geo.bottom() + increment_pos.y());

                if (moveReiszeState.surfaceItem->resizeSurface(geo.size().toSize()))
                    moveReiszeState.surfaceItem->setPosition(geo.topLeft());
            }

            return true;
        } else if (event->type() == QEvent::MouseButtonRelease || event->type() == QEvent::TouchEnd) {
            stopMoveResize();
        }
    }

    return false;
}

bool Helper::afterHandleEvent(WSeat *seat, WSurface *watched, QObject *surfaceItem, QObject *, QInputEvent *event)
{
    Q_UNUSED(seat)

    if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::TouchBegin) {
        // surfaceItem is qml type: XdgSurfaceItem or LayerSurfaceItem
        auto toplevelSurface = qobject_cast<WSurfaceItem*>(surfaceItem)->shellSurface();
        if (!toplevelSurface)
            return false;
        Q_ASSERT(toplevelSurface->surface() == watched);
        if (auto *xdgSurface = qobject_cast<WXdgSurface *>(toplevelSurface)) {
            // TODO: popupSurface should not inherit WToplevelSurface
            if (xdgSurface->isPopup()) {
                return false;
            }
        }
        setActivateSurface(toplevelSurface);
    }

    return false;
}

bool Helper::unacceptedEvent(WSeat *, QWindow *, QInputEvent *event)
{
    if (event->isSinglePointEvent()) {
        if (static_cast<QSinglePointEvent*>(event)->isBeginEvent())
            setActivateSurface(nullptr);
    }

    return false;
}

WToplevelSurface *Helper::activatedSurface() const
{
    return m_activateSurface;
}

void Helper::setActivateSurface(WToplevelSurface *newActivate)
{
    if (m_activateSurface == newActivate)
        return;

    if (newActivate && newActivate->doesNotAcceptFocus())
        return;

    if (m_activateSurface) {
        if (newActivate) {
            if (m_activateSurface->keyboardFocusPriority() > newActivate->keyboardFocusPriority())
                return;
        } else {
            if (m_activateSurface->keyboardFocusPriority() > 0)
                return;
        }

        m_activateSurface->setActivate(false);
    }
    m_activateSurface = newActivate;
    if (newActivate)
        newActivate->setActivate(true);
    Q_EMIT activatedSurfaceChanged();
}

void Helper::onOutputRequeseState(wlr_output_event_request_state *newState)
{
    if (newState->state->committed & WLR_OUTPUT_STATE_MODE) {
        auto output = qobject_cast<QWOutput*>(sender());

        if (newState->state->mode_type == WLR_OUTPUT_STATE_MODE_CUSTOM) {
            const QSize size(newState->state->custom_mode.width, newState->state->custom_mode.height);
            output->setCustomMode(size, newState->state->custom_mode.refresh);
        } else {
            output->setMode(newState->state->mode);
        }

        output->commit();
    }
}

OutputInfo* Helper::getOutputInfo(WOutput *output)
{
    for (const auto &[woutput, infoPtr]: m_outputExclusiveZoneInfo)
        if (woutput == output)
            return infoPtr;
    auto infoPtr = new OutputInfo;
    m_outputExclusiveZoneInfo.append(std::make_pair(output, infoPtr));
    return infoPtr;
}

int main(int argc, char *argv[]) {
    WRenderHelper::setupRendererBackend();

    QWLog::init();
    WServer::initializeQPA();
//    QQuickStyle::setStyle("Material");

    QGuiApplication::setAttribute(Qt::AA_UseOpenGLES);
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    QGuiApplication::setQuitOnLastWindowClosed(false);
    QGuiApplication app(argc, argv);

    QQmlApplicationEngine waylandEngine;

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    waylandEngine.loadFromModule("Tinywl", "Main");
#else
    waylandEngine.load(QUrl(u"qrc:/Tinywl/Main.qml"_qs));
#endif
    // TODO: direct new WServer in here
    WServer *server = waylandEngine.rootObjects().first()->findChild<WServer*>();
    Q_ASSERT(server);
    Q_ASSERT(server->isRunning());

    auto window = waylandEngine.rootObjects().first()->findChild<WOutputRenderWindow*>();
    Q_ASSERT(window);

    Helper *helper = waylandEngine.singletonInstance<Helper*>("Tinywl", "Helper");
    Q_ASSERT(helper);

    helper->initProtocols(server, window, &waylandEngine);

    // multi output
//    qobject_cast<QWMultiBackend*>(backend->backend())->forEachBackend([] (wlr_backend *backend, void *) {
//        if (auto x11 = QWX11Backend::from(backend))
//            x11->createOutput();
//    }, nullptr);

    return app.exec();
}
