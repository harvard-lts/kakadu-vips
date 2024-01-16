/* save as jpeg2000
 */

/*
#define DEBUG_VERBOSE
#define DEBUG
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vips/vips.h>

#include "kakadu.h"

#include <kdu_stripe_compressor.h>

using namespace kdu_supp; // includes the core namespace

/* A VipsTarget as a Kakadu output object. This keeps the reference
 * alive while it's alive.
 */
class VipsKakaduTarget : public kdu_compressed_target {
public:
	VipsKakaduTarget()
	{
#ifdef DEBUG_VERBOSE
		printf("new VipsKakaduTarget:\n");
#endif /*DEBUG_VERBOSE*/

		target = NULL;
	}

	~VipsKakaduTarget()
	{
#ifdef DEBUG_VERBOSE
		printf("~VipsKakaduTarget:\n");
#endif /*DEBUG_VERBOSE*/

		close();
	}

	bool exists() 
	{ 
		return target != NULL; 
	}

	bool operator!() 
	{ 
		return target == NULL; 
	}

	bool open(VipsTarget *_target)
	{
#ifdef DEBUG_VERBOSE
		printf("VipsKakaduTarget::open()\n");
#endif /*DEBUG_VERBOSE*/

		close();

		target = _target;
		g_object_ref(target);

		return true;
	}

	int get_capabilities() 
	{
		// a simple byte target
		return KDU_TARGET_CAP_SEQUENTIAL;
	}

	virtual bool write(const kdu_byte *buf, int num_bytes)
	{
#ifdef DEBUG_VERBOSE
		printf("VipsKakaduTarget: write %d bytes ...\n", num_bytes);
#endif /*DEBUG_VERBOSE*/

		return !vips_target_write(target, buf, num_bytes);
	}

	virtual bool close()
	{
#ifdef DEBUG_VERBOSE
		printf("VipsKakaduTarget: close()\n");
#endif /*DEBUG_VERBOSE*/

		VIPS_UNREF(target);
		return true;
	}

private:
	VipsTarget *target;
};

typedef struct _VipsForeignSaveKakadu {
	VipsForeignSave parent_object;

	/* Where to write (set by subclasses).
	 */
	VipsTarget *target;

	/* "target" wrapped up as a kakadu byte data target.
	 */
	VipsKakaduTarget *kakadu_target;

	int tile_width;
	int tile_height;

	/* Lossless mode.
	 */
	gboolean lossless;

	/* Quality factor.
	 */
	int Q;

	/* Chroma subsample mode.
	 */
	VipsForeignSubsample subsample_mode;

	/* Encoder state.
	 */
	kdu_thread_env *env;
	kdu_stripe_compressor *compressor;

	/* The line of tiles we are building, and the buffer we
	 * unpack to for output.
	 */
	VipsRegion *strip;
	VipsPel *tile_buffer;
	int *stripe_heights;

	/* If we need to subsample during unpacking.
	 */
	gboolean subsample;

	/* If we convert RGB to YCC during save.
	 */
	gboolean save_as_ycc;

	/* Accumulate a line of sums here during chroma subsample.
	 */
	VipsPel *accumulate;
} VipsForeignSaveKakadu;

typedef VipsForeignSaveClass VipsForeignSaveKakaduClass;

G_DEFINE_ABSTRACT_TYPE(VipsForeignSaveKakadu, vips_foreign_save_kakadu,
	VIPS_TYPE_FOREIGN_SAVE);

static void
vips_foreign_save_kakadu_dispose(GObject *gobject)
{
	VipsForeignSaveKakadu *kakadu = (VipsForeignSaveKakadu *) gobject;

	DELETE(kakadu->env);
	DELETE(kakadu->compressor);
	DELETE(kakadu->kakadu_target);

	VIPS_UNREF(kakadu->target);
	VIPS_UNREF(kakadu->strip);

	VIPS_FREE(kakadu->tile_buffer);
	VIPS_FREE(kakadu->accumulate);
	VIPS_FREE(kakadu->stripe_heights);

	G_OBJECT_CLASS(vips_foreign_save_kakadu_parent_class)->dispose(gobject);
}

