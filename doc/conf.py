#
# Sphinx makes the HTML, of the guide as well as of the reference, whose content Breathe takes from
# the XML that Doxygen makes of the public headers. 'cmake --build build --target docs' runs both
# and says where the XML is and which version this is, see CMakeLists.txt in here.
#
project = "c-ares-async"
author = "Peter Eisenlohr"
copyright = "2026, Peter Eisenlohr"

extensions = ["breathe", "sphinx.ext.extlinks"]

# a file of the repository, which the pages link to rather than copy
extlinks = {"src": ("https://github.com/pgit/c-ares-async/blob/master/%s", "%s")}

breathe_default_project = "c_ares_async"
breathe_default_members = ()

primary_domain = "cpp"
highlight_language = "cpp"
cpp_index_common_prefix = ["c_ares_async::"]

html_theme = "sphinx_rtd_theme"
html_theme_options = {"navigation_depth": 3}

exclude_patterns = ["README.md"]

#
# A reference that goes nowhere is an error, but only for what is there to go to: the names of the
# standard library, Boost and c-ares are not documented here, nor is detail, and a namespace is
# only ever part of the name of what is in it.
#
nitpicky = True
nitpick_ignore_regex = [
    (r"cpp:.*", r"(std|boost)(::.*)?"),
    (r"cpp:.*", r"ares_.*"),
    (r"cpp:.*", r"detail(::.*)?"),
    (r"cpp:identifier", r"c_ares_async(::errors)?"),
]
