# Camera Capture Plugin for UE 5

Plugin for capturing camera (RGB + depth + motion vectors) data from multiple
cameras in UE 5

Provides:
- An actor component `CaptureComponent` which can be added to any actor. It will
  automatically find all `UIntrinsicSceneCaptureComponent2D` components that are attached to the
  parent actor. For each camera it finds, it will configure that camera to
  output depth and motion vectors (by applying the `M_DmvCapture` post process
  material to it) and will configure it to output to a render texture. It will
  then create a copy of each camera which will render the color data to a
  texture as well. These textures can be captured (for other uses in the
  engine), and optionally serialized / saved to disk. The component can be
  configured to capture every frame of the camera or to capture frames at a
  specific interval of time.
- `UIntrinsicSceneCaptureComponent2D` - A subclass of `USceneCaptureComponent2D` that
  supports custom camera intrinsics for precise camera calibration. Each camera
  can have different intrinsics defined either inline or via reusable data assets.
- `FRammsCameraIntrinsics` - Data asset for storing camera intrinsic parameters
  (focal length, principal point, image dimensions) that can be shared across
  multiple cameras (e.g., "RealSense D435" preset).
- A `M_DmvCapture` material for saving depth + motion vectors to a single
  texture (used by `CaptureComponent`)

## Usage

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

## Data Collected

The `CameraCapture` plugin's `CaptureComponent` creates the following data files
within the `SaveLocation`:

- `camera_config.csv`: CSV formatted data (first row is header), which as the
  follow information:
  - `name`: The name of the camera
  - `width`: The width of the image in pixels
  - `height`: The height of the image in pixels
  - `focalLength`: The focal length of the camera in meters
  - `fov`: The horizontal field of view of the camera
  - `nearClipPlane`: The near clip plane of the camera view frustum in meters
  - `farClipPlane`: the far clip plane of the camera view frustum in meters (may be `inf`)
  - `tx`: The relative position of the camera w.r.t. the actor position, x-axis
  - `ty`: The relative position of the camera w.r.t. the actor position, y-axis
  - `tz`: The relative position of the camera w.r.t. the actor position, z-axis
  - `qw`: The relative orientation of the camera w.r.t. the actor orientation, w-component
  - `qx`: The relative orientation of the camera w.r.t. the actor orientation, x-component
  - `qy`: The relative orientation of the camera w.r.t. the actor orientation, y-component
  - `qz`: The relative orientation of the camera w.r.t. the actor orientation, z-component
- `transformations.csv`: CSV formatted data (first row is header), which has the
  following information:
  - `i`: frame index
  - `time`: time since the start of the program at which data
    (frames/transforms) were captured
  - `tx`: actor world position along the world x-axis
  - `ty`: actor world position along the world y-axis
  - `tz`: actor world position along the world z-axis
  - `qw`: actor world orientation as a quaternion, w-component
  - `qx`: actor world orientation as a quaternion, x-component
  - `qy`: actor world orientation as a quaternion, y-component
  - `qz`: actor world orientation as a quaternion, z-component
- `<camera name>_<index>.raw`: Uncompressed 4-channel, 32bit float formatted
  (32bit float per channel) color image data for `<camera_name>` at 0-indexed
  `<index>`. The mapping from `<index>` to time can be found in the
  `transformations.csv` file.
- `<camera name>_depth_motion_<index>.raw`: Uncompressed 4-channel 32bit float
  formatted (32bit float per channel) depth (red channel) + motion vector data
  (pixel x-y motion frame to frame in the green and blue channels) for `<camera
  name>` at 0-indexed `<index>`. The mapping from `<index>` to time can be found
  in the `transformations.csv` file.

The output data in this folder can be visualized using the [display_raw python
script](https://github.com/finger563/unreal-python-tools/blob/main/display_raw.py).
An example visualization can be found below:

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
