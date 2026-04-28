QT -= gui
QT += core network

CONFIG += console
CONFIG -= app_bundle

CONFIG += c++14

# ============================================================
# Host-config (enable ONE only)
# Default here: PowerEdge
# ============================================================
#CONFIG  += Behzad
#DEFINES += Behzad

CONFIG  += PowerEdge
DEFINES += PowerEdge

#CONFIG  += Arash
#DEFINES += Arash

#CONFIG  += SligoCreek
#DEFINES += SligoCreek

#CONFIG  += Jason
#DEFINES += Jason

#CONFIG  += WSL
#DEFINES += WSL

# ============================================================
# Active DT model-config (enable ONE only)
# Options: DT_MODEL_VN, DT_MODEL_DRYWELL, DT_MODEL_HQ, DT_MODEL_R
# Default here: VN drywell
# ============================================================
CONFIG  += DT_MODEL_VN
DEFINES += DT_MODEL_VN

#CONFIG  += DT_MODEL_DRYWELL
#DEFINES += DT_MODEL_DRYWELL

#CONFIG  += DT_MODEL_HQ
#DEFINES += DT_MODEL_HQ

#CONFIG  += DT_MODEL_R
#DEFINES += DT_MODEL_R

# ============================================================
# Host build folders (keeps build artifacts out of the source tree)
# ============================================================
HOST_TAG = unknown
MODEL_TAG = unknown

contains(DEFINES, Jason)      { HOST_TAG = jason }
contains(DEFINES, PowerEdge)  { HOST_TAG = poweredge }
contains(DEFINES, Behzad)     { HOST_TAG = behzad }
contains(DEFINES, Arash)      { HOST_TAG = arash }
contains(DEFINES, SligoCreek) { HOST_TAG = sligocreek }
contains(DEFINES, WSL)        { HOST_TAG = wsl }

contains(DEFINES, DT_MODEL_VN)      { MODEL_TAG = vn }
contains(DEFINES, DT_MODEL_DRYWELL) { MODEL_TAG = drywell }
contains(DEFINES, DT_MODEL_HQ)      { MODEL_TAG = hq }
contains(DEFINES, DT_MODEL_R)       { MODEL_TAG = r }

BUILD_TAG = $$HOST_TAG-$$MODEL_TAG
BUILD_DIR = $$PWD/build-qmake-$$BUILD_TAG

!exists($$BUILD_DIR)     { system(mkdir -p $$BUILD_DIR) }
!exists($$BUILD_DIR/obj) { system(mkdir -p $$BUILD_DIR/obj) }
!exists($$BUILD_DIR/moc) { system(mkdir -p $$BUILD_DIR/moc) }
!exists($$BUILD_DIR/rcc) { system(mkdir -p $$BUILD_DIR/rcc) }
!exists($$BUILD_DIR/ui)  { system(mkdir -p $$BUILD_DIR/ui) }
!exists($$BUILD_DIR/bin) { system(mkdir -p $$BUILD_DIR/bin) }

OBJECTS_DIR = $$BUILD_DIR/obj
MOC_DIR     = $$BUILD_DIR/moc
RCC_DIR     = $$BUILD_DIR/rcc
UI_DIR      = $$BUILD_DIR/ui
DESTDIR     = $$BUILD_DIR/bin

message("HOST_TAG    = $$HOST_TAG")
message("MODEL_TAG   = $$MODEL_TAG")
message("BUILD_TAG   = $$BUILD_TAG")
message("BUILD_DIR   = $$BUILD_DIR")
message("DESTDIR     = $$DESTDIR")

# Set to 1 to compile OHQ from source, 0 to use the shared library
OHQ_FROM_SOURCE = 1

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

TARGET = DryWellDT_$$BUILD_TAG
TEMPLATE = app
win32:QMAKE_CXXFLAGS += /MP

