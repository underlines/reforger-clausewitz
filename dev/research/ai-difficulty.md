# AI difficulty knobs (vanilla), M4.6

Source: `scripts/Game/AI/Components/SCR_AICombatComponent.c` (game 1.8.x, read with `game_read`) and the Script API. Static research only, not yet run.

## Knobs on `SCR_AICombatComponent` (a component of the AI character entity)
| knob | API | notes |
|---|---|---|
| Skill | `enum EAISkill { NONE, NOOB=10, ROOKIE=20, REGULAR=50, VETERAN=70, EXPERT=80, CYLON=100 }`; `SetAISkill(EAISkill)`, `GetAISkill()`, `GetAISkillDefault()`, `ResetAISkill()` | prefab attribute `m_eAISkillDefault` (default **REGULAR 50**). `EOnInit` copies the default into `m_eAISkill`, so a value set before the component's init frame is overwritten. The skill is read by the AI behaviour trees (aim error, reaction time), not by script. |
| Perception | `SetPerceptionFactor(float)` (negative -> 0), `GetPerceptionFactor()` | multiplier on the visual detection speed (bigger = enemies recognised proportionally faster; "now it only affects visual detection"). `UpdatePerceptionFactor` multiplies it with the threat-state factor (SAFE 1.0, VIGILANT/ALERTED 2.5, THREATENED 0.4) and the equipment factor (binoculars 3.0), so the setting survives state changes. `SetPerceptionFactor` dereferences `m_Utility.m_ThreatSystem`: only call it after the character's init. |
| Fire rate | `SetFireRateCoef(float coef, bool persistent)` | group orders reset it (`SetGroupFireRateCoef`); semantics unclear and group-overridden, NOT used. |
| `PerceptionComponent.SetPerceptionFactor` | engine side of the above | not set directly, the combat component recomputes it. |

No `SCR_AIConfigComponent` knob for skill exists in the API (`api_search AISkill` lists only the combat component, the editor attribute `SCR_AiSkillEditorAttribute` and the Scenario Framework slots, which call the same setters).

## Enumeration
`AIWorld.GetAIAgents(out array<AIAgent>)` lists every spawned agent (groups and characters). A character agent: `agent.GetControlledEntity()`, faction from its `FactionAffiliationComponent`. Player-controlled characters are skipped via `PlayerManager.GetPlayerIdFromControlledEntity`.

## Mapping chosen (`CLW_AiDifficulty`)
| preset | skill | perception factor |
|---|---|---|
| easy | ROOKIE (20) | 0.6 |
| normal | untouched (vanilla REGULAR 50, factor 1.0): a no-op | - |
| hard | VETERAN (70) | 1.3 |
| veteran | EXPERT (80) | 1.6 |

A preset per side (`aiDifficultyA`, `aiDifficultyB`, side = scenario slot 0/1, mapped to the AI's faction key). A server sweep every 4 s sees each new agent once, waits one sweep (so `EOnInit` has run), then applies the side's preset once. Log: `CLW: ai difficulty <side> <preset> applied to N new agents`.
