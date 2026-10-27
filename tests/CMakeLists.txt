# Unit tests for omapic's logic layer (Qt Test + CTest).
#
# The core library (omapic-core, from the top-level CMakeLists) holds the
# GUI-free units: text folding, the database and the OQL engine. The remaining
# backend sources the app compiles via qt_add_qml_module are gathered here into
# a second static library so the tests can link them directly, without the QML
# engine or any GUI. Run them with `ctest` from the build directory.

find_package(Qt6 6.5 REQUIRED COMPONENTS Test Core Sql Qml Gui)
find_package(exiv2 REQUIRED) # the sidecar reader parses XMP via exiv2

# The pure-logic sources under test. Kept in sync by hand with CMakeLists.txt's
# qt_add_qml_module SOURCES list; only the GUI/QML-free units belong here.
add_library(omapic_logic STATIC
    ${CMAKE_SOURCE_DIR}/src/textmatch.h
    ${CMAKE_SOURCE_DIR}/src/mediascan.cpp
    ${CMAKE_SOURCE_DIR}/src/sidecar.cpp
    ${CMAKE_SOURCE_DIR}/src/photometadata.cpp
    ${CMAKE_SOURCE_DIR}/src/libraryworker.cpp
    ${CMAKE_SOURCE_DIR}/src/photomodel.cpp
    ${CMAKE_SOURCE_DIR}/src/photofiltermodel.cpp
    ${CMAKE_SOURCE_DIR}/src/triagesession.cpp
)
set_target_properties(omapic_logic PROPERTIES AUTOMOC ON)
target_include_directories(omapic_logic PUBLIC ${CMAKE_SOURCE_DIR}/src)
# Qt6::Qml is only needed because the model headers carry QML_ELEMENT macros;
# the tests never spin up a QML engine.
target_link_libraries(omapic_logic PUBLIC omapic-core Qt6::Core Qt6::Sql Qt6::Qml Qt6::Gui Exiv2::exiv2lib)

# Each test is a standalone QTEST_GUILESS_MAIN executable registered with CTest.
function(omapic_add_test name)
    qt_add_executable(${name} ${name}.cpp)
    set_target_properties(${name} PROPERTIES AUTOMOC ON)
    target_link_libraries(${name} PRIVATE omapic_logic Qt6::Test)
    add_test(NAME ${name} COMMAND ${name})
    # Harmless for the guiless tests; keeps things headless if a future test
    # pulls in a GUI class on a machine with no display.
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()

omapic_add_test(tst_textfold)
omapic_add_test(tst_oql)
omapic_add_test(tst_mediascan)
omapic_add_test(tst_database)
omapic_add_test(tst_photofiltermodel)
omapic_add_test(tst_schema)
omapic_add_test(tst_sidecar)
omapic_add_test(tst_import)
omapic_add_test(tst_triagesession)
