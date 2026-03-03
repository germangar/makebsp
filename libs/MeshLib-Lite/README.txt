MeshLib-Lite: Geometric Core
===========================

What is MeshLib-Lite?
--------------------
MeshLib-Lite is a custom, streamlined redistribution of MeshLib (https://github.com/MeshInspector/MeshLib). 
It is designed specifically for "making things as lite as possible" while retaining 100% 
functional parity for core geometric processing tasks.

Relation with MeshLib:
----------------------
MeshLib-Lite consists of the core geometric and topological source files from the official 
MeshLib repository, but with several architectural modifications:
1. Static Linking: Engineered to be compiled directly into a host application without 
   requiring external DLLs or heavy dependencies like TBB or OpenCV.
2. Stripped Dependencies: High-level modules like Scene Management, GUI, Visualization, 
   and complex IO filters have been removed.
3. Stubbed Subsystems: Advanced and computationally heavy features (like Laplacian 
   smoothing and Precise Predicates) are replaced with lightweight stubs to minimize 
   binary size and build complexity.
4. Header Consolidation: Include paths have been flattened to support simpler build 
   environments.

Feature Audit
-------------

### Core Geometry & Topology (Full Support)
*   Half-Edge Topology: Full bidirectional connectivity (MRMeshTopology). Walk edges, 
    find neighbors, and trace boundaries.
*   Vertex Identification: Native vertex-welding (MRIdentifyVertices), ensuring 
    meshes aren't "exploded" triangle soups.
*   Mesh Reconstruction: Build meshes from vertex/index buffers (MRMeshBuilder).

### Spatial Indexing & Queries (Full Support)
*   AABB Trees: Fast spatial lookups (MRAABBTree).
*   Raycasting: High-speed ray-mesh intersection tests (MRMeshIntersect).
*   Proximity: Finding all points/faces within a radius (MRPointsInBall).

### Mesh Repair & Conditioning (Full Support)
*   Hole Filling: Sophisticated hole-filling algorithms (MRMeshFillHole, 
    MRFillHoleNicely).
*   Manifold Fixer: Automatically fixing self-intersections and non-manifold edges 
    (MRMeshFixer).
*   Decimation: High-fidelity mesh simplification (MRMeshDecimate).
*   Subdivision: Smooth mesh refinement (MRMeshSubdivide).

### Analysis & Pathfinding (Full Support)
*   Metrics: Calculating areas, volumes, and bounding boxes (MRMeshMetrics, 
    MRComputeBoundingBox).
*   Shortest Path: Finding the shortest path between vertices on the mesh surface 
    (MREdgePaths).
*   Region Growth: Expanding and shrinking face/vertex selections (MRExpandShrink).

### Excluded Modules (Stubbed for Performance)
*   Boolean Operations: Union, Subtract, and Intersect operations.
*   Offsetting/Morphology: Creating shells or thickened versions of meshes.
*   Laplacian Smoothing: Advanced vertex relaxation.
*   Precise Predicates: Replaced with standard floating-point arithmetic.
*   Scene Graph: No support for Objects, Scenes, or Cameras.
