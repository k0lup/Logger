QT += core

TEMPLATE = lib
CONFIG += shared c++17

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

SDK_ROOT = $$(JOBQT_SDK)

isEmpty(SDK_ROOT) {
    error("JOBQT_SDK environment variable is not set")
}

LOGGER_INSTALL_ROOT = $$SDK_ROOT/Logger/$$VERSION

target.path = $$LOGGER_INSTALL_ROOT/lib

headers.files = \
    $$PWD/include/Logger/logger.h \
    $$PWD/include/Logger/logger_global.h \
    $$PWD/include/Logger/version.h

headers.path = $$LOGGER_INSTALL_ROOT/include/Logger

INSTALLS += target headers
