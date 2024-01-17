/* load jpeg2000
 */

/*
#define DEBUG_VERBOSE
 */
#define DEBUG

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vips/vips.h>

#include "kakadu.h"

#include <kdu_region_decompressor.h>

using namespace kdu_supp; // includes the core namespace

/* A VipsSource as a Kakadu input object. This keeps the reference
 * alive while it's alive.
 */
class VipsKakaduSource : public kdu_compressed_source {
public:
	VipsKakaduSource(VipsSource *_source)
	{
		source = _source;
		g_object_ref(source);
	}

	~VipsKakaduSource()
	{
#ifdef DEBUG
		printf("~VipsKakaduSource:\n");
#endif /*DEBUG*/

		VIPS_UNREF(source);
	}

	virtual int get_capabilities() 
	{
		return KDU_SOURCE_CAP_SEQUENTIAL | KDU_SOURCE_CAP_SEEKABLE; 
	}

	virtual bool seek(kdu_long offset)
	{
#ifdef DEBUG_VERBOSE
		printf("VipsKakaduSource: seek(%lld)\n", offset);
#endif /*DEBUG_VERBOSE*/

		// kakadu assumes this will always succeed
		(void) vips_source_seek(source, offset, SEEK_SET);

		return true;
	}

	virtual kdu_long get_pos()
	{
		 return vips_source_seek(source, 0, SEEK_CUR);
	}

	virtual int read(kdu_byte *buf, int num_bytes)
	{
		gint64 bytes_read = vips_source_read(source, buf, num_bytes);

#ifdef DEBUG_VERBOSE
		printf("VipsKakaduSource: read(%d) = %ld\n", num_bytes, bytes_read);
#endif /*DEBUG_VERBOSE*/

		return bytes_read;
	}

	void rewind()
	{
		vips_source_rewind(source);
	}

	virtual bool close()
	{
#ifdef DEBUG
		printf("VipsKakaduSource: close()\n");
#endif /*DEBUG*/

		VIPS_UNREF(source);
		return true;
	}

private:
	VipsSource *source;
};

static VipsForeignKakaduError vips_foreign_kakadu_error;
static VipsForeignKakaduWarn vips_foreign_kakadu_warn;

kdu_message_formatter 
	vips_foreign_kakadu_error_handler(&vips_foreign_kakadu_error);
kdu_message_formatter 
	vips_foreign_kakadu_warn_handler(&vips_foreign_kakadu_warn);

typedef struct _VipsForeignLoadKakadu {
	VipsForeignLoad parent_object;

	/* Source to load from (set by subclasses).
	 */
	VipsSource *vips_source;

	/* "source" wrapped up as a kakadu byte data source.
	 */
	VipsKakaduSource *kakadu_source;

	/* Page set by user, then we translate that into shrink factor.
	 */
	int page;
	int shrink;

	/* The kakadu input objects.
	 */
	jp2_family_src *input;
	jpx_source *source;

	/* Refs to parts of the input object we discover.
	 */
	jpx_codestream_source codestream_source;
	jpx_layer_source layer;
	jp2_channels channels;
	jp2_palette palette;
	jp2_resolution resolution;
	jp2_colour colour;
	kdu_coords layer_size;
	jp2_dimensions dimensions;

	/* Needed during tile generation.
	 */
	kdu_codestream codestream;
	int *channel_offsets;
	kdu_channel_mapping *channel_mapping;
	kdu_region_decompressor *region_decompressor;

	/* kakadu colour mapping.
	 */
	int cmp;
	int lut;
	int stream_id;
	int fmt;

	/* Detected image properties.
	 */
	int width;
	int height;
	int tile_width;
	int tile_height;
	int bands;
	int bits_per_sample;
	int n_pages;
	VipsBandFormat format;
	VipsInterpretation interpretation;
	double xres;
	double yres;

	/* Number of errors reported during load -- use this to block load of
	 * corrupted images.
	 */
	int n_errors;
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

	DELETE(kakadu->channel_mapping);
	DELETE(kakadu->region_decompressor);
	DELETE(kakadu->input);
	DELETE(kakadu->source);
	DELETE(kakadu->kakadu_source);

	VIPS_FREE(kakadu->channel_offsets);

	VIPS_UNREF(kakadu->vips_source);

	G_OBJECT_CLASS(vips_foreign_load_kakadu_parent_class)->dispose(gobject);
}

