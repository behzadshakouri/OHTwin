QT += core gui widgets charts network
CONFIG += c++17

TARGET   = DrywellDTViewer
TEMPLATE = app

SOURCES += \
    main.cpp \
    MainWindow.cpp \
    CsvLoader.cpp

HEADERS += \
    MainWindow.h \
    CsvLoader.h

# Qt 6 wasm: QApplication + widgets are fully supported.
# Build with:
#   /path/to/qt-wasm/bin/qmake DrywellDTViewer.pro
#   make
# Output: DrywellDTViewer.html + .js + .wasm (serve via nginx alongside selected_output.csv)
