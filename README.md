# Omni

Omni is an Unreal Engine navigation authoring plugin for creating enhanced point links and broad traversal coverage without manually placing large numbers of individual NavLinkProxy actors.

## Current development version

**Omni 0.39.0** targets **Unreal Engine 5.8.0 - 5.8.3**, Windows 64-bit.

This development build is a **dense-generation optimization pass**. Volume and Spline endpoint duplicate suppression now use a regeneration-local spatial bucket keyed from the accepted pair's Source endpoint. A new candidate only searches the 27 neighboring cells that can possibly contain an accepted Source endpoint within **Endpoint Merge Distance**, then runs the same exact Source and Target squared-distance tests used previously. Pairing order, merge tolerance, traversal classification, validation, diagnostics, and generated-link limits are unchanged.

The optimization is most relevant to dense Volume and Filled Area Spline setups, where the previous implementation could scan every already-accepted preview pair for every new candidate. **Last Generation Time**, added in 0.38, remains available under Omni Status so the same actor can be measured before and after the optimization.

Automatic editor regeneration also retains the 0.37 deferred/coalesced behavior: construction, property, and completed-move notifications queue one refresh for the next editor tick, while explicit **Regenerate Link(s)**, **Recheck Diagnostics**, navigation-build regeneration, component registration, Undo/Redo, and authoring actions remain immediate.

**Recheck Diagnostics** remains available on all three actor types to close the diagnose → fix → regenerate → verify loop.

See `Doc/Spatial-Duplicate-Merge-Optimization-Test-Plan.md` for the 0.39 performance regression plan, `Doc/Generation-Performance-Readout-Test-Plan.md` for the 0.38 timing pass, and `Doc/Deferred-Editor-Regeneration-Test-Plan.md` for the 0.37 coalescing pass.

### Built-in Mantle Traversal

Set **Traversal Action = Mantle** when a crossing should use Omni's built-in ledge traversal instead of a ballistic Jump or project-defined Custom action. Mantle is available on Point, Volume, and Spline and can also be selected independently for each direction through **Use Directional Actions**.

The built-in Mantle runtime targets `ACharacter`. When path following reaches the smart link, Omni temporarily switches Character Movement to Flying, moves the Character along a controlled three-stage swept path, restores the previous movement mode, and resumes path following. The path lifts first, traverses toward the ledge top, then settles onto the destination. Sweep movement is used so a blocked runtime path does not intentionally teleport through world geometry.

Before publishing a built-in Mantle link, Omni sweeps the configured traversal capsule along that same three-stage path and validates both traversal endpoints. The departure must provide credible capsule support plus short approach clearance behind the start, while the destination must provide landing support plus short inward recovery clearance. **Validate Mantle Path** is enabled by default. A blocked explicit Mantle direction is rejected, a blocked bidirectional Mantle can reduce to one-way, and a blocked Automatic Mantle candidate falls back to Jump so the existing ballistic validator can test an alternate route. Diagnostics preview rejected Mantle paths or invalid endpoints in red.

Automatic can classify a short upward ledge as Mantle when **Auto Use Mantle** is enabled, the rise is above **Auto Rise Threshold**, the rise is at or below **Auto Mantle Max Height**, the horizontal reach is at or below **Auto Mantle Max Reach**, and native validation reports missing support or an oversized step. **Auto Require Ledge Geometry** is enabled by default. Omni first requires a WorldStatic face that substantially opposes travel, then probes roughly one traversal-agent radius beyond that face toward the elevated destination. That top probe must find support near the destination height, the support must be within the configured Automatic ground-slope limit, and an upright traversal capsule must fit there. A dimensionally mantle-sized crossing that fails this ledge-geometry test falls back to Jump. Larger or longer unsupported upward crossings continue to use Jump. Level gaps remain Jump, and eligible short downward drops remain Native.

Mantle-specific controls are **Mantle Duration**, **Mantle Arc Height**, **Validate Mantle Path**, **Mantle Validation Segments**, **Mantle Landing Tolerance**, **Mantle Timeout**, and **Mantle Area Class**. Automatic also exposes **Auto Use Mantle**, **Auto Require Ledge Geometry**, **Auto Mantle Max Height**, and **Auto Mantle Max Reach**. Mantle validation uses the shared **Traversal Agent Radius** and **Traversal Agent Half Height**. `OmniNavArea_Mantle` is the default Mantle area with a travel-cost multiplier of 2.25 and a fixed entering cost of 60.

