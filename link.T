{
   global: retro_*;
           RdZ80; WrZ80; InZ80; OutZ80;
           PSL; SSL; SSLReg; EnWrite; RAM; Mode;
           msxpico_fmsx_attach_rom; msxpico_fmsx_stop;
           msxpico_fmsx_claim_port; msxpico_fmsx_attached;
           msxpico_fmsx_sd_commands; msxpico_fmsx_sd_reads;
           RAMPages; VRAMPages;
           fmsx_cpu_frequency_hz; HPeriod; VPeriod;
           sim_io_write_count; sim_io_last_port; sim_io_last_data;
   local: *;
};

