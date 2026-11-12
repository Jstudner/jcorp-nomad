/* nomad-docs.js - documents, slides and spreadsheets read in the browser, offline.
   NomadDocs.kind(name) says what a file is; NomadDocs.render(host, { name, buffer }) draws it
   into host and resolves with a small controller (slides, sheets, zoom) for docview.html.

   Written for Nomad (nothing vendored here): DOCX, ODT, PPTX, ODP, XLSX, ODS, CSV/TSV, Markdown,
   plain text and RTF. Zip formats are opened with unzipit, loaded only when needed.
   Document text always goes in as textContent, never as markup. */
(function () {
  'use strict';

  const MAX_BYTES = 40 * 1024 * 1024;
  const MAX_ROWS = 2000, MAX_COLS = 200;
  const KINDS = {
    docx: 'docx', docm: 'docx', dotx: 'docx', odt: 'odt', ott: 'odt',
    pptx: 'pptx', pptm: 'pptx', ppsx: 'pptx', potx: 'pptx', odp: 'odp', otp: 'odp',
    xlsx: 'xlsx', xlsm: 'xlsx', xltx: 'xlsx', ods: 'ods', ots: 'ods',
    csv: 'csv', tsv: 'csv', md: 'md', markdown: 'md', txt: 'txt', text: 'txt', log: 'txt', rtf: 'rtf',
    gdoc: 'google', gsheet: 'google', gslides: 'google', gdraw: 'google', gform: 'google',
    doc: 'legacy', xls: 'legacy', ppt: 'legacy', pps: 'legacy',
  };
  const ext = (name) => { const i = String(name).lastIndexOf('.'); return i > 0 ? name.slice(i + 1).toLowerCase() : ''; };

  const CSS = `
  .nd-paper{background:#fff;color:#1a1a1a;max-width:820px;margin:0 auto 16px;padding:40px 44px;box-shadow:0 2px 14px rgba(0,0,0,.18);border-radius:4px;
    font:calc(15px * var(--nd-zoom,1))/1.55 Calibri,Carlito,'Segoe UI',Roboto,Arial,sans-serif;overflow-wrap:anywhere;box-sizing:border-box}
  @media (max-width:600px){.nd-paper{padding:22px 18px}}
  .nd-paper p{margin:0 0 .6em;min-height:1em}
  .nd-paper h1,.nd-paper h2,.nd-paper h3,.nd-paper h4,.nd-paper h5,.nd-paper h6{margin:.9em 0 .4em;line-height:1.25;color:#1f3864}
  .nd-paper h1{font-size:1.75em}.nd-paper h2{font-size:1.4em}.nd-paper h3{font-size:1.2em}.nd-paper h4,.nd-paper h5,.nd-paper h6{font-size:1.05em}
  .nd-paper .nd-title{font-size:2.1em;color:#111;margin-top:0}.nd-paper .nd-subtitle{font-size:1.2em;color:#555;font-weight:400}
  .nd-paper a{color:#1155cc}
  .nd-paper img{max-width:100%;height:auto;vertical-align:middle}
  .nd-paper hr.nd-break{border:0;border-top:2px dashed #c8c8c8;margin:28px 0}
  .nd-li{position:relative;margin:0 0 .3em}.nd-li .nd-mark{position:absolute;left:-1.6em;width:1.4em;text-align:right;color:#444}
  .nd-paper ul,.nd-paper ol{margin:0 0 .6em;padding-left:1.6em}
  .nd-table{border-collapse:collapse;margin:.4em 0 1em;max-width:100%}
  .nd-table td,.nd-table th{border:1px solid #bbb;padding:4px 7px;vertical-align:top}
  .nd-table td p{margin:0}
  .nd-scroll{overflow-x:auto;max-width:100%}
  .nd-pre{white-space:pre-wrap;font:calc(13px * var(--nd-zoom,1))/1.5 ui-monospace,Menlo,Consolas,monospace}
  .nd-paper code{font-family:ui-monospace,Menlo,Consolas,monospace;font-size:.9em;background:#f0f0f0;padding:1px 4px;border-radius:3px}
  .nd-paper pre{background:#f4f4f4;padding:10px 12px;border-radius:6px;overflow-x:auto}
  .nd-paper pre code{background:none;padding:0}
  .nd-paper blockquote{margin:0 0 .6em;padding:.2em 0 .2em 1em;border-left:4px solid #ddd;color:#555}

  .nd-slides{display:flex;flex-direction:column;align-items:center;gap:18px;padding-bottom:20px}
  .nd-slidebox{position:relative;box-shadow:0 2px 14px rgba(0,0,0,.25);background:#fff;overflow:hidden;flex:0 0 auto}
  .nd-slide{position:absolute;left:0;top:0;transform-origin:0 0;overflow:hidden;background:#fff;color:#111;font-family:Calibri,Carlito,'Segoe UI',Arial,sans-serif}
  .nd-shape{position:absolute;box-sizing:border-box;display:flex;flex-direction:column;overflow:hidden;line-height:1.15;padding:4px 8px}
  .nd-shape p{margin:0}
  .nd-shape img,.nd-pic{position:absolute;object-fit:fill}
  .nd-slidenum{position:absolute;right:8px;bottom:6px;font:600 11px system-ui,sans-serif;color:#888;z-index:2}

  .nd-sheet{background:#fff;color:#111;border-radius:4px;box-shadow:0 2px 14px rgba(0,0,0,.15);overflow:auto;max-width:100%}
  .nd-grid{border-collapse:collapse;font:calc(13px * var(--nd-zoom,1))/1.35 Calibri,Carlito,'Segoe UI',Arial,sans-serif;table-layout:fixed}
  .nd-grid td,.nd-grid th{border:1px solid #d9d9d9;padding:2px 6px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;max-width:420px}
  .nd-grid th{background:#f3f3f3;color:#555;font-weight:600;text-align:center;position:sticky;top:0;z-index:1}
  .nd-grid th.nd-rh{position:sticky;left:0;z-index:2;min-width:34px}
  .nd-grid thead th.nd-rh{z-index:3}
  .nd-grid td.nd-num{text-align:right}
  .nd-grid td.nd-wrap{white-space:pre-wrap}
  .nd-note{font-size:.85rem;color:var(--muted,#666);padding:8px 2px}
  .nd-msg{max-width:520px;margin:30px auto;text-align:center;background:var(--card-bg,var(--card,#fff));color:var(--text,#111);border:1px solid var(--card-border,#ddd);border-radius:14px;padding:24px 20px}
  .nd-msg h2{margin:0 0 8px;font-size:1.1rem}.nd-msg p{margin:0 0 8px;color:var(--muted,#666);line-height:1.5}
  .nd-msg code{overflow-wrap:anywhere;font-size:.85em}
  `;


  function h(tag, props, ...kids) {
    const e = document.createElement(tag);
    if (props) for (const k in props) {
      const v = props[k];
      if (v == null || v === false) continue;
      if (k === 'class') e.className = v;
      else if (k === 'style') Object.assign(e.style, v);
      else if (k === 'text') e.textContent = v;
      else e.setAttribute(k, v);
    }
    for (const k of kids) if (k != null) e.append(k);
    return e;
  }
  function xml(text) {
    const d = new DOMParser().parseFromString(text, 'application/xml');
    if (d.getElementsByTagName('parsererror').length) throw new Error('This file is damaged (its XML could not be read).');
    return d;
  }
  // by local name, so namespace prefixes (w:, a:, text:) don't matter
  const kids = (el, name) => (el ? Array.from(el.children).filter((c) => !name || c.localName === name) : []);
  function kid(el, name) { if (el) for (const c of el.children) if (c.localName === name) return c; return null; }
  function find(el, name) { if (!el) return null; for (const e of el.getElementsByTagName('*')) if (e.localName === name) return e; return null; }
  const findAll = (el, name) => (el ? Array.from(el.getElementsByTagName('*')).filter((e) => e.localName === name) : []);
  function at(el, name) { if (!el || !el.attributes) return null; for (const a of el.attributes) if (a.localName === name) return a.value; return null; }
  // a relationship attribute (r:id, r:embed): sldId carries a plain id="256" beside r:id
  function rid(el, name) { if (!el || !el.attributes) return null; for (const a of el.attributes) if (a.localName === name && /relationships/.test(a.namespaceURI || '')) return a.value; return at(el, name); }
  const on = (v) => v != null && v !== '0' && v !== 'false' && v !== 'none';
  function color(hex) { return /^[0-9a-f]{6}$/i.test(hex || '') ? '#' + hex : null; }
  function unit(v) {
    // ODF lengths: 2.54cm, 1in, 10mm, 12pt, 100px -> CSS px
    const m = /^(-?[\d.]+)(cm|mm|in|pt|pc|px)?$/.exec(String(v || '').trim());
    if (!m) return 0;
    const n = parseFloat(m[1]);
    return n * ({ cm: 96 / 2.54, mm: 96 / 25.4, in: 96, pt: 96 / 72, pc: 16, px: 1 }[m[2] || 'px']);
  }
  const EMU = 9525;   // EMU per CSS px

  let scriptP = null;
  function needUnzip() {
    if (window.unzipit) return Promise.resolve();
    return scriptP || (scriptP = new Promise((res, rej) => {
      const s = document.createElement('script');
      s.src = '/assets/unzipit.min.js';
      s.onload = res; s.onerror = () => rej(new Error("The zip reader didn't load."));
      document.head.appendChild(s);
    }));
  }
  const urls = [];
  const MIME = { png: 'image/png', jpg: 'image/jpeg', jpeg: 'image/jpeg', gif: 'image/gif', bmp: 'image/bmp', svg: 'image/svg+xml', webp: 'image/webp', tif: 'image/tiff', tiff: 'image/tiff' };
  async function openZip(buffer) {
    await needUnzip();
    let entries;
    try { ({ entries } = await window.unzipit.unzip(buffer)); }
    catch (e) { throw new Error("This file isn't a valid document (it couldn't be unpacked)."); }
    const get = (n) => entries[n] || entries[Object.keys(entries).find((k) => k.toLowerCase() === n.toLowerCase())];
    return {
      has: (n) => !!get(n),
      text: async (n) => (get(n) ? get(n).text() : null),
      async url(n) {
        const e = get(n);
        const type = MIME[ext(n)];
        if (!e || !type) return null;   // EMF/WMF and the like: browsers can't draw them
        const u = URL.createObjectURL(await e.blob(type));
        urls.push(u);
        return u;
      },
    };
  }
  function resolve(base, target) {
    if (!target) return '';
    if (target.startsWith('/')) return target.slice(1);
    const parts = base.split('/');
    parts.pop();
    for (const seg of target.split('/')) { if (seg === '..') parts.pop(); else if (seg && seg !== '.') parts.push(seg); }
    return parts.join('/');
  }
  async function relsOf(zip, part) {
    const t = await zip.text(part.replace(/([^/]+)$/, '_rels/$1.rels'));
    const map = {};
    if (!t) return map;
    for (const r of findAll(xml(t), 'Relationship')) {
      const external = at(r, 'TargetMode') === 'External';
      map[at(r, 'Id')] = { target: external ? at(r, 'Target') : resolve(part, at(r, 'Target')), external, type: at(r, 'Type') || '' };
    }
    return map;
  }
  function safeHref(u) {
    u = String(u || '').trim();
    return /^(https?:|mailto:|#)/i.test(u) ? u : null;
  }
  function textDecode(buffer) {
    const b = new Uint8Array(buffer);
    if (b[0] === 0xFF && b[1] === 0xFE) return new TextDecoder('utf-16le').decode(b);
    if (b[0] === 0xFE && b[1] === 0xFF) return new TextDecoder('utf-16be').decode(b);
    return new TextDecoder('utf-8').decode(b);
  }
  // numbering in lists: 1, a, i, A, I
  function counter(n, fmt) {
    if (/letter/i.test(fmt)) { let s = ''; n = Math.max(1, n); while (n > 0) { s = String.fromCharCode(97 + (n - 1) % 26) + s; n = Math.floor((n - 1) / 26); } return /upper/i.test(fmt) ? s.toUpperCase() : s; }
    if (/roman/i.test(fmt)) {
      const R = [[1000, 'm'], [900, 'cm'], [500, 'd'], [400, 'cd'], [100, 'c'], [90, 'xc'], [50, 'l'], [40, 'xl'], [10, 'x'], [9, 'ix'], [5, 'v'], [4, 'iv'], [1, 'i']];
      let s = ''; for (const [v, r] of R) while (n >= v) { s += r; n -= v; }
      return /upper/i.test(fmt) ? s.toUpperCase() : s;
    }
    return String(n);
  }
  const BULLETS = ['•', '◦', '▪'];
  function css() { if (!document.getElementById('nd-style')) document.head.append(h('style', { id: 'nd-style', text: CSS })); }
  function message(host, title, lines) {
    css();
    host.replaceChildren(h('div', { class: 'nd-msg' }, h('h2', { text: title }), ...lines.map((l) => (typeof l === 'string' ? h('p', { text: l }) : l))));
    return { kind: 'message' };
  }

  // Word: DOCX

  async function docx(host, buffer) {
    const zip = await openZip(buffer);
    const main = 'word/document.xml';
    const doc = await zip.text(main);
    if (!doc) throw new Error("This doesn't look like a Word document.");
    const rels = await relsOf(zip, main);
    // styles: headings by name or outline level, list numbering, through basedOn
    const styles = {};
    const sx = await zip.text('word/styles.xml');
    if (sx) for (const s of findAll(xml(sx), 'style')) {
      const name = (at(kid(s, 'name'), 'val') || '').toLowerCase();
      const pPr = kid(s, 'pPr');
      const outline = at(find(pPr, 'outlineLvl'), 'val');
      const numPr = find(pPr, 'numPr');
      let heading = 0;
      const m = /^heading\s*(\d)/.exec(name);
      if (m) heading = +m[1]; else if (outline != null && +outline < 9) heading = +outline + 1;
      styles[at(s, 'styleId')] = { name, based: at(kid(s, 'basedOn'), 'val'), heading,
        title: name === 'title', subtitle: name === 'subtitle',
        numId: numPr ? at(kid(numPr, 'numId'), 'val') : null, ilvl: numPr ? +(at(kid(numPr, 'ilvl'), 'val') || 0) : 0 };
    }
    const style = (id) => {
      const out = { heading: 0 };
      for (let s = styles[id], i = 0; s && i < 10; s = styles[s.based], i++) {
        if (!out.heading && s.heading) out.heading = s.heading;
        if (s.title) out.title = true;
        if (s.subtitle) out.subtitle = true;
        if (out.numId == null && s.numId) { out.numId = s.numId; out.ilvl = s.ilvl; }
      }
      return out;
    };
    // numbering: numId -> abstract levels (format, text, start)
    const nums = {};
    const nx = await zip.text('word/numbering.xml');
    if (nx) {
      const nd = xml(nx), abs = {};
      for (const a of findAll(nd, 'abstractNum')) {
        const lv = {};
        for (const l of kids(a, 'lvl')) lv[+at(l, 'ilvl')] = { fmt: at(kid(l, 'numFmt'), 'val') || 'decimal', text: at(kid(l, 'lvlText'), 'val') || '', start: +(at(kid(l, 'start'), 'val') || 1) };
        abs[at(a, 'abstractNumId')] = lv;
      }
      for (const n of findAll(nd, 'num')) nums[at(n, 'numId')] = abs[at(kid(n, 'abstractNumId'), 'val')] || {};
    }
    const counts = {};
    function marker(numId, ilvl) {
      const lv = nums[numId] || {};
      const me = lv[ilvl] || { fmt: 'bullet' };
      if (me.fmt === 'bullet' || me.fmt === 'none') return me.fmt === 'none' ? '' : BULLETS[ilvl % 3];
      const c = counts[numId] || (counts[numId] = []);
      c[ilvl] = (c[ilvl] == null ? (me.start || 1) : c[ilvl] + 1);
      for (let k = ilvl + 1; k < 9; k++) c[k] = null;   // deeper levels restart
      const text = me.text || `%${ilvl + 1}.`;
      return text.replace(/%(\d)/g, (_, d) => { const k = +d - 1; return counter(c[k] == null ? (lv[k] && lv[k].start) || 1 : c[k], (lv[k] || {}).fmt || 'decimal'); });
    }

    async function runs(parent, el, link) {
      for (const n of kids(el)) {
        switch (n.localName) {
          case 'r': await run(parent, n, link); break;
          case 'hyperlink': {
            const rel = rels[rid(n, 'id')];
            const href = rel && rel.external ? safeHref(rel.target) : null;
            const a = href ? h('a', { href, target: '_blank', rel: 'noopener' }) : h('span');
            parent.append(a);
            await runs(a, n, true);
            break;
          }
          case 'ins': case 'smartTag': case 'fldSimple': case 'customXml': await runs(parent, n, link); break;
          case 'sdt': await runs(parent, kid(n, 'sdtContent'), link); break;
          default: break;   // deletions, bookmarks, proofing marks
        }
      }
    }
    async function run(parent, r, link) {
      const rPr = kid(r, 'rPr');
      const st = {};
      if (rPr) {
        if (kid(rPr, 'b') && on(at(kid(rPr, 'b'), 'val') ?? '1')) st.fontWeight = '700';
        if (kid(rPr, 'i') && on(at(kid(rPr, 'i'), 'val') ?? '1')) st.fontStyle = 'italic';
        const u = kid(rPr, 'u'); if (u && on(at(u, 'val') ?? 'single')) st.textDecoration = 'underline';
        if (kid(rPr, 'strike') && on(at(kid(rPr, 'strike'), 'val') ?? '1')) st.textDecoration = (st.textDecoration ? st.textDecoration + ' ' : '') + 'line-through';
        const va = at(kid(rPr, 'vertAlign'), 'val');
        if (va === 'superscript') { st.verticalAlign = 'super'; st.fontSize = '.75em'; } else if (va === 'subscript') { st.verticalAlign = 'sub'; st.fontSize = '.75em'; }
        const c = color(at(kid(rPr, 'color'), 'val')); if (c && !link) st.color = c;
      }
      const span = h('span', { style: st });
      for (const n of kids(r)) {
        switch (n.localName) {
          case 't': span.append(n.textContent); break;
          case 'tab': span.append(' '); break;
          case 'br': if (at(n, 'type') === 'page') parent.dataset.pageAfter = '1'; else span.append(h('br')); break;
          case 'noBreakHyphen': span.append('-'); break;
          case 'sym': { const ch = parseInt(at(n, 'char') || '', 16); if (ch) span.append(String.fromCharCode(ch >= 0xF000 ? ch - 0xF000 : ch)); break; }
          case 'drawing': case 'pict': case 'object': {
            const blip = find(n, 'blip'), vml = find(n, 'imagedata');
            const rel = rels[rid(blip, 'embed') || rid(blip, 'link') || rid(vml, 'id')];
            const src = rel && !rel.external ? await zip.url(rel.target) : null;
            if (src) {
              const ext0 = find(n, 'extent');
              const w = ext0 ? Math.round(+at(ext0, 'cx') / EMU) : null;
              span.append(h('img', { src, alt: '', style: w ? { width: w + 'px' } : null }));
            }
            break;
          }
          default: break;
        }
      }
      if (span.childNodes.length) parent.append(span);
    }
    async function para(p, out) {
      const pPr = kid(p, 'pPr');
      const st = style(at(kid(pPr, 'pStyle'), 'val'));
      if (pPr && kid(pPr, 'pageBreakBefore')) out.append(h('hr', { class: 'nd-break' }));
      const numPr = kid(pPr, 'numPr');
      const numId = numPr ? at(kid(numPr, 'numId'), 'val') : st.numId;
      const ilvl = numPr ? +(at(kid(numPr, 'ilvl'), 'val') || 0) : (st.ilvl || 0);
      let el;
      if (st.title) el = h('h1', { class: 'nd-title' });
      else if (st.subtitle) el = h('p', { class: 'nd-subtitle' });
      else if (st.heading) el = h('h' + Math.min(6, st.heading));
      else el = h('p');
      const jc = at(kid(pPr, 'jc'), 'val');
      if (jc) el.style.textAlign = { center: 'center', right: 'right', end: 'right', both: 'justify', distribute: 'justify' }[jc] || '';
      await runs(el, p);
      if (numId && numId !== '0' && nums[numId]) {
        const li = h('div', { class: 'nd-li', style: { marginLeft: (1.6 * (ilvl + 1)) + 'em' } }, h('span', { class: 'nd-mark', text: marker(numId, ilvl) }));
        el.style.margin = '0';
        li.append(el);
        out.append(li);
      } else out.append(el);
      if (el.dataset.pageAfter) out.append(h('hr', { class: 'nd-break' }));
      if (pPr && kid(pPr, 'sectPr')) out.append(h('hr', { class: 'nd-break' }));
    }
    async function table(tbl, out) {
      const t = h('table', { class: 'nd-table' });
      const open = {};   // grid column -> the cell a vertical merge started in
      for (const tr of kids(tbl, 'tr')) {
        const row = h('tr');
        let col = 0;
        for (const tc of kids(tr, 'tc')) {
          const tcPr = kid(tc, 'tcPr');
          const span = +(at(kid(tcPr, 'gridSpan'), 'val') || 1);
          const vm = kid(tcPr, 'vMerge');
          const vmv = vm ? (at(vm, 'val') || 'continue') : null;
          if (vmv === 'continue' && open[col]) { open[col].rowSpan++; col += span; continue; }
          const td = h('td');
          if (span > 1) td.colSpan = span;
          const fill = color(at(kid(tcPr, 'shd'), 'fill')); if (fill && fill !== '#FFFFFF') td.style.background = fill;
          await blocks(tc, td);
          if (vmv === 'restart') open[col] = td; else delete open[col];
          row.append(td);
          col += span;
        }
        t.append(row);
      }
      out.append(h('div', { class: 'nd-scroll' }, t));
    }
    async function blocks(el, out) {
      for (const n of kids(el)) {
        if (n.localName === 'p') await para(n, out);
        else if (n.localName === 'tbl') await table(n, out);
        else if (n.localName === 'sdt') await blocks(kid(n, 'sdtContent'), out);
      }
    }
    const paper = h('div', { class: 'nd-paper' });
    await blocks(find(xml(doc), 'body'), paper);
    host.replaceChildren(paper);
    return { kind: 'doc' };
  }

  // OpenDocument: shared style reading

  function odfStyles(...docs) {
    const map = {}, lists = {};
    for (const d of docs) {
      if (!d) continue;
      for (const s of findAll(d, 'style')) {
        if (s.namespaceURI && !/:style:/.test(s.namespaceURI)) continue;
        const tp = kid(s, 'text-properties'), pp = kid(s, 'paragraph-properties'), gp = kid(s, 'graphic-properties') || kid(s, 'drawing-page-properties');
        const cp = kid(s, 'table-column-properties');
        map[at(s, 'name')] = {
          parent: at(s, 'parent-style-name'),
          bold: tp ? /bold|[6-9]00/.test(at(tp, 'font-weight') || '') : undefined,
          italic: tp ? /italic|oblique/.test(at(tp, 'font-style') || '') : undefined,
          underline: tp && at(tp, 'text-underline-style') ? at(tp, 'text-underline-style') !== 'none' : undefined,
          strike: tp && at(tp, 'text-line-through-style') ? at(tp, 'text-line-through-style') !== 'none' : undefined,
          color: tp ? at(tp, 'color') : null,
          size: tp ? at(tp, 'font-size') : null,
          align: pp ? at(pp, 'text-align') : null,
          breakBefore: pp ? at(pp, 'break-before') === 'page' : false,
          fill: gp && at(gp, 'fill') === 'solid' ? at(gp, 'fill-color') : null,
          width: cp ? at(cp, 'column-width') : null,
        };
      }
      for (const l of findAll(d, 'list-style')) {
        const lv = {};
        for (const c of kids(l)) lv[+(at(c, 'level') || 1)] = c.localName === 'list-level-style-number';
        lists[at(l, 'name')] = lv;
      }
    }
    const get = (name, key) => { for (let s = map[name], i = 0; s && i < 10; s = map[s.parent], i++) if (s[key] != null && s[key] !== undefined) return s[key]; return null; };
    return { get, map, lists };
  }
  function odfInline(el, S, zip, parent) {
    const out = [];
    for (const n of el.childNodes) {
      if (n.nodeType === 3) { parent.append(n.nodeValue); continue; }
      if (n.nodeType !== 1) continue;
      switch (n.localName) {
        case 's': parent.append(' '.repeat(+(at(n, 'c') || 1))); break;
        case 'tab': parent.append(' '); break;
        case 'line-break': parent.append(h('br')); break;
        case 'span': {
          const name = at(n, 'style-name'), st = {};
          if (S.get(name, 'bold')) st.fontWeight = '700';
          if (S.get(name, 'italic')) st.fontStyle = 'italic';
          const deco = [S.get(name, 'underline') && 'underline', S.get(name, 'strike') && 'line-through'].filter(Boolean).join(' ');
          if (deco) st.textDecoration = deco;
          const c = S.get(name, 'color'); if (c && /^#[0-9a-f]{6}$/i.test(c)) st.color = c;
          const sz = S.get(name, 'size'); if (sz && /pt$/.test(sz)) st.fontSize = unit(sz) + 'px';
          const span = h('span', { style: st });
          parent.append(span);
          out.push(...odfInline(n, S, zip, span));
          break;
        }
        case 'a': {
          const href = safeHref(at(n, 'href'));
          const a = href ? h('a', { href, target: '_blank', rel: 'noopener' }) : h('span');
          parent.append(a);
          out.push(...odfInline(n, S, zip, a));
          break;
        }
        case 'frame': {
          const img = kid(n, 'image');
          if (img) { const im = h('img', { alt: '', style: { width: unit(at(n, 'width')) ? unit(at(n, 'width')) + 'px' : null } }); parent.append(im); out.push([im, at(img, 'href')]); }
          break;
        }
        case 'note': case 'annotation': case 'bookmark': case 'bookmark-start': case 'bookmark-end': case 'soft-page-break': break;
        default: out.push(...odfInline(n, S, zip, parent));
      }
    }
    return out;   // images still to load: [img, href]
  }
  async function loadImgs(list, zip) {
    for (const [img, href] of list) { const u = href ? await zip.url(href.replace(/^\.\//, '')) : null; if (u) img.src = u; else img.remove(); }
  }

  // OpenDocument text: ODT

  async function odt(host, buffer) {
    const zip = await openZip(buffer);
    const c = await zip.text('content.xml');
    if (!c) throw new Error("This doesn't look like an OpenDocument text file.");
    const cd = xml(c);
    const sd = await zip.text('styles.xml');
    const S = odfStyles(cd, sd ? xml(sd) : null);
    const imgs = [];
    function paraEl(n) {
      const name = at(n, 'style-name');
      let el;
      if (n.localName === 'h') {
        const lv = +(at(n, 'outline-level') || 1);
        el = h('h' + Math.min(6, lv));
      } else {
        let lv = 0, title = false;
        for (let st = name, i = 0; st && i < 10; st = S.map[st] && S.map[st].parent, i++) {
          const m = /^Heading(?:_20_)?(\d)$/.exec(st);
          if (m) { lv = +m[1]; break; }
          if (/^Title$/.test(st)) { title = true; break; }
        }
        el = title ? h('h1', { class: 'nd-title' }) : lv ? h('h' + Math.min(6, lv)) : h('p');
      }
      const al = S.get(name, 'align');
      if (al) el.style.textAlign = { start: 'left', end: 'right', center: 'center', justify: 'justify', left: 'left', right: 'right' }[al] || '';
      imgs.push(...odfInline(n, S, zip, el));
      return el;
    }
    function list(n, out, level, styleName) {
      const sn = at(n, 'style-name') || styleName;
      const ordered = sn && S.lists[sn] ? !!S.lists[sn][level] : false;
      const L = h(ordered ? 'ol' : 'ul');
      for (const item of kids(n)) {
        if (item.localName !== 'list-item' && item.localName !== 'list-header') continue;
        const li = h('li');
        for (const c2 of kids(item)) {
          if (c2.localName === 'list') list(c2, li, level + 1, sn);
          else if (c2.localName === 'p' || c2.localName === 'h') { const p = paraEl(c2); p.style.margin = '0'; li.append(p); }
        }
        L.append(li);
      }
      out.append(L);
    }
    function table(n, out) {
      const t = h('table', { class: 'nd-table' });
      const rows = [];
      (function collect(el) { for (const c2 of kids(el)) { if (c2.localName === 'table-row') rows.push(c2); else if (/table-(header-)?rows|table-row-group/.test(c2.localName)) collect(c2); } })(n);
      for (const r of rows) {
        const tr = h('tr');
        for (const cell of kids(r)) {
          if (cell.localName !== 'table-cell') continue;
          const td = h('td');
          const cs = +(at(cell, 'number-columns-spanned') || 1), rs = +(at(cell, 'number-rows-spanned') || 1);
          if (cs > 1) td.colSpan = cs;
          if (rs > 1) td.rowSpan = rs;
          blocks(cell, td);
          tr.append(td);
        }
        t.append(tr);
      }
      out.append(h('div', { class: 'nd-scroll' }, t));
    }
    function blocks(el, out) {
      for (const n of kids(el)) {
        if (n.localName === 'p' || n.localName === 'h') {
          if (S.get(at(n, 'style-name'), 'breakBefore') && out.childNodes.length) out.append(h('hr', { class: 'nd-break' }));
          out.append(paraEl(n));
        } else if (n.localName === 'list') list(n, out, 1, null);
        else if (n.localName === 'table') table(n, out);
        else if (n.localName === 'section' || n.localName === 'index-body' || /-index$|table-of-content/.test(n.localName)) blocks(n, out);
      }
    }
    const paper = h('div', { class: 'nd-paper' });
    blocks(find(cd, 'text'), paper);
    await loadImgs(imgs, zip);
    host.replaceChildren(paper);
    return { kind: 'doc' };
  }

  // slides: shared stack, counter and fit

  function slideStack(host, slides, W, H) {
    // slides: [{ el, bg }] built at W x H CSS px, shown as a scrolling stack fitted to the width
    const wrap = h('div', { class: 'nd-slides' });
    const boxes = slides.map((s, i) => {
      s.el.classList.add('nd-slide');
      s.el.style.width = W + 'px';
      s.el.style.height = H + 'px';
      if (s.bg) s.el.style.background = s.bg;
      const box = h('div', { class: 'nd-slidebox', 'data-i': String(i) }, s.el, h('span', { class: 'nd-slidenum', text: `${i + 1}` }));
      wrap.append(box);
      return box;
    });
    host.replaceChildren(wrap);
    let zoom = 1, current = 0;
    function fit() {
      const avail = Math.max(200, (host.clientWidth || window.innerWidth) - 8);
      const s = Math.min(avail / W, 1.6) * zoom;
      boxes.forEach((b) => { b.style.width = W * s + 'px'; b.style.height = H * s + 'px'; b.firstChild.style.transform = `scale(${s})`; });
    }
    fit();
    const ro = window.ResizeObserver ? new ResizeObserver(fit) : null;
    if (ro) ro.observe(host); else window.addEventListener('resize', fit);
    const ctl = {
      kind: 'slides', count: boxes.length,
      get index() { return current; },
      go(i) { i = Math.max(0, Math.min(boxes.length - 1, i)); boxes[i].scrollIntoView({ behavior: 'smooth', block: 'start' }); },
      zoom(f) { zoom = f; fit(); },
      onchange: null,
    };
    if (window.IntersectionObserver) {
      // the topmost slide that is mostly on screen (small slides on a phone show several at once)
      const seen = new Set();
      const io = new IntersectionObserver((es) => {
        for (const e of es) { if (e.isIntersecting && e.intersectionRatio >= 0.5) seen.add(+e.target.dataset.i); else seen.delete(+e.target.dataset.i); }
        if (!seen.size) return;
        const top = Math.min(...seen);
        if (top !== current) { current = top; if (ctl.onchange) ctl.onchange(current); }
      }, { threshold: [0.5] });
      boxes.forEach((b) => io.observe(b));
    }
    return ctl;
  }

  // PowerPoint: PPTX

  async function pptx(host, buffer) {
    const zip = await openZip(buffer);
    const pres = await zip.text('ppt/presentation.xml');
    if (!pres) throw new Error("This doesn't look like a PowerPoint file.");
    const pd = xml(pres);
    const sz = find(pd, 'sldSz');
    const CX = +(at(sz, 'cx') || 9144000), CY = +(at(sz, 'cy') || 6858000);
    const W = CX / EMU, H = CY / EMU;
    const prels = await relsOf(zip, 'ppt/presentation.xml');
    const order = findAll(find(pd, 'sldIdLst'), 'sldId').map((s) => prels[rid(s, 'id')]).filter(Boolean).map((r) => r.target);
    const partCache = {};
    async function part(name) {
      if (!(name in partCache)) {
        const t = await zip.text(name);
        partCache[name] = t ? { doc: xml(t), rels: await relsOf(zip, name) } : null;
      }
      return partCache[name];
    }
    // placeholders on a layout or master: [{ type, idx, xfrm, size }]
    function placeholders(p) {
      const out = [];
      if (!p) return out;
      for (const sp of findAll(p.doc, 'sp')) {
        const ph = find(kid(sp, 'nvSpPr'), 'ph');
        if (!ph) continue;
        const sz0 = find(find(sp, 'lstStyle'), 'defRPr');
        out.push({ type: at(ph, 'type') || 'body', idx: at(ph, 'idx'), xfrm: find(kid(sp, 'spPr'), 'xfrm'), size: sz0 ? +at(sz0, 'sz') : null });
      }
      return out;
    }
    function bgOf(p) {
      const bg = find(p && p.doc, 'bg');
      const c = color(at(find(find(bg, 'solidFill'), 'srgbClr'), 'val'));
      return c;
    }
    const SIZES = { title: 40, ctrTitle: 44, subTitle: 24, body: 20, obj: 20, dt: 12, ftr: 12, sldNum: 12 };

    const slides = [];
    for (const name of order) {
      const s = await part(name);
      if (!s) continue;
      const layoutRel = Object.values(s.rels).find((r) => /slideLayout$/.test(r.type));
      const layout = layoutRel ? await part(layoutRel.target) : null;
      const masterRel = layout ? Object.values(layout.rels).find((r) => /slideMaster$/.test(r.type)) : null;
      const master = masterRel ? await part(masterRel.target) : null;
      const phs = [placeholders(layout), placeholders(master)];
      const findPh = (type, idx) => {
        for (const list of phs) {
          // only a placeholder that says where it goes is any use
          const hit = (idx != null && list.find((x) => x.idx === idx && x.xfrm)) || list.find((x) => x.type === type && x.xfrm) ||
            (type === 'ctrTitle' && list.find((x) => x.type === 'title' && x.xfrm)) || (type === 'subTitle' && list.find((x) => x.type === 'body' && x.xfrm));
          if (hit) return hit;
        }
        return null;
      };
      const el = h('div');
      const imgs = [];
      // xfrm -> box in slide px, through any group transforms
      const box = (xf, tr) => {
        const off = kid(xf, 'off'), ex = kid(xf, 'ext');
        if (!off || !ex) return null;
        const x = +at(off, 'x'), y = +at(off, 'y'), cx = +at(ex, 'cx'), cy = +at(ex, 'cy');
        return { x: (tr.ox + (x - tr.cx0) * tr.sx) / EMU, y: (tr.oy + (y - tr.cy0) * tr.sy) / EMU, w: cx * tr.sx / EMU, h: cy * tr.sy / EMU };
      };
      const place = (node, b) => Object.assign(node.style, { left: b.x + 'px', top: b.y + 'px', width: b.w + 'px', height: b.h + 'px' });
      function textBody(txBody, type) {
        const frag = [];
        const counts = [];
        const anchor = at(find(txBody, 'bodyPr'), 'anchor');
        const bulletDefault = type === 'body' || type === 'obj';
        for (const p of kids(txBody, 'p')) {
          const pPr = kid(p, 'pPr');
          const lvl = +(at(pPr, 'lvl') || 0);
          const para = h('p');
          const algn = at(pPr, 'algn');
          if (algn) para.style.textAlign = { ctr: 'center', r: 'right', just: 'justify', l: 'left' }[algn] || '';
          let maxSz = 0;
          for (const r of kids(p)) {
            if (r.localName === 'br') { para.append(h('br')); continue; }
            if (r.localName !== 'r' && r.localName !== 'fld') continue;
            const rPr = kid(r, 'rPr');
            const st = {};
            const szv = +(at(rPr, 'sz') || 0);
            if (szv) { st.fontSize = (szv / 100) * 96 / 72 + 'px'; maxSz = Math.max(maxSz, szv); }
            if (on(at(rPr, 'b'))) st.fontWeight = '700';
            if (on(at(rPr, 'i'))) st.fontStyle = 'italic';
            if (at(rPr, 'u') && at(rPr, 'u') !== 'none') st.textDecoration = 'underline';
            const c = color(at(find(find(rPr, 'solidFill'), 'srgbClr'), 'val')); if (c) st.color = c;
            const t = kid(r, 't');
            para.append(h('span', { style: st, text: t ? t.textContent : '' }));
          }
          const hasText = para.textContent.trim().length > 0;
          if (hasText && pPr && kid(pPr, 'buNone') == null && (kid(pPr, 'buChar') || kid(pPr, 'buAutoNum') || bulletDefault)) {
            let mark = BULLETS[lvl % 3];
            // Symbol and Wingdings bullets are private-use characters with no glyph in a browser
            if (kid(pPr, 'buChar')) { const ch = at(kid(pPr, 'buChar'), 'char') || ''; const code = ch.codePointAt(0) || 0; if (ch && !(code >= 0xE000 && code <= 0xF8FF)) mark = ch; }
            if (kid(pPr, 'buAutoNum')) { counts[lvl] = (counts[lvl] || 0) + 1; mark = counter(counts[lvl], at(kid(pPr, 'buAutoNum'), 'type') || '') + '.'; }
            para.prepend(h('span', { text: mark + ' ', style: { marginLeft: '-1.1em', display: 'inline-block', width: '1.1em' } }));
            para.style.paddingLeft = (1.1 + lvl * 1.4) + 'em';
          } else if (hasText && bulletDefault && !pPr) {
            para.prepend(h('span', { text: BULLETS[0] + ' ', style: { marginLeft: '-1.1em', display: 'inline-block', width: '1.1em' } }));
            para.style.paddingLeft = '1.1em';
          } else if (lvl) para.style.paddingLeft = lvl * 1.4 + 'em';
          if (!hasText) para.append(' ');
          frag.push(para);
        }
        return { frag, anchor };
      }
      async function tree(node, tr) {
        for (const n of kids(node)) {
          if (n.localName === 'sp') {
            const nv = kid(n, 'nvSpPr'), ph = find(nv, 'ph');
            const type = ph ? (at(ph, 'type') || 'body') : null, idx = ph ? at(ph, 'idx') : null;
            let xf = find(kid(n, 'spPr'), 'xfrm');
            let defSize = null;
            if (ph) { const hit = findPh(type, idx); if (!xf && hit) xf = hit.xfrm; if (hit && hit.size) defSize = hit.size / 100; }
            const b = xf ? box(xf, tr) : null;
            if (!b) continue;
            const shape = h('div', { class: 'nd-shape' });
            place(shape, b);
            shape.style.fontSize = (defSize || SIZES[type] || 18) * 96 / 72 + 'px';
            if (type === 'title' || type === 'ctrTitle') shape.style.fontWeight = '600';
            // the shape's own fill, not its outline's (a:ln can hold a noFill or solidFill of its own)
            const fill = color(at(kid(kid(kid(n, 'spPr'), 'solidFill'), 'srgbClr'), 'val'));
            if (fill) shape.style.background = fill;
            const tx = kid(n, 'txBody');
            if (tx) {
              const { frag, anchor } = textBody(tx, type);
              shape.style.justifyContent = anchor === 'ctr' ? 'center' : anchor === 'b' ? 'flex-end' : 'flex-start';
              if (type === 'ctrTitle' || type === 'subTitle') shape.style.textAlign = 'center';
              shape.append(...frag);
            }
            el.append(shape);
          } else if (n.localName === 'pic') {
            const b = box(find(kid(n, 'spPr'), 'xfrm'), tr);
            const rel = s.rels[rid(find(n, 'blip'), 'embed')];
            if (!b || !rel || rel.external) continue;
            const img = h('img', { class: 'nd-pic', alt: '' });
            place(img, b);
            el.append(img);
            imgs.push([img, rel.target]);
          } else if (n.localName === 'grpSp') {
            const gx = find(kid(n, 'grpSpPr'), 'xfrm');
            const off = kid(gx, 'off'), ex = kid(gx, 'ext'), co = kid(gx, 'chOff'), ce = kid(gx, 'chExt');
            if (off && ex && co && ce) {
              const sx = (+at(ce, 'cx') ? +at(ex, 'cx') / +at(ce, 'cx') : 1) * tr.sx, sy = (+at(ce, 'cy') ? +at(ex, 'cy') / +at(ce, 'cy') : 1) * tr.sy;
              await tree(n, { ox: tr.ox + (+at(off, 'x') - tr.cx0) * tr.sx, oy: tr.oy + (+at(off, 'y') - tr.cy0) * tr.sy, cx0: +at(co, 'x'), cy0: +at(co, 'y'), sx, sy });
            } else await tree(n, tr);
          } else if (n.localName === 'graphicFrame') {
            const tbl = find(n, 'tbl');
            const b = box(kid(n, 'xfrm'), tr);
            if (!tbl || !b) continue;
            const t = h('table', { class: 'nd-table', style: { width: '100%', fontSize: '14px' } });
            for (const row of kids(tbl, 'tr')) {
              const trEl = h('tr');
              for (const tc of kids(row, 'tc')) {
                if (on(at(tc, 'hMerge')) || on(at(tc, 'vMerge'))) continue;
                const td = h('td');
                if (+at(tc, 'gridSpan') > 1) td.colSpan = +at(tc, 'gridSpan');
                if (+at(tc, 'rowSpan') > 1) td.rowSpan = +at(tc, 'rowSpan');
                const tx = kid(tc, 'txBody');
                if (tx) td.append(...textBody(tx, null).frag);
                trEl.append(td);
              }
              t.append(trEl);
            }
            const holder = h('div', { class: 'nd-shape', style: { padding: '0' } }, t);
            place(holder, b);
            holder.style.height = 'auto';
            el.append(holder);
          }
        }
      }
      await tree(find(find(s.doc, 'cSld'), 'spTree'), { ox: 0, oy: 0, cx0: 0, cy0: 0, sx: 1, sy: 1 });
      for (const [img, target] of imgs) { const u = await zip.url(target); if (u) img.src = u; else img.remove(); }
      slides.push({ el, bg: bgOf(s) || bgOf(layout) || bgOf(master) || '#fff' });
    }
    if (!slides.length) return message(host, 'No slides', ['This presentation has no slides to show.']);
    return slideStack(host, slides, W, H);
  }

  // OpenDocument slides: ODP

  async function odp(host, buffer) {
    const zip = await openZip(buffer);
    const c = await zip.text('content.xml');
    if (!c) throw new Error("This doesn't look like an OpenDocument presentation.");
    const cd = xml(c);
    const sdText = await zip.text('styles.xml');
    const sd = sdText ? xml(sdText) : null;
    const S = odfStyles(cd, sd);
    // the size of the slides: the page layout of the master page the first slide uses (the
    // file has others too, for notes and handouts, which are portrait)
    const firstPage = find(find(cd, 'presentation'), 'page');
    const masterName = at(firstPage, 'master-page-name');
    const master = findAll(sd, 'master-page').find((m) => at(m, 'name') === masterName) || find(sd, 'master-page');
    const layoutName = at(master, 'page-layout-name');
    const layout = findAll(sd, 'page-layout').find((l) => at(l, 'name') === layoutName) || find(sd, 'page-layout');
    const pl = kid(layout, 'page-layout-properties');
    const W = unit(at(pl, 'page-width')) || 1024, H = unit(at(pl, 'page-height')) || 768;
    const SIZES = { title: 40, subtitle: 24, outline: 22, notes: 14 };
    const slides = [];
    for (const page of findAll(find(cd, 'presentation'), 'page')) {
      const el = h('div');
      const imgs = [];
      for (const fr of kids(page)) {
        if (!/^(frame|custom-shape|rect|ellipse|polygon)$/.test(fr.localName)) continue;
        const x = unit(at(fr, 'x')), y = unit(at(fr, 'y')), w = unit(at(fr, 'width')), hh = unit(at(fr, 'height'));
        const cls = at(fr, 'class') || '';
        const img = kid(fr, 'image');
        if (img && !(find(img, 'p') && find(img, 'p').textContent.trim())) {
          const im = h('img', { class: 'nd-pic', alt: '', style: { left: x + 'px', top: y + 'px', width: w + 'px', height: hh + 'px' } });
          el.append(im);
          imgs.push([im, at(img, 'href')]);
          continue;
        }
        const tb = kid(fr, 'text-box') || fr;
        const shape = h('div', { class: 'nd-shape', style: { left: x + 'px', top: y + 'px', width: w + 'px', height: hh + 'px', fontSize: (SIZES[cls] || 18) * 96 / 72 + 'px' } });
        if (cls === 'title') { shape.style.fontWeight = '600'; shape.style.justifyContent = 'center'; }
        if (cls === 'subtitle') { shape.style.textAlign = 'center'; shape.style.justifyContent = 'center'; }
        const fill = S.get(at(fr, 'style-name'), 'fill'); if (fill && fr.localName !== 'frame') shape.style.background = fill;
        (function walk(node, level) {
          for (const n of kids(node)) {
            if (n.localName === 'p' || n.localName === 'h') {
              const p = h('p');
              imgs.push(...odfInline(n, S, zip, p));
              if (level > 0) { p.prepend(h('span', { text: BULLETS[(level - 1) % 3] + ' ' })); p.style.paddingLeft = (level - 1) * 1.4 + 'em'; }
              if (!p.textContent.trim()) p.append(' ');
              shape.append(p);
            } else if (n.localName === 'list') {
              for (const item of kids(n, 'list-item')) walk(item, level + 1);
            }
          }
        })(tb, 0);
        if (shape.childNodes.length) el.append(shape);
      }
      await loadImgs(imgs, zip);
      slides.push({ el, bg: S.get(at(page, 'style-name'), 'fill') || '#fff' });
    }
    if (!slides.length) return message(host, 'No slides', ['This presentation has no slides to show.']);
    return slideStack(host, slides, W, H);
  }

  // spreadsheets: shared grid

  const colName = (i) => { let s = ''; i++; while (i > 0) { s = String.fromCharCode(65 + (i - 1) % 26) + s; i = Math.floor((i - 1) / 26); } return s; };
  // sheet: { name, rows: Map<r, Map<c, {text, num, wrap}>>, merges: [{r,c,rs,cs}], widths: {c: px}, maxR, maxC, cut }
  function grid(sheet) {
    const t = h('table', { class: 'nd-grid' });
    const nR = Math.min(sheet.maxR + 1, MAX_ROWS), nC = Math.min(sheet.maxC + 1, MAX_COLS);
    const cg = h('colgroup', null, h('col', { style: { width: '40px' } }));
    for (let c = 0; c < nC; c++) cg.append(h('col', { style: { width: (sheet.widths[c] || 80) + 'px' } }));
    t.append(cg);
    const head = h('tr', null, h('th', { class: 'nd-rh' }));
    for (let c = 0; c < nC; c++) head.append(h('th', { text: colName(c) }));
    t.append(h('thead', null, head));
    const covered = new Set(), spans = {};
    for (const m of sheet.merges) {
      spans[m.r + ',' + m.c] = m;
      for (let r = m.r; r < m.r + m.rs; r++) for (let c = m.c; c < m.c + m.cs; c++) if (r !== m.r || c !== m.c) covered.add(r + ',' + c);
    }
    const body = h('tbody');
    for (let r = 0; r < nR; r++) {
      const tr = h('tr', null, h('th', { class: 'nd-rh', text: String(r + 1) }));
      const row = sheet.rows.get(r);
      for (let c = 0; c < nC; c++) {
        if (covered.has(r + ',' + c)) continue;
        const cell = row && row.get(c);
        const td = h('td', { class: cell ? [cell.num ? 'nd-num' : '', cell.wrap ? 'nd-wrap' : ''].join(' ').trim() || null : null, text: cell ? cell.text : '' });
        const m = spans[r + ',' + c];
        if (m) { if (m.cs > 1) td.colSpan = Math.min(m.cs, nC - c); if (m.rs > 1) td.rowSpan = Math.min(m.rs, nR - r); }
        if (cell && cell.bold) td.style.fontWeight = '700';
        tr.append(td);
      }
      body.append(tr);
    }
    t.append(body);
    const wrap = h('div');
    wrap.append(h('div', { class: 'nd-sheet' }, t));
    if (sheet.cut || sheet.maxR + 1 > MAX_ROWS || sheet.maxC + 1 > MAX_COLS)
      wrap.append(h('div', { class: 'nd-note', text: `Showing the first ${nR.toLocaleString()} rows and ${nC} columns. Download the file to see all of it.` }));
    return wrap;
  }
  function sheetBook(host, sheets) {
    if (!sheets.length) return message(host, 'Empty', ['This spreadsheet has no sheets to show.']);
    let index = 0;
    const show = (i) => { index = Math.max(0, Math.min(sheets.length - 1, i)); host.replaceChildren(grid(sheets[index])); };
    show(0);
    return { kind: 'sheet', names: sheets.map((s) => s.name), get index() { return index; }, show, zoom(f) { host.style.setProperty('--nd-zoom', f); } };
  }
  const newSheet = (name) => ({ name, rows: new Map(), merges: [], widths: {}, maxR: -1, maxC: -1, cut: false });
  function setCell(sh, r, c, cell) {
    if (r >= MAX_ROWS) { sh.cut = true; return; }
    if (c >= MAX_COLS) { sh.cut = true; return; }
    if (!sh.rows.has(r)) sh.rows.set(r, new Map());
    sh.rows.get(r).set(c, cell);
    if (r > sh.maxR) sh.maxR = r;
    if (c > sh.maxC) sh.maxC = c;
  }

  // Excel: XLSX

  const DATE_IDS = new Set([14, 15, 16, 17, 18, 19, 20, 21, 22, 27, 30, 36, 45, 46, 47, 50, 57]);
  function fmtNumber(v, code, id, date1904) {
    const plain = String(code || '').replace(/"[^"]*"|\[[^\]]*\]|\\./g, '');
    const isDate = DATE_IDS.has(id) || (/[dmyhs]/i.test(plain) && !/^[#0.,%\s]*$/.test(plain) && id >= 164);
    if (isDate) {
      const ms = date1904 ? Date.UTC(1904, 0, 1) + v * 864e5 : (v < 60 ? Date.UTC(1899, 11, 31) : Date.UTC(1899, 11, 30)) + v * 864e5;
      const d = new Date(Math.round(ms / 1000) * 1000);
      const hasDay = /[dy]/i.test(plain) || [14, 15, 16, 17, 22, 27, 30, 36, 50, 57].includes(id);
      const hasTime = /[hs]/i.test(plain) || [18, 19, 20, 21, 22, 45, 46, 47].includes(id);
      const day = d.toLocaleDateString(undefined, { year: 'numeric', month: 'short', day: 'numeric', timeZone: 'UTC' });
      const time = d.toLocaleTimeString(undefined, { hour: '2-digit', minute: '2-digit', timeZone: 'UTC' });
      return hasDay && hasTime ? `${day} ${time}` : hasTime && !hasDay ? time : day;
    }
    const pct = id === 9 || id === 10 || /%/.test(plain);
    const n = pct ? v * 100 : v;
    const dec = (/\.(0+)/.exec(plain) || [, ''])[1].length || (id === 2 || id === 4 || id === 10 ? 2 : 0);
    const thousands = /#,##0|0,0/.test(plain) || id === 3 || id === 4;
    let s;
    if (id === 0 && !code || (!/[0#]/.test(plain) && !pct)) s = Number.isInteger(n) ? String(n) : String(+n.toPrecision(10));
    else s = n.toLocaleString(undefined, { minimumFractionDigits: dec, maximumFractionDigits: dec, useGrouping: thousands });
    if (pct) s += '%';
    const cur = /[$€£¥]/.exec(code || '');
    if (cur) s = (s.startsWith('-') ? '-' + cur[0] + s.slice(1) : cur[0] + s);
    return s;
  }
  function refRC(ref) {
    const m = /^([A-Z]+)(\d+)$/.exec(ref || '');
    if (!m) return null;
    let c = 0; for (const ch of m[1]) c = c * 26 + ch.charCodeAt(0) - 64;
    return { r: +m[2] - 1, c: c - 1 };
  }
  async function xlsx(host, buffer) {
    const zip = await openZip(buffer);
    const wbPart = 'xl/workbook.xml';
    const wbText = await zip.text(wbPart);
    if (!wbText) throw new Error("This doesn't look like an Excel file.");
    const wb = xml(wbText);
    const date1904 = on(at(find(wb, 'workbookPr'), 'date1904'));
    const rels = await relsOf(zip, wbPart);
    const shared = [];
    const ss = await zip.text('xl/sharedStrings.xml');
    if (ss) for (const si of findAll(xml(ss), 'si')) shared.push(findAll(si, 't').filter((t) => t.parentNode.localName !== 'rPh').map((t) => t.textContent).join(''));
    const fmts = {}, xfs = [], bolds = [];
    const st = await zip.text('xl/styles.xml');
    if (st) {
      const sd = xml(st);
      for (const f of findAll(find(sd, 'numFmts'), 'numFmt')) fmts[at(f, 'numFmtId')] = at(f, 'formatCode');
      const fonts = findAll(find(sd, 'fonts'), 'font').map((f) => !!kid(f, 'b') && on(at(kid(f, 'b'), 'val') ?? '1'));
      for (const x of kids(find(sd, 'cellXfs'), 'xf')) { xfs.push(+(at(x, 'numFmtId') || 0)); bolds.push(!!fonts[+(at(x, 'fontId') || 0)]); }
    }
    const sheets = [];
    for (const s of findAll(find(wb, 'sheets'), 'sheet')) {
      if (at(s, 'state') === 'hidden' || at(s, 'state') === 'veryHidden') continue;
      const rel = rels[rid(s, 'id')];
      const text = rel ? await zip.text(rel.target) : null;
      if (!text) continue;
      const d = xml(text);
      const sh = newSheet(at(s, 'name') || 'Sheet');
      for (const col of findAll(find(d, 'cols'), 'col')) {
        const w = +(at(col, 'width') || 0), hidden = on(at(col, 'hidden'));
        for (let c = +at(col, 'min') - 1; c <= +at(col, 'max') - 1 && c < MAX_COLS; c++) sh.widths[c] = hidden ? 0 : Math.round(w * 7 + 5);
      }
      for (const row of findAll(find(d, 'sheetData'), 'row')) {
        let rIdx = +(at(row, 'r') || 0) - 1;
        if (rIdx >= MAX_ROWS) { sh.cut = true; break; }
        let cIdx = 0;
        for (const c of kids(row, 'c')) {
          const pos = refRC(at(c, 'r'));
          if (pos) { rIdx = pos.r; cIdx = pos.c; }
          const t = at(c, 't'), v = kid(c, 'v'), sIdx = +(at(c, 's') || 0);
          let text = '', num = false;
          if (t === 's') text = shared[+(v ? v.textContent : -1)] ?? '';
          else if (t === 'inlineStr') text = findAll(kid(c, 'is'), 't').map((x) => x.textContent).join('');
          else if (t === 'b') text = v && v.textContent === '1' ? 'TRUE' : 'FALSE';
          else if (t === 'str' || t === 'e') text = v ? v.textContent : '';
          else if (v && v.textContent !== '') {
            const id = xfs[sIdx] || 0;
            const n = +v.textContent;
            text = isFinite(n) ? fmtNumber(n, fmts[id], id, date1904) : v.textContent;
            num = true;
          }
          if (text !== '') setCell(sh, rIdx, cIdx, { text, num, wrap: text.includes('\n'), bold: bolds[sIdx] });
          cIdx++;
        }
      }
      for (const m of findAll(find(d, 'mergeCells'), 'mergeCell')) {
        const [a, b] = (at(m, 'ref') || '').split(':').map(refRC);
        if (a && b) { sh.merges.push({ r: a.r, c: a.c, rs: b.r - a.r + 1, cs: b.c - a.c + 1 }); sh.maxR = Math.max(sh.maxR, Math.min(b.r, MAX_ROWS - 1)); sh.maxC = Math.max(sh.maxC, Math.min(b.c, MAX_COLS - 1)); }
      }
      if (sh.maxR < 0) { sh.maxR = 0; sh.maxC = 0; }
      sheets.push(sh);
    }
    return sheetBook(host, sheets);
  }

  // OpenDocument sheets: ODS

  async function ods(host, buffer) {
    const zip = await openZip(buffer);
    const c = await zip.text('content.xml');
    if (!c) throw new Error("This doesn't look like an OpenDocument spreadsheet.");
    const cd = xml(c);
    const S = odfStyles(cd);
    const sheets = [];
    for (const tbl of findAll(find(cd, 'spreadsheet'), 'table')) {
      const sh = newSheet(at(tbl, 'name') || 'Sheet');
      let col = 0;
      for (const cdef of findAll(tbl, 'table-column')) {
        const rep = Math.min(+(at(cdef, 'number-columns-repeated') || 1), MAX_COLS);
        const w = S.get(at(cdef, 'style-name'), 'width');
        for (let i = 0; i < rep && col < MAX_COLS; i++, col++) if (w) sh.widths[col] = Math.round(unit(w));
      }
      let r = 0;
      const rows = [];
      (function collect(el) { for (const n of kids(el)) { if (n.localName === 'table-row') rows.push(n); else if (/table-(header-)?rows|table-row-group/.test(n.localName)) collect(n); } })(tbl);
      for (const row of rows) {
        const rep = +(at(row, 'number-rows-repeated') || 1);
        const cells = kids(row).filter((n) => n.localName === 'table-cell' || n.localName === 'covered-table-cell');
        const has = cells.some((n) => n.localName === 'table-cell' && (find(n, 'p') || at(n, 'value-type')));
        if (!has) { r += rep; if (r >= MAX_ROWS) break; continue; }   // empty rows: count them, store nothing
        for (let k = 0; k < Math.min(rep, MAX_ROWS); k++, r++) {
          if (r >= MAX_ROWS) { sh.cut = true; break; }
          let cIdx = 0;
          for (const cell of cells) {
            const crep = Math.min(+(at(cell, 'number-columns-repeated') || 1), MAX_COLS);
            if (cell.localName === 'table-cell') {
              const text = findAll(cell, 'p').filter((p) => p.parentNode === cell).map((p) => p.textContent).join('\n');
              const vt = at(cell, 'value-type');
              const cs = +(at(cell, 'number-columns-spanned') || 1), rs = +(at(cell, 'number-rows-spanned') || 1);
              for (let j = 0; j < crep && cIdx < MAX_COLS; j++, cIdx++) {
                if (text !== '') setCell(sh, r, cIdx, { text, num: /float|percentage|currency|date|time/.test(vt || ''), wrap: text.includes('\n') });
                if ((cs > 1 || rs > 1) && j === 0) sh.merges.push({ r, c: cIdx, rs, cs });
              }
            } else cIdx += crep;
            if (cIdx >= MAX_COLS) break;
          }
        }
      }
      if (sh.maxR < 0) { sh.maxR = 0; sh.maxC = 0; }
      sheets.push(sh);
    }
    return sheetBook(host, sheets);
  }

  // CSV and TSV

  function csv(host, buffer, name) {
    const text = textDecode(buffer).replace(/^﻿/, '');
    const first = text.slice(0, text.indexOf('\n') > 0 ? text.indexOf('\n') : 2000);
    const counts = { ',': 0, ';': 0, '\t': 0 };
    for (const ch of first) if (ch in counts) counts[ch]++;
    const delim = ext(name) === 'tsv' ? '\t' : Object.keys(counts).sort((a, b) => counts[b] - counts[a])[0] || ',';
    const sh = newSheet(name.replace(/\.[^.]+$/, ''));
    let r = 0, c = 0, cell = '', q = false;
    const push = () => { if (cell !== '') setCell(sh, r, c, { text: cell, num: /^-?[\d,.]+%?$/.test(cell.trim()), wrap: cell.includes('\n') }); cell = ''; };
    for (let i = 0; i < text.length && r < MAX_ROWS; i++) {
      const ch = text[i];
      if (q) {
        if (ch === '"') { if (text[i + 1] === '"') { cell += '"'; i++; } else q = false; }
        else cell += ch;
      } else if (ch === '"' && cell === '') q = true;
      else if (ch === delim) { push(); c++; }
      else if (ch === '\n' || ch === '\r') { if (ch === '\r' && text[i + 1] === '\n') i++; push(); r++; c = 0; }
      else cell += ch;
    }
    if (cell !== '') push();
    if (r >= MAX_ROWS) sh.cut = true;
    if (sh.maxR < 0) { sh.maxR = 0; sh.maxC = 0; }
    return sheetBook(host, [sh]);
  }

  // Markdown, text, RTF

  // inline Markdown into DOM nodes: `code`, **bold**, *em*, _em_, ~~strike~~, [text](url), <url>
  function mdInline(s, parent) {
    const re = /(`+)([\s\S]*?)\1|\*\*([\s\S]+?)\*\*|__([\s\S]+?)__|~~([\s\S]+?)~~|\*([^*\s][^*]*?)\*|\b_([^_\s][^_]*?)_\b|\[([^\]]+)\]\(([^)\s]+)(?:\s+"[^"]*")?\)|<(https?:\/\/[^>\s]+)>/g;
    let last = 0, m;
    while ((m = re.exec(s))) {
      if (m.index > last) parent.append(s.slice(last, m.index));
      if (m[1]) parent.append(h('code', { text: m[2] }));
      else if (m[3] || m[4]) { const b = h('strong'); mdInline(m[3] || m[4], b); parent.append(b); }
      else if (m[5]) { const d = h('del'); mdInline(m[5], d); parent.append(d); }
      else if (m[6] || m[7]) { const e = h('em'); mdInline(m[6] || m[7], e); parent.append(e); }
      else if (m[8]) { const href = safeHref(m[9]); const a = href ? h('a', { href, target: '_blank', rel: 'noopener' }) : h('span'); mdInline(m[8], a); parent.append(a); }
      else if (m[10]) parent.append(h('a', { href: m[10], target: '_blank', rel: 'noopener', text: m[10] }));
      last = re.lastIndex;
    }
    if (last < s.length) parent.append(s.slice(last));
  }
  function markdown(host, buffer) {
    const lines = textDecode(buffer).replace(/\r\n?/g, '\n').split('\n');
    const paper = h('div', { class: 'nd-paper' });
    let i = 0;
    const isTableSep = (l) => /^\s*\|?\s*:?-{2,}:?\s*(\|\s*:?-{2,}:?\s*)*\|?\s*$/.test(l);
    const cellsOf = (l) => l.trim().replace(/^\||\|$/g, '').split('|').map((x) => x.trim());
    while (i < lines.length) {
      const l = lines[i];
      if (!l.trim()) { i++; continue; }
      let m;
      if ((m = /^```\s*(\S*)/.exec(l))) {
        const code = [];
        for (i++; i < lines.length && !/^```/.test(lines[i]); i++) code.push(lines[i]);
        i++;
        paper.append(h('pre', null, h('code', { text: code.join('\n') })));
      } else if ((m = /^(#{1,6})\s+(.*?)\s*#*$/.exec(l))) {
        const e = h('h' + m[1].length); mdInline(m[2], e); paper.append(e); i++;
      } else if (/^(\*\s*){3,}$|^(-\s*){3,}$|^(_\s*){3,}$/.test(l.trim())) { paper.append(h('hr')); i++; }
      else if (/^\s*>/.test(l)) {
        const q = [];
        for (; i < lines.length && /^\s*>/.test(lines[i]); i++) q.push(lines[i].replace(/^\s*>\s?/, ''));
        const b = h('blockquote'); mdInline(q.join(' '), b); paper.append(b);
      } else if (l.includes('|') && i + 1 < lines.length && isTableSep(lines[i + 1])) {
        const t = h('table', { class: 'nd-table' });
        const head = h('tr'); for (const c of cellsOf(l)) { const th = h('th'); mdInline(c, th); head.append(th); }
        t.append(head);
        for (i += 2; i < lines.length && lines[i].includes('|') && lines[i].trim(); i++) {
          const tr = h('tr'); for (const c of cellsOf(lines[i])) { const td = h('td'); mdInline(c, td); tr.append(td); } t.append(tr);
        }
        paper.append(h('div', { class: 'nd-scroll' }, t));
      } else if (/^\s*([-*+]|\d+[.)])\s+/.test(l)) {
        // nested by indent: a stack of open lists
        const stack = [];
        for (; i < lines.length && (/^\s*([-*+]|\d+[.)])\s+/.test(lines[i]) || (/^\s{2,}\S/.test(lines[i]) && stack.length)); i++) {
          const mm = /^(\s*)([-*+]|\d+[.)])\s+(.*)$/.exec(lines[i]);
          if (!mm) { const lastLi = stack[stack.length - 1].list.lastChild; if (lastLi) { lastLi.append(' '); mdInline(lines[i].trim(), lastLi); } continue; }
          const ind = mm[1].replace(/\t/g, '    ').length, ordered = /\d/.test(mm[2]);
          while (stack.length && ind < stack[stack.length - 1].ind) stack.pop();
          // a numbered list straight after a bulleted one (or the other way round) is a new list
          if (stack.length && ind === stack[stack.length - 1].ind && (stack[stack.length - 1].list.tagName === 'OL') !== ordered) stack.pop();
          if (!stack.length || ind > stack[stack.length - 1].ind) {
            const list = h(ordered ? 'ol' : 'ul');
            if (stack.length) (stack[stack.length - 1].list.lastChild || stack[stack.length - 1].list).append(list); else paper.append(list);
            stack.push({ ind, list });
          }
          const li = h('li');
          const task = /^\[([ xX])\]\s+(.*)$/.exec(mm[3]);
          if (task) { li.append(task[1] === ' ' ? '☐ ' : '☑ '); mdInline(task[2], li); } else mdInline(mm[3], li);
          stack[stack.length - 1].list.append(li);
        }
      } else {
        const para = [];
        for (; i < lines.length && lines[i].trim() && !/^(#{1,6}\s|```|\s*>|\s*([-*+]|\d+[.)])\s+)/.test(lines[i]); i++) para.push(lines[i].trim());
        if (!para.length) { para.push(lines[i].trim()); i++; }
        const p = h('p'); mdInline(para.join(' '), p); paper.append(p);
      }
    }
    host.replaceChildren(paper);
    return { kind: 'doc' };
  }
  function plain(host, text) {
    host.replaceChildren(h('div', { class: 'nd-paper' }, h('div', { class: 'nd-pre', text })));
    return { kind: 'doc' };
  }
  function rtf(host, buffer) {
    // text only: groups that aren't text (fonts, colours, pictures) dropped, controls read for breaks and characters
    const src = new TextDecoder('latin1').decode(new Uint8Array(buffer));
    let out = '', depth = 0, skipAt = -1, uc = 1, skipChars = 0;
    const cp1252 = new TextDecoder('windows-1252');
    for (let i = 0; i < src.length; i++) {
      const ch = src[i];
      if (ch === '{') { depth++; if (src.startsWith('{\\*', i) && skipAt < 0) skipAt = depth; continue; }
      if (ch === '}') { if (depth === skipAt) skipAt = -1; depth--; continue; }
      if (ch === '\\') {
        const m = /^\\([a-z]+)(-?\d+)? ?|^\\'([0-9a-f]{2})|^\\(.)/i.exec(src.slice(i, i + 40));
        if (!m) continue;
        i += m[0].length - 1;
        if (skipAt >= 0) continue;
        if (m[3]) { if (skipChars) { skipChars--; continue; } out += cp1252.decode(new Uint8Array([parseInt(m[3], 16)])); continue; }
        if (m[4]) { if (m[4] === '\n' || m[4] === '\r') out += '\n'; else if ('\\{}'.includes(m[4])) out += m[4]; else if (m[4] === '~') out += ' '; continue; }
        const w = m[1], n = m[2];
        if (/^(fonttbl|colortbl|stylesheet|info|pict|header|footer|footnote|listtable|listoverridetable|themedata|datastore|generator)$/.test(w)) { skipAt = depth; continue; }
        if (w === 'par' || w === 'line' || w === 'sect' || w === 'page') out += '\n';
        else if (w === 'tab') out += '\t';
        else if (w === 'uc') uc = +n;
        else if (w === 'u') { let code = +n; if (code < 0) code += 65536; out += String.fromCharCode(code); skipChars = uc; }
        else if (w === 'emdash') out += '—'; else if (w === 'endash') out += '–';
        else if (w === 'bullet') out += '•'; else if (w === 'lquote') out += '‘'; else if (w === 'rquote') out += '’';
        else if (w === 'ldblquote') out += '“'; else if (w === 'rdblquote') out += '”';
        continue;
      }
      if (skipAt >= 0 || ch === '\r' || ch === '\n') continue;
      if (skipChars) { skipChars--; continue; }
      out += ch;
    }
    return plain(host, out.replace(/\n{3,}/g, '\n\n').trim());
  }

  // Google shortcuts and old binary formats

  function google(host, buffer, name) {
    let url = '';
    try { url = JSON.parse(textDecode(buffer)).url || ''; } catch (e) {}
    const kind = { gdoc: ['Google Docs', '.docx'], gsheet: ['Google Sheets', '.xlsx'], gslides: ['Google Slides', '.pptx'] }[ext(name)] || ['Google Drive', ''];
    return message(host, `This is a link to ${kind[0]} online`, [
      `The file itself stays online.`,
      `Open it in ${kind[0]}, choose File, then Download${kind[1] ? `, as ${kind[1]}` : ''}, and copy that file to the card.`,
      url ? h('p', null, h('code', { text: url })) : null,
    ].filter(Boolean));
  }
  function legacy(host, name) {
    const to = { doc: '.docx', xls: '.xlsx', ppt: '.pptx', pps: '.pptx' }[ext(name)];
    return message(host, "This older format can't be opened here", [
      `${ext(name).toUpperCase()} files need Office or LibreOffice.`,
      `Open it there and save it as ${to}, then copy the new file to the card.`,
    ]);
  }


  function kind(name) { return KINDS[ext(name)] || null; }
  async function render(host, { name, buffer }) {
    css();
    const k = kind(name);
    if (buffer.byteLength > MAX_BYTES) return message(host, 'Too big to open here', [`The limit is ${MAX_BYTES / 1048576} MB. Download it instead.`]);
    switch (k) {
      case 'docx': return docx(host, buffer);
      case 'odt': return odt(host, buffer);
      case 'pptx': return pptx(host, buffer);
      case 'odp': return odp(host, buffer);
      case 'xlsx': return xlsx(host, buffer);
      case 'ods': return ods(host, buffer);
      case 'csv': return csv(host, buffer, name);
      case 'md': return markdown(host, buffer);
      case 'txt': return plain(host, textDecode(buffer));
      case 'rtf': return rtf(host, buffer);
      case 'google': return google(host, buffer, name);
      case 'legacy': return legacy(host, name);
      default: return message(host, "This file can't be opened here", ['Download it to open it on your device.']);
    }
  }
  window.addEventListener('pagehide', () => { urls.forEach((u) => URL.revokeObjectURL(u)); urls.length = 0; });
  window.NomadDocs = { kind, render, MAX_BYTES, _message: message };
})();
