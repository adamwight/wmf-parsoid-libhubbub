cmake_minimum_required( VERSION 2.8 )

project( libhubbub CXX )

include_directories( ${CMAKE_BINARY_DIR}/include )
add_subdirectory( src/treebuilder )
