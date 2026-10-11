# Omni

Omni is an Unreal Engine navigation authoring plugin for creating enhanced point links and broad traversal coverage without manually placing large numbers of individual NavLinkProxy actors.

## Current development version

**Omni 0.15.0** targets **Unreal Engine 5.8.0 - 5.8.3**, Windows 64-bit.

This development build adds designer-defined **Custom** smart-link traversal to Point, Volume, and Spline while keeping the shared traversal core introduced in 0.14. All three authoring types now expose Native Movement, Jump, Automatic, and Custom traversal through the same generated-link pipeline.

### Custom Traversal

Set **Traversal Action = Custom** when Omni should provide navigation connectivity but your game should perform the physical movement. Custom is intended for mantle, climb, teleport, launch pads, doors, ziplines, scripted animation, or other project-specific traversal.

Each Omni authoring actor exposes an internal **Custom Traversal** component. Bind to **On Traversal Requested** in Blueprint or C++. The event provides:

- **Request Id**
- **Pawn**
- **Start**
- **Destination**

Move the Pawn using your own logic, then call **Complete Custom Traversal** on the same Custom Traversal component with the Request Id. Omni then releases the smart link and normal path following resumes.

**Custom Traversal Timeout** defaults to 5 seconds and prevents a forgotten completion call from holding path following forever. If no listener is bound when the AI reaches the link, Omni releases the link immediately rather than deadlocking the path follower.

Custom links are drawn as dashed traversal lines in the editor preview. Direction colors remain the normal Omni green, yellow, and purple convention.

See `Doc/Custom-Traversal-Test-Plan.md` for the dedicated test pass.

### Preview Detail

All three Omni authoring actors now expose **Preview Detail** under **Omni > Visualization**:

- **Links Only** draws only the generated traversal links.
- **Samples + Links** is the default and also draws projected Source/Target NavMesh samples.
- **Diagnostics** adds rejected Jump trajectories as red arcs.

Jump preview direction is now exact across Point, Volume, and Spline. Target-to-Source jumps draw from Target toward Source, and Both Ways jumps draw both ballistic arcs when the two directions differ by height. Preview arc resolution also follows **Jump Validation Segments** instead of using a separate hard-coded segment count.

## Omni Point NavLink

Place an `OmniPointNavLink` when you need one explicit Source-to-Target connection rather than generated regional coverage. **Source Point** is cyan and **Target Point** is orange. Each point is projected to NavMesh, then the single crossing is processed by the full Omni traversal stack.

The normal Point setup controls are intentionally small:

- **Enabled**
- **Direction**: Both Ways, Source to Target, or Target to Source
- **Max Crossing Distance**
- **Traversal Action**: Native Movement, Jump, Automatic, or Custom

Point authoring does not expose Pairing Mode or Coverage Density because there is exactly one Source endpoint and one Target endpoint.

Under **Actions**:

- **Regenerate Link**
- **Swap Source and Target**
- **Reset Points**

Under **Status**, Omni reports **Generated Links**, **Native Links**, **Jump Links**, **Custom Links**, **Source On NavMesh**, and **Target On NavMesh**. A Both Ways Point link can produce two internal one-way links when Automatic resolves one direction as Native and the reverse direction as Jump.

With **Show Preview** enabled, **Preview Detail** controls how much is drawn. Samples + Links shows the projected Source and Target locations as cyan and orange points. Native traversal is drawn as a straight line and Jump traversal as a ballistic arc. Diagnostics additionally shows rejected jumps as red arcs. Green, yellow, and purple retain the same Both Ways, Source to Target, and Target to Source meaning used by Volume and Spline.

The advanced projection, Nav Area, supported-agent, Automatic validation, Jump validation, height-limit, and path-cost controls match the corresponding Omni Volume and Spline behavior.

See `Doc/Point-NavLink-Test-Plan.md` for the dedicated Point test pass.

## Omni Volume NavLink

Place an `OmniVolumeNavLink` and position its **Source Box** and **Target Box** over two navigable regions that should be connected.

Omni:

1. Treats each box's local X/Y footprint as a 2D authoring region.
2. Builds a grid of candidate points across each volume using the selected **Coverage Density**.
3. Projects every grid point to Unreal navigation data.
4. Rejects projections that fail, move too far, or leave their authoring volume.
5. Pairs valid samples according to **Pairing Mode**.
   - **Nearest** pairs samples by nearest projected world position from both sides, then deduplicates the result.
   - **Matched Grid** pairs normalized local grid positions one-to-one. If an exact corresponding sample is unavailable, it uses the nearest unused valid normalized sample as a local fallback.
