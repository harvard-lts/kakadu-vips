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

#include <jp2.h>
#include <jpx.h>

#include <iostream>

using namespace kdu_supp; // includes the core namespace

// i18n placeholder
#define _(S) (S)

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
		int result = vips_source_seek(source, offset, SEEK_SET);
#ifdef DEBUG_VERBOSE
		printf("VipsKakaduSource: seek(%lld) == %d\n", offset, result);
#endif /*DEBUG_VERBOSE*/

		// kakadu assumes this will always succeed
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
	jpx_layer_source layer;
	jp2_channels channels;
	jp2_palette palette;
	jp2_resolution resolution;
	jp2_colour colour;
	kdu_coords layer_size;

	/* For detailed parsing.
	 */
	kdu_codestream codestream;
	kdu_dims tile_indices;

	/* kakadu colour mapping.
	 */
	int cmp;
	int lut;
	int stream_id;
	int fmt;

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

#define DELETE(P) \
G_STMT_START \
    { \
        if (P) { \
            delete (P); \
            (P) = NULL; \
        } \
    } \
G_STMT_END

static void
vips_foreign_load_kakadu_dispose(GObject *gobject)
{
	VipsForeignLoadKakadu *kakadu = (VipsForeignLoadKakadu *) gobject;

#ifdef DEBUG
	printf("vips_foreign_load_kakadu_dispose:\n");
#endif /*DEBUG*/

	DELETE(kakadu->input);
	DELETE(kakadu->source);
	DELETE(kakadu->kakadu_source);

	VIPS_UNREF(kakadu->vips_source);

	G_OBJECT_CLASS(vips_foreign_load_kakadu_parent_class)->dispose(gobject);
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
	VipsObjectClass *klass = VIPS_OBJECT_GET_CLASS(kakadu);

#ifdef DEBUG
	printf("vips_foreign_load_kakadu_set_header:\n");
#endif /*DEBUG*/

	if (vips_image_pipelinev(out, VIPS_DEMAND_STYLE_SMALLTILE, NULL))
		return -1;

	int bands = kakadu->channels.get_num_colours() + 
		kakadu->channels.get_num_non_colours();

	VipsInterpretation interpretation;
	int expected_colour_bands;
	switch (kakadu->colour.get_space()) {
	case JP2_CMYK_SPACE:
		interpretation = VIPS_INTERPRETATION_CMYK;
		expected_colour_bands = 4;
		break;

	case JP2_CIELab_SPACE:
		interpretation = VIPS_INTERPRETATION_LAB;
		expected_colour_bands = 3;
		break;

	case JP2_sRGB_SPACE:
		interpretation = VIPS_INTERPRETATION_sRGB;
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
	case JP2_iccRGB_SPACE:
	case JP2_iccANY_SPACE:
	case JP2_vendor_SPACE:
	default:
		// unimplemented, or we're unsure
		interpretation = VIPS_INTERPRETATION_MULTIBAND;
		break;
	}

	if (kakadu->channels.get_num_colours() != expected_colour_bands) {
		vips_error(klass->nickname,
			"%s", _("incorrect number of colour bands for colour space"));
		return -1;
	}

	// FIXME ... perhaps we have to decode data to get the format?
	VipsBandFormat format = VIPS_FORMAT_UCHAR;

	// kakadu uses pixels per metre
	float yres = kakadu->resolution.get_resolution() / 1000.0;
	float xres = yres * kakadu->resolution.get_aspect_ratio();

	vips_image_init_fields(out,
		kakadu->layer_size.get_x(), kakadu->layer_size.get_y(), 
		bands, 
		format,
		VIPS_CODING_NONE, 
		interpretation, 
		xres, 
		yres);

	/*
	out->Xoffset =
		-VIPS_ROUND_INT((double) kakadu->image->x0 / kakadu->shrink);
	out->Yoffset =
		-VIPS_ROUND_INT((double) kakadu->image->y0 / kakadu->shrink);
	 */

	int num_bytes;
	const kdu_byte *data = kakadu->colour.get_icc_profile(&num_bytes);
	if (num_bytes > 0)
		vips_image_set_blob_copy(out, VIPS_META_ICC_NAME, data, num_bytes);

	/*
	if (kakadu->info &&
		kakadu->info->m_default_tile_info.tccp_info)
		vips_image_set_int(out, VIPS_META_N_PAGES,
			kakadu->info->m_default_tile_info.tccp_info->numresolutions);

	vips_image_set_int(out, VIPS_META_BITS_PER_SAMPLE, first->prec);
	 */

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
#endif /*DEBUG*/

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
	int result = kakadu->source->open(kakadu->input, true);
	printf("vips_foreign_load_kakadu_header: open() = %d\n", result);
	if (result > 0) {
		int count;

		// a jp2/jph file 
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
		jpx_codestream_source codestream = 
			kakadu->source->access_codestream(kakadu->stream_id);
		kakadu->palette = codestream.access_palette();

#ifdef DEBUG
		printf("  source.get_metadata_memory() = %lld\n", 
				kakadu->source->get_metadata_memory());

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
		printf("  resolution.get_resolution() = %g pixels per metre\n", 
				kakadu->resolution.get_resolution());

		printf("  colour.get_num_colours() = %d\n", 
				kakadu->colour.get_num_colours());
		printf("  colour.get_space() = %s\n", 
				space2string(kakadu->colour.get_space()));
		printf("  colour.is_opponent_space() = %d\n", 
				kakadu->colour.is_opponent_space());
		for (i = 0; i < kakadu->colour.get_num_colours(); i++)
			printf("  colour.get_natural_unsigned_zero_point(%d) = %f\n", 
					i, kakadu->colour.get_natural_unsigned_zero_point(i));
		printf("  colour.get_approximation_level() = %d\n", 
				kakadu->colour.get_approximation_level());

		int num_bytes;
		const kdu_byte *data = kakadu->colour.get_icc_profile(&num_bytes);
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
		for (i = 0; i < kakadu->palette.get_num_luts(); i++)
			printf("  palette.get_bit_depth(%d) = %d\n", 
					i, kakadu->palette.get_bit_depth(i));
#endif /*DEBUG*/
	}
	else {
#ifdef DEBUG
		printf("vips_foreign_load_kakadu_header: "
				"not a jpx ... raw codec load\n");
#endif /*DEBUG*/
	}

	/*
	if (!(kakadu->codec = opj_create_decompress(kakadu->format)))
		return -1;

	vips_foreign_load_kakadu_attach_handlers(kakadu, kakadu->codec);
	 */

	if (vips_foreign_load_kakadu_set_header(kakadu, load->out))
		return -1;

	VIPS_SETSTR(load->out->filename,
		vips_connection_filename(VIPS_CONNECTION(kakadu->vips_source)));

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

	/* jp2k tiles get smaller with the layer size.
	 */
	int tile_width = VIPS_ROUND_UINT(
		(double) jp2k->info->tdx / jp2k->shrink);
	int tile_height = VIPS_ROUND_UINT(
		(double) jp2k->info->tdy / jp2k->shrink);

	/* ... so tiles_across is always the same.
	 */
	int tiles_across = jp2k->info->tw;

	int x, y, z;

	y = 0;
	while (y < r->height) {
		VipsRect tile, hit;

		/* Not necessary, but it stops static analyzers complaining
		 * about a used-before-set.
		 */
		hit.height = 0;

		x = 0;
		while (x < r->width) {
			/* Tile the xy falls in, in tile numbers.
			 */
			int tx = (r->left + x) / tile_width;
			int ty = (r->top + y) / tile_height;

			/* Pixel coordinates of the tile that xy falls in.
			 */
			int xs = tx * tile_width;
			int ys = ty * tile_height;

			int tile_index = ty * tiles_across + tx;

			/* Fetch the tile.
			 */
#ifdef DEBUG_VERBOSE
			printf("   fetch tile %d\n", tile_index);
#endif /*DEBUG_VERBOSE*/
			if (!opj_get_decoded_tile(jp2k->codec,
					jp2k->stream, jp2k->image, tile_index))
				return -1;

			/* Intersect tile with request to get pixels we need
			 * to copy out.
			 */
			tile.left = xs;
			tile.top = ys;
			tile.width = tile_width;
			tile.height = tile_height;
			vips_rect_intersectrect(&tile, r, &hit);

			/* Unpack hit pixels to buffer in vips layout.
			 */
			for (z = 0; z < hit.height; z++) {
				VipsPel *q = VIPS_REGION_ADDR(out,
					hit.left, hit.top + z);

				vips_foreign_load_jp2k_pack(jp2k->upsample,
					jp2k->image, out->im, q,
					hit.left - tile.left,
					hit.top - tile.top + z,
					hit.width);

				if (jp2k->ycc_to_rgb)
					vips_foreign_load_jp2k_ycc_to_rgb(
						jp2k->image, out->im, q,
						hit.width);

				vips_foreign_load_jp2k_ljust(jp2k->image,
					out->im, q, hit.width);
			}

			x += hit.width;
		}

		/* This will be the same for all tiles in the row we've just
		 * done.
		 */
		y += hit.height;
	}

	return 0;
}

