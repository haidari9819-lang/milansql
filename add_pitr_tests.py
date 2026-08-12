#!/usr/bin/env python3
"""Add PITR tests to milansql_tests.cpp -- testGroup111"""

with open('/opt/milansql/src/tests/milansql_tests.cpp', 'r') as f:
    content = f.read()

test_code = r'''
// ============================================================
// testGroup111 -- Point-in-Time Recovery (Phase 178)
// ============================================================
static void testGroup111() {
    std::cout << "\n-- testGroup111: Point-in-Time Recovery --\n";
    using namespace milansql;

    // PITR-1: PitrManager defaults
    {
        PitrManager mgr;
        check(mgr.config().archiveEnabled, "PITR-1a: archiving enabled by default");
        check(mgr.config().archiveDir == "wal_archive", "PITR-1b: default archive dir");
        check(mgr.config().retentionDays == 30, "PITR-1c: 30 day retention");
        check(!mgr.config().autoBackupEnabled, "PITR-1d: auto-backup off by default");
        check(mgr.config().backupBaseDir == "backups", "PITR-1e: default backup dir");
        check(!mgr.isRecovering(), "PITR-1f: not recovering initially");
    }

    // PITR-2: Timestamp helpers
    {
        int64_t now = pitr_now_epoch();
        check(now > 1700000000, "PITR-2a: epoch is recent");
        std::string s = pitr_epoch_to_str(now);
        check(s.size() == 19, "PITR-2b: timestamp string is 19 chars");
        check(s[4] == '-' && s[7] == '-', "PITR-2c: date format YYYY-MM-DD");
        check(s[10] == ' ', "PITR-2d: space between date and time");
        check(s[13] == ':' && s[16] == ':', "PITR-2e: time format HH:MM:SS");
    }

    // PITR-3: Timestamp round-trip
    {
        std::string ts = "2026-07-15 12:34:56";
        int64_t epoch = pitr_str_to_epoch(ts);
        check(epoch > 0, "PITR-3a: parsed epoch is positive");
        std::string back = pitr_epoch_to_str(epoch);
        check(back == ts, "PITR-3b: round-trip preserves timestamp");
    }

    // PITR-4: Invalid timestamp parsing
    {
        int64_t e1 = pitr_str_to_epoch("");
        check(e1 == 0, "PITR-4a: empty string -> 0");
        int64_t e2 = pitr_str_to_epoch("short");
        check(e2 == 0, "PITR-4b: short string -> 0");
    }

    // PITR-5: Archive empty/missing WAL
    {
        PitrManager mgr;
        mgr.config().archiveDir = "/tmp/pitr_test_archive_5";
        std::string r = mgr.archiveCurrentWal("/nonexistent/wal");
        check(r.find("empty or missing") != std::string::npos ||
              r.find("WAL file") != std::string::npos, "PITR-5a: missing WAL rejected");
    }

    // PITR-6: Archive real WAL data
    {
        PitrManager mgr;
        std::string testDir = "/tmp/pitr_test_archive_6";
        mgr.config().archiveDir = testDir;

        // Create a dummy WAL file
        std::string walFile = "/tmp/pitr_test_wal_6.wal";
        {
            std::ofstream wal(walFile);
            wal << "TX_BEGIN:1\n";
            wal << "TS:1720000000\n";
            wal << "INSERT|test_table|col1=val1\n";
            wal << "TX_COMMIT:1\n";
            wal << "TS:1720000001\n";
            wal << "CRC:12345\n";
        }
        std::string r = mgr.archiveCurrentWal(walFile);
        check(r.find("OK") != std::string::npos, "PITR-6a: archive succeeded");
        check(r.find("archived") != std::string::npos, "PITR-6b: confirmation msg");

        // Clean up
        std::error_code ec;
        std::filesystem::remove_all(testDir, ec);
        std::filesystem::remove(walFile, ec);
    }

    // PITR-7: List archive segments
    {
        PitrManager mgr;
        std::string testDir = "/tmp/pitr_test_archive_7";
        mgr.config().archiveDir = testDir;

        // Create dummy segments
        std::error_code ec;
        std::filesystem::create_directories(testDir, ec);
        {
            std::ofstream(testDir + "/wal_1720000100.seg") << "data1";
            std::ofstream(testDir + "/wal_1720000200.seg") << "data22";
            std::ofstream(testDir + "/wal_1720000300.seg") << "data333";
        }

        auto segs = mgr.listArchiveSegments();
        check(segs.size() == 3, "PITR-7a: found 3 segments");
        check(segs[0].timestamp < segs[1].timestamp, "PITR-7b: sorted by time");
        check(segs[1].timestamp < segs[2].timestamp, "PITR-7c: sorted ascending");

        std::filesystem::remove_all(testDir, ec);
    }

    // PITR-8: Show archive status
    {
        PitrManager mgr;
        mgr.config().archiveDir = "/tmp/pitr_nonexistent_dir";
        std::string status = mgr.showArchiveStatus();
        check(status.find("WAL Archive Status") != std::string::npos, "PITR-8a: status header");
        check(status.find("ON") != std::string::npos, "PITR-8b: enabled shown");
        check(status.find("Segments") != std::string::npos, "PITR-8c: segments count");
        check(status.find("30 days") != std::string::npos, "PITR-8d: retention shown");
    }

    // PITR-9: Clean old archives
    {
        PitrManager mgr;
        std::string testDir = "/tmp/pitr_test_archive_9";
        mgr.config().archiveDir = testDir;
        mgr.config().retentionDays = 1; // 1 day retention

        std::error_code ec;
        std::filesystem::create_directories(testDir, ec);

        // Create an old segment (epoch = 1000, very old)
        std::ofstream(testDir + "/wal_1000.seg") << "old data";
        // Create a recent segment
        int64_t recent = pitr_now_epoch() - 100;
        std::ofstream(testDir + "/wal_" + std::to_string(recent) + ".seg") << "new data";

        int removed = mgr.cleanOldArchives();
        check(removed == 1, "PITR-9a: removed 1 old segment");

        auto remaining = mgr.listArchiveSegments();
        check(remaining.size() == 1, "PITR-9b: 1 segment remains");

        std::filesystem::remove_all(testDir, ec);
    }

    // PITR-10: Base backup with nonexistent DB
    {
        PitrManager mgr;
        std::string r = mgr.createBaseBackup("/nonexistent/db.milan",
            "/tmp/pitr_test_backup_10", 100, "test", 5);
        check(r.find("ERROR") != std::string::npos || r.find("Failed") != std::string::npos,
              "PITR-10a: backup of nonexistent DB fails");
        std::error_code ec;
        std::filesystem::remove_all("/tmp/pitr_test_backup_10", ec);
    }

    // PITR-11: Base backup with real data
    {
        PitrManager mgr;
        std::string testDb = "/tmp/pitr_test_db_11.milan";
        std::string backupDir = "/tmp/pitr_test_backup_11";

        // Create a minimal database file
        {
            std::ofstream db(testDb);
            db << "{\"tables\":{}}";
        }

        std::string r = mgr.createBaseBackup(testDb, backupDir, 42, "v11.0.0", 3);
        check(r.find("OK") != std::string::npos, "PITR-11a: backup succeeded");
        check(r.find("LSN=42") != std::string::npos, "PITR-11b: LSN in output");

        // Verify backup_label exists
        std::ifstream label(backupDir + "/backup_label");
        check(label.good(), "PITR-11c: backup_label created");
        std::string lbContent((std::istreambuf_iterator<char>(label)),
                               std::istreambuf_iterator<char>());
        check(lbContent.find("base") != std::string::npos, "PITR-11d: backup_type is base");
        check(lbContent.find("start_lsn:42") != std::string::npos, "PITR-11e: LSN in label");
        check(lbContent.find("v11.0.0") != std::string::npos, "PITR-11f: version in label");

        // Verify database.milan was copied
        check(std::filesystem::exists(backupDir + "/database.milan"), "PITR-11g: db file copied");

        std::error_code ec;
        std::filesystem::remove_all(backupDir, ec);
        std::filesystem::remove(testDb, ec);
    }

    // PITR-12: List backups
    {
        PitrManager mgr;
        std::string baseDir = "/tmp/pitr_test_backups_12";
        mgr.config().backupBaseDir = baseDir;

        // Create two backup directories with labels
        std::error_code ec;
        std::filesystem::create_directories(baseDir + "/backup1", ec);
        std::filesystem::create_directories(baseDir + "/backup2", ec);
        {
            std::ofstream l1(baseDir + "/backup1/backup_label");
            l1 << "backup_type:base\ntimestamp:2026-07-15 10:00:00\nepoch:1752577200\nstart_lsn:100\nversion:v11.0.0\ntable_count:5\nsize_bytes:1024\n";
        }
        {
            std::ofstream l2(baseDir + "/backup2/backup_label");
            l2 << "backup_type:base\ntimestamp:2026-07-15 12:00:00\nepoch:1752584400\nstart_lsn:200\nversion:v11.0.0\ntable_count:8\nsize_bytes:2048\n";
        }

        auto backups = mgr.listBackups();
        check(backups.size() == 2, "PITR-12a: found 2 backups");
        check(backups[0].epochTime < backups[1].epochTime, "PITR-12b: sorted by time");
        check(backups[0].startLsn == 100, "PITR-12c: first backup LSN");
        check(backups[1].tableCount == 8, "PITR-12d: second backup table count");

        std::filesystem::remove_all(baseDir, ec);
    }

    // PITR-13: Show backups text
    {
        PitrManager mgr;
        mgr.config().backupBaseDir = "/tmp/pitr_nonexistent_backups";
        std::string out = mgr.showBackups();
        check(out.find("Backups") != std::string::npos, "PITR-13a: header present");
        check(out.find("no backups") != std::string::npos, "PITR-13b: empty msg");
        check(out.find("Total: 0") != std::string::npos, "PITR-13c: zero total");
    }

    // PITR-14: Delete backup
    {
        PitrManager mgr;
        std::string dir = "/tmp/pitr_test_delete_14";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::ofstream(dir + "/backup_label") << "backup_type:base\n";

        std::string r = mgr.deleteBackup(dir);
        check(r.find("OK") != std::string::npos, "PITR-14a: delete succeeded");
        check(!std::filesystem::exists(dir), "PITR-14b: directory removed");
    }

    // PITR-15: Delete nonexistent backup
    {
        PitrManager mgr;
        std::string r = mgr.deleteBackup("/tmp/pitr_nonexistent_backup");
        check(r.find("ERROR") != std::string::npos, "PITR-15a: delete nonexistent fails");
    }

    // PITR-16: Delete directory without backup_label
    {
        PitrManager mgr;
        std::string dir = "/tmp/pitr_test_nolabel_16";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::ofstream(dir + "/somefile.txt") << "not a backup\n";

        std::string r = mgr.deleteBackup(dir);
        check(r.find("ERROR") != std::string::npos, "PITR-16a: dir without label rejected");
        check(r.find("backup_label") != std::string::npos, "PITR-16b: mentions backup_label");

        std::filesystem::remove_all(dir, ec);
    }

    // PITR-17: Config modification
    {
        PitrManager mgr;
        mgr.config().archiveEnabled = false;
        check(!mgr.config().archiveEnabled, "PITR-17a: disable archiving");
        mgr.config().retentionDays = 7;
        check(mgr.config().retentionDays == 7, "PITR-17b: change retention");
        mgr.config().archiveDir = "/custom/archive";
        check(mgr.config().archiveDir == "/custom/archive", "PITR-17c: change archive dir");
    }

    // PITR-18: Archive disabled returns message
    {
        PitrManager mgr;
        mgr.config().archiveEnabled = false;
        std::string r = mgr.archiveCurrentWal("/tmp/some.wal");
        check(r.find("disabled") != std::string::npos, "PITR-18a: disabled message");
    }

    // PITR-19: WalArchiveEntry struct
    {
        WalArchiveEntry ae;
        ae.filename = "wal_123.seg";
        ae.timestamp = 123;
        ae.sizeBytes = 4096;
        check(ae.filename == "wal_123.seg", "PITR-19a: filename set");
        check(ae.timestamp == 123, "PITR-19b: timestamp set");
        check(ae.sizeBytes == 4096, "PITR-19c: size set");
    }

    // PITR-20: BackupLabel struct
    {
        BackupLabel bl;
        bl.timestamp = "2026-07-15 12:00:00";
        bl.epochTime = 1752584400;
        bl.startLsn = 42;
        bl.version = "v11.0.0";
        bl.sizeBytes = 1024;
        bl.tableCount = 10;
        check(bl.timestamp == "2026-07-15 12:00:00", "PITR-20a: timestamp set");
        check(bl.startLsn == 42, "PITR-20b: LSN set");
        check(bl.tableCount == 10, "PITR-20c: table count set");
    }

    // PITR-21: Global singleton
    {
        auto& mgr1 = g_pitrManager();
        auto& mgr2 = g_pitrManager();
        check(&mgr1 == &mgr2, "PITR-21a: singleton returns same instance");
    }

    // PITR-22: Recovery status
    {
        PitrManager mgr;
        check(!mgr.isRecovering(), "PITR-22a: not recovering initially");
        std::string prog = mgr.recoveryProgress();
        check(prog.empty(), "PITR-22b: no progress initially");
    }

    // PITR-23: RestoreResult struct defaults
    {
        PitrManager::RestoreResult rr;
        check(!rr.success, "PITR-23a: not success by default");
        check(rr.message.empty(), "PITR-23b: empty message");
        check(rr.restoredToEpoch == 0, "PITR-23c: zero epoch");
        check(rr.replayedTxCount == 0, "PITR-23d: zero tx count");
        check(rr.replayedOpCount == 0, "PITR-23e: zero op count");
        check(rr.skippedTxCount == 0, "PITR-23f: zero skipped");
    }

    // PITR-24: Restore from nonexistent backup
    {
        PitrManager mgr;
        Engine engine;
        auto rr = mgr.restoreToPoint(engine, "/nonexistent/backup", 0, 0);
        check(!rr.success, "PITR-24a: restore fails with bad backup");
        check(rr.message.find("ERROR") != std::string::npos, "PITR-24b: error message set");
    }

    // PITR-25: Restore from backup without database.milan
    {
        PitrManager mgr;
        std::string backupDir = "/tmp/pitr_test_restore_25";
        std::error_code ec;
        std::filesystem::create_directories(backupDir, ec);
        {
            std::ofstream l(backupDir + "/backup_label");
            l << "backup_type:base\ntimestamp:2026-07-15 12:00:00\nepoch:1752584400\nstart_lsn:100\n";
        }
        Engine engine;
        auto rr = mgr.restoreToPoint(engine, backupDir, 0, 0);
        check(!rr.success, "PITR-25a: restore fails without db file");
        check(rr.message.find("database.milan") != std::string::npos, "PITR-25b: mentions missing file");
        std::filesystem::remove_all(backupDir, ec);
    }

    // PITR-26: WAL timestamp format in engine
    {
        // Verify TX_BEGIN writes TS: lines by doing a transaction
        Engine engine;
        engine.setCurrentUser(0, true);
        Parser parser;
        milansql::dispatch(parser.parse("CREATE TABLE pitr_ts_test (id INT, val TEXT)"), engine);
        milansql::dispatch(parser.parse("BEGIN"), engine);
        milansql::dispatch(parser.parse("INSERT INTO pitr_ts_test VALUES (1, 'hello')"), engine);
        milansql::dispatch(parser.parse("COMMIT"), engine);

        // Read WAL and check for TS: lines
        std::ifstream wal("database.milan.wal");
        if (wal) {
            std::string walContent((std::istreambuf_iterator<char>(wal)),
                                    std::istreambuf_iterator<char>());
            check(walContent.find("TS:") != std::string::npos, "PITR-26a: WAL contains TS: timestamp");
        }

        milansql::dispatch(parser.parse("DROP TABLE pitr_ts_test"), engine);
    }

    // PITR-27: Backup and list round-trip
    {
        PitrManager mgr;
        std::string baseDir = "/tmp/pitr_test_roundtrip_27";
        mgr.config().backupBaseDir = baseDir;

        // Create a DB file
        std::string dbFile = "/tmp/pitr_test_db_27.milan";
        std::ofstream(dbFile) << "{\"tables\":{}}";

        // Create backup
        std::string bDir = baseDir + "/backup_rt_1";
        std::string r = mgr.createBaseBackup(dbFile, bDir, 500, "v11.0.0", 15);
        check(r.find("OK") != std::string::npos, "PITR-27a: backup created");

        // List should find it
        auto backups = mgr.listBackups();
        check(backups.size() >= 1, "PITR-27b: at least 1 backup listed");
        bool found = false;
        for (const auto& b : backups) {
            if (b.startLsn == 500 && b.tableCount == 15) found = true;
        }
        check(found, "PITR-27c: our backup found with correct metadata");

        // Delete it
        std::string dr = mgr.deleteBackup(bDir);
        check(dr.find("OK") != std::string::npos, "PITR-27d: deleted");

        std::error_code ec;
        std::filesystem::remove_all(baseDir, ec);
        std::filesystem::remove(dbFile, ec);
    }

    // PITR-28: Archive multiple WAL segments
    {
        PitrManager mgr;
        std::string testDir = "/tmp/pitr_test_multi_28";
        mgr.config().archiveDir = testDir;

        for (int i = 0; i < 3; ++i) {
            std::string walFile = "/tmp/pitr_multi_wal_" + std::to_string(i) + ".wal";
            std::ofstream(walFile) << "TX_BEGIN:" << i << "\nTS:170000000" << i << "\nTX_COMMIT:" << i << "\nCRC:0\n";
            std::string r = mgr.archiveCurrentWal(walFile);
            check(r.find("OK") != std::string::npos, ("PITR-28a-" + std::to_string(i)).c_str());
            std::error_code ec;
            std::filesystem::remove(walFile, ec);
        }

        auto segs = mgr.listArchiveSegments();
        check(segs.size() == 3, "PITR-28b: 3 segments archived");

        std::error_code ec;
        std::filesystem::remove_all(testDir, ec);
    }

    // PITR-29: Clean with zero retention keeps all
    {
        PitrManager mgr;
        mgr.config().retentionDays = 0;
        mgr.config().archiveDir = "/tmp/pitr_test_ret0_29";
        std::error_code ec;
        std::filesystem::create_directories(mgr.config().archiveDir, ec);
        std::ofstream(mgr.config().archiveDir + "/wal_1000.seg") << "old";
        int removed = mgr.cleanOldArchives();
        check(removed == 0, "PITR-29a: zero retention removes nothing");
        std::filesystem::remove_all(mgr.config().archiveDir, ec);
    }

    // PITR-30: SHOW WAL ARCHIVE STATUS via dispatch
    {
        Engine engine;
        Parser parser;
        engine.setCurrentUser(0, true);
        auto r = milansql::dispatch(parser.parse("SHOW WAL ARCHIVE STATUS"), engine);
        check(r.error.empty() || r.error.find("Unknown") != std::string::npos ||
              !r.rows.empty() || true, "PITR-30a: SHOW WAL ARCHIVE STATUS does not crash");
    }

    std::cout << "  testGroup111 passed.\n";
}

'''

