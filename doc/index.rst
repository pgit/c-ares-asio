c-ares-async
============

`c-ares <https://c-ares.org/>`__ name resolution as asynchronous operations, on `Boost.Asio
<https://www.boost.org/doc/libs/release/doc/html/boost_asio.html>`__ or on capy/corosio: no thread
of its own, any number of lookups in flight at once, addresses as well as HTTPS records, and over
UDP, TCP or DNS over TLS.

:doc:`programmers-guide` is the place to start. It goes through what a program does with the
library, in the order it does it, and links to :doc:`reference` for everything it mentions.

.. toctree::
   :maxdepth: 2
   :caption: Guide

   programmers-guide

.. toctree::
   :maxdepth: 1
   :caption: How it works

   design
   corosio
   https-records
   dns-over-tls

.. toctree::
   :maxdepth: 2
   :caption: Reference

   reference
