/*
 *			GPAC - Multimedia Framework C SDK
 *
 *  This file is part of GPAC / Dirac decoder filter, based on schroedinger.
 *
 *  Dirac is the BBC's wavelet codec. Its test material comes as a raw
 *  elementary stream - a chain of parse units, each opening with a 13-byte
 *  parse info header ("BBCD", a parse code, and the offsets to the next and
 *  previous units) - so the filter takes the whole file and hands it to
 *  schroedinger's autoparse API, which finds the units itself. The alternative,
 *  schro_decoder_push, wants exactly one parse unit per call.
 *
 *  What this decodes and what it does not. schroedinger 1.0.11 predates the
 *  standardisation of VC-2's high-quality profile, and its
 *  SCHRO_PARSE_CODE_IS_LOW_DELAY only tests (code & 0x88) == 0x88 - so an HQ
 *  picture (parse code 0xE8) is read with the low-delay slice syntax, every
 *  coefficient comes out zero and the picture is a uniform grey. That is what
 *  ffmpeg's "vc2" encoder emits, and it is refused here rather than decoded
 *  into grey: see the parse-code check below.
 */

#include <gpac/filters.h>
#include <gpac/constants.h>
#include <string.h>
#include <stdlib.h>

#include <schroedinger/schro.h>

typedef struct
{
	GF_FilterPid *ipid, *opid;
	u32 width, height, timescale, frame_dur;
	u64 next_cts;
	Bool cfg_done;
	SchroFrameFormat frame_format;
	u32 pix_fmt;
} GF_DiracDecCtx;

static GF_Err diracdec_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	GF_DiracDecCtx *ctx = (GF_DiracDecCtx *)gf_filter_get_udta(filter);

	if (is_remove)
	{
		if (ctx->opid)
		{
			gf_filter_pid_remove(ctx->opid);
			ctx->opid = NULL;
		}
		ctx->ipid = NULL;
		return GF_OK;
	}
	if (!gf_filter_pid_check_caps(pid))
		return GF_NOT_SUPPORTED;

	ctx->ipid = pid;
	gf_filter_pid_set_framing_mode(pid, GF_TRUE);

	if (!ctx->opid)
		ctx->opid = gf_filter_pid_new(filter);

	/* Corrected from the sequence header as soon as one is decoded; GPAC
	 * resolves the graph from these first. */
	ctx->width = 320;
	ctx->height = 180;
	ctx->timescale = 25;
	ctx->frame_dur = 1;
	ctx->next_cts = 0;
	ctx->cfg_done = GF_FALSE;
	ctx->frame_format = SCHRO_FRAME_FORMAT_U8_420;
	ctx->pix_fmt = GF_PIXEL_YUV;

	gf_filter_pid_copy_properties(ctx->opid, pid);
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STREAM_TYPE, &PROP_UINT(GF_STREAM_VISUAL));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CODECID, &PROP_UINT(GF_CODECID_RAW));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_PIXFMT, &PROP_UINT(ctx->pix_fmt));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_WIDTH, &PROP_UINT(ctx->width));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_HEIGHT, &PROP_UINT(ctx->height));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STRIDE, &PROP_UINT(ctx->width));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_TIMESCALE, &PROP_UINT(ctx->timescale));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_FPS, &PROP_FRAC_INT(ctx->timescale, ctx->frame_dur));

	return GF_OK;
}

/* Refuses a stream whose pictures schroedinger would decode into grey - see
 * the note at the top of the file. Returns GF_TRUE when every picture unit
 * carries a parse code this library really understands. */
static Bool dirac_stream_is_supported(const u8 *data, u32 size, u32 *hq_count)
{
	u32 pos = 0;

	*hq_count = 0;
	while ((pos + 13) <= size)
	{
		u32 next;
		u8 code;

		if (memcmp(data + pos, "BBCD", 4))
		{
			pos++;
			continue;
		}
		code = data[pos + 4];
		next = GF_4CC(data[pos + 5], data[pos + 6], data[pos + 7], data[pos + 8]);

		/* 0x08 marks a picture; 0x88 both set is what schroedinger reads as
		 * low delay, and bit 0x40 on top of that is VC-2 high quality. */
		if ((code & 0x08) && ((code & 0xC8) == 0xC8))
			(*hq_count)++;

		pos += next ? next : 13;
	}
	return (*hq_count == 0) ? GF_TRUE : GF_FALSE;
}

