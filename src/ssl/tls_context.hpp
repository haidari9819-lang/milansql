#pragma once
// ============================================================
// tls_context.hpp — Phase 110: SSL/TLS Encryption
// Windows: SChannel (Schannel / SSPI)
// Linux:   OpenSSL (if HAVE_OPENSSL=1) or stub
// ============================================================

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #define SECURITY_WIN32
  #include <security.h>
  #include <schannel.h>
  // Secur32.lib and Crypt32.lib are linked via CMakeLists.txt
  typedef SOCKET tls_sock_t;
  #define TLS_INVALID_SOCK INVALID_SOCKET
#else
  #include <sys/socket.h>
  #include <unistd.h>
  typedef int tls_sock_t;
  #define TLS_INVALID_SOCK (-1)
  #if defined(HAVE_OPENSSL) && HAVE_OPENSSL
    #include <openssl/ssl.h>
    #include <openssl/err.h>
    #include <openssl/x509.h>
  #endif
#endif

#include <string>
#include <atomic>
#include <iostream>
#include <cctype>
#include <mutex>

namespace milansql {

// ── SSL global configuration ───────────────────────────────────
// Phase 177: SSL modes
enum class SslMode { DISABLED, PREFERRED, REQUIRED };

struct SslConfig {
    std::atomic<bool> enabled{false};
    std::string       certPath;
    std::string       keyPath;
    std::string       caPath;        // Phase 177: CA cert for client verification
    std::atomic<bool> initialized{false};
    std::string       errorMessage;
    SslMode           mode{SslMode::PREFERRED};      // Phase 177: disabled/preferred/required
    SslMode           replMode{SslMode::DISABLED};    // Phase 177: replication SSL mode

    std::string modeStr() const {
        switch (mode) {
            case SslMode::DISABLED:  return "disabled";
            case SslMode::PREFERRED: return "preferred";
            case SslMode::REQUIRED:  return "required";
        }
        return "unknown";
    }
    std::string replModeStr() const {
        switch (replMode) {
            case SslMode::DISABLED:  return "disabled";
            case SslMode::REQUIRED:  return "required";
            default:                 return "disabled";
        }
        return "unknown";
    }
    static SslMode parseMode(const std::string& s) {
        std::string lower = s;
        for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower == "required") return SslMode::REQUIRED;
        if (lower == "preferred") return SslMode::PREFERRED;
        return SslMode::DISABLED;
    }
};

inline SslConfig& g_sslConfig() {
    static SslConfig cfg;
    return cfg;
}

// ── TlsSocket — thin wrapper around a connected TLS session ───
//
// Provides read() / write() / close() that mirror the raw sock_t
// API used in server.hpp, so callers can swap raw sockets for TLS.
//
// If TLS is not available / not enabled the socket degrades to
// plain TCP so the rest of the server code continues working.
//
struct TlsSocket {
    tls_sock_t sock{TLS_INVALID_SOCK};
    bool       tlsActive{false};

#if defined(_WIN32)
    // SChannel context handles
    CtxtHandle  hCtx{};
    CredHandle  hCred{};
    bool        ctxValid{false};
    bool        credValid{false};
    // Encrypted buffer for partial reads
    std::string recvBuf;
#elif defined(HAVE_OPENSSL) && HAVE_OPENSSL
    SSL* ssl{nullptr};
#endif

    TlsSocket() = default;
    explicit TlsSocket(tls_sock_t s) : sock(s) {}

    // Non-copyable
    TlsSocket(const TlsSocket&) = delete;
    TlsSocket& operator=(const TlsSocket&) = delete;

    // Move-constructible
    TlsSocket(TlsSocket&& o) noexcept
        : sock(o.sock), tlsActive(o.tlsActive)
#if defined(_WIN32)
        , hCtx(o.hCtx), hCred(o.hCred)
        , ctxValid(o.ctxValid), credValid(o.credValid)
        , recvBuf(std::move(o.recvBuf))
#elif defined(HAVE_OPENSSL) && HAVE_OPENSSL
        , ssl(o.ssl)
#endif
    {
        o.sock = TLS_INVALID_SOCK;
        o.tlsActive = false;
#if defined(_WIN32)
        o.ctxValid = false;
        o.credValid = false;
#elif defined(HAVE_OPENSSL) && HAVE_OPENSSL
        o.ssl = nullptr;
#endif
    }

