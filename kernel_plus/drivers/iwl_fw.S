/* Gomulu AX201 (So / HR) firmware. linux-firmware iwlwifi-so-a0-hr-b0-86.
 * C:/firmware altindaki ayni adli dosya varsa surucu onu tercih eder. */
.section .rodata
.global iwl_fw_blob
.global iwl_fw_blob_end
.align 16
iwl_fw_blob:
.incbin "third_party/firmware/iwlwifi-so-a0-hr-b0-86.ucode"
iwl_fw_blob_end:
