# @dosconio 20240405
QT += widgets # Turn to help, search "QApplication" and you can get this line

LIBS += -LE:/PROJ/SVGN/_bin -lw64d # use MinGW-64's UNISYM

INCLUDEPATH += E:/PROJ/SVGN/unisym/inc

DEFINES += _DEBUG _NO_uint

SOURCES += \
    WidgetMain.cpp \
    main.cpp

HEADERS += \
    WidgetMain.h

