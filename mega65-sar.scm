;;; Stage one's memory map: the stock mega65-plain.scm plus the 8 KB of RAM
;;; that lives under the BASIC ROM.
;;;
;;; **The 32 KB everything in this project has been squeezed into is this file's
;;; doing, not the machine's.** mega65-plain.scm gives `program` $2001-$9FFF
;;; because that is what is safe with every ROM mapped in. Bank BASIC out and
;;; $A000-$BFFF is ordinary RAM -- see Bank Switching on c64-wiki, and
;;; src/mapgen/kernal.s for the one register that does it here.
;;;
;;; **Only BSS goes up there, and that is deliberate.** A PRG is loaded by the
;;; ROM with BASIC still mapped in, so anything above $9FFF that has to arrive
;;; from the disk raises a question about whether the write falls through to
;;; the RAM underneath. zdata and cstack are not in the file -- they are only
;;; ever written by the program itself, after __low_level_init has banked BASIC
;;; out -- so the question does not arise. Code and initialised data stay low.
;;;
;;; $C000-$CFFF is banked out with it, for 12 KB in all. The KERNAL at $E000
;;; stays: printf goes through it and the interrupt vectors live in it, which
;;; is what keeps this free of SEI and a handler of our own.

(define memories
  '((memory program
            (address (#x2001 . #x9fff)) (type any)
            (section (programStart #x2001) (startup #x200e)))
    ;; Named rather than taking `zdata` wholesale: BSS is 11 KB and this is 8,
    ;; so all-or-nothing does not place. What goes here is chosen in the source
    ;; with HIGH_BSS, and the first tenant is the 8 KB work union.
    (memory highram
            (address (#xa000 . #xcfff)) (type any)
            (section highbss))
    (memory zeroPage (address (#x2 . #x7f)) (type ram) (qualifier zpage)
            (section (registers #x2)))
    (memory stackPage (address (#x100 . #x1ff)) (type ram))
    (memory freeSpace (address (#x1600 . #x1eff)) (section zpsave))
    ))
