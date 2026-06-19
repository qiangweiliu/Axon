/* llm_core.h — Model adapter interface (llm_client core layer)
 *
 * Each model backend self-registers via LLM_MODEL_REGISTER macro,
 * which places a pointer in the .llm_models ELF section.
 * Core layer scans this section at startup to discover all models.
 */

#ifndef LLM_CORE_H
#define LLM_CORE_H

#include <stddef.h>
#include <stdint.h>

/* Place a model pointer in .llm_models section for auto-discovery.
   Usage — at bottom of each model_xxx.c:
     LLM_MODEL_REGISTER(my_adapter_instance); */
#define LLM_MODEL_REGISTER(var) \
    __attribute__((section(".llm_models"), used)) \
    const llm_model_t *_llm_m_##var = &var

/* Model adapter: one instance per model family */
typedef struct {
    const char *name;  /* model name prefix, e.g. "deepseek", "agnes-" */

    /* Build JSON request body. stream=1 for SSE, 0 for single response.
       Returns bytes written, or <0 on error. */
    int (*build_body)(char *buf, size_t buf_len,
                      const char *model, const char *prompt, int stream);

    /* Extract text content from response JSON.
       Returns malloc'd string, caller frees. NULL if no content. */
    char* (*extract_content)(const char *json, size_t *out_len);

    /* Extract an integer field from response JSON.
       Returns the value, or 0 if not found. */
    int (*extract_int)(const char *json, const char *field);
} llm_model_t;

/* Discover all registered models by scanning .llm_models section.
   Called once at startup before first llm_model_select(). */
void llm_model_discover(void);

/* Select a model adapter by model name (prefix match).
   Falls back to the first-discovered adapter. Never returns NULL. */
const llm_model_t* llm_model_select(const char *model_name);

#endif /* LLM_CORE_H */
