/*
 * Copyright 2010-2019 Bluecherry, LLC
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "media_writer.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <assert.h>
#include <string>

extern "C" {
#include <libavutil/mathematics.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include "bc-server.h"
#include "lavf_device.h"

static void bc_avlog(int val, const char *msg)
{
	char err[512] = { 0 };
	av_strerror(val, err, sizeof(err));
	bc_log(Error, "%s: %s", msg, err);
}

///////////////////////////////////////////////////////////////
// S.K. >> Implementation of separated media writer
///////////////////////////////////////////////////////////////

media_writer::media_writer():
	last_mux_dts{AV_NOPTS_VALUE, AV_NOPTS_VALUE}
{
}

media_writer::~media_writer()
{
	close();
}

bool media_writer::write_packet(const stream_packet &pkt)
{
	AVStream *stream = 0;
	AVPacket opkt;
	int re;

	if (pkt.type == AVMEDIA_TYPE_AUDIO) stream = audio_st;
	else if (pkt.type == AVMEDIA_TYPE_VIDEO) stream = video_st;
	else bc_log(Debug, "write_packet: ignoring unexpected packet: pkt.type %d, dts %" PRId64 ", pts %" PRId64 ", size %d",
		pkt.type, pkt.dts, pkt.pts, pkt.size);

	if (!stream) return true;
	av_init_packet(&opkt);

	opkt.flags        = pkt.flags;
	opkt.pts          = av_rescale_q_rnd(pkt.pts, AV_TIME_BASE_Q, stream->time_base, (enum AVRounding)(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
	opkt.dts          = av_rescale_q_rnd(pkt.dts, AV_TIME_BASE_Q, stream->time_base, (enum AVRounding)(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
	opkt.data         = const_cast<uint8_t*>(pkt.data());
	opkt.size         = pkt.size;
	opkt.stream_index = stream->index;

	/* Fix non-increasing timestamps */
	static_assert(AVMEDIA_TYPE_VIDEO == 0);
	static_assert(AVMEDIA_TYPE_AUDIO == 1);
	int64_t &last_mux_dts = this->last_mux_dts[pkt.type];
	if (opkt.dts == AV_NOPTS_VALUE) {
		// Do nothing.
		// In ffmpeg, ffmpeg_mux.c:write_packet() does nothing and
		// lavf/mux.c:compute_muxer_pkt_fields() deals with it for now
		bc_log(Info, "Got bad dts=NOPTS on stream %d, passing to libavformat to handle", opkt.stream_index);
	} else if (last_mux_dts == AV_NOPTS_VALUE) {
		last_mux_dts = opkt.dts;
		// First packet ever. Initialize last_mux_dts and move on.
	} else if (last_mux_dts < opkt.dts) {
		// Monotonically increasing timestamps. This is normal.

		const int tolerated_gap_seconds = 1;
		int64_t delta_dts = opkt.dts - last_mux_dts;
		if (delta_dts > tolerated_gap_seconds * AV_TIME_BASE) {
			// Too large a gap, assume discontinuity and close this recording file.
			bc_log(Info, "Bad timestamp: too large a gap of %d seconds (%d tolerated), dts=%" PRId64 " while last was %" PRId64 " on stream %d, bailing out, causing the recording file to restart", (int)(delta_dts / AV_TIME_BASE), tolerated_gap_seconds, opkt.dts, last_mux_dts, opkt.stream_index);
			return false;
		}
	} else {
		// In this clause we're dealing with incorrect timestamp received from the source.
		assert(last_mux_dts >= opkt.dts);
		assert(last_mux_dts != AV_NOPTS_VALUE);
		bc_log(Info, "Got bad dts=%" PRId64 " while last was %" PRId64 " on stream %d, bailing out, causing the recording file to restart", opkt.dts, last_mux_dts, opkt.stream_index);
		return false;
	}

	bc_log(Debug, "av_interleaved_write_frame: dts=%" PRId64 " pts=%" PRId64 " tb=%d/%d s_i=%d k=%d",
		opkt.dts, opkt.pts, out_ctx->streams[opkt.stream_index]->time_base.num,
		out_ctx->streams[opkt.stream_index]->time_base.den, opkt.stream_index,
		!!(opkt.flags & AV_PKT_FLAG_KEY));

	re = av_interleaved_write_frame(out_ctx, &opkt);
	if (re < 0)
	{
		if (re == AVERROR(EINVAL)) {
			bc_log(Error, "Error writing frame to recording. Likely timestamping problem.");
		} else {
			bc_avlog(re, "Error writing frame to recording.");
		}
		return false;
	}
	last_mux_dts = opkt.dts;

	return true;
}

