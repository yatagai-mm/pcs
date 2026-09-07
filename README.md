# PCS
UE plugin for rendering a sequence of XYZRGB point cloud frames. This plugin parses .ply files and render them with specified frame rate.

<img width="320" alt="longdress" src="https://github.com/user-attachments/assets/970edbfb-9d4f-4b03-9ebe-b887a9939bcd" />
<img width="320" alt="baseball" src="https://github.com/user-attachments/assets/0242378b-e06d-4047-9eda-e69479f3b708" />

PCS has demonstrated 30 FPS real-time playback of a VV sequence containing about 10 million XYZRGB points per frame. Each binary PLY frame is approximately 148.5 MB, corresponding to roughly 297 million points and 4.46 GB of uncompressed point-cloud data per second.

## Install
Add PCS to your Unreal project as a Git submodule:

```sh
git submodule add https://github.com/yatagai-mm/pcs.git Plugins/PCS
```

Enable PCS from Edit > Plugins, then restart Unreal Editor. After cloning the project elsewhere, initialize the submodule with:

```sh
git submodule update --init --recursive
```

## How to Use
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

See [yatagai-mm/pcs-playground](https://github.com/yatagai-mm/pcs-playground) for a sample project.