static int
vips_foreign_save_kakadu_write_block(VipsRegion *region, VipsRect *area,
    void *user)
{
    VipsForeignSaveKakadu *kakadu = (VipsForeignSaveKakadu *) user;
	VipsImage *image = region->im;
	VipsRect *r = &region->valid;

#ifdef DEBUG_VERBOSE
	printf("vips_foreign_save_kakadu_write_block: "
		   "left = %d, top = %d, width = %d, height = %d\n",
		r->left, r->top, r->width, r->height);
#endif /*DEBUG_VERBOSE*/

	for (int i = 0; i < image->Bands; i++) 
		kakadu->stripe_heights[i] = r->height;

	kakadu->compressor->push_stripe(
		(kdu_byte *) VIPS_REGION_ADDR(region, r->left, r->top),
		kakadu->stripe_heights);

	return 0;
}

static int
vips_foreign_save_kakadu_build(VipsObject *object)
{
	VipsObjectClass *klass = VIPS_OBJECT_GET_CLASS(object);
	VipsForeignSave *save = (VipsForeignSave *) object;
	VipsForeignSaveKakadu *kakadu = (VipsForeignSaveKakadu *) object;

	VipsImage *image;

	// install error and warn messages
	kdu_customize_errors(&vips_foreign_kakadu_error_handler);
	kdu_customize_warnings(&vips_foreign_kakadu_warn_handler);

	if (VIPS_OBJECT_CLASS(vips_foreign_save_kakadu_parent_class)->
		build(object))
		return -1;

	image = save->ready;

	if (!vips_band_format_isint(image->BandFmt)) {
		vips_error(klass->nickname, "%s", _("not an integer format"));
		return -1;
	}

	kakadu->stripe_heights = VIPS_ARRAY(NULL, image->Bands, int);

	siz_params siz;

	siz.set(Scomponents, 0, 0, image->Bands);
	siz.set(Sdims, 0, 0, image->Ysize);
	siz.set(Sdims, 0, 1, image->Xsize);
	siz.set(Sprecision, 0, 0, 
			(int) (vips_format_sizeof(image->BandFmt) << 3));
	siz.set(Ssigned, 0, 0, false);

	// finalize to complete other fields ... has to be a reference
	kdu_params *siz_ref = &siz; 
	siz_ref->finalize();

	// a kdu_compressed_target
	kakadu->kakadu_target = new VipsKakaduTarget();
	kakadu->kakadu_target->open(kakadu->target);

	jp2_family_tgt target;
	target.open(kakadu->kakadu_target);

	jp2_target output;
	output.open(&target);

	// set image properties
	jp2_dimensions dims = output.access_dimensions(); 
	dims.init(&siz);
	jp2_colour colr = output.access_colour();
	colr.init((image->Bands == 3) ? JP2_sRGB_SPACE : JP2_sLUM_SPACE);

	// kakadu does not seem to support setting an icc profile, only 
	// loading them

	// serialise the image into a codestream
	kdu_codestream codestream; 
	codestream.create(&siz, &output);

	/*
	 * FIXME ... an easy way to set props
	 *
	codestream.access_siz()->parse_string("Clayers=12");
	codestream.access_siz()->parse_string("Creversible=yes");
	codestream.access_siz()->parse_string("Qfactor=85");
	codestream.access_siz()->parse_string("Ctype=Y,Cb,Cr,N");
	 *
	 */

    output.write_header();
    output.open_codestream(true);

	kakadu->compressor = new kdu_stripe_compressor();

	// 16 seems like a sensible limit ... we want to avoid overcommitting
	// thread resources if we can
	int n_threads = VIPS_MIN(16, vips_concurrency_get());

	kdu_thread_env env;
	env.create();
	for (int i = 0; i < n_threads; i++)
		if (!env.add_thread()) {
			vips_error(klass->nickname, "%s", "thread create failed");
			return -1;
		}

	kakadu->compressor->start(codestream, 
		0, 			// num_layer_specs
		NULL, 		// layer_sizes
		NULL, 		// layer_slopes
		0, 			// min_slope_threshold
		false, 		// no_auto_complexity_control
		false, 		// force_precise
		true, 		// record_layer_info_in_comment
		0.0, 		// size_tolerance
		0, 			// num_components
		false, 		// want_fastest
		&env,		// thread set
		NULL, 		// env_queue
		-1, 		// env_dbuf_height
		-1, 		// env_tile_concurrency
		true, 		// trim_to_rate
		0, 			// flush_flags
		NULL);		// multi_xform_extra_params

	if (vips_sink_disc(image, vips_foreign_save_kakadu_write_block, kakadu))
		return -1;

	kakadu->compressor->finish();

	codestream.destroy();
	output.close();

	if (vips_target_end(kakadu->target))
		return -1;

	return 0;
}

