#include "overlay.h"

#include "padserver.h"

#include <LayerShellQt/window.h>

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QQuickWindow>
#include <QScreen>
#include <QStandardPaths>
#include <QTabletEvent>
#include <QUrl>
#include <algorithm>

const QStringList Overlay::paletteColors{"#ff3b30", "#ff9f0a", "#ffd60a", "#30d158",
                                         "#0a84ff", "#bf5af2", "#ffffff", "#1c1c1e"};
const QList<double> Overlay::sizePresets{2, 3.5, 6, 10};
const QList<int> Overlay::nibAngles{15, 30, 45, 0};

Overlay::Overlay(InkModel *model, QObject *parent) : QObject(parent), m_model(model) {
    connect(m_model, &InkModel::historyChanged, this, &Overlay::historyChanged);
    connect(m_model, &InkModel::reset, this, &Overlay::historyChanged);
    connect(m_model, &InkModel::committed, this, &Overlay::historyChanged);
    connect(m_model, &InkModel::removed, this, &Overlay::historyChanged);
    connect(m_model, &InkModel::boardChanged, this, &Overlay::boardChanged);
}

QList<Overlay::Monitor> Overlay::monitors() {
    QList<Monitor> list;
    if (qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE"))
        return list;
    QProcess hyprctl;
    hyprctl.start("hyprctl", {"-j", "monitors"});
    if (!hyprctl.waitForFinished(1500) || hyprctl.exitCode() != 0)
        return list;
    for (const auto &value : QJsonDocument::fromJson(hyprctl.readAllStandardOutput()).array()) {
        const auto object = value.toObject();
        list.append({object.value("name").toString(), object.value("width").toInt(),
                     object.value("focused").toBool()});
    }
    return list;
}

QString Overlay::focusedOutput() {
    for (const auto &monitor : monitors())
        if (monitor.focused)
            return monitor.name;
    return {};
}

void Overlay::attach(QQuickWindow *window, const QString &preferredOutput) {
    m_window = window;
    auto *layer = LayerShellQt::Window::get(window);
    layer->setLayer(LayerShellQt::Window::LayerOverlay);
    layer->setAnchors(LayerShellQt::Window::Anchors(LayerShellQt::Window::AnchorTop | LayerShellQt::Window::AnchorBottom |
                                                    LayerShellQt::Window::AnchorLeft | LayerShellQt::Window::AnchorRight));
    // Covers the bar too, and doesn't push other windows aside: the ink has
    // to line up with the whole screen, which is what the tablet mirrors.
    layer->setExclusiveZone(-1);
    layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityNone);
    layer->setScope("omaink");
    layer->setActivateOnShow(false);
    layer->setWantsToBeOnActiveScreen(false);

    window->installEventFilter(this);
    connect(window, &QWindow::widthChanged, this, &Overlay::updateGeometry);
    connect(window, &QWindow::heightChanged, this, &Overlay::updateGeometry);

    moveTo(!preferredOutput.isEmpty() ? preferredOutput : focusedOutput());
    applyInputMode();
    window->show();
}

void Overlay::moveTo(const QString &output) {
    if (!m_window)
        return;
    QScreen *target = nullptr;
    for (QScreen *screen : QGuiApplication::screens())
        if (screen->name() == output)
            target = screen;
    if (!target)
        target = m_window->screen() ? m_window->screen() : QGuiApplication::primaryScreen();
    if (!target)
        return;

    auto *layer = LayerShellQt::Window::get(m_window);
    const bool wasVisible = m_window->isVisible();
    if (wasVisible && m_window->screen() == target && m_output == target->name())
        return;
    // A layer surface is bound to its screen when it's created, so moving
    // means hiding and showing it again on the new one.
    if (wasVisible)
        m_window->hide();
    layer->setScreen(target);
    m_window->setScreen(target);
    m_output = target->name();
    // hyprctl reports the screen's size in hardware pixels, which is what the
    // tablet mirror captures; Qt's own size is scaled.
    m_physicalWidth = int(target->geometry().width() * target->devicePixelRatio());
    for (const auto &monitor : monitors())
        if (monitor.name == m_output && monitor.width > 0)
            m_physicalWidth = monitor.width;
    if (wasVisible)
        m_window->show();
    updateGeometry();
    emit outputChanged();
}

