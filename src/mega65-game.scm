;;; The game's memory map: the stock mega65-plain.scm with its BSS and its C
;;; stack moved into the RAM under the BASIC ROM.
;;;
;;; **The 32 KB at $2001 was the tightest thing in this project** -- 44 bytes
;;; free before this -- and it was never the machine's limit, only what is safe
;;; with every ROM mapped in. The game banks BASIC out for its whole run (see
;;; src/bank.s) and gets $A000-$BFFF back, which is where `zdata` and `cstack`
;;; go: 1.6 KB and 4 KB, so about 5.7 KB of the 32 comes free.
;;;
;;; **Named, not wholesale.** `zdata` and `cstack` are sections the linker
;;; rules already know, and putting either in a second `any` memory makes a
;;; second *content* area, which a PRG cannot have -- "multiple program areas
;;; not allowed in prg output". A section of our own is only ever BSS, so it
;;; places without creating one. HIGH_BSS in src/vic4.h marks what moves.
;;;
;;; **Only BSS and the stack.** Neither is in the PRG, so neither has to be
;;; written above $9FFF by a loader running with the ROM still mapped. Code and
;;; initialised data stay where they were.
;;;
;;; The KERNAL at $E000 stays mapped, which is what makes this safe: the game
;;; reads its resources through it at startup, its interrupt vectors are the
;;; ROM's, and it never needs an SEI. $C000-$CFFF is left alone too -- stage
;;; one banks it out, but stage one does no disk I/O, and the C65 keeps parts
;;; of its kernel there.

(define memories
  '((memory program
            (address (#x2001 . #x9fff)) (type any)
            (section (programStart #x2001) (startup #x200e)))
    (memory highram
            (address (#xa000 . #xbfff)) (type any)
            (section highbss))
    (memory zeroPage (address (#x2 . #x7f)) (type ram) (qualifier zpage)
            (section (registers #x2)))
    (memory stackPage (address (#x100 . #x1ff)) (type ram))
    (memory freeSpace (address (#x1600 . #x1eff)) (section zpsave))
    ))