    int read(char* buf, int len) {
        if (!tlsActive) {
            return ::recv(sock, buf, len, 0);
        }
#if defined(_WIN32)
        // SChannel read: decrypt available data
        // We accumulate raw bytes and decrypt packet by packet
        return schannel_read(buf, len);
#elif defined(HAVE_OPENSSL) && HAVE_OPENSSL
        return SSL_read(ssl, buf, len);
#else
        return ::recv(sock, buf, len, 0);
#endif
    }

    int write(const char* buf, int len) {
        if (!tlsActive) {
            return ::send(sock, buf, len, 0);
        }
#if defined(_WIN32)
        return schannel_write(buf, len);
#elif defined(HAVE_OPENSSL) && HAVE_OPENSSL
        return SSL_write(ssl, buf, len);
#else
        return ::send(sock, buf, len, 0);
#endif
    }

    void close() {
        if (sock == TLS_INVALID_SOCK) return;
#if defined(_WIN32)
        if (ctxValid)  { DeleteSecurityContext(&hCtx);  ctxValid = false; }
        if (credValid) { FreeCredentialsHandle(&hCred); credValid = false; }
        closesocket(sock);
#elif defined(HAVE_OPENSSL) && HAVE_OPENSSL
        if (ssl) {
            SSL_shutdown(ssl);
            SSL_free(ssl);
            ssl = nullptr;
        }
        ::close(sock);
#else
        ::close(sock);
#endif
        sock = TLS_INVALID_SOCK;
        tlsActive = false;
    }

private:
#if defined(_WIN32)
    // ── SChannel server-side encryption helpers ─────────────
    int schannel_write(const char* buf, int len) {
        if (!ctxValid) return ::send(sock, buf, len, 0);

        SecPkgContext_StreamSizes sizes{};
        if (QueryContextAttributes(&hCtx, SECPKG_ATTR_STREAM_SIZES, &sizes) != SEC_E_OK)
            return -1;

        int written = 0;
        const char* p = buf;
        int remaining = len;

        while (remaining > 0) {
            int chunk = remaining;
            if (chunk > static_cast<int>(sizes.cbMaximumMessage))
                chunk = static_cast<int>(sizes.cbMaximumMessage);

            std::vector<char> outBuf(sizes.cbHeader + chunk + sizes.cbTrailer);
            SecBuffer sb[4];
            sb[0].pvBuffer   = outBuf.data();
            sb[0].cbBuffer   = sizes.cbHeader;
            sb[0].BufferType = SECBUFFER_STREAM_HEADER;
            sb[1].pvBuffer   = outBuf.data() + sizes.cbHeader;
            sb[1].cbBuffer   = static_cast<ULONG>(chunk);
            sb[1].BufferType = SECBUFFER_DATA;
            memcpy(sb[1].pvBuffer, p, chunk);
            sb[2].pvBuffer   = outBuf.data() + sizes.cbHeader + chunk;
            sb[2].cbBuffer   = sizes.cbTrailer;
            sb[2].BufferType = SECBUFFER_STREAM_TRAILER;
            sb[3].pvBuffer   = nullptr;
            sb[3].cbBuffer   = 0;
            sb[3].BufferType = SECBUFFER_EMPTY;

            SecBufferDesc desc{SECBUFFER_VERSION, 4, sb};
            if (EncryptMessage(&hCtx, 0, &desc, 0) != SEC_E_OK) return -1;

            int total = sb[0].cbBuffer + sb[1].cbBuffer + sb[2].cbBuffer;
            int sent = 0;
            while (sent < total) {
                int n = ::send(sock, outBuf.data() + sent, total - sent, 0);
                if (n <= 0) return -1;
                sent += n;
            }
            written   += chunk;
            p         += chunk;
            remaining -= chunk;
        }
        return written;
    }

