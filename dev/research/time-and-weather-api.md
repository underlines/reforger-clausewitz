# Time and weather API (Reforger 1.8, read from the game scripts, no runtime test yet)

Sources: `scripts/GameCode/World/TimeAndWeatherManagerEntity.c`, `scripts/GameLib/generated/Entities/BaseWeatherManagerEntity.c`,
`GameLib/generated/WeatherState.c`, `BaseWeatherStateTransitionManager.c`, `WeatherStateTransitionNode.c`,
`Game/Components/Environment/SCR_TimeAndWeatherHandlerComponent.c`, `Editor/.../SCR_DayDurationEditorAttribute.c`.

## Getting the manager
`ChimeraWorld world = ChimeraWorld.CastFrom(GetGame().GetWorld()); TimeAndWeatherManagerEntity m = world.GetTimeAndWeatherManager();` (vanilla handler does the same with `GetOwner().GetWorld()`). VERIFIED in vanilla.

## Time and date (BaseWeatherManagerEntity, authority only, replicated automatically)
- `SetDate(year, month, day)` returns false for an invalid date; `CheckValidDate(y,m,d)`; `GetDate(out y, out m, out d)`. VERIFIED (handler `SetupDate`).
- `SetTimeOfTheDay(float hours24)`, `SetHoursMinutesSeconds(h,m,s)` (vanilla uses this), `GetTimeOfTheDay()` 0..24.
- `GetDayDuration()` / `SetDayDuration(realtimeSeconds)`: real seconds per game day. 1x = 86400, speed N = `86400 / N` (VERIFIED: `SCR_DayDurationEditorAttribute`: `SetDayDuration(86400 / multiplier)`, read back as `86400 / GetDayDuration()`).
- `SetIsDayAutoAdvanced(bool)` / `GetIsDayAutoAdvanced()`: used for 0x (clock stands still). Signature VERIFIED, behaviour at 0x untested (guess: weather transitions measured in game hours also freeze, so at 0x we only use instant transitions).
- Sun: `GetSunriseHour(out float)`, `GetSunsetHour(out float)` return false when the sun never rises/sets (vanilla falls back to 5 / 19). `IsSunSet()`, `IsNightHour(h)`, `IsDayHour(h)`.

## Weather
- `GetWeatherStatesList(out array<ref WeatherState>)`; `WeatherState.GetStateName()` (id, unique), `GetLocalizedName()` (string key), `GetStateID()`. VERIFIED (vanilla random starting weather uses the list + `GetRandomElement().GetStateName()`). Vanilla names (Tutorial enum SCR_EWeatherStates): Clear, Cloudy, Overcast, Rainy; the exact set per world comes from weatherStates.conf, not hard-wired here (fallback list only when discovery fails).
- `GetCurrentWeatherState()`; `GetTransitionManager().GetCurrentState()/GetNextState()/GetTimeLeftUntilNextState()` (game hours).
- `ForceWeatherTo(bool looping, string stateName, float transitionDurationHours, float stateDurationHours, int playerId)`: server only, "does not care if the transition is valid"; builds a transition node, `RequestStateTransitionImmediately`. `looping=true` keeps the state (no random next state); vanilla passes `!m_bRandomWeatherChanges`. Durations are IN-GAME HOURS: real seconds * speed / 3600. VERIFIED signature and use. The behaviour of `looping=true` with a transition duration > 0 is read from the code only.
- Rain/fog/wind overrides exist (`SetRainIntensityOverride`, `SetFogAmountOverride`, `SetWindSpeedOverride`) but are not used.

## Replication
Everything above is authority-only; the manager replicates date/time/day duration/weather to clients itself (`GetOnWeatherChanged` exists for client notifications). `GetTransitionManager()` is null on non-master clients. So all Clausewitz env state is server side. Vanilla sets time/weather in `SCR_TimeAndWeatherHandlerComponent.OnWorldPostProcess` (server), the same moment class of init as our war start.

## Not verified at runtime
daySpeed at 0x, transition behaviour with looping=true, whether `GetCurrentWeatherState()` flips at the start or the end of a transition, sunrise/sunset on non-default dates.

## Why the weather changed fast (M4.6 13b, static analysis, no runtime test)
- Not vanilla's state machine and not the day speed: `ForceWeather` converts real seconds to in-game hours (`sec * speed / 3600`), so ramps and holds scale correctly with the day speed. Mode `world` leaves vanilla's own random state machine alone (not changed).
- Cause was our plan: 6 slots over the time limit (60 min war = 10 min per slot), the change to the next slot ran over the second half of the slot (BLEND_FRACTION 0.5 = 5 min) and was split into one forced hop per state in between (e.g. Rainy -> Overcast -> Cloudy -> Clear = 3 hops of ~100 s). So a state lasted ~5 min and each change took ~5 min.
- Fix: setup param `weatherRate` (slow | normal | fast, default slow) = minimum slot length 45 / 25 / 0 min (fast = old limit/6). With slow in a 60 min war: one change, starting at 45 min with a ~22 min blend (~7 min per hop). Slots beyond the war end are never reached.
