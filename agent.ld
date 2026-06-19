/* agent.ld — Linker script for .agent_modules and .llm_models sections */
SECTIONS
{
    .agent_modules : {
        __start___agent_modules = .;
        KEEP(*(.agent_modules))
        __stop___agent_modules = .;
    }
    .llm_models : {
        __start___llm_models = .;
        KEEP(*(.llm_models))
        __stop___llm_models = .;
    }
}
INSERT AFTER .rodata;
