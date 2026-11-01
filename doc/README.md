# Documentation

The documentation has two parts, and is made into one site:

* the **guide**: the `.rst` files in here, written by hand. [`programmers-guide.rst`](programmers-guide.rst)
  is the place to start, and the others are about how it all works. Its code is in
  [`snippets.cpp`](snippets.cpp), which is compiled with every build.
* the **reference**: [`reference.rst`](reference.rst) says what goes where, and the content comes
  from the comments in the public headers in [`include/c_ares_async`](../include/c_ares_async).

## Making it

```sh
cmake --preset dev
cmake --build build --target docs
```

The pages end up in `build/doc/html`, with `index.html` to start from. It takes
[Doxygen](https://www.doxygen.nl), [Sphinx](https://www.sphinx-doc.org),
[Breathe](https://breathe.readthedocs.io) and the
[Read the Docs theme](https://sphinx-rtd-theme.readthedocs.io), which the devcontainer has. On
Debian that is:

```sh
apt-get install doxygen python3-sphinx python3-breathe python3-sphinx-rtd-theme
```

The target is not part of a regular build, and not there at all where CMake finds no `doxygen` or
no `sphinx-build`. It can be made from a build on either runtime and gives the same result: the
comments on the types of the two runtimes are in `asio/runtime.hpp`.

## Writing it

Warnings are errors in both tools, so a public symbol without a comment, a parameter without a
`@param` or a reference that goes nowhere fails the build, and with it a pull request.

* A new public symbol goes into [`reference.rst`](reference.rst), as nothing puts it there by itself.
* Code in the guide comes from `snippets.cpp`, between a `// [name]` and a `// [name-end]` comment,
  and is shown with `literalinclude`. Only pseudo code is written in place.
* In a page, names link to the reference with `` :cpp:func:`resolver::resolve` `` and the like, and
  files of the repository with `` :src:`src/resolver.cpp` ``.

[`Doxyfile.in`](Doxyfile.in) configures Doxygen and [`conf.py`](conf.py) configures Sphinx.
