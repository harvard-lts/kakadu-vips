/* load jpeg2000
 */

/*
 */
#define DEBUG_VERBOSE
#define DEBUG

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vips/vips.h>

// Kakadu core includes
#include "kdu_elementary.h"
#include "kdu_messaging.h"
#include "kdu_params.h"
#include "kdu_compressed.h"
#include "kdu_sample_processing.h"
#include "kdu_block_coding.h"
#include "kdu_arch.h"

using namespace kdu_core;

// i18n placeholder
#define _(S) (S)

typedef struct _VipsForeignLoadKakadu {
	VipsForeignLoad parent_object;

	/* Source to load from (set by subclasses).
	 */
	VipsSource *source;

	/* Page set by user, then we translate that into shrink factor.
	 */
	int page;
	int shrink;

	/* Decompress state.
	opj_stream_t *stream;			// Source as an opj stream 
	OPJ_CODEC_FORMAT format;		// libopenjp2 format 
	opj_codec_t *codec;				// Decompress codec 
	opj_dparameters_t parameters;	// Core decompress params 
	opj_image_t *image;				// Read image to here 
	opj_codestream_info_v2_t *info; // Tile geometry 
	 */

	/* Number of errors reported during load -- use this to block load of
	 * corrupted images.
	 */
	int n_errors;

	/* If we need to upsample tiles read from opj.
	 */
	gboolean upsample;

	/* If we need to do ycc->rgb conversion on load.
	 */
	gboolean ycc_to_rgb;
} VipsForeignLoadKakadu;

typedef VipsForeignLoadClass VipsForeignLoadKakaduClass;

extern "C" {
	G_DEFINE_ABSTRACT_TYPE(VipsForeignLoadKakadu, vips_foreign_load_kakadu,
		VIPS_TYPE_FOREIGN_LOAD);
}

static void
vips_foreign_load_kakadu_dispose(GObject *gobject)
{
	VipsForeignLoadKakadu *kakadu = (VipsForeignLoadKakadu *) gobject;

#ifdef DEBUG
	printf("vips_foreign_load_kakadu_dispose:\n");
#endif /*DEBUG*/

	/*
	 * FIXME ... do we need this? seems to just cause warnings
	 *
	if (kakadu->codec &&
		kakadu->stream)
		opj_end_decompress(kakadu->codec, kakadu->stream);
	 *
	 */

	/*
	if (kakadu->info)
		opj_destroy_cstr_info(&kakadu->info);
	VIPS_FREEF(opj_destroy_codec, kakadu->codec);
	VIPS_FREEF(opj_stream_destroy, kakadu->stream);
	VIPS_FREEF(opj_image_destroy, kakadu->image);
	 */

	VIPS_UNREF(kakadu->source);

	G_OBJECT_CLASS(vips_foreign_load_kakadu_parent_class)->dispose(gobject);
}

static size_t
vips_foreign_load_kakadu_read_source(void *buffer, size_t length, void *client)
{
	VipsSource *source = VIPS_SOURCE(client);
	gint64 bytes_read = vips_source_read(source, buffer, length);

	/* openjpeg read uses -1 for both EOF and error return.
	 */
	return bytes_read == 0 ? -1 : bytes_read;
}

static off_t
vips_foreign_load_kakadu_skip_source(off_t n_bytes, void *client)
{
	VipsSource *source = VIPS_SOURCE(client);

	if (vips_source_seek(source, n_bytes, SEEK_CUR) == -1)
		/* openjpeg skip uses -1 for both end of stream and error.
		 */
		return -1;

	return n_bytes;
}

static bool
vips_foreign_load_kakadu_seek_source(off_t position, void *client)
{
	VipsSource *source = VIPS_SOURCE(client);

	if (vips_source_seek(source, position, SEEK_SET) == -1)
		/* openjpeg seek uses FALSE for both end of stream and error.
		 */
		return FALSE;

	return TRUE;
}

