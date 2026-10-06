# PCS
Point Cloud Sequence (PCS) is a UE plugin for rendering a sequence of XYZRGB point cloud frames. This plugin parses .ply files and render them with specified frame rate.

## Decode a PLY from another module

Add `PCS` to your module dependencies and include `PCSPlyLoader.h`:

```cpp
const FPCSPlyLoadResult Result = FPCSPlyLoader::LoadFromFile(FilePath);
if (Result.IsSuccess())
{
    TSharedPtr<const FPCSFrameData> Frame = Result.FrameData;
    // Frame->Vertices and Frame->Bounds are ready for consumers.
}
// On failure, Result.ErrorMessage describes the problem.
```

This is synchronous file I/O; invoke it on a worker for playback or publishing.
The loader has no UObject dependency and independent calls can run concurrently.
It supports binary little-endian PLY 1.0 with scalar vertex properties. Positions,
colors and bounds are decoded directly into `FPCSFrameData`; optional
`frame_to_world` comments are applied. No actor transform or color-space conversion
is applied. Treat returned frame data as immutable. Empty vertex sets are valid;
missing or malformed files return an error. Frame IDs, PTS and transport are the
caller's responsibility.

<img width="320" alt="longdress" src="https://github.com/user-attachments/assets/7d9f2c6f-718e-401c-b4bb-94ff7dd48375" />
<img width="320" alt="baseball" src="https://github.com/user-attachments/assets/e109fac5-450c-4232-9acf-4bc5681df222" />

PCS has demonstrated 30 FPS real-time playback of a VV sequence containing about 10 million XYZRGB points per frame. Each binary PLY frame is 148.5 MB, corresponding to 297 million points and 4.46 GB of uncompressed point-cloud data per second.

## Install
Add PCS to your Unreal project as a Git submodule and enable it in the Plugins window.

```sh
git submodule add https://github.com/yatagai-mm/pcs.git Plugins/PCS
```

## How to Use
### Load from .ply files
For C++ projects, add `PCS` to your module's dependencies:

```csharp
PublicDependencyModuleNames.Add("PCS");
```

A minimal Actor can use the component directly as its root:

```cpp
#include "PointCloudActor.h"
#include "PointCloudSequenceComponent.h"

APointCloudActor::APointCloudActor()
{
	auto* PointCloud = CreateDefaultSubobject<UPointCloudSequenceComponent>(TEXT("PointCloud"));
	SetRootComponent(PointCloud);
	PointCloud->SetRelativeScale3D(FVector(100.0));
	PointCloud->PointSize = 1.5f;
	PointCloud->FrameRate = 5.0f;
    PointCloud->bAutoPlay = true;
	PointCloud->bLoop = true;
}
```

You will see a detail panel like this:

<img width="480" alt="image" src="https://github.com/user-attachments/assets/1c105282-ab1c-48f0-8b48-8154aaa95b22" />

Then you can specify the path to the PLY sequence folder and the regex pattern for the PLY files.

Under **Point Cloud Sequence > Rendering**, **Convert sRGB to Linear** is enabled
by default. Enable it for sRGB-encoded PLY colors, or disable it for colors already
stored in linear RGB (the original PCS behavior). Alpha is unchanged. The setting
applies per component and updates the displayed frame even while playback is
paused. Blueprints can change it using **Set Convert SRGB to Linear**.

See [yatagai-mm/pcs-playground](https://github.com/yatagai-mm/pcs-playground) for a sample project.

### Streaming decoded frames

Include `PCSStreamInput.h` and `PointCloudSequenceComponent.h` in a module that
depends on `PCS`. No PLY files or transport library are required. Open the stream
and control playback on the game thread:

```cpp
FPCSStreamConfig Config;
Config.MaxBufferedFrames = 8;
Config.MaxBufferedBytes = 512ull * 1024 * 1024;
auto Input = PointCloud->OpenStream(Config);
PointCloud->Play();
```

Keep `Input` in the producer. From a receive/decoder worker, submit complete,
decoded frames with media timestamps in seconds:

```cpp
auto Data = MakeShared<FPCSFrameData, ESPMode::ThreadSafe>();
FPCSPointVertex Point;
Point.Position = FVector3f(1.0f, 2.0f, 3.0f);
Point.Color = FColor::Red;
Data->Vertices.Add(Point);
Data->Bounds += Point.Position;

FPCSTimedFrame Frame;
Frame.FrameId = 42;               // Non-negative application ID, independent of PTS.
Frame.PresentationTime = 12.5;    // Media time, not network arrival time.
Frame.Duration = 1.0 / 30.0;      // Positive; independent of the component's FrameRate.
Frame.Data = Data;
const EPCSSubmitResult Result = Input->TrySubmit(Frame);
// Handle Backpressure by retrying later or dropping according to producer policy.
// Accepted retains a shared reference: do not mutate Data after submission.
```
