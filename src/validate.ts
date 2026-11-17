import { ErrorCodes } from './errors';

// invariant: bounds shared with cpp/sr/Pipeline.h (kMinFrames..kMaxFrames, kMinN..kMaxN).
const MIN_FRAMES = 3;
const MAX_FRAMES = 30;
const MIN_N = 2;
const MAX_N = 30;
const SCALES = [1, 2, 3, 4];
const EXTENSIONS = ['.jpg', '.jpeg', '.png'];

function invalid(detail: string): string {
  return `${ErrorCodes.INVALID_ARGUMENT}: ${detail}`;
}

function lowerExt(path: string): string {
  const base = path.slice(path.lastIndexOf('/') + 1);
  const dot = base.lastIndexOf('.');
  return dot < 0 ? '' : base.slice(dot).toLowerCase();
}

// invariant: mirrors cpp/sr/Validate.cpp; returns null iff the native call may start.
export function validateArgs(paths: unknown, options: unknown): string | null {
  if (!Array.isArray(paths)) return invalid('paths must be an array');
  if (paths.length < MIN_FRAMES || paths.length > MAX_FRAMES) {
    return invalid(
      `paths must contain ${MIN_FRAMES} to ${MAX_FRAMES} entries, got ${paths.length}`
    );
  }
  const bad = paths.findIndex((p) => typeof p !== 'string' || p === '');
  if (bad >= 0) return invalid(`paths[${bad}] must be a non-empty string`);

  if (options === undefined) return null;
  if (typeof options !== 'object' || options === null) {
    return invalid('options must be an object');
  }
  const { n, blur, scale, outputPath } = options as Record<string, unknown>;
  if (
    n !== undefined &&
    !(Number.isInteger(n) && (n as number) >= MIN_N && (n as number) <= MAX_N)
  ) {
    return invalid(
      `n must be an integer from ${MIN_N} to ${MAX_N}, got ${String(n)}`
    );
  }
  if (
    blur !== undefined &&
    !(typeof blur === 'number' && blur >= 0 && blur <= 1)
  ) {
    return invalid(`blur must be a number from 0 to 1, got ${String(blur)}`);
  }
  if (scale !== undefined && !SCALES.includes(scale as number)) {
    return invalid(`scale must be 1, 2, 3 or 4, got ${String(scale)}`);
  }
  if (
    outputPath !== undefined &&
    (typeof outputPath !== 'string' ||
      !EXTENSIONS.includes(lowerExt(outputPath)))
  ) {
    return invalid(
      `outputPath must end in .jpg, .jpeg or .png: '${String(outputPath)}'`
    );
  }
  return null;
}