static int
vips_foreign_load_kakadu_build(VipsObject *object)
{
	VipsObjectClass *klass = VIPS_OBJECT_GET_CLASS(object);
	VipsForeignLoadKakadu *kakadu = (VipsForeignLoadKakadu *) object;

#ifdef DEBUG
	printf("vips_foreign_load_kakadu_build:\n");
#endif /*DEBUG*/

	/*
static void
set_error_behaviour(kdu_args &args, kdu_codestream codestream)
{
  bool fussy = false;
  bool resilient = false;
  bool ubiquitous_sops = false;
  if (args.find("-fussy") != NULL)
	{ args.advance(); fussy = true; }
  if (args.find("-resilient") != NULL)
	{ args.advance(); resilient = true; }
  if (args.find("-resilient_sop") != NULL)
	{ args.advance(); resilient = true; ubiquitous_sops = true; }
  if (resilient)
	codestream.set_resilient(ubiquitous_sops);
  else if (fussy)
	codestream.set_fussy();
  else
	codestream.set_fast();
}
	 */

	/* Default parameters.
	kakadu->parameters.decod_format = -1;
	kakadu->parameters.cod_format = -1;
	opj_set_default_decoder_parameters(&kakadu->parameters);
	 */

	/* Link the openjpeg stream to our VipsSource.
	if (kakadu->source) {
		kakadu->stream = vips_foreign_load_kakadu_stream(kakadu->source);
		if (!kakadu->stream) {
			vips_error(class->nickname,
				"%s", _("unable to create kakadu stream"));
			return -1;
		}
	}
	 */

	if (VIPS_OBJECT_CLASS(vips_foreign_load_kakadu_parent_class)->build(object))
		return -1;

	return 0;
}

#define JP2_RFC3745_MAGIC "\x00\x00\x00\x0c\x6a\x50\x20\x20\x0d\x0a\x87\x0a"
#define JP2_MAGIC "\x0d\x0a\x87\x0a"
/* position 45: "\xff\x52" */
#define J2K_CODESTREAM_MAGIC "\xff\x4f\xff\x51"

/* Return the image format. OpenJPEG supports several different image types.
 */
static const char *
vips_foreign_load_kakadu_get_format(VipsSource *source)
{
	unsigned char *data;

	if (vips_source_sniff_at_most(source, &data, 12) < 12)
		return NULL;

	/* There's also OPJ_CODEC_JPT for xxx.jpt files, but we don't support
	 * that.
	 */
	if (memcmp(data, JP2_RFC3745_MAGIC, 12) == 0 ||
		memcmp(data, JP2_MAGIC, 4) == 0)
		return "is jp2";
	else if (memcmp(data, J2K_CODESTREAM_MAGIC, 4) == 0)
		return "is plain codec";
	else
		return NULL;
}

static gboolean
vips_foreign_load_kakadu_is_a_source(VipsSource *source)
{
	return vips_foreign_load_kakadu_get_format(source) != NULL;
}

static VipsForeignFlags
vips_foreign_load_kakadu_get_flags(VipsForeignLoad *load)
{
	return VIPS_FOREIGN_PARTIAL;
}

/*
class kdu_stream_message : public kdu_thread_safe_message {
  public: // Member classes
	kdu_stream_message(std::ostream *stream)
	  { this->stream = stream; }
	void put_text(const char *string)
	  { (*stream) << string; }
	void flush(bool end_of_message=false)
	  { stream->flush();
		kdu_thread_safe_message::flush(end_of_message); }
  private: // Data
	std::ostream *stream;
};

static kdu_stream_message cout_message(&std::cout);
static kdu_stream_message cerr_message(&std::cerr);
static kdu_message_formatter pretty_cout(&cout_message);
static kdu_message_formatter pretty_cerr(&cerr_message);
 */

