# Raster split: giving the panel its own geometry

`raster-split.patch` is a working two-interrupt raster split that swaps the
VIC-IV's text geometry part way down the screen — 20 stretched characters for
the 3D view, 40 real ones for the panel. It is kept because the *interrupt*
half of it works and was expensive to get right, not because the split does.

**Read it, do not apply it.** It is a diff against `14f5080`, before the
framebuffer went to 320 pixels and the panel to 40 columns, so it will not go
onto HEAD cleanly. The parts worth having are `src/irq_asm.s` whole, and
`irq_install()` in `src/vic4.c`.

## What it proves

The interrupts fire exactly where they are asked to: logging the raster inside
the handler gave `[48, 204, 48, 204, …]` all the way down.

The split still does nothing, because **the VIC-IV latches `SCRNPTR`,
`LINESTEP`, `CHRCOUNT` and `CHRXSCL` once a frame**. Writing them mid-screen
changes nothing until the next frame begins, so whichever half was written last
wins the whole of the following one. The clincher: swapping the two `CHRXSCL`
values made the *entire* display change together, both halves, rather than
just the top. Only per-pixel registers such as the border colour answer
mid-frame.

That is why the 40-column panel was bought with a 320-pixel framebuffer
instead, at about 7% of the frame. See Performance in `CLAUDE.md`.

## Why it is kept

1. If a real MEGA65 honours a mid-frame `CHRXSCL` where xemu does not, the
   split becomes nearly free and that 7% could be handed back. Frame-latching
   looks like deliberate VIC-IV design — it is why the machine has a Raster
   Rewrite Buffer for per-row differences — so this is not the way to bet, but
   it has never been tried on hardware.
2. `src/irq_asm.s` is a correct MEGA65 raster interrupt, and the game will
   want one eventually.

## The three traps, which cost most of the time

- **The raster compare is `$D012`, written in VIC-II line numbers.** `$D012`
  reads as the current line but a write still latches the compare. The
  VIC-IV's own `$D079`/`$D07A` pair never produced an interrupt at all.
  `TEXTYPOS` is 104 *physical* rasters and a VIC-II line is two of them, so
  the character display starts at line 52 and each row is 8 lines.
- **The C65 ROM's `$0314` dispatcher is not the C64's.** It is not the
  three-register one at `$FF48` but the 45GS02-aware one at `$FA23`, which
  does `PHA / PHX / PHY / PHZ / TBA / PHA` and leaves **five** bytes on the
  stack: A, X, Y, Z and the base page register B. The exit is therefore
  `PLA / TAB / PLZ / PLY / PLX / PLA / RTI`. A C64-style three-pull exit RTIs
  onto a mismatched frame and the machine is dead after one interrupt.
- **Chaining to the displaced handler is not a way round that.** It keeps the
  machine alive, but the ROM's handler reprograms the raster compare to its
  own line every time, so both splits end up firing in the same place.

One more, if the handler is reused: it must preserve **Z**. The renderer holds
Z across its span fill loop, and nothing on the ROM path saves it.
