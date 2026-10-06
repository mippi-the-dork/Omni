# Omni

Omni is an Unreal Engine navigation authoring plugin for creating broad traversal coverage without manually placing large numbers of individual NavLinkProxy actors.

## Current development version

**Omni 0.3.0** targets **Unreal Engine 5.8.0 - 5.8.3**, Windows 64-bit.

This development build upgrades the Volume-to-Volume actor from line sampling to true 2D volume-footprint coverage while retaining the Phase 0 test actor as a known-good baseline.

## Omni Volume NavLink

Place an `OmniVolumeNavLink` and position its **Source Box** and **Target Box** over two navigable regions that should be connected.

Omni:

1. Treats each box's local X/Y footprint as a 2D authoring region.
2. Builds a grid of candidate points across each volume using **Link Spacing**.
3. Projects every grid point to Unreal navigation data.
4. Rejects projections that fail, move too far, or leave their authoring volume.
5. Pairs every valid Source sample with its nearest valid Target sample.
6. Performs the reverse Target-to-Source pairing so differently sized volumes retain coverage on both sides.
7. Rejects links beyond **Maximum Link Distance**.
8. Deduplicates nearly identical endpoint pairs.
9. Publishes the surviving connections as ordinary native `FNavigationLink` entries through a `UNavLinkComponent`.

The generated links are internal data. Omni does not spawn generated NavLinkProxy actors in the Outliner.

### Why the grid is horizontal

The useful navigation surface is normally the NavMesh under the volume, so Omni samples the box footprint rather than stacking samples vertically. Multiple Z samples would usually project onto the same walkable polygons and create duplicate links. The box's Z extent still matters because projected points must remain inside the authoring volume.

### Main settings

- **Enabled**: Enables or removes the generated links.
- **Link Spacing**: Approximate world-space spacing between grid candidates in each volume.
- **Maximum Link Distance**: Rejects links longer than the configured distance. Zero disables the limit.
- **Projection Extent**: NavMesh projection search extent around each candidate.
- **Projection Containment Tolerance**: Allows a small amount of projection movement beyond the exact authoring box boundary.
- **Maximum Projection Distance**: Rejects projections that move too far from the raw grid point. Zero disables the limit.
- **Direction**: Both Ways, Left to Right, or Right to Left.
- **Area Class**: Native navigation area assigned to generated links.
- **Supported Agents**: Native Unreal nav-agent mask.
- **Endpoint Merge Distance**: Merges nearly identical generated links.
- **Maximum Grid Samples Per Volume**: Safety cap for projected grid candidates gathered from each box.
- **Maximum Generated Links**: Safety cap for native links published by one Omni actor.
- **Regenerate After Navigation Build**: Re-evaluates links after Unreal finishes navigation generation.

### Debug counters

The Details panel reports:

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

`OmniTestNavLink` remains included for development comparison. It creates a fixed number of evenly distributed native links between two boxes and was used to prove that normal Unreal pathfinding consumes Omni-generated `FNavigationLink` data.

## Important traversal note

Omni 0.3.0 creates navigation connectivity. It does not yet provide a jump, mantle, climb, teleport, or other physical traversal implementation.

A normal Character can cross links that its movement can physically complete. For example, a small same-height gap may work directly, while a low-to-high connection can be pathfindable but still require a future jump or mantle traversal layer.

## Architecture

Omni deliberately uses ordinary native point links for its production foundation. Unreal Engine 5.8 contains segment-link code, but Epic's own engine source describes segment links as unsupported and not completed to production quality.

No engine source modifications are required.