static int
vips_foreign_load_kakadu_set_header(VipsForeignLoadKakadu *kakadu, 
	VipsImage *out)
{
	VipsObjectClass *klass = VIPS_OBJECT_GET_CLASS(kakadu);

#ifdef DEBUG
	printf("vips_foreign_load_kakadu_set_header:\n");
#endif /*DEBUG*/

	/*
	if (vips_image_pipelinev(out, VIPS_DEMAND_STYLE_SMALLTILE, NULL))
		return -1;

	vips_image_init_fields(out,
		first->w, first->h, kakadu->image->numcomps, format,
		VIPS_CODING_NONE, interpretation, 1.0, 1.0);

	out->Xoffset =
		-VIPS_ROUND_INT((double) kakadu->image->x0 / kakadu->shrink);
	out->Yoffset =
		-VIPS_ROUND_INT((double) kakadu->image->y0 / kakadu->shrink);

	if (kakadu->image->icc_profile_buf &&
		kakadu->image->icc_profile_len > 0)
		vips_image_set_blob_copy(out, VIPS_META_ICC_NAME,
			kakadu->image->icc_profile_buf,
			kakadu->image->icc_profile_len);

	if (kakadu->info &&
		kakadu->info->m_default_tile_info.tccp_info)
		vips_image_set_int(out, VIPS_META_N_PAGES,
			kakadu->info->m_default_tile_info.tccp_info->numresolutions);

	vips_image_set_int(out, VIPS_META_BITS_PER_SAMPLE, first->prec);
	 */

	return 0;
}

static int
vips_foreign_load_kakadu_header(VipsForeignLoad *load)
{
	VipsObjectClass *klass = VIPS_OBJECT_GET_CLASS(load);
	VipsForeignLoadKakadu *kakadu = (VipsForeignLoadKakadu *) load;

	int i;

#ifdef DEBUG
	printf("vips_foreign_load_kakadu_header:\n");
#endif /*DEBUG*/

	vips_source_rewind(kakadu->source);

	/*
	if (!(kakadu->codec = opj_create_decompress(kakadu->format)))
		return -1;

	vips_foreign_load_kakadu_attach_handlers(kakadu, kakadu->codec);
	 */

	if (vips_foreign_load_kakadu_set_header(kakadu, load->out))
		return -1;

	VIPS_SETSTR(load->out->filename,
		vips_connection_filename(VIPS_CONNECTION(kakadu->source)));

	return 0;
}

/*
static kdu_long
  expand_single_threaded(kdu_codestream codestream, kdu_dims tile_indices,
						 kde_file_binding *outputs, int num_output_channels,
						 bool last_output_channel_is_alpha,
						 bool alpha_is_premultiplied,
						 int num_used_components, int *used_component_indices,
						 jp2_channels channels, jp2_palette palette,
						 bool allow_shorts, bool want_fastest,
						 bool skip_ycc, int dwt_stripe_height,
						 int num_stats_layers, int num_stats_resolutions,
						 kdu_long *stats_reslayer_bytes,
						 kdu_long *stats_reslayer_packets,
						 kdu_long *stats_resmax_packets,
						 int progress_interval, kdu_membroker *membroker,
						 kdu_push_pull_params *extra_params)
{
  int x_tnum;
  kde_flow_control **tile_flows = new kde_flow_control *[tile_indices.size.x];
  for (x_tnum=0; x_tnum < tile_indices.size.x; x_tnum++)
	{
	  tile_flows[x_tnum] = new
		kde_flow_control(outputs,num_output_channels,
						 last_output_channel_is_alpha,alpha_is_premultiplied,
						 num_used_components,used_component_indices,
						 codestream,x_tnum,allow_shorts,want_fastest,
						 channels,palette,skip_ycc,dwt_stripe_height,
						 false,NULL,NULL,membroker,extra_params);
	  if (num_stats_layers > 0)
		tile_flows[x_tnum]->collect_layer_stats(num_stats_layers,
												num_stats_resolutions,
												num_stats_resolutions-1,
												stats_reslayer_bytes,
												stats_reslayer_packets,
												stats_resmax_packets,NULL);
	}
  bool done = false;
  int tile_row = 0; // Just for progress counter
  int progress_counter = 0;
  while (!done)
	{
	  while (!done)
		{ // Process a row of tiles line by line.
		  done = true;
		  for (x_tnum=0; x_tnum < tile_indices.size.x; x_tnum++)
			{
			  if (tile_flows[x_tnum]->advance_components())
				{
				  done = false;
				  tile_flows[x_tnum]->process_components();
				}
			}
		  if ((!done) && ((++progress_counter) == progress_interval))
			{
			  pretty_cout << "\t\tProgress with current tile row = "
						  << tile_flows[0]->percent_pulled() << "%\n";
			  progress_counter = 0;
			}
		}
	  for (x_tnum=0; x_tnum < tile_indices.size.x; x_tnum++)
		if (tile_flows[x_tnum]->advance_tile())
		  done = false;
	  tile_row++;
	  progress_counter = 0;
	  if (progress_interval > 0)
		pretty_cout << "\tFinished processing " << tile_row
					<< " of " << tile_indices.size.y << " tile rows\n";
	}
  kdu_long processing_sample_bytes = 0;
  for (x_tnum=0; x_tnum < tile_indices.size.x; x_tnum++)
	{
	  processing_sample_bytes += tile_flows[x_tnum]->get_buffer_memory();
	  delete tile_flows[x_tnum];
	}
  delete[] tile_flows;

  return processing_sample_bytes;
}
 */

