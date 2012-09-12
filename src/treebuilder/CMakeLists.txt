cmake_minimum_required( VERSION 2.8 )
project( libhubbub-treebuilder )

set(
	TREEBUILDER_SRCS
	after_after_body.c
	after_after_frameset.c
	after_body.c
	after_frameset.c
	after_head.c
	before_head.c
	before_html.c
	generic_rcdata.c
	in_body.c
	in_caption.c
	in_cell.c
	in_column_group.c
	in_foreign_content.c
	in_frameset.c
	in_head.c
	in_head_noscript.c
	initial.c
	in_row.c
	in_select.c
	in_select_in_table.c
	in_table_body.c
	in_table.c
	treebuilder.c
)
include_directories( 
    ${CMAKE_CURRENT_SOURCE_DIR}/..
	${CMAKE_CURRENT_SOURCE_DIR}/../../include
)
add_library(
    hubbub_treebuilder
	${TREEBUILDER_SRCS}
)
