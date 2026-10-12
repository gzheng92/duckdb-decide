# CURDIR preserves spaces in checkout names, unlike make's word-based dir helper.
PROJ_DIR := $(CURDIR)/
EXT_NAME = decide
EXT_CONFIG = $(PROJ_DIR)extension_config.cmake

# Standard targets consumed by the DuckDB community distribution workflow.
include extension-ci-tools/makefiles/duckdb_extension.Makefile