Built-in Mantle is intentionally animation-agnostic. Projects that need root motion, motion matching, gameplay ability integration, hand placement, bespoke collision logic, or authored mantle animations should continue to use **Custom** traversal.

See `Doc/Deferred-Editor-Regeneration-Test-Plan.md` for the 0.37 performance pass, `Doc/Context-Aware-Details-Test-Plan.md` for the 0.36 Details-polish pass, `Doc/Authoring-Consistency-Test-Plan.md` for the 0.35 authoring-consistency pass, `Doc/Recheck-Diagnostics-Test-Plan.md` for the 0.34 recheck pass, `Doc/Diagnostic-Failure-Focus-Test-Plan.md` for the 0.33 failure-focus pass, `Doc/Diagnostic-Blocker-Focus-Test-Plan.md` for the 0.32 blocker-focus pass, `Doc/Diagnostic-Blocker-Selection-Test-Plan.md` for the 0.31 blocker-selection pass, `Doc/Clearance-Blocker-Diagnostics-Test-Plan.md` for the 0.30 clearance-blocker pass, `Doc/Path-Blocker-Diagnostics-Test-Plan.md` for the 0.29 exact blocker pass, `Doc/Rejected-Traversal-Failure-Markers-Test-Plan.md` for the 0.28 failure-marker pass, `Doc/Rejected-Traversal-Labels-Test-Plan.md` for the 0.27 viewport-diagnostics pass, `Doc/Preview-Visual-Language-Test-Plan.md` for the 0.26 preview pass, `Doc/Traversal-Validation-Diagnostics-Test-Plan.md` for the 0.25 diagnostics pass, `Doc/Traversal-Departure-Validation-Test-Plan.md` for the 0.24 departure pass, `Doc/Traversal-Landing-Recovery-Test-Plan.md` for the 0.23 landing-recovery pass, `Doc/Traversal-Landing-Validation-Test-Plan.md` for the 0.22 landing-support pass, `Doc/Mantle-Traversal-Test-Plan.md` for runtime traversal coverage, `Doc/Mantle-Validation-Test-Plan.md` for generation-time path validation, and `Doc/Automatic-Mantle-Ledge-Geometry-Test-Plan.md` for the Automatic ledge-geometry pass. The 0.20 face-only regression plan remains in `Doc/Automatic-Mantle-Ledge-Test-Plan.md`.

### Custom Traversal Context

Custom traversal now supports explicit routing IDs so one project handler can distinguish different gameplay actions without guessing from coordinates. When Custom is used through the normal single Traversal Action workflow, set **Custom Traversal ID**. When **Use Directional Actions** is enabled, Custom directions instead expose **Source to Target Custom ID** and **Target to Source Custom ID**.

The Custom Traversal component now exposes **On Traversal Requested Detailed**. Its `FOmniCustomTraversalRequest` payload contains:

- **Request Id**
- **Pawn**
- **Omni Actor**
- **Traversal Id**
- **Direction**: Source to Target or Target to Source
- **Start**: the Pawn's actual location when the smart link was reached
- **Destination**: the runtime path-following destination
- **Source** and **Target**: the authored/projected Omni endpoints

This makes directional setups explicit. A Both Ways Custom link can remain one bidirectional smart link while still reporting one ID in the Source to Target direction and a different ID in the Target to Source direction.

The original compact **On Traversal Requested** event remains available for existing bindings. New work should prefer the detailed event when it needs routing or direction context.

Custom traversal also now reports **On Traversal Finished** with one of four reasons: **Completed**, **Timed Out**, **Cancelled**, or **No Listener**. **Get Custom Traversal Request** can retrieve the full payload for any active Request Id.

See `Doc/Custom-Traversal-Context-Test-Plan.md` for the dedicated test pass.

### Directional Traversal Actions

All three Omni authoring actors now expose **Use Directional Actions** under **Omni > Traversal**. It is off by default, so existing placed actors preserve their current behavior.

When disabled, use the normal **Traversal Action** property exactly as before. When enabled, Omni replaces that single choice with:

- **Source to Target Action**
- **Target to Source Action**