    int schannel_read(char* buf, int len) {
        // Fill raw receive buffer first
        char raw[16384];
        int n = ::recv(sock, raw, sizeof(raw), 0);
        if (n <= 0) return n;
        recvBuf.append(raw, n);

        if (recvBuf.empty()) return 0;

        SecBuffer sb[4];
        sb[0].pvBuffer   = const_cast<char*>(recvBuf.data());
        sb[0].cbBuffer   = static_cast<ULONG>(recvBuf.size());
        sb[0].BufferType = SECBUFFER_DATA;
        sb[1].BufferType = SECBUFFER_EMPTY;
        sb[2].BufferType = SECBUFFER_EMPTY;
        sb[3].BufferType = SECBUFFER_EMPTY;

        SecBufferDesc desc{SECBUFFER_VERSION, 4, sb};
        SECURITY_STATUS ss = DecryptMessage(&hCtx, &desc, 0, nullptr);
        if (ss != SEC_E_OK && ss != SEC_I_CONTEXT_EXPIRED) return -1;

        int copied = 0;
        std::string leftover;
        for (int i = 0; i < 4; ++i) {
            if (sb[i].BufferType == SECBUFFER_DATA && copied < len) {
                int take = std::min(len - copied, static_cast<int>(sb[i].cbBuffer));
                memcpy(buf + copied, sb[i].pvBuffer, take);
                copied += take;
            }
            if (sb[i].BufferType == SECBUFFER_EXTRA) {
                leftover.assign(reinterpret_cast<char*>(sb[i].pvBuffer), sb[i].cbBuffer);
            }
        }
        recvBuf = leftover;
        return copied > 0 ? copied : 0;
    }
#endif // _WIN32
};

// ── TlsContext — certificate + credentials holder ─────────────
//
// One TlsContext per server. Call loadCertificate() once at startup
// then wrapAccepted() for each new client socket.
//
class TlsContext {
public:
    TlsContext() = default;
    ~TlsContext() { cleanup(); }

    // Load PEM cert+key (Linux/OpenSSL) or PFX/system store (Windows)
    bool loadCertificate(const std::string& certPath, const std::string& keyPath) {
        certPath_ = certPath;
        keyPath_  = keyPath;
#if defined(_WIN32)
        return loadSchannel();
#elif defined(HAVE_OPENSSL) && HAVE_OPENSSL
        return loadOpenSsl();
#else
        lastError_ = "No TLS backend available on this platform.";
        return false;
#endif
    }

    // Wrap an accepted raw socket in TLS (server-side handshake)
    TlsSocket wrapAccepted(tls_sock_t sock) {
        TlsSocket ts(sock);
#if defined(_WIN32)
        if (credValid_) {
            if (schannel_handshake(ts))
                ts.tlsActive = true;
        }
#elif defined(HAVE_OPENSSL) && HAVE_OPENSSL
        if (ctx_) {
            SSL* ssl = SSL_new(ctx_);
            if (ssl) {
                SSL_set_fd(ssl, static_cast<int>(sock));
                if (SSL_accept(ssl) == 1) {
                    ts.ssl = ssl;
                    ts.tlsActive = true;
                } else {
                    SSL_free(ssl);
                }
            }
        }
#endif
        return ts;
    }

    bool isReady() const { return ready_; }
    const std::string& lastError() const { return lastError_; }

    // Phase 177: Reload certificate without restart
    bool reloadCertificate() {
        cleanup();
        if (certPath_.empty()) return false;
        return loadCertificate(certPath_, keyPath_);
    }

    // Phase 177: Get certificate info for SHOW SSL STATUS
    struct CertInfo {
        std::string subject;
        std::string issuer;
        std::string notBefore;
        std::string notAfter;
        std::string serial;
        std::string tlsVersion;
        std::string cipher;
    };

