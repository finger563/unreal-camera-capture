# Camera Capture Plugin for UE 5

Plugin for capturing camera (RGB + depth + motion vectors) data from multiple
cameras in UE 5

Provides:
- An actor component `CaptureComponent` which can be added to any actor. It will
  automatically find all `UIntrinsicSceneCaptureComponent2D` components that are
  attached to the parent actor. For each camera it finds, it will configure that
  camera to output depth and motion vectors (by applying the `M_DmvCapture` post
  process material to it) and will configure it to output to a render texture.
  It will then create a copy of each camera which will render the color data to
  a texture as well. These textures can be captured (for other uses in the
  engine), and optionally serialized / saved to disk as EXR files. The component
  can be configured to capture every frame of the camera or to capture frames at
  a specific interval of time.
- `ACameraCaptureManager` - A manager actor that provides centralized control
  over multiple cameras using the `CameraCaptureSubsystem`. Automatically
  discovers and registers `UIntrinsicSceneCaptureComponent2D` cameras in the
  level, and provides Blueprint/C++ API for controlling capture.
- `UCameraCaptureSubsystem` - Game instance subsystem that handles the actual
  capture logic, file writing, and camera registration. Both `CaptureComponent`
  and `CameraCaptureManager` can be used - they produce identical output formats.
- `UIntrinsicSceneCaptureComponent2D` - A subclass of `USceneCaptureComponent2D`
  that supports custom camera intrinsics for precise camera calibration. Each
  camera can have different intrinsics defined either inline or via reusable
  data assets. Also supports frustum visualization in the editor. Use this for
  capturing/rendering from non-player cameras.
- `UIntrinsicCameraComponent` - A subclass of `UCameraComponent` that supports
  custom camera intrinsics for player cameras. Allows you to set precise focal
  lengths and principal points for first-person/third-person cameras, matching
  real-world camera specifications. Includes frustum visualization.
- `FCameraIntrinsics` - Data asset for storing camera intrinsic parameters
  (focal length, principal point, image dimensions) that can be shared across
  multiple cameras (e.g., "RealSense D435" preset).
- A `M_DmvCapture` material for saving depth + motion vectors to a single
  texture (used by both capture systems)
