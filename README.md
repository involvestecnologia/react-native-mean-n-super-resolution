# react-native-mean-n-super-resolution

Multi-frame image merging for React Native. It takes a burst of 3 to 30 photos of the same scene and returns one lower-noise image, optionally upscaled. It is a port of the `mean_N` method of the burst-sr-lab research code. Blurry frames are dropped and the sharpest frame becomes the reference. The others are aligned to it with a homography, and the closest `n` frames are averaged. The mean is then upscaled with bicubic interpolation. It uses OpenCV ([opencv-mobile](https://github.com/nihui/opencv-mobile)).

- Runs off the JS thread and returns a Promise.
- Takes local files in and writes a file out (JPEG or PNG).
- Uses one shared C++ pipeline on iOS and Android, built as a pure C++ TurboModule.

## Requirements

- React Native **New Architecture** only. The Old Architecture (bridge) is not supported.
- **Android:** NDK **r30** (`30.0.14904198`) or newer. The prebuilt opencv-mobile static libraries are built with a recent NDK and do not link with RN 0.88's default NDK 27. Set it in `android/build.gradle`:

  ```groovy
  buildscript {
    ext {
      ndkVersion = "30.0.14904198"
    }
  }
  ```

- **iOS:** CocoaPods with `curl` and `unzip` on the machine that runs `pod install`.
- The first native build needs network access to download OpenCV (see below).

## Installation

```sh
npm install react-native-mean-n-super-resolution
# or
yarn add react-native-mean-n-super-resolution
```

iOS:

```sh
cd ios && bundle exec pod install
```

### OpenCV is downloaded at build time

OpenCV is not in the npm package. The library pins one opencv-mobile release in `opencv.json` (version, URL and SHA-256 for each platform). The package is downloaded on first build, its checksum is verified, and it is cached in `node_modules/react-native-mean-n-super-resolution/.opencv/`:

- **Android:** downloaded by the library's CMake while Gradle configures the native build. All ABIs share one download.
- **iOS:** downloaded while CocoaPods evaluates the podspec (`pod install`), then vendored as `opencv2.xcframework`.

A checksum mismatch fails the build.

**Offline or air-gapped Android builds:** set `OPENCV_MOBILE_DIR` to an already-extracted opencv-mobile Android package and the download is skipped:

```sh
export OPENCV_MOBILE_DIR=/path/to/extracted/opencv-mobile-android   # the folder containing opencv-mobile-4.14.0-android/
```

## Usage

```ts
import {
  superResolve,
  ErrorCodes,
  type SuperResolveResult,
} from 'react-native-mean-n-super-resolution';

const paths = [
  'file:///data/user/0/com.app/cache/burst-0.jpg',
  'file:///data/user/0/com.app/cache/burst-1.jpg',
  'file:///data/user/0/com.app/cache/burst-2.jpg',
  'file:///data/user/0/com.app/cache/burst-3.jpg',
  'file:///data/user/0/com.app/cache/burst-4.jpg',
];

try {
  const result: SuperResolveResult = await superResolve(paths, {
    n: 8,
    scale: 2,
  });
  console.log(result.path, result.width, result.height);
  console.log(result.refIndex, result.used, result.dropped);
} catch (e) {
  const message = e instanceof Error ? e.message : String(e);
  if (message.startsWith(ErrorCodes.ALIGNMENT_FAILED)) {
    // frames are too different from the reference
  }
}
```

### `superResolve(paths, options?)`

`superResolve(paths: string[], options?: SuperResolveOptions): Promise<SuperResolveResult>`

#### `paths`

- **Frame count:** the array must contain **3 to 30** paths. Any other length rejects with `E_INVALID_ARGUMENT`.
- **Path form:** each entry is an absolute filesystem path or a `file://` URI. Percent-encoding such as `%20` is decoded. `content://`, `ph://` and `http(s)://` URIs are rejected, so copy those images to a local file first.
- **Reference frame:** chosen by the pipeline, not by position: it is the sharpest of the first 8 frames left after the blur filter. `result.refIndex` reports it, and the output is aligned to it.
- **Frame sizes:** every frame must decode to the same width × height as `paths[0]`. This is checked for all frames before any alignment starts.
- **Formats:** frames can be JPEG, PNG, BMP or PNM. Grayscale and alpha inputs are converted to 3-channel color, and EXIF orientation is applied to all frames.
- **Duplicates:** the same path may appear more than once. Each entry counts as a separate frame.

#### Options

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `n` | `number` | `8` | Frames averaged, reference included (the lab's `mean_N`), an integer from 2 to 30. Fewer are averaged when fewer frames survive the blur filter and alignment. Pass `paths.length` to average every usable frame. |
| `blur` | `number` | `0.8` | Blur filter ratio, from 0 to 1. A frame whose sharpness is below `blur ×` the burst median is dropped, unless fewer than 3 frames would remain (then none is dropped). `0` turns the filter off. |
| `scale` | `1 \| 2 \| 3 \| 4` | `1` | Integer upscale factor, applied to the mean with bicubic interpolation. Output size is `scale ×` the frame size. |
| `outputPath` | `string` | unique file in the platform temp dir | Absolute path or `file://` URI where the result is written. The extension (case-insensitive) picks the format: `.jpg`/`.jpeg` writes JPEG at quality 95, `.png` writes PNG. An existing file is overwritten, and the directory must already exist. |

#### Result

```ts
type SuperResolveResult = {
  path: string;          // file:// URI of the written image
  width: number;         // scale × frame width
  height: number;        // scale × frame height
  framesUsed: number;    // frames averaged, reference included (2..n); equals used.length
  refIndex: number;      // index in paths of the reference frame; used[0]
  used: number[];        // indices in paths of the averaged frames, in temporal order from the reference
  dropped: SuperResolveDropped[]; // frames dropped by the blur filter or by alignment
};

type SuperResolveDropped = {
  index: number;  // position of the frame in paths
  reason: string; // lab log text, e.g. "ECC cc 0.873 < 0.9, dropped"
};
```

Indices are 0-based positions in `paths`. A frame that is never looked at is in neither list. This happens when `n` frames were accepted before alignment reached it.

#### How the image is built

The steps are the same as burst-sr-lab's `mean_N` followed by `bicubic_x2`:

1. **Sharpness:** each frame is decoded and converted to a gray copy whose long side is 1024 px. Its sharpness is the variance of the Laplacian of that copy.
2. **Blur filter:** frames with sharpness below `blur ×` the median sharpness are dropped. If fewer than 3 frames would remain, none is dropped.
3. **Reference:** the sharpest of the first 8 remaining frames.
4. **Order:** the remaining frames are sorted by distance to the reference in the burst (`|i − refIndex|`; on a tie the earlier frame comes first).
5. **Alignment:** in that order, each frame gets a starting shift from phase correlation. ECC then refines it to a homography on the gray copies. A frame whose ECC correlation is below 0.9, or whose ECC does not converge, is dropped. Accepted frames are warped onto the reference (bilinear, replicated border). Alignment stops as soon as `n` frames, reference included, are accepted.
6. **Mean:** the accepted frames are averaged per pixel and rounded half to even, like numpy.
7. **Upscale:** with `scale > 1`, the mean is resized with bicubic interpolation.

`dropped[].reason` uses the lab's log text:

| Reason | Step |
| --- | --- |
| `sharpness 0.41 of median < 0.8, dropped (blur)` | Blur filter |
| `ECC cc 0.873 < 0.9, dropped` | Alignment: correlation too low |
| `ECC did not converge, dropped` | Alignment: ECC failed |

- **Too few frames:** if fewer than 2 frames are accepted, including the reference, the call rejects with `E_ALIGNMENT_FAILED`. The message lists the dropped frames and their reasons.
- **Determinism:** the same inputs and options give byte-identical output on the same device and build.
- **Parity with the lab:** on the synthetic test bursts, the reference, used frames and drops match the lab's Python code. The homography corners differ by less than 0.05 px, and the scale-1 mean is within 2 levels (PSNR ≥ 45 dB). The small pixel differences come from the JPEG decoder (see `yarn test:parity` below).

### Default output location and lifecycle

When `outputPath` is omitted, the result is written as JPEG to a unique `superres-<…>.jpg` file in the platform temp/cache directory:

- **Android:** `TMPDIR` when it is set and writable, otherwise the app's cache dir (`Context.getCacheDir()`).
- **iOS:** `NSTemporaryDirectory()`.

Concurrent calls never write to the same file.

The OS can delete files in these folders at any time, so move or copy results you want to keep. The library never deletes output files, so you must clean up old ones yourself.

### Errors

The Promise rejects with an `Error` whose `message` starts with one of these codes, followed by `: ` and a detail. The codes are exported as `ErrorCodes`.

| Code | When |
| --- | --- |
| `E_INVALID_ARGUMENT` | `paths` is not an array of 3 to 30 non-empty strings, `n` is not an integer from 2 to 30, `blur` is not a number from 0 to 1, `scale` is not 1–4, `outputPath` has an extension other than `.jpg`/`.jpeg`/`.png`, or a path uses an unsupported URI scheme. Array, count, `n`, `blur`, `scale` and extension checks run in JS before the native call. The native side re-checks them, and rejects non-`file://` URI schemes (`content://`, `ph://`, `http(s)://`) before decoding anything. |
| `E_DECODE_FAILED` | A frame is missing or cannot be decoded. The message includes `index=<i>`. |
| `E_SIZE_MISMATCH` | A frame's size differs from `paths[0]`. The message includes the first mismatching `index=<i>` and both sizes. It is raised before any alignment. |
| `E_ALIGNMENT_FAILED` | Fewer than 2 frames, including the reference, could be aligned (the message lists the drops), or the frames are too thin to align (a side under 2 px in the 1024 px gray copy). |
| `E_WRITE_FAILED` | The output file cannot be written, for example because the directory is missing or not writable, or no temp directory is available. |
| `E_INTERNAL` | An unexpected C++ exception was thrown, or the estimated peak memory exceeds the 1 GiB budget. |

#### Memory limit and residual crash risk

- **Memory budget:** right after decoding `paths[0]`, the pipeline estimates its peak memory from the frame size `w × h`, the frame count and `scale`. If that exceeds **1 GiB**, the call rejects with `E_INTERNAL` before anything else is allocated. The estimate is the larger of two phases:
  - **merge:** `frames × 4 × a + 18 × w × h + 96 MiB`, where `a` is the area of the 1024 px gray copy and 96 MiB covers ECC;
  - **upscale:** `3 × w × h × (1 + scale²)`.

  For example, 30 frames of 12 MP (4000 × 3000) work at every scale (≈ 584 MiB at `scale: 4`). 30 frames of 48 MP (8000 × 6000) work at `scale: 1` and `2` (≈ 1010 MiB) but not at `scale: 3`.
- **Residual crash risk:** opencv-mobile is built without C++ exceptions, so an internal OpenCV assertion terminates the process instead of throwing. Every input is validated before it reaches OpenCV to prevent this. A crash is still possible in situations those checks did not anticipate.

### Web

Not supported. On web, `superResolve` rejects with `'react-native-mean-n-super-resolution' is only supported on native platforms.`

## Example app

`example/` has two screens:

- **Home:** merges 5 bundled frames (generated by `example/scripts/generate-frames.mjs`) at `scale: 2`. It shows the result, its dimensions, `framesUsed`, `refIndex`, `used`, `dropped`, and a JS heartbeat counter that keeps ticking while the native work runs.
- **Camera burst:** captures 5, 15 or 30 frames with react-native-vision-camera and merges all of them (`n` = frame count). The preview shows the same metadata and can switch to the reference frame. Each run is logged as `[camera-burst] ok N=… refIndex: … | used: … | dropped: …`.

```sh
yarn
yarn example android   # or: yarn example ios
```

## Contributing

- [Development workflow](CONTRIBUTING.md#development-workflow)
- [Sending a pull request](CONTRIBUTING.md#sending-a-pull-request)
- [Code of conduct](CODE_OF_CONDUCT.md)

The native pipeline has a host GoogleTest suite: `yarn test:native` (needs CMake and a C++20 compiler).

`yarn test:parity` compares the native merge with burst-sr-lab's own Python code. It needs a burst-sr-lab checkout in `LAB_DIR`, plus Python 3 with `opencv-python` and `numpy`. It runs on deterministic synthetic bursts, and also on pulled field sessions with `--sessions <dir>`. It prints one pass/fail row per burst. Tolerances and results are in `.specs/features/lab-parity-mean/validation.md`.

## Migrating from the earlier API

This version changes the algorithm and the API:

- **Frame count:** `paths` takes 3 to 30 frames. The "exactly 5, 15 or 30" rule is gone.
- **`scale` default:** it is now `1`, down from `2`. Pass `{ scale: 2 }` to keep the old output size.
- **Reference:** the reference is no longer `paths[0]`. It is the sharpest early frame, given in `result.refIndex`.
- **Frames averaged:** only the `n` frames closest to the reference are averaged (default 8). Pass `{ n: paths.length }` to average every usable frame, as before.
- **Blur filter:** blurry frames are dropped first (`blur`, default 0.8). Pass `{ blur: 0 }` to disable it.
- **Alignment:** frames are aligned with a homography (phase correlation + ECC), so rotation and perspective are compensated, not only shifts. Frames below ECC correlation 0.9 are dropped.
- **Output:** the frames are averaged at their own size and the mean is upscaled once. Before, each frame was upscaled and then averaged. Results differ from the earlier version.
- **Result:** new fields `refIndex`, `used` and `dropped`.
- **Errors:** `E_SIZE_MISMATCH` is raised before any alignment. `E_ALIGNMENT_FAILED` lists the dropped frames. The `E_INVALID_ARGUMENT` messages for frame count, `n` and `blur` are new.

## Credits

The pipeline is a port of the `mean_N` and `bicubic_x2` methods of burst-sr-lab, a burst super-resolution research project. The frame alignment uses a vendored copy of OpenCV 4.14.0's ECC (`cpp/sr/vendor/`, under its upstream Intel License Agreement header, kept verbatim). Unlike the library version, it reports non-convergence instead of aborting.

## License

MIT

---

Made with [create-react-native-library](https://github.com/callstack/react-native-builder-bob)