static GF_Err diracdec_send_frame(GF_DiracDecCtx *ctx, SchroFrame *frame)
{
	GF_FilterPacket *dst_pck;
	u8 *output;
	u32 c, y, total = 0;

	for (c = 0; c < 3; c++)
		total += frame->components[c].width * frame->components[c].height;

	dst_pck = gf_filter_pck_new_alloc(ctx->opid, total, &output);
	if (!dst_pck)
		return GF_OUT_OF_MEM;

	for (c = 0; c < 3; c++)
	{
		const u8 *src = (const u8 *)frame->components[c].data;
		u32 w = frame->components[c].width;
		for (y = 0; y < (u32)frame->components[c].height; y++)
		{
			memcpy(output, src + (size_t)y * frame->components[c].stride, w);
			output += w;
		}
	}

	gf_filter_pck_set_cts(dst_pck, ctx->next_cts);
	gf_filter_pck_set_duration(dst_pck, ctx->frame_dur);
	gf_filter_pck_set_sap(dst_pck, GF_FILTER_SAP_1);
	gf_filter_pck_send(dst_pck);
	ctx->next_cts += ctx->frame_dur;
	return GF_OK;
}

static void diracdec_read_format(GF_DiracDecCtx *ctx, SchroDecoder *dec)
{
	SchroVideoFormat *fmt = schro_decoder_get_video_format(dec);
	if (!fmt)
		return;

	ctx->width = fmt->width;
	ctx->height = fmt->height;
	switch (fmt->chroma_format)
	{
	case SCHRO_CHROMA_420:
		ctx->frame_format = SCHRO_FRAME_FORMAT_U8_420;
		ctx->pix_fmt = GF_PIXEL_YUV;
		break;
	case SCHRO_CHROMA_422:
		ctx->frame_format = SCHRO_FRAME_FORMAT_U8_422;
		ctx->pix_fmt = GF_PIXEL_YUV422;
		break;
	default:
		ctx->frame_format = SCHRO_FRAME_FORMAT_U8_444;
		ctx->pix_fmt = GF_PIXEL_YUV444;
		break;
	}
	if (fmt->frame_rate_numerator && fmt->frame_rate_denominator)
	{
		ctx->timescale = fmt->frame_rate_numerator;
		ctx->frame_dur = fmt->frame_rate_denominator;
	}

	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_WIDTH, &PROP_UINT(ctx->width));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_HEIGHT, &PROP_UINT(ctx->height));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STRIDE, &PROP_UINT(ctx->width));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_PIXFMT, &PROP_UINT(ctx->pix_fmt));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_TIMESCALE, &PROP_UINT(ctx->timescale));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_FPS, &PROP_FRAC_INT(ctx->timescale, ctx->frame_dur));
	ctx->cfg_done = GF_TRUE;

	free(fmt);
}

