cmake_minimum_required(VERSION 3.20)
project(HEPShelf VERSION 0.9.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.4 REQUIRED COMPONENTS
    Widgets
    Network
    Sql
    Concurrent
    Pdf
    PdfWidgets
)

qt_standard_project_setup()

qt_add_executable(hepshelf
    src/main.cpp
    src/mainwindow.cpp
    src/mainwindow.h
    src/database.cpp
    src/database.h
    src/arxiv_client.cpp
    src/arxiv_client.h
    src/arxiv_discovery.cpp
    src/arxiv_discovery.h
    src/arxiv_feed.cpp
    src/arxiv_feed.h
    src/arxiv_watches.cpp
    src/arxiv_watches.h
    src/arxiv_watches_dialog.cpp
    src/arxiv_watches_dialog.h
    src/inspire_client.cpp
    src/inspire_client.h
    src/reference_dialog.cpp
    src/reference_dialog.h
    src/cited_by_dialog.cpp
    src/cited_by_dialog.h
    src/citation_graph_dialog.cpp
    src/citation_graph_dialog.h
    src/literature_trails_dialog.cpp
    src/literature_trails_dialog.h
    src/arxiv_id.cpp
    src/arxiv_id.h
    src/scanner.cpp
    src/scanner.h
)

target_link_libraries(hepshelf PRIVATE
    Qt6::Widgets
    Qt6::Network
    Qt6::Sql
    Qt6::Concurrent
    Qt6::Pdf
    Qt6::PdfWidgets
)

set_target_properties(hepshelf PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE TRUE
)

install(TARGETS hepshelf
    BUNDLE DESTINATION .
    RUNTIME DESTINATION bin
)

if(UNIX AND NOT APPLE)
    install(FILES packaging/hepshelf.desktop
        DESTINATION share/applications
    )
    install(FILES packaging/hepshelf.svg
        DESTINATION share/icons/hicolor/scalable/apps
    )

    set(CPACK_GENERATOR "DEB")
    set(CPACK_PACKAGE_NAME "hepshelf")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Local research paper library and PDF reader")
    set(CPACK_PACKAGE_VENDOR "HEPShelf contributors")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "HEPShelf contributors")
    set(CPACK_DEBIAN_PACKAGE_SECTION "science")
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libqt6sql6-sqlite")
    set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "poppler-utils")
    include(CPack)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(arxiv_watches_test
        tests/arxiv_watches_test.cpp
        src/arxiv_watches.cpp
        src/arxiv_feed.cpp
        src/arxiv_id.cpp
        src/inspire_client.cpp
        src/database.cpp
    )
    target_include_directories(arxiv_watches_test PRIVATE src)
    target_link_libraries(arxiv_watches_test PRIVATE Qt6::Widgets Qt6::Network Qt6::Sql)
    add_test(NAME arxiv_watches_test COMMAND arxiv_watches_test)
    set_tests_properties(arxiv_watches_test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
