/* Keep the runtime and parser from the same Kraken Mount revision. */
.section .rodata
.global sm_kraken_runtime_start
.global sm_kraken_runtime_end
.global sm_kraken_revision
.global sm_kraken_playgo_start
.global sm_kraken_playgo_end

.balign 16
sm_kraken_runtime_start:
.incbin "vendor/kraken/vendor/ampr-emu-ng/libSceAmpr.sprx"
sm_kraken_runtime_end:
sm_kraken_revision:
.incbin "vendor/kraken/REVISION"
.byte 0

.balign 16
sm_kraken_playgo_start:
.incbin "vendor/pgo_stub/libScePlayGo.sprx"
sm_kraken_playgo_end:

.section .note.GNU-stack,"",@progbits
