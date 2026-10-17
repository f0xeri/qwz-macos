/*
 * QwzWifi: linux-firmware ath12k/WCN7850/hw2.0 blobs, linked in unmodified.
 * Paths are relative to the repository root (scripts/build.sh runs there).
 */
	.const
	.p2align 4
	.globl _qwz_fw_amss, _qwz_fw_amss_end
_qwz_fw_amss:
	.incbin "firmware/amss.bin"
_qwz_fw_amss_end:

	.p2align 4
	.globl _qwz_fw_m3, _qwz_fw_m3_end
_qwz_fw_m3:
	.incbin "firmware/m3.bin"
_qwz_fw_m3_end:

	.p2align 4
	.globl _qwz_fw_board2, _qwz_fw_board2_end
_qwz_fw_board2:
	.incbin "firmware/board-2.bin"
_qwz_fw_board2_end:
