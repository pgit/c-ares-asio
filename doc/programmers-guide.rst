.. cpp:namespace:: c_ares_async

Programmer's guide
==================

This goes through what a program does with the library, from its first lookup to shutting down.
The code is taken from :src:`doc/snippets.cpp`, which is compiled with every build. What is about
how it is all done rather than how it is used has a page of its own, starting with
:doc:`design`.

.. contents:: On this page
   :local:
   :depth: 2

Prerequisites
-------------

A C++20 compiler, c-ares 1.34 or later and Boost 1.88 or later. A build on Corosio needs no
Boost, as Corosio and its dependency Capy are fetched along with the library. The README has
:src:`the full list <README.md>`. Whichever way the library gets into the build, the target has the
same name:

.. code-block:: cmake

   find_package(c_ares_async 0.1 REQUIRED)

   target_link_libraries(app PRIVATE c_ares_async::c_ares_async)

and there is one header to include, which has everything:

.. code-block:: cpp

   #include <c_ares_async.hpp>

A build tree is on one runtime or the other, which is decided when it is configured:
``-DC_ARES_ASYNC_API=ASIO``, the default, or ``COROSIO``. The difference to a program is that
Corosio has no completion tokens, and that the types in the interface are those of the runtime.
They have the same names on both, such as :cpp:type:`Task` and :cpp:type:`Endpoint`, and
:ref:`the table on the Corosio page <corosio-runtimes>` says what each name stands for.

A first lookup
--------------

The whole of a program on Asio, which looks up the addresses of a name and then its HTTPS
record:

.. literalinclude:: ../example/asio/minimal.cpp
   :language: cpp
   :start-at: #include <c_ares_async.hpp>

A :cpp:class:`resolver` is made on an executor, and from then on everything it does happens there.
There is no thread behind it, so nothing happens unless the context runs.

The same program on Corosio is :src:`example/corosio/minimal.cpp`.

Two spellings of an operation
-----------------------------

The coroutine spelling
^^^^^^^^^^^^^^^^^^^^^^

:cpp:func:`resolver::resolve` and :cpp:func:`resolver::resolve_https` are coroutines that yield the
error and the result as a tuple. Both runtimes have them, so code written like this builds on
either:

.. literalinclude:: snippets.cpp
   :language: cpp
   :start-after: // [coroutine]
   :end-before: // [coroutine-end]

Completion tokens
^^^^^^^^^^^^^^^^^

On Asio there are also :cpp:func:`resolver::async_resolve` and
:cpp:func:`resolver::async_resolve_https`, the same lookups as asynchronous operations that take any
completion token. With ``use_awaitable``, what goes wrong is thrown:

.. literalinclude:: snippets.cpp
   :language: cpp
   :start-after: // [token-exception]
   :end-before: // [token-exception-end]

With ``as_tuple`` it is what the coroutine spelling does, which is how to tell one failure from
another without catching anything:

.. literalinclude:: snippets.cpp
   :language: cpp
   :start-after: // [token-tuple]
   :end-before: // [token-tuple-end]

And it can be a plain callback, which is never called from inside the function that it is given to:

.. literalinclude:: snippets.cpp
   :language: cpp
   :start-after: // [token-callback]
   :end-before: // [token-callback-end]

Handling errors
---------------

A lookup fails with a c-ares status code in :cpp:func:`ares_category`, which
:cpp:func:`make_ares_error` makes something to compare against, or with
:cpp:var:`errors::canceled`:

.. literalinclude:: snippets.cpp
   :language: cpp
   :start-after: // [errors]
   :end-before: // [errors-end]

The ones to expect:

.. list-table::
   :header-rows: 1

   * - Error
     - What it means
   * - ``ARES_ENOTFOUND``
     - There is no such name.
   * - ``ARES_ENODATA``
     - The name is there, a record of the type asked for is not. For an HTTPS record that is the
       common case rather than a problem.
   * - ``ARES_ETIMEOUT``
     - The servers do not answer.
   * - ``ARES_ECONNREFUSED``
     - The servers cannot be reached. With DNS over TLS this is also what a certificate that was
       turned down looks like, see :ref:`logging`.
   * - :cpp:var:`errors::canceled`
     - The lookup was cancelled, or its resolver went away.

The only thing that throws is the constructor of the :cpp:class:`resolver`, where c-ares or the TLS
library cannot be set up.

