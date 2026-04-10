# StackSort

Inventory auto-sort mod for Kenshi that optimizes for maximum contiguous free space.

Replaces the vanilla sort button with a smarter packer. Click "Stacksort [N]" to cycle through target sizes — the mod pre-reserves an N-cell-tall (or N-cell-wide) empty strip so incoming items of a known size always have room. Switch between **H**eight-mode and **W**idth-mode with the H/W toggle button to reserve space for whichever orientation fits your use case. Results are precomputed on a background thread for instant response.

![StackSort demo](stacksort.gif)

## Features

- MAXRECTS/Skyline-based 2D bin packing, far superior to vanilla's greedy 3-pass placer
- Player-controlled target size: click Stacksort to cycle, back button to decrement
- **Dual-mode targeting**: H-mode tries to reserve a stack `N` high; W-mode reserves a strip on the right `N` wide. Toggle with the H/W button.
- Precomputed results on a worker thread — zero main-thread stutter
- Guaranteed empty rectangle via pre-reservation (not emergent)
- Automatic stack consolidation on first click (merges split stacks)
- Works on character backpacks, body inventories, and containers
- Right-click the sort button to revert to original item positions
- Multiple simultaneous inventories supported (up to 16)
- Results cached across inventory close/reopen for the same character
- Trader inventories excluded (vanilla sort preserved)
- Full [KenshiRotate](https://github.com/jacobericson/KenshiRotate) integration: evaluates both orientations when KenshiRotate is installed

## Requirements

- [RE_Kenshi](https://github.com/BFrizzleFoShizzle/RE_Kenshi) ([Nexus](https://www.nexusmods.com/kenshi/mods/847)) — includes KenshiLib
- Optional: [KenshiRotate](https://github.com/jacobericson/KenshiRotate) for rotation-aware sorting

## Installation

1. Download the latest release
2. Copy the `StackSort` folder into Kenshi's `mods\` directory
3. Launch Kenshi via the RE_Kenshi launcher

## Usage

Open any inventory. The vanilla "Arrange" button is replaced with four controls:

```
[ < ]  [ Stacksort [N] ]  [ H/W ]  [ > ]
```

- **Stacksort [N]** — Left click to see the top 5 scoring inventory arrangements in descending order. Right click to revert your inventory to its initial layout.
- **H/W** — toggles between Height-mode and Width-mode. The button caption shows the active dim. Toggling invalidates results and precomputes under the new dim; the first click after toggling uses a quick sync fallback while the worker catches up.
- **<** — back button, decrements N with symmetric skip logic.
- **>** — forward button, increments N with symmetric skip logic.

Target N=1 is the general-purpose sort — it packs items tightly and lets the largest empty rectangle emerge naturally. N=2+ pre-reserves a strip of that size along the active axis, useful when you know what size items you'll be picking up (e.g., W-mode with N=7 before buying katanas you plan to stack flat, or H-mode with N=4 if you want a tall column to stuff your wheat).

Right-click the Stacksort button at any time to revert items to their original positions.

If KenshiRotate is installed, items are automatically rotated to achieve better packing and rotated back when cycling. (Rotation is penalized due to visual clutter)

## Building from Source

Requires:
- Visual Studio 2010 v100 x64 toolset
- RE_Kenshi / KenshiLib headers and libraries
- Boost 1.60 headers

Link dependencies: `KenshiLib.lib`, `MyGUIEngine_x64.lib`

## How It Works

The plugin hooks five game functions via KenshiLib:

1. **`InventoryGUI::autoArrangeButton`** — Intercepts the sort button click. Routes to StackSort's H-cycling logic or falls through to vanilla for trader GUIs.
2. **`InventoryGUI::_NV_show`** — Detects inventory close. Aborts running worker jobs and cleans up UI state.
3. **`InventoryGUI::_NV_update`** — Detects inventory open (visible transition), snapshots items into the result cache, enqueues worker jobs, polls for completion, and initializes the StackSort UI.
4. **`Inventory::_sectionAddItemCallback`** — Detects item additions. Invalidates cached results and arms a debounced re-snapshot.
5. **`Inventory::_sectionRemoveItemCallback`** — Same as above for item removals.

### Packing Algorithm

The packer uses a 5-layer approach:

1. **Greedy packer** (MAXRECTS or Skyline) places items in a single pass
2. **LAHC search** (Late Acceptance Hill Climbing) explores item orderings to find better layouts
3. **Diversified moves** (swap, insert, rotate-flip, repair) perturb the ordering
4. **Multi-restart** (16 restarts first pass, 20 refinement) escapes local optima
5. **Post-packing grouping swap** exchanges same-footprint items across types to improve clustering
6. **Best-across-H** selects the best result at query time

For target>1, a pre-reservation scan carves out a strip at the grid edge (bottom in H-mode, right in W-mode) and packs items into the L-shaped complement. The empty strip is guaranteed by construction. Scoring uses a unified function with strict tier separation: items placed >> LER area >> target-met bonus >> fragmentation >> grouping.

W-mode is implemented via transposition: the inner packer only knows how to reserve at the bottom, so W-mode swaps the grid and item dimensions on the way in and swaps them back on the way out. All the scoring, LER, skyline, and search code stays dim-agnostic.

### Performance

All packing computation runs on 4 background worker threads. Opening an inventory triggers precomputation of every target value; clicking the sort button reads the cached result with zero main-thread work. If the worker hasn't finished yet (first click within ~1 second of opening), a quick synchronous fallback runs in under 1ms.

On a fully loaded 20x20 backpack (43-45 items, the largest inventories in the game), the worker sweep completes in **1.3-1.6 seconds** for the first pass and **2-3 seconds** including refinement, measured under real game load. Smaller inventories (8x10 body sections, 10x14 backpacks) finish proportionally faster.

Across a test corpus of 18 real Kenshi inventory configurations (200 seeds, both with and without KenshiRotate), the packer achieves 100% item placement, median free-space concentration of 1.0 (all empty space in one contiguous region), and zero stranded cells (no isolated interior waste).

### Tuning

Search parameters were tuned over 10 rounds using a standalone benchmark harness (`tests/harness/`) with automated ablation analysis. Each round evaluated multiple configurations across an 18-instance corpus at 100 seeds per configuration, with per-component score decomposition (LER, grouping, stranded cells, concentration) and bimodal basin-rate analysis for diversity-sensitive signals. The corpus covers sparse to 95%-full inventories across body, backpack, and container grid sizes.

#### First pass (16 restarts x 4000 iters)

| Parameter | Value | Alternatives tested | Result |
|---|---|---|---|
| LAHC history | 200 | 100, 500, 1000 | 200 is the sweet spot; 100 gains grouping but loses -4.6pp basin rate from weakened refinement; 500+ wastes budget on lateral moves |
| Plateau threshold | 1500 | 500, 750, 1000, 2000 | Monotonic: higher = better basin rate. 1500 gets +4.3pp over 1000 at 1.5x wall time; 2000 adds only +0.9pp more |
| Restarts | 16 | 2, 4, 8 | +17.9pp basin rate over 4 restarts; ~96ms first-pass cost hidden by worker thread |
| Grouping power | b^1.5 | b^1.25, b^1.75, b^2 | Local quality optimum; tested across 3 separate rounds |
| Repair move | enabled | disabled | Live feature: ~1.4k attempts/run, 33% LAHC acceptance rate |
| Pre-reservation | enabled | disabled | Mandatory for target>1; disabling drops quality to 0% on dense inventories |

#### Refinement (20 restarts x 8000 iters, conditional)

Refinement runs a second LAHC pass warm-started from the first-pass best ordering. It triggers conditionally to avoid wasting budget on inventories that are already well-packed.

| Parameter | Value | Alternatives tested | Result |
|---|---|---|---|
| LAHC history | 200 | 100, 400, 800, 1000 | Monotonic: shorter = better. 200 won on basin rate (+2.4pp over 1000), replace rate, score lift, and grouping lift |
| Restarts | 20 | 8, 24, 32, 64 | Monotonic but diminishing: 24 gets +6.4pp basin rate over 8; 64 adds only +1.8pp more past 32. 20 balances quality vs wall time |
| Plateau threshold | 2000 | 1000, 1500 | Higher plateau gives refinement more budget per restart to escape local optima on warm-started seeds |

#### Refinement trigger heuristic

Correlation analysis on `refine_always` data (every target refined regardless of quality) identified `num_items` as the strongest predictor of refinement value (r=0.67), far above density (r=0.17) or concentration (r=-0.03).

| Trigger condition | Purpose |
|---|---|
| `!allPlaced` | Always refine if items couldn't be placed |
| `perpendicularLerSide < gridExtent` | LER doesn't span full width (when structurally possible) |
| `concentration < 0.95` | Free space isn't well consolidated |
| `strandedCells > 0` | Any isolated interior waste cells |
| `numItems > 20` | High item count — first pass is search-starved |

The item-count gate was tuned at thresholds 20, 25, and 30. Threshold 20 matches baseline basin rate (-0.4pp, noise) while cutting trigger rate from 54% to 42% and saving ~100ms wall time. Threshold 30 loses -5.3pp basin rate from skipping 25-30 item inventories that genuinely benefit.

### Compatibility

The plugin hooks game functions at runtime and doesn't modify any game data, save files, or item templates. Compatible with saves made before installation and with other mods. KenshiRotate is detected at startup and used opportunistically — StackSort works fine without it.

### Removing the mod

Uninstall by removing the `StackSort` folder from Kenshi's `mods\` directory. No persistent state is stored outside the mod folder.

## License

GPL-3.0 - see [LICENSE](LICENSE) for details.
