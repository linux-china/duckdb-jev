# This file is included by DuckDB's build system. It specifies which extension to load

# Extension from this repo
duckdb_extension_load(jev
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
)

# jev's macros extract fields with json_extract_string, so the json extension has
# to be available. It is autoloadable in a released DuckDB; building it in keeps
# the test binary self-contained.
duckdb_extension_load(json)