Addresses
---------

:cpp:func:`resolver::resolve` looks up the addresses of both families, the way
``ares_getaddrinfo()`` does for a stream socket. ``/etc/hosts`` and the search domains are taken
into account and the endpoints come sorted, the one to try first in front. An address in place of
the name is taken as it is, without a query.

The second argument is the port that every endpoint gets, by its number or by the name it has in
``/etc/services``. It does not take part in the question, so a service that is not in there is
no error: it comes to port 0, as an empty one does.

HTTPS records
-------------

An :cpp:struct:`https_record` tells a client which protocols an endpoint speaks before it connects:

.. literalinclude:: snippets.cpp
   :language: cpp
   :start-after: // [https-records]
   :end-before: // [https-records-end]

A record with a :cpp:member:`priority <https_record::priority>` of 0 is an alias, where
:cpp:member:`target <https_record::target>` names the host to go on with. Any other describes a
service, with :cpp:member:`params <https_record::params>` saying how to reach it, one string per
parameter the way a zone file has it, like ``alpn=h3,h2``. :doc:`https-records` has more on what
comes back.

Addresses and records at once
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

These are two separate questions, and c-ares has no trouble with several of them in flight, so the
record costs no second round trip:

.. literalinclude:: snippets.cpp
   :language: cpp
   :start-after: // [both-at-once]
   :end-before: // [both-at-once-end]

Which servers to ask
--------------------

A resolver starts out with the servers of the system, from ``/etc/resolv.conf``.
:cpp:func:`resolver::set_servers` replaces them:

.. literalinclude:: snippets.cpp
   :language: cpp
   :start-after: // [set-servers]
   :end-before: // [set-servers-end]

They have to be addresses. A server that only has a name needs that name looked up first, by another
resolver.

DNS over TLS
------------

:cpp:struct:`tls_options` is given to the constructor, as it is for all the queries of a resolver or
for none of them:

.. literalinclude:: snippets.cpp
   :language: cpp
   :start-after: // [dns-over-tls]
   :end-before: // [dns-over-tls-end]

.. warning::

   Without a :cpp:member:`hostname <tls_options::hostname>` the certificate chain is verified, but
   not whose certificate it is. Setting :cpp:member:`verify <tls_options::verify>` to false takes
   any certificate at all, which is for testing.

How the transport works, and why one name is better off as one server, is in :doc:`dns-over-tls`.

.. _logging:

Logging
-------

The library writes nowhere by itself. What it has to say goes to the function given to
:cpp:func:`resolver::set_logger`, with a :cpp:enum:`log_level` that says how much it matters:

.. literalinclude:: snippets.cpp
   :language: cpp
   :start-after: // [logging]
   :end-before: // [logging-end]

This is worth having for DNS over TLS in particular. All c-ares gets to see of a handshake that
failed is a connection that went away, so the lookup says no more than ``ARES_ECONNREFUSED``, and
the reason is in the message and nowhere else.

The function is called on a thread that runs the executor, from inside the operations of the
resolver. It should come back quickly, and must not destroy the resolver.

Cancellation
------------

.. warning::

   **Cancelling one lookup cancels all of them.** c-ares has no handle for a single request, so what
   a cancellation can do is ``ares_cancel()``, for the whole resolver. Every pending operation then
   completes with :cpp:var:`errors::canceled`. Use c-ares 1.34.7 or later where lookups get
   cancelled, as earlier ones can run into `GHSA-6wfj-rwm7-3542
   <https://github.com/c-ares/c-ares/security/advisories/GHSA-6wfj-rwm7-3542>`__ here. CMake warns
   about it.

On Asio
^^^^^^^

Any type of cancellation will do. ``cancel_after()`` gives a lookup a deadline, and it takes a
timer here:

.. literalinclude:: snippets.cpp
   :language: cpp
   :start-after: // [cancel-after]
   :end-before: // [cancel-after-end]

A ``cancellation_signal`` does it when told:

.. literalinclude:: snippets.cpp
   :language: cpp
   :start-after: // [cancel-signal]
   :end-before: // [cancel-signal-end]

On Corosio
^^^^^^^^^^

What cancels a lookup is the ``std::stop_token`` of the coroutine that awaits it:

.. literalinclude:: snippets.cpp
   :language: cpp
   :start-after: // [stop-token]
   :end-before: // [stop-token-end]

