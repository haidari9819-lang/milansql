#!/usr/bin/env python3
"""Patch WebUI: per-table RLS policy display in sidebar + SQL editor RLS button."""

HTTP = "/opt/milansql/src/server/http_server.hpp"
ENGINE = "/opt/milansql/src/engine/engine.hpp"

def read(path):
    with open(path, 'r') as f:
        return f.read()

def write(path, content):
    with open(path, 'w') as f:
        f.write(content)

def patch(path, old, new, label):
    content = read(path)
    if old not in content:
        print(f"  SKIP: {label} — pattern not found")
        return False
    content = content.replace(old, new, 1)
    write(path, content)
    print(f"  OK: {label}")
    return True

# ══════════════════════════════════════════════════════════════
# 1. Add getRlsPoliciesJson() to engine
# ══════════════════════════════════════════════════════════════
print("=== 1. Engine: getRlsPoliciesJson ===")
patch(ENGINE,
    '''    void showPolicies(const std::string& table) const {''',
    '''    // Phase 170: JSON export of all RLS policies (for WebUI)
    std::string getRlsPoliciesJson() const {
        std::string json = "{";
        // Enabled tables
        json += "\\"enabled_tables\\":[";
        bool first = true;
        for (const auto& tbl : rlsEnabled_) {
            if (!first) json += ",";
            json += "\\"" + tbl + "\\"";
            first = false;
        }
        json += "],\\"policies\\":{";
        first = true;
        for (const auto& [tbl, pols] : rlsPolicies_) {
            if (!first) json += ",";
            json += "\\"" + tbl + "\\":[";
            bool first2 = true;
            for (const auto& p : pols) {
                if (!first2) json += ",";
                json += "{\\"name\\":\\"" + p.name + "\\",\\"command\\":\\"" + p.command
                       + "\\",\\"role\\":\\"" + p.role + "\\",\\"using\\":\\"";
                // Escape quotes in expressions
                for (char c : p.usingExpr) {
                    if (c == '"') json += "\\\\\\"";
                    else if (c == '\\\\') json += "\\\\\\\\";
                    else json += c;
                }
                json += "\\"";
                if (!p.withCheckExpr.empty()) {
                    json += ",\\"with_check\\":\\"";
                    for (char c : p.withCheckExpr) {
                        if (c == '"') json += "\\\\\\"";
                        else if (c == '\\\\') json += "\\\\\\\\";
                        else json += c;
                    }
                    json += "\\"";
                }
                json += "}";
                first2 = false;
            }
            json += "]";
            first = false;
        }
        json += "}}";
        return json;
    }

    // Phase 170: JSON policies for a specific table
    std::string getTablePoliciesJson(const std::string& table) const {
        auto key = resolveTableName(table);
        bool enabled = rlsEnabled_.count(key) > 0;
        std::string json = "{\\"table\\":\\"" + key + "\\",\\"rls_enabled\\":" + (enabled ? "true" : "false");
        json += ",\\"policies\\":[";
        auto it = rlsPolicies_.find(key);
        if (it != rlsPolicies_.end()) {
            bool first = true;
            for (const auto& p : it->second) {
                if (!first) json += ",";
                json += "{\\"name\\":\\"" + p.name + "\\",\\"command\\":\\"" + p.command
                       + "\\",\\"role\\":\\"" + p.role + "\\",\\"using\\":\\"";
                for (char c : p.usingExpr) {
                    if (c == '"') json += "\\\\\\"";
                    else if (c == '\\\\') json += "\\\\\\\\";
                    else json += c;
                }
                json += "\\"";
                if (!p.withCheckExpr.empty()) {
                    json += ",\\"with_check\\":\\"";
                    for (char c : p.withCheckExpr) {
                        if (c == '"') json += "\\\\\\"";
                        else if (c == '\\\\') json += "\\\\\\\\";
                        else json += c;
                    }
                    json += "\\"";
                }
                json += "}";
                first = false;
            }
        }
        json += "]}";
        return json;
    }

    void showPolicies(const std::string& table) const {''',
    "getRlsPoliciesJson + getTablePoliciesJson")

