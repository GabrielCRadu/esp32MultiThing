/**
 * background.js (MV3 service worker)
 * Receives MEDIA_UPDATE messages from content scripts and POSTs
 * them to the local Python server at localhost:8000/extension/update.
 * Silently ignores network errors (server may not be running).
 */

const SERVER = "http://localhost:8000/extension/update";

chrome.runtime.onMessage.addListener((msg) => {
  if (msg.type !== "MEDIA_UPDATE") return;
  fetch(SERVER, {
    method:  "POST",
    headers: { "Content-Type": "application/json" },
    body:    JSON.stringify({
      position: msg.pos,
      duration: msg.dur,
      playing:  msg.playing,
    }),
  }).catch(() => {});
});
