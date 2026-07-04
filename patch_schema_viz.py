#!/usr/bin/env python3
"""Add Schema Visualizer to MilanSQL WebUI."""

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
        print(f"  SKIP: {label}")
        return False
    content = content.replace(old, new, 1)
    write(path, content)
    print(f"  OK: {label}")
    return True

# ══════════════════════════════════════════════════════════════
# 1. Add getSchemaJson() to engine
# ══════════════════════════════════════════════════════════════
print("=== 1. Engine: getSchemaJson ===")

content = read(ENGINE)

schema_json_method = '''    // Phase 171: Full schema JSON for Schema Visualizer
    std::string getSchemaJson() const {
        auto je = [](const std::string& s) -> std::string {
            std::string r;
            for (char c : s) {
                if (c == '"') r += "\\\\\\"";
                else if (c == '\\\\') r += "\\\\\\\\";
                else if (c == '\\n') r += "\\\\n";
                else r += c;
            }
            return r;
        };
        std::string json = "{\\"tables\\":[";
        bool first = true;
        for (const auto& [name, tbl] : tables_) {
            if (!first) json += ",";
            json += "{\\"name\\":\\"" + je(name) + "\\",\\"columns\\":[";
            const auto& cols = tbl.columns();
            for (size_t i = 0; i < cols.size(); ++i) {
                if (i > 0) json += ",";
                json += "{\\"name\\":\\"" + je(cols[i].name) + "\\",\\"type\\":\\"" + je(cols[i].type) + "\\"";
                if (cols[i].isPrimaryKey) json += ",\\"pk\\":true";
                if (cols[i].isUnique) json += ",\\"unique\\":true";
                if (cols[i].notNull) json += ",\\"not_null\\":true";
                if (cols[i].autoIncrement) json += ",\\"auto_increment\\":true";
                if (!cols[i].defaultVal.empty()) json += ",\\"default\\":\\"" + je(cols[i].defaultVal) + "\\"";
                json += "}";
            }
            json += "],\\"foreign_keys\\":[";
            const auto& fks = tbl.getForeignKeys();
            for (size_t i = 0; i < fks.size(); ++i) {
                if (i > 0) json += ",";
                json += "{\\"from\\":\\"" + je(fks[i].fromCol) + "\\",\\"ref_table\\":\\"" + je(fks[i].refTable)
                       + "\\",\\"ref_col\\":\\"" + je(fks[i].refCol) + "\\",\\"on_delete\\":\\"" + je(fks[i].onDelete) + "\\"}";
            }
            json += "]";
            // RLS info
            bool rlsOn = rlsEnabled_.count(name) > 0;
            json += ",\\"rls_enabled\\":" + std::string(rlsOn ? "true" : "false");
            auto pit = rlsPolicies_.find(name);
            if (pit != rlsPolicies_.end() && !pit->second.empty()) {
                json += ",\\"policies\\":[";
                for (size_t i = 0; i < pit->second.size(); ++i) {
                    if (i > 0) json += ",";
                    const auto& p = pit->second[i];
                    json += "{\\"name\\":\\"" + je(p.name) + "\\",\\"command\\":\\"" + je(p.command)
                           + "\\",\\"role\\":\\"" + je(p.role) + "\\",\\"using\\":\\"" + je(p.usingExpr) + "\\"";
                    if (!p.withCheckExpr.empty())
                        json += ",\\"with_check\\":\\"" + je(p.withCheckExpr) + "\\"";
                    json += "}";
                }
                json += "]";
            }
            json += "}";
            first = false;
        }
        json += "]}";
        return json;
    }

'''

anchor = '    // Phase 170: JSON export of all RLS policies (for WebUI)'
if 'getSchemaJson' not in content:
    content = content.replace(anchor, schema_json_method + '    ' + anchor[4:])
    write(ENGINE, content)
    print("  OK: getSchemaJson() added to engine")
else:
    print("  SKIP: already exists")

# ══════════════════════════════════════════════════════════════
# 2. Add /api/schema route
# ══════════════════════════════════════════════════════════════
print("\n=== 2. HTTP route: /api/schema ===")
patch(HTTP,
    '    // Phase 170: RLS policies API',
    '''    // Phase 171: Schema Visualizer API
    if (req.path == "/api/schema") {
        std::lock_guard<std::mutex> lock(engineMutex_);
        return buildHttpResponse(200, engine_.getSchemaJson(), "application/json");
    }

    // Phase 170: RLS policies API''',
    "/api/schema route")

# ══════════════════════════════════════════════════════════════
# 3. Add Schema nav item to sidebar
# ══════════════════════════════════════════════════════════════
print("\n=== 3. Sidebar: Schema Visualizer nav item ===")
patch(HTTP,
    '''      <div class="nav-item" data-page="browser" onclick="showPage('browser',this)">
        <span class="icon">&#x1F5C3;</span> Table Browser
      </div>''',
    '''      <div class="nav-item" data-page="browser" onclick="showPage('browser',this)">
        <span class="icon">&#x1F5C3;</span> Table Browser
      </div>
      <div class="nav-item" data-page="schema" onclick="showPage('schema',this)">
        <span class="icon">&#x25C9;</span> Schema
      </div>''',
    "Schema nav item")

# ══════════════════════════════════════════════════════════════
# 4. Add Schema page HTML
# ══════════════════════════════════════════════════════════════
print("\n=== 4. Schema Visualizer page HTML ===")

SCHEMA_PAGE = '''
    <!-- SCHEMA VISUALIZER PAGE -->
    <div class="page" id="page-schema">
      <div id="schema-toolbar" style="display:flex;align-items:center;gap:8px;padding:8px 12px;border-bottom:1px solid #21262d;background:#0d1117">
        <input id="schema-search" type="text" placeholder="Filter tables..." style="background:#161b22;border:1px solid #30363d;border-radius:4px;color:#e6edf3;padding:4px 10px;font-size:0.8rem;width:180px;outline:none">
        <button class="btn btn-gray" onclick="schemaAutoLayout()" style="font-size:0.75rem;padding:4px 10px">&#x2B50; Auto Layout</button>
        <button class="btn btn-gray" onclick="schemaFitAll()" style="font-size:0.75rem;padding:4px 10px">&#x26F6; Fit</button>
        <button class="btn btn-gray" onclick="schemaReload()" style="font-size:0.75rem;padding:4px 10px">&#x21BB; Reload</button>
        <span id="schema-status" style="margin-left:auto;font-size:0.72rem;color:#8b949e"></span>
      </div>
      <div id="schema-canvas" style="flex:1;position:relative;overflow:hidden;background:#0a0c10;cursor:grab">
        <svg id="schema-svg" style="position:absolute;top:0;left:0;width:100%;height:100%;pointer-events:none;z-index:1"></svg>
        <div id="schema-cards" style="position:absolute;top:0;left:0;z-index:2"></div>
      </div>
      <div id="schema-policy-editor" style="display:none;position:absolute;bottom:0;left:0;right:0;background:#161b22;border-top:2px solid #ff6b1a;padding:12px 16px;z-index:100">
        <div style="display:flex;align-items:center;gap:8px;margin-bottom:8px">
          <span style="font-size:0.85rem;font-weight:600;color:#ff6b1a">&#x1F6E1; Edit RLS Policy</span>
          <span id="schema-pe-table" style="font-size:0.8rem;color:#8b949e"></span>
          <button onclick="closePolicyEditor()" style="margin-left:auto;background:none;border:none;color:#8b949e;cursor:pointer;font-size:1rem">&#x2715;</button>
        </div>
        <div style="display:flex;gap:12px;align-items:flex-start;flex-wrap:wrap">
          <div style="flex:1;min-width:200px">
            <label style="font-size:0.7rem;color:#6e7681;text-transform:uppercase;letter-spacing:.5px">Policy Name</label>
            <input id="schema-pe-name" style="width:100%;background:#0d1117;border:1px solid #30363d;border-radius:4px;color:#e6edf3;padding:4px 8px;font-size:0.8rem;margin-top:2px">
          </div>
          <div style="flex:1;min-width:80px;max-width:120px">
            <label style="font-size:0.7rem;color:#6e7681;text-transform:uppercase;letter-spacing:.5px">Command</label>
            <select id="schema-pe-cmd" style="width:100%;background:#0d1117;border:1px solid #30363d;border-radius:4px;color:#e6edf3;padding:4px 8px;font-size:0.8rem;margin-top:2px">
              <option>ALL</option><option>SELECT</option><option>INSERT</option><option>UPDATE</option><option>DELETE</option>
            </select>
          </div>
          <div style="flex:1;min-width:100px;max-width:120px">
            <label style="font-size:0.7rem;color:#6e7681;text-transform:uppercase;letter-spacing:.5px">Role</label>
            <input id="schema-pe-role" value="PUBLIC" style="width:100%;background:#0d1117;border:1px solid #30363d;border-radius:4px;color:#e6edf3;padding:4px 8px;font-size:0.8rem;margin-top:2px">
          </div>
          <div style="flex:2;min-width:200px">
            <label style="font-size:0.7rem;color:#6e7681;text-transform:uppercase;letter-spacing:.5px">USING Expression</label>
            <input id="schema-pe-using" placeholder="e.g. owner = CURRENT_USER_ID()" style="width:100%;background:#0d1117;border:1px solid #30363d;border-radius:4px;color:#e6edf3;padding:4px 8px;font-size:0.8rem;font-family:monospace;margin-top:2px">
          </div>
          <div style="flex:2;min-width:200px">
            <label style="font-size:0.7rem;color:#6e7681;text-transform:uppercase;letter-spacing:.5px">WITH CHECK</label>
            <input id="schema-pe-check" placeholder="optional" style="width:100%;background:#0d1117;border:1px solid #30363d;border-radius:4px;color:#e6edf3;padding:4px 8px;font-size:0.8rem;font-family:monospace;margin-top:2px">
          </div>
          <div style="display:flex;align-items:flex-end">
            <button class="btn btn-green" onclick="savePolicyFromEditor()" style="font-size:0.8rem;padding:5px 14px;margin-top:14px">Save Policy</button>
          </div>
        </div>
        <div id="schema-pe-msg" style="font-size:0.75rem;margin-top:6px;color:#8b949e"></div>
      </div>
    </div>
'''

patch(HTTP,
    '    <!-- MONITORING PAGE -->',
    SCHEMA_PAGE + '    <!-- MONITORING PAGE -->',
    "Schema page HTML")

# ══════════════════════════════════════════════════════════════
# 5. Add CSS for Schema Visualizer
# ══════════════════════════════════════════════════════════════
print("\n=== 5. Schema CSS ===")

SCHEMA_CSS = '''
/* SCHEMA VISUALIZER */
#page-schema{display:flex;flex-direction:column;position:relative}
.schema-card{position:absolute;background:#161b22;border:1px solid #30363d;border-radius:8px;min-width:180px;max-width:260px;cursor:move;transition:box-shadow .15s,opacity .15s;user-select:none;z-index:2}
.schema-card:hover{box-shadow:0 0 0 1px #ff6b1a}
.schema-card.dimmed{opacity:0.3}
.schema-card.highlighted{box-shadow:0 0 0 2px #ff6b1a}
.schema-card-header{display:flex;align-items:center;gap:6px;padding:8px 10px;border-bottom:1px solid #21262d;font-size:0.8rem;font-weight:600;color:#e6edf3}
.schema-card-header .rls-dot{width:7px;height:7px;border-radius:50%;flex-shrink:0}
.schema-card-header .rls-dot.on{background:#3fb950}
.schema-card-header .rls-dot.off{background:#484f58}
.schema-card-header .pol-count{margin-left:auto;font-size:9px;font-weight:600;background:#1c3a2a;color:#3fb950;padding:1px 5px;border-radius:8px}
.schema-card-cols{padding:4px 0;font-size:0.75rem;max-height:200px;overflow-y:auto}
.schema-col{display:flex;align-items:center;gap:4px;padding:2px 10px;color:#8b949e}
.schema-col .col-icon{width:12px;font-size:9px;text-align:center;flex-shrink:0}
.schema-col .col-icon.pk{color:#f0a500}
.schema-col .col-icon.fk{color:#3fb950}
.schema-col .col-name{flex:1;color:#cdd6f4;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.schema-col .col-name.fk-col{color:#3fb950}
.schema-col .col-type{color:#484f58;font-size:0.7rem;font-family:monospace;flex-shrink:0}
.schema-card-rls{padding:5px 10px;border-top:1px solid #21262d;background:#0d1117;border-radius:0 0 7px 7px;font-size:0.7rem;font-family:monospace;color:#6e7681;cursor:pointer;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;max-height:60px}
.schema-card-rls:hover{color:#8b949e}
'''

