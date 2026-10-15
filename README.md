# Clausewitz - AI High Command for Arma Reforger

Two AI headquarters wage a whole war against each other: they buy and command infantry, motorized, mechanized and armored units, build FOBs, run supply convoys, react to what their soldiers actually see (real fog of war) and brief their side with SITREPs. You fight inside that war on either side, as a rifleman or as a squad leader who gets missions from HQ and can radio requests back.

The commanders run on a built-in rule engine out of the box. Optionally, an **LLM** (any OpenAI-compatible API, e.g. OpenRouter) can take over each HQ through a small Node.js **sidecar** server. It also gives you a web dashboard with a live map of the war.

- **Workshop:** [Clausewitz - AI High Command](https://reforger.armaplatform.com/workshop/C1A05E7B3D9F4A28) (in game: Workshop, search "Clausewitz")
- **Source and sidecar:** this repository
- **License:** MIT (see `LICENSE`)

## Features
- Own game mode with a setup menu: factions for both sides (any playable faction incl. RHS, WCS and other packs), war mode, battle size, fixed or random start positions, objectives.
- Any map: ready scenarios for Arland, Everon and Kolguyev, plus an automatic scenario on other maps (HQs and objectives found at game start).
- Control points, tickets, supplies and income, bases with respawn, FOBs, convoys, vehicle depots, arsenals; war ends on tickets, hold-all or time limit.
- Human players join HQ squads from the deploy/groups menu, receive missions as squad leaders, read SITREPs (journal, `/sitrep`), and use `/request`, `/report`, `/hq <free text>` and the HQ menu at request terminals.
- English and German texts; with the LLM, every player gets SITREPs and HQ replies in their own language.
- HQ radial menu (hold **U**, or `/hqr`): report contacts (what, count, distance, direction or "ahead"), cancel your last report, call for backup, requests (vehicle by type, resupply, squad, reinforce, mission, status), SITREP, full HQ menu.
- Natural-language radio: `/hq 2 enemy infantry 100m north` becomes a contact report, `/hq need help` a backup call (needs the LLM sidecar).
- Weather and time in the setup menu: day speed 0x-96x, start date and hour, six weather slots (or random) blended over the war; HQ gets weather and daylight updates.
- HQ commander traits per side (aggressive, defensive, methodical, cautious, opportunistic, crazy, or random): they shape the rule commander, the LLM's decisions, SITREP tone and radio style.
- Loadouts (beta): save your kit at any arsenal; server loadout library on the dashboard with item names, types and prices. Works alongside loadout-editor mods such as WCS_LoadoutEditor.
- Pack factions field their own soldiers: missing roles are filled from the faction's own roster, borrowed kin vehicles carry the side's own infantry; arsenal items without a price get a default by type (`Configs/CLW/ArsenalPrices.conf`).
- No dependencies: base game only. RHS / WCS / GRS content is picked up automatically when loaded alongside.

## Playing / hosting without the LLM
Nothing else is needed. Subscribe to the mod, start one of the Clausewitz scenarios (Arland, Everon, Kolguyev). The first player gets the setup menu. With no sidecar running, both HQs use the rule commander.

Dedicated server (`config.json`):
```json
"game": {
  "scenarioId": "{C1A05E7B3D9F5A01}Missions/CLW_Arland.conf",
  "mods": [ { "modId": "C1A05E7B3D9F4A28", "name": "Clausewitz - AI High Command" } ]
}
```
Other scenarios: `{C1A05E7B3D9F5A02}Missions/CLW_Eden.conf` (Everon), `{C1A05E7B3D9F5A03}Missions/CLW_Cain.conf` (Kolguyev).

Server settings live in `<server profile>/Clausewitz/overrides.json` (an example is written on first start): unattended setup preset, catalog fixes, debug chat, sidecar URL and token. Reference: [`dev/notes/overrides.md`](dev/notes/overrides.md).

## The LLM sidecar (optional)
The sidecar is the Node.js server in [`dev/tools/`](dev/tools). The game server sends it each faction's situation every few seconds and gets orders back. It also serves the dashboard (live map, forces, SITREPs, HQ chat, missions, configuration, loadouts). If the sidecar is down or slow, the game falls back to the rule commander automatically.

### 1. Install
Requirements: [Node.js 20+](https://nodejs.org/) and an API key for an LLM provider (OpenRouter recommended; any OpenAI-compatible endpoint works).
```sh
git clone https://github.com/underlines/reforger-clausewitz.git
cd reforger-clausewitz/dev/tools
npm install
node llm_sidecar.mjs
```
It listens on `http://127.0.0.1:8788/` by default.

### 2. Configure
1. Open the dashboard at http://127.0.0.1:8788/ and log in. **Default password: `clausewitz`**. Change it right away under *Usage & Config > Admin account*.
2. *Configuration*: choose the provider (`openrouter` or `openai-compatible`), the model and the API key, then use *Test connection*. `fake` is a built-in offline commander for testing.
3. Optional: a second, cheaper "weak" model for translations, and a backup model per slot.
4. Set a **game token** (*Usage & Config*) whenever the sidecar is reachable from other machines. The game server must send the same token (step 3).

Settings are stored in `dev/tools/sidecar-config.json` (never commit it: it holds your API key).

### 3. Point the game server at the sidecar
By default the game looks for the sidecar on the same machine (`http://127.0.0.1:8788/`). For a sidecar elsewhere, set the URL (and the token) on the **game server** in one of two ways:

- Command line of the server:
  ```
  ArmaReforgerServer.exe -config config.json -clwSidecar http://10.0.0.5:8788/ -clwSidecarToken <token>
  ```
- Or `<server profile>/Clausewitz/overrides.json`:
  ```json
  { "sidecarUrl": "https://clw.example.com/", "sidecarToken": "<token>" }
  ```

The command line wins over the file. The server log shows the endpoint in use: `CLW: sidecar url http://10.0.0.5:8788/ (command line -clwSidecar, token set)`.

### 4. Sidecar on another machine
Start the sidecar so it listens on the network and accepts your host name:
```sh
CLW_HOST=0.0.0.0 CLW_ALLOWED_HOSTS=clw.example.com node llm_sidecar.mjs
```
(PowerShell: `$env:CLW_HOST='0.0.0.0'; $env:CLW_ALLOWED_HOSTS='clw.example.com'; node llm_sidecar.mjs`)

- Always set a game token (step 2.4) when the port is reachable from outside.
- For the dashboard over the internet, put an HTTPS reverse proxy (Caddy, nginx, ...) in front and forward `X-Forwarded-Proto`. Plain http sends the dashboard password in clear text.
- Private network addresses (10.x, 192.168.x, 172.16-31.x, `*.lan`, `*.local`) are accepted automatically with `CLW_HOST=0.0.0.0`.

### 5. Docker
[`dev/tools/docker/`](dev/tools/docker) runs the sidecar as a container next to a dedicated server:
```sh
cd dev/tools/docker
cp .env.example .env   # edit: data dir, game dir, host name, reverse proxy
docker compose up -d --build
```
- The port is published on the host's loopback only (`127.0.0.1:8788`), so the game server on the same host needs no URL setting. This also works for a game server in host networking mode (e.g. a server manager container).
- Settings and caches live in `SIDECAR_DATA_DIR` (writable by uid 1000). The API key, game token and dashboard password are set in the dashboard, not in `.env`.
- Map image: a dedicated-server install has no UI map textures, so the dashboard map has no background image there (roads, buildings and places still show). Render it on a machine with the game client (`cd dev/tools && node map/index.mjs Arland CLW_Arland.png 2048`, likewise `Eden` / `Cain`) and put it into `SIDECAR_DATA_DIR/cache/maps-override/` named after the world file (`CLW_Arland.png`, `CLW_Eden.png`, `CLW_Cain.png`).
- `MODS_DIR` (optional) mounts the server manager's Workshop mod cache read-only (`.../mods/reforger/addons`, the folder of `<Name>_<GUID>` addon dirs) so the dashboard can draw maps of mod worlds (e.g. Takistan); mods are opened lazily, only when the current world needs them.
- The dashboard goes through a reverse proxy (Traefik labels in the compose file; `TRAEFIK_ENABLE=false` to skip them). Set a game token as soon as it is reachable from outside, and the same token on the game server (step 3).
- `SIDECAR_BUILD_CONTEXT` can be a git URL (`https://github.com/underlines/reforger-clausewitz.git#main:dev/tools`), or deploy the compose file as a Portainer git stack (compose path `dev/tools/docker/docker-compose.yml`). The `Sidecar` GitHub workflow runs the sidecar tests and an image build, then calls the `PORTAINER_WEBHOOK_URL` repo secret on a push to `main`.

### Environment variables
| Variable | Default | Meaning |
|---|---|---|
| `CLW_PORT` | `8788` | HTTP port |
| `CLW_HOST` | `127.0.0.1` | bind address (`0.0.0.0` = all interfaces) |
| `CLW_ALLOWED_HOSTS` | | extra host names / IPs (comma-separated) accepted in the `Host` header |
| `CLW_CONFIG` | `dev/tools/sidecar-config.json` | settings file |
| `CLW_DATA_DIR` | `dev/tools/.cache` | sessions, loadout store, map caches |
| `ARMA_REFORGER_DIR` | auto (Steam) | game install, used to render the dashboard map. Without it the map has no background image |
| `CLW_ADDON_DIRS` | | extra addon dirs for maps of worlds in Workshop mods (Takistan, own wrapper worlds). Separated by `;` (Windows) / `:` (Linux) or `,`; each entry is one addon dir (holds `*.pak`) or a parent folder of addon dirs. Listed before the dashboard setting `addonDirs`; game paks are searched first, then these in order, each opened only when needed (Docker: `MODS_DIR`) |
| `CLW_LLM`, `CLW_MODEL` | `fake` | initial provider/model before the first save in the dashboard |

## Chat commands (in game)
`/war` (setup / status), `/sitrep`, `/request <what>`, `/report <count> <inf|veh|armor|air|unknown> <dist> [dir|ahead|bearing]`, `/report cancel`, `/backup [note]` (help call, any role), `/hq <message>` (free-text radio to your HQ, needs the LLM), `/hq language <code>`, `/hqmenu`, `/hqr` (radial menu without the key; default key U).

## Repository layout
- `addon/`: the Reforger addon (`addon.gproj`, GUID `C1A05E7B3D9F4A28`), the only part that is published to the Workshop.
- `dev/tools/`: LLM sidecar, dashboard, tests, map-wrapper generator.
- `dev/notes/`, `dev/research/`: design notes and API research; `dev/VISION.md` (vision and roadmap), `dev/PROGRESS.md` (dev log).
- `dev/testpacks/`: test projects that load Clausewitz together with RHS / WCS.
- `CHANGELOG.md`, `VERSION`: release history.

## Building from source
Open `addon/addon.gproj` in Arma Reforger Tools (Workbench). For a local dedicated-server test, start `ArmaReforgerServer.exe -server Worlds/CLW_Arland.ent -addonsDir <folder containing a link to addon/> -addons C1A05E7B3D9F4A28`.

## Credits
Made by underlines with Claude Code. Arma Reforger and its content belong to Bohemia Interactive; this mod contains no copied game assets.