# ══════════════════════════════════════════════════════════════
# 2. Add /api/rls-policies route to HTTP server
# ══════════════════════════════════════════════════════════════
print("\n=== 2. HTTP route: /api/rls-policies ===")

content = read(HTTP)

# Find a good anchor to insert before — the /tables route
rls_route = '''    // Phase 170: RLS policies API
    if (req.path == "/api/rls-policies") {
        std::lock_guard<std::mutex> lock(engineMutex_);
        return buildHttpResponse(200, engine_.getRlsPoliciesJson(), "application/json");
    }

    // Phase 170: Per-table RLS policies
    if (req.path.rfind("/api/rls-policies/", 0) == 0) {
        std::string tblName = req.path.substr(18);  // after "/api/rls-policies/"
        std::lock_guard<std::mutex> lock(engineMutex_);
        return buildHttpResponse(200, engine_.getTablePoliciesJson(tblName), "application/json");
    }

    '''

anchor = '    if (req.path == "/tables") {'
if '/api/rls-policies' not in content:
    content = content.replace(anchor, rls_route + anchor)
    print("  OK: /api/rls-policies route added")
else:
    print("  SKIP: route already exists")

write(HTTP, content)

# ══════════════════════════════════════════════════════════════
# 3. Intercept SHOW POLICIES in handleQueryForUser for JSON output
# ══════════════════════════════════════════════════════════════
print("\n=== 3. SHOW POLICIES JSON intercept ===")

content = read(HTTP)

# Find the @@variable intercepts section ending and add SHOW POLICIES intercept after it
show_pol_intercept = '''
    // Phase 170: SHOW POLICIES ON <table> — return as JSON result set
    if (upper.rfind("SHOW POLICIES", 0) == 0) {
        // Extract table name after ON
        std::string tblName;
        auto onPos = upper.find(" ON ");
        if (onPos != std::string::npos) {
            tblName = trimmed.substr(onPos + 4);
            while (!tblName.empty() && (tblName.back()==';'||tblName.back()==' ')) tblName.pop_back();
            while (!tblName.empty() && tblName.front()==' ') tblName.erase(0,1);
        }
        if (!tblName.empty()) {
            return engine_.getTablePoliciesJson(tblName);
        }
        return engine_.getRlsPoliciesJson();
    }

'''

# Insert after the @@variable block (find the closing brace of that block)
show_anchor = '    // Phase 157: @@variable intercepts'
# Actually let's find a better spot — after the @@variable block
# Look for the next section after @@variables
intercept_anchor = "    // Intercept special SQL commands\n"
if "SHOW POLICIES" not in content.split("handleQueryForUser")[1][:2000]:
    # Find position after @@variable block closes
    # Search for the pattern that comes after the @@variable intercepts
    pos_after_vars = content.find("        if (u2 == \"SELECT @@AUTOCOMMIT\")")
    if pos_after_vars > 0:
        # Find the closing } of that if block and the parent block
        # Let's insert before the next major section
        next_section = content.find("\n    // ", pos_after_vars + 100)
        if next_section > 0:
            content = content[:next_section] + "\n" + show_pol_intercept + content[next_section:]
            print("  OK: SHOW POLICIES JSON intercept added")
        else:
            print("  SKIP: couldn't find insertion point")
    else:
        print("  SKIP: @@AUTOCOMMIT anchor not found")
else:
    print("  SKIP: SHOW POLICIES intercept already exists")

write(HTTP, content)

# ══════════════════════════════════════════════════════════════
# 4. Update sidebar table list to show RLS badges
# ══════════════════════════════════════════════════════════════
print("\n=== 4. Sidebar: RLS policy badges ===")

content = read(HTTP)

# Replace the loadSidebarTables function
OLD_SIDEBAR = '''async function loadSidebarTables() {
  try {
    var r = await fetch('/tables', {credentials:'include'});
    var data = await r.json();
    var tables = Array.isArray(data) ? data : (data.tables || []);
    var el = document.getElementById('sidebar-tables');
    if (!tables.length) { el.innerHTML = '<div style="font-size:0.75rem;color:#484f58;padding:4px 8px">No tables</div>'; return; }
    el.innerHTML = tables.map(function(t) {
      var name = typeof t === 'string' ? t : t.name;
      return '<div class="table-item" onclick="selectFromTable(\\'' + escAttr(name) + '\\')">' + escHtml(name) + '</div>';
    }).join('');
  } catch(e) { /* silent */ }
}'''

