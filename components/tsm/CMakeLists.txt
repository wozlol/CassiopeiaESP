idf_component_register(SRCS "src/vtparse.c" "src/color.c" "src/charsets.c" "src/termstate.c" INCLUDE_DIRS "include")
set_source_files_properties(src/vtparse.c src/termstate.c PROPERTIES COMPILE_OPTIONS "-O2")
