/* Common definitions for kakadu load and save.
 */

#include <iostream>

#include <vector>
#include <string>
#include <numeric>

#include <jpx.h>
#include <jp2.h>

#include <kdu_elementary.h>
#include <kdu_messaging.h>
#include <kdu_params.h>
#include <kdu_compressed.h>
#include <kdu_sample_processing.h>
#include <kdu_utils.h>
#include <kdu_file_io.h>

// i18n placeholder
#define _(S) (S)

#define DELETE(P) \
G_STMT_START \
    { \
        if (P) { \
            delete (P); \
            (P) = NULL; \
        } \
    } \
G_STMT_END

// libvips 8.14 compatibility
#ifndef VIPS_META_BITS_PER_SAMPLE
#define VIPS_META_BITS_PER_SAMPLE "bits-per-sample"
#endif

extern const char *vips__kakadu_suffs[];

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

class VipsForeignKakaduError : public kdu_core::kdu_thread_safe_message {
public:
    void 
	put_text(const char *string)
	{
		strings.push_back(std::string(string));
	}

    void 
	flush(bool end_of_message)
	{
		if (end_of_message) {
			vips_error("VipsForeignKakadu", "%s", 
				std::accumulate(strings.begin(), 
					            strings.end(), 
					            std::string("")).c_str());

			// without an exception, the process will be terminated when this
			// call returns ... we must throw something of type
			// kdu_exception (an int)
			throw -1;
		}

		kdu_thread_safe_message::flush(end_of_message);
	}

private:
    std::vector<std::string> strings;
};

class VipsForeignKakaduWarn : public kdu_core::kdu_thread_safe_message {
public:
    void 
	put_text(const char *string)
	{
		strings.push_back(string);
	}

    void 
	flush(bool end_of_message)
	{
		if (end_of_message) {
			g_warning("%s", std::accumulate(strings.begin(), 
				   			                strings.end(), 
							                std::string("")).c_str());
			strings.clear();
		}

		kdu_thread_safe_message::flush(end_of_message);
	}

private:
    std::vector<std::string> strings;
};

extern kdu_core::kdu_message_formatter vips_foreign_kakadu_error_handler;
extern kdu_core::kdu_message_formatter vips_foreign_kakadu_warn_handler;