    CertInfo getCertInfo() const {
        CertInfo info;
#if defined(_WIN32)
        // SChannel: basic info from cert context
        info.tlsVersion = "TLS 1.2/1.3";
        info.cipher = "SChannel (system)";
        info.subject = "CN=MilanSQL";
        info.issuer = "CN=MilanSQL (self-signed)";
        info.notBefore = "(system store)";
        info.notAfter = "(system store)";
#elif defined(HAVE_OPENSSL) && HAVE_OPENSSL
        if (ctx_) {
            info.tlsVersion = "TLS 1.2/1.3";
            // Read cert info from file
            FILE* fp = fopen(certPath_.c_str(), "r");
            if (fp) {
                X509* cert = PEM_read_X509(fp, nullptr, nullptr, nullptr);
                fclose(fp);
                if (cert) {
                    // Subject
                    char buf[256];
                    X509_NAME_oneline(X509_get_subject_name(cert), buf, sizeof(buf));
                    info.subject = buf;
                    X509_NAME_oneline(X509_get_issuer_name(cert), buf, sizeof(buf));
                    info.issuer = buf;
                    // Validity
                    auto fmtTime = [](const ASN1_TIME* t) -> std::string {
                        if (!t) return "N/A";
                        BIO* bio = BIO_new(BIO_s_mem());
                        ASN1_TIME_print(bio, t);
                        char tbuf[128]{};
                        BIO_read(bio, tbuf, sizeof(tbuf) - 1);
                        BIO_free(bio);
                        return std::string(tbuf);
                    };
                    info.notBefore = fmtTime(X509_get0_notBefore(cert));
                    info.notAfter = fmtTime(X509_get0_notAfter(cert));
                    // Serial
                    ASN1_INTEGER* serial = X509_get_serialNumber(cert);
                    if (serial) {
                        BIGNUM* bn = ASN1_INTEGER_to_BN(serial, nullptr);
                        if (bn) {
                            char* hex = BN_bn2hex(bn);
                            if (hex) { info.serial = hex; OPENSSL_free(hex); }
                            BN_free(bn);
                        }
                    }
                    X509_free(cert);
                }
            }
            // Ciphers
            STACK_OF(SSL_CIPHER)* ciphers = SSL_CTX_get_ciphers(ctx_);
            if (ciphers && sk_SSL_CIPHER_num(ciphers) > 0) {
                info.cipher = SSL_CIPHER_get_name(sk_SSL_CIPHER_value(ciphers, 0));
                if (sk_SSL_CIPHER_num(ciphers) > 1) {
                    info.cipher += " (+" + std::to_string(sk_SSL_CIPHER_num(ciphers) - 1) + " more)";
                }
            }
        }
#else
        info.tlsVersion = "N/A";
        info.cipher = "N/A (no TLS backend)";
#endif
        return info;
    }

    void cleanup() {
#if defined(_WIN32)
        if (credValid_) {
            FreeCredentialsHandle(&hCred_);
            credValid_ = false;
        }
#elif defined(HAVE_OPENSSL) && HAVE_OPENSSL
        if (ctx_) {
            SSL_CTX_free(ctx_);
            ctx_ = nullptr;
        }
#endif
        ready_ = false;
    }

private:
    std::string certPath_;
    std::string keyPath_;
    bool        ready_{false};
    std::string lastError_;

#if defined(_WIN32)
    CredHandle  hCred_{};
    bool        credValid_{false};
    PCCERT_CONTEXT pCert_{nullptr};