void Overlay::updateGeometry() {
    if (!m_window || m_window->height() <= 0)
        return;
    const double aspect = double(m_window->width()) / m_window->height();
    m_model->setAspect(aspect);
    if (m_pad)
        m_pad->setOutput(m_output, m_window->width(), m_physicalWidth, aspect);
}

void Overlay::applyInputMode() {
    if (!m_window)
        return;
    // While not drawing, the overlay takes no clicks and no keys, so the
    // desktop works normally under the ink.
    m_window->setFlag(Qt::WindowTransparentForInput, !m_drawing);
    LayerShellQt::Window::get(m_window)->setKeyboardInteractivity(
        m_drawing ? LayerShellQt::Window::KeyboardInteractivityExclusive
                  : LayerShellQt::Window::KeyboardInteractivityNone);
    m_window->setCursor(m_drawing ? Qt::CrossCursor : Qt::ArrowCursor);
    if (m_drawing)
        m_window->requestActivate();
}

void Overlay::setDrawing(bool drawing) {
    if (drawing == m_drawing)
        return;
    if (drawing) {
        // Drawing starts on the screen you're working on.
        const QString focused = focusedOutput();
        if (!focused.isEmpty() && focused != m_output)
            moveTo(focused);
        setInkVisible(true);
    } else if (!m_localId.isEmpty()) {
        endLocal();
    }
    m_drawing = drawing;
    applyInputMode();
    emit drawingChanged();
}

void Overlay::setInkVisible(bool visible) {
    if (visible == m_visible)
        return;
    m_visible = visible;
    if (!visible && m_drawing)
        setDrawing(false);
    emit inkVisibleChanged();
}

void Overlay::setPadServer(PadServer *server, const QString &error) {
    m_pad = server;
    m_padError = error;
    if (m_pad) {
        connect(m_pad, &PadServer::clientsChanged, this, [this] {
            const int count = m_pad->clientCount();
            emit notify(count > m_knownClients ? tr("Tablet connected") : tr("Tablet disconnected"));
            m_knownClients = count;
            emit padChanged();
        });
        updateGeometry();
        makeQrCode();
    }
    emit padChanged();
}

QStringList Overlay::padUrls() const {
    QStringList urls;
    if (!m_pad || !m_pad->isListening())
        return urls;
    for (const auto &address : PadServer::addresses())
        urls.append(PadServer::url(address, m_pad->port(), m_pad->token()));
    return urls;
}

int Overlay::padClients() const { return m_pad ? m_pad->clientCount() : 0; }

void Overlay::makeQrCode() {
    const QString qrencode = QStandardPaths::findExecutable("qrencode");
    const QStringList urls = padUrls();
    if (qrencode.isEmpty() || urls.isEmpty())
        return;
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    const QString path = dir + "/omaink-pad.png";
    auto *process = new QProcess(this);
    connect(process, &QProcess::finished, this, [this, process, path](int code) {
        process->deleteLater();
        if (code == 0) {
            m_qrSource = QUrl::fromLocalFile(path).toString() + "?" +
                         QString::number(QDateTime::currentMSecsSinceEpoch());
            emit padChanged();
        }
    });
    process->start(qrencode, {"-o", path, "-s", "8", "-m", "2", "-l", "M", urls.first()});
}

void Overlay::setTool(const QString &tool) {
    if (!InkModel::tools.contains(tool) || tool == m_tool)
        return;
    m_tool = tool;
    emit toolChanged();
}

void Overlay::setColor(const QString &color) {
    const QColor parsed(color);
    if (!parsed.isValid() || parsed.name() == m_color)
        return;
    m_color = parsed.name();
    emit colorChanged();
}

void Overlay::setSizeIndex(int index) {
    index = std::clamp(index, 0, int(sizePresets.size()) - 1);
    if (index == m_sizeIndex)
        return;
    m_sizeIndex = index;
    emit sizeChanged();
}

QVariantList Overlay::sizes() const {
    QVariantList list;
    for (double size : sizePresets)
        list.append(size);
    return list;
}

