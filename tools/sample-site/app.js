// Minimal check of the WebSocket bridge: open /ws, send "P4 INFO", print
// whatever comes back. Frames are raw protocol bytes; message boundaries
// are not preserved, so a real client must reassemble lines itself.
const log = document.getElementById('log');
const ws = new WebSocket(`ws://${location.host}/ws`);
ws.binaryType = 'arraybuffer';
const dec = new TextDecoder();
ws.onopen = () => { log.textContent += 'ws open\n'; ws.send(new TextEncoder().encode('P4 INFO\n')); };
ws.onmessage = (ev) => { log.textContent += dec.decode(new Uint8Array(ev.data)); };
ws.onclose = () => { log.textContent += 'ws closed\n'; };
fetch('assets/data.json').then(r => r.json()).then(j => { log.textContent += `data.json: ${JSON.stringify(j)}\n`; });