NEW_SIDEBAR = '''var _rlsPoliciesCache = {};
async function loadRlsPolicies() {
  try {
    var r = await fetch('/api/rls-policies', {credentials:'include'});
    _rlsPoliciesCache = await r.json();
  } catch(e) { _rlsPoliciesCache = {}; }
}

function getRlsBadge(tableName) {
  var pols = _rlsPoliciesCache.policies || {};
  // Check both raw name and with user prefix
  var count = 0;
  var enabled = false;
  var enabledTables = _rlsPoliciesCache.enabled_tables || [];
  for (var key in pols) {
    if (key === tableName || key.endsWith('_' + tableName)) {
      count = pols[key].length;
    }
  }
  for (var i = 0; i < enabledTables.length; i++) {
    if (enabledTables[i] === tableName || enabledTables[i].endsWith('_' + tableName)) {
      enabled = true;
    }
  }
  if (!enabled && count === 0) return '';
  if (enabled && count > 0)
    return '<span style="margin-left:auto;background:#1c3a2a;color:#3fb950;font-size:9px;padding:1px 5px;border-radius:8px;font-weight:600" title="' + count + ' RLS ' + (count===1?'Policy':'Policies') + '">' + count + ' RLS</span>';
  if (enabled)
    return '<span style="margin-left:auto;background:#1c3a2a;color:#3fb950;font-size:9px;padding:1px 5px;border-radius:8px" title="RLS enabled (no policies)">RLS</span>';
  return '';
}

async function loadSidebarTables() {
  try {
    await loadRlsPolicies();
    var r = await fetch('/tables', {credentials:'include'});
    var data = await r.json();
    var tables = Array.isArray(data) ? data : (data.tables || []);
    var el = document.getElementById('sidebar-tables');
    if (!tables.length) { el.innerHTML = '<div style="font-size:0.75rem;color:#484f58;padding:4px 8px">No tables</div>'; return; }
    el.innerHTML = tables.map(function(t) {
      var name = typeof t === 'string' ? t : t.name;
      var badge = getRlsBadge(name);
      return '<div class="table-item" style="display:flex;align-items:center" onclick="selectFromTable(\\'' + escAttr(name) + '\\')">'
        + '<span>' + escHtml(name) + '</span>' + badge + '</div>';
    }).join('');
  } catch(e) { /* silent */ }
}'''

if OLD_SIDEBAR in content:
    content = content.replace(OLD_SIDEBAR, NEW_SIDEBAR)
    print("  OK: sidebar with RLS badges")
else:
    print("  SKIP: sidebar function not found")

write(HTTP, content)

# ══════════════════════════════════════════════════════════════
# 5. Update RLS panel with dynamic per-table info
# ══════════════════════════════════════════════════════════════
print("\n=== 5. RLS panel: dynamic per-table info ===")

OLD_RLS_PANEL = '''    rp.innerHTML='<div style="font-size:10px;color:#6e7681;text-transform:uppercase;letter-spacing:0.5px;margin-bottom:4px">Row Level Security</div>'
      +'<div style="display:flex;align-items:center;gap:6px;margin-bottom:3px"><span style="color:#3fb950;font-size:9px">●</span><span style="font-size:11px;color:#3fb950;font-weight:600">ACTIVE</span></div>'
      +'<div style="font-size:10px;color:#8b949e">User: <b style="color:#cdd6f4">'+escHtml(msUser)+'</b> (id: '+msUserId+')</div>'
      +'<div style="font-size:10px;color:#8b949e">Isolation: <b style="color:#3fb950">ENABLED</b></div>';'''

