// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// A DNS server on the loopback interface that answers from a table, for a resolver to be pointed
// at. It speaks UDP, TCP and, once it has a certificate, DNS over TLS -- all on one port, as
// c-ares has a single port for a server whichever way it asks.
//
// The messages are taken apart and put together by c-ares' own record functions, so the tests do
// not come with a second implementation of the wire format to keep right.
//
#pragma once

#include "test_certificate.hpp"

#include <boost/asio/any_io_executor.hpp>

#include <ares.h>

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class fake_dns_server
{
public:
   enum class transport
   {
      udp,
      tcp,
      tls
   };

   //
   // One question the server was asked.
   //
   struct query
   {
      std::string name;
      ares_dns_rec_type_t type;
      transport via;
   };

   //
   // One SvcParam of an HTTPS record, with its value in wire format.
   //
   struct svc_param
   {
      unsigned short key;
      std::vector<unsigned char> value;
   };

   explicit fake_dns_server(const boost::asio::any_io_executor& executor);
   ~fake_dns_server();

   fake_dns_server(const fake_dns_server&) = delete;
   fake_dns_server& operator=(const fake_dns_server&) = delete;

   //
   // Where the server listens, the way resolver::set_servers() takes it: "127.0.0.1:<port>".
   //
   std::string address() const;

   //
   // The zone. A name that has any record at all exists, which makes the difference between an
   // empty answer and NXDOMAIN for the record types it does not have.
   //
   void add_address(std::string_view name, std::string_view address); // A or AAAA
   void add_cname(std::string_view name, std::string_view target);
   void add_https(std::string_view name, unsigned short priority, std::string_view target,
                  std::vector<svc_param> params = {});

   //
   // Ways of not being helpful: swallowing every question, answering all of them with an error,
   // or claiming over UDP that the answer did not fit, which sends the resolver to TCP.
   //
   void set_silent(bool silent);
   void set_rcode(ares_dns_rcode_t rcode);
   void set_truncate_udp(bool truncate);

   //
   // Makes every connection from here on start with a TLS handshake, the server presenting
   // 'certificate'.
   //
   void use_tls(const test_certificate& certificate);

   //
   // What happened so far.
   //
   const std::vector<query>& queries() const;
   std::size_t count(std::string_view name, ares_dns_rec_type_t type) const;
   std::size_t count(transport via) const;
   std::size_t connections() const; // accepted, whatever became of them
   std::size_t handshakes() const; // TLS handshakes that went through

private:
   struct state;
   std::shared_ptr<state> m_state; // shared with the coroutines that do the serving
};
