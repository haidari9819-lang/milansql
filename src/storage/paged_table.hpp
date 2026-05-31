#pragma once
// ============================================================
// paged_table.hpp — Phase 84: PagedTable Class
// Included inside namespace milansql in engine.hpp (after PageManager).
// Do NOT add a namespace milansql wrapper here.
//
// PagedTable stores rows across a linked list of 8KB Pages.
// Each table has a schema (vector<Column>) and all rows
// serialised in pages managed by a shared PageManager.
//
// Page chain: page 0 → page 1 → ... → last page (nextPageId == NO_NEXT)
// On insert: try last page; if full, allocate a new page and link.
// ============================================================

struct PagedTable {
    std::string          name_;
    std::vector<Column>  columns_;
    PageManager*         mgr_ = nullptr;  // shared manager (Engine owns it)

    PagedTable() = default;
    PagedTable(const std::string& name,
               const std::vector<Column>& cols,
               PageManager& mgr)
        : name_(name), columns_(cols), mgr_(&mgr) {}

    const std::string&         name()    const { return name_; }
    const std::vector<Column>& columns() const { return columns_; }

    // ── Insert a row ──────────────────────────────────────────
    // Finds the last page in the chain, tries to append.
    // If full, allocates a new page and links it.
    void insertRow(const Row& row) {
        if (!mgr_) throw std::runtime_error("PagedTable: no PageManager");

        uint64_t pageCount = mgr_->getPageCount(name_);

        if (pageCount == 0) {
            // First page
            Page pg = mgr_->allocatePage(name_);
            pg.addRow(row);
            pg.updateChecksum();
            mgr_->writePage(name_, pg);
            mgr_->flushDirtyPages(name_);
            return;
        }

        // Walk to last page
        uint64_t lastId = pageCount - 1;  // last allocated page index

        Page last = mgr_->readPage(name_, lastId);
        if (last.addRow(row)) {
            last.updateChecksum();
            mgr_->writePage(name_, last);
            mgr_->flushDirtyPages(name_);
            return;
        }

        // Last page is full — allocate a new one
        Page newPg = mgr_->allocatePage(name_);
        uint64_t newId = newPg.pageId();

        // Update last page's nextPageId and flush
        last.setNextPageId(newId);
        last.updateChecksum();
        mgr_->writePage(name_, last);

        // Write row to new page
        newPg.addRow(row);
        newPg.updateChecksum();
        mgr_->writePage(name_, newPg);
        mgr_->flushDirtyPages(name_);
    }

    // ── Scan all rows ─────────────────────────────────────────
    std::vector<Row> scanAll() const {
        if (!mgr_) return {};
        std::vector<Row> result;
        uint64_t pageCount = mgr_->getPageCount(name_);
        for (uint64_t pid = 0; pid < pageCount; ++pid) {
            Page pg = mgr_->readPage(name_, pid);
            if (!pg.verifyChecksum()) continue;  // skip corrupt pages
            auto rows = pg.getRows();
            result.insert(result.end(), rows.begin(), rows.end());
        }
        return result;
    }

    // ── Row count (from all pages) ────────────────────────────
    size_t getRowCount() const {
        if (!mgr_) return 0;
        size_t total = 0;
        uint64_t pageCount = mgr_->getPageCount(name_);
        for (uint64_t pid = 0; pid < pageCount; ++pid) {
            Page pg = mgr_->readPage(name_, pid);
            if (!pg.verifyChecksum()) continue;
            total += pg.rowCount();
        }
        return total;
    }

    // ── Convert to in-memory Table (for SELECT processing) ────
    Table toTable() const {
        Table t(name_, columns_);
        for (const auto& row : scanAll())
            t.insert(row);
        return t;
    }

    // ── Page statistics string ────────────────────────────────
    std::string pageStats() const {
        if (!mgr_) return "  No manager\n";
        uint64_t pageCount = mgr_->getPageCount(name_);
        size_t totalRows = 0;
        char buf[1024];
        std::string out;
        out += "  Paged Table: " + name_ + "\n";
        out += "  Columns    : " + std::to_string(columns_.size()) + "\n";
        out += "  Pages      : " + std::to_string(pageCount) + "\n";
        for (uint64_t pid = 0; pid < pageCount; ++pid) {
            Page pg = mgr_->readPage(name_, pid);
            bool ok = pg.verifyChecksum();
            uint16_t rc = pg.rowCount();
            uint16_t wp = pg.writePos();
            totalRows += rc;
            std::snprintf(buf, sizeof(buf),
                "    Page %4llu: rows=%u, writePos=%u, checksum=%s\n",
                (unsigned long long)pid, rc, wp, ok ? "OK" : "FAIL");
            out += buf;
        }
        out += "  Total rows : " + std::to_string(totalRows) + "\n";
        return out;
    }
};