void media_writer::close()
{
	video_st = audio_st = NULL;
	for (int i = 0; i < sizeof(last_mux_dts)/sizeof(last_mux_dts[0]); i++) {
		last_mux_dts[i] = AV_NOPTS_VALUE;
	}

	if (out_ctx)
	{
		if (out_ctx->pb)
			av_write_trailer(out_ctx);

		if (out_ctx->pb)
			avio_close(out_ctx->pb);

		avformat_free_context(out_ctx);
		out_ctx = NULL;
	}
}

#define MKDIR_RECURSIVE_DEPTH 25

/* XXX: this function doesn't belong here */
int bc_mkdir_recursive(char *path)
{
	unsigned int depth = 0;
	char *bp[MKDIR_RECURSIVE_DEPTH];

	while (depth <= MKDIR_RECURSIVE_DEPTH)
	{
		if (!mkdir(path, 0750) || errno == EEXIST)
		{
			if (!depth) return 0;

			/* Continue with next child */
			*bp[--depth] = '/';
			continue;
		}

		if (errno != ENOENT)
			goto error;

		/* Missing parent, try to make it */
		bp[depth] = strrchr(path, '/');
		if (!bp[depth] || bp[depth] == path)
		{
			errno = EINVAL;
			goto error;
		}

		*bp[depth++] = 0;
	}

	bc_log(Error, "mkdir_recursive: path too deep: %s", path);

 error:
	/* Revert path string to it's original state */
	while (depth--)
		*bp[depth] = '/';

	return -1;
}

#if 0 /* XXX unused */
static int setup_solo_output(struct bc_record *bc_rec, AVFormatContext *oc)
{
	AVStream *st;
	int fden   = bc_rec->bc->v4l2.vparm.parm.capture.timeperframe.denominator;
	int fnum   = bc_rec->bc->v4l2.vparm.parm.capture.timeperframe.numerator;
	int width  = bc_rec->bc->v4l2.vfmt.fmt.pix.width;
	int height = bc_rec->bc->v4l2.vfmt.fmt.pix.height;

	/* Setup new video stream */
	if ((st = avformat_new_stream(oc, NULL)) == NULL)
		return -1;

	st->time_base.den = fden;
	st->time_base.num = fnum;

	if (bc_rec->bc->v4l2.codec_id == AV_CODEC_ID_NONE) {
		bc_dev_warn(bc_rec, "Invalid CODEC, assuming H264");
		st->codec->codec_id = AV_CODEC_ID_H264;
	} else
		st->codec->codec_id = bc_rec->bc->v4l2.codec_id;

	/* h264 requires us to work around libavcodec broken defaults */
	if (st->codec->codec_id == AV_CODEC_ID_H264) {
		st->codec->me_range = 16;
		st->codec->me_subpel_quality = 7;
		st->codec->qmin = 10;
		st->codec->qmax = 51;
		st->codec->max_qdiff = 4;
		st->codec->qcompress = 0.6;
		st->codec->i_quant_factor = 0.71;
		st->codec->b_frame_strategy = 1;
	}

	st->codec->codec_type = AVMEDIA_TYPE_VIDEO;
	st->codec->pix_fmt = AV_PIX_FMT_YUV420P;
	st->codec->width = width;
	st->codec->height = height;
	st->codec->time_base.num = fnum;
	st->codec->time_base.den = fden;

	if (oc->oformat->flags & AVFMT_GLOBALHEADER)
		st->codec->flags |= CODEC_FLAG_GLOBAL_HEADER;

	st = NULL;

	/* We don't fail when this happens. Video with no sound is
	 * better than no video at all. */
	if (!bc_rec->cfg.aud_disabled && bc_alsa_open(bc_rec))
		bc_alsa_close(bc_rec);

	/* Setup new audio stream */
	if (has_audio(bc_rec)) {
		enum AVCodecID codec_id = AV_CODEC_ID_PCM_S16LE;

		/* If we can't find an encoder, just skip it */
		if (avcodec_find_encoder(codec_id) == NULL) {
			bc_dev_warn(bc_rec, "Failed to find audio codec (%08x) "
				"so not recording", codec_id);
			goto no_audio;
		}

		if ((st = avformat_new_stream(oc, NULL)) == NULL)
			goto no_audio;
		st->codec->codec_id = codec_id;
		st->codec->codec_type = AVMEDIA_TYPE_AUDIO;

		st->codec->sample_rate = 8000;
		st->codec->sample_fmt = AV_SAMPLE_FMT_S16;
		st->codec->channels = 1;
		st->codec->time_base = (AVRational){1, 8000};

		if (oc->oformat->flags & AVFMT_GLOBALHEADER)
			st->codec->flags |= CODEC_FLAG_GLOBAL_HEADER;
no_audio:
		st = NULL;
	}

	return 0;
}

