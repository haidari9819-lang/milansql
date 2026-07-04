#!/usr/bin/env python3
"""Insert Schema Visualizer JS functions into http_server.hpp."""

HTTP = "/opt/milansql/src/server/http_server.hpp"

with open(HTTP, 'r') as f:
    content = f.read()

if 'async function loadSchemaViz' in content:
    print("SKIP: loadSchemaViz already defined")
    exit()

SCHEMA_JS = """
// Phase 171: Schema Visualizer
var _schemaData = null;
var _schemaPositions = {};
var _schemaDrag = null;

function schemaStorageKey() { return 'milansql_schema_pos_' + (msUser||'anon'); }
function loadSchemaPositions() { try { _schemaPositions = JSON.parse(localStorage.getItem(schemaStorageKey())) || {}; } catch(e) { _schemaPositions = {}; } }
function saveSchemaPositions() { try { localStorage.setItem(schemaStorageKey(), JSON.stringify(_schemaPositions)); } catch(e) {} }

async function loadSchemaViz() {
  var status = document.getElementById('schema-status');
  if (status) status.textContent = 'Loading...';
  loadSchemaPositions();
  try { var r = await fetch('/api/schema', {credentials:'include'}); _schemaData = await r.json(); }
  catch(e) { if(status) status.textContent='Error'; return; }
  renderSchemaViz();
}
function schemaReload() { loadSchemaViz(); }

function buildFkMap() {
  var tables = (_schemaData && _schemaData.tables) || [];
  var fkMap = [], fkCols = {}, tblNames = {};
  tables.forEach(function(t) { fkCols[t.name] = {}; tblNames[t.name] = true; var bare = t.name.replace(/^u[0-9]+_/, ''); if(bare!==t.name) tblNames[bare]=t.name; });
  tables.forEach(function(t) {
    (t.foreign_keys||[]).forEach(function(fk) { fkMap.push({from:t.name,fromCol:fk.from,to:fk.ref_table,toCol:fk.ref_col}); fkCols[t.name][fk.from]=fk.ref_table; });
    (t.columns||[]).forEach(function(c) {
      if (c.name.endsWith('_id') && !fkCols[t.name][c.name]) {
        var ref = c.name.slice(0,-3);
        [ref,ref+'s',ref+'e',ref+'en'].forEach(function(cn) {
          if(tblNames[cn]&&cn!==t.name) { var real=typeof tblNames[cn]==='string'?tblNames[cn]:cn; if(real!==t.name){fkMap.push({from:t.name,fromCol:c.name,to:real,toCol:'id',inferred:true}); fkCols[t.name][c.name]=real;} }
        });
      }
    });
  });
  return {fkMap:fkMap,fkCols:fkCols};
}

function renderSchemaViz() {
  var tables = (_schemaData && _schemaData.tables) || [];
  var cardsEl = document.getElementById('schema-cards');
  var svgEl = document.getElementById('schema-svg');
  if (!cardsEl||!svgEl) return;
  cardsEl.innerHTML = '';
  var filter = (document.getElementById('schema-search')||{}).value||'';
  filter = filter.toLowerCase();
  var fi = buildFkMap(), fkMap=fi.fkMap, fkCols=fi.fkCols;
  var cols = Math.max(2, Math.ceil(Math.sqrt(tables.length)));
  tables.forEach(function(t,i) { if(!_schemaPositions[t.name]) _schemaPositions[t.name]={x:40+(i%cols)*280,y:40+Math.floor(i/cols)*260}; });
  var ft = tables.filter(function(t){return !filter||t.name.toLowerCase().indexOf(filter)>=0;});
  var vis = {}; ft.forEach(function(t){vis[t.name]=true;});
  ft.forEach(function(t) {
    var card = document.createElement('div');
    card.className = 'schema-card'; card.dataset.table = t.name;
    var pos = _schemaPositions[t.name];
    card.style.left = pos.x+'px'; card.style.top = pos.y+'px';
    var pc = (t.policies||[]).length, rlsOn = t.rls_enabled;
    var h = '<div class="schema-card-header"><span class="rls-dot '+(rlsOn?'on':'off')+'" title="RLS '+(rlsOn?'active':'inactive')+'"></span><span>'+escHtml(t.name)+'</span>';
    if(pc>0) h+='<span class="pol-count">'+pc+'</span>';
    h+='</div><div class="schema-card-cols">';
    (t.columns||[]).forEach(function(c) {
      var isFk=!!(fkCols[t.name]||{})[c.name];
      var icon='';
      if(c.pk) icon='<span class="col-icon pk" title="PK">&#x1F511;</span>';
      else if(isFk) icon='<span class="col-icon fk" title="FK">&#x2192;</span>';
      else icon='<span class="col-icon"></span>';
      h+='<div class="schema-col">'+icon+'<span class="col-name'+(isFk?' fk-col':'')+'">'+escHtml(c.name)+'</span><span class="col-type">'+escHtml(c.type)+'</span></div>';
    });
    h+='</div>';
    if(rlsOn&&pc>0) { var fp=t.policies[0]; h+='<div class="schema-card-rls" title="Click to edit" onclick="openPolicyEditor(\\''+t.name.replace(/'/g,"\\\\'")+'\\')">' +escHtml(fp.name)+': '+escHtml(fp['using']||'')+'</div>'; }
    else if(rlsOn) h+='<div class="schema-card-rls" onclick="openPolicyEditor(\\''+t.name.replace(/'/g,"\\\\'")+'\\')">' +'+ Add policy</div>';
    card.innerHTML = h;
    card.addEventListener('mouseenter', function() {
      var conn={}; conn[t.name]=true;
      fkMap.forEach(function(fk){if(fk.from===t.name)conn[fk.to]=true;if(fk.to===t.name)conn[fk.from]=true;});
      document.querySelectorAll('.schema-card').forEach(function(c){if(conn[c.dataset.table])c.classList.add('highlighted');else c.classList.add('dimmed');});
      document.querySelectorAll('.schema-line').forEach(function(l){if(l.dataset.from===t.name||l.dataset.to===t.name){l.style.opacity='1';l.style.strokeWidth='2';}else l.style.opacity='0.15';});
      document.querySelectorAll('.schema-line-label').forEach(function(l){if(l.dataset.from===t.name||l.dataset.to===t.name)l.style.opacity='1';else l.style.opacity='0.15';});
    });
    card.addEventListener('mouseleave', function() {
      document.querySelectorAll('.schema-card').forEach(function(c){c.classList.remove('highlighted','dimmed');});
      document.querySelectorAll('.schema-line').forEach(function(l){l.style.opacity='';l.style.strokeWidth='';});
      document.querySelectorAll('.schema-line-label').forEach(function(l){l.style.opacity='';});
    });
    card.addEventListener('mousedown', function(e) {
      if(e.target.tagName==='INPUT'||e.target.tagName==='SELECT'||e.target.tagName==='BUTTON'||e.target.closest('.schema-card-rls'))return;
      e.preventDefault(); _schemaDrag={el:card,name:t.name,sx:e.clientX,sy:e.clientY,ox:pos.x,oy:pos.y};
    });
    cardsEl.appendChild(card);
  });
  drawSchemaLines(fkMap, vis);
  var status=document.getElementById('schema-status');
  if(status)status.textContent=ft.length+' tables, '+fkMap.length+' relations';
}

function drawSchemaLines(fkMap,vis) {
  var svg=document.getElementById('schema-svg'); if(!svg)return; svg.innerHTML='';
  var ns='http://www.w3.org/2000/svg';
  fkMap.forEach(function(fk) {
    if(!vis[fk.from]||!vis[fk.to])return;
    var fp=_schemaPositions[fk.from],tp=_schemaPositions[fk.to]; if(!fp||!tp)return;
    var fc=document.querySelector('.schema-card[data-table="'+fk.from+'"]');
    var tc=document.querySelector('.schema-card[data-table="'+fk.to+'"]');
    if(!fc||!tc)return;
    var fw=fc.offsetWidth||200,fh=fc.offsetHeight||120,tw=tc.offsetWidth||200,th=tc.offsetHeight||120;
    var fx=fp.x+fw,fy=fp.y+fh/2,tx=tp.x,ty=tp.y+th/2;
    if(tp.x+tw<fp.x){fx=fp.x;tx=tp.x+tw;}
    else if(Math.abs(fp.x-tp.x)<fw){if(tp.y>fp.y){fx=fp.x+fw/2;fy=fp.y+fh;tx=tp.x+tw/2;ty=tp.y;}else{fx=fp.x+fw/2;fy=fp.y;tx=tp.x+tw/2;ty=tp.y+th;}}
    var ft2=(_schemaData.tables||[]).find(function(t){return t.name===fk.from;});
    var tt2=(_schemaData.tables||[]).find(function(t){return t.name===fk.to;});
    var bothRls=ft2&&tt2&&ft2.rls_enabled&&tt2.rls_enabled;
    var lc=bothRls?'#ff6b1a':'#30363d';
    var line=document.createElementNS(ns,'line');
    line.setAttribute('x1',fx);line.setAttribute('y1',fy);line.setAttribute('x2',tx);line.setAttribute('y2',ty);
    line.setAttribute('stroke',lc);line.setAttribute('stroke-width','1');line.setAttribute('stroke-dasharray','4,3');
    line.classList.add('schema-line');line.dataset.from=fk.from;line.dataset.to=fk.to;line.style.pointerEvents='none';
    svg.appendChild(line);
    var mx=(fx+tx)/2,my=(fy+ty)/2;
    var text=document.createElementNS(ns,'text');
    text.setAttribute('x',mx);text.setAttribute('y',my-4);text.setAttribute('fill','#6e7681');
    text.setAttribute('font-size','9');text.setAttribute('text-anchor','middle');text.setAttribute('font-family','-apple-system,sans-serif');
    text.classList.add('schema-line-label');text.dataset.from=fk.from;text.dataset.to=fk.to;text.style.pointerEvents='none';
    text.textContent=fk.fromCol;svg.appendChild(text);
  });
}

document.addEventListener('mousemove',function(e){
  if(_schemaDrag){var d=_schemaDrag,nx=d.ox+(e.clientX-d.sx),ny=d.oy+(e.clientY-d.sy);
    d.el.style.left=nx+'px';d.el.style.top=ny+'px';_schemaPositions[d.name]={x:nx,y:ny};
    var fi=buildFkMap(),vis={};(_schemaData.tables||[]).forEach(function(t){vis[t.name]=true;});drawSchemaLines(fi.fkMap,vis);}
});
document.addEventListener('mouseup',function(){if(_schemaDrag){saveSchemaPositions();_schemaDrag=null;}});
document.addEventListener('input',function(e){if(e.target.id==='schema-search')renderSchemaViz();});

function schemaAutoLayout(){
  var tables=(_schemaData&&_schemaData.tables)||[];if(!tables.length)return;
  var adj={};tables.forEach(function(t){adj[t.name]=[];});
  tables.forEach(function(t){(t.foreign_keys||[]).forEach(function(fk){if(adj[fk.ref_table]){adj[t.name].push(fk.ref_table);adj[fk.ref_table].push(t.name);}});});
  var sorted=tables.slice().sort(function(a,b){return(adj[b.name]||[]).length-(adj[a.name]||[]).length;});
  var cols=Math.max(2,Math.ceil(Math.sqrt(tables.length)));
  sorted.forEach(function(t,i){_schemaPositions[t.name]={x:40+(i%cols)*280,y:40+Math.floor(i/cols)*260};});
  saveSchemaPositions();renderSchemaViz();
}
function schemaFitAll(){
  var tables=(_schemaData&&_schemaData.tables)||[];if(!tables.length)return;
  var minX=1e9,minY=1e9;tables.forEach(function(t){var p=_schemaPositions[t.name];if(p){if(p.x<minX)minX=p.x;if(p.y<minY)minY=p.y;}});
  tables.forEach(function(t){var p=_schemaPositions[t.name];if(p){p.x-=minX-40;p.y-=minY-40;}});
  saveSchemaPositions();renderSchemaViz();
}

function openPolicyEditor(tn){
  var pe=document.getElementById('schema-policy-editor');if(!pe)return;pe.style.display='block';
  document.getElementById('schema-pe-table').textContent=tn;pe.dataset.table=tn;
  var tbl=(_schemaData.tables||[]).find(function(t){return t.name===tn;});
  if(tbl&&tbl.policies&&tbl.policies.length>0){var p=tbl.policies[0];document.getElementById('schema-pe-name').value=p.name;document.getElementById('schema-pe-cmd').value=p.command;document.getElementById('schema-pe-role').value=p.role;document.getElementById('schema-pe-using').value=p['using']||'';document.getElementById('schema-pe-check').value=p.with_check||'';}
  else{document.getElementById('schema-pe-name').value=tn+'_policy';document.getElementById('schema-pe-cmd').value='ALL';document.getElementById('schema-pe-role').value='PUBLIC';document.getElementById('schema-pe-using').value='';document.getElementById('schema-pe-check').value='';}
  document.getElementById('schema-pe-msg').textContent='';
}
function closePolicyEditor(){var pe=document.getElementById('schema-policy-editor');if(pe)pe.style.display='none';}

async function savePolicyFromEditor(){
  var pe=document.getElementById('schema-policy-editor'),tbl=pe.dataset.table;
  var name=document.getElementById('schema-pe-name').value.trim(),cmd=document.getElementById('schema-pe-cmd').value;
  var role=document.getElementById('schema-pe-role').value.trim()||'PUBLIC';
  var ue=document.getElementById('schema-pe-using').value.trim(),ce=document.getElementById('schema-pe-check').value.trim();
  var msg=document.getElementById('schema-pe-msg');
  if(!name||!ue){msg.style.color='#f85149';msg.textContent='Name and USING required';return;}
  await fetch('/api/query',{method:'POST',credentials:'include',headers:{'Content-Type':'application/json'},body:JSON.stringify({sql:'ALTER TABLE '+tbl+' ENABLE ROW LEVEL SECURITY'})});
  await fetch('/api/query',{method:'POST',credentials:'include',headers:{'Content-Type':'application/json'},body:JSON.stringify({sql:'DROP POLICY '+name+' ON '+tbl})});
  var sql='CREATE POLICY '+name+' ON '+tbl+' FOR '+cmd+' TO '+role+' USING ('+ue+')';
  if(ce)sql+=' WITH CHECK ('+ce+')';
  try{var r=await fetch('/api/query',{method:'POST',credentials:'include',headers:{'Content-Type':'application/json'},body:JSON.stringify({sql:sql})});var data=await r.json();
    if(data.error){msg.style.color='#f85149';msg.textContent='Error: '+data.error;}
    else{msg.style.color='#3fb950';msg.textContent='Policy saved!';setTimeout(function(){closePolicyEditor();loadSchemaViz();},800);}
  }catch(e){msg.style.color='#f85149';msg.textContent='Network error';}
}
"""

# Insert before the fetch patching line
anchor = "// Patch all fetch calls to include auth token + cookie credentials"
if anchor in content:
    content = content.replace(anchor, SCHEMA_JS + "\n" + anchor, 1)
    with open(HTTP, 'w') as f:
        f.write(content)
    print("OK: Schema Visualizer JS inserted")
else:
    print("FAIL: anchor not found")
