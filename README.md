## PCS
UE plugin for rendering a sequence of XYZRGB point cloud frames. This plugin parses .ply files one by one and render them with specified frame rate.

### Components
`PointCloudSequenceComponent` is a game-thread-facing component exposed to the plugin users. This component generates `PCSPlyLoader` which parses .ply header and puts its payload into `FPCSFrameData` struct on a worker thread.

