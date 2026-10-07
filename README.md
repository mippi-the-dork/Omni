# Omni

Omni is an Unreal Engine navigation authoring plugin for creating broad traversal coverage without manually placing large numbers of individual NavLinkProxy actors.

## Current development version

**Omni 0.5.0** targets **Unreal Engine 5.8.0 - 5.8.3**, Windows 64-bit.

This development build adds explicit pairing behavior and live authoring visualization. **Nearest** remains the default general-purpose solver. **Matched Grid** attempts one-to-one correspondence between normalized Source and Target grid positions. When the actor is selected, Omni previews valid Source samples, valid Target samples, and the actual generated pairings directly in the viewport.

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
8. Publishes the surviving connections as ordinary native `FNavigationLink` entries through a `UNavLinkComponent`.

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
7. Check **Status**. A healthy actor reports `Ready`, the pairing mode, and its generated link count.
8. Keep **Show Preview** enabled while authoring to see valid sample points and the actual generated pairings when the actor is selected.

The normal setup controls are intentionally small:

- **Enabled**
- **Direction**: Both Ways, Source to Target, or Target to Source.
- **Pairing Mode**: Nearest or Matched Grid.
- **Coverage Density**: Sparse, Balanced, Dense, or Custom.
- **Custom Spacing**: only appears when Coverage Density is Custom.
- **Max Crossing Distance**: longest connection Omni is allowed to generate. Zero disables the limit.

The **Visualization** category contains **Show Preview**. With the actor selected, cyan points are valid Source NavMesh samples, orange points are valid Target NavMesh samples, and green lines are the actual generated native link pairings.

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

## Phase 0 test actor

`OmniTestNavLink` remains in the plugin only as an internal development baseline so existing test maps can still load it. It is now **NotPlaceable**, so it no longer appears as a normal actor designers can place. New authoring should use `OmniVolumeNavLink`.

## Important traversal note

Omni 0.5.0 creates navigation connectivity. It does not yet provide a jump, mantle, climb, teleport, or other physical traversal implementation.

A normal Character can cross links that its movement can physically complete. For example, a small same-height gap may work directly, while a low-to-high connection can be pathfindable but still require a future jump or mantle traversal layer.

## Architecture

Omni deliberately uses ordinary native point links for its production foundation. Unreal Engine 5.8 contains segment-link code, but Epic's own engine source describes segment links as unsupported and not completed to production quality.

No engine source modifications are required.