static int
vips_foreign_load_kakadu_load(VipsForeignLoad *load)
{
	VipsForeignLoadKakadu *kakadu = (VipsForeignLoadKakadu *) load;
	VipsImage **t = (VipsImage **)
		vips_object_local_array(VIPS_OBJECT(load), 3);

	int tile_width;
	int tile_height;
	int tiles_across;

#ifdef DEBUG
	printf("vips_foreign_load_kakadu_load:\n");
#endif /*DEBUG*/

	t[0] = vips_image_new();
	if (vips_foreign_load_kakadu_set_header(kakadu, t[0]))
		return -1;

	kakadu->codestream.create(kakadu->input);

	// FIXME
	// vips_foreign_load_kakadu_set_error_behaviour(kakadu);

	kakadu->codestream.get_valid_tiles(kakadu->tile_indices);
	tile_width = kakadu->tile_indicess.size.x;
	tile_height = kakadu->tile_indicess.size.y;
	tiles_across = t[0]->Xsize / tile_width;

	if (vips_image_generate(t[0],
		NULL, vips_foreign_load_kakadu_generate_tiled, NULL,
		kakadu, NULL))
		return -1;

	/* Copy to out, adding a cache. Enough tiles for two complete
	 * rows, plus 50%.
	 */
	if (vips_tilecache(t[0], &t[1],
		"tile_width", tile_width,
		"tile_height", tile_height,
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
