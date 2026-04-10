QT -= gui
QT += core

CONFIG += console
CONFIG -= app_bundle

CONFIG += c++17

INCLUDEPATH += ../OpenHydroQual/aquifolium/include
INCLUDEPATH += ../OpenHydroQual/aquifolium/src
INCLUDEPATH += ../OpenHydroQual/aquifolium/include/GA
INCLUDEPATH += ../OpenHydroQual/aquifolium/include/MCMC
INCLUDEPATH += ../OpenHydroQual/jsoncpp/include/
INCLUDEPATH += ../OpenHydroQual/

macx: DEFINES += mac_version
linux: DEFINES += ubuntu_version
win32: DEFINES += windows_version

DEFINES += Terminal_version
DEFINES += Q_JSON_SUPPORT

TARGET = OpenHydroQual-Console
TEMPLATE = app
win32:QMAKE_CXXFLAGS += /MP

# Link against the OHQ shared library
LIBS += -L$$PWD/libs -lOHQLib
QMAKE_RPATHDIR += $$PWD/libs

linux {
    DEFINES += ARMA_USE_LAPACK ARMA_USE_BLAS GSL
    LIBS += -larmadillo -llapack -lblas -lgsl
}

macx {
    LIBS += /opt/homebrew/Cellar/armadillo/11.4.2/lib/libarmadillo.dylib
    INCLUDEPATH += $$PWD/../../../../opt/homebrew/Cellar/armadillo/11.4.2/include
    DEPENDPATH += $$PWD/../../../../opt/homebrew/Cellar/armadillo/11.4.2/include
}

CONFIG(debug, debug|release) {
    message(Building in debug mode)
    DEFINES += NO_OPENMP DEBUG
}

SOURCES += \
    DTConfig.cpp \
    DTRunner.cpp \
    main.cpp

HEADERS += \
    ../../XString.h \
    ../OpenHydroQual/aquifolium/include/Objective_Function.h \
    ../OpenHydroQual/aquifolium/include/Objective_Function_Set.h \
    ../OpenHydroQual/aquifolium/include/Precipitation.h \
    ../OpenHydroQual/aquifolium/include/RxnParameter.h \
    ../OpenHydroQual/aquifolium/include/constituent.h \
    ../OpenHydroQual/aquifolium/include/observation.h \
    ../OpenHydroQual/aquifolium/include/precalculatedfunction.h \
    ../OpenHydroQual/aquifolium/include/solutionlogger.h \
    ../OpenHydroQual/aquifolium/include/GA/GA.h \
    ../OpenHydroQual/aquifolium/include/MCMC/MCMC.h \
    ../OpenHydroQual/aquifolium/include/MCMC/MCMC.hpp \
    ../OpenHydroQual/aquifolium/include/Utilities.h \
    ../OpenHydroQual/aquifolium/include/restorepoint.h \
    ../OpenHydroQual/aquifolium/include/safevector.h \
    ../OpenHydroQual/aquifolium/include/safevector.hpp \
    ../OpenHydroQual/aquifolium/include/Block.h \
    ../OpenHydroQual/aquifolium/include/BTC.h \
    ../OpenHydroQual/aquifolium/include/BTCSet.h \
    ../OpenHydroQual/aquifolium/include/Expression.h \
    ../OpenHydroQual/aquifolium/include/Link.h \
    ../OpenHydroQual/aquifolium/include/Matrix.h \
    ../OpenHydroQual/aquifolium/include/Matrix_arma.h \
    ../OpenHydroQual/aquifolium/include/MetaModel.h \
    ../OpenHydroQual/aquifolium/include/NormalDist.h \
    ../OpenHydroQual/aquifolium/include/Object.h \
    ../OpenHydroQual/aquifolium/include/Quan.h \
    ../OpenHydroQual/aquifolium/include/QuanSet.h \
    ../OpenHydroQual/aquifolium/include/QuickSort.h \
    ../OpenHydroQual/aquifolium/include/System.h \
    ../OpenHydroQual/aquifolium/include/Vector.h \
    ../OpenHydroQual/aquifolium/include/Vector_arma.h \
    ../OpenHydroQual/jsoncpp/include/json/allocator.h \
    ../OpenHydroQual/jsoncpp/include/json/assertions.h \
    ../OpenHydroQual/jsoncpp/include/json/autolink.h \
    ../OpenHydroQual/jsoncpp/include/json/config.h \
    ../OpenHydroQual/jsoncpp/include/json/features.h \
    ../OpenHydroQual/jsoncpp/include/json/forwards.h \
    ../OpenHydroQual/jsoncpp/include/json/json.h \
    ../OpenHydroQual/jsoncpp/include/json/reader.h \
    ../OpenHydroQual/jsoncpp/include/json/value.h \
    ../OpenHydroQual/jsoncpp/include/json/version.h \
    ../OpenHydroQual/jsoncpp/include/json/writer.h \
    ../OpenHydroQual/jsoncpp/src/lib_json/json_tool.h \
    ../OpenHydroQual/jsoncpp/src/lib_json/version.h.in \
    ../OpenHydroQual/aquifolium/include/Parameter.h \
    ../OpenHydroQual/aquifolium/include/Parameter_Set.h \
    ../OpenHydroQual/aquifolium/include/Command.h \
    ../OpenHydroQual/aquifolium/include/Script.h \
    ../OpenHydroQual/aquifolium/include/GA/Binary.h \
    ../OpenHydroQual/aquifolium/include/GA/Distribution.h \
    ../OpenHydroQual/aquifolium/include/GA/DistributionNUnif.h \
    ../OpenHydroQual/aquifolium/include/GA/Individual.h \
    ../OpenHydroQual/aquifolium/include/GA/GA.hpp \
    ../OpenHydroQual/aquifolium/src/BTC.hpp \
    ../OpenHydroQual/aquifolium/src/BTCSet.hpp \
    ../OpenHydroQual/aquifolium/include/reaction.h \
    DTConfig.h \
    DTRunner.h

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target
