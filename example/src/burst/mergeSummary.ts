import type { SuperResolveResult } from 'react-native-mean-n-super-resolution';

// why: raw indices into the `paths` passed to superResolve (0-based), so the screen
// and the log read like the lab's run log.
export function mergeSummary(
  result: Pick<SuperResolveResult, 'refIndex' | 'used' | 'dropped'>
): string[] {
  const dropped = result.dropped.length
    ? result.dropped.map((d) => `${d.index} (${d.reason})`).join('; ')
    : 'none';
  return [
    `refIndex: ${result.refIndex}`,
    `used: ${result.used.join(', ')}`,
    `dropped: ${dropped}`,
  ];
}
