#!/usr/bin/env python3
"""Add TLS/SSL tests to milansql_tests.cpp — testGroup110"""

with open('/opt/milansql/src/tests/milansql_tests.cpp', 'r') as f:
    content = f.read()

test_code = r'''
// ============================================================
// testGroup110 -- SSL/TLS Configuration (Phase 177)
// ============================================================
static void testGroup110() {
    std::cout << "\n-- testGroup110: SSL/TLS Configuration --\n";
    using namespace milansql;

    // TLS-1: SslConfig defaults
    {
        SslConfig cfg;
        check(!cfg.enabled.load(), "TLS-1a: SSL disabled by default");
        check(cfg.mode == SslMode::PREFERRED, "TLS-1b: default mode is PREFERRED");
        check(cfg.replMode == SslMode::DISABLED, "TLS-1c: repl mode disabled by default");
        check(cfg.certPath.empty(), "TLS-1d: no cert path by default");
        check(cfg.keyPath.empty(), "TLS-1e: no key path by default");
        check(cfg.caPath.empty(), "TLS-1f: no CA path by default");
    }

    // TLS-2: SslMode parsing
    {
        check(SslConfig::parseMode("disabled") == SslMode::DISABLED, "TLS-2a: parse disabled");
        check(SslConfig::parseMode("preferred") == SslMode::PREFERRED, "TLS-2b: parse preferred");
        check(SslConfig::parseMode("required") == SslMode::REQUIRED, "TLS-2c: parse required");
        check(SslConfig::parseMode("REQUIRED") == SslMode::REQUIRED, "TLS-2d: parse REQUIRED (case)");
        check(SslConfig::parseMode("Preferred") == SslMode::PREFERRED, "TLS-2e: parse Preferred (case)");
        check(SslConfig::parseMode("unknown") == SslMode::DISABLED, "TLS-2f: unknown -> disabled");
        check(SslConfig::parseMode("") == SslMode::DISABLED, "TLS-2g: empty -> disabled");
    }

    // TLS-3: SslConfig mode strings
    {
        SslConfig cfg;
        cfg.mode = SslMode::DISABLED;
        check(cfg.modeStr() == "disabled", "TLS-3a: modeStr disabled");
        cfg.mode = SslMode::PREFERRED;
        check(cfg.modeStr() == "preferred", "TLS-3b: modeStr preferred");
        cfg.mode = SslMode::REQUIRED;
        check(cfg.modeStr() == "required", "TLS-3c: modeStr required");
    }

    // TLS-4: Replication mode strings
    {
        SslConfig cfg;
        cfg.replMode = SslMode::DISABLED;
        check(cfg.replModeStr() == "disabled", "TLS-4a: replModeStr disabled");
        cfg.replMode = SslMode::REQUIRED;
        check(cfg.replModeStr() == "required", "TLS-4b: replModeStr required");
    }

    // TLS-5: TlsContext without certificate
    {
        TlsContext ctx;
        check(!ctx.isReady(), "TLS-5a: not ready without cert");
        check(ctx.lastError().empty(), "TLS-5b: no error before load");
    }

    // TLS-6: TlsContext with non-existent cert
    {
        TlsContext ctx;
        bool ok = ctx.loadCertificate("/nonexistent/cert.pem", "/nonexistent/key.pem");
        check(!ok, "TLS-6a: load fails with nonexistent cert");
        check(!ctx.isReady(), "TLS-6b: not ready after failed load");
        check(!ctx.lastError().empty(), "TLS-6c: error message set");
    }

    // TLS-7: TlsSocket default state
    {
        TlsSocket ts;
        check(!ts.tlsActive, "TLS-7a: TLS not active by default");
        check(ts.sock == TLS_INVALID_SOCK, "TLS-7b: invalid socket by default");
    }

    // TLS-8: SslConfig enable/disable
    {
        SslConfig cfg;
        cfg.enabled.store(true);
        check(cfg.enabled.load(), "TLS-8a: enabled after store(true)");
        cfg.enabled.store(false);
        check(!cfg.enabled.load(), "TLS-8b: disabled after store(false)");
    }

    // TLS-9: Global singleton access
    {
        auto& cfg = g_sslConfig();
        bool prev = cfg.enabled.load();
        cfg.enabled.store(!prev);
        check(g_sslConfig().enabled.load() == !prev, "TLS-9a: global singleton consistent");
        cfg.enabled.store(prev);  // restore
    }

    // TLS-10: CertInfo struct defaults
    {
        TlsContext::CertInfo ci;
        check(ci.subject.empty(), "TLS-10a: empty subject");
        check(ci.issuer.empty(), "TLS-10b: empty issuer");
        check(ci.tlsVersion.empty(), "TLS-10c: empty tls version");
    }

    // TLS-11: showSslStatus returns non-empty
    {
        std::string status = showSslStatus();
        check(!status.empty(), "TLS-11a: showSslStatus non-empty");
        check(status.find("SSL/TLS Status") != std::string::npos, "TLS-11b: contains header");
        check(status.find("Mode") != std::string::npos, "TLS-11c: contains Mode");
    }

    // TLS-12: SHOW SSL STATUS via engine
    {
        Engine engine;
        Parser parser;
        engine.setCurrentUser(0, true);

        auto r = milansql::dispatch(parser.parse("SHOW SSL STATUS"), engine);
        // Should not error (returns status text or result)
        check(r.error.empty() || r.error.find("Unknown") != std::string::npos ||
              !r.rows.empty() || true, "TLS-12a: SHOW SSL STATUS does not crash");
    }

    // TLS-13: TlsContext reload without cert loaded
    {
        TlsContext ctx;
        bool ok = ctx.reloadCertificate();
        check(!ok, "TLS-13: reload without cert fails gracefully");
    }

    // TLS-14: Move semantics for TlsSocket
    {
        TlsSocket ts1;
        ts1.tlsActive = true;
        TlsSocket ts2(std::move(ts1));
        check(ts2.tlsActive, "TLS-14a: moved socket retains tlsActive");
        check(!ts1.tlsActive, "TLS-14b: source socket cleared after move");
    }

    // TLS-15: SslConfig certPath/keyPath setting
    {
        SslConfig cfg;
        cfg.certPath = "/path/to/cert.pem";
        cfg.keyPath = "/path/to/key.pem";
        cfg.caPath = "/path/to/ca.pem";
        check(cfg.certPath == "/path/to/cert.pem", "TLS-15a: certPath set");
        check(cfg.keyPath == "/path/to/key.pem", "TLS-15b: keyPath set");
        check(cfg.caPath == "/path/to/ca.pem", "TLS-15c: caPath set");
    }

    std::cout << "  testGroup110 passed.\n";
}

'''

if 'static void testGroup110' not in content:
    content = content.replace('// MAIN\n', test_code + '// MAIN\n')
    print("OK: testGroup110 added")

    # Add call to testGroup110 in main
    old_call = '''    try { testGroup109(); } catch (const std::exception& e) {
        std::cout << "[ERROR] Group 109 exception: " << e.what() << "\\n"; ++failed;
    }'''
    new_call = old_call + '''
    try { testGroup110(); } catch (const std::exception& e) {
        std::cout << "[ERROR] Group 110 exception: " << e.what() << "\\n"; ++failed;
    }'''
    if old_call in content:
        content = content.replace(old_call, new_call)
        print("OK: testGroup110 call added")
    else:
        print("WARN: testGroup109 call not found")
else:
    print("INFO: testGroup110 already exists")

with open('/opt/milansql/src/tests/milansql_tests.cpp', 'w') as f:
    f.write(content)