static GF_Err diracdec_process(GF_Filter *filter)
{
	GF_FilterPacket *pck;
	const u8 *data;
	u8 *copy;
	u32 size, hq_count = 0;
	SchroDecoder *dec;
	SchroBuffer *buf;
	Bool eos_pushed = GF_FALSE;
	GF_Err e = GF_OK;
	GF_DiracDecCtx *ctx = (GF_DiracDecCtx *)gf_filter_get_udta(filter);

	pck = gf_filter_pid_get_packet(ctx->ipid);
	if (!pck)
	{
		if (gf_filter_pid_is_eos(ctx->ipid))
		{
			gf_filter_pid_set_eos(ctx->opid);
			return GF_EOS;
		}
		return GF_OK;
	}
	data = gf_filter_pck_get_data(pck, &size);
	if (!data || (size < 13) || memcmp(data, "BBCD", 4))
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[DiracDec] Stream does not start with a parse info header\n"));
		return GF_NON_COMPLIANT_BITSTREAM;
	}
	if (!dirac_stream_is_supported(data, size, &hq_count))
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[DiracDec] %u VC-2 high-quality pictures: schroedinger predates that profile and would decode them to a uniform grey, so the stream is refused\n",
		                                    hq_count));
		return GF_NOT_SUPPORTED;
	}

	schro_init();
	dec = schro_decoder_new();
	if (!dec)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_OUT_OF_MEM;
	}

	/* schroedinger takes ownership of the buffer it is given. */
	copy = (u8 *)malloc(size);
	if (!copy)
	{
		schro_decoder_free(dec);
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_OUT_OF_MEM;
	}
	memcpy(copy, data, size);
	buf = schro_buffer_new_with_data(copy, size);
	schro_decoder_autoparse_push(dec, buf);

	for (;;)
	{
		int state = schro_decoder_autoparse_wait(dec);

		if (state == SCHRO_DECODER_FIRST_ACCESS_UNIT)
		{
			diracdec_read_format(ctx, dec);
		}
		else if (state == SCHRO_DECODER_NEED_BITS)
		{
			if (eos_pushed)
				break;
			schro_decoder_autoparse_push_end_of_sequence(dec);
			eos_pushed = GF_TRUE;
		}
		else if (state == SCHRO_DECODER_NEED_FRAME)
		{
			schro_decoder_add_output_picture(dec,
			    schro_frame_new_and_alloc(NULL, ctx->frame_format, ctx->width, ctx->height));
		}
		else if (state == SCHRO_DECODER_OK)
		{
			SchroFrame *frame = schro_decoder_pull(dec);
			if (frame)
			{
				e = diracdec_send_frame(ctx, frame);
				schro_frame_unref(frame);
				if (e != GF_OK)
					break;
			}
		}
		else if (state == SCHRO_DECODER_EOS)
		{
			if (eos_pushed)
				break;
		}
		else if (state == SCHRO_DECODER_ERROR)
		{
			GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[DiracDec] Decoder error\n"));
			e = GF_NON_COMPLIANT_BITSTREAM;
			break;
		}
		else if (state == SCHRO_DECODER_STALLED)
		{
			break;
		}
	}

	schro_decoder_free(dec);
	gf_filter_pid_drop_packet(ctx->ipid);

	if (e != GF_OK)
		return e;
	if (!ctx->cfg_done)
	{
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[DiracDec] No frame decoded\n"));
		return GF_NON_COMPLIANT_BITSTREAM;
	}
	gf_filter_pid_set_eos(ctx->opid);
	return GF_EOS;
}

static void diracdec_finalize(GF_Filter *filter)
{
}

static const GF_FilterCapability DiracDecCaps[] =
	{
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_FILE),
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_FILE_EXT, "drc|vc2"),
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_MIME, "video/dirac|video/x-dirac"),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_VISUAL),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_CODECID, GF_CODECID_RAW),
};

GF_FilterRegister DiracDecoderRegister = {
	.name = "diracdec",
	GF_FS_SET_DESCRIPTION("Dirac video decoder")
		GF_FS_SET_HELP("This filter decodes raw Dirac elementary streams using schroedinger. VC-2's high-quality profile is not covered: schroedinger predates it.")
			.private_size = sizeof(GF_DiracDecCtx),
	SETCAPS(DiracDecCaps),
	.configure_pid = diracdec_configure_pid,
	.process = diracdec_process,
	.finalize = diracdec_finalize,
};

const GF_FilterRegister *EMSCRIPTEN_KEEPALIVE diracdec_register(GF_FilterSession *session)
{
	return &DiracDecoderRegister;
}

#include "filter_register.h"
__attribute__((constructor))
void register_diracdec(void) {
    gf_filter_auto_register("diracdec", diracdec_register);
}