Each direction can independently use **Native Movement**, **Jump**, **Automatic**, **Custom**, or **Mantle**.

For example, a ledge can be authored as:

- Source to Target Action = **Custom** for a project-specific mantle
- Target to Source Action = **Native Movement** for a simple drop

If the two directions resolve to the same underlying traversal type, Omni keeps one Both Ways link. If they resolve differently, Omni emits two one-way links internally. This avoids unnecessary link duplication while still supporting asymmetric traversal.

When directional actions are enabled in the editor, Omni first copies the current Traversal Action into both directions. Turning the feature on therefore does not immediately change existing behavior.

For one-way Direction settings, the action for the unused direction is hidden.

The Status section reports the resulting Native, Jump, Mantle, and Custom link counts. Diagnostics adds **Directional Split Pair** on Point and **Directional Split Pairs** on Volume and Spline.

See `Doc/Directional-Traversal-Test-Plan.md` for the dedicated test pass.

### Custom Traversal

Set **Traversal Action = Custom** when Omni should provide navigation connectivity but your game should perform the physical movement. Custom is intended for animation-driven mantle, climb, teleport, launch pads, doors, ziplines, scripted animation, or other project-specific traversal.

Each Omni authoring actor exposes an internal **Custom Traversal** component. For new work, bind to **On Traversal Requested Detailed** in Blueprint or C++. The request includes the designer-authored Traversal ID, exact Source/Target direction, owning Omni actor, actual Pawn start, runtime destination, and authored/projected Source and Target endpoints. The original four-parameter **On Traversal Requested** event remains available for compact or existing bindings.

Move the Pawn using your own logic, then call **Complete Custom Traversal** on the same Custom Traversal component with the Request Id. Omni releases the smart link and normal path following resumes. **On Traversal Finished** reports whether that request Completed, Timed Out, was Cancelled by regeneration/cleanup, or was released because no request listener was bound.

**Custom Traversal Timeout** defaults to 5 seconds and prevents a forgotten completion call from holding path following forever. If no listener is bound when the AI reaches the link, Omni releases the link immediately rather than deadlocking the path follower.

Custom links are drawn as dashed purple traversal lines in the editor preview. Preview color communicates traversal type, while arrowheads communicate direction.

See `Doc/Custom-Traversal-Test-Plan.md` for the dedicated test pass.

### Preview Detail

All three Omni authoring actors now expose **Preview Detail** under **Omni > Visualization**:

- **Links Only** draws only the generated traversal links.
- **Samples + Links** is the default and also draws projected Source/Target NavMesh samples.
- **Diagnostics** adds rejected Jump and Mantle trajectories in red and labels each rejected direction with its first failing validation stage.

Jump path validation also requires a supported landing footprint at the destination, using the configured Traversal Agent Radius. Jump preview direction is exact across Point, Volume, and Spline. Target-to-Source jumps draw from Target toward Source, and Both Ways jumps draw both ballistic arcs when the two directions differ by height. Preview arc resolution follows **Jump Validation Segments**. Mantle preview uses the same controlled path helper as runtime movement and also respects the actual generated direction.

## Omni Point NavLink

Place an `OmniPointNavLink` when you need one explicit Source-to-Target connection rather than generated regional coverage. **Source Point** is cyan and **Target Point** is orange. Each point is projected to NavMesh, then the single crossing is processed by the full Omni traversal stack.

The normal Point setup controls are intentionally small:

- **Enabled**
- **Direction**: Both Ways, Source to Target, or Target to Source
- **Max Crossing Distance**
- **Traversal Action**: Native Movement, Jump, Automatic, Custom, or Mantle
- **Use Directional Actions**: optional per-direction override that exposes Source to Target Action and Target to Source Action

Point authoring does not expose Pairing Mode or Coverage Density because there is exactly one Source endpoint and one Target endpoint.

Under **Actions**:

- **Regenerate Link**
- **Swap Source and Target**
- **Reset Points**

Under **Status**, Omni reports **Generated Links**, **Native Links**, **Jump Links**, **Mantle Links**, **Custom Links**, **Source On NavMesh**, and **Target On NavMesh**. A Both Ways Point link can produce two internal one-way links when Automatic resolves its two directions to different traversal types.

