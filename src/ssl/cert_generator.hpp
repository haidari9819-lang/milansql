#pragma once
// ============================================================
// cert_generator.hpp — Phase 110: Self-Signed Certificate
//
// Windows: CertCreateSelfSignCertificate + PFX export
// Linux:   openssl CLI subprocess
// ============================================================

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <wincrypt.h>
  #include <vector>
  // Crypt32.lib is linked via CMakeLists.txt
#endif

#include <string>
#include <iostream>
#include <fstream>
#include <cstdlib>

namespace milansql {

class CertGenerator {
public:
    // Generate a self-signed certificate and private key.
    // certPath: output PEM cert (Linux) or informational path (Windows)
    // keyPath:  output PEM key (Linux) or ignored (Windows uses MY store)
    // Returns: status message
    static std::string generateSelfSigned(const std::string& certPath,
                                          const std::string& keyPath) {
#if defined(_WIN32)
        (void)keyPath; // Windows: key stored in system store
        return generateWindows(certPath);
#else
        return generateLinux(certPath, keyPath);
#endif
    }

private:
#if defined(_WIN32)
    static std::string generateWindows(const std::string& /*certPath*/) {
        // Create self-signed certificate in the Windows MY certificate store
        // Subject: CN=MilanSQL
        std::string subjectStr = "CN=MilanSQL";

        // Encode subject name
        DWORD cbEncoded = 0;
        if (!CertStrToNameA(X509_ASN_ENCODING,
                            subjectStr.c_str(),
                            CERT_OID_NAME_STR,
                            nullptr, nullptr, &cbEncoded, nullptr)) {
            return "  ERROR: CertStrToNameA (size) failed: " +
                   std::to_string(GetLastError()) + "\n";
        }

        std::vector<BYTE> encoded(cbEncoded);
        if (!CertStrToNameA(X509_ASN_ENCODING,
                            subjectStr.c_str(),
                            CERT_OID_NAME_STR,
                            nullptr, encoded.data(), &cbEncoded, nullptr)) {
            return "  ERROR: CertStrToNameA (encode) failed: " +
                   std::to_string(GetLastError()) + "\n";
        }

        CERT_NAME_BLOB nameBlob{cbEncoded, encoded.data()};

        // Key provider info
        CRYPT_KEY_PROV_INFO kpi{};
        kpi.pwszContainerName = const_cast<LPWSTR>(L"MilanSQLKey");
        kpi.pwszProvName      = nullptr;
        kpi.dwProvType        = PROV_RSA_FULL;
        kpi.dwFlags           = 0;
        kpi.cProvParam        = 0;
        kpi.rgProvParam       = nullptr;
        kpi.dwKeySpec         = AT_KEYEXCHANGE;

        // Algorithm
        CRYPT_ALGORITHM_IDENTIFIER sigAlg{};
        sigAlg.pszObjId = const_cast<LPSTR>(szOID_RSA_SHA256RSA);

        // Validity: 2 years
        SYSTEMTIME stStart{}, stEnd{};
        GetSystemTime(&stStart);
        stEnd = stStart;
        stEnd.wYear = static_cast<WORD>(stEnd.wYear + 2);

        PCCERT_CONTEXT pCert = CertCreateSelfSignCertificate(
            0, &nameBlob, 0, &kpi, &sigAlg, &stStart, &stEnd, nullptr);

        if (!pCert) {
            return "  ERROR: CertCreateSelfSignCertificate failed: " +
                   std::to_string(GetLastError()) + "\n";
        }

        // Add to MY store
        HCERTSTORE hStore = CertOpenSystemStoreA(0, "MY");
        if (!hStore) {
            CertFreeCertificateContext(pCert);
            return "  ERROR: CertOpenSystemStore(MY) failed.\n";
        }

        BOOL added = CertAddCertificateContextToStore(
            hStore, pCert, CERT_STORE_ADD_REPLACE_EXISTING, nullptr);
        CertCloseStore(hStore, 0);
        CertFreeCertificateContext(pCert);

        if (!added) {
            return "  ERROR: CertAddCertificateContextToStore failed: " +
                   std::to_string(GetLastError()) + "\n";
        }

        return "  Self-signed certificate created and added to Windows MY store.\n"
               "  Subject: CN=MilanSQL\n"
               "  Validity: 2 years\n"
               "  Use 'milansql.exe --ssl --server' to enable TLS.\n\n";
    }

#else
    static std::string generateLinux(const std::string& certPath,
                                     const std::string& keyPath) {
        // Validate paths to prevent command injection
        auto isSafePath = [](const std::string& p) {
            for (char c : p) {
                if (!std::isalnum(c) && c != '/' && c != '.' && c != '-' && c != '_')
                    return false;
            }
            return !p.empty() && p.find("..") == std::string::npos;
        };
        if (!isSafePath(certPath) || !isSafePath(keyPath)) {
            return "  ERROR: Invalid characters in cert/key path.\n\n";
        }
        // Use openssl CLI to generate a self-signed cert
        std::string cmd =
            "openssl req -x509 -newkey rsa:2048 -keyout \"" + keyPath + "\""
            " -out \"" + certPath + "\""
            " -days 730 -nodes"
            " -subj '/CN=MilanSQL/O=MilanSQL/C=DE'"
            " 2>&1";

        int ret = std::system(cmd.c_str());
        if (ret != 0) {
            return "  ERROR: openssl command failed (exit code " +
                   std::to_string(ret) + ").\n"
                   "  Make sure openssl is installed and in PATH.\n\n";
        }

        return "  Self-signed certificate generated:\n"
               "    Cert: " + certPath + "\n"
               "    Key:  " + keyPath  + "\n"
               "  Validity: 730 days (2 years)\n"
               "  Use 'milansql --ssl --ssl-cert " + certPath +
               " --ssl-key " + keyPath + " --server' to enable TLS.\n\n";
    }
#endif
};

} // namespace milansql
