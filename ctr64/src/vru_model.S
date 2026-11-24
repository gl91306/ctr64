/* The VRU phoneme model (tools/vru: train.py, export.py), built into the program */
    .section .rodata.vru_model, "a"
    .balign 16
    .global vru_model_data
vru_model_data:
    .incbin "vru_model.bin"
    .global vru_model_end
vru_model_end:
