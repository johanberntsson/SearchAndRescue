; ===========================================================================
;  music.asm -- "Electric Pursuit"
;
;  Driving minor key ostinato in the Peter Gunn tradition: a relentless
;  eighth note bass under a brass lead, 125 BPM, 16 bar loop with a two bar
;  intro that is skipped on the repeat.
;
;  Rows are 1/16 notes, two bytes each: note, instrument.
;  Notes are written as n_<name> + o<octave>, e.g. n_fs+o4.
;  0,0 = let the previous note ring on.  Every pattern is 32 rows / 2 bars.
; ===========================================================================

; --- note names ------------------------------------------------------------
n_c  = 1
n_cs = 2
n_d  = 3
n_ds = 4
n_e  = 5
n_f  = 6
n_fs = 7
n_g  = 8
n_gs = 9
n_a  = 10
n_as = 11
n_b  = 12

o0 = 0
o1 = 12
o2 = 24
o3 = 36
o4 = 48
o5 = 60
o6 = 72

; --- instruments -----------------------------------------------------------
IBASS   = 0
ILEAD   = 1
ISTABM  = 2
IKICK   = 3
ISNARE  = 4
IHAT    = 5
IOHAT   = 6
ISTABJ  = 7

; --- pattern numbers -------------------------------------------------------
PD_INTRO = 0
PD_BEAT  = 1
PD_FILL  = 2
PB_REST  = 3
PB_INTRO = 4
PB_EM    = 5
PB_EM2   = 6
PB_C     = 7
PB_G     = 8
PB_B     = 9
PL_REST  = 10
PL_STAB  = 11
PL_A1    = 12
PL_A2    = 13
PL_C     = 14
PL_G     = 15
PL_B     = 16
PL_FILL  = 17

; guards against a mistyped pattern: 32 rows of 2 bytes plus the end marker
!macro endpat .start {
        !byte CMD_END
        !if * - .start != 65 {
                !error "pattern is not 32 rows long"
        }
}


; ---------------------------------------------------------------------------
;  instruments (parallel tables, indexed by instrument number)
; ---------------------------------------------------------------------------
;                 bass   lead  stab-  kick  snare  hat   open   stab-
;                                min                     hat    maj
ins_ad    !byte   $08,   $17,   $06,  $07,  $08,   $02,  $05,   $06
ins_sr    !byte   $a6,   $b8,   $00,  $00,  $00,   $00,  $00,   $00
ins_wave  !byte   $41,   $21,   $41,  $81,  $81,   $81,  $81,   $41
ins_wave2 !byte   $41,   $21,   $41,  $11,  $81,   $81,  $81,   $41
ins_wdelay!byte   $00,   $00,   $00,  $01,  $00,   $00,  $00,   $00
ins_pwlo  !byte   $00,   $00,   $00,  $00,  $00,   $00,  $00,   $00
ins_pwhi  !byte   $07,   $00,   $04,  $00,  $00,   $00,  $00,   $04
ins_pwd   !byte   $03,   $00,   $fe,  $00,  $00,   $00,  $00,   $fe
ins_sl_l  !byte   $00,   $00,   $00,  $00,  $80,   $00,  $00,   $00
ins_sl_h  !byte   $00,   $00,   $00,  $fd,  $ff,   $00,  $00,   $00
ins_arp   !byte   $ff,   $ff,   $00,  $ff,  $ff,   $ff,  $ff,   $01
ins_vibdep!byte   $00,   $05,   $00,  $00,  $00,   $00,  $00,   $00
ins_vibdel!byte   $00,   $0c,   $00,  $00,  $00,   $00,  $00,   $00

; --- arpeggio tables, semitone offsets, $80 = loop back --------------------
arp_lo    !byte <arp_minor, <arp_major, <arp_octave
arp_hi    !byte >arp_minor, >arp_major, >arp_octave

arp_minor !byte 0, 3, 7, $80
arp_major !byte 0, 4, 7, $80
arp_octave!byte 0, 12, $80


; ---------------------------------------------------------------------------
;  pattern address table
; ---------------------------------------------------------------------------
patt_lo   !byte <pd_intro, <pd_beat, <pd_fill
          !byte <pb_rest, <pb_intro, <pb_em, <pb_em2, <pb_c, <pb_g, <pb_b
          !byte <pl_rest, <pl_stab, <pl_a1, <pl_a2, <pl_c, <pl_g, <pl_b
          !byte <pl_fill
patt_hi   !byte >pd_intro, >pd_beat, >pd_fill
          !byte >pb_rest, >pb_intro, >pb_em, >pb_em2, >pb_c, >pb_g, >pb_b
          !byte >pl_rest, >pl_stab, >pl_a1, >pl_a2, >pl_c, >pl_g, >pl_b
          !byte >pl_fill