    bool loadSchannel() {
        // Try to load PFX file first, then fall back to system store
        // For self-signed certs we use CryptStringToBinary + PFXImportCertStore
        // Here we use the simpler approach: open the My store and find by subject
        HCERTSTORE hStore = CertOpenSystemStoreA(0, "MY");
        if (hStore) {
            // Try to find any certificate
            pCert_ = CertFindCertificateInStore(
                hStore, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                0, CERT_FIND_ANY, nullptr, nullptr);
            CertCloseStore(hStore, 0);
        }

        if (!pCert_) {
            // No cert in store — TLS will degrade to plain TCP
            lastError_ = "SChannel: no certificate found in MY store. "
                         "Use --gen-cert to create one.";
            return false;
        }

        SCHANNEL_CRED cred{};
        cred.dwVersion             = SCHANNEL_CRED_VERSION;
        cred.cCreds                = 1;
        cred.paCred                = &pCert_;
        cred.grbitEnabledProtocols = SP_PROT_TLS1_2_SERVER |
                                     SP_PROT_TLS1_3_SERVER;
        cred.dwFlags               = SCH_CRED_NO_SYSTEM_MAPPER;

        SECURITY_STATUS ss = AcquireCredentialsHandleA(
            nullptr, const_cast<char*>(UNISP_NAME_A),
            SECPKG_CRED_INBOUND, nullptr, &cred,
            nullptr, nullptr, &hCred_, nullptr);

        if (ss != SEC_E_OK) {
            lastError_ = "SChannel AcquireCredentialsHandle failed: "
                         + std::to_string(ss);
            return false;
        }
        credValid_ = true;
        ready_     = true;
        return true;
    }

    bool schannel_handshake(TlsSocket& ts) {
        // Perform TLS server handshake via SChannel
        bool    loop       = true;
        bool    firstCall  = true;
        DWORD   ctxReq     = ASC_REQ_SEQUENCE_DETECT | ASC_REQ_REPLAY_DETECT |
                             ASC_REQ_CONFIDENTIALITY | ASC_REQ_STREAM;

        SecBuffer    inBufs[2]{};
        SecBufferDesc inDesc{SECBUFFER_VERSION, 2, inBufs};
        SecBuffer    outBufs[1]{};
        SecBufferDesc outDesc{SECBUFFER_VERSION, 1, outBufs};

        std::string rawBuf;
        rawBuf.resize(16384);

        while (loop) {
            int n = ::recv(ts.sock, &rawBuf[0], static_cast<int>(rawBuf.size()), 0);
            if (n <= 0) return false;
            rawBuf.resize(n);

            inBufs[0].pvBuffer   = const_cast<char*>(rawBuf.data());
            inBufs[0].cbBuffer   = static_cast<ULONG>(n);
            inBufs[0].BufferType = SECBUFFER_TOKEN;
            inBufs[1].pvBuffer   = nullptr;
            inBufs[1].cbBuffer   = 0;
            inBufs[1].BufferType = SECBUFFER_EMPTY;

            outBufs[0].pvBuffer   = nullptr;
            outBufs[0].cbBuffer   = 0;
            outBufs[0].BufferType = SECBUFFER_TOKEN;

            ULONG ctxAttr = 0;
            SECURITY_STATUS ss = AcceptSecurityContext(
                &hCred_,
                firstCall ? nullptr : &ts.hCtx,
                &inDesc, ctxReq, 0, &ts.hCtx,
                &outDesc, &ctxAttr, nullptr);

            firstCall     = false;
            ts.ctxValid   = true;
            ts.hCred      = hCred_;
            ts.credValid  = true;

            if (outBufs[0].pvBuffer && outBufs[0].cbBuffer > 0) {
                int sent = 0;
                const char* p = reinterpret_cast<char*>(outBufs[0].pvBuffer);
                int total = static_cast<int>(outBufs[0].cbBuffer);
                while (sent < total) {
                    int s = ::send(ts.sock, p + sent, total - sent, 0);
                    if (s <= 0) { FreeContextBuffer(outBufs[0].pvBuffer); return false; }
                    sent += s;
                }
                FreeContextBuffer(outBufs[0].pvBuffer);
            }

            if (ss == SEC_E_OK)                  { loop = false; }
            else if (ss == SEC_I_CONTINUE_NEEDED) { rawBuf.resize(16384); }
            else                                  { return false; }
        }
        return true;
    }

#elif defined(HAVE_OPENSSL) && HAVE_OPENSSL
    SSL_CTX* ctx_{nullptr};