NEW_RLS_PANEL = '''    var rlsHtml='<div style="font-size:10px;color:#6e7681;text-transform:uppercase;letter-spacing:0.5px;margin-bottom:4px">Row Level Security</div>'
      +'<div style="display:flex;align-items:center;gap:6px;margin-bottom:3px"><span style="color:#3fb950;font-size:9px">●</span><span style="font-size:11px;color:#3fb950;font-weight:600">ACTIVE</span></div>'
      +'<div style="font-size:10px;color:#8b949e;margin-bottom:3px">User: <b style="color:#cdd6f4">'+escHtml(msUser)+'</b></div>';
    var enabledCount = (_rlsPoliciesCache.enabled_tables||[]).length;
    var totalPolicies = 0;
    var pols = _rlsPoliciesCache.policies || {};
    for (var k in pols) totalPolicies += pols[k].length;
    rlsHtml += '<div style="font-size:10px;color:#8b949e;margin-bottom:2px">Tables: <b style="color:#58a6ff">'+enabledCount+'</b> protected</div>';
    rlsHtml += '<div style="font-size:10px;color:#8b949e">Policies: <b style="color:#f0a500">'+totalPolicies+'</b> active</div>';
    rp.innerHTML = rlsHtml;'''

patch(HTTP, OLD_RLS_PANEL, NEW_RLS_PANEL, "RLS panel dynamic info")

# ══════════════════════════════════════════════════════════════
# 6. Add "RLS Policies" button to SQL editor toolbar
# ══════════════════════════════════════════════════════════════
print("\n=== 6. SQL editor: RLS Policies button ===")

patch(HTTP,
    '''          <button class="btn btn-gray" onclick="copyCSV()" title="Copy results as CSV">&#x1F4CB; CSV</button>
          <span class="exec-time" id="exec-time"></span>''',
    '''          <button class="btn btn-gray" onclick="copyCSV()" title="Copy results as CSV">&#x1F4CB; CSV</button>
          <button class="btn btn-gray" onclick="showRlsPolicies()" title="Show RLS policies for selected table" style="border-color:#f0a500;color:#f0a500">&#x1F6E1; RLS</button>
          <span class="exec-time" id="exec-time"></span>''',
    "RLS Policies button in toolbar")

# ══════════════════════════════════════════════════════════════
# 7. Add showRlsPolicies() JavaScript function
# ══════════════════════════════════════════════════════════════
print("\n=== 7. showRlsPolicies() function ===")

