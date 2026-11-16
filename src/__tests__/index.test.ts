import { expect, it, jest } from '@jest/globals';

jest.mock('../NativeMeanNSuperResolution', () => ({
  __esModule: true,
  default: { superResolve: jest.fn() },
}));

import type { SuperResolveDropped, SuperResolveResult } from '../index';

const api = require('../index') as Record<string, unknown>;

it('exports superResolve and ErrorCodes, and no multiply (SR-DOC-01)', () => {
  expect(typeof api.superResolve).toBe('function');
  expect(api.ErrorCodes).toEqual({
    INVALID_ARGUMENT: 'E_INVALID_ARGUMENT',
    DECODE_FAILED: 'E_DECODE_FAILED',
    SIZE_MISMATCH: 'E_SIZE_MISMATCH',
    ALIGNMENT_FAILED: 'E_ALIGNMENT_FAILED',
    WRITE_FAILED: 'E_WRITE_FAILED',
    INTERNAL: 'E_INTERNAL',
  });
  expect('multiply' in api).toBe(false);
});

it('exports the result types with merge metadata (LP-API-01)', () => {
  const dropped: SuperResolveDropped = { index: 3, reason: 'x' };
  const result: SuperResolveResult = {
    path: 'file:///o.jpg',
    width: 1,
    height: 1,
    framesUsed: 2,
    refIndex: 0,
    used: [0, 1],
    dropped: [dropped],
  };
  expect(result.used[0]).toBe(result.refIndex);
});