/* Read a tile from the file. libvips tiles can be much larger or smaller than
 * jp2k tiles, so we must loop over the output region, painting in
 * tiles from the file.
 */
static int
vips_foreign_load_kakadu_generate_tiled(VipsRegion *out,
	void *seq, void *a, void *b, gboolean *stop)
{
	VipsForeignLoad *load = (VipsForeignLoad *) a;
	VipsForeignLoadKakadu *kakadu = (VipsForeignLoadKakadu *) load;
	VipsRect *r = &out->valid;

#ifdef DEBUG_VERBOSE
	printf("vips_foreign_load_kakadu_generate: "
		   "left = %d, top = %d, width = %d, height = %d\n",
		r->left, r->top, r->width, r->height);
#endif /*DEBUG_VERBOSE*/

	return 0;
}

static int
vips_foreign_load_kakadu_load(VipsForeignLoad *load)
{
	VipsForeignLoadKakadu *kakadu = (VipsForeignLoadKakadu *) load;
	VipsImage **t = (VipsImage **)
		vips_object_local_array(VIPS_OBJECT(load), 3);

	int vips_tile_width;
	int vips_tile_height;
	int vips_tiles_across;

#ifdef DEBUG
	printf("vips_foreign_load_kakadu_load:\n");
#endif /*DEBUG*/

	t[0] = vips_image_new();
	if (vips_foreign_load_kakadu_set_header(kakadu, t[0]))
		return -1;

	// fix this
	vips_tile_width = 512;
	vips_tile_height = 512;
	vips_tiles_across = 10;

	if (vips_image_generate(t[0],
		NULL, vips_foreign_load_kakadu_generate_tiled, NULL,
		kakadu, NULL))
		return -1;

	/* Copy to out, adding a cache. Enough tiles for two complete
	 * rows, plus 50%.
	 */
	if (vips_tilecache(t[0], &t[1],
		"tile_width", vips_tile_width,
		"tile_height", vips_tile_height,
		"max_tiles", 3 * vips_tiles_across,
		NULL))
		return -1;

	if (vips_image_write(t[1], load->real))
		return -1;

	return 0;
}

/* We want to be able to OR flag enums.
 */
inline VipsOperationFlags 
operator|(VipsOperationFlags a, VipsOperationFlags b)
{
    return (VipsOperationFlags)((int) a | (int) b);
}

inline VipsOperationFlags &
operator|=(VipsOperationFlags &a, const VipsOperationFlags b)
{
    a = a | b;

    return a;
}

static void
vips_foreign_load_kakadu_class_init(VipsForeignLoadKakaduClass *klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
	VipsObjectClass *object_class = (VipsObjectClass *) klass;
	VipsOperationClass *operation_class = VIPS_OPERATION_CLASS(klass);
	VipsForeignLoadClass *load_class = (VipsForeignLoadClass *) klass;

	gobject_class->dispose = vips_foreign_load_kakadu_dispose;
	gobject_class->set_property = vips_object_set_property;
	gobject_class->get_property = vips_object_get_property;

	object_class->nickname = "kakaduload_base";
	object_class->description = _("load JPEG2000 image");
	object_class->build = vips_foreign_load_kakadu_build;

	// no idea how secure kakadu is ... is it fuzzed?
	operation_class->flags |= VIPS_OPERATION_UNTRUSTED;

	load_class->get_flags = vips_foreign_load_kakadu_get_flags;
	load_class->header = vips_foreign_load_kakadu_header;
	load_class->load = vips_foreign_load_kakadu_load;

	VIPS_ARG_INT(klass, "page", 20,
		_("Page"),
		_("Load this page from the image"),
		VIPS_ARGUMENT_OPTIONAL_INPUT,
		G_STRUCT_OFFSET(VipsForeignLoadKakadu, page),
		0, 100000, 0);
}

static void
vips_foreign_load_kakadu_init(VipsForeignLoadKakadu *kakadu)
{
}

typedef struct _VipsForeignLoadKakaduFile {
	VipsForeignLoadKakadu parent_object;

	/* Filename for load.
	 */
	char *filename;

} VipsForeignLoadKakaduFile;

typedef VipsForeignLoadKakaduClass VipsForeignLoadKakaduFileClass;

extern "C" {
	G_DEFINE_TYPE(VipsForeignLoadKakaduFile, vips_foreign_load_kakadu_file,
		vips_foreign_load_kakadu_get_type());
}

static int
vips_foreign_load_kakadu_file_build(VipsObject *object)
{
	VipsForeignLoadKakadu *kakadu = (VipsForeignLoadKakadu *) object;
	VipsForeignLoadKakaduFile *file = (VipsForeignLoadKakaduFile *) object;

	if (file->filename &&
		!(kakadu->source = vips_source_new_from_file(file->filename)))
		return -1;

	if (VIPS_OBJECT_CLASS(vips_foreign_load_kakadu_file_parent_class)
			->build(object))
		return -1;

	return 0;
}

const char *vips__kakadu_suffs[] = {
	".j2k", ".jp2", ".jpt", ".j2c", ".jpc", NULL
};

static int
vips_foreign_load_kakadu_is_a(const char *filename)
{
	VipsSource *source;
	gboolean result;

	if (!(source = vips_source_new_from_file(filename)))
		return FALSE;
	result = vips_foreign_load_kakadu_is_a_source(source);
	VIPS_UNREF(source);

	return result;
}

static void
vips_foreign_load_kakadu_file_class_init(VipsForeignLoadKakaduFileClass *klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
	VipsObjectClass *object_class = (VipsObjectClass *) klass;
	VipsForeignClass *foreign_class = (VipsForeignClass *) klass;
	VipsForeignLoadClass *load_class = (VipsForeignLoadClass *) klass;

	gobject_class->set_property = vips_object_set_property;
	gobject_class->get_property = vips_object_get_property;

	object_class->nickname = "kakaduload";
	object_class->build = vips_foreign_load_kakadu_file_build;

	foreign_class->suffs = vips__kakadu_suffs;

	load_class->is_a = vips_foreign_load_kakadu_is_a;

	VIPS_ARG_STRING(klass, "filename", 1,
		_("Filename"),
		_("Filename to load from"),
		VIPS_ARGUMENT_REQUIRED_INPUT,
		G_STRUCT_OFFSET(VipsForeignLoadKakaduFile, filename),
		NULL);
}

static void
vips_foreign_load_kakadu_file_init(VipsForeignLoadKakaduFile *kakadu)
{
}