6. Rejects links beyond **Max Crossing Distance**.
7. Deduplicates nearly identical endpoint pairs.
8. Publishes the surviving connections according to **Traversal Action**.
   - **Native Movement** uses ordinary `FNavigationLink` entries through a `UNavLinkComponent`.
   - **Jump** uses generated `UNavLinkCustomComponent` smart links with built-in Character launch behavior.
   - **Automatic** resolves each direction independently as Native Movement or Jump from physical support, rise/drop direction, and crossing distance.
   - **Custom** uses generated smart links that pause path following and fire the Custom Traversal request event for Blueprint or C++ movement.

The generated links are internal data. Omni does not spawn generated NavLinkProxy actors in the Outliner.

### Why the grid is horizontal

The useful navigation surface is normally the NavMesh under the volume, so Omni samples the box footprint rather than stacking samples vertically. Multiple Z samples would usually project onto the same walkable polygons and create duplicate links. The box's Z extent still matters because projected points must remain inside the authoring volume.

### Normal workflow

1. Place `OmniVolumeNavLink`.
2. Select **Source Box** and move/resize it over the first navigable region. Source is drawn cyan.
3. Select **Target Box** and move/resize it over the region it should connect to. Target is drawn orange.
4. Choose **Direction** using Source/Target terminology.
5. Choose **Pairing Mode**. Nearest is the recommended default.
6. Choose **Coverage Density**. Balanced is the recommended default.
7. Choose **Traversal Action** under Omni > Traversal. Native Movement and Jump force one behavior. **Automatic** is the recommended mode to test when one actor covers a mixture of ordinary crossings, gaps, rises, and drops.
8. Check **Status**. A healthy actor reports `Ready`, the pairing mode, total generated links, and Jump details when enabled.
9. Keep **Show Preview** enabled while authoring to see valid sample points and the actual generated pairings when the actor is selected. Jump links are drawn as arcs.

The normal setup controls are intentionally small:

- **Enabled**
- **Direction**: Both Ways, Source to Target, or Target to Source.
- **Pairing Mode**: Nearest or Matched Grid.
- **Coverage Density**: Sparse, Balanced, Dense, or Custom.
- **Custom Spacing**: only appears when Coverage Density is Custom.
- **Max Crossing Distance**: longest connection Omni is allowed to generate. Zero disables the limit.

The **Visualization** category contains **Show Preview** and **Show Rejected Jumps**. With the actor selected, cyan points are valid Source NavMesh samples and orange points are valid Target NavMesh samples. Native links are drawn as straight lines. Jump links are drawn as arcs. Link color still communicates direction: green for Both Ways, yellow for Source to Target, and purple for Target to Source. Rejected Jump directions are drawn as red arcs.

The **Actions** category provides:

- **Regenerate Links**
- **Swap Source and Target**
- **Match Target Size to Source**
- **Reset Volumes**

Projection, native navigation, safety-cap, and optimization settings remain available under **Advanced**, but should not be necessary for ordinary placement.

### Status and diagnostics

The Status category now reports plain-English feedback such as:

- Ready and generated link count
- no NavMesh found inside Source Box
- no NavMesh found inside Target Box
- volumes exceeding Max Crossing Distance
- generated-link cap reached
- grid safety cap reached

Low-level counters remain under **Diagnostics** for troubleshooting.

### Diagnostic counters

When deeper troubleshooting is needed, Diagnostics reports:

- Generated Link Count
- Source Grid Candidate Count
- Target Grid Candidate Count
- Valid Source Sample Count
- Valid Target Sample Count
- Candidate Count
- rejected source projections
- rejected target projections
- rejected out-of-volume projections
- rejected links exceeding Maximum Link Distance
- merged duplicates

Use **Regenerate Links** for an explicit refresh while testing.


## Omni Spline NavLink

Place an `OmniSplineNavLink` and edit its **Source Spline** and **Target Spline** directly in the viewport. Source is cyan and Target is orange.

Omni samples each spline by distance along the actual curve, projects those samples to NavMesh, and publishes the surviving pairings as native `FNavigationLink` entries. The number of spline control points does not determine link density. A long two-point spline can still generate many evenly spaced navigation samples.

### Spline pairing

- **Nearest** pairs valid projected samples by world-space distance and is the recommended default.
- **Matched Grid** maps normalized progress along one spline to normalized progress along the other. For example, 25% along Source attempts to match 25% along Target. This remains useful when the splines have different physical lengths or shapes.

Both **open** and **closed** splines are supported. **Closed Spline Coverage** chooses how a closed spline contributes samples:

- **Perimeter** is the default and samples along the closed curve. Matched Grid uses normalized progress around the loop and wraps across the 0/1 seam.
- **Filled Area** treats the closed spline as an irregular 2D authoring region. Omni samples the actual curved boundary, fills the enclosed local XY region with a density-driven grid, projects those interior samples to NavMesh, and rejects projections that leave the authored polygon. When both Source and Target are Filled Areas, Matched Grid uses one-to-one normalized 2D region correspondence.

Mixed open/closed pairs remain supported. Nearest works normally. If Matched Grid is selected while one side is a Filled Area and the other side is a path/perimeter, Omni falls back to Nearest and reports that fallback in Status rather than inventing a misleading 1D-to-2D mapping.

The normal Spline setup controls are:

- **Enabled**
- **Direction**: Both Ways, Source to Target, or Target to Source
- **Pairing Mode**: Nearest or Matched Grid
- **Closed Spline Coverage**: Perimeter or Filled Area
- **Coverage Density**: Sparse, Balanced, Dense, or Custom
- **Custom Spacing**: only appears when Coverage Density is Custom
- **Max Crossing Distance**

With **Show Preview** enabled and the actor selected, **Preview Detail** controls clutter. Samples + Links shows cyan valid Source samples and orange valid Target samples. Native links are straight and Jump links are ballistic arcs. Green, yellow, and purple communicate Both Ways, Source to Target, and Target to Source respectively. Diagnostics adds rejected Jump arcs in red.

Use **Reset Splines** to return both authoring splines to the default straight parallel layout.

See `Doc/Spline-NavLink-Test-Plan.md` for the dedicated spline test pass.

## Phase 0 test actor

`OmniTestNavLink` remains in the plugin only as an internal development baseline so existing test maps can still load it. It is now **NotPlaceable**, so it no longer appears as a normal actor designers can place. New authoring should use `OmniPointNavLink`, `OmniVolumeNavLink`, or `OmniSplineNavLink`.

## Traversal actions

All three authoring actors, `OmniPointNavLink`, `OmniVolumeNavLink`, and `OmniSplineNavLink`, expose **Traversal Action** under **Omni > Traversal**: **Native Movement**, **Jump**, **Automatic**, and **Custom**.

### Native Movement

**Native Movement** is the default and preserves Omni 0.8 behavior. Omni creates ordinary native point links and lets the controlled pawn's normal movement determine whether it can physically complete the crossing.

### Automatic

**Automatic** classifies each generated traversal direction independently. It probes the straight crossing for world-static ground support and, by default, validates whether that support actually looks walkable for the configured traversal agent.

A continuously supported crossing stays Native only when it also passes the enabled native-path checks. Omni can promote a direction to Jump when it finds an oversized step, a surface steeper than **Auto Max Ground Slope**, or blocked upright capsule clearance. Unsupported crossings continue to use these rules:

- upward crossings above **Auto Rise Threshold** use Jump
- level gaps use Jump
- downward crossings within **Auto Native Drop Distance** stay Native drops
- longer unsupported drops use Jump

The short native-drop exception applies only when support disappears. Omni does not treat a steep surface, oversized step, or blocked capsule as a harmless drop.

For **Both Ways**, the two directions are classified independently. A high-to-low connection can therefore be a native drop while the return direction becomes a Jump. Omni represents that as two one-way links internally and reports it as an **Automatic Split Pair**.

Automatic-specific controls are:

- **Validate Native Path**: enabled by default
- **Auto Max Step Height**: default 45 cm
- **Auto Max Ground Slope**: default 44 degrees
- **Auto Rise Threshold**: default 10 cm
- **Auto Native Drop Distance**: default 200 cm
- **Auto Ground Probe Depth**: advanced, default 75 cm
- **Auto Ground Probe Spacing**: advanced, default 75 cm
- **Traversal Agent Radius**: advanced, default 34 cm
- **Traversal Agent Half Height**: advanced, default 88 cm

Diagnostics report **Auto Step Promotions**, **Auto Slope Promotions**, and **Auto Clearance Promotions**. Status summarizes these promotions when they occur.

Automatic remains opt-in so existing actors retain their previous traversal choice.

### Jump

**Jump** converts generated crossings into custom navigation links. When normal Unreal path following reaches one of those links, Omni:

1. Finds the AIController and its controlled `ACharacter`.
2. Uses the Character Movement component's standard world-Z gravity magnitude.
3. Calculates a ballistic launch velocity from the Character's current position to the custom-link destination.
4. Adds **Jump Apex Height** above the higher endpoint to control the shape of the arc.
5. Calls `LaunchCharacter`.
6. Holds normal path-following steering while the custom link is active.
7. Returns control to path following when the Character reaches the landing region, or when **Jump Timeout** expires as a safety fallback.

