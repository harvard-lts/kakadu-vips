/* Plugin init.
 */

#include <vips/vips.h>

#include "kakadu.h"

extern "C" {
const gchar *
g_module_check_init(GModule *module)
{
	vips_foreign_load_kakadu_file_get_type();
	vips_foreign_load_kakadu_buffer_get_type();
	vips_foreign_load_kakadu_source_get_type();
	//vips_foreign_save_kakadu_file_get_type();
	//vips_foreign_save_kakadu_buffer_get_type();
	//vips_foreign_save_kakadu_target_get_type();

	g_module_make_resident(module);

	return NULL; 
}
}
