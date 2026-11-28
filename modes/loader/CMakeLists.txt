cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS ../../components)
# One version number for everything, from version.txt at the top of the project. It goes up on every build that ships.
file(STRINGS ${CMAKE_CURRENT_LIST_DIR}/../../version.txt PROJECT_VER LIMIT_COUNT 1)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(loader)
