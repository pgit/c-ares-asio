API reference
=============

Made from the comments in the public headers. :doc:`programmers-guide` is the place to start, and
says how these go together.

Everything is in the namespace ``c_ares_async``, and everything comes with one include:

.. code-block:: cpp

   #include <c_ares_async.hpp>

Resolver
--------

.. code-block:: cpp

   #include <c_ares_async/resolver.hpp>

.. doxygenclass:: c_ares_async::resolver
   :members:

HTTPS records
-------------

.. doxygenstruct:: c_ares_async::https_record
   :members:

DNS over TLS
------------

.. doxygenstruct:: c_ares_async::tls_options
   :members:

Logging
-------

.. doxygenenum:: c_ares_async::log_level

.. doxygentypedef:: c_ares_async::log_function

Errors
------

.. code-block:: cpp

   #include <c_ares_async/error.hpp>

.. doxygenfunction:: c_ares_async::ares_category

.. doxygenfunction:: c_ares_async::make_ares_error

The runtime
-----------

.. code-block:: cpp

   #include <c_ares_async/runtime.hpp>

What the interface is written in, and what each name is on Boost.Asio. On corosio the same names
are something else, which :ref:`the table in the corosio page <corosio-runtimes>` has.

.. doxygentypedef:: c_ares_async::Task

.. doxygentypedef:: c_ares_async::Executor

.. doxygentypedef:: c_ares_async::IpAddress

.. doxygentypedef:: c_ares_async::Endpoint

.. doxygentypedef:: c_ares_async::error_code

.. doxygentypedef:: c_ares_async::error_category

.. doxygentypedef:: c_ares_async::system_error

.. doxygenvariable:: c_ares_async::errors::canceled

.. doxygenfunction:: c_ares_async::make_system_error