int setup_out_ctx(struct bc_record *bc_rec, struct AVFormatContext *oc)
{
	struct bc_handle *bc = bc_rec->bc;
	if (bc->type == BC_DEVICE_LAVF)
		return bc->input->setup_output(oc);
	else if (bc->type == BC_DEVICE_V4L2 && (bc->cam_caps & BC_CAM_CAP_SOLO))
		return setup_solo_output(bc_rec, oc);
	else
		return -1;
}
#endif

int media_writer::open(const std::string &path, const stream_properties &properties)
{
	if (out_ctx) return 0;
	char error[512];
	int ret = 0;

	AVDictionary *muxer_opts = NULL;
	AVCodec *codec;

	/* Get the output format */
	const AVOutputFormat *fmt_out = av_guess_format("mp4", NULL, "video/mp4");
	if (fmt_out == NULL)
	{
		bc_log(Error, "media_writer: MP4 output format is not found!");
		errno = EINVAL;

		close();
		return -1;
	}

	avformat_alloc_output_context2(&out_ctx, fmt_out, NULL, path.c_str());
	if (out_ctx == NULL)
	{
		close();
		return -1;
	}

	video_st = avformat_new_stream(out_ctx, NULL);
	if (!video_st)
	{
		close();
		return -1;
	}

	properties.video.apply(video_st->codecpar);

	if (properties.has_audio())
	{
		audio_st = avformat_new_stream(out_ctx, NULL);
		if (!audio_st)
		{
			close();
			return -1;
		}

		properties.audio.apply(audio_st->codecpar);
	}

	/* Open output file */
	if ((ret = avio_open(&out_ctx->pb, path.c_str(), AVIO_FLAG_WRITE)) < 0)
	{
		av_strerror(ret, error, sizeof(error));
		bc_log(Error, "Cannot open media output file %s: %s (%d)",
			   path.c_str(), error, ret);

		close();
		return -1;
	}

	this->recording_path = path;
	av_dict_set(&muxer_opts, "avoid_negative_ts", "+make_zero", 0);

	ret = avformat_write_header(out_ctx, &muxer_opts);
	av_dict_free(&muxer_opts);

	if (ret)
	{
		av_strerror(ret, error, sizeof(error));
		bc_log(Error, "Failed to init muxer for output file %s: %s (%d)",
				recording_path.c_str(), error, ret);

		avio_closep(&out_ctx->pb);
		close();
		return -1;
	}

	return 0;
}

///////////////////////////////////////////////////////////////
// S.K. >> Implementation of separated snapshot writer
///////////////////////////////////////////////////////////////

snapshot_writer::snapshot_writer(const char *path)
{
	output_path = std::string(path);
}

snapshot_writer::~snapshot_writer()
{
	cleanup();
}

void snapshot_writer::cleanup(bool flush)
{
	destroy_encoder(flush);
	destroy_decoder();
	destroy_muxer();
}

void snapshot_writer::destroy_encoder(bool flush)
{
	if (encoder != NULL)
	{
		if (flush)
		{
			AVPacket avpkt;
			av_init_packet(&avpkt);

			int ret = avcodec_send_frame(encoder, NULL);
			while (ret >= 0)
			{
				ret = avcodec_receive_packet(encoder, &avpkt);
				if (ret < 0) break;
				av_packet_unref(&avpkt);
			}
		}

		avcodec_close(encoder);
		av_freep(&encoder);
		encoder = NULL;
	}

	if (output_file != NULL)
	{
		fclose(output_file);
		output_file = NULL;
	}
}

