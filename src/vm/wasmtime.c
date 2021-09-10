#include <wasi.h>
#include <wasm.h>
#include <wasmtime.h>
#include <http/ngx_http_wasm_api.h>
#include "vm.h"


typedef struct {
    wasmtime_module_t       *module;
    wasmtime_store_t        *store;
    wasmtime_context_t      *context;
    wasmtime_linker_t       *linker;
    wasmtime_instance_t      instance;
} ngx_wasm_wasmtime_plugin_t;


static ngx_str_t      vm_name = ngx_string("wasmtime");
static wasm_engine_t *vm_engine;
static wasmtime_val_t   param_int32_int32[2] = {{ .kind = WASMTIME_I32 }, { .kind = WASMTIME_I32 }};


static void
ngx_wasm_wasmtime_report_error(ngx_log_t *log, const char *message,
    wasmtime_error_t *error, wasm_trap_t *trap)
{
    wasm_byte_vec_t error_message;

    if (error != NULL) {
        wasmtime_error_message(error, &error_message);
        wasmtime_error_delete(error);
    } else {
        wasm_trap_message(trap, &error_message);
        wasm_trap_delete(trap);
    }

    ngx_log_error(NGX_LOG_ERR, log, 0, "%s%*s",
                  message, error_message.size, error_message.data);
    wasm_byte_vec_delete(&error_message);
}


static ngx_int_t
ngx_wasm_wasmtime_init(void)
{
    ngx_log_error(NGX_LOG_NOTICE, ngx_cycle->log, 0, "init wasm vm: wasmtime");

    vm_engine = wasm_engine_new();
    if (vm_engine == NULL) {
        return NGX_DECLINED;
    }

    return NGX_OK;
}


static void
ngx_wasm_wasmtime_cleanup(void)
{
    if (vm_engine == NULL) {
        return;
    }

    ngx_log_error(NGX_LOG_INFO, ngx_cycle->log, 0, "cleanup wasm vm: wasmtime");
    wasm_engine_delete(vm_engine);
}


static void *
ngx_wasm_wasmtime_load(const char *bytecode, size_t size)
{
    int                           i;
    wasm_trap_t                  *trap = NULL;
    wasmtime_module_t            *module;
    wasmtime_store_t             *store;
    wasmtime_context_t           *context;
    wasi_config_t                *wasi_config;
    wasmtime_error_t             *error;
    ngx_wasm_wasmtime_plugin_t   *plugin;

    // TODO: separate WASM compiling from the store init
    error = wasmtime_module_new(vm_engine, (const uint8_t*) bytecode, size, &module);
    if (module == NULL) {
        return NULL;
    }

    store = wasmtime_store_new(vm_engine, NULL, NULL);
    if (store == NULL) {
        goto free_module;
    }

    context = wasmtime_store_context(store);

    wasi_config = wasi_config_new();
    if (wasi_config == NULL) {
        goto free_store;
    }

    wasi_config_inherit_argv(wasi_config);
    wasi_config_inherit_env(wasi_config);
    wasi_config_inherit_stdin(wasi_config);
    wasi_config_inherit_stdout(wasi_config);
    wasi_config_inherit_stderr(wasi_config);

    error = wasmtime_context_set_wasi(context, wasi_config);
    if (error != NULL) {
        ngx_wasm_wasmtime_report_error(ngx_cycle->log, "failed to init WASI: ", error, NULL);
        goto free_config;
    }

    wasmtime_linker_t *linker = wasmtime_linker_new(vm_engine);
    if (linker == NULL) {
        goto free_config;
    }

    error = wasmtime_linker_define_wasi(linker);
    if (error != NULL) {
        ngx_wasm_wasmtime_report_error(ngx_cycle->log, "failed to init WASI: ", error, NULL);
        goto free_linker;
    }

    for (i = 0; host_apis[i].name.len; i++) {
        ngx_wasm_host_api_t *api = &host_apis[i];
        wasm_functype_t     *f;

        ngx_log_debug1(NGX_LOG_DEBUG_CORE, ngx_cycle->log, 0,
                       "define wasm host API %V", &api->name);

        f = ngx_http_wasm_host_api_func(api);
        error = wasmtime_linker_define_func(linker, "env", 3,
                                            (const char *) api->name.data, api->name.len,
                                            f,
                                            api->cb, NULL, NULL);
        wasm_functype_delete(f);

        if (error != NULL) {
            ngx_wasm_wasmtime_report_error(ngx_cycle->log, "failed to define API", error, NULL);
            goto free_linker;
        }
    }

    plugin = ngx_alloc(sizeof(ngx_wasm_wasmtime_plugin_t), ngx_cycle->log);
    if (plugin == NULL) {
        goto free_linker;
    }

    error = wasmtime_linker_instantiate(linker, context, module, &plugin->instance, &trap);
    if (error != NULL) {
        ngx_wasm_wasmtime_report_error(ngx_cycle->log, "failed to new instance: ", error, NULL);
        goto free_linker;
    }

    plugin->module = module;
    plugin->store = store;
    plugin->context = context;
    plugin->linker = linker;

    ngx_log_error(NGX_LOG_INFO, ngx_cycle->log, 0, "loaded wasm plugin");

    return plugin;

free_linker:
    wasmtime_linker_delete(linker);

free_config:
    wasi_config_delete(wasi_config);

free_store:
    wasmtime_store_delete(store);

free_module:
    wasmtime_module_delete(module);

    return NULL;
}