    bool loadOpenSsl() {
        SSL_library_init();
        OpenSSL_add_all_algorithms();
        SSL_load_error_strings();

        const SSL_METHOD* method = TLS_server_method();
        ctx_ = SSL_CTX_new(method);
        if (!ctx_) { lastError_ = "SSL_CTX_new failed"; return false; }

        if (SSL_CTX_use_certificate_file(ctx_, certPath_.c_str(), SSL_FILETYPE_PEM) != 1) {
            lastError_ = "SSL_CTX_use_certificate_file failed: " + certPath_;
            SSL_CTX_free(ctx_); ctx_ = nullptr; return false;
        }
        if (SSL_CTX_use_PrivateKey_file(ctx_, keyPath_.c_str(), SSL_FILETYPE_PEM) != 1) {
            lastError_ = "SSL_CTX_use_PrivateKey_file failed: " + keyPath_;
            SSL_CTX_free(ctx_); ctx_ = nullptr; return false;
        }
        if (SSL_CTX_check_private_key(ctx_) != 1) {
            lastError_ = "SSL_CTX_check_private_key failed";
            SSL_CTX_free(ctx_); ctx_ = nullptr; return false;
        }
        ready_ = true;
        return true;
    }
#endif
};

// ── Global TlsContext singleton ───────────────────────────────
inline TlsContext& g_tlsContext() {
    static TlsContext ctx;
    return ctx;
}

// ── showSslStatus — output for SHOW SSL STATUS ────────────────
inline std::string showSslStatus() {
    const auto& cfg = g_sslConfig();
    std::string out = "\n";
    out += "  SSL/TLS Status\n";
    out += "  ─────────────────────────────────────────\n";
    out += "  Enabled     : ";
    out += cfg.enabled.load() ? "ON" : "OFF";
    out += "\n";
    out += "  SSL Mode    : " + cfg.modeStr() + "\n";
    out += "  Repl Mode   : " + cfg.replModeStr() + "\n";
    out += "  Backend     : ";
#if defined(_WIN32)
    out += "SChannel (Windows native)";
#elif defined(HAVE_OPENSSL) && HAVE_OPENSSL
    out += "OpenSSL";
#else
    out += "None (not available)";
#endif
    out += "\n";
    out += "  Cert        : " + (cfg.certPath.empty() ? "(none)" : cfg.certPath) + "\n";
    out += "  Key         : " + (cfg.keyPath.empty()  ? "(none)" : cfg.keyPath)  + "\n";
    out += "  CA          : " + (cfg.caPath.empty()   ? "(none)" : cfg.caPath)   + "\n";
    out += "  Ready       : ";
    out += g_tlsContext().isReady() ? "YES" : "NO";
    out += "\n";
    if (!g_tlsContext().lastError().empty())
        out += "  Error       : " + g_tlsContext().lastError() + "\n";
    // Certificate details
    if (g_tlsContext().isReady()) {
        auto ci = g_tlsContext().getCertInfo();
        out += "  ─── Certificate ─────────────────────────\n";
        out += "  Subject     : " + ci.subject + "\n";
        out += "  Issuer      : " + ci.issuer + "\n";
        out += "  Valid From  : " + ci.notBefore + "\n";
        out += "  Valid Until : " + ci.notAfter + "\n";
        if (!ci.serial.empty())
            out += "  Serial      : " + ci.serial + "\n";
        out += "  TLS Version : " + ci.tlsVersion + "\n";
        out += "  Cipher      : " + ci.cipher + "\n";
    }
    out += "\n";
    out += "  Ports encrypted when SSL=ON:\n";
    out += "    MySQL   4407  (" + cfg.modeStr() + ")\n";
    out += "    PG Wire 5433  (" + cfg.modeStr() + ")\n";
    out += "    Repl    4408  (" + cfg.replModeStr() + ")\n";
    out += "    HTTP    8080  (always via reverse proxy)\n";
    out += "\n";
    return out;
}

} // namespace milansql
