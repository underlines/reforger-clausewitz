# Workshop page text (Clausewitz)

Source of truth for the Workshop dialog fields (Workbench: File > Publish to Workshop). The CLI update (`-publishAddon`) only sets the
version and the change notes; Summary and Description change only through the dialog. Limits: Summary 200 chars, Description 5000.

## Summary
AI vs AI wars with fog of war, logistics, FOBs and SITREPs. Fight on either side as soldier or squad leader. Rule-based HQ built in, optional LLM commander via open-source sidecar.

## Description
```
CLAUSEWITZ - AI HIGH COMMAND

Two AI headquarters fight a whole war. They see only what their soldiers see, buy forces with supplies, send squads, vehicles and convoys, build FOBs and brief their side. You fight inside that war.

FEATURES
- Own game mode with a setup menu in sections: factions for both sides (vanilla or mod packs such as RHS / WCS), war mode, battle size, AI difficulty per side, fixed or random starts, weather and time. The menu remembers your last round and has "Reset to defaults".
- Scenarios for Arland, Everon and Kolguyev; an automatic scenario on other maps.
- Control points, tickets, supplies and income, bases with respawn, arsenals, vehicle depots, FOBs, supply convoys.
- Infantry, motorized, mechanized and armored units that mount, dismount, overwatch and return to base.
- Join HQ squads from the deploy/groups menu, receive missions as squad leader (shared with your squad), read SITREPs, use /request, /report, /hq and the HQ menu (hold U).
- English and German; with the LLM every player gets SITREPs and HQ replies in their own language.
- Loadouts (beta): save your kit at any arsenal and spawn with it.
- No dependencies.

OPTIONAL LLM COMMANDER
Without anything else, both HQs use the built-in rule commander. To let an LLM command (it also picks FOB sites from a map picture, and you get a live web dashboard of the war), run the free sidecar server next to your game server:
https://github.com/underlines/reforger-clausewitz
The README explains installation, the API key, and how to point the server at a remote sidecar (-clwSidecar <url> or "sidecarUrl" in <profile>/Clausewitz/overrides.json).

SERVER
scenarioId: {C1A05E7B3D9F5A01}Missions/CLW_Arland.conf (Everon: {C1A05E7B3D9F5A02}Missions/CLW_Eden.conf, Kolguyev: {C1A05E7B3D9F5A03}Missions/CLW_Cain.conf).
Settings and an unattended setup preset: <server profile>/Clausewitz/overrides.json.

Open source (MIT): https://github.com/underlines/reforger-clausewitz
```