static void
vips_foreign_save_kakadu_class_init(VipsForeignSaveKakaduClass *klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
	VipsObjectClass *object_class = (VipsObjectClass *) klass;
	VipsForeignClass *foreign_class = (VipsForeignClass *) klass;
	VipsForeignSaveClass *save_class = (VipsForeignSaveClass *) klass;

#ifdef DEBUG
	printf("vips_foreign_save_kakadu_class_init:\n");
#endif /*DEBUG*/

	gobject_class->dispose = vips_foreign_save_kakadu_dispose;
	gobject_class->set_property = vips_object_set_property;
	gobject_class->get_property = vips_object_get_property;

	object_class->nickname = "kakadusave_base";
	object_class->description = _("save image in JPEG2000 format");
	object_class->build = vips_foreign_save_kakadu_build;

	foreign_class->suffs = vips__kakadu_suffs;

	save_class->saveable = VIPS_SAVEABLE_ANY;

	VIPS_ARG_INT(klass, "tile_width", 11,
		_("Tile width"),
		_("Tile width in pixels"),
		VIPS_ARGUMENT_OPTIONAL_INPUT,
		G_STRUCT_OFFSET(VipsForeignSaveKakadu, tile_width),
		1, 32768, 512);

	VIPS_ARG_INT(klass, "tile_height", 12,
		_("Tile height"),
		_("Tile height in pixels"),
		VIPS_ARGUMENT_OPTIONAL_INPUT,
		G_STRUCT_OFFSET(VipsForeignSaveKakadu, tile_height),
		1, 32768, 512);

	VIPS_ARG_BOOL(klass, "lossless", 13,
		_("Lossless"),
		_("Enable lossless compression"),
		VIPS_ARGUMENT_OPTIONAL_INPUT,
		G_STRUCT_OFFSET(VipsForeignSaveKakadu, lossless),
		FALSE);

	VIPS_ARG_ENUM(klass, "subsample_mode", 19,
		_("Subsample mode"),
		_("Select chroma subsample operation mode"),
		VIPS_ARGUMENT_OPTIONAL_INPUT,
		G_STRUCT_OFFSET(VipsForeignSaveKakadu, subsample_mode),
		VIPS_TYPE_FOREIGN_SUBSAMPLE,
		VIPS_FOREIGN_SUBSAMPLE_OFF);

	VIPS_ARG_INT(klass, "Q", 14,
		_("Q"),
		_("Q factor"),
		VIPS_ARGUMENT_OPTIONAL_INPUT,
		G_STRUCT_OFFSET(VipsForeignSaveKakadu, Q),
		1, 100, 48);
}

static void
vips_foreign_save_kakadu_init(VipsForeignSaveKakadu *kakadu)
{
	kakadu->tile_width = 512;
	kakadu->tile_height = 512;

	/* Chosen to give about the same filesize as regular jpg Q75.
	 */
	kakadu->Q = 48;

	kakadu->subsample_mode = VIPS_FOREIGN_SUBSAMPLE_OFF;
}

typedef struct _VipsForeignSaveKakaduFile {
	VipsForeignSaveKakadu parent_object;

	/* Filename for save.
	 */
	char *filename;

} VipsForeignSaveKakaduFile;

typedef VipsForeignSaveKakaduClass VipsForeignSaveKakaduFileClass;

G_DEFINE_TYPE(VipsForeignSaveKakaduFile, vips_foreign_save_kakadu_file,
	vips_foreign_save_kakadu_get_type());

static int
vips_foreign_save_kakadu_file_build(VipsObject *object)
{
	VipsForeignSaveKakadu *kakadu = (VipsForeignSaveKakadu *) object;
	VipsForeignSaveKakaduFile *file = (VipsForeignSaveKakaduFile *) object;

	if (!(kakadu->target = vips_target_new_to_file(file->filename)))
		return -1;

	if (VIPS_OBJECT_CLASS(vips_foreign_save_kakadu_file_parent_class)
			->build(object))
		return -1;

	return 0;
}