void Overlay::cycleNib() {
    m_nibAngle = nibAngles.at((nibAngles.indexOf(m_nibAngle) + 1) % nibAngles.size());
    emit nibAngleChanged();
    emit notify(tr("Nib angle %1°").arg(m_nibAngle));
}

double Overlay::currentPressure() const {
    // A tablet sends its pressure in tablet events just before the mouse
    // events Qt makes from them; a plain mouse gets a steady medium line.
    if (m_tabletClock.isValid() && m_tabletClock.elapsed() < 80)
        return std::clamp(m_tabletPressure, 0.05, 1.0);
    return 0.75;
}

void Overlay::beginLocal(QPointF point, bool erase) {
    if (!m_drawing)
        return;
    if (!m_localId.isEmpty())
        endLocal();
    auto stroke = StrokePtr::create();
    stroke->id = QString("l%1").arg(++m_localCount);
    stroke->tool = erase || m_tabletEraser ? QString("eraser") : m_tool;
    stroke->color = QColor(m_color);
    stroke->size = stroke->tool == "eraser" ? 16 : sizePresets.at(m_sizeIndex);
    stroke->angle = m_nibAngle;
    stroke->pts.append({point.x(), point.y(), currentPressure()});
    if (m_model->begin(stroke))
        m_localId = stroke->id;
}

void Overlay::extendLocal(QPointF point) {
    if (!m_localId.isEmpty())
        m_model->extend(m_localId, {{point.x(), point.y(), currentPressure()}});
}

void Overlay::endLocal() {
    if (m_localId.isEmpty())
        return;
    m_model->end(m_localId, QDateTime::currentMSecsSinceEpoch());
    m_localId.clear();
}

bool Overlay::eventFilter(QObject *watched, QEvent *event) {
    switch (event->type()) {
    case QEvent::TabletPress:
    case QEvent::TabletMove:
    case QEvent::TabletRelease: {
        const auto *tablet = static_cast<QTabletEvent *>(event);
        m_tabletPressure = tablet->pressure();
        m_tabletEraser = tablet->pointerType() == QPointingDevice::PointerType::Eraser;
        m_tabletClock.restart();
        break;
    }
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}

QJsonObject Overlay::run(const QString &command, const QStringList &arguments) {
    const QString argument = arguments.value(0);
    if (command == "toggle")
        toggleDrawing();
    else if (command == "draw")
        setDrawing(true);
    else if (command == "done")
        setDrawing(false);
    else if (command == "show")
        setInkVisible(true);
    else if (command == "hide")
        setInkVisible(false);
    else if (command == "visible")
        setInkVisible(!m_visible);
    else if (command == "clear")
        clear();
    else if (command == "undo")
        undo();
    else if (command == "redo")
        redo();
    else if (command == "board") {
        if (argument.isEmpty())
            cycleBoard();
        else if (InkModel::boards.contains(argument == "off" ? QString() : argument))
            m_model->setBoard(argument == "off" ? QString() : argument);
        else
            return {{"ok", false}, {"error", tr("Boards are white, dark and off.")}};
    } else if (command == "tool") {
        if (!InkModel::tools.contains(argument))
            return {{"ok", false}, {"error", tr("Tools are %1.").arg(InkModel::tools.join(", "))}};
        setTool(argument);
    } else if (command == "color") {
        if (!QColor(argument).isValid())
            return {{"ok", false}, {"error", tr("Colors are written like #ff3b30.")}};
        setColor(argument);
    } else if (command == "quit") {
        emit quitRequested();
    } else if (command != "status" && command != "pad" && command != "open") {
        return {{"ok", false}, {"error", tr("Unknown command: %1").arg(command)}};
    }

    return {{"ok", true},
            {"drawing", m_drawing},
            {"visible", m_visible},
            {"tool", m_tool},
            {"color", m_color},
            {"board", m_model->board().isEmpty() ? QString("off") : m_model->board()},
            {"strokes", int(m_model->strokes().size())},
            {"output", m_output},
            {"pad", QJsonObject{{"urls", QJsonArray::fromStringList(padUrls())},
                                {"clients", padClients()},
                                {"error", m_padError}}}};
}