typedef struct _VipsForeignLoadKakaduBuffer {
	VipsForeignLoadKakadu parent_object;

	/* Load from a buffer.
	 */
	VipsArea *buf;

} VipsForeignLoadKakaduBuffer;

typedef VipsForeignLoadKakaduClass VipsForeignLoadKakaduBufferClass;

extern "C" {
	G_DEFINE_TYPE(VipsForeignLoadKakaduBuffer, vips_foreign_load_kakadu_buffer,
		vips_foreign_load_kakadu_get_type());
}

static int
vips_foreign_load_kakadu_buffer_build(VipsObject *object)
{
	VipsForeignLoadKakadu *kakadu = (VipsForeignLoadKakadu *) object;
	VipsForeignLoadKakaduBuffer *buffer =
		(VipsForeignLoadKakaduBuffer *) object;

	if (buffer->buf)
		if (!(kakadu->source = vips_source_new_from_memory(
				  VIPS_AREA(buffer->buf)->data,
				  VIPS_AREA(buffer->buf)->length)))
			return -1;

	if (VIPS_OBJECT_CLASS(vips_foreign_load_kakadu_file_parent_class)
			->build(object))
		return -1;

	return 0;
}

static gboolean
vips_foreign_load_kakadu_buffer_is_a(const void *buf, size_t len)
{
	VipsSource *source;
	gboolean result;

	if (!(source = vips_source_new_from_memory(buf, len)))
		return FALSE;
	result = vips_foreign_load_kakadu_is_a_source(source);
	VIPS_UNREF(source);

	return result;
}

static void
vips_foreign_load_kakadu_buffer_class_init(
    VipsForeignLoadKakaduBufferClass *klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
	VipsObjectClass *object_class = (VipsObjectClass *) klass;
	VipsForeignLoadClass *load_class = (VipsForeignLoadClass *) klass;

	gobject_class->set_property = vips_object_set_property;
	gobject_class->get_property = vips_object_get_property;

	object_class->nickname = "kakaduload_buffer";
	object_class->build = vips_foreign_load_kakadu_buffer_build;

	load_class->is_a_buffer = vips_foreign_load_kakadu_buffer_is_a;

	VIPS_ARG_BOXED(klass, "buffer", 1,
		_("Buffer"),
		_("Buffer to load from"),
		VIPS_ARGUMENT_REQUIRED_INPUT,
		G_STRUCT_OFFSET(VipsForeignLoadKakaduBuffer, buf),
		VIPS_TYPE_BLOB);
}

static void
vips_foreign_load_kakadu_buffer_init(VipsForeignLoadKakaduBuffer *buffer)
{
}

typedef struct _VipsForeignLoadKakaduSource {
	VipsForeignLoadKakadu parent_object;

	/* Load from a source.
	 */
	VipsSource *source;

} VipsForeignLoadKakaduSource;

typedef VipsForeignLoadKakaduClass VipsForeignLoadKakaduSourceClass;

extern "C" {
	G_DEFINE_TYPE(VipsForeignLoadKakaduSource, vips_foreign_load_kakadu_source,
		vips_foreign_load_kakadu_get_type());
}

static int
vips_foreign_load_kakadu_source_build(VipsObject *object)
{
	VipsForeignLoadKakadu *kakadu = (VipsForeignLoadKakadu *) object;
	VipsForeignLoadKakaduSource *source =
		(VipsForeignLoadKakaduSource *) object;

	if (source->source) {
		kakadu->source = source->source;
		g_object_ref(kakadu->source);
	}

	if (VIPS_OBJECT_CLASS(vips_foreign_load_kakadu_source_parent_class)
			->build(object))
		return -1;

	return 0;
}