``corosio::timeout()`` gives it a deadline. The lookup is cancelled just the same, but what it
reports in that place is its own code, not :cpp:var:`errors::canceled`:

.. literalinclude:: snippets.cpp
   :language: cpp
   :start-after: // [timeout]
   :end-before: // [timeout-end]

Completion and lifetime
-----------------------

**A lookup is not complete until the context ran its handler.** c-ares calls back from inside its
own functions, in some cases before the initiating function has returned, so the completion is
never called from there but handed to its executor.

That includes destroying a resolver with lookups pending. They complete with
:cpp:var:`errors::canceled`, after the destructor has returned, so the context has to run once more
for them to. On Corosio, a context destroyed instead unwinds them without a result; a lookup that a
coroutine awaits on a resolver in its own frame is out of its reach, see :ref:`corosio-constraints`.

A resolver stays where it was made, and can be neither copied nor moved. :doc:`design` says what
happens to what is in flight, and why.

Threads
-------

A resolver is for one thread at a time. Where the context runs on several, make its executor a
strand and call it from inside that strand:

.. literalinclude:: snippets.cpp
   :language: cpp
   :start-after: // [strand]
   :end-before: // [strand-end]

This is tested on Asio. On Corosio a resolver on a ``capy::strand`` should stay on it, but
nothing here runs it that way yet.

Pseudo code for looking up many names
-------------------------------------

One resolver is enough for any number of names, as c-ares keeps the queries apart. What is left to
the program is how many of them to have in flight, which comes to a number of workers that share
one list. They all run on one thread and only ever switch at a ``co_await``, so no lock is needed:

.. code-block:: text

   next = 0

   worker():
       while next < hosts.size():
           host = hosts[next++]
           ec, endpoints = co_await resolver.resolve(host, "443")
           report(host, ec, endpoints)

   co_await when_all(worker() times concurrency)

Keep in mind that cancelling any of these lookups cancels all of them, which is what a program that
stops on a signal wants. A worker should ask whether it was cancelled itself rather than go by the
error of the lookup, as an answer that was on its way when the signal came is a good one, and going
by that alone it would start on the next name. The real thing is ``resolveAll()`` in
:src:`example/resolve/main.cpp`, on both runtimes.

Pseudo code for driving c-ares from an event loop
-------------------------------------------------

There is nothing a program has to do for this, as it is what the library is for, but it is the
shortest way to say how it works. c-ares says which sockets it wants watched, the resolver waits for
them the way the runtime waits for anything, and c-ares is told when one of them is ready:

.. code-block:: text

   // c-ares calls this for every socket it opens, closes or changes its interest in
   on_socket_state(fd, readable, writable):
       if not readable and not writable:
           forget(fd)                       // about to be closed, and still valid for now
           return
       socket = sockets[fd]                 // the first time, starts watching fd
       socket.want_read = readable
       socket.want_write = writable
       arm(socket)

   arm(socket):
       if socket.want_read and not socket.waiting_read:
           wait until readable, then on_socket_event(socket, READ)
       if socket.want_write and not socket.waiting_write:
           wait until writable, then on_socket_event(socket, WRITE)

   on_socket_event(socket, event):
       if c-ares has lost interest in event since:
           arm(socket)
           return
       if handshake of DNS over TLS not finished:
           advance it, and arm(socket) for what it needs next
           return
       ares_process_fds(channel, { socket.fd, event })
       arm(socket)                          // the callback above only fires on changes
       update_timeout()

   update_timeout():
       if ares_timeout() says nothing is in flight:
           timer.cancel()
           return
       timer.arm(what it says, then:
           ares_process_fds(channel, no events)
           update_timeout())

   // c-ares calls this from inside ares_getaddrinfo() or ares_process_fds()
   on_lookup_done(status, result):
       post(executor, handler(make_ares_error(status), result))   // never from in here

The real thing is in :src:`src/resolver.cpp`, and :doc:`design` has the reasons for it.

Examples
--------

* :src:`example/asio/minimal.cpp` and :src:`example/corosio/minimal.cpp` are the shortest program
  that resolves a name, once per runtime.
* :src:`example/resolve <example/resolve/README.md>` is a command line with all of it: many names at
  once, HTTPS records, DNS over TLS, cancellation.
