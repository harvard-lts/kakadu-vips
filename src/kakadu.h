/* Common definitions for kakadu load and save.
 */

// object init
extern "C" {
GType vips_foreign_load_kakadu_file_get_type(void);
GType vips_foreign_load_kakadu_buffer_get_type(void);
GType vips_foreign_load_kakadu_source_get_type(void);
GType vips_foreign_save_kakadu_file_get_type(void);
GType vips_foreign_save_kakadu_buffer_get_type(void);
GType vips_foreign_save_kakadu_target_get_type(void);
}

// C API wrappers
extern "C" {
int vips_kakaduload(const char *filename, VipsImage **out, ...);
int vips_kakaduload_buffer(void *buf, size_t len, VipsImage **out, ...);
int vips_kakaduload_source(VipsSource *source, VipsImage **out, ...);
int vips_kakadusave(VipsImage *in, const char *filename, ...);
int vips_kakadusave_buffer(VipsImage *in, void **buf, size_t *len, ...);
int vips_kakadusave_target(VipsImage *in, VipsTarget *target, ...);
}

