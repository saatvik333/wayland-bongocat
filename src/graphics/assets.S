/* Cat artwork, embedded verbatim and NUL-terminated so it can be parsed as a
   C string. Paths are relative to the repository root, where builds run. */

  .section .rodata

  .globl ASSET_BOTH_UP, ASSET_LEFT_DOWN, ASSET_RIGHT_DOWN, ASSET_BOTH_DOWN
  .globl ASSET_SLEEPING

ASSET_BOTH_UP:
  .incbin "assets/svg/bongo-both-up.svg"
  .byte 0
ASSET_LEFT_DOWN:
  .incbin "assets/svg/bongo-left-down.svg"
  .byte 0
ASSET_RIGHT_DOWN:
  .incbin "assets/svg/bongo-right-down.svg"
  .byte 0
ASSET_BOTH_DOWN:
  .incbin "assets/svg/bongo-both-down.svg"
  .byte 0
ASSET_SLEEPING:
  .incbin "assets/svg/bongo-sleeping.svg"
  .byte 0

  .section .note.GNU-stack, "", %progbits
