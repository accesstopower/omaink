QT += core gui network qml quick quickcontrols2 svg testlib
CONFIG += c++17 testcase
TARGET = omaink_tests
TEMPLATE = app

INCLUDEPATH += ../src
LIBS += -lLayerShellQtInterface

HEADERS += ../src/ink.h ../src/inkrenderer.h ../src/padserver.h ../src/theme.h ../src/translator.h ../src/websocket.h
SOURCES += omaink_tests.cpp ../src/ink.cpp ../src/inkrenderer.cpp ../src/padserver.cpp \
           ../src/theme.cpp ../src/translator.cpp ../src/websocket.cpp
RESOURCES += ../src/resources.qrc