void snapshot_writer::destroy_decoder(void)
{
	if (decoder != NULL)
	{
		av_freep(&decoder->extradata);
		decoder->extradata_size = 0;

		avcodec_close(decoder);
		av_freep(&decoder);
		decoder = NULL;
	}
}

void snapshot_writer::destroy_muxer()
{
	if (out_ctx)
	{
		if (out_ctx->pb)
			av_write_trailer(out_ctx);

		if (out_ctx->pb)
			avio_close(out_ctx->pb);

		avformat_free_context(out_ctx);
		out_ctx = NULL;
	}
}

int snapshot_writer::feed(const stream_packet &pkt)
{
	int ret = push_packet(pkt);
	if (!ret) bc_log(Debug, "Fed snapshot decoder with a frame and got picture");
	else if (ret < 0) bc_log(Error, "Feeding snapshot decoder failed");
	else bc_log(Debug, "Fed frame to snapshot decoder but it needs more");
	return ret;
}

int snapshot_writer::init_decoder(const stream_packet &pkt)
{
	if (decoder != NULL) return 0;

	std::shared_ptr<const stream_properties> properties = pkt.properties();
	const AVCodec *codec = avcodec_find_decoder(properties->video.codec_id);

	if (!codec || !(decoder = avcodec_alloc_context3(codec)))
	{
		bc_log(Error, "Failed to allocate snapshot decoder context");
		return -1;
	}

	properties->video.apply(decoder);
	AVDictionary *decoder_opts = NULL;
	av_dict_set(&decoder_opts, "refcounted_frames", "1", 0);

	int ret = avcodec_open2(decoder, codec, &decoder_opts);
	av_dict_free(&decoder_opts);

	if (ret < 0)
	{
		bc_avlog(ret, "Failed to initialize snapshot decoder context");
		av_free(decoder);
		decoder = NULL;
	}

	return ret;
}

/*
 * @return negative on failure, 0 if we have picture, positive if no picture so far
 */
int snapshot_writer::push_packet(const stream_packet &pkt)
{
	int ret = init_decoder(pkt);
	if (ret < 0) return ret;

	AVPacket packet;
	av_init_packet(&packet);

	packet.flags        = pkt.is_key_frame() ? AV_PKT_FLAG_KEY : 0;
	/* Setting DTS from PTS is silly, but in curent scheme DTS is lost.
	 * Should work for decoding one frame right after keyframe (most of time?) */
	packet.dts          = pkt.pts;
	packet.pts          = pkt.pts;
	packet.data         = const_cast<uint8_t*>(pkt.data());
	packet.size         = pkt.size;

	ret = avcodec_send_packet(decoder, &packet);
	if (ret < 0)
	{
		bc_avlog(ret, "avcodec_send_packet failed for snapshot");
		return ret;
	}

	AVFrame *frame = av_frame_alloc();
	if (!frame) return AVERROR(ENOMEM);

	bool got_frame = false;
	bool frame_done = false;

	while (ret >= 0)
	{
		ret = avcodec_receive_frame(decoder, frame);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
		{
			ret = 1;
			break;
		}

		if (ret < 0)
		{
			if (got_frame)
			{
				ret = 0;
				break;
			}

			bc_avlog(ret, "avcodec_receive_frame failed");
			break;
		}

		got_frame = true;
		ret = write_frame(frame);

		if (ret < 0)
		{
			bc_log(Error, "failed to write snapshot frame");
			break;
		}
		else if (ret > 0)
		{
			av_frame_unref(frame);
			continue;
		}

		break;
	}

	av_frame_free(&frame);
	return ret;
}

int snapshot_writer::init_encoder(int width, int height)
{
	if (encoder == NULL)
	{
		const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
		if (!codec || !(encoder = avcodec_alloc_context3(codec)))
		{
			bc_log(Bug, "Failed to allocate encoder context for snapshot");
			return -1;
		}

		encoder->width   = width;
		encoder->height  = height;
		encoder->pix_fmt = AV_PIX_FMT_YUVJ420P;
		encoder->mb_lmin = encoder->qmin * FF_QP2LAMBDA;
		encoder->mb_lmax = encoder->qmax * FF_QP2LAMBDA;
		encoder->flags  |= AV_CODEC_FLAG_QSCALE;
		encoder->global_quality = encoder->qmin * FF_QP2LAMBDA;
		encoder->time_base.num = 1;
		encoder->time_base.den = 30000;

		int ret = avcodec_open2(encoder, codec, NULL);
		if (ret < 0)
		{
			bc_avlog(ret, "Failed to init encoding codec for snapshot");
			return -1;
		}
	}

	if (output_file == NULL)
	{
		output_file = fopen(output_path.c_str(), "w");
		if (output_file == NULL)
		{
			bc_log(Error, "Failed to open snapshot file: '%s' (%s)",
				output_path.c_str(), strerror(errno));
			return -1;
		}
	}

	return 0;
}

