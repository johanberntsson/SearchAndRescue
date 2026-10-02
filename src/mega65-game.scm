;;; The game's memory map: the stock mega65-plain.scm, plus the 8 KB of RAM
;;; under the BASIC ROM for whatever BSS is marked HIGH_BSS (src/loader.h).
;;;
;;; **The 32 KB at $2001 was this file's doing, not the machine's.** The
;;; stock script gives `program` $2001-$9FFF because that is what is safe with
;;; every ROM mapped in. src/bank.s banks BASIC out ($D030 bit 4) before main
;;; runs, and $A000-$BFFF is ordinary RAM from then on.
;;;
;;; **No `(type ...)` on it, and that is the whole trick.** Declared `any`, or
;;; even `ram`, the linker may put `zdata`'s initialised half or `cstack` up
;;; here, and either makes a second *content* area, which a PRG cannot have --
;;; "multiple program areas not allowed in prg output". Declared the way the
;;; stock script declares freeSpace -- a name and a section and nothing else
;;; -- it takes the named section and never becomes one. Only BSS goes up
;;; here, so nothing in the window has to arrive from the disk with the ROM
;;; still mapped over it.
;;;
;;; The KERNAL at $E000 and $C000-$CFFF are left alone: the game reads its
;;; resources through the KERNAL and its interrupt vectors are the ROM's.

(define memories
  '((memory program
            (address (#x2001 . #x9fff)) (type any)
            (section (programStart #x2001) (startup #x200e)))
    (memory highram
            (address (#xa000 . #xbfff))
            (section highbss))
    (memory zeroPage (address (#x2 . #x7f)) (type ram) (qualifier zpage)
            (section (registers #x2)))
    (memory stackPage (address (#x100 . #x1ff)) (type ram))
    (memory freeSpace (address (#x1600 . #x1eff)) (section zpsave))
    ))
