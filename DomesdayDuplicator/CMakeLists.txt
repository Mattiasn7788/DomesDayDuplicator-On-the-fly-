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

if(APPLE)
    set_target_properties(DomesdayDuplicator PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/MacOSXBundleInfo.plist.in"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.domesdayduplicator.app"
        MACOSX_BUNDLE_BUNDLE_NAME "DomesdayDuplicator"
    )
endif()

target_compile_definitions(DomesdayDuplicator PRIVATE
    QT_DEPRECATED_WARNINGS
)

# Maximise optimisation for Release builds (GCC/MinGW and Clang/AppleClang).
# This is especially important on macOS where the data-conversion loop runs at 80 MB/s;
# without -O3 the ProcessingThread can't keep pace with the USB transfer rate.
target_compile_options(DomesdayDuplicator PRIVATE
    $<$<AND:$<CONFIG:Release>,$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>>:-O3 -funroll-loops>
)

# Suppress deprecation warnings from the third-party qcustomplot.cpp on all platforms
set_source_files_properties(qcustomplot.cpp PROPERTIES
    COMPILE_OPTIONS "-Wno-deprecated-declarations"
)

target_link_libraries(DomesdayDuplicator PRIVATE
    Qt::Core
    Qt::Gui
    Qt::Widgets
    Qt::SerialPort
    ${LibUSB_LIBRARIES}
    PkgConfig::SWRESAMPLE
    PkgConfig::AVUTIL
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