# ── OHQ: source vs library ───────────────────────────────────────
equals(OHQ_FROM_SOURCE, 1) {
    message(Building with OHQ source files)
    SOURCES += \
        ../OpenHydroQual/aquifolium/src/Block.cpp \
        ../OpenHydroQual/aquifolium/src/Command.cpp \
        ../OpenHydroQual/aquifolium/src/Condition.cpp \
        ../OpenHydroQual/aquifolium/src/ErrorHandler.cpp \
        ../OpenHydroQual/aquifolium/src/Expression.cpp \
        ../OpenHydroQual/aquifolium/src/Link.cpp \
        ../OpenHydroQual/aquifolium/src/Matrix.cpp \
        ../OpenHydroQual/aquifolium/src/Matrix_arma.cpp \
        ../OpenHydroQual/aquifolium/src/MetaModel.cpp \
        ../OpenHydroQual/aquifolium/src/NormalDist.cpp \
        ../OpenHydroQual/aquifolium/src/Object.cpp \
        ../OpenHydroQual/aquifolium/src/Objective_Function.cpp \
        ../OpenHydroQual/aquifolium/src/Objective_Function_Set.cpp \
        ../OpenHydroQual/aquifolium/src/Parameter.cpp \
        ../OpenHydroQual/aquifolium/src/Parameter_Set.cpp \
        ../OpenHydroQual/aquifolium/src/Precipitation.cpp \
        ../OpenHydroQual/aquifolium/src/Quan.cpp \
        ../OpenHydroQual/aquifolium/src/QuanSet.cpp \
        ../OpenHydroQual/aquifolium/src/QuickSort.cpp \
        ../OpenHydroQual/aquifolium/src/Rule.cpp \
        ../OpenHydroQual/aquifolium/src/RxnParameter.cpp \
        ../OpenHydroQual/aquifolium/src/Script.cpp \
        ../OpenHydroQual/aquifolium/src/Source.cpp \
        ../OpenHydroQual/aquifolium/src/System.cpp \
        ../OpenHydroQual/aquifolium/src/Utilities.cpp \
        ../OpenHydroQual/aquifolium/src/Vector.cpp \
        ../OpenHydroQual/aquifolium/src/Vector_arma.cpp \
        ../OpenHydroQual/aquifolium/src/constituent.cpp \
        ../OpenHydroQual/aquifolium/src/observation.cpp \
        ../OpenHydroQual/aquifolium/src/precalculatedfunction.cpp \
        ../OpenHydroQual/aquifolium/src/reaction.cpp \
        ../OpenHydroQual/aquifolium/src/restorepoint.cpp \
        ../OpenHydroQual/aquifolium/src/solutionlogger.cpp \
        ../OpenHydroQual/aquifolium/src/GA/Binary.cpp \
        ../OpenHydroQual/aquifolium/src/GA/Individual.cpp \
        ../OpenHydroQual/aquifolium/src/GA/DistributionNUnif.cpp \
        ../OpenHydroQual/aquifolium/src/GA/Distribution.cpp \
        ../OpenHydroQual/jsoncpp/src/lib_json/json_reader.cpp \
        ../OpenHydroQual/jsoncpp/src/lib_json/json_value.cpp \
        ../OpenHydroQual/jsoncpp/src/lib_json/json_writer.cpp
} else {
    message(Building with OHQ shared library)
    CONFIG(debug, debug|release) {
        LIBS += -L$$PWD/libs/debug -lOHQLib
        QMAKE_RPATHDIR += $$PWD/libs/debug
    }
    CONFIG(release, debug|release) {
        LIBS += -L$$PWD/libs/release -lOHQLib
        QMAKE_RPATHDIR += $$PWD/libs/release
    }
}

# ── Platform libs ────────────────────────────────────────────────
linux {
    DEFINES += ARMA_USE_LAPACK ARMA_USE_BLAS GSL
    LIBS += -larmadillo -llapack -lblas -lgsl -lgomp
}

macx {
    LIBS += /opt/homebrew/Cellar/armadillo/11.4.2/lib/libarmadillo.dylib
    INCLUDEPATH += $$PWD/../../../../opt/homebrew/Cellar/armadillo/11.4.2/include
    DEPENDPATH += $$PWD/../../../../opt/homebrew/Cellar/armadillo/11.4.2/include
}

# ── Debug/Release ────────────────────────────────────────────────
CONFIG(debug, debug|release) {
    message(Building in debug mode)
    DEFINES += NO_OPENMP DEBUG
}

# ── Project sources ──────────────────────────────────────────────
SOURCES += \
    DTConfig.cpp \
    DTRunner.cpp \
    VizRenderer.cpp \
    main.cpp \
    noaaweatherfetcher.cpp

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
    DTRunner.h \
    RuntimeFiles.h \
    VizRenderer.h \
    noaaweatherfetcher.h

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target


DISTFILES += \
    config.json \
    viz_drywell.json

# Copy runtime config and model-specific visualization JSON next to the executable
# after every successful link. The runner reads config.json from DESTDIR and then
# uses config.json::viz_file to locate the selected visualization file.
QMAKE_POST_LINK += $$QMAKE_COPY $$shell_quote($$PWD/config.json)       $$shell_quote($$DESTDIR/config.json)       $$escape_expand(\n\t)
QMAKE_POST_LINK += $$QMAKE_COPY $$shell_quote($$PWD/viz_drywell.json) $$shell_quote($$DESTDIR/viz_drywell.json) $$escape_expand(\n\t)
