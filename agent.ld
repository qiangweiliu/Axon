/* agent.ld — Linker script for .agent_modules section */
SECTIONS
{
    .agent_modules : {
        __start___agent_modules = .;
        KEEP(*(.agent_modules))
        __stop___agent_modules = .;
    }
}
INSERT AFTER .rodata;
