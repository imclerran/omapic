cmake_minimum_required(VERSION 3.16)
project(omapic VERSION 0.5.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Quick QuickControls2 Sql Gui Multimedia Concurrent DBus)
find_package(exiv2 REQUIRED) # EXIF/metadata for the details pane

qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(omapic
    src/main.cpp
)

qt_add_qml_module(omapic
    URI Omapic
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/PhotoDelegate.qml
        qml/TagSidebar.qml
        qml/TagPicker.qml
        qml/DetailPanel.qml
        qml/WheelScroller.qml
        qml/TagAutoField.qml
        qml/TagInfoPopover.qml
        qml/SlideshowView.qml
        qml/PhotoViewer.qml
        qml/Viewer.qml
        qml/SettingsDialog.qml
        qml/TagManager.qml
        qml/TimelineView.qml
        qml/FolderView.qml
        qml/FolderMosaicCell.qml
        qml/AlbumView.qml
        qml/ClearButton.qml
        qml/TriageSidebar.qml
        qml/TriageView.qml
    SOURCES
        src/photo.h
        src/database.h src/database.cpp
        src/photomodel.h src/photomodel.cpp
        src/photofiltermodel.h src/photofiltermodel.cpp
        src/textfold.h src/textfold.cpp
        src/photometadata.h src/photometadata.cpp
        src/tagmodel.h src/tagmodel.cpp
        src/library.h src/library.cpp
        src/triagesession.h src/triagesession.cpp
        src/libraryworker.h src/libraryworker.cpp
        src/mediascan.h src/mediascan.cpp
        src/sidecar.h src/sidecar.cpp
        src/mediascanner.h src/mediascanner.cpp
        src/thumbnailprovider.h src/thumbnailprovider.cpp
        src/omarchytheme.h src/omarchytheme.cpp
)

target_include_directories(omapic PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(omapic PRIVATE Qt6::Quick Qt6::QuickControls2 Qt6::Sql Qt6::Gui Qt6::Multimedia Qt6::Concurrent Qt6::DBus Exiv2::exiv2lib)

# Unit tests for the logic layer. On by default; configure with
# -DOMAPIC_BUILD_TESTS=OFF to skip them (e.g. in the packaging build). Run with
# `ctest --test-dir build` (or `ctest` from the build directory) after building.
option(OMAPIC_BUILD_TESTS "Build the logic unit tests" ON)
if(OMAPIC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Installation. The QML module is compiled into the executable, so only the
# binary plus its desktop entry and icon need to be installed.
include(GNUInstallDirs)

install(TARGETS omapic
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(FILES assets/omapic.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)

install(FILES assets/omapic.svg
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
