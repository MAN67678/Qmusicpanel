cmake_minimum_required(VERSION 3.24)
project(QPanel CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(TAGLIB REQUIRED taglib)
pkg_check_modules(FFTW REQUIRED fftw3f)

add_executable(qpanel
    src/main.cpp
    src/vendor_impl.cpp
    src/term/Terminal.cpp
    src/audio/AudioEngine.cpp
    src/dsp/Spectrum.cpp
    src/media/Media.cpp
    src/media/ChafaRender.cpp
    src/fs/Library.cpp
    src/ui/Widgets.cpp
    src/app/App.cpp
    src/app/Config.cpp
)

target_include_directories(qpanel PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor
    ${TAGLIB_INCLUDE_DIRS}
    src
)

target_link_libraries(qpanel PRIVATE
    ${TAGLIB_LIBRARIES}
    ${FFTW_LIBRARIES}
    pthread
    dl
)

# ---- chafa art engine (optional) ------------------------------------------
# Headers are vendored in vendor/chafa; the runtime .so.0 ships with the distro
# package. Absent either one, the built-in half-block renderer is used.
pkg_check_modules(GLIB glib-2.0)
find_path(CHAFA_INCLUDE_DIR chafa.h
          PATHS ${CMAKE_CURRENT_SOURCE_DIR}/vendor/chafa NO_DEFAULT_PATH)
find_library(CHAFA_LIBRARY NAMES chafa libchafa.so.0)
if(CHAFA_INCLUDE_DIR AND CHAFA_LIBRARY AND GLIB_FOUND)
    message(STATUS "chafa art engine: ${CHAFA_LIBRARY}")
    target_compile_definitions(qpanel PRIVATE QP_HAVE_CHAFA=1)
    target_include_directories(qpanel PRIVATE ${CHAFA_INCLUDE_DIR} ${GLIB_INCLUDE_DIRS})
    target_link_libraries(qpanel PRIVATE ${CHAFA_LIBRARY})
else()
    message(STATUS "chafa not found — builtin half-block art only")
endif()

target_compile_options(qpanel PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