static void
vips_foreign_load_kakadu_source_class_init(
	VipsForeignLoadKakaduSourceClass *klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
	VipsObjectClass *object_class = (VipsObjectClass *) klass;
	VipsOperationClass *operation_class = VIPS_OPERATION_CLASS(klass);
	VipsForeignLoadClass *load_class = (VipsForeignLoadClass *) klass;

	gobject_class->set_property = vips_object_set_property;
	gobject_class->get_property = vips_object_get_property;

	object_class->nickname = "kakaduload_source";
	object_class->build = vips_foreign_load_kakadu_source_build;

	operation_class->flags |= VIPS_OPERATION_NOCACHE;

	load_class->is_a_source = vips_foreign_load_kakadu_is_a_source;

	VIPS_ARG_OBJECT(klass, "source", 1,
		_("Source"),
		_("Source to load from"),
		VIPS_ARGUMENT_REQUIRED_INPUT,
		G_STRUCT_OFFSET(VipsForeignLoadKakaduSource, source),
		VIPS_TYPE_SOURCE);
}

static void
vips_foreign_load_kakadu_source_init(
	VipsForeignLoadKakaduSource *kakadu)
{
}

extern "C" {

/**
 * vips_kakaduload:
 * @filename: file to load
 * @out: (out): decompressed image
 * @...: %NULL-terminated list of optional named arguments
 *
 * Optional arguments:
 *
 * * @page: %gint, load this page
 * * @fail_on: #VipsFailOn, types of read error to fail on
 *
 * Read a JPEG2000 image. The loader supports 8, 16 and 32-bit int pixel
 * values, signed and unsigned. It supports greyscale, RGB, YCC, CMYK and
 * multispectral colour spaces. It will read any ICC profile on the image.
 *
 * It will only load images where all channels have the same format.
 *
 * Use @page to set the page to load, where page 0 is the base resolution
 * image and higher-numbered pages are x2 reductions. Use the metadata item
 * "n-pages" to find the number of pyramid layers.
 *
 * Use @fail_on to set the type of error that will cause load to fail. By
 * default, loaders are permissive, that is, #VIPS_FAIL_ON_NONE.
 *
 * See also: vips_image_new_from_file().
 *
 * Returns: 0 on success, -1 on error.
 */
int
vips_kakaduload(const char *filename, VipsImage **out, ...)
{
	va_list ap;
	int result;

	va_start(ap, out);
	result = vips_call_split("kakaduload", ap, filename, out);
	va_end(ap);

	return result;
}

/**
 * vips_kakaduload_buffer:
 * @buf: (array length=len) (element-type guint8): memory area to load
 * @len: (type gsize): size of memory area
 * @out: (out): image to write
 * @...: %NULL-terminated list of optional named arguments
 *
 * Optional arguments:
 *
 * * @page: %gint, load this page
 * * @fail_on: #VipsFailOn, types of read error to fail on
 *
 * Exactly as vips_kakaduload(), but read from a buffer.
 *
 * You must not free the buffer while @out is active. The
 * #VipsObject::postclose signal on @out is a good place to free.
 *
 * Returns: 0 on success, -1 on error.
 */
int
vips_kakaduload_buffer(void *buf, size_t len, VipsImage **out, ...)
{
	va_list ap;
	VipsBlob *blob;
	int result;

	/* We don't take a copy of the data or free it.
	 */
	blob = vips_blob_new(NULL, buf, len);

	va_start(ap, out);
	result = vips_call_split("kakaduload_buffer", ap, blob, out);
	va_end(ap);

	vips_area_unref(VIPS_AREA(blob));

	return result;
}

/**
 * vips_kakaduload_source:
 * @source: source to load from
 * @out: (out): decompressed image
 * @...: %NULL-terminated list of optional named arguments
 *
 * Optional arguments:
 *
 * * @page: %gint, load this page
 * * @fail_on: #VipsFailOn, types of read error to fail on
 *
 * Exactly as vips_kakaduload(), but read from a source.
 *
 * Returns: 0 on success, -1 on error.
 */
int
vips_kakaduload_source(VipsSource *source, VipsImage **out, ...)
{
	va_list ap;
	int result;

	va_start(ap, out);
	result = vips_call_split("kakaduload_source", ap, source, out);
	va_end(ap);

	return result;
}

}
