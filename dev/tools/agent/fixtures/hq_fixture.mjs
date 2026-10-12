// Fixture for the HQ chat agent turn: a manifest like the game's (the five player request entries + extra player entries that end up
// deferred behind find_tools), a v3 situation and /hq messages. Used by agent/test_agent.mjs and the second-instance HTTP test.
const req = (name, desc, params = [], cost = 0) => ({ name, layer: "primitive", channel: "requests", authority: "player", cost, tags: ["request"], requires: ["requests"], desc, params });
const extra = (name, desc, tags, params = []) => ({ name, layer: "primitive", channel: "requests", authority: "player", cost: 0, tags, requires: [], desc, params });

export const manifest = {
  hash: "fixture-1", faction: "US", mode: "conquest", capabilities: ["requests", "economy"],
  intents: [{ name: "ATTACK", layer: "intent", channel: "orders", authority: "hq", cost: 0, tags: ["offence"], requires: [], desc: "Assault the objective.", params: [{ name: "squad", type: "unit", required: true, hint: "squad id" }] }],
  primitives: [
    req("REQUEST_VEHICLE", "A player asks HQ for an empty vehicle from the faction catalog, parked at the vehicle depot. Soldiers and leaders; supplies, caps and a cooldown apply.", [{ name: "type", type: "string", required: false, hint: "catalog type, kind or part of the name; empty = cheapest transport" }]),
    req("REQUEST_SQUAD", "A player (squad leader) asks HQ for an AI infantry team that joins the player's squad.", [{ name: "type", type: "string", required: false, hint: "catalog infantry type; empty = main infantry unit" }]),
    req("REQUEST_RESUPPLY", "A player asks HQ for an ammo / resupply box at the player's position (near a friendly base) or at the HQ.", [], 20),
    req("REQUEST_REINFORCE", "A player (squad leader) asks HQ to send the nearest free AI group to the player's position."),
    req("REQUEST_RECON", "A player (squad leader) asks HQ to send the nearest free AI group to look at a map grid, an objective or the player's position.", [{ name: "where", type: "string", required: false, hint: "grid 'ddd-ddd' (100 m cells), objective id or 'here'" }]),
    extra("REQUEST_CONVOY", "A player asks HQ to send a supply convoy with a truck to a base or FOB.", ["logistics", "supply"], [{ name: "to", type: "base", required: true, hint: "base or FOB id" }]),
    extra("REQUEST_MEDEVAC", "A player asks for a medical evacuation vehicle to the player's position.", ["medical", "evac"]),
    extra("REQUEST_ARTILLERY", "A player asks for an artillery fire mission on a grid.", ["fire", "support"], [{ name: "where", type: "position", required: true, hint: "target" }]),
    // hq-only entries are never shown to the agent
    { name: "form_squad", layer: "primitive", channel: "orders", authority: "hq", cost: 0, tags: [], requires: [], desc: "HQ only.", params: [] },
  ],
};

export const situation = {
  v: 3, faction: "US", side: 0, time: 1300,
  war: { supplies: 310, tickets: 512, phase: "running" },
  objectives: [{ id: "B", name: "Highway Crossing", pos: [1500, 1400], grid: "015-014", owner: "US", contested: true }],
  bases: [{ faction: "US", pos: [600, 0, 760] }],
  squads: [
    { id: "US_Squad1", kind: "infantry", type: "Rifle squad", pos: [900, 0, 950], agents: 9, humanLed: true, state: "moving" },
    { id: "US_Squad2", kind: "infantry", type: "Rifle squad", pos: [1380, 0, 1320], agents: 7, state: "engaged" },
    { id: "US_Hmv1", kind: "light", type: "M998 HMG", pos: [1650, 0, 1500], agents: 3, state: "moving" },
  ],
  contacts: [{ pos: [1750, 0, 1620], desc: "5x USSR infantry", kind: "infantry", ageSec: 12 }, { pos: [2700, 0, 2330], desc: "1x USSR vehicle", kind: "vehicle", ageSec: 140 }],
  hq: [],
};

export const messages = {
  vehicle: { id: 11, player: "Alice", role: "soldier", squad: "US_Squad1", grid: "009-009", pos: [900, 950], text: "HQ, we need a truck at the camp, our feet hurt", ageSec: 1 },
  ammo: { id: 12, player: "Bob", role: "leader", squad: "US_Squad1", grid: "009-009", pos: [900, 950], text: "Brauchen Munition, bitte Nachschub", ageSec: 1 },
  recon: { id: 13, player: "Bob", role: "leader", squad: "US_Squad1", grid: "009-009", pos: [900, 950], text: "send someone to look at grid 017-016", ageSec: 1 },
  contacts: { id: 14, player: "Alice", role: "soldier", squad: "US_Squad1", grid: "009-009", pos: [900, 950], text: "where is the enemy right now?", ageSec: 1 },
  artillery: { id: 16, player: "Bob", role: "leader", squad: "US_Squad1", grid: "009-009", pos: [900, 950], text: "need an artillery strike on 1750 1620, enemy squad there", ageSec: 1 },
  convoy: { id: 15, player: "Bob", role: "leader", squad: "US_Squad1", grid: "009-009", pos: [900, 950], text: "can we get a supply convoy to base B", ageSec: 1 },
};