static void
vips_foreign_save_kakadu_file_class_init(
	VipsForeignSaveKakaduFileClass *klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
	VipsObjectClass *object_class = (VipsObjectClass *) klass;

	gobject_class->set_property = vips_object_set_property;
	gobject_class->get_property = vips_object_get_property;

	object_class->nickname = "kakadusave";
	object_class->build = vips_foreign_save_kakadu_file_build;

	VIPS_ARG_STRING(klass, "filename", 1,
		_("Filename"),
		_("Filename to load from"),
		VIPS_ARGUMENT_REQUIRED_INPUT,
		G_STRUCT_OFFSET(VipsForeignSaveKakaduFile, filename),
		NULL);
}

static void
vips_foreign_save_kakadu_file_init(VipsForeignSaveKakaduFile *file)
{
}

typedef struct _VipsForeignSaveKakaduBuffer {
	VipsForeignSaveKakadu parent_object;

	/* Save to a buffer.
	 */
	VipsArea *buf;

} VipsForeignSaveKakaduBuffer;

typedef VipsForeignSaveKakaduClass VipsForeignSaveKakaduBufferClass;

G_DEFINE_TYPE(VipsForeignSaveKakaduBuffer, vips_foreign_save_kakadu_buffer,
	vips_foreign_save_kakadu_get_type());

static int
vips_foreign_save_kakadu_buffer_build(VipsObject *object)
{
	VipsForeignSaveKakadu *kakadu = (VipsForeignSaveKakadu *) object;
	VipsForeignSaveKakaduBuffer *buffer =
		(VipsForeignSaveKakaduBuffer *) object;

	VipsBlob *blob;

	if (!(kakadu->target = vips_target_new_to_memory()))
		return -1;

	if (VIPS_OBJECT_CLASS(vips_foreign_save_kakadu_buffer_parent_class)
			->build(object))
		return -1;

	g_object_get(kakadu->target, "blob", &blob, NULL);
	g_object_set(buffer, "buffer", blob, NULL);
	vips_area_unref(VIPS_AREA(blob));

	return 0;
}

static void
vips_foreign_save_kakadu_buffer_class_init(
	VipsForeignSaveKakaduBufferClass *klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
	VipsObjectClass *object_class = (VipsObjectClass *) klass;

	gobject_class->set_property = vips_object_set_property;
	gobject_class->get_property = vips_object_get_property;

	object_class->nickname = "kakadusave_buffer";
	object_class->build = vips_foreign_save_kakadu_buffer_build;

	VIPS_ARG_BOXED(klass, "buffer", 1,
		_("Buffer"),
		_("Buffer to save to"),
		VIPS_ARGUMENT_REQUIRED_OUTPUT,
		G_STRUCT_OFFSET(VipsForeignSaveKakaduBuffer, buf),
		VIPS_TYPE_BLOB);
}

static void
vips_foreign_save_kakadu_buffer_init(VipsForeignSaveKakaduBuffer *buffer)
{
}

typedef struct _VipsForeignSaveKakaduTarget {
	VipsForeignSaveKakadu parent_object;

	VipsTarget *target;
} VipsForeignSaveKakaduTarget;

typedef VipsForeignSaveKakaduClass VipsForeignSaveKakaduTargetClass;

G_DEFINE_TYPE(VipsForeignSaveKakaduTarget, vips_foreign_save_kakadu_target,
	vips_foreign_save_kakadu_get_type());

static int
vips_foreign_save_kakadu_target_build(VipsObject *object)
{
	VipsForeignSaveKakadu *kakadu = (VipsForeignSaveKakadu *) object;
	VipsForeignSaveKakaduTarget *target =
		(VipsForeignSaveKakaduTarget *) object;

	if (target->target) {
		kakadu->target = target->target;
		g_object_ref(kakadu->target);
	}

	if (VIPS_OBJECT_CLASS(vips_foreign_save_kakadu_target_parent_class)
			->build(object))
		return -1;

	return 0;
}

