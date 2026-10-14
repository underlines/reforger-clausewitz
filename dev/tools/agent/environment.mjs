// Environment paragraph for HQ prompts: time, daylight, weather, next change, short tactical hints.
// Input: sit.env as built by CLW_Environment.GetJson() (time, daylight day|dusk|night|dawn, weather, next, nextInMin, ...).
export function envPrompt(sit) {
  const env = sit?.env;
  if (!env || typeof env !== 'object') return '';
  const parts = [];
  const time = env.time ? `${env.time}` : '';
  const light = env.daylight || '';
  if (time || light) parts.push(`time ${time}${light ? ` (${light})` : ''}`);
  if (env.weather) parts.push(`weather ${String(env.weather).toLowerCase()}`);
  if (env.next) {
    const mins = Number.isFinite(env.nextInMin) ? ` in about ${Math.max(1, Math.round(env.nextInMin))} min` : '';
    parts.push(`turning ${String(env.next).toLowerCase()}${mins}`);
  }
  if (!parts.length) return '';
  const hints = [];
  if (light === 'night') {
    hints.push('Night: detection range is short, use recon, avoid long vehicle moves without need.');
  } else if (light === 'dusk') {
    hints.push('Dusk: night is coming, finish long moves and prepare for low visibility.');
  } else if (light === 'dawn') {
    hints.push('Dawn: visibility is improving.');
  }
  const wet = `${env.weather || ''} ${env.next || ''}`.toLowerCase();
  if (/rain|storm|fog|mist|overcast/.test(`${env.weather || ''}`.toLowerCase())) {
    hints.push('Rain/fog/overcast lowers visibility; expect enemy contacts at shorter range.');
  } else if (/rain|storm|fog|mist/.test(wet)) {
    hints.push('Visibility will drop when the weather turns.');
  }
  return `ENVIRONMENT: ${parts.join(', ')}.${hints.length ? ' ' + hints.join(' ') : ''}`;
}
