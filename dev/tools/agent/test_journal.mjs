// Offline unit test of agent/journal.mjs (no network, no game). node agent/test_journal.mjs
import assert from "node:assert/strict";
import * as J from "./journal.mjs";

const logs = [];
const log = (m) => logs.push(m);
const sit = (extra = {}) => ({ v: 3, faction: "US", time: 600, timeLeftSec: 3000, war: { phase: "running", tickets: 300, enemyTickets: 280, supplies: 200 },
  objectives: [{ id: "C", name: "Timber Hub", pos: [1000, 1000], radius: 80, owner: "none", side: "centre" }, { id: "D", name: "Harbor", pos: [2000, 500], radius: 80, owner: "USSR", side: "left" }],
  squads: [{ id: "US_Inf1", pos: [900, 0, 950] }], contacts: [], ...extra });

// --- tries from orders + outcome events
let j = J.emptyJournal();
J.noteOrders(j, [{ squad: "US_Inf1", command: "ATTACK", target: "C" }, { squad: "US_Inf2", command: "ATTACK", pos: [1010, 990] }, { squad: "US_Inf3", command: "MOVE", target: "C" }], sit(), log);
assert.equal(j.tries.C.tried, 2, "two attacks on C (one by pos)");
J.noteOrders(j, [{ squad: "US_Inf1", command: "ATTACK", target: "C" }], sit(), log);
assert.equal(j.tries.C.tried, 2, "re-sending the same order is not a new try");
J.ingestEvents(j, ["outcome US_Inf1 ATTACK C failed: unit lost (after 240 s)"], sit({ time: 700 }), null, log);
assert.equal(j.tries.C.failed, 1);
J.ingestEvents(j, ["US_Inf1 lost"], sit({ time: 701 }), null, log);
assert.equal(j.tries.C.failed, 1, "lost event after the outcome is a duplicate");
J.ingestEvents(j, ["US_Inf2 lost"], sit({ time: 710, squads: [] }), sit({ squads: [{ id: "US_Inf2", pos: [1100, 0, 1000] }] }), log);
assert.equal(j.tries.C.failed, 2);
assert.match(j.tries.C.last, /near target/);
// unmatched outcome (rule commander / bought squad) counts as one try
J.ingestEvents(j, ["outcome US_Inf9 ATTACK D failed: stuck: moved 3 m in 120 s (after 300 s)"], sit({ time: 720 }), null, log);
assert.deepEqual([j.tries.D.tried, j.tries.D.failed], [1, 1]);
J.ingestEvents(j, ["outcome US_Inf9 ATTACK D failed: stuck: moved 3 m in 120 s (after 300 s)"], sit({ time: 722 }), null, log);
assert.equal(j.tries.D.failed, 1, "duplicate within the closed window ignored");
// captured closes open attacks as done
J.noteOrders(j, [{ squad: "US_Inf4", command: "ATTACK", target: "C" }], sit({ time: 730 }), log);
J.ingestEvents(j, ["C Timber Hub captured by US", "outcome US_Inf4 ATTACK C done: objective C captured (after 40 s)"], sit({ time: 760 }), null, log);
assert.equal(j.tries.C.done, 1);
assert.equal(j.tries.C.tried, 3, "captured + outcome counted once");
assert.equal(j.tries.C.last, "captured");
// lines: C is captured; owned by us -> hidden; D shown
let lines = J.triesLines(j, sit({ time: 800, objectives: [{ id: "C", name: "Timber Hub", pos: [1000, 1000], owner: "US" }, { id: "D", name: "Harbor", pos: [2000, 500], owner: "USSR" }] }));
assert.equal(lines.length, 1);
assert.match(lines[0], /^D Harbor: 1 attacks, 1 failed/);
// enemy-held C is shown too
lines = J.triesLines(j, sit({ time: 800 }));
assert.equal(lines.length, 2);
assert.equal(J.triesLines(Object.assign(J.emptyJournal(), { tries: Object.fromEntries("ABCDEFG".split("").map((id, i) => [id, { id, name: "", tried: 3, failed: i + 1, done: 0, last: "x", lastAt: 1 }])) }), sit()).length, 5, "max 5 lines");

// --- review validation
let v = J.validateReview('Sure!\n{"intent":"' + "x".repeat(300) + '","goals":[{"text":"' + "g".repeat(80) + '","status":"open"},"plain",{"text":"c","status":"done"},{"text":"d","status":"open"}],"lessons":["' + "l".repeat(100) + '","b","c","d"]}');
assert.ok(v.value, v.error);
assert.equal(v.value.intent.length, 120);
assert.equal(v.value.goals.length, 3);
assert.equal(v.value.goals[0].text.length, 40);
assert.equal(v.value.lessons.length, 3);
assert.equal(v.value.lessons[0].length, 60);
assert.ok(v.notes.length >= 4, v.notes.join("|"));
assert.ok(J.validateReview('{"intent":1,"goals":[],"lessons":[]}').error);
assert.ok(J.validateReview('{"intent":"a","goals":[{"text":"x","status":"maybe"}],"lessons":[]}').error);
assert.ok(J.validateReview('{"intent":"a","goals":{},"lessons":[]}').error);
assert.ok(J.validateReview("no json here").error);
assert.ok(J.validateReview('{"intent":"","goals":[],"lessons":[]}').error, "empty review rejected");
assert.ok(J.validateReview(J.fakeReview()).value, "fake journal is valid");

// --- block
j.intent = "Hold C, flank D via the north"; j.goals = [{ text: "Take D", status: "open" }]; j.lessons = ["frontal on D failed"];
const block = J.renderBlock(j, sit({ time: 800 }));
assert.match(block, /^HQ JOURNAL/);
assert.match(block, /Intent: Hold C/);
assert.match(block, /Failed attacks:/);
assert.ok(block.length < 900, "block size " + block.length);
assert.equal(J.renderBlock(J.emptyJournal(), sit()), "");
console.log(J.reviewUser(sit(), j));

// --- war boundaries
j = J.emptyJournal();
assert.equal(J.newWarReason(j, sit()), "");
J.noteSituation(j, sit());
assert.match(J.newWarReason(j, sit({ time: 100 })), /time went back/);
assert.match(J.newWarReason(j, sit({ war: { phase: "setup" } })), /setup/);
assert.match(J.newWarReason(j, sit({ timeLeftSec: 3600 })), /time limit restarted/);
J.noteSituation(j, sit({ war: { phase: "running", id: "w1" } }));
assert.match(J.newWarReason(j, sit({ war: { phase: "running", id: "w2" } })), /war id changed/);
assert.equal(J.newWarReason(Object.assign(J.emptyJournal(), { active: false }), sit({ war: { phase: "setup" } })), "", "setup before any running phase: nothing to reset");

console.log("journal tests ok (" + logs.length + " log lines)");