- Semantic segmentation: a per-camera class mask from the custom-depth
  stencil (`M_SegStencil`), with fine and coarse class tables, labelled by
  `seg:<Class>` tags (`UCameraCaptureSegmentationSubsystem`). See
  [Segmentation](#segmentation).

<!-- markdown-toc start - Don't edit this section. Run M-x markdown-toc-refresh-toc -->
**Table of Contents**

- [Camera Capture Plugin for UE 5](#camera-capture-plugin-for-ue-5)
  - [Usage](#usage)
  - [Camera Intrinsics](#camera-intrinsics)
  - [Data Collected](#data-collected)
  - [Segmentation](#segmentation)
  - [Developing](#developing)
    - [Code style](#code-style)
  - [References](#references)

<!-- markdown-toc end -->


## Usage

### Option 1: Using CaptureComponent (Per-Actor)

1. **Add cameras to your actor**: Attach `UIntrinsicSceneCaptureComponent2D` components
   to your actor (not the base `USceneCaptureComponent2D`).

2. **Configure camera intrinsics** (optional):
   - **Option A - Create reusable preset**: Right-click in Content Browser → 
     Miscellaneous → Data Asset → Select `CameraIntrinsicsAsset`. Configure
     the intrinsics and save (e.g., "DA_RealSense_D435").
   - **Option B - Inline configuration**: Set intrinsics directly on each camera
     component.
   - On the camera component, enable `bUseCustomIntrinsics`, choose between
     `bUseIntrinsicsAsset` or inline parameters.

3. **Add CaptureComponent**: Add the `CaptureComponent` to your actor. It will
   automatically find and configure all `UIntrinsicSceneCaptureComponent2D` cameras.

4. **Configure capture settings**: Set `TimerPeriod`, `SaveLocation`, and other
   options on the `CaptureComponent`.

### Option 2: Using CameraCaptureManager (Global)

1. **Add cameras to the level**: Place actors with `UIntrinsicSceneCaptureComponent2D`
   components in your level.

2. **Add CameraCaptureManager**: Place a `CameraCaptureManager` actor in your level.

3. **Configure manager settings**:
   - Set `OutputDirectory` for where to save captures
   - Set `CaptureEveryNFrames` for capture rate
   - Enable/disable RGB, depth, and motion vector capture
   - Enable `bAutoStartOnBeginPlay` to start capturing automatically

4. **Control via Blueprint/C++**: Use `StartCapture()`, `StopCapture()`, and other
   functions to control when capturing happens.

**Note**: Each camera now defines its own image resolution through its intrinsics
settings. The previous global `ImageWidth`/`ImageHeight` parameters have been
removed.

### Option 3: Using IntrinsicCameraComponent (Player Cameras)

Use `UIntrinsicCameraComponent` instead of the base `UCameraComponent` for player cameras (first-person, third-person, etc.) when you need precise camera calibration.

1. **Replace UCameraComponent**: In your Pawn or Character Blueprint/C++, replace
   the standard `CameraComponent` with `IntrinsicCameraComponent`.

2. **Configure intrinsics** (optional):
   - Enable `bUseCustomIntrinsics`
   - Either reference a `CameraIntrinsicsAsset` or configure inline parameters
   - Set focal lengths, principal point, and image dimensions

3. **Use as normal camera**: The component works exactly like `UCameraComponent` but
   with the added ability to use custom projection matrices for precise calibration.

**Use Cases**:
- Matching a real-world camera's field of view precisely
- Simulating specific camera hardware (e.g., GoPro, webcam)
- Research applications requiring exact camera parameters
- VR/AR applications with specific lens characteristics

## Camera Intrinsics

The plugin supports two modes for camera projection (available on both `UIntrinsicSceneCaptureComponent2D` and `UIntrinsicCameraComponent`):

1. **Custom Intrinsics Mode** (`bUseCustomIntrinsics = true`):
   - Builds a custom projection matrix from pixel-based camera parameters
   - Parameters: `FocalLengthX`, `FocalLengthY`, `PrincipalPointX`, `PrincipalPointY`
   - Use this to precisely match real-world camera sensors

2. **Maintain Y-Axis Mode** (`bMaintainYAxis = true`):
   - Adjusts horizontal FOV to maintain vertical FOV as aspect ratio changes
   - Matches gameplay camera behavior
   - Use this for consistent framing across different resolutions

See the
[unreal-camera-capture-example](https://github.com/finger563/unreal-camera-capture-example)
repo for a simple example project using this plugin.

See also the
[unreal-python-tools](https://github.com/finger563/unreal-python-tools) repo for
some example code which provides processing and display of the data produced by
the component in this repo.

## Data Output Format

Both `CaptureComponent` and `CameraCaptureManager` produce identical output in EXR format with JSON metadata.

### Directory Structure

Both systems organize output by **actor**, then by **camera**:

```
SaveLocation/
├── ActorName1/                        # e.g., "Robot_BP_C_0"
│   ├── camera_config.csv              # Legacy config file (CaptureComponent only)
│   ├── transformations.csv            # Legacy transform CSV (CaptureComponent only)
│   ├── CameraName1/                   # e.g., "HeadCamera"
│   │   ├── frame_0000000.exr          # RGB + Depth (in alpha channel)
│   │   ├── frame_0000000_motion.exr   # Motion vectors (X in R, Y in G)
│   │   ├── frame_0000000.json         # Metadata
│   │   ├── frame_0000001.exr
│   │   ├── frame_0000001_motion.exr
│   │   └── frame_0000001.json
│   └── CameraName2/                   # e.g., "ChestCamera"
│       └── ...
└── ActorName2/                        # e.g., "Robot_BP_C_1"
    └── ...
```

**Note**: The actor-based folder structure makes it easy to organize data from multiple robots or actors in the same capture session.

### EXR Files

- **`frame_NNNNNNN.exr`**: 
  - **RGB channels**: Linear color data (FLinearColor)
  - **Alpha channel**: Depth data (in cm, raw from depth buffer)
  - Format: EXR with 32-bit float precision
  
- **`frame_NNNNNNN_motion.exr`**:
  - **R channel**: Motion vector X (horizontal pixel motion)
  - **G channel**: Motion vector Y (vertical pixel motion)
  - **B/A channels**: Unused (0.0)
  - Format: EXR with 32-bit float precision

### JSON Metadata

Each frame has an accompanying JSON file (`frame_NNNNNNN.json`) with complete camera and transform information:

```json
{
  "frame_number": 0,
  "timestamp": 1.234,
  "camera_id": "RGBCamera",
  "world_transform": {
    "location": [x, y, z],           // In cm (Unreal units)
    "rotation": [pitch, yaw, roll],  // In degrees
    "quaternion": [w, x, y, z],      // Normalized quaternion
    "scale": [x, y, z]               // Component scale
  },
  "relative_transform": {
    "location": [x, y, z],           // Relative to owning actor root, in cm
    "rotation": [pitch, yaw, roll],  // In degrees
    "quaternion": [w, x, y, z],      // Normalized quaternion
    "scale": [x, y, z]               // Relative scale
  },
  "intrinsics": {
    "focal_length_x": 320.0,         // Pixels
    "focal_length_y": 320.0,         // Pixels
    "principal_point_x": 320.0,      // Pixels
    "principal_point_y": 240.0,      // Pixels
    "image_width": 640,              // Pixels
    "image_height": 480,             // Pixels
    "maintain_y_axis": false         // Y-axis FOV mode
  },
  "actor_path": "/Game/...",         // Full UE object path
  "level_name": "MyLevel"            // Level name
}
```

**Transform fields:**
- **`world_transform`**: Camera's absolute position/orientation in the level (Unreal world space, cm).
- **`relative_transform`**: Camera's position/orientation relative to its owning actor's root component. Useful for replaying captures on a differently-positioned actor — compose with the actor's current world transform to get the correct projection pose.

### Legacy CSV Files (CaptureComponent only)

For backward compatibility, `CaptureComponent` also generates:

- **`camera_config.csv`**: Static camera configuration
  - Columns: `name`, `width`, `height`, `focalLength`, `fov`, `nearClipPlane`, `farClipPlane`, 
    `tx`, `ty`, `tz`, `qw`, `qx`, `qy`, `qz` (relative transform)

- **`transformations.csv`**: Per-frame actor transforms
  - Columns: `i` (frame index), `time`, `tx`, `ty`, `tz`, `qw`, `qx`, `qy`, `qz` (world position in meters + quaternion)

**Note**: The JSON metadata format is recommended for new projects as it includes more complete information.

## Segmentation

Turn on `bCaptureSegmentation` on the `CameraCaptureManager` (or call
`UCameraCaptureSubsystem::SetCaptureSegmentation(true)`) and every camera
also writes a class mask per frame:

```
SaveLocation/
├── segmentation_classes.csv          # value,name,coarse_id,coarse_name,reserved
├── segmentation_classes.json         # the same, plus the coarse list
└── ActorName/CameraName/
    ├── frame_0000000_class.png        # 8-bit, fine class value per pixel (0 = unlabelled)
    ├── frame_0000000_class_coarse.png # 8-bit, coarse class id per pixel
    └── frame_0000000.json             # + "segmentation": classes in view with pixel counts
```

The masks are on the colour grid (the colour intrinsics and pose) and hold
exact values: read them as single-channel 8-bit images, with no colour
conversion. The class table is written beside the camera folders when capture
starts and again whenever a class is added mid-run.

**How it works.** While segmentation is on, every primitive renders into the
custom-depth pass: labelled ones with their class value in the stencil,
unlabelled ones with 0, so an unlabelled surface still hides a labelled one
behind it. A second capture per camera, with lighting and the other shading
features off, runs `M_SegStencil`, a post-process material that writes the
stencil to R (and 1 to G, so a frame where the material did not run, while it
is still compiling, say, is detected and dropped rather than written as
plausible-looking values). It costs one more render per camera, but a cheap
one. Custom depth needs `r.CustomDepth=3` (with stencil); the subsystem raises
it if it is lower and logs that it did.

The stencil pass respects masked materials, so foliage is cut out leaf by leaf,
and it works for skeletal and instanced meshes alike.

**Labels.** A primitive's class is the first of:

1. a component tag `seg:<Class>` (`SetComponentClass`);
2. an actor tag `seg:<Class>` (`SetActorClass`);
3. the first `ActorClassRules` entry its actor is an instance of (pedestrians,
   robots);
4. `UntaggedClass`, if set.

Otherwise it is unlabelled (0). Actors spawned while segmentation is on are
labelled as they appear; call `RelabelWorld` after re-tagging in bulk.

**Classes.** `[/Script/CameraCapture.CameraCaptureSegmentationSettings]` in
the project's `DefaultGame.ini`:

```ini
[/Script/CameraCapture.CameraCaptureSegmentationSettings]
TagPrefix=seg:
DefaultCoarseClass=other
+ReservedStencilValues=200            ; used by something else: never handed out, never overwritten
+ActorClassRules=(ActorClass="/Script/MyGame.MyPedestrian",Class="Person")
+CoarseClasses=road                   ; coarse id 1, in order
+CoarseClasses=person
+CoarseClasses=other
+Classes=(Name="Road",Coarse="road",Value=1)
+Classes=(Name="Person",Coarse="person",Value=2)
```

Up to 255 fine classes. Give each a `Value` to keep masks comparable across
runs. A tag naming a class not in the list still works: it gets the next free
value and `DefaultCoarseClass` for that run, and a warning says so. A
primitive that already renders custom depth with a reserved stencil value is
left alone, and its pixels carry that value (marked `reserved` in the table).

**Limits.**

- Translucent surfaces (glass, water) do not render custom depth unless their
  material allows custom-depth writes, so they take the class of what is
  behind them.
- One class per primitive: an instanced mesh component is one class for all of
  its instances, and a mesh with several materials is one class.
- Instances are not told apart. That is the next step: the class pass plus the
  depth plane, matched against each instance's bounds.

Scripted capture (dataset sweeps, tests) can put a camera anywhere with
`UCameraCaptureSubsystem::SpawnCamera(Name, WorldTransform, Intrinsics)`.

## Reading EXR Files

EXR files can be read using standard libraries:

**Python (OpenEXR):**
```python
import OpenEXR
import Imath
import numpy as np

# Read RGB + Depth
exr_file = OpenEXR.InputFile("frame_0000000.exr")
header = exr_file.header()
dw = header['dataWindow']
width = dw.max.x - dw.min.x + 1
height = dw.max.y - dw.min.y + 1

# Read channels
pt = Imath.PixelType(Imath.PixelType.FLOAT)
rgb_str = [exr_file.channel(c, pt) for c in ['R', 'G', 'B']]
depth_str = exr_file.channel('A', pt)

# Convert to numpy arrays
rgb = np.array([np.frombuffer(c, dtype=np.float32).reshape(height, width) for c in rgb_str])
rgb = np.transpose(rgb, (1, 2, 0))  # HWC format
depth = np.frombuffer(depth_str, dtype=np.float32).reshape(height, width)
```

**Python (imageio):**
```python
import imageio
import numpy as np

# Simpler API using imageio
img = imageio.imread("frame_0000000.exr")
rgb = img[:, :, :3]
depth = img[:, :, 3]
```

## Visualization

An example visualization of captured data:

![example](https://github.com/finger563/unreal-camera-capture/assets/213467/dc91ba5a-21bc-47c5-92cd-5b09414c6420)

## Developing

If you want to contribute or customize the plugin, you can clone the repository
and set it up in your Unreal project. If you want to make changes to the code,
please follow the code style guidelines below.

### Code style

1. Ensure `clang-format` is installed
2. Ensure [pre-commit](https://pre-commit.com) is installed
3. Set up `pre-commit` for this repository:

  ``` console
  pre-commit install
  ```

This helps ensure that consistent code formatting is applied.

## References

- [post process velocity lookup in
  MaterialTemplate.ush](https://github.com/EpicGames/UnrealEngine/blob/407acc04a93f09ecb42c07c98b74fd00cc967100/Engine/Shaders/Private/MaterialTemplate.ush#L2370)
- [Velocity vector encoding in shaders](https://github.com/EpicGames/UnrealEngine/blob/407acc04a93f09ecb42c07c98b74fd00cc967100/Engine/Shaders/Private/Common.ush#L1728)
- [Unreal visualization of velocity vectors](https://github.com/EpicGames/UnrealEngine/blob/407acc04a93f09ecb42c07c98b74fd00cc967100/Engine/Shaders/Private/MotionBlur/VisualizeMotionVectors.usf#L63)
- [PR 6933 for fixing camera motion velocity](https://github.com/EpicGames/UnrealEngine/pull/6933)
- [Coordinate space terminology in UE](https://docs.unrealengine.com/5.2/en-US/coordinate-space-terminology-in-unreal-engine/)
- [UE 5 FViewUniformShaderParameters](https://docs.unrealengine.com/5.0/en-US/API/Runtime/Engine/FViewUniformShaderParameters/)
- [SceneView.cpp setting the View Uniform Shader Parameters](https://github.com/EpicGames/UnrealEngine/blob/407acc04a93f09ecb42c07c98b74fd00cc967100/Engine/Source/Runtime/Engine/Private/SceneView.cpp#L2670)
