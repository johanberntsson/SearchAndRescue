# MEGA65 Voxel Engine & Drone Search & Rescue Game Summary

A technical overview and gameplay concept for a 1990s-style heightfield voxel flight simulator targeting the MEGA65 hardware.

---

## 1. Technical Architecture Summary

The core technical objective is a high-performance **320x200 256-color voxel engine** running at smooth, playable framerates (25–50 FPS) using a classic front-to-back heightfield ray-casting algorithm.

### Core Stack & Hardware Utilization

* **CPU Strategy (GS4510 @ 40.5 MHz):**
  * **C Language (`llvm-mos` / `CalypSi`):** Handles high-level game logic, flight physics/controls, mission states, map loading, and flight instruments.
  * **Assembly Language:** Implements the critical vertical column rendering inner loop, using 32-bit zero-page indirect pointers, base-page indexing, and fixed-point Digital Differential Analyzer (DDA) calculations.

* **Graphics & Display Mode:**
  * **VIC-IV Full Color Mode (FCM):** 320x200 resolution with 8-bit palette index precision per pixel referencing a 256-color palette.
  * **Perspective Division LUTs:** Fast column rendering supported by precomputed Look-Up Tables ($1/Z$ perspective scaling).
  * **Hardware Scaling Option:** Capability to render at 160x200 and enable VIC-IV horizontal hardware expansion to stretch output to 320x200 with zero CPU penalty if pitch/roll calculations require extra processing headroom.

* **Entities & Objects:**
  * **Software Billboards / Sprites:** 2D scaling sprites drawn into the backbuffer with $Z$-depth clipping against the voxel heightfield for world-anchored objects (survivors, campfires, supply crates, hazards).
  * **VIC-IV Hardware Sprites:** Utilized for high-priority elements like HUD overlays, crosshairs, and foreground rotor/aircraft effects (up to 16 full-color sprites per rasterline).

* **Dynamic Terrain Modification:**
  * Direct microsecond RAM updates to the 2D heightmap ($H$) and colormap ($C$) arrays allow real-time world alterations (e.g., sinking/submerged boats, landslides, ground fissures, collapsing bridges) with no additional rendering pipeline cost.

* **DMA Controller (DMAgic):**
  * Operates at 20–40 MB/s to clear backbuffers (sky gradients), perform double-buffering page flips, and execute memory transfers during VBLANK.

---

## 2. Gameplay & Mission Concept Summary

### Theme: Post-Earthquake Search & Rescue (SAR)

A peaceful, high-stakes simulator focusing on emergency response, precision low-altitude flight, navigation, and critical resource management across broken, dynamic terrain.

### Core Gameplay Loop

1. **Scout & Scan:** Fly low-altitude grid patterns across ruined urban zones, landslides, and remote mountain villages to locate survivors trapped under rubble.
2. **Resource & Flight Management:** Balance battery life, payload capacity, and radio line-of-sight signal strength (which degrades when dipping behind canyon walls or collapsed structures).
3. **Payload Delivery:** Maneuver into precise hover stances to deploy medical kits, structural beacons, or radio relays.
4. **Route Inspection:** Scan ruined infrastructure ahead of ground convoys (ambulances, rescue trucks) to assess bridge stability and chart safe paths.

### Key Features & Mechanics

* **Multi-Sensor HUD Vision Modes:**
  * **RGB Optical View:** Standard full-color terrain view.
  * **Thermal / FLIR Palette Swap:** Instant VIC-IV palette swap rendering cold terrain in dark tones while human heat signatures and fires glow in bright white/orange.
  * **Acoustic / Gas Overlays:** Specialized HUD modes visualizing tapping sounds beneath debris as radial pulse rings or identifying color-coded gas leaks.

* **Flight Controller On-Screen Display (OSD):** Authentic drone interface featuring an artificial horizon, battery voltage monitor, GPS coordinates, signal RSSI indicator, and satellite locks rendered over the voxel backbuffer.

* **Dynamic Aftershock Events:** Real-time terrain manipulation triggers mid-flight events—causing landslides, bridge collapses, or localized flooding that alter navigation routes dynamically.
