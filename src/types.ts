export type SuperResolveOptions = {
  /**
   * Frames averaged, reference included (lab `mean_N`), integer 2..30. Default `8`.
   * Fewer are averaged when fewer frames survive the blur filter and alignment.
   */
  n?: number;
  /**
   * Blur filter ratio in [0, 1]: frames whose sharpness is below `blur` × the burst
   * median are dropped (unless fewer than 3 would remain). `0` turns it off. Default `0.8`.
   */
  blur?: number;
  /** Integer upscale factor applied to the mean with bicubic interpolation. Default `1`. */
  scale?: 1 | 2 | 3 | 4;
  /** Absolute path or `file://` URI ending in `.jpg`, `.jpeg` or `.png`. */
  outputPath?: string;
};

/** A frame left out of the mean. */
export type SuperResolveDropped = {
  /** Position of the frame in `paths`. */
  index: number;
  /** Lab log text, e.g. `ECC cc 0.873 < 0.9, dropped`. */
  reason: string;
};

export type SuperResolveResult = {
  /** `file://` URI of the written image. */
  path: string;
  width: number;
  height: number;
  /** Frames averaged, reference included (2..n); equals `used.length`. */
  framesUsed: number;
  /** Index in `paths` of the reference frame (sharpest of the first 8 kept); `used[0]`. */
  refIndex: number;
  /** Indices in `paths` of the averaged frames, in temporal order from the reference. */
  used: number[];
  /** Frames dropped by the blur filter or by alignment, with the reason. */
  dropped: SuperResolveDropped[];
};