Jump-specific controls are:

- **Jump Apex Height**: visible whenever Traversal Action is Jump or Automatic. Default 150 cm.
- **Validate Jump Path**: on by default. Rejects blocked or excessively fast jump directions.
- **Max Jump Horizontal Speed**: advanced. Default 1600 cm/s. Zero disables the speed limit.
- **Traversal Agent Radius**: advanced. Default 34 cm. Shared by Automatic native-path validation and Jump clearance validation.
- **Traversal Agent Half Height**: advanced. Default 88 cm. Shared by Automatic native-path validation and Jump clearance validation.
- **Jump Validation Segments**: advanced. Default 16.
- **Jump Area Class**: advanced. Defaults to `OmniNavArea_Jump`.
- **Jump Landing Tolerance**: advanced. Default 75 cm.
- **Jump Timeout**: advanced. Default 3 seconds.

The built-in Jump traversal currently targets `ACharacter` using `UCharacterMovementComponent` and standard world-Z gravity. Runtime launch now aims the Character capsule center at the destination capsule-center height rather than aiming the actor center at the NavMesh feet point. A non-Character pawn is released from the custom link immediately rather than being held forever, but Omni does not provide a physical jump implementation for that pawn type in this version. Custom gravity directions are not handled by the current Jump implementation.

Jump validation uses a standard 980 cm/s^2 world-Z gravity model during generation, plus the configured agent radius and half height. If a Both Ways jump is clear in only one direction, Omni keeps the valid one-way jump and reports **Jump Limited To One Way**. Blocked or over-speed directions are counted under Diagnostics and can be previewed as red arcs.

`OmniNavArea_Jump` defaults to a travel-cost multiplier of 2.0 and a fixed entering cost of 50. This gives A* a reason to prefer a normal ground route while still allowing Jump links when they are materially better or required. You can replace **Jump Area Class** with another Nav Area for project-specific path costs.

### Custom

**Custom** converts generated crossings into smart links but does not move the Pawn itself. When path following reaches the link, Omni pauses normal path following and broadcasts **On Traversal Requested** from the actor's **Custom Traversal** component.

The event provides **Request Id**, **Pawn**, **Start**, and **Destination**. Your Blueprint or C++ code performs whatever traversal the project needs, then calls **Complete Custom Traversal** with the Request Id. This keeps navigation ownership and physical movement cleanly separated.

**Custom Traversal Timeout** defaults to 5 seconds. If the completion call is missed, Omni eventually releases path following as a safety fallback. If there is no listener bound at all, Omni releases immediately instead of leaving the AI stuck.

Custom uses the actor's normal **Area Class** and **Supported Agents** settings. In the editor preview, Custom links are dashed while preserving the same direction colors used by Native and Jump links.

The Status category now separates:

- **Generated Links**: total links produced by the actor.
- **Native Links**: ordinary point links.
- **Jump Links**: custom smart links using Omni's built-in Jump traversal.
- **Custom Links**: smart links whose physical movement is supplied by Blueprint or C++.

## Height-aware traversal filtering

All three authoring actors also expose:

- **Use Height Limits**: off by default for backward compatibility.
- **Max Rise**: maximum upward Z change permitted in a traversal direction. Default 100 cm.
- **Max Drop**: maximum downward Z change permitted in a traversal direction. Default 300 cm.

The limits are directional. A pair that drops 250 cm from Source to Target rises 250 cm from Target to Source. With Max Rise 100 and Max Drop 300, Direction = Both Ways produces a Source to Target one-way link rather than advertising a return path the limits do not allow.

Height filtering applies to Native Movement, Jump, Automatic, and Custom. Pair search is height-aware, so Nearest and Matched Grid try another valid sample when the geometrically preferred match violates Max Crossing Distance or the active height limits.

Viewport preview link colors communicate the actual generated direction:

- Green: Both Ways
- Yellow: Source to Target
- Purple: Target to Source

Status reports when height limits reduce otherwise bidirectional pairs to one-way links. Diagnostics includes **Height Limited To One Way**.

See `Doc/Jump-Traversal-Test-Plan.md`, `Doc/Automatic-Traversal-Test-Plan.md`, and `Doc/Jump-Validation-Test-Plan.md` for the current traversal test passes.

## Architecture

Omni deliberately uses ordinary native point links for its production foundation. Unreal Engine 5.8 contains segment-link code, but Epic's own engine source describes segment links as unsupported and not completed to production quality.

No engine source modifications are required.
