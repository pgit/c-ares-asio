// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

#include "test_certificate.hpp"

#include <openssl/bio.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <memory>
#include <stdexcept>

namespace
{

void check(bool ok, const char* what)
{
   if (!ok)
      throw std::runtime_error(std::string(what) + " failed");
}

void add_extension(X509* certificate, int nid, const std::string& value)
{
   X509V3_CTX context;
   X509V3_set_ctx_nodb(&context);
   X509V3_set_ctx(&context, certificate, certificate, nullptr, nullptr, 0);

   X509_EXTENSION* extension = X509V3_EXT_nconf_nid(nullptr, &context, nid, value.c_str());
   check(extension != nullptr, "X509V3_EXT_nconf_nid");
   check(X509_add_ext(certificate, extension, -1) == 1, "X509_add_ext");
   X509_EXTENSION_free(extension);
}

template <typename Write>
std::string to_pem(Write&& write)
{
   BIO* bio = BIO_new(BIO_s_mem());
   check(bio != nullptr, "BIO_new");
   check(write(bio) == 1, "PEM_write_bio");

   const char* data = nullptr;
   const auto length = BIO_get_mem_data(bio, &data);
   std::string pem(data, static_cast<std::size_t>(length));
   BIO_free(bio);
   return pem;
}

} // namespace

test_certificate make_test_certificate(std::string_view host)
{
   //
   // The key: ECDSA on P-256, which every TLS library offers and accepts without being asked.
   //
   EVP_PKEY* generated = nullptr;
   {
      EVP_PKEY_CTX* context = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
      check(context != nullptr, "EVP_PKEY_CTX_new_id");
      check(EVP_PKEY_keygen_init(context) == 1, "EVP_PKEY_keygen_init");
      check(EVP_PKEY_CTX_set_ec_paramgen_curve_nid(context, NID_X9_62_prime256v1) == 1,
            "EVP_PKEY_CTX_set_ec_paramgen_curve_nid");
      check(EVP_PKEY_keygen(context, &generated) == 1, "EVP_PKEY_keygen");
      EVP_PKEY_CTX_free(context);
   }
   const std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(generated, &EVP_PKEY_free);

   const std::unique_ptr<X509, decltype(&X509_free)> certificate(X509_new(), &X509_free);
   check(certificate != nullptr, "X509_new");

   check(X509_set_version(certificate.get(), 2) == 1, "X509_set_version"); // which is v3
   check(ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1) == 1, "ASN1_INTEGER_set");
   check(X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -60 * 60) != nullptr,
         "X509_gmtime_adj");
   check(X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 24 * 60 * 60) != nullptr,
         "X509_gmtime_adj");
   check(X509_set_pubkey(certificate.get(), key.get()) == 1, "X509_set_pubkey");

   const std::string name(host);
   X509_NAME* subject = X509_get_subject_name(certificate.get());
   check(X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
                                    reinterpret_cast<const unsigned char*>(name.c_str()), -1, -1,
                                    0) == 1,
         "X509_NAME_add_entry_by_txt");
   check(X509_set_issuer_name(certificate.get(), subject) == 1, "X509_set_issuer_name");

   //
   // The name a client checks is the one in here, the common name is for people to read. And the
   // certificate has to be allowed to sign certificates to pass as the root of its own chain.
   //
   add_extension(certificate.get(), NID_subject_alt_name, "DNS:" + name);
   add_extension(certificate.get(), NID_basic_constraints, "critical,CA:TRUE");

   check(X509_sign(certificate.get(), key.get(), EVP_sha256()) > 0, "X509_sign");

   return {
      .certificate_pem =
         to_pem([&](BIO* bio) { return PEM_write_bio_X509(bio, certificate.get()); }),
      .key_pem = to_pem([&](BIO* bio) {
         return PEM_write_bio_PrivateKey(bio, key.get(), nullptr, nullptr, 0, nullptr, nullptr);
      }),
   };
}