static int
vips_foreign_load_kakadu_build(VipsObject *object)
{
	VipsForeignLoadKakadu *kakadu = (VipsForeignLoadKakadu *) object;

#ifdef DEBUG
	printf("vips_foreign_load_kakadu_build:\n");
#endif /*DEBUG*/

	// install error and warn messages
	kdu_customize_errors(&vips_foreign_kakadu_error_handler);
	kdu_customize_warnings(&vips_foreign_kakadu_warn_handler);

	kakadu->kakadu_source = new VipsKakaduSource(kakadu->vips_source);

	// read bytes and image data into these
	kakadu->input = new jp2_family_src();
	kakadu->source = new jpx_source();

	if (VIPS_OBJECT_CLASS(vips_foreign_load_kakadu_parent_class)->
			build(object))
		return -1;

	// we use page to set the reduction factor so we work simply with
	// "thumbnail"
	kakadu->shrink = 1 << kakadu->page;

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

static int
vips_foreign_load_kakadu_set_header(VipsForeignLoadKakadu *kakadu, 
	VipsImage *out)
{
#ifdef DEBUG
	printf("vips_foreign_load_kakadu_set_header:\n");
#endif /*DEBUG*/

	if (vips_image_pipelinev(out, VIPS_DEMAND_STYLE_SMALLTILE, NULL))
		return -1;

	vips_image_init_fields(out, 
			kakadu->width, kakadu->height, 
			kakadu->bands, 
			kakadu->format,
			VIPS_CODING_NONE, 
			kakadu->interpretation, 
			kakadu->xres, 
			kakadu->yres);

	kdu_dims dims;
	kakadu->codestream.get_dims(-1, dims);
	out->Xoffset = dims.access_pos()->x;
	out->Yoffset = dims.access_pos()->y;

	int num_bytes;
	const kdu_byte *data = kakadu->colour.get_icc_profile(&num_bytes);
	if (data && num_bytes > 0)
		vips_image_set_blob_copy(out, VIPS_META_ICC_NAME, data, num_bytes);

	vips_image_set_int(out, VIPS_META_N_PAGES, kakadu->n_pages);
	vips_image_set_int(out, 
			VIPS_META_BITS_PER_SAMPLE, kakadu->bits_per_sample);

	return 0;
}

#ifdef DEBUG
static const char *
space2string(jp2_colour_space space)
{
	switch(space) {
	case JP2_EMPTY_SPACE: 		return "JP2_EMPTY_SPACE";
	case JP2_bilevel1_SPACE: 	return "JP2_bilevel1_SPACE";
	case JP2_YCbCr1_SPACE: 		return "JP2_YCbCr1_SPACE";
	case JP2_YCbCr2_SPACE: 		return "JP2_YCbCr2_SPACE";
	case JP2_YCbCr3_SPACE: 		return "JP2_YCbCr3_SPACE";
	case JP2_PhotoYCC_SPACE: 	return "JP2_PhotoYCC_SPACE";
	case JP2_CMY_SPACE: 		return "JP2_CMY_SPACE";
	case JP2_CMYK_SPACE: 		return "JP2_CMYK_SPACE";
	case JP2_YCCK_SPACE: 		return "JP2_YCCK_SPACE";
	case JP2_CIELab_SPACE: 		return "JP2_CIELab_SPACE";
	case JP2_bilevel2_SPACE: 	return "JP2_bilevel2_SPACE";
	case JP2_sRGB_SPACE: 		return "JP2_sRGB_SPACE";
	case JP2_sLUM_SPACE: 		return "JP2_sLUM_SPACE";
	case JP2_sYCC_SPACE: 		return "JP2_sYCC_SPACE";
	case JP2_CIEJab_SPACE: 		return "JP2_CIEJab_SPACE";
	case JP2_esRGB_SPACE: 		return "JP2_esRGB_SPACE";
	case JP2_ROMMRGB_SPACE: 	return "JP2_ROMMRGB_SPACE";
	case JP2_YPbPr60_SPACE: 	return "JP2_YPbPr60_SPACE";
	case JP2_YPbPr50_SPACE: 	return "JP2_YPbPr50_SPACE";
	case JP2_esYCC_SPACE: 		return "JP2_esYCC_SPACE";
	case JP2_iccLUM_SPACE: 		return "JP2_iccLUM_SPACE";
	case JP2_iccRGB_SPACE: 		return "JP2_iccRGB_SPACE";
	case JP2_iccANY_SPACE: 		return "JP2_iccANY_SPACE";
	case JP2_vendor_SPACE: 		return "JP2_vendor_SPACE";

	default:					return "<unknown>";
	}
}

static void
vips_foreign_load_kakadu_print(VipsForeignLoadKakadu *kakadu)
{
	printf("  width = %d\n", kakadu->width);
	printf("  height = %d\n", kakadu->height);
	printf("  tile_width = %d\n", kakadu->tile_width);
	printf("  tile_height = %d\n", kakadu->tile_height);
	printf("  bands = %d\n", kakadu->bands);
	printf("  format = %s\n", 
			vips_enum_string(VIPS_TYPE_BAND_FORMAT, kakadu->format));
	printf("  interpretation = %s\n", 
			vips_enum_string(VIPS_TYPE_INTERPRETATION, 
				kakadu->interpretation));
	printf("  xres = %g\n", kakadu->xres);
	printf("  yres = %g\n", kakadu->yres);

	printf("  source.get_metadata_memory() = %lld\n", 
			kakadu->source->get_metadata_memory());

	int count;
	if (kakadu->source->count_codestreams(count))
		printf("  source.count_codestreams() = %d\n", count);

	jpx_compatibility compat = kakadu->source->access_compatibility();
	printf("  compat.is_jp2() = %d\n", compat.is_jp2());
	printf("  compat.is_jp2_compatible() = %d\n", 
			compat.is_jp2_compatible());
	printf("  compat.is_jph_compatible() = %d\n", 
			compat.is_jph_compatible());
	printf("  compat.is_jpxb_compatible() = %d\n", 
			compat.is_jpxb_compatible());
	printf("  compat.is_jhxb_compatible() = %d\n", 
			compat.is_jhxb_compatible());
	printf("  compat.has_reader_requirements_box() = %d\n", 
			compat.has_reader_requirements_box());
	// use compat.check_standard_feature() to make sure we can decode

	if (kakadu->source->count_compositing_layers(count))
		printf("  source.count_compositing_layers() = %d\n", count);
	if (kakadu->source->count_containers(count))
		printf("  source.count_containers() = %d\n", count);

	printf("  layer.get_layer_id() = %d\n", kakadu->layer.get_layer_id());
	printf("  layer.get_num_codestreams() = %d\n", 
			kakadu->layer.get_num_codestreams());
	printf("  layer_size.get_x() = %d\n", kakadu->layer_size.get_x());
	printf("  layer_size.get_y() = %d\n", kakadu->layer_size.get_y());

	printf("  resolution.get_aspect_ratio() = %g\n", 
			kakadu->resolution.get_aspect_ratio());
	printf("  resolution.get_resolution(false) = %g pixels per metre\n", 
			kakadu->resolution.get_resolution(false));
	printf("  resolution.get_resolution(true) = %g pixels per metre\n", 
			kakadu->resolution.get_resolution(true));

	printf("  colour.get_num_colours() = %d\n", 
			kakadu->colour.get_num_colours());
	printf("  colour.get_space() = %s\n", 
			space2string(kakadu->colour.get_space()));
	printf("  colour.is_opponent_space() = %d\n", 
			kakadu->colour.is_opponent_space());
	for (int i = 0; i < kakadu->colour.get_num_colours(); i++)
		printf("  colour.get_natural_unsigned_zero_point(%d) = %f\n", 
				i, kakadu->colour.get_natural_unsigned_zero_point(i));
	printf("  colour.get_approximation_level() = %d\n", 
			kakadu->colour.get_approximation_level());

	int num_bytes;
	if (kakadu->colour.get_icc_profile(&num_bytes))
		printf("  colour.get_icc_profile() = %d bytes\n", num_bytes);

	printf("  channels.get_colour_mapping().cmp = %d\n", kakadu->cmp);
	printf("  channels.get_colour_mapping().lut = %d\n", kakadu->lut);
	printf("  channels.get_colour_mapping().stream_id = %d\n", 
			kakadu->stream_id);
	printf("  channels.get_colour_mapping().fmt = %d\n", kakadu->fmt);
	printf("  channels.get_num_colours() = %d\n", 
			kakadu->channels.get_num_colours());
	printf("  channels.get_num_non_colours() = %d\n", 
			kakadu->channels.get_num_non_colours());

	printf("  palette.exists() = %d\n", kakadu->palette.exists());
	printf("  palette.get_num_entries() = %d\n", 
			kakadu->palette.get_num_entries());
	printf("  palette.get_num_luts() = %d\n", 
			kakadu->palette.get_num_luts());
	for (int i = 0; i < kakadu->palette.get_num_luts(); i++)
		printf("  palette.get_bit_depth(%d) = %d\n", 
				i, kakadu->palette.get_bit_depth(i));

	kdu_dims valid_tiles;
	kakadu->codestream.get_valid_tiles(valid_tiles);

	printf("  valid_tiles.pos.x = %d\n", valid_tiles.pos.x);
	printf("  valid_tiles.pos.y = %d\n", valid_tiles.pos.y);
	printf("  valid_tiles.size.x = %d\n", valid_tiles.size.x);
	printf("  valid_tiles.size.y = %d\n", valid_tiles.size.y);

	for (int i = 0; i < kakadu->channels.get_num_colours(); i++)
		printf("  codestream.get_bit_depth(%d) = %d\n", 
				i, kakadu->codestream.get_bit_depth(i));
}
#endif /*DEBUG*/

static void
vips_foreign_load_kakadu_set_error_behaviour(VipsForeignLoadKakadu *kakadu)
{
	VipsForeignLoad *load = (VipsForeignLoad *) kakadu;

	if (load->fail_on <= VIPS_FAIL_ON_NONE)
		kakadu->codestream.set_resilient(TRUE);
	else if (load->fail_on >= VIPS_FAIL_ON_WARNING)
		kakadu->codestream.set_fussy();
	else
		// FIXME ... hmm check this
		kakadu->codestream.set_fast();
}

static void
vips_foreign_load_kakadu_get_resolution( VipsForeignLoadKakadu *kakadu )
{
	double kakadu_resolution;

	// first check for capture resolution 
	kakadu_resolution = kakadu->resolution.get_resolution(false);
	if (kakadu_resolution <= 0.0) {
		// try for display resolution 
		kakadu_resolution = kakadu->resolution.get_resolution(true);
		if (kakadu_resolution <= 0.0) 
			kakadu_resolution = 1.0;
	}

	// kakadu uses pixels per metre
	kakadu->yres = kakadu_resolution / 1000.0;
	kakadu->xres = kakadu->yres * kakadu->resolution.get_aspect_ratio();
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

	kakadu->kakadu_source->rewind();

	kakadu->input->open(kakadu->kakadu_source);
	if (kakadu->source->open(kakadu->input, true) <= 0) {
		vips_error(klass->nickname,
			"%s", _("raw codec load not implemented"));
		return -1;
	}

#ifdef DEBUG
	printf("vips_foreign_load_kakadu_header: opened as jpx\n");
#endif /*DEBUG*/

	kakadu->layer = kakadu->source->access_layer(0);
	kakadu->resolution = kakadu->layer.access_resolution();
	kakadu->colour = kakadu->layer.access_colour(0);
	kakadu->layer_size = kakadu->layer.get_layer_size();

	kakadu->channels = kakadu->layer.access_channels();
	if (!kakadu->channels.get_colour_mapping(0, 
			kakadu->cmp, 
			kakadu->lut, 
			kakadu->stream_id, 
			kakadu->fmt)) {
		kdu_uint16 key;
		kakadu->channels.get_non_colour_mapping(0, 
			key,
			kakadu->cmp, 
			kakadu->lut, 
			kakadu->stream_id, 
			kakadu->fmt);
	}
	kakadu->codestream_source = 
		kakadu->source->access_codestream(kakadu->stream_id);
	kakadu->palette = kakadu->codestream_source.access_palette();
	kakadu->dimensions = kakadu->codestream_source.access_dimensions();

	/* Try to guess how much we can reduce the image by (ie. n_pages)
	 * from the size of the first layer.
	 *
	 * Aim for no reduction possible, or a max reduction which will leave at 
	 * least 128 pixels on the shortest axis.
	 */
	int full_width = kakadu->layer_size.get_x();
	int full_height = kakadu->layer_size.get_y();
	double max_size_bits = log(VIPS_MIN(full_width, full_height)) / log(2);
	kakadu->n_pages = VIPS_MAX(1, max_size_bits - 6);

	if (kakadu->page >= kakadu->n_pages) {
		vips_error(klass->nickname,
			_("page should be less than %d"), kakadu->n_pages);
		return -1;
	}

	// and we need a codestream to get bitdepth, width, height, etc.
	kdu_compressed_source *compressed_source = 
		kakadu->codestream_source.open_stream();
	kakadu->codestream.create(compressed_source);

	vips_foreign_load_kakadu_set_error_behaviour(kakadu);

	// select all bands
	int first_component = 0;
	int max_components = 0;

	// use pages to pick a reduction factor
	int discard_levels = kakadu->page;

	// load all image layers
	int max_layers = 0;

	// no region of interest
	const kdu_dims *region_of_interest = NULL;

	kakadu->codestream.apply_input_restrictions(first_component,
		max_components,
		discard_levels,
		max_layers,
		region_of_interest);

	// random tile access needs a persistent codestream 
	kakadu->codestream.set_persistent();

	// get the decoded image dimensions
	kdu_dims dims;
	kakadu->codestream.get_dims(0, dims);
	kakadu->width = dims.size.x;
	kakadu->height = dims.size.y;

	// get the tile size (used to size the libvips tile cache)
	kakadu->codestream.get_tile_partition(dims);
	kakadu->tile_width = dims.size.x;
	kakadu->tile_height = dims.size.y;

	kakadu->bands = kakadu->codestream.get_num_components();

	// FIXME ... just the uint formats for now
	kakadu->bits_per_sample = -1;
	for (i = 0; i < kakadu->bands; i++) 
		kakadu->bits_per_sample = VIPS_MAX(kakadu->bits_per_sample, 
				kakadu->codestream.get_bit_depth(i));
	if (kakadu->bits_per_sample <= 8)
		kakadu->format = VIPS_FORMAT_UCHAR;
	else if (kakadu->bits_per_sample <= 16)
		kakadu->format = VIPS_FORMAT_USHORT;
	else if (kakadu->bits_per_sample <= 32)
		kakadu->format = VIPS_FORMAT_UINT;
	else {
		vips_error(klass->nickname, "%s", _("unsupported bits per sample"));
        return -1;
	}

	int expected_colour_bands;
	switch (kakadu->colour.get_space()) {
	case JP2_CMYK_SPACE:
		kakadu->interpretation = VIPS_INTERPRETATION_CMYK;
		expected_colour_bands = 4;
		break;

	case JP2_CIELab_SPACE:
		kakadu->interpretation = VIPS_INTERPRETATION_LAB;
		expected_colour_bands = 3;
		break;

	case JP2_iccRGB_SPACE:
	case JP2_sRGB_SPACE:
		kakadu->interpretation = VIPS_INTERPRETATION_sRGB;
		expected_colour_bands = 3;
		break;

	case JP2_EMPTY_SPACE: 		
	case JP2_CMY_SPACE:
	case JP2_bilevel1_SPACE:
	case JP2_YCbCr1_SPACE:
	case JP2_YCbCr2_SPACE:
	case JP2_YCbCr3_SPACE:
	case JP2_PhotoYCC_SPACE:
	case JP2_YCCK_SPACE:
	case JP2_bilevel2_SPACE:
	case JP2_sLUM_SPACE:
	case JP2_sYCC_SPACE:
	case JP2_CIEJab_SPACE:
	case JP2_esRGB_SPACE:
	case JP2_ROMMRGB_SPACE:
	case JP2_YPbPr60_SPACE:
	case JP2_YPbPr50_SPACE:
	case JP2_esYCC_SPACE:
	case JP2_iccLUM_SPACE:
	case JP2_iccANY_SPACE:
	case JP2_vendor_SPACE:
	default:
		// unimplemented, or we're unsure
		kakadu->interpretation = VIPS_INTERPRETATION_MULTIBAND;
		expected_colour_bands = kakadu->channels.get_num_colours();
		break;
	}

	// can be more with alpha etc.
	if (expected_colour_bands < kakadu->bands) {
		vips_error(klass->nickname,
			"%s", _("incorrect number of colour bands for colour space"));
		return -1;
	}

	vips_foreign_load_kakadu_get_resolution(kakadu);

#ifdef DEBUG
	vips_foreign_load_kakadu_print(kakadu);
#endif /*DEBUG*/

	if (vips_foreign_load_kakadu_set_header(kakadu, load->out))
		return -1;

	VIPS_SETSTR(load->out->filename,
		vips_connection_filename(VIPS_CONNECTION(kakadu->vips_source)));

	return 0;
}

static int
vips_foreign_load_kakadu_generate(VipsRegion *out,
	void *seq, void *a, void *b, gboolean *stop)
{
	VipsForeignLoad *load = (VipsForeignLoad *) a;
	VipsObjectClass *klass = VIPS_OBJECT_GET_CLASS(load);
	VipsForeignLoadKakadu *kakadu = (VipsForeignLoadKakadu *) load;
	VipsRect *r = &out->valid;

#ifdef DEBUG_VERBOSE
	printf("vips_foreign_load_kakadu_generate: "
		   "left = %d, top = %d, width = %d, height = %d\n",
		r->left, r->top, r->width, r->height);
#endif /*DEBUG_VERBOSE*/

	// 16 seems like a sensible limit ... we want to avoid overcommitting
	// thread resources if we can
	int n_threads = VIPS_MIN(16, vips_concurrency_get());

	// we can't reuse this between libvips workers, unfortunately, since
	// region_decompressor needs the calling thread to always be the same,
	// which libvips can't guarantee
	//
	// we have to create and destroy thread_env for each tile
	kdu_thread_env env;
	env.create();
	for (int i = 0; i < n_threads; i++)
		if (!env.add_thread()) {
			vips_error(klass->nickname, "%s", "thread create failed");
			return -1;
		}

	// coordinates in tile_position are always in terms of the full size
	// image, so we must scale up with the reduction factor
	int scale = 1;
	kdu_dims tile_position;
	tile_position.pos = kdu_coords(r->left * scale, r->top * scale);
	tile_position.size = kdu_coords(r->width * scale, r->height * scale);
	kdu_coords expand_numerator(1, 1);
	kdu_coords expand_denominator(1, 1);

	// not used, since we supply a channel mapping
	int single_component = 0;

	// decode all quality layers
	int max_layers = 1000;

	// aim for speed rather than ultimate precision
	bool precise = false;

	// specify params in terms of the output image
	kdu_component_access_mode mode = KDU_WANT_OUTPUT_COMPONENTS;

	// aim for a fast path
	bool fastest = true;

	if (!kakadu->region_decompressor->start(
			kakadu->codestream,
			kakadu->channel_mapping,
			single_component,
			kakadu->page,
			max_layers,
			tile_position,
			expand_numerator,
			expand_denominator,
			precise,
			mode,
			fastest,
			&env)) {
		vips_error(klass->nickname, "%s", "start failed");
		return -1;
	}

	kdu_dims incomplete_region = tile_position;
	kdu_dims new_region;
	int top = r->top;
	do {
		// we have to step data down the output area while we generate it
		kdu_byte *data = (kdu_byte *) VIPS_REGION_ADDR(out, r->left, top);

		kdu_coords buffer_origin = kdu_coords(0, 0);
		int row_gap = 0;
		int suggested_increment = 0;

		// we always want the whole tile
		int max_region_pixels = 1000000000;

		if (!kakadu->region_decompressor->process(data,
				kakadu->channel_offsets,
				kakadu->bands,
				buffer_origin,
				row_gap,
				suggested_increment,
				max_region_pixels,
				incomplete_region,
				new_region))
			break;

		// down by the number of generated scanlines
		top += new_region.size.y;
	} while (incomplete_region.size.y > 0);

	if (!kakadu->region_decompressor->finish()) {
		vips_error(klass->nickname, "%s", "finish failed");
		return -1;
	}

	return 0;
}

static int
vips_foreign_load_kakadu_load(VipsForeignLoad *load)
{
	VipsForeignLoadKakadu *kakadu = (VipsForeignLoadKakadu *) load;
	VipsImage **t = (VipsImage **)
		vips_object_local_array(VIPS_OBJECT(load), 3);

#ifdef DEBUG
	printf("vips_foreign_load_kakadu_load:\n");
#endif /*DEBUG*/

	t[0] = vips_image_new();
	if (vips_foreign_load_kakadu_set_header(kakadu, t[0]))
		return -1;

	// grab all channels
	// FIXME this won't work well for multispectral data
	kakadu->channel_mapping = new kdu_channel_mapping();
	kakadu->channel_mapping->configure(
			kakadu->colour,
			kakadu->channels,
			0,						// int codestream_idx
			kakadu->palette,
			kakadu->dimensions);

	kakadu->channel_offsets = VIPS_ARRAY(NULL, kakadu->bands, int);
	for (int i = 0; i < kakadu->bands; i++) 
		kakadu->channel_offsets[i] = i;

	kakadu->region_decompressor = new kdu_region_decompressor();

	if (vips_image_generate(t[0],
		NULL, vips_foreign_load_kakadu_generate, NULL,
		kakadu, NULL))
		return -1;

	// FIXME .. maybe scale up the real tile size to get c. 512x512?
	// on this PC: 
	// 256x256 == 5.4s
	// 512x512 == 2.2s
	// larger tiles fail to decode properly for some reason I don't 
	// understand
	int tiles_across = VIPS_ROUND_UP(kakadu->width, kakadu->tile_width) / 
		kakadu->tile_width;

	/* Copy to out, adding a cache. Enough tiles for two complete
	 * rows, plus 50%.
	 *
	 * FIXME ... set the tile size from the source image
	 *
	 * FIXME ... have the decompressor per thread and allow threaded cache
	 * access
	 */
	if (vips_tilecache(t[0], &t[1],
		"tile_width", kakadu->tile_width,
		"tile_height", kakadu->tile_height,
		"max_tiles", 3 * tiles_across,
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
		!(kakadu->vips_source = vips_source_new_from_file(file->filename)))
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
		if (!(kakadu->vips_source = vips_source_new_from_memory(
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
		kakadu->vips_source = source->source;
		g_object_ref(kakadu->vips_source);
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