With **Show Preview** enabled, **Preview Detail** controls how much is drawn. Samples + Links shows the projected Source and Target locations as cyan and orange points. Native traversal is drawn as a straight line, Jump traversal as a ballistic arc, and Mantle traversal as a compact controlled arc. Diagnostics additionally shows rejected jumps as red arcs. Color communicates traversal type: Native is green, Jump is yellow, Mantle is cyan, and Custom is purple. Arrowheads communicate direction, with an arrow at each end for Both Ways. Automatic Mantle fallbacks that successfully become Jump links receive a small orange midpoint marker.

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
   - **Mantle** uses generated `UNavLinkCustomComponent` smart links with built-in controlled Character movement for short ledges.
   - **Automatic** resolves each direction independently as Native Movement, Mantle, or Jump from physical support, rise/drop direction, and crossing geometry.
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
7. Choose **Traversal Action** under Omni > Traversal. Native Movement, Jump, Mantle, and Custom force one behavior. **Automatic** is the recommended mode to test when one actor covers a mixture of ordinary crossings, gaps, ledges, rises, and drops. Enable **Use Directional Actions** only when Source to Target and Target to Source intentionally need different behavior.
8. Check **Status**. A healthy actor reports `Ready`, the pairing mode, total generated links, and the resulting Native, Jump, Mantle, or Custom mix.
9. Keep **Show Preview** enabled while authoring to see valid sample points and the actual generated pairings when the actor is selected. Jump links use ballistic arcs and Mantle links use compact controlled arcs.

The normal setup controls are intentionally small:

- **Enabled**
- **Direction**: Both Ways, Source to Target, or Target to Source.
- **Pairing Mode**: Nearest or Matched Grid.
- **Coverage Density**: Sparse, Balanced, Dense, or Custom.
- **Custom Spacing**: only appears when Coverage Density is Custom.
- **Max Crossing Distance**: longest connection Omni is allowed to generate. Zero disables the limit.

The **Visualization** category contains **Show Preview** and **Preview Detail**. With the actor selected, cyan points are valid Source NavMesh samples and orange points are valid Target NavMesh samples. Native links are drawn as straight lines, Jump links as ballistic arcs, Mantle links as compact controlled arcs, and Custom links as dashed lines. Link color communicates traversal type: green for Native, yellow for Jump, cyan for Mantle, and purple for Custom. Arrowheads communicate direction, with arrows at both ends for Both Ways. **Diagnostics** preview detail additionally draws rejected Jump and Mantle directions in red.

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

Low-level counters remain under **Diagnostics** for troubleshooting. Jump and Mantle rejection counters now identify the first failed validation stage: **Departure Support**, **Departure Clearance**, **Path**, **Landing Support**, **Landing Clearance**, and **Invalid**. Jump also reports **Speed** separately. A rejected direction contributes to exactly one reason counter, which keeps totals readable and prevents one bad link from inflating several diagnostics at once.

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

Omni samples each spline by distance along the actual curve, projects those samples to NavMesh, and publishes the surviving pairings through the same Native, Jump, Mantle, Automatic, and Custom traversal stack used by Point and Volume. The number of spline control points does not determine link density. A long two-point spline can still generate many evenly spaced navigation samples.

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

With **Show Preview** enabled and the actor selected, **Preview Detail** controls clutter. Samples + Links shows cyan valid Source samples and orange valid Target samples. Native links are straight, Jump links are ballistic arcs, and Mantle links use compact controlled arcs. Green, yellow, cyan, and purple communicate Native, Jump, Mantle, and Custom respectively. Arrowheads communicate link direction. Diagnostics adds rejected Jump and Mantle paths in red.

Use **Reset Splines** to return both authoring splines to the default straight parallel layout.

See `Doc/Spline-NavLink-Test-Plan.md` for the dedicated spline test pass.

## Phase 0 test actor

`OmniTestNavLink` remains in the plugin only as an internal development baseline so existing test maps can still load it. It is now **NotPlaceable**, so it no longer appears as a normal actor designers can place. New authoring should use `OmniPointNavLink`, `OmniVolumeNavLink`, or `OmniSplineNavLink`.

## Traversal actions

