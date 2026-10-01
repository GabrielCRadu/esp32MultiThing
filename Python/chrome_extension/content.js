/**
 * content.js — runs in every tab.
 * Finds the active media element, tracks its position, and forwards
 * { position, duration, playing } to the background service worker
 * which POSTs it to the Python server at localhost:8000.
 */
(function () {
  "use strict";

  let _media    = null;   // the tracked HTMLMediaElement
  let _ticker   = null;   // setInterval handle
  let _lastKey  = "";     // dedup: only send when state changed
  let _observer = null;   // MutationObserver for SPA nav

  // ----------------------------------------------------------------
  // Find the media element that is most likely the "main" one.
  // Prefers a playing element; falls back to any element with duration.
  // ----------------------------------------------------------------
  function findMedia() {
    const all = Array.from(document.querySelectorAll("video, audio"));
    return (
      all.find(m => !m.paused && m.duration > 0) ||
      all.find(m => m.duration > 0) ||
      null
    );
  }

  // ----------------------------------------------------------------
  // Build the payload and send it (with dedup so we don't spam).
  // ----------------------------------------------------------------
  function push(media) {
    if (!media) return;
    const pos     = Math.floor(media.currentTime);
    const dur     = Math.floor(media.duration) || 0;
    const playing = !media.paused && !media.ended && dur > 0;
    const key     = `${playing}|${pos}|${dur}`;
    if (key === _lastKey) return;
    _lastKey = key;
    chrome.runtime.sendMessage({ type: "MEDIA_UPDATE", pos, dur, playing });
  }

  // ----------------------------------------------------------------
  // Attach event listeners to a media element and start polling.
  // ----------------------------------------------------------------
  function attach(media) {
    if (_media === media) return;
    detach();
    _media = media;

    const immediate = () => push(media);
    ["play", "pause", "ended", "seeked", "durationchange"].forEach(e =>
      media.addEventListener(e, immediate)
    );

    // Poll every 1 s so the server always has a fresh timestamp.
    _ticker = setInterval(() => {
      if (!_media) return;
      // If a better (playing) element appeared, switch to it
      const best = findMedia();
      if (best && best !== _media) { attach(best); return; }
      push(_media);
    }, 1000);

    push(media); // send immediately on first attach
  }

  function detach() {
    if (_ticker) { clearInterval(_ticker); _ticker = null; }
    _media   = null;
    _lastKey = "";
  }

  // ----------------------------------------------------------------
  // Initialise: find a media element or wait for one to appear.
  // ----------------------------------------------------------------
  function init() {
    const m = findMedia();
    if (m) { attach(m); return; }

    // Wait for a media element to be inserted (covers lazy-loaded players).
    const mo = new MutationObserver(() => {
      const found = findMedia();
      if (found) { mo.disconnect(); attach(found); }
    });
    mo.observe(document.documentElement, { childList: true, subtree: true });
  }

  // ----------------------------------------------------------------
  // Re-init on SPA navigation (YouTube, Spotify, etc. swap pages
  // without a full reload).
  // ----------------------------------------------------------------
  let _lastUrl = location.href;
  new MutationObserver(() => {
    if (location.href !== _lastUrl) {
      _lastUrl = location.href;
      // Tell server position is invalid before we go silent for 800ms
      chrome.runtime.sendMessage({ type: "MEDIA_UPDATE", pos: 0, dur: 0, playing: false });
      detach();
      setTimeout(init, 800); // brief delay for new DOM to settle
    }
  }).observe(document, { subtree: true, childList: true });

  // ----------------------------------------------------------------
  // Kick off
  // ----------------------------------------------------------------
  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", init);
  } else {
    init();
  }
})();
