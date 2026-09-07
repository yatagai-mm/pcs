# PCS
UE plugin for rendering a sequence of XYZRGB point cloud frames. This plugin parses .ply files and render them with specified frame rate.

<img width="320" alt="longdress" src="https://github.com/user-attachments/assets/970edbfb-9d4f-4b03-9ebe-b887a9939bcd" />
<img width="320" alt="baseball" src="https://github.com/user-attachments/assets/0242378b-e06d-4047-9eda-e69479f3b708" />

PCS has demonstrated 30 FPS real-time playback of a VV sequence containing about 10 million XYZRGB points per frame. Each binary PLY frame is approximately 148.5 MB, corresponding to roughly 297 million points and 4.46 GB of uncompressed point-cloud data per second.

## How to Use

Add PCS to your Unreal project as a Git submodule:

```sh
git submodule add https://github.com/yatagai-mm/pcs.git Plugins/PCS
```

Enable PCS from Edit > Plugins, then restart Unreal Editor. After cloning the project elsewhere, initialize the submodule with:

```sh
git submodule update --init --recursive
```

For C++ projects, add `PCS` to your module's dependencies:

```csharp
PublicDependencyModuleNames.Add("PCS");
```

See [yatagai-mm/pcs-playground](https://github.com/yatagai-mm/pcs-playground) for a sample project.
