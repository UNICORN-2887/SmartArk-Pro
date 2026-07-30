# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "1.8.2")

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

# Force response files on Windows to avoid "command line too long" errors
if(WIN32)
    set(CMAKE_NINJA_FORCE_RESPONSE_FILE ON CACHE BOOL "Force Ninja response files")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(xiaozhi)

