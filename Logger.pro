QT += core

TEMPLATE = lib

CONFIG += shared
CONFIG += c++17

TARGET = Logger

VERSION = 1.0.0

DEFINES += \
    LOGGER_LIBRARY \
    LOGGER_VERSION=\\\"$$VERSION\\\"

INCLUDEPATH += \
    $$PWD/include \
    $$PWD/src

HEADERS += \
    include/Logger/logger.h \
    include/Logger/logger_global.h \
    include/Logger/version.h \
    src/logworker.h \
    src/crashhandler.h

SOURCES += \
    src/logger.cpp \
    src/logworker.cpp \
    src/crashhandler.cpp \
    src/version.cpp