All three authoring actors, `OmniPointNavLink`, `OmniVolumeNavLink`, and `OmniSplineNavLink`, expose **Traversal Action** under **Omni > Traversal**: **Native Movement**, **Jump**, **Automatic**, **Custom**, and **Mantle**. Enable **Use Directional Actions** when the two traversal directions should use different actions.

### Native Movement

**Native Movement** is the default and preserves Omni 0.8 behavior. Omni creates ordinary native point links and lets the controlled pawn's normal movement determine whether it can physically complete the crossing.

### Automatic

**Automatic** classifies each generated traversal direction independently. It probes the straight crossing for world-static ground support and, by default, validates whether that support actually looks walkable for the configured traversal agent.

A continuously supported crossing stays Native only when it also passes the enabled native-path checks. Short upward ledges can become Mantle when they fit the Automatic mantle limits. Other non-walkable directions fall back to Jump. Unsupported crossings use these rules:

- upward ledges above **Auto Rise Threshold** may use Mantle when they are within **Auto Mantle Max Height** and **Auto Mantle Max Reach**
- upward crossings outside the Mantle limits use Jump
- level gaps use Jump
- downward crossings within **Auto Native Drop Distance** stay Native drops
- longer unsupported drops use Jump

The short native-drop exception applies only when support disappears. Omni does not treat a steep surface, oversized step, or blocked capsule as a harmless drop.

For **Both Ways**, the two directions are classified independently. A high-to-low connection can therefore be a native drop while the return direction becomes a Mantle or Jump. Omni represents different traversal types as two one-way links internally and reports them as an **Automatic Split Pair**.

Automatic-specific controls are:

- **Validate Native Path**: enabled by default
- **Auto Max Step Height**: default 45 cm
- **Auto Max Ground Slope**: default 44 degrees
- **Auto Rise Threshold**: default 10 cm
- **Auto Use Mantle**: enabled by default
- **Auto Require Ledge Geometry**: enabled by default; requires an opposing face plus usable top support and capsule clearance
- **Auto Mantle Max Height**: default 150 cm
- **Auto Mantle Max Reach**: default 120 cm
- **Auto Native Drop Distance**: default 200 cm
- **Auto Ground Probe Depth**: advanced, default 75 cm
- **Auto Ground Probe Spacing**: advanced, default 75 cm
- **Traversal Agent Radius**: advanced, default 34 cm
- **Traversal Agent Half Height**: advanced, default 88 cm

Diagnostics report **Auto Mantle Promotions**, **Auto Mantle Validation Fallbacks**, **Auto Ledge Geometry Fallbacks**, **Auto Step Promotions**, **Auto Slope Promotions**, and **Auto Clearance Promotions**. Status summarizes these classifications and promotions when they occur.

Automatic remains opt-in so existing actors retain their previous traversal choice.

### Mantle

**Mantle** converts generated crossings into custom navigation links with built-in controlled `ACharacter` movement. It is intended for short ledges that are not ordinary walkable steps but do not need a full ballistic Jump.

When path following reaches a Mantle link, Omni:

1. Finds the controlled `ACharacter` and records its current Character Movement mode.
2. Converts the NavMesh destination to the Character capsule-center height.
3. Temporarily switches Character Movement to Flying and stops existing velocity.
4. Moves the Character over **Mantle Duration** along a three-stage swept path controlled by **Mantle Arc Height**.
5. Restores the previous movement mode and resumes normal path following when the destination is reached.
6. Releases the traversal without forcing a blocked teleport if **Mantle Timeout** is reached.

Mantle-specific controls are:

- **Mantle Duration**: default 0.45 seconds.
- **Mantle Arc Height**: default 35 cm above the higher endpoint.
- **Validate Mantle Path**: enabled by default. Sweeps the traversal capsule along the built-in Mantle path before publishing the direction.
- **Mantle Validation Segments**: advanced, default 12. Controls sweep resolution from 6 to 64 segments.
- **Traversal Agent Radius / Half Height**: advanced shared capsule dimensions used by Automatic native validation, Jump validation, and Mantle validation.
- **Mantle Landing Tolerance**: advanced, default 20 cm.
- **Mantle Timeout**: advanced, default 2 seconds.
- **Mantle Area Class**: advanced, defaults to `OmniNavArea_Mantle`.

