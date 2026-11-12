/* nomad-readaloud.js - e-book read-aloud with the phone's own voices.
   speechSynthesis works on plain http (unlike the microphone) and is offline. quality and
   languages vary by phone.

     NomadReadAloud.attach(button, { rendition: () => rendition, lang: () => 'en' });

   Long paragraphs are spoken a sentence or two at a time because Chrome's speech stops by
   itself part way through a long one. Turning a page by hand stops reading. */
(function () {
  'use strict';
  const SET = 'nomad_readaloud';
  const synth = window.speechSynthesis;
  const esc = (s) => String(s == null ? '' : s).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
  const icon = (n, s) => (window.NomadIcons ? NomadIcons.svg(n, s || 18) : '');
  const BLOCKS = 'p,li,h1,h2,h3,h4,h5,h6,blockquote,pre,dt,dd,td,figcaption';

  function settings() { try { return JSON.parse(localStorage.getItem(SET) || '{}'); } catch (e) { return {}; } }
  function keep(s) { try { localStorage.setItem(SET, JSON.stringify(s)); } catch (e) {} }

  // a paragraph in pieces of a sentence or two, none longer than ~220 characters
  function pieces(text) {
    const out = [];
    const sentences = text.replace(/\s+/g, ' ').trim().match(/[^.!?…]+[.!?…]+["'”’)\]]*\s*|[^.!?…]+$/g) || [];
    let cur = '';
    for (const s of sentences) {
      if ((cur + s).length > 220 && cur) { out.push(cur.trim()); cur = ''; }
      if (s.length > 220) { for (let i = 0; i < s.length; i += 200) out.push(s.slice(i, i + 200)); continue; }
      cur += s;
    }
    if (cur.trim()) out.push(cur.trim());
    return out;
  }

  function attach(button, o) {
    if (!button) return null;
    if (!synth || !window.SpeechSynthesisUtterance) { button.hidden = true; return null; }
    let reading = false, paused = false, blocks = [], bi = 0, pi = 0, parts = [], doc = null, lit = null, myTurn = false, problem = '';

    const voices = () => synth.getVoices();
    // the book's language, or the phone's when the book doesn't say
    const want = () => ((o.lang && o.lang()) || navigator.language || 'en').slice(0, 2).toLowerCase();
    function pickVoice() {
      const s = settings(), all = voices(), w = want();
      const same = all.filter((v) => v.lang.slice(0, 2).toLowerCase() === w);
      return all.find((v) => v.voiceURI === s.voice) || same.find((v) => v.localService) || same[0] ||
             all.find((v) => v.default) || all[0] || null;
    }

    function light(el) {
      if (lit) lit.classList.remove('nomad-ra');
      lit = el;
      if (!el) return;
      if (!el.ownerDocument.getElementById('nomad-ra-style')) {
        const st = el.ownerDocument.createElement('style');
        st.id = 'nomad-ra-style';
        st.textContent = '.nomad-ra{background:rgba(255,200,0,.28)!important;border-radius:3px;box-shadow:0 0 0 3px rgba(255,200,0,.28)}';
        el.ownerDocument.head.appendChild(st);
      }
      el.classList.add('nomad-ra');
    }

    // the paragraphs of the section on screen, and the one the page starts at
    function load(fromTop) {
      const r = o.rendition();
      const contents = r && r.getContents && r.getContents()[0];
      if (!contents) return false;
      doc = contents.document;
      const found = [...doc.body.querySelectorAll(BLOCKS)].filter((el) => el.textContent.trim() && !el.querySelector(BLOCKS));
      blocks = found.length ? found : [doc.body];
      bi = 0;
      if (!fromTop) {
        try {
          const loc = r.currentLocation(), range = loc && loc.start && contents.range(loc.start.cfi);
          const node = range && range.startContainer;
          const el = node && (node.nodeType === 1 ? node : node.parentElement);
          if (el) {
            const i = blocks.findIndex((b) => b === el || b.contains(el) || (el.compareDocumentPosition(b) & Node.DOCUMENT_POSITION_FOLLOWING));
            if (i >= 0) bi = i;
          }
        } catch (e) {}
      }
      return true;
    }

    // keep the paragraph being read on the page
    function follow(el) {
      const r = o.rendition();
      try {
        const contents = r.getContents()[0];
        const cfi = contents.cfiFromNode(el);
        const end = r.currentLocation() && r.currentLocation().end && r.currentLocation().end.cfi;
        const C = window.ePub && (ePub.CFI || (ePub.default && ePub.default.CFI));
        if (end && C && new C().compare(cfi, end) > 0) { myTurn = true; r.display(cfi); }
        else if (r.settings && r.settings.flow === 'scrolled-doc') el.scrollIntoView({ block: 'center', behavior: 'smooth' });
      } catch (e) {}
    }

    function speak() {
      if (!reading || paused) return;
      if (!parts.length) {                         // the next paragraph
        if (bi >= blocks.length) return nextSection();
        const el = blocks[bi];
        parts = pieces(el.textContent); pi = 0;
        if (!parts.length) { bi++; return setTimeout(speak, 0); }
        light(el); follow(el);
      }
      if (pi >= parts.length) { bi++; parts = []; return setTimeout(speak, 0); }
      const u = new SpeechSynthesisUtterance(parts[pi]);
      const v = pickVoice(), s = settings();
      if (v) { u.voice = v; u.lang = v.lang; }
      u.rate = s.rate || 1;
      // the next piece on a fresh call stack: a voice that fails at once would otherwise
      // recurse through the whole book
      u.onend = () => { if (reading && !paused) { pi++; setTimeout(speak, 0); } };
      u.onerror = (e) => {
        if (e.error === 'interrupted' || e.error === 'canceled') return;
        // no voice, or speech not allowed: say so rather than race through the book
        problem = e.error === 'not-allowed' ? 'The phone blocked speech. Tap Read from here again.'
                : "No voice on this phone can read this. Pick another voice or add one in the phone's settings.";
        stop();
      };
      synth.speak(u);
    }

    async function nextSection() {
      const r = o.rendition();
      const before = r.currentLocation() && r.currentLocation().start && r.currentLocation().start.index;
      myTurn = true;
      await r.next();
      // wait for the next section's page to be up
      for (let i = 0; i < 40; i++) {
        const now = r.currentLocation() && r.currentLocation().start && r.currentLocation().start.index;
        if (now !== before) break;
        await new Promise((res) => setTimeout(res, 100));
      }
      if (r.currentLocation() && r.currentLocation().atEnd && bi >= blocks.length) return stop();
      if (!load(true)) return stop();
      parts = []; pi = 0;
      speak();
    }

    function play() {
      if (paused) { paused = false; speak(); draw(); return; }
      synth.cancel();
      if (!load(false)) return;
      reading = true; paused = false; parts = []; pi = 0; problem = '';
      speak(); draw();
    }
    function pause() { paused = true; synth.cancel(); draw(); }   // cancel, not pause(): Android's pause is unreliable
    function stop() { reading = false; paused = false; synth.cancel(); light(null); draw(); }

    // a page turned by hand (not by us) ends the reading. the page calls this for every
    // rendition it makes (switching between pages and scrolling makes a new one)
    function watch(r) {
      try { r.on('relocated', () => { if (myTurn) { myTurn = false; return; } if (reading && !paused) stop(); }); } catch (e) {}
    }

    let pop = null;
    function draw() {
      button.classList.toggle('on', reading && !paused);
      if (!pop) return;
      const s = settings();
      const list = voices();
      const w = want();
      const sorted = list.slice().sort((a, b) => (b.lang.slice(0, 2).toLowerCase() === w) - (a.lang.slice(0, 2).toLowerCase() === w) || a.name.localeCompare(b.name));
      const cur = pickVoice();
      pop.querySelector('.ra-voice').innerHTML = sorted.length
        ? sorted.map((v) => `<option value="${esc(v.voiceURI)}"${cur && v.voiceURI === cur.voiceURI ? ' selected' : ''}>${esc(v.name)} (${esc(v.lang)})</option>`).join('')
        : '<option>Default voice</option>';
      pop.querySelector('.ra-rate').value = s.rate || 1;
      pop.querySelector('.ra-rate-v').textContent = (s.rate || 1).toFixed(1) + '×';
      pop.querySelector('[data-a=play]').innerHTML = reading && !paused ? `${icon('volume-off')}Pause` : `${icon('play')}${paused ? 'Carry on' : 'Read from here'}`;
      pop.querySelector('[data-a=stop]').disabled = !reading;
      pop.querySelector('.ra-state').textContent = problem || (!reading ? '' : paused ? 'Paused' : 'Reading · turning a page stops it');
    }
    function openPop() {
      if (pop) { closePop(); return; }
      pop = document.createElement('div');
      pop.className = 'ra-pop';
      pop.innerHTML = `<div class="ra-card" role="dialog" aria-label="Read aloud">
        <div class="ra-head"><b>Read aloud</b><button class="ra-x" data-a="close" aria-label="Close">${icon('x')}</button></div>
        <label>Voice<select class="ra-voice"></select></label>
        <label>Speed <span class="ra-rate-v"></span><input class="ra-rate" type="range" min="0.6" max="2" step="0.1"></label>
        <div class="ra-row"><button class="ra-b primary" data-a="play"></button><button class="ra-b" data-a="stop">Stop</button></div>
        <div class="ra-state"></div></div>`;
      document.body.appendChild(pop);
      if (!document.getElementById('ra-style')) {
        const st = document.createElement('style');
        st.id = 'ra-style';
        st.textContent = `.ra-pop{position:fixed;right:12px;top:58px;z-index:9400;max-width:calc(100vw - 24px)}
          .ra-card{background:var(--card-bg,var(--card,#fff));color:var(--text,#222);border:1px solid var(--card-border,rgba(0,0,0,.15));border-radius:14px;
            padding:12px;width:300px;max-width:100%;box-sizing:border-box;box-shadow:0 10px 30px rgba(0,0,0,.25);display:grid;gap:10px;font-size:.9rem}
          .ra-head{display:flex;align-items:center;justify-content:space-between}
          .ra-card label{display:grid;gap:4px;font-weight:600;font-size:.82rem;color:var(--muted,#555)}
          .ra-card select{height:36px;border-radius:9px;border:1px solid var(--card-border,#ccc);background:var(--bg,#fff);color:var(--text,#222);font:inherit;max-width:100%}
          .ra-rate{accent-color:var(--primary,#007bff)}
          .ra-row{display:flex;gap:8px}
          .ra-b{flex:1;display:inline-flex;align-items:center;justify-content:center;gap:6px;min-height:38px;border-radius:10px;border:1px solid var(--card-border,#ccc);
            background:var(--card-bg,#fff);color:var(--text,#222);font:inherit;font-weight:700;cursor:pointer}
          .ra-b.primary{background:var(--primary,#007bff);border-color:var(--primary,#007bff);color:#fff}
          .ra-b:disabled{opacity:.5}
          .ra-b svg,.ra-x svg{width:16px;height:16px}
          .ra-x{border:0;background:none;color:inherit;cursor:pointer;padding:4px}
          .ra-state{font-size:.78rem;color:var(--muted,#666)}`;
        document.head.appendChild(st);
      }
      pop.addEventListener('click', (e) => {
        const b = e.target.closest('[data-a]'); if (!b) return;
        if (b.dataset.a === 'close') closePop();
        if (b.dataset.a === 'play') { reading && !paused ? pause() : play(); }
        if (b.dataset.a === 'stop') stop();
      });
      pop.querySelector('.ra-voice').addEventListener('change', (e) => { const s = settings(); s.voice = e.target.value; keep(s); if (reading && !paused) { synth.cancel(); speak(); } });
      pop.querySelector('.ra-rate').addEventListener('input', (e) => { const s = settings(); s.rate = +e.target.value; keep(s); pop.querySelector('.ra-rate-v').textContent = s.rate.toFixed(1) + '×'; });
      pop.querySelector('.ra-rate').addEventListener('change', () => { if (reading && !paused) { synth.cancel(); speak(); } });
      draw();
    }
    function closePop() { if (pop) pop.remove(); pop = null; }
    button.addEventListener('click', openPop);
    if (synth.addEventListener) synth.addEventListener('voiceschanged', draw);
    window.addEventListener('pagehide', () => synth.cancel());
    return { play, pause, stop, watch, get reading() { return reading; } };
  }

  window.NomadReadAloud = { attach };
})();
