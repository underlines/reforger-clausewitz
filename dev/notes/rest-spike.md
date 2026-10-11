# REST spike (LLM bridge, M1 risk 1)

Question: can the game server script call a local HTTP service with JSON body and custom headers?

## Run
1. Start echo server (port 8787): `node dev/tools/echo_server.mjs` (this machine has NO real Python, only the MS Store stub;
   `python dev/tools/echo_server.py` is equivalent, untested here). The node twin was verified with curl.
2. Compile + Play the Clausewitz world (see reforger-workbench-test). `CLW_GameMode.RUN_REST_SPIKE = true` fires
   `CLW_RestSpike.Run()` 10 s after game start, server only.
3. Check game log and echo server console.

## Expected log lines (game console/log)
```
CLW: REST spike start
CLW: REST GET SetHeaders ok=1
CLW: REST GET sent id=<n>
CLW: REST POST SetHeaders ok=1
CLW: REST POST sent id=<n>
CLW: REST GET OnSuccess http=200 result=... size=<n> body={"echo":"/get?x=10&y=5","method":"GET",...}
CLW: REST POST OnSuccess http=200 ... body={"echo":{"prompt":"hello","n":1},"method":"POST",...}
```
Echo server should print `authorization: Bearer clw-test-token`, `x-clw-test: spike`, and for POST
`content-type: application/json` + the body. Failure shows `OnError code=... result=...` or `OnTimeout`.

## API (from api_search, group Online)
- `GetGame().GetRestApi().GetContext(string baseUrl)` -> RestContext. Request path is appended to the base URL.
- RestContext: async `GET(cb, req)`, `POST(cb, req, data)`, `PUT`, `DELETE`, `FILE`; each returns int. Sync variants
  `GET_now(req)`, `POST_now(req, data)` return the body string and BLOCK the main thread (avoid).
- `SetTimeout(int seconds)`, `SetHeaders(string definition)` returns bool, `reset()`.
- RestCallback: `SetOnSuccess/SetOnError(RestCallbackFunc)` is the new way; `OnSuccess(string data,int size)`,
  `OnError(int code)`, `OnTimeout()` overrides are marked OBSOLETE (still work, wiki example uses them). New style:
  check `GetRestResult() == ERestResult.EREST_ERROR_TIMEOUT`, `GetHttpCode()`, `GetData()`.
- Callback object lifetime is the scripter's job: keep a `ref` (spike uses static refs).

## Findings / limits
- Wiki (REST API Usage): async; body < 1 MB; "no support for custom headers" (OUTDATED: `SetHeaders` exists in the
  current API); Print() truncates strings > 1024 bytes in console (check dataSize; log chunks if needed).
- SetHeaders string format is NOT documented in the bundled wiki. Spike assumes comma-separated flat
  `"Name,Value,Name,Value"` (e.g. `"Content-Type,application/json"`). VERIFY at runtime via the echo server headers;
  alternatives to try if wrong: `"Name: Value"` or `"Name:Value"`.
- Headers set on the context persist for requests made from it; spike creates a fresh context per request.
- JSON helpers: `JsonApiStruct` (RegV("field"), `ExpandFromRAW(string)` to parse, `PackToString()`/`AsString` style pack),
  and vanilla `SCR_JsonSaveContext` / `SCR_JsonLoadContext` for serialization. Not exercised in the spike
  (POST body is a hand-written string).
- Localhost in Workbench play: no doc found restricting it; unverified. Dedicated servers/online play may restrict or
  require the `-rest` style allow-list; unknown. Plain `http://` (non-TLS) to 127.0.0.1 is the thing to prove.