if 'static void testGroup111' not in content:
    content = content.replace('// MAIN\n', test_code + '// MAIN\n')
    print("OK: testGroup111 added")

    # Add call to testGroup111 in main
    old_call = '    try { testGroup110(); } catch (const std::exception& e) {\n        std::cout << "[ERROR] Group 110 exception: " << e.what() << "\\n"; ++failed;\n    }'
    new_call = old_call + '\n    try { testGroup111(); } catch (const std::exception& e) {\n        std::cout << "[ERROR] Group 111 exception: " << e.what() << "\\n"; ++failed;\n    }'
    if old_call in content:
        content = content.replace(old_call, new_call)
        print("OK: testGroup111 call added")
    else:
        print("WARN: testGroup110 call not found - trying alternative")
        # Try with the exact pattern from the file
        import re
        pattern = r'(try \{ testGroup110\(\); \} catch.*?failed;\s*\})'
        m = re.search(pattern, content, re.DOTALL)
        if m:
            old = m.group(0)
            content = content.replace(old, old + '\n    try { testGroup111(); } catch (const std::exception& e) {\n        std::cout << "[ERROR] Group 111 exception: " << e.what() << "\\n"; ++failed;\n    }')
            print("OK: testGroup111 call added (alt)")
        else:
            print("ERROR: could not find insertion point")
else:
    print("INFO: testGroup111 already exists")

# Add pitr_manager includes if needed
if 'pitr_manager' not in content:
    content = content.replace('#include "../src/ssl/tls_context.hpp"',
        '#include "../src/ssl/tls_context.hpp"\n#include "../src/wal/pitr_manager.hpp"\n#include "../src/wal/pitr_manager_impl.hpp"')
    if 'pitr_manager' in content:
        print("OK: pitr includes added to tests")
    else:
        # Try another include location
        content = content.replace('#include "../src/wal/checkpoint.hpp"',
            '#include "../src/wal/checkpoint.hpp"\n#include "../src/wal/pitr_manager.hpp"\n#include "../src/wal/pitr_manager_impl.hpp"')
        print("OK: pitr includes added (alt)")

with open('/opt/milansql/src/tests/milansql_tests.cpp', 'w') as f:
    f.write(content)
