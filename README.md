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
  data assets. Also supports frustum visualization in the editor.
- `FCameraIntrinsics` - Data asset for storing camera intrinsic parameters
  (focal length, principal point, image dimensions) that can be shared across
  multiple cameras (e.g., "RealSense D435" preset).
- A `M_DmvCapture` material for saving depth + motion vectors to a single
  texture (used by both capture systems)

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

## Camera Intrinsics

The plugin supports two modes for camera projection:

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

```
SaveLocation/
├── CameraName1/
│   ├── frame_0000000.exr          # RGB + Depth (in alpha channel)
│   ├── frame_0000000_motion.exr   # Motion vectors (X in R, Y in G)
│   ├── frame_0000000.json         # Metadata
│   ├── frame_0000001.exr
│   ├── frame_0000001_motion.exr
│   └── frame_0000001.json
├── CameraName2/
│   └── ...
└── camera_config.csv               # Legacy config file (CaptureComponent only)
    transformations.csv              # Legacy transform CSV (CaptureComponent only)
```

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
    "location": [x, y, z],           // In meters (converted from UE cm)
    "rotation": [pitch, yaw, roll],  // In degrees
    "quaternion": [w, x, y, z],      // Normalized quaternion
    "scale": [x, y, z]               // Component scale
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

### Legacy CSV Files (CaptureComponent only)

For backward compatibility, `CaptureComponent` also generates:

- **`camera_config.csv`**: Static camera configuration
  - Columns: `name`, `width`, `height`, `focalLength`, `fov`, `nearClipPlane`, `farClipPlane`, 
    `tx`, `ty`, `tz`, `qw`, `qx`, `qy`, `qz` (relative transform)

- **`transformations.csv`**: Per-frame actor transforms
  - Columns: `i` (frame index), `time`, `tx`, `ty`, `tz`, `qw`, `qx`, `qy`, `qz` (world position in meters + quaternion)

**Note**: The JSON metadata format is recommended for new projects as it includes more complete information.

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

## References

- [post process velocity lookup in
  MaterialTemplate.ush](https://github.com/EpicGames/UnrealEngine/blob/407acc04a93f09ecb42c07c98b74fd00cc967100/Engine/Shaders/Private/MaterialTemplate.ush#L2370)
- [Velocity vector encoding in shaders](https://github.com/EpicGames/UnrealEngine/blob/407acc04a93f09ecb42c07c98b74fd00cc967100/Engine/Shaders/Private/Common.ush#L1728)
- [Unreal visualization of velocity vectors](https://github.com/EpicGames/UnrealEngine/blob/407acc04a93f09ecb42c07c98b74fd00cc967100/Engine/Shaders/Private/MotionBlur/VisualizeMotionVectors.usf#L63)
- [PR 6933 for fixing camera motion velocity](https://github.com/EpicGames/UnrealEngine/pull/6933)
- [Coordinate space terminology in UE](https://docs.unrealengine.com/5.2/en-US/coordinate-space-terminology-in-unreal-engine/)
- [UE 5 FViewUniformShaderParameters](https://docs.unrealengine.com/5.0/en-US/API/Runtime/Engine/FViewUniformShaderParameters/)
- [SceneView.cpp setting the View Uniform Shader Parameters](https://github.com/EpicGames/UnrealEngine/blob/407acc04a93f09ecb42c07c98b74fd00cc967100/Engine/Source/Runtime/Engine/Private/SceneView.cpp#L2670)
