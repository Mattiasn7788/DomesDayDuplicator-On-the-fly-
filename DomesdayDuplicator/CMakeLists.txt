set(DomesdayDuplicator_SOURCES
    aboutdialog.cpp aboutdialog.ui
    advancednamingdialog.cpp advancednamingdialog.ui
    amplitudemeasurement.cpp
    AudioResampler.cpp
    automaticcapturedialog.cpp automaticcapturedialog.ui
    configuration.cpp
    configurationdialog.cpp configurationdialog.ui
    main.cpp
    mainwindow.cpp mainwindow.ui
    playercommunication.cpp
    playercontrol.cpp
    playerremotedialog.cpp playerremotedialog.ui
    qcustomplot.cpp
    QtLogger.cpp
    UsbDeviceBase.cpp
    UsbDeviceLibUsb.cpp
)

if(WIN32)
    list(APPEND DomesdayDuplicator_SOURCES UsbDeviceWinUsb.cpp)
endif()

qt_add_resources(DomesdayDuplicator_SOURCES resources.qrc)

add_executable(DomesdayDuplicator WIN32 MACOSX_BUNDLE
    ${DomesdayDuplicator_SOURCES})

target_compile_definitions(DomesdayDuplicator PRIVATE
    QT_DEPRECATED_WARNINGS
)

# Maximise optimisation for Release builds with MinGW/GCC to close the
# performance gap vs MSVC-compiled binaries.
target_compile_options(DomesdayDuplicator PRIVATE
    $<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:GNU>>:-O3 -funroll-loops>
)

target_link_libraries(DomesdayDuplicator PRIVATE
    Qt::Core
    Qt::Gui
    Qt::Widgets
    Qt::SerialPort
    ${LibUSB_LIBRARIES}
    ${SWRESAMPLE_LIBRARIES}
    ${AVUTIL_LIBRARIES}
)

target_include_directories(DomesdayDuplicator PRIVATE
    ${LibUSB_INCLUDE_DIRS}
    ${SWRESAMPLE_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    .
)

if(WIN32)
    target_compile_definitions(DomesdayDuplicator PRIVATE
        NOMINMAX
        QUSB_LIBRARY
    )

    target_link_libraries(DomesdayDuplicator PRIVATE
        advapi32
        setupapi
        winusb
    )
endif()

install(TARGETS DomesdayDuplicator
        BUNDLE DESTINATION .
        RUNTIME DESTINATION bin
)