; ---------------------------------------------------------------------------
;  order lists -- 16 bars, then back to bar 3 so the intro plays only once
; ---------------------------------------------------------------------------
ord_lo    !byte <ord_bass, <ord_lead, <ord_drum
ord_hi    !byte >ord_bass, >ord_lead, >ord_drum

ord_bass  !byte PB_INTRO, PB_EM, PB_EM,  PB_EM2
          !byte PB_C,    PB_G,   PB_B,   PB_EM2
          !byte ORD_JMP, 1

ord_lead  !byte PL_REST, PL_STAB, PL_A1, PL_A2
          !byte PL_C,    PL_G,    PL_B,  PL_FILL
          !byte ORD_JMP, 1

ord_drum  !byte PD_INTRO, PD_BEAT, PD_BEAT, PD_FILL
          !byte PD_BEAT,  PD_BEAT, PD_BEAT, PD_FILL
          !byte ORD_JMP, 1


; ===========================================================================
;  drums (voice 3) -- one line per beat, four 1/16 rows
; ===========================================================================
pd_intro
        !byte n_a+o3,IKICK, 0,0, n_g+o6,IHAT,  0,0
        !byte 0,0,          0,0, n_g+o6,IHAT,  0,0
        !byte n_a+o3,IKICK, 0,0, n_g+o6,IHAT,  0,0
        !byte 0,0,          0,0, n_g+o6,IHAT,  n_g+o6,IHAT
        !byte n_a+o3,IKICK, 0,0, n_g+o6,IHAT,  0,0
        !byte 0,0,          0,0, n_g+o6,IHAT,  0,0
        !byte n_a+o3,IKICK, 0,0, n_g+o6,IHAT,  0,0
        !byte 0,0,          0,0, n_g+o6,IHAT,  n_e+o6,IOHAT
        +endpat pd_intro

pd_beat
        !byte n_a+o3,IKICK,  0,0, n_g+o6,IHAT, 0,0
        !byte n_e+o5,ISNARE, 0,0, n_g+o6,IHAT, 0,0
        !byte n_a+o3,IKICK,  0,0, n_a+o3,IKICK, 0,0
        !byte n_e+o5,ISNARE, 0,0, n_g+o6,IHAT, n_g+o6,IHAT
        !byte n_a+o3,IKICK,  0,0, n_g+o6,IHAT, 0,0
        !byte n_e+o5,ISNARE, 0,0, n_g+o6,IHAT, 0,0
        !byte n_a+o3,IKICK,  0,0, n_a+o3,IKICK, 0,0
        !byte n_e+o5,ISNARE, 0,0, n_g+o6,IHAT, n_e+o6,IOHAT
        +endpat pd_beat

pd_fill
        !byte n_a+o3,IKICK,  0,0, n_g+o6,IHAT, 0,0
        !byte n_e+o5,ISNARE, 0,0, n_g+o6,IHAT, 0,0
        !byte n_a+o3,IKICK,  0,0, n_a+o3,IKICK, 0,0
        !byte n_e+o5,ISNARE, 0,0, n_g+o6,IHAT, n_g+o6,IHAT
        !byte n_a+o3,IKICK,  0,0, n_g+o6,IHAT, 0,0
        !byte n_e+o5,ISNARE, 0,0, n_g+o6,IHAT, 0,0
        !byte n_e+o5,ISNARE, n_e+o5,ISNARE, n_e+o5,ISNARE, n_e+o5,ISNARE
        !byte n_e+o5,ISNARE, 0,0, n_e+o5,ISNARE, n_e+o5,ISNARE
        +endpat pd_fill


; ===========================================================================
;  bass (voice 1) -- straight eighth notes, one line per beat
; ===========================================================================
pb_rest
        !fill 64, 0
        +endpat pb_rest

;  intro: a low pedal note under the drums, then two eighths as a pick up
pb_intro
        !byte n_e+o2,IBASS, 0,0, 0,0,          0,0
        !byte 0,0,          0,0, 0,0,          0,0
        !byte n_e+o2,IBASS, 0,0, 0,0,          0,0
        !byte 0,0,          0,0, 0,0,          0,0
        !byte n_e+o2,IBASS, 0,0, 0,0,          0,0
        !byte 0,0,          0,0, 0,0,          0,0
        !byte n_e+o2,IBASS, 0,0, 0,0,          0,0
        !byte n_e+o2,IBASS, 0,0, n_e+o2,IBASS, 0,0
        +endpat pb_intro

