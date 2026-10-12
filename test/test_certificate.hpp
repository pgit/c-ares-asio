//
// A certificate for the TLS side of the fake DNS server, made up on the spot: self-signed, and
// with that its own trust anchor wherever a test decides to trust it.
//
#pragma once

#include <string>
#include <string_view>

struct test_certificate
{
   std::string certificate_pem;
   std::string key_pem;
};

//
// A certificate for the DNS name 'host', valid from an hour ago until tomorrow.
//
test_certificate make_test_certificate(std::string_view host);
