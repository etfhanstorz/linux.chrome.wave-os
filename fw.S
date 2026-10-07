// The Wi-Fi chip firmware (Marvell sd8897_uapsta.bin) built into the image. It is fetched by
// build.sh into fw/ (NOT stored in git: Marvell's licence, redistributable but kept out of the repo).
    .section .rodata
    .balign 16
    .global fw_start
    .global fw_end
    .hidden fw_start
    .hidden fw_end
fw_start:
    .incbin "fw/sd8897_uapsta.bin"
fw_end:
    .balign 16