`OmniNavArea_Mantle` defaults to a travel-cost multiplier of 2.25 and a fixed entering cost of 60. This makes Mantle more expensive than ordinary walking by default while still allowing the pathfinder to use it when appropriate.

The editor preview draws Mantle as a compact controlled arc using the same path helper as runtime movement. Mantle previews are cyan and use arrowheads to communicate direction.

The built-in Mantle implementation is animation-agnostic and targets `ACharacter` with normal world-Z orientation. Use **Custom** when the project needs animation-driven or game-specific mantle behavior. Generation-time validation checks WorldStatic geometry, while runtime swept movement and timeout still protect against dynamic blockers or runtime-only changes.

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

- **Jump Apex Height**: visible whenever an active traversal direction uses Jump or Automatic. Default 150 cm.
- **Validate Jump Path**: on by default. Rejects blocked or excessively fast jump directions.
- **Max Jump Horizontal Speed**: advanced. Default 1600 cm/s. Zero disables the speed limit.
- **Traversal Agent Radius**: advanced. Default 34 cm. Shared by Automatic native-path, Jump, and Mantle clearance validation.
- **Traversal Agent Half Height**: advanced. Default 88 cm. Shared by Automatic native-path, Jump, and Mantle clearance validation.
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

Custom uses the actor's normal **Area Class** and **Supported Agents** settings. In the editor preview, Custom links are dashed purple lines and use the same arrowhead direction language as Native, Jump, and Mantle links.

The Status category now separates:

- **Generated Links**: total links produced by the actor.
- **Native Links**: ordinary point links.
- **Jump Links**: smart links using Omni's built-in Jump traversal.
- **Mantle Links**: smart links using Omni's built-in Mantle traversal.
- **Custom Links**: smart links whose physical movement is supplied by Blueprint or C++.

## Height-aware traversal filtering

All three authoring actors also expose:

- **Use Height Limits**: off by default for backward compatibility.
- **Max Rise**: maximum upward Z change permitted in a traversal direction. Default 100 cm.
- **Max Drop**: maximum downward Z change permitted in a traversal direction. Default 300 cm.

The limits are directional. A pair that drops 250 cm from Source to Target rises 250 cm from Target to Source. With Max Rise 100 and Max Drop 300, Direction = Both Ways produces a Source to Target one-way link rather than advertising a return path the limits do not allow.

Height filtering applies to Native Movement, Jump, Mantle, Automatic, and Custom. Pair search is height-aware, so Nearest and Matched Grid try another valid sample when the geometrically preferred match violates Max Crossing Distance or the active height limits.

Viewport preview colors communicate the actual generated traversal type:

- Green: Native
- Yellow: Jump
- Cyan: Mantle
- Purple: Custom
- Red: Rejected traversal candidate in Diagnostics

Arrowheads communicate direction. One arrowhead marks a one-way traversal, while Both Ways displays an arrowhead at each end. Automatic Mantle fallbacks that successfully resolve to Jump also receive a small orange midpoint marker.

Status reports when height limits reduce otherwise bidirectional pairs to one-way links. Diagnostics includes **Height Limited To One Way**.

See `Doc/Mantle-Traversal-Test-Plan.md`, `Doc/Jump-Traversal-Test-Plan.md`, `Doc/Automatic-Traversal-Test-Plan.md`, and `Doc/Jump-Validation-Test-Plan.md` for the current traversal test passes.

## Architecture

Omni deliberately uses ordinary native point links for its production foundation. Unreal Engine 5.8 contains segment-link code, but Epic's own engine source describes segment links as unsupported and not completed to production quality.

No engine source modifications are required.

### Diagnostic Blocker Actions

Point, Volume, and Spline Omni actors expose two blocker actions under **Omni | Actions**:

- **Select Diagnostic Blockers** gathers the unique actors currently identified by rejected Jump and Mantle diagnostics and selects them in the Level Editor without moving the camera.
- **Focus Diagnostic Blockers** selects the same blocker set and frames the complete set in the active Level Editor viewport. Other viewports are left unchanged.

These actions are especially useful with Volume and Spline generation where several rejected crossings can point at different pieces of level geometry. Both use diagnostics already produced during generation and do not perform additional collision queries or change link validity.

If the current rejected diagnostics do not identify a blocking actor, such as missing support or speed-only failures, there is nothing to select or focus and the current viewport is left alone.
