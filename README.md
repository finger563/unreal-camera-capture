# Camera Capture Plugin for UE 5

Plugin for capturing camera (RGB + depth + motion vectors) data from multiple
cameras in UE 5

Provides:
- An actor component `CaptureComponent` which can be added to any actor. It will
  automatically find all `SceneCapture2D` components that are attached to the
  parent actor. For each camera it finds, it will configure that camera to
  output depth and motion vectors (by applying the `M_DmvCapture` post process
  material to it) and will configure it to output to a render texture. It will
  then create a copy of each camera which will render the color data to a
  texture as well. These textures can be captured (for other uses in the
  engine), and optionally serialized / saved to disk. The component can be
  configured to capture every frame of the camera or to capture frames at a
  specific interval of time.
- A `M_DmvCapture` material for saving depth + motion vectors to a single
  texture (used by `CaptureComponent`)

See the
[unreal-camera-capture-example](https://github.com/finger563/unreal-camera-capture-example)
repo for a simple example project using this plugin.

See also the
[unreal-python-tools](https://github.com/finger563/unreal-python-tools) repo for
some example code which provides processing and display of the data produced by
the component in this repo.

## References

- [post process velocity lookup in
  MaterialTemplate.ush](https://github.com/EpicGames/UnrealEngine/blob/407acc04a93f09ecb42c07c98b74fd00cc967100/Engine/Shaders/Private/MaterialTemplate.ush#L2370)
- [Velocity vector encoding in shaders](https://github.com/EpicGames/UnrealEngine/blob/407acc04a93f09ecb42c07c98b74fd00cc967100/Engine/Shaders/Private/Common.ush#L1728)
- [Unreal visualization of velocity vectors](https://github.com/EpicGames/UnrealEngine/blob/407acc04a93f09ecb42c07c98b74fd00cc967100/Engine/Shaders/Private/MotionBlur/VisualizeMotionVectors.usf#L63)
- [PR 6933 for fixing camera motion velocity](https://github.com/EpicGames/UnrealEngine/pull/6933)
- [Coordinate space terminology in UE](https://docs.unrealengine.com/5.2/en-US/coordinate-space-terminology-in-unreal-engine/)
- [UE 5 FViewUniformShaderParameters](https://docs.unrealengine.com/5.0/en-US/API/Runtime/Engine/FViewUniformShaderParameters/)
- [SceneView.cpp setting the View Uniform Shader Parameters](https://github.com/EpicGames/UnrealEngine/blob/407acc04a93f09ecb42c07c98b74fd00cc967100/Engine/Source/Runtime/Engine/Private/SceneView.cpp#L2670)
