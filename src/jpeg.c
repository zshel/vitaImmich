/* vitaImmich unity build — part of one translation unit.
 * This file is #included by src/main.c (in order) and shares all
 * file-scope state with the other parts; it is NOT compiled on its own.
 * Only src/main.c is listed in CMakeLists.txt. Editors/clangd may flag
 * undefined symbols here because of that — those are expected.
 */

/* ------------------------------------------------------------------ */
/* JPEG decoding                                                       */
/*                                                                     */
/* libjpeg directly instead of vita2d_load_JPEG_buffer: vita2d rejects */
/* any JPEG whose first marker isn't APP0/APP1, but Immich previews    */
/* start with an APP2 ICC-profile marker (ff d8 ff e2). Also lets us   */
/* downscale at decode time for grid thumbnails, and a longjmp error   */
/* handler keeps corrupt data from exit()ing the app.                  */
/* ------------------------------------------------------------------ */

struct jpeg_jmp_err {
	struct jpeg_error_mgr mgr;
	jmp_buf jb;
	char msg[JMSG_LENGTH_MAX];
};

static void jpeg_jmp_error_exit(j_common_ptr cinfo)
{
	struct jpeg_jmp_err *e = (struct jpeg_jmp_err *)cinfo->err;
	cinfo->err->format_message(cinfo, e->msg);
	longjmp(e->jb, 1);
}


/* decode into a tightly packed malloc'd pixel buffer; no vita2d/GXM
 * calls, so this is safe to run on the loader thread */
static unsigned char *decode_jpeg_buf(const void *data, size_t size, int maxdim,
				      int *w, int *h, int *comps,
				      char *err, size_t errlen)
{
	struct jpeg_decompress_struct ji;
	struct jpeg_jmp_err jerr;
	unsigned char *pix = NULL;

	ji.err = jpeg_std_error(&jerr.mgr);
	jerr.mgr.error_exit = jpeg_jmp_error_exit;
	if (setjmp(jerr.jb)) {
		snprintf(err, errlen, "libjpeg: %s", jerr.msg);
		jpeg_destroy_decompress(&ji);
		free(pix);
		return NULL;
	}

	jpeg_create_decompress(&ji);
	jpeg_mem_src(&ji, (void *)data, size);
	jpeg_read_header(&ji, 1);

	unsigned int longer = ji.image_width > ji.image_height ?
			      ji.image_width : ji.image_height;
	ji.scale_num = 1;
	ji.scale_denom = 1;
	while (longer / ji.scale_denom > (unsigned int)maxdim && ji.scale_denom < 8)
		ji.scale_denom *= 2;

	jpeg_start_decompress(&ji);

	if (ji.output_components != 1 && ji.output_components != 3) {
		snprintf(err, errlen, "unsupported JPEG: %d components (colorspace %d)",
			 ji.output_components, ji.jpeg_color_space);
		jpeg_abort_decompress(&ji);
		jpeg_destroy_decompress(&ji);
		return NULL;
	}

	unsigned int rowbytes = ji.output_width * ji.output_components;
	pix = malloc(rowbytes * ji.output_height);
	if (!pix) {
		snprintf(err, errlen, "out of memory (%ux%u)",
			 ji.output_width, ji.output_height);
		jpeg_abort_decompress(&ji);
		jpeg_destroy_decompress(&ji);
		return NULL;
	}

	unsigned char *row = pix;
	while (ji.output_scanline < ji.output_height) {
		jpeg_read_scanlines(&ji, &row, 1);
		row += rowbytes;
	}

	*w = ji.output_width;
	*h = ji.output_height;
	*comps = ji.output_components;
	jpeg_finish_decompress(&ji);
	jpeg_destroy_decompress(&ji);
	return pix;
}