;  E  E | E  B | E  E | G  A || E  E | E  B | E  G | A  A#
pb_em
        !byte n_e+o2,IBASS,  0,0, n_e+o2,IBASS,  0,0
        !byte n_e+o2,IBASS,  0,0, n_b+o1,IBASS,  0,0
        !byte n_e+o2,IBASS,  0,0, n_e+o2,IBASS,  0,0
        !byte n_g+o2,IBASS,  0,0, n_a+o2,IBASS,  0,0
        !byte n_e+o2,IBASS,  0,0, n_e+o2,IBASS,  0,0
        !byte n_e+o2,IBASS,  0,0, n_b+o1,IBASS,  0,0
        !byte n_e+o2,IBASS,  0,0, n_g+o2,IBASS,  0,0
        !byte n_a+o2,IBASS,  0,0, n_as+o2,IBASS, 0,0
        +endpat pb_em

;  same opening, then a walk down to the turnaround
pb_em2
        !byte n_e+o2,IBASS,  0,0, n_e+o2,IBASS,  0,0
        !byte n_e+o2,IBASS,  0,0, n_b+o1,IBASS,  0,0
        !byte n_e+o2,IBASS,  0,0, n_e+o2,IBASS,  0,0
        !byte n_g+o2,IBASS,  0,0, n_a+o2,IBASS,  0,0
        !byte n_b+o2,IBASS,  0,0, n_b+o2,IBASS,  0,0
        !byte n_a+o2,IBASS,  0,0, n_a+o2,IBASS,  0,0
        !byte n_g+o2,IBASS,  0,0, n_g+o2,IBASS,  0,0
        !byte n_fs+o2,IBASS, 0,0, n_f+o2,IBASS,  0,0
        +endpat pb_em2

pb_c
        !byte n_c+o2,IBASS,  0,0, n_c+o2,IBASS,  0,0
        !byte n_c+o2,IBASS,  0,0, n_g+o1,IBASS,  0,0
        !byte n_c+o2,IBASS,  0,0, n_c+o2,IBASS,  0,0
        !byte n_ds+o2,IBASS, 0,0, n_f+o2,IBASS,  0,0
        !byte n_c+o2,IBASS,  0,0, n_c+o2,IBASS,  0,0
        !byte n_c+o2,IBASS,  0,0, n_g+o1,IBASS,  0,0
        !byte n_c+o2,IBASS,  0,0, n_ds+o2,IBASS, 0,0
        !byte n_f+o2,IBASS,  0,0, n_fs+o2,IBASS, 0,0
        +endpat pb_c

pb_g
        !byte n_g+o2,IBASS,  0,0, n_g+o2,IBASS,  0,0
        !byte n_g+o2,IBASS,  0,0, n_d+o2,IBASS,  0,0
        !byte n_g+o2,IBASS,  0,0, n_g+o2,IBASS,  0,0
        !byte n_as+o2,IBASS, 0,0, n_c+o3,IBASS,  0,0
        !byte n_g+o2,IBASS,  0,0, n_g+o2,IBASS,  0,0
        !byte n_g+o2,IBASS,  0,0, n_d+o2,IBASS,  0,0
        !byte n_g+o2,IBASS,  0,0, n_as+o2,IBASS, 0,0
        !byte n_c+o3,IBASS,  0,0, n_cs+o3,IBASS, 0,0
        +endpat pb_g

;  the V chord: B major, the D# pulls back to E minor
pb_b
        !byte n_b+o1,IBASS,  0,0, n_b+o1,IBASS,  0,0
        !byte n_b+o1,IBASS,  0,0, n_fs+o2,IBASS, 0,0
        !byte n_b+o1,IBASS,  0,0, n_b+o1,IBASS,  0,0
        !byte n_ds+o2,IBASS, 0,0, n_fs+o2,IBASS, 0,0
        !byte n_b+o1,IBASS,  0,0, n_b+o1,IBASS,  0,0
        !byte n_b+o1,IBASS,  0,0, n_fs+o2,IBASS, 0,0
        !byte n_b+o1,IBASS,  0,0, n_ds+o2,IBASS, 0,0
        !byte n_fs+o2,IBASS, 0,0, n_a+o2,IBASS,  0,0
        +endpat pb_b


; ===========================================================================
;  lead (voice 2)
; ===========================================================================
pl_rest
        !fill 64, 0
        +endpat pl_rest

;  intro: off beat chord stabs
pl_stab
        !byte 0,0, 0,0, n_e+o4,ISTABM, 0,0
        !byte 0,0, 0,0, n_e+o4,ISTABM, 0,0
        !byte 0,0, 0,0, n_e+o4,ISTABM, 0,0
        !byte 0,0, 0,0, n_e+o4,ISTABM, 0,0
        !byte 0,0, 0,0, n_e+o4,ISTABM, 0,0
        !byte 0,0, 0,0, n_e+o4,ISTABM, 0,0
        !byte 0,0, 0,0, n_e+o4,ISTABM, 0,0
        !byte 0,0, 0,0, n_b+o4,ISTABM, 0,0
        +endpat pl_stab