static void
vips_foreign_save_kakadu_target_class_init(
	VipsForeignSaveKakaduTargetClass *klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
	VipsObjectClass *object_class = (VipsObjectClass *) klass;

	gobject_class->set_property = vips_object_set_property;
	gobject_class->get_property = vips_object_get_property;

	object_class->nickname = "kakadusave_target";
	object_class->build = vips_foreign_save_kakadu_target_build;

	VIPS_ARG_OBJECT(klass, "target", 1,
		_("Target"),
		_("Target to save to"),
		VIPS_ARGUMENT_REQUIRED_INPUT,
		G_STRUCT_OFFSET(VipsForeignSaveKakaduTarget, target),
		VIPS_TYPE_TARGET);
}

static void
vips_foreign_save_kakadu_target_init(VipsForeignSaveKakaduTarget *target)
{
}

/**
 * vips_kakadusave: (method)
 * @in: image to save
 * @filename: file to write to
 * @...: %NULL-terminated list of optional named arguments
 *
 * Optional arguments:
 *
 * * @Q: %gint, quality factor
 * * @lossless: %gboolean, enables lossless compression
 * * @tile_width: %gint for tile size
 * * @tile_height: %gint for tile size
 * * @subsample_mode: #VipsForeignSubsample, chroma subsampling mode
 *
 * Write a VIPS image to a file in JPEG2000 format.
 * The saver supports 8, 16 and 32-bit int pixel
 * values, signed and unsigned. It supports greyscale, RGB, CMYK and
 * multispectral images.
 *
 * Use @Q to set the compression quality factor. The default value
 * produces file with approximately the same size as regular JPEG Q 75.
 *
 * Set @lossless to enable lossless compression.
 *
 * Use @tile_width and @tile_height to set the tile size. The default is 512.
 *
 * Chroma subsampling is normally disabled for compatibility. Set
 * @subsample_mode to auto to enable chroma subsample for Q < 90. Subsample
 * mode uses YCC rather than RGB colourspace, and many jpeg2000 decoders do
 * not support this.
 *
 * This operation always writes a pyramid.
 *
 * See also: vips_image_write_to_file(), vips_kakaduload().
 *
 * Returns: 0 on success, -1 on error.
 */
int
vips_kakadusave(VipsImage *in, const char *filename, ...)
{
	va_list ap;
	int result;

	va_start(ap, filename);
	result = vips_call_split("kakadusave", ap, in, filename);
	va_end(ap);

	return result;
}

/**
 * vips_kakadusave_buffer: (method)
 * @in: image to save
 * @buf: (array length=len) (element-type guint8): return output buffer here
 * @len: (type gsize): return output length here
 * @...: %NULL-terminated list of optional named arguments
 *
 * Optional arguments:
 *
 * * @Q: %gint, quality factor
 * * @lossless: %gboolean, enables lossless compression
 * * @tile_width: %gint for tile size
 * * @tile_height: %gint for tile size
 * * @subsample_mode: #VipsForeignSubsample, chroma subsampling mode
 *
 * As vips_kakadusave(), but save to a target.
 *
 * See also: vips_kakadusave(), vips_image_write_to_target().
 *
 * Returns: 0 on success, -1 on error.
 */
int
vips_kakadusave_buffer(VipsImage *in, void **buf, size_t *len, ...)
{
	va_list ap;
	VipsArea *area;
	int result;

	area = NULL;

	va_start(ap, len);
	result = vips_call_split("kakadusave_buffer", ap, in, &area);
	va_end(ap);

	if (!result &&
		area) {
		if (buf) {
			*buf = area->data;
			area->free_fn = NULL;
		}
		if (len)
			*len = area->length;

		vips_area_unref(area);
	}

	return result;
}

/**
 * vips_kakadusave_target: (method)
 * @in: image to save
 * @target: save image to this target
 * @...: %NULL-terminated list of optional named arguments
 *
 * Optional arguments:
 *
 * * @Q: %gint, quality factor
 * * @lossless: %gboolean, enables lossless compression
 * * @tile_width: %gint for tile size
 * * @tile_height: %gint for tile size
 * * @subsample_mode: #VipsForeignSubsample, chroma subsampling mode
 *
 * As vips_kakadusave(), but save to a target.
 *
 * See also: vips_kakadusave(), vips_image_write_to_target().
 *
 * Returns: 0 on success, -1 on error.
 */
int
vips_kakadusave_target(VipsImage *in, VipsTarget *target, ...)
{
	va_list ap;
	int result;

	va_start(ap, target);
	result = vips_call_split("kakadusave_target", ap, in, target);
	va_end(ap);

	return result;
}