static void
ngx_wasm_wasmtime_unload(void *data)
{
    ngx_wasm_wasmtime_plugin_t *plugin = data;

    wasmtime_module_delete(plugin->module);
    wasmtime_store_delete(plugin->store);
    wasmtime_linker_delete(plugin->linker);

    ngx_free(plugin);

    ngx_log_error(NGX_LOG_INFO, ngx_cycle->log, 0, "unloaded wasm plugin");
}


static ngx_int_t
ngx_wasm_wasmtime_call(void *data, ngx_str_t *name, bool has_result, int param_type, ...)
{
    ngx_wasm_wasmtime_plugin_t *plugin = data;
    wasmtime_extern_t           func;
    wasm_trap_t                *trap = NULL;
    wasmtime_error_t           *error;
    bool                        found;
    va_list                     args;
    wasmtime_val_t             *params = NULL;
    size_t                      param_num  =0;
    wasmtime_val_t              results[1];
    ngx_int_t                   rc;

    ngx_log_debug1(NGX_LOG_DEBUG_CORE, ngx_cycle->log, 0, "wasmtime call function %V", name);

    found = wasmtime_instance_export_get(plugin->context, &plugin->instance,
                                         (const char *) name->data, name->len, &func);
    if (!found) {
        ngx_log_debug1(NGX_LOG_DEBUG_CORE, ngx_cycle->log, 0,
                       "wasmtime function %V not defined", name);
        return NGX_OK;
    }

    va_start(args, param_type);

    switch (param_type) {
    case NGX_WASM_PARAM_I32_I32:
        params = param_int32_int32;
        params[0].of.i32 = va_arg(args, int32_t);
        params[1].of.i32 = va_arg(args, int32_t);
        param_num = 2;
        break;

    case NGX_WASM_PARAM_VOID:
        break;

    default:
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "unknown param type: %d", param_type);
        return NGX_ERROR;
    }

    error = wasmtime_func_call(plugin->context, &func.of.func, params, param_num, results,
                               has_result ? 1 : 0, &trap);
    if (error != NULL || trap != NULL) {
        ngx_wasm_wasmtime_report_error(ngx_cycle->log, "failed to call function: ", error, trap);
        return NGX_ERROR;
    }

    if (!has_result) {
        ngx_log_debug0(NGX_LOG_DEBUG_CORE, ngx_cycle->log, 0,
                       "wasmtime call function done");
        return NGX_OK;
    }

    if (results[0].kind != WASMTIME_I32) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "function returns unexpected type: %d",
                      results[0].kind);
        return NGX_ERROR;
    }

    rc = results[0].of.i32;
    ngx_log_debug1(NGX_LOG_DEBUG_CORE, ngx_cycle->log, 0,
                   "wasmtime call function result: %d", rc);

    return rc;
}


ngx_wasm_vm_t ngx_wasm_vm = {
    &vm_name,
    ngx_wasm_wasmtime_init,
    ngx_wasm_wasmtime_cleanup,
    ngx_wasm_wasmtime_load,
    ngx_wasm_wasmtime_unload,
    ngx_wasm_wasmtime_call,
};