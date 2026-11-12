/* zim-frame.js - runs inside every page the full-mode archive reader shows. zim-sw.js
 * puts it at the top of <head>, ahead of the page's scripts, since archive.html's load hook
 * can be 30 s late on a big wiki page.
 *  - "_top"/"_parent" links (Zimit copies of framed sites) would replace the reader, and
 *    the browser blocks that, so they open in place
 *  - links to the live web are stopped, nothing is there offline
 *  - tells archive.html as soon as the page is readable (title, fit-to-phone pass) */
(function () {
  'use strict';
  var host = null;
  try { if (window.parent !== window && window.parent.nomadZimFrame) host = window.parent.nomadZimFrame; } catch (e) { }
  if (!host) return;   // opened on its own, not inside the reader

  function fixTarget(el) {
    var t = (el.getAttribute('target') || '').toLowerCase();
    if (t === '_top' || t === '_parent') el.setAttribute('target', '_self');
  }

  document.addEventListener('click', function (e) {
    var a = e.target && e.target.closest && e.target.closest('a[href]');
    if (!a) return;
    var href = a.getAttribute('href') || '';
    if (/^(https?:)?\/\//i.test(href) && a.origin !== location.origin) {
      e.preventDefault();
      host.external(a.href);
      return;
    }
    fixTarget(a);
  }, true);

  document.addEventListener('submit', function (e) { if (e.target && e.target.getAttribute) fixTarget(e.target); }, true);

  function ready() { try { host.ready(window); } catch (e) { } }
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', ready);
  else ready();
})();
