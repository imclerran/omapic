cmake_minimum_required(VERSION 3.16)
project(omapic VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Quick QuickControls2 Sql Gui)

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
        qml/DetailPanel.qml
        qml/SlideshowView.qml
        qml/LibraryManager.qml
        qml/TagManager.qml
    SOURCES
        src/photo.h
        src/database.h src/database.cpp
        src/photomodel.h src/photomodel.cpp
        src/photofiltermodel.h src/photofiltermodel.cpp
        src/tagmodel.h src/tagmodel.cpp
        src/library.h src/library.cpp
        src/libraryworker.h src/libraryworker.cpp
        src/thumbnailprovider.h src/thumbnailprovider.cpp
        src/omarchytheme.h src/omarchytheme.cpp
)

target_include_directories(omapic PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(omapic PRIVATE Qt6::Quick Qt6::QuickControls2 Qt6::Sql Qt6::Gui)
