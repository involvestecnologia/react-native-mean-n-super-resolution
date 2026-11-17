// why: camera choices only; superResolve accepts 3 to 30 paths. The screen passes
// n = count, so 15 and 30 are the lab's mean_15 and mean_30.
export const FRAME_COUNTS = [5, 15, 30] as const;

export type FrameCount = (typeof FRAME_COUNTS)[number];

export const DEFAULT_FRAME_COUNT: FrameCount = 5;
