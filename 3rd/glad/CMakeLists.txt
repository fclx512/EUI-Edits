cmake_minimum_required(VERSION 3.5)
project(glad)

add_library(glad src/glad.c)
target_include_directories(glad PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/include>)

if(NOT WIN32)
  target_link_libraries(glad PUBLIC ${CMAKE_DL_LIBS})
endif()

# Generate position independent code
set_target_properties(glad PROPERTIES POSITION_INDEPENDENT_CODE ON)
