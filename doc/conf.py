# Copyright (c) 2026 Vilhelm Engström
#
# SPDX-License-Identifier: Apache-2.0

import inspect
import pathlib
import os
import sys

from typing import List

import sphinx
import sphinx.cmd.build

zrpc_base = pathlib.Path(os.environ["ZRPC_BASE"])
assert zrpc_base.is_dir()

deploy = os.environ.get("DEPLOY", 0)
version = os.environ.get("ZRPC_VERSION", "latest")

# -- Command line arguments --------------------------------------------------
parser = sphinx.cmd.build.get_parser()
args = parser.parse_args()
builddir = pathlib.Path(args.outputdir).resolve().parent.parent

# -- Project information -----------------------------------------------------
# https://www.sphinx-doc.org/en/master/usage/configuration.html#project-information

project = "zRPC"
copyright = "2026, Vilhelm Engström"
author = "Vilhelm Engström"

# -- General configuration ---------------------------------------------------
# https://www.sphinx-doc.org/en/master/usage/configuration.html#general-configuration

extensions = [
    "sphinxcontrib.tikz",
    "sphinx.ext.graphviz",
    "sphinx.ext.intersphinx",
]

templates_path = ["_templates"]
exclude_patterns: List[str] = []

# -- Intersphinx -------------------------------------------------------------

intersphinx_disabled_reftypes = ["*"]

zephyr_docs = f"https://docs.zephyrproject.org/latest"
intersphinx_mapping = {"zephyr": (zephyr_docs, None)}

# -- Tikz --------------------------------------------------------------------

tikz_proc_suite = "GhostScript"
tikz_transparent = False
tikz_resolution = 1024

# -- Options for HTML output -------------------------------------------------
# https://www.sphinx-doc.org/en/master/usage/configuration.html#options-for-html-output

form_reference = lambda page: f"/en/{version}/{page}" if deploy else f"/{page}"

html_theme = "sphinx_rtd_theme"
html_static_path = ["_static"]
html_theme_options = {"logo_only": False, "display_version": False}
html_context = {
    "reference_links": {
        "C API": form_reference("doxygen"),
        "Python API": form_reference("pdoc"),
    },
}
