{
   global: retro_*;
           RdZ80; WrZ80; InZ80; OutZ80;
           PSL; SSL; SSLReg; EnWrite; RAM; Mode;
           msxpico_fmsx_attach_from_env; msxpico_fmsx_stop;
           msxpico_fmsx_attached;
           msxpico_fmsx_mem_read; msxpico_fmsx_mem_write;
           msxpico_fmsx_io_read; msxpico_fmsx_io_write;
           RAMPages; VRAMPages;
           fmsx_cpu_frequency_hz; fmsx_z80_pc; fmsx_z80_bc; fmsx_z80_sp;
           fmsx_z80_cycles; HPeriod; VPeriod;
           fmsx_pico_ntsc_locked;
           /* No simulator-side mapper/debug globals: the native firmware
            * receives every bus transaction directly. */
   local: *;
};

