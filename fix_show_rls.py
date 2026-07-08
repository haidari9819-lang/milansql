#!/usr/bin/env python3
"""Insert showRlsPolicies() function after explainQuery()."""

HTTP = "/opt/milansql/src/server/http_server.hpp"

with open(HTTP, 'r') as f:
    content = f.read()

FUNC = '''
// Phase 170: Show RLS Policies for table in editor
async function showRlsPolicies() {
  var sql = document.getElementById('sql-editor').value.trim();
  var tblMatch = sql.match(/(?:FROM|JOIN|TABLE|ON|INTO|UPDATE|POLICIES)\\s+([a-zA-Z_][a-zA-Z0-9_]*)/i);
  if (!tblMatch) {
    try {
      var r = await fetch('/api/rls-policies', {credentials:'include'});
      var data = await r.json();
      var out = document.getElementById('output-area');
      var html = '<div style="padding:12px"><h3 style="color:#f0a500;margin-bottom:12px">&#x1F6E1; RLS Policies Overview</h3>';
      var enabled = data.enabled_tables || [];
      html += '<div style="color:#8b949e;margin-bottom:8px">Protected tables: <b style="color:#3fb950">' + enabled.length + '</b></div>';
      if (enabled.length === 0) {
        html += '<div style="color:#484f58">No tables have RLS enabled.</div>';
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
    } catch(e) {}
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
      html += '<div style="color:#8b949e;font-size:0.8rem">Create: <code style="color:#58a6ff">CREATE POLICY name ON ' + escHtml(tblName) + ' FOR ALL TO PUBLIC USING (expr);</code></div>';
    } else {
      html += '<table><thead><tr><th>Policy</th><th>Command</th><th>Role</th><th>USING</th><th>WITH CHECK</th></tr></thead><tbody>';
      for (var i = 0; i < pols.length; i++) {
        var p = pols[i];
        html += '<tr>';
        html += '<td style="color:#58a6ff;font-weight:600">' + escHtml(p.name) + '</td>';
        html += '<td><span style="background:#21262d;padding:2px 6px;border-radius:3px;font-size:0.75rem;color:#f0a500">' + escHtml(p.command) + '</span></td>';
        html += '<td>' + escHtml(p.role) + '</td>';
        html += '<td style="font-family:monospace;font-size:0.8rem;color:#cdd6f4">' + escHtml(p['using']||'') + '</td>';
        html += '<td style="font-family:monospace;font-size:0.8rem;color:#cdd6f4">' + escHtml(p.with_check||'-') + '</td>';
        html += '</tr>';
      }
      html += '</tbody></table>';
    }
    html += '</div>';
    if (out) out.innerHTML = html;
  } catch(e) {}
}

'''

# Find explainQuery function and insert after it
anchor = "async function explainQuery() {"
pos = content.find(anchor)
if pos < 0:
    print("FAIL: explainQuery not found")
else:
    # Find the closing of explainQuery — look for next "async function" or "function "
    # at the start of a line
    search_from = pos + len(anchor)
    # explainQuery is short — find the next function
    next_func = content.find("\nasync function ", search_from)
    if next_func < 0:
        next_func = content.find("\nfunction ", search_from)

    if next_func > 0 and 'async function showRlsPolicies' not in content:
        content = content[:next_func] + "\n" + FUNC + content[next_func:]
        print("OK: showRlsPolicies() inserted after explainQuery()")
    elif 'async function showRlsPolicies' in content:
        print("SKIP: already exists")
    else:
        print("FAIL: couldn't find insertion point")

with open(HTTP, 'w') as f:
    f.write(content)
