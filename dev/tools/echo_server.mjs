// Node twin of echo_server.py (this machine has no real Python). node dev/tools/echo_server.mjs
import http from "node:http";
http.createServer((req, res) => {
  let body = "";
  req.on("data", (c) => (body += c));
  req.on("end", () => {
    console.log(`---- ${req.method} ${req.url}`);
    for (const [k, v] of Object.entries(req.headers)) console.log(`  ${k}: ${v}`);
    console.log(`  body: ${JSON.stringify(body)}`);
    let echo = req.url;
    if (body) { try { echo = JSON.parse(body); } catch { echo = body; } }
    const out = JSON.stringify({ echo, method: req.method, path: req.url, headers: req.headers });
    res.writeHead(200, { "Content-Type": "application/json" });
    res.end(out);
  });
}).listen(8787, "127.0.0.1", () => console.log("echo server on http://127.0.0.1:8787"));
