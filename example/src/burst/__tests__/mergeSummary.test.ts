import { describe, expect, it } from '@jest/globals';
import { mergeSummary } from '../mergeSummary';

describe('mergeSummary (LP-DOC-03)', () => {
  it('lists reference, used frames in merge order and drops with reasons', () => {
    expect(
      mergeSummary({
        refIndex: 6,
        used: [6, 7, 4],
        dropped: [
          {
            index: 3,
            reason: 'sharpness 0.07 of median < 0.8, dropped (blur)',
          },
          { index: 5, reason: 'ECC cc 0.117 < 0.9, dropped' },
        ],
      })
    ).toEqual([
      'refIndex: 6',
      'used: 6, 7, 4',
      'dropped: 3 (sharpness 0.07 of median < 0.8, dropped (blur)); 5 (ECC cc 0.117 < 0.9, dropped)',
    ]);
  });

  it('says none when nothing was dropped', () => {
    expect(mergeSummary({ refIndex: 0, used: [0, 1], dropped: [] })).toEqual([
      'refIndex: 0',
      'used: 0, 1',
      'dropped: none',
    ]);
  });
});