patch(HTTP,
    '/* TABLE BROWSER PAGE */',
    SCHEMA_CSS + '/* TABLE BROWSER PAGE */',
    "Schema CSS")

# ══════════════════════════════════════════════════════════════
# 6. Add showPage handler for schema
# ══════════════════════════════════════════════════════════════
print("\n=== 6. showPage: schema ===")
patch(HTTP,
    "  if (name === 'browser') loadBrowserTables();",
    "  if (name === 'browser') loadBrowserTables();\n  if (name === 'schema') loadSchemaViz();",
    "showPage schema handler")

# ══════════════════════════════════════════════════════════════
# 7. Add Schema Visualizer JavaScript
# ══════════════════════════════════════════════════════════════
print("\n=== 7. Schema Visualizer JavaScript ===")

SCHEMA_JS = '''

// ══════════════════════════════════════════════════════════════
// Phase 171: Schema Visualizer
// ══════════════════════════════════════════════════════════════
var _schemaData = null;
var _schemaPositions = {};
var _schemaDrag = null;
var _schemaCanvasDrag = null;
var _schemaOffset = {x:0,y:0};
var _schemaScale = 1;

function schemaStorageKey() { return 'milansql_schema_pos_' + (msUser||'anon'); }

function loadSchemaPositions() {
  try { _schemaPositions = JSON.parse(localStorage.getItem(schemaStorageKey())) || {}; } catch(e) { _schemaPositions = {}; }
}

function saveSchemaPositions() {
  try { localStorage.setItem(schemaStorageKey(), JSON.stringify(_schemaPositions)); } catch(e) {}
}

async function loadSchemaViz() {
  var status = document.getElementById('schema-status');
  if (status) status.textContent = 'Loading schema...';
  loadSchemaPositions();
  try {
    var r = await fetch('/api/schema', {credentials:'include'});
    _schemaData = await r.json();
  } catch(e) { if(status) status.textContent='Error loading schema'; return; }
  renderSchemaViz();
}

function schemaReload() { loadSchemaViz(); }

function renderSchemaViz() {
  var tables = (_schemaData && _schemaData.tables) || [];
  var cardsEl = document.getElementById('schema-cards');
  var svgEl = document.getElementById('schema-svg');
  if (!cardsEl || !svgEl) return;
  cardsEl.innerHTML = '';
  var filter = (document.getElementById('schema-search')||{}).value || '';
  filter = filter.toLowerCase();

  // Build FK map
  var fkMap = []; // {from:tableName, fromCol, to:refTable, toCol}
  var fkCols = {}; // tableName -> Set of fromCol names
  tables.forEach(function(t) {
    fkCols[t.name] = {};
    (t.foreign_keys||[]).forEach(function(fk) {
      fkMap.push({from:t.name, fromCol:fk.from, to:fk.ref_table, toCol:fk.ref_col});
      fkCols[t.name][fk.from] = fk.ref_table;
    });
  });

  // Also infer FK from *_id convention
  var tblNames = {};
  tables.forEach(function(t) {
    tblNames[t.name] = true;
    // Also check without prefix
    var bare = t.name.replace(/^u\\d+_/, '');
    tblNames[bare] = t.name;
  });
  tables.forEach(function(t) {
    (t.columns||[]).forEach(function(c) {
      if (c.name.endsWith('_id') && !fkCols[t.name][c.name]) {
        var refName = c.name.slice(0, -3);
        // Try plural/singular
        var candidates = [refName, refName + 's', refName + 'e', refName + 'en'];
        for (var ci = 0; ci < candidates.length; ci++) {
          var cand = candidates[ci];
          if (tblNames[cand] && cand !== t.name) {
            var realName = typeof tblNames[cand] === 'string' ? tblNames[cand] : cand;
            if (realName !== t.name) {
              fkMap.push({from:t.name, fromCol:c.name, to:realName, toCol:'id', inferred:true});
              fkCols[t.name][c.name] = realName;
            }
            break;
          }
        }
      }
    });
  });

  // Default positions if not saved
  var cols = Math.max(2, Math.ceil(Math.sqrt(tables.length)));
  tables.forEach(function(t, i) {
    if (!_schemaPositions[t.name]) {
      _schemaPositions[t.name] = {x: 40 + (i % cols) * 280, y: 40 + Math.floor(i / cols) * 260};
    }
  });

  // Render cards
  var filteredTables = tables.filter(function(t) { return !filter || t.name.toLowerCase().indexOf(filter) >= 0; });
  var visibleNames = {};
  filteredTables.forEach(function(t) { visibleNames[t.name] = true; });

  filteredTables.forEach(function(t) {
    var card = document.createElement('div');
    card.className = 'schema-card';
    card.dataset.table = t.name;
    var pos = _schemaPositions[t.name];
    card.style.left = pos.x + 'px';
    card.style.top = pos.y + 'px';

    var polCount = (t.policies||[]).length;
    var rlsOn = t.rls_enabled;

    // Header
    var hdr = '<div class="schema-card-header">';
    hdr += '<span class="rls-dot ' + (rlsOn ? 'on' : 'off') + '" title="RLS ' + (rlsOn ? 'active' : 'inactive') + '"></span>';
    hdr += '<span>' + escHtml(t.name) + '</span>';
    if (polCount > 0) hdr += '<span class="pol-count">' + polCount + '</span>';
    hdr += '</div>';

    // Columns
    var colsHtml = '<div class="schema-card-cols">';
    (t.columns||[]).forEach(function(c) {
      var isFk = !!fkCols[t.name][c.name];
      var icon = '';
      if (c.pk) icon = '<span class="col-icon pk" title="Primary Key">&#x1F511;</span>';
      else if (isFk) icon = '<span class="col-icon fk" title="FK \\u2192 ' + escHtml(fkCols[t.name][c.name]) + '">&#x2192;</span>';
      else icon = '<span class="col-icon"></span>';
      colsHtml += '<div class="schema-col">' + icon
        + '<span class="col-name' + (isFk ? ' fk-col' : '') + '">' + escHtml(c.name) + '</span>'
        + '<span class="col-type">' + escHtml(c.type) + '</span></div>';
    });
    colsHtml += '</div>';

    // RLS expression strip
    var rlsStrip = '';
    if (rlsOn && polCount > 0) {
      var firstPol = t.policies[0];
      var expr = firstPol['using'] || '';
      rlsStrip = '<div class="schema-card-rls" title="Click to edit policy" onclick="openPolicyEditor(\\'' + escAttr(t.name) + '\\')">'
        + escHtml(firstPol.name) + ': ' + escHtml(expr) + '</div>';
    } else if (rlsOn) {
      rlsStrip = '<div class="schema-card-rls" onclick="openPolicyEditor(\\'' + escAttr(t.name) + '\\')">+ Add policy</div>';
    }

    card.innerHTML = hdr + colsHtml + rlsStrip;

    // Hover: highlight connected
    card.addEventListener('mouseenter', function() {
      var connected = {};
      connected[t.name] = true;
      fkMap.forEach(function(fk) {
        if (fk.from === t.name) connected[fk.to] = true;
        if (fk.to === t.name) connected[fk.from] = true;
      });
      document.querySelectorAll('.schema-card').forEach(function(c) {
        if (connected[c.dataset.table]) c.classList.add('highlighted');
        else c.classList.add('dimmed');
      });
      // Highlight SVG lines
      document.querySelectorAll('.schema-line').forEach(function(l) {
        if (l.dataset.from === t.name || l.dataset.to === t.name) {
          l.style.opacity = '1'; l.style.strokeWidth = '2';
        } else {
          l.style.opacity = '0.15';
        }
      });
      document.querySelectorAll('.schema-line-label').forEach(function(l) {
        if (l.dataset.from === t.name || l.dataset.to === t.name) l.style.opacity = '1';
        else l.style.opacity = '0.15';
      });
    });
    card.addEventListener('mouseleave', function() {
      document.querySelectorAll('.schema-card').forEach(function(c) { c.classList.remove('highlighted','dimmed'); });
      document.querySelectorAll('.schema-line').forEach(function(l) { l.style.opacity = ''; l.style.strokeWidth = ''; });
      document.querySelectorAll('.schema-line-label').forEach(function(l) { l.style.opacity = ''; });
    });

    // Drag
    card.addEventListener('mousedown', function(e) {
      if (e.target.tagName === 'INPUT' || e.target.tagName === 'SELECT' || e.target.tagName === 'BUTTON' || e.target.closest('.schema-card-rls')) return;
      e.preventDefault();
      _schemaDrag = {el:card, name:t.name, sx:e.clientX, sy:e.clientY, ox:pos.x, oy:pos.y};
    });

    cardsEl.appendChild(card);
  });

  drawSchemaLines(fkMap, visibleNames);

  var status = document.getElementById('schema-status');
  if (status) status.textContent = filteredTables.length + ' tables, ' + fkMap.length + ' relations';
}

function drawSchemaLines(fkMap, visibleNames) {
  var svg = document.getElementById('schema-svg');
  if (!svg) return;
  svg.innerHTML = '';
  var ns = 'http://www.w3.org/2000/svg';

  fkMap.forEach(function(fk) {
    if (!visibleNames[fk.from] || !visibleNames[fk.to]) return;
    var fromPos = _schemaPositions[fk.from];
    var toPos = _schemaPositions[fk.to];
    if (!fromPos || !toPos) return;

    var fromCard = document.querySelector('.schema-card[data-table="' + fk.from + '"]');
    var toCard = document.querySelector('.schema-card[data-table="' + fk.to + '"]');
    if (!fromCard || !toCard) return;

    var fw = fromCard.offsetWidth || 200;
    var fh = fromCard.offsetHeight || 120;
    var tw = toCard.offsetWidth || 200;
    var th = toCard.offsetHeight || 120;

    // Connect from right side of from-card to left side of to-card (or center-to-center)
    var fx = fromPos.x + fw;
    var fy = fromPos.y + fh / 2;
    var tx = toPos.x;
    var ty = toPos.y + th / 2;

    // If to is to the left of from, reverse
    if (toPos.x + tw < fromPos.x) {
      fx = fromPos.x; tx = toPos.x + tw;
    } else if (Math.abs(fromPos.x - toPos.x) < fw) {
      // Vertical — use bottom/top
      if (toPos.y > fromPos.y) {
        fx = fromPos.x + fw/2; fy = fromPos.y + fh;
        tx = toPos.x + tw/2; ty = toPos.y;
      } else {
        fx = fromPos.x + fw/2; fy = fromPos.y;
        tx = toPos.x + tw/2; ty = toPos.y + th;
      }
    }

    // Check if both tables have RLS
    var fromTbl = (_schemaData.tables||[]).find(function(t){return t.name===fk.from;});
    var toTbl = (_schemaData.tables||[]).find(function(t){return t.name===fk.to;});
    var bothRls = fromTbl && toTbl && fromTbl.rls_enabled && toTbl.rls_enabled;
    var lineColor = bothRls ? '#ff6b1a' : '#30363d';

    var line = document.createElementNS(ns, 'line');
    line.setAttribute('x1', fx); line.setAttribute('y1', fy);
    line.setAttribute('x2', tx); line.setAttribute('y2', ty);
    line.setAttribute('stroke', lineColor);
    line.setAttribute('stroke-width', '1');
    line.setAttribute('stroke-dasharray', '4,3');
    line.classList.add('schema-line');
    line.dataset.from = fk.from;
    line.dataset.to = fk.to;
    line.style.pointerEvents = 'none';
    svg.appendChild(line);

    // Label
    var mx = (fx + tx) / 2, my = (fy + ty) / 2;
    var text = document.createElementNS(ns, 'text');
    text.setAttribute('x', mx); text.setAttribute('y', my - 4);
    text.setAttribute('fill', '#6e7681');
    text.setAttribute('font-size', '9');
    text.setAttribute('text-anchor', 'middle');
    text.setAttribute('font-family', '-apple-system,sans-serif');
    text.classList.add('schema-line-label');
    text.dataset.from = fk.from;
    text.dataset.to = fk.to;
    text.style.pointerEvents = 'none';
    text.textContent = fk.fromCol;
    svg.appendChild(text);
  });
}

// Drag handling
document.addEventListener('mousemove', function(e) {
  if (_schemaDrag) {
    var dx = e.clientX - _schemaDrag.sx;
    var dy = e.clientY - _schemaDrag.sy;
    var nx = _schemaDrag.ox + dx;
    var ny = _schemaDrag.oy + dy;
    _schemaDrag.el.style.left = nx + 'px';
    _schemaDrag.el.style.top = ny + 'px';
    _schemaPositions[_schemaDrag.name] = {x:nx, y:ny};
    // Rebuild FK map for redraw
    var fkMap = [];
    var visibleNames = {};
    (_schemaData.tables||[]).forEach(function(t) {
      visibleNames[t.name] = true;
      (t.foreign_keys||[]).forEach(function(fk) {
        fkMap.push({from:t.name, fromCol:fk.from, to:fk.ref_table, toCol:fk.ref_col});
      });
      // Inferred FKs
      var tblNames = {};
      (_schemaData.tables||[]).forEach(function(t2) { tblNames[t2.name] = true; });
      (t.columns||[]).forEach(function(c) {
        if (c.name.endsWith('_id')) {
          var refName = c.name.slice(0,-3);
          [refName, refName+'s', refName+'e', refName+'en'].forEach(function(cand) {
            if (tblNames[cand] && cand !== t.name) fkMap.push({from:t.name,fromCol:c.name,to:cand,toCol:'id',inferred:true});
          });
        }
      });
    });
    drawSchemaLines(fkMap, visibleNames);
  }
});
document.addEventListener('mouseup', function() {
  if (_schemaDrag) { saveSchemaPositions(); _schemaDrag = null; }
});

// Search filter
document.addEventListener('input', function(e) {
  if (e.target.id === 'schema-search') renderSchemaViz();
});

// Auto-layout: force-directed simple
function schemaAutoLayout() {
  var tables = (_schemaData && _schemaData.tables) || [];
  if (!tables.length) return;
  // Build adjacency
  var adj = {};
  tables.forEach(function(t) { adj[t.name] = []; });
  tables.forEach(function(t) {
    (t.foreign_keys||[]).forEach(function(fk) {
      if (adj[fk.ref_table]) {
        adj[t.name].push(fk.ref_table);
        adj[fk.ref_table].push(t.name);
      }
    });
  });

  // Sort by connection count (most connected first in center)
  var sorted = tables.slice().sort(function(a,b) { return (adj[b.name]||[]).length - (adj[a.name]||[]).length; });

  // Grid layout with connected tables nearby
  var placed = {};
  var cols = Math.max(2, Math.ceil(Math.sqrt(tables.length)));
  var gapX = 280, gapY = 260;
  var cx = 60, cy = 60;
  sorted.forEach(function(t, i) {
    _schemaPositions[t.name] = {x: cx + (i % cols) * gapX, y: cy + Math.floor(i / cols) * gapY};
  });

  saveSchemaPositions();
  renderSchemaViz();
}

function schemaFitAll() {
  var tables = (_schemaData && _schemaData.tables) || [];
  if (!tables.length) return;
  var minX=Infinity, minY=Infinity;
  tables.forEach(function(t) {
    var p = _schemaPositions[t.name];
    if (p) { if(p.x < minX) minX=p.x; if(p.y < minY) minY=p.y; }
  });
  // Shift all so minimum is at 40,40
  tables.forEach(function(t) {
    var p = _schemaPositions[t.name];
    if (p) { p.x -= minX - 40; p.y -= minY - 40; }
  });
  saveSchemaPositions();
  renderSchemaViz();
}

// Policy editor
function openPolicyEditor(tableName) {
  var pe = document.getElementById('schema-policy-editor');
  if (!pe) return;
  pe.style.display = 'block';
  document.getElementById('schema-pe-table').textContent = tableName;
  pe.dataset.table = tableName;
  // Pre-fill if policy exists
  var tbl = (_schemaData.tables||[]).find(function(t){return t.name===tableName;});
  if (tbl && tbl.policies && tbl.policies.length > 0) {
    var p = tbl.policies[0];
    document.getElementById('schema-pe-name').value = p.name;
    document.getElementById('schema-pe-cmd').value = p.command;
    document.getElementById('schema-pe-role').value = p.role;
    document.getElementById('schema-pe-using').value = p['using']||'';
    document.getElementById('schema-pe-check').value = p.with_check||'';
  } else {
    document.getElementById('schema-pe-name').value = tableName + '_policy';
    document.getElementById('schema-pe-cmd').value = 'ALL';
    document.getElementById('schema-pe-role').value = 'PUBLIC';
    document.getElementById('schema-pe-using').value = '';
    document.getElementById('schema-pe-check').value = '';
  }
  document.getElementById('schema-pe-msg').textContent = '';
}

function closePolicyEditor() {
  var pe = document.getElementById('schema-policy-editor');
  if (pe) pe.style.display = 'none';
}

async function savePolicyFromEditor() {
  var pe = document.getElementById('schema-policy-editor');
  var tbl = pe.dataset.table;
  var name = document.getElementById('schema-pe-name').value.trim();
  var cmd = document.getElementById('schema-pe-cmd').value;
  var role = document.getElementById('schema-pe-role').value.trim() || 'PUBLIC';
  var usingExpr = document.getElementById('schema-pe-using').value.trim();
  var checkExpr = document.getElementById('schema-pe-check').value.trim();
  var msg = document.getElementById('schema-pe-msg');

  if (!name || !usingExpr) { msg.style.color='#f85149'; msg.textContent='Name and USING expression required'; return; }

  // First ensure RLS is enabled
  var sql1 = 'ALTER TABLE ' + tbl + ' ENABLE ROW LEVEL SECURITY';
  await fetch('/api/query', {method:'POST',credentials:'include',headers:{'Content-Type':'application/json'},body:JSON.stringify({sql:sql1})});

  // Drop existing policy with same name (ignore errors)
  var sqlDrop = 'DROP POLICY ' + name + ' ON ' + tbl;
  await fetch('/api/query', {method:'POST',credentials:'include',headers:{'Content-Type':'application/json'},body:JSON.stringify({sql:sqlDrop})});

  // Create new policy
  var sql = 'CREATE POLICY ' + name + ' ON ' + tbl + ' FOR ' + cmd + ' TO ' + role + ' USING (' + usingExpr + ')';
  if (checkExpr) sql += ' WITH CHECK (' + checkExpr + ')';

  try {
    var r = await fetch('/api/query', {method:'POST',credentials:'include',headers:{'Content-Type':'application/json'},body:JSON.stringify({sql:sql})});
    var data = await r.json();
    if (data.error) { msg.style.color='#f85149'; msg.textContent='Error: ' + data.error; }
    else { msg.style.color='#3fb950'; msg.textContent='Policy saved!'; setTimeout(function(){ closePolicyEditor(); loadSchemaViz(); }, 800); }
  } catch(e) { msg.style.color='#f85149'; msg.textContent='Network error'; }
}

'''

content = read(HTTP)
# Insert before the closing of the script — find a good anchor
anchor_js = "// Patch all fetch calls to include auth token + cookie credentials"
if 'loadSchemaViz' not in content:
    content = content.replace(anchor_js, SCHEMA_JS + anchor_js)
    write(HTTP, content)
    print("  OK: Schema Visualizer JavaScript added")
else:
    print("  SKIP: already exists")

print("\n" + "=" * 50)
print("ALL SCHEMA VISUALIZER PATCHES APPLIED")
print("=" * 50)
