QT += core gui network qml quick quickcontrols2 svg

CONFIG += c++17 release
TARGET = omaink
TEMPLATE = app

# The overlay sits on the compositor's overlay layer through layer-shell-qt.
LIBS += -lLayerShellQtInterface

HEADERS += \
    src/ink.h \
    src/inkcanvas.h \
    src/inkrenderer.h \
    src/overlay.h \
    src/padserver.h \
    src/theme.h \
    src/translator.h \
    src/websocket.h

SOURCES += \
    src/main.cpp \
    src/ink.cpp \
    src/inkcanvas.cpp \
    src/inkrenderer.cpp \
    src/overlay.cpp \
    src/padserver.cpp \
    src/theme.cpp \
    src/translator.cpp \
    src/websocket.cpp

RESOURCES += src/resources.qrc