AVFrame* snapshot_writer::scale_frame(AVFrame *rawFrame, bool &allocated)
{
	// No actual size rescaling takes place here (it could though),
	// just pixel format change for compatibility with JPEG file format.
	AVFrame *frame = av_frame_alloc();
	if (frame == NULL)
	{
		bc_log(Error, "Failed to allocate frame for scaling: %s", strerror(errno));
		return NULL;
	}
	frame->format = AV_PIX_FMT_YUVJ420P;
	frame->width = rawFrame->width;
	frame->height = rawFrame->height;

	if (rawFrame->format != frame->format)
	{
		SwsContext *sws = NULL;
		sws = sws_getCachedContext(NULL,
				rawFrame->width, rawFrame->height, (AVPixelFormat)rawFrame->format,
				rawFrame->width, rawFrame->height, (AVPixelFormat)frame->format,
				SWS_BICUBIC, NULL, NULL, NULL);

		if (sws == NULL)
		{
			bc_log(Bug, "Failed to convert pixel format for JPEG (format is %d)", rawFrame->format);
			av_frame_free(&frame);
			return NULL;
		}

		int ret = av_image_alloc( frame->data, frame->linesize,
			rawFrame->width, rawFrame->height, (AVPixelFormat)frame->format, 4);

		if (ret < 0)
		{
			bc_log(Error, "Failed to allocate picture for scaling: %s", strerror(errno));
			sws_freeContext(sws);
			av_frame_free(&frame);
			return NULL;
		}

		sws_scale(sws,
			(const uint8_t**)rawFrame->data,
			rawFrame->linesize, 0,
			rawFrame->height,
			frame->data,
			frame->linesize);
		sws_freeContext(sws);

		allocated = true;
		return frame;
	}

	av_frame_move_ref(frame, rawFrame);
	allocated = false;
	return frame;
}

/*
 * @return negative on error, 0 otherwise
 */
int snapshot_writer::write_frame(AVFrame *rawFrame)
{
	int ret = init_encoder(rawFrame->width, rawFrame->height);
	if (ret < 0) return -1;

	AVPacket avpkt;
	av_init_packet(&avpkt);

	avpkt.size = 0;
	avpkt.data = NULL;
	FILE *file = NULL;

	bool allocated = false;
	bool got_pkt = false;

	AVFrame *frame = scale_frame(rawFrame, allocated);
	if (frame == NULL) return -1;

	ret = avcodec_send_frame(encoder, frame);
	if (ret < 0)
	{
		bc_avlog(ret, "avcodec_send_frame: snapshot encoding failed");
		if (allocated) av_freep(&frame->data[0]);
		av_frame_free(&frame);
		return -1;
	}

	while (ret >= 0)
	{
		ret = avcodec_receive_packet(encoder, &avpkt);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
		{
			ret = got_pkt ? 0 : 1;
			break;
		}

		if (ret < 0)
		{
			if (got_pkt)
			{
				ret = 0;
				break;
			}

			bc_avlog(ret, "avcodec_receive_packet: snapshot encoding failed");
			break;
		}

		assert(avpkt.size && avpkt.data);
		got_pkt = true;

		if (fwrite(avpkt.data, 1, avpkt.size, output_file) < (unsigned)avpkt.size)
		{
			bc_log(Error, "Failed to write snapshot file: %s", strerror(errno));
			ret = -1;
			break;
		}
	}

	if (allocated) av_freep(&frame->data[0]);
	av_frame_free(&frame);
	av_packet_unref(&avpkt);
	return ret;
}

///////////////////////////////////////////////////////////////
// S.K. >> END-OF: Implementation of separated snapshot writer
///////////////////////////////////////////////////////////////