;  theme, first half
pl_a1
        !byte n_b+o4,ILEAD, 0,0, n_b+o4,ILEAD, 0,0
        !byte n_d+o5,ILEAD, 0,0, n_b+o4,ILEAD, 0,0
        !byte n_a+o4,ILEAD, 0,0, 0,0,          0,0
        !byte 0,0,          0,0, 0,0,          0,0
        !byte n_g+o4,ILEAD, 0,0, n_g+o4,ILEAD, 0,0
        !byte n_a+o4,ILEAD, 0,0, n_b+o4,ILEAD, 0,0
        !byte n_e+o4,ILEAD, 0,0, 0,0,          0,0
        !byte 0,0,          0,0, 0,0,          0,0
        +endpat pl_a1

;  theme, answer
pl_a2
        !byte n_e+o5,ILEAD, 0,0, n_d+o5,ILEAD, 0,0
        !byte n_b+o4,ILEAD, 0,0, n_a+o4,ILEAD, 0,0
        !byte n_g+o4,ILEAD, 0,0, 0,0,          0,0
        !byte 0,0,          0,0, 0,0,          0,0
        !byte n_a+o4,ILEAD, 0,0, n_b+o4,ILEAD, 0,0
        !byte n_a+o4,ILEAD, 0,0, n_g+o4,ILEAD, 0,0
        !byte n_fs+o4,ILEAD,0,0, 0,0,          0,0
        !byte 0,0,          0,0, 0,0,          0,0
        +endpat pl_a2

pl_c
        !byte n_c+o5,ILEAD, 0,0, n_c+o5,ILEAD, 0,0
        !byte n_e+o5,ILEAD, 0,0, n_c+o5,ILEAD, 0,0
        !byte n_g+o4,ILEAD, 0,0, 0,0,          0,0
        !byte 0,0,          0,0, 0,0,          0,0
        !byte n_e+o5,ILEAD, 0,0, n_d+o5,ILEAD, 0,0
        !byte n_c+o5,ILEAD, 0,0, n_b+o4,ILEAD, 0,0
        !byte n_g+o4,ILEAD, 0,0, 0,0,          0,0
        !byte 0,0,          0,0, 0,0,          0,0
        +endpat pl_c

pl_g
        !byte n_d+o5,ILEAD, 0,0, n_d+o5,ILEAD, 0,0
        !byte n_g+o5,ILEAD, 0,0, n_d+o5,ILEAD, 0,0
        !byte n_b+o4,ILEAD, 0,0, 0,0,          0,0
        !byte 0,0,          0,0, 0,0,          0,0
        !byte n_d+o5,ILEAD, 0,0, n_b+o4,ILEAD, 0,0
        !byte n_g+o4,ILEAD, 0,0, n_a+o4,ILEAD, 0,0
        !byte n_b+o4,ILEAD, 0,0, 0,0,          0,0
        !byte 0,0,          0,0, 0,0,          0,0
        +endpat pl_g

pl_b
        !byte n_fs+o5,ILEAD,0,0, n_fs+o5,ILEAD,0,0
        !byte n_e+o5,ILEAD, 0,0, n_ds+o5,ILEAD,0,0
        !byte n_b+o4,ILEAD, 0,0, 0,0,          0,0
        !byte 0,0,          0,0, 0,0,          0,0
        !byte n_ds+o5,ILEAD,0,0, n_fs+o5,ILEAD,0,0
        !byte n_e+o5,ILEAD, 0,0, n_ds+o5,ILEAD,0,0
        !byte n_a+o4,ILEAD, 0,0, 0,0,          0,0
        !byte 0,0,          0,0, 0,0,          0,0
        +endpat pl_b

;  turnaround: hold, then a run back up into the top of the loop
pl_fill
        !byte n_b+o4,ILEAD, 0,0, n_a+o4,ILEAD, 0,0
        !byte n_g+o4,ILEAD, 0,0, 0,0,          0,0
        !byte n_fs+o4,ILEAD,0,0, 0,0,          0,0
        !byte 0,0,          0,0, 0,0,          0,0
        !byte n_e+o4,ILEAD, 0,0, 0,0,          0,0
        !byte 0,0,          0,0, 0,0,          0,0
        !byte n_e+o4,ILEAD, n_g+o4,ILEAD, n_a+o4,ILEAD, n_b+o4,ILEAD
        !byte n_d+o5,ILEAD, n_e+o5,ILEAD, 0,0,          0,0
        +endpat pl_fill