SHOW_RLS_FUNC = '''
// Phase 170: Show RLS Policies for table in editor
async function showRlsPolicies() {
  // Try to detect table name from current SQL
  var sql = document.getElementById('sql-editor').value.trim();
  var tblMatch = sql.match(/(?:FROM|JOIN|TABLE|ON|INTO|UPDATE|POLICIES)\\s+([a-zA-Z_][a-zA-Z0-9_]*)/i);
  if (!tblMatch) {
    // If no table in SQL, show all policies
    try {
      var r = await fetch('/api/rls-policies', {credentials:'include'});
      var data = await r.json();
      var out = document.getElementById('output-area');
      var html = '<div style="padding:12px"><h3 style="color:#f0a500;margin-bottom:12px">&#x1F6E1; RLS Policies Overview</h3>';
      var enabled = data.enabled_tables || [];
      html += '<div style="color:#8b949e;margin-bottom:8px">Protected tables: <b style="color:#3fb950">' + enabled.length + '</b></div>';
      if (enabled.length === 0) {
        html += '<div style="color:#484f58">No tables have RLS enabled. Use: ALTER TABLE name ENABLE ROW LEVEL SECURITY;</div>';
      } else {
        html += '<table><thead><tr><th>Table</th><th>Policies</th><th>Details</th></tr></thead><tbody>';
        for (var i = 0; i < enabled.length; i++) {
          var tbl = enabled[i];
          var pols = (data.policies || {})[tbl] || [];
          var details = pols.map(function(p) { return '<span style="color:#58a6ff">' + escHtml(p.name) + '</span> (' + p.command + ' TO ' + p.role + ')'; }).join(', ') || '<span style="color:#484f58">none</span>';
          html += '<tr><td style="color:#cdd6f4;font-weight:600">' + escHtml(tbl) + '</td><td style="text-align:center">' + pols.length + '</td><td>' + details + '</td></tr>';
        }
        html += '</tbody></table>';
      }
      html += '</div>';
      if (out) out.innerHTML = html;
    } catch(e) { /* silent */ }
    return;
  }

  var tblName = tblMatch[1];
  try {
    var r = await fetch('/api/rls-policies/' + encodeURIComponent(tblName), {credentials:'include'});
    var data = await r.json();
    var out = document.getElementById('output-area');
    var html = '<div style="padding:12px"><h3 style="color:#f0a500;margin-bottom:12px">&#x1F6E1; RLS Policies: ' + escHtml(tblName) + '</h3>';
    html += '<div style="margin-bottom:8px;color:#8b949e">RLS Status: ' + (data.rls_enabled ? '<b style="color:#3fb950">ENABLED</b>' : '<b style="color:#f38ba8">DISABLED</b>') + '</div>';
    var pols = data.policies || [];
    if (pols.length === 0) {
      html += '<div style="color:#484f58;margin-bottom:8px">No policies defined.</div>';
      if (!data.rls_enabled) {
        html += '<div style="color:#8b949e;font-size:0.8rem">Enable RLS: <code style="color:#58a6ff">ALTER TABLE ' + escHtml(tblName) + ' ENABLE ROW LEVEL SECURITY;</code></div>';
      }
      html += '<div style="color:#8b949e;font-size:0.8rem;margin-top:4px">Create policy: <code style="color:#58a6ff">CREATE POLICY name ON ' + escHtml(tblName) + ' FOR ALL TO PUBLIC USING (expr);</code></div>';
    } else {
      html += '<table><thead><tr><th>Policy</th><th>Command</th><th>Role</th><th>USING</th><th>WITH CHECK</th></tr></thead><tbody>';
      for (var i = 0; i < pols.length; i++) {
        var p = pols[i];
        html += '<tr>';
        html += '<td style="color:#58a6ff;font-weight:600">' + escHtml(p.name) + '</td>';
        html += '<td><span style="background:#21262d;padding:2px 6px;border-radius:3px;font-size:0.75rem;color:#f0a500">' + escHtml(p.command) + '</span></td>';
        html += '<td>' + escHtml(p.role) + '</td>';
        html += '<td style="font-family:monospace;font-size:0.8rem;color:#cdd6f4">' + escHtml(p['using'] || '') + '</td>';
        html += '<td style="font-family:monospace;font-size:0.8rem;color:#cdd6f4">' + escHtml(p.with_check || '-') + '</td>';
        html += '</tr>';
      }
      html += '</tbody></table>';
    }
    html += '</div>';
    if (out) out.innerHTML = html;
  } catch(e) { /* silent */ }
}
'''

# Insert after the explainQuery function
content = read(HTTP)
explain_anchor = "async function explainQuery() {"
if 'showRlsPolicies' not in content:
    # Find the end of explainQuery function block — look for the next async function
    pos = content.find(explain_anchor)
    if pos > 0:
        # Find the next function definition after explainQuery
        next_fn = content.find("\nasync function ", pos + 10)
        if next_fn < 0:
            next_fn = content.find("\nfunction ", pos + 10)
        if next_fn > 0:
            content = content[:next_fn] + "\n" + SHOW_RLS_FUNC + content[next_fn:]
            print("  OK: showRlsPolicies() function added")
        else:
            print("  SKIP: couldn't find insertion point")
    else:
        print("  SKIP: explainQuery not found")
else:
    print("  SKIP: showRlsPolicies already exists")

write(HTTP, content)

# ══════════════════════════════════════════════════════════════
# 8. Also update browser table list to show RLS badges
# ══════════════════════════════════════════════════════════════
print("\n=== 8. Browser tab: RLS badges ===")

patch(HTTP,
    '''      return '<button class="tbl-btn" onclick="browseTable(\\'' + escAttr(name) + '\\',this)">' + escHtml(name) + '</button>';''',
    '''      var badge = getRlsBadge(name);
      return '<button class="tbl-btn" style="display:flex;align-items:center" onclick="browseTable(\\'' + escAttr(name) + '\\',this)"><span>' + escHtml(name) + '</span>' + badge + '</button>';''',
    "browser table list RLS badges")

print("\n" + "=" * 50)
print("ALL WEBUI RLS PATCHES APPLIED")
print("=" * 50)
