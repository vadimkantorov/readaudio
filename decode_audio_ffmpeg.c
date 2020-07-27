// based on https://github.com/FFmpeg/FFmpeg/blob/master/doc/examples/decode_audio.c and https://github.com/FFmpeg/FFmpeg/blob/master/doc/examples/demuxing_decoding.c

#include <stdio.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>

/* BEGIN https://github.com/dmlc/dlpack/blob/master/include/dlpack/dlpack.h */

#include <stdint.h>
#include <stddef.h>

typedef enum {
  kDLCPU = 1,
  kDLGPU = 2,
  kDLCPUPinned = 3,
  kDLOpenCL = 4,
  kDLVulkan = 7,
  kDLMetal = 8,
  kDLVPI = 9,
  kDLROCM = 10,
  kDLExtDev = 12,
} DLDeviceType;

typedef struct {
  DLDeviceType device_type;
  int device_id;
} DLContext;

typedef enum {
  kDLInt = 0U,
  kDLUInt = 1U,
  kDLFloat = 2U
} DLDataTypeCode;

typedef struct {
  uint8_t code;
  uint8_t bits;
  uint16_t lanes;
} DLDataType;

typedef struct {
  void* data;
  DLContext ctx;
  int ndim;
  DLDataType dtype;
  int64_t* shape;
  int64_t* strides;
  uint64_t byte_offset;
} DLTensor;

typedef struct DLManagedTensor {
  DLTensor dl_tensor;
  void * manager_ctx;
  void (*deleter)(struct DLManagedTensor * self);
} DLManagedTensor;

/* END https://github.com/dmlc/dlpack/blob/master/include/dlpack/dlpack.h */

void deleter(struct DLManagedTensor* self)
{
	if(self->dl_tensor.data)
		free(self->dl_tensor.data);

	if(self->dl_tensor.shape)
		free(self->dl_tensor.shape);
	
	if(self->dl_tensor.strides)
		free(self->dl_tensor.strides);
}

void __attribute__ ((constructor)) onload()
{
	//needed before ffmpeg 4.0, deprecated in ffmpeg 4.0
	av_register_all();
	fprintf(stderr, "Ffmpeg initialized\n");
}

struct DecodeAudio
{
	char error[128];
	char fmt[8];
	uint64_t sample_rate;

	DLManagedTensor dl_managed_tensor;
};

int decode_packet(AVCodecContext *avctx, AVPacket *pkt, uint8_t** data, int itemsize)
{
	AVFrame *frame = av_frame_alloc();
	int ret = avcodec_send_packet(avctx, pkt);
	
	while (ret >= 0)
	{
		ret = avcodec_receive_frame(avctx, frame);
		if (ret == 0)
		{
			//data = memcpy(*data, frame->data) + itemsize * frame->nb_samples * avctx->channels;
			
			for (int i = 0; i < frame->nb_samples; i++)
				for (int ch = 0; ch < avctx->channels; ch++)
					*data = memcpy(*data, frame->data[ch] + itemsize * i, itemsize) + itemsize;
		}
	}

	if (ret == AVERROR(EAGAIN))
		ret = 0;
	
	av_frame_free(&frame);
	return ret;
}

struct DecodeAudio decode_audio(const char* input_path)
{
	struct DecodeAudio audio = {0};

	AVFormatContext *pFormatCtx = NULL;
	AVCodecContext* pCodecCtx = NULL;
	AVPacket* pkt = NULL;
	
	if (avformat_open_input(&pFormatCtx, input_path, NULL, NULL) != 0)
	{
		strcpy(audio.error, "Could not open file");
		goto end;
	}

	if (avformat_find_stream_info(pFormatCtx, NULL) < 0)
	{
		strcpy(audio.error, "Could not open find stream information");
		goto end;
	}

	int stream_index = av_find_best_stream(pFormatCtx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
	if (stream_index < 0)
	{
		strcpy(audio.error, "Could not find audio stream");
		goto end;
	}
	AVStream *stream = pFormatCtx->streams[stream_index];
	
	AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
	if (!codec)
	{
		strcpy(audio.error, "Codec not found");
		goto end;
	}

	pCodecCtx = avcodec_alloc_context3(codec);
	if (!pCodecCtx)
	{
		strcpy(audio.error, "Could not allocate audio codec context");
		goto end;
	}

	if (avcodec_parameters_to_context(pCodecCtx, stream->codecpar) < 0)
	{
		strcpy(audio.error, "Failed to copy audio codec parameters to decoder context");
		goto end;
	}

	if (avcodec_open2(pCodecCtx, codec, NULL) < 0)
	{
		strcpy(audio.error, "Could not open codec");
		goto end;
	}

	enum AVSampleFormat sample_fmt = pCodecCtx->sample_fmt;
	if (av_sample_fmt_is_planar(sample_fmt))
	{
		const char *packed = av_get_sample_fmt_name(sample_fmt);
		printf("Warning: the sample format the decoder produced is planar (%s). This example will output the first channel only.\n", packed ? packed : "?");
		sample_fmt = av_get_packed_sample_fmt(pCodecCtx->sample_fmt);
	}
    
	static struct sample_fmt_entry {enum AVSampleFormat sample_fmt; const char *fmt_be, *fmt_le;} sample_fmt_entries[] =
	{
		{ AV_SAMPLE_FMT_U8,  "u8",    "u8"    },
		{ AV_SAMPLE_FMT_S16, "s16be", "s16le" },
		{ AV_SAMPLE_FMT_S32, "s32be", "s32le" },
		{ AV_SAMPLE_FMT_FLT, "f32be", "f32le" },
		{ AV_SAMPLE_FMT_DBL, "f64be", "f64le" },
	};

	int i;
	for (i = 0; i < FF_ARRAY_ELEMS(sample_fmt_entries); i++)
	{
		struct sample_fmt_entry *entry = &sample_fmt_entries[i];
		if (sample_fmt == entry->sample_fmt)
		{
            		strcpy(audio.fmt, AV_NE(entry->fmt_be, entry->fmt_le));
			i = -1;
			break;
		}
	}

	if (i != -1)
	{
		strcpy(audio.error, "Could not deduce format");
		goto end;
	}

	audio.sample_rate = pCodecCtx->sample_rate;
	
	audio.dl_managed_tensor.deleter = deleter;
	audio.dl_managed_tensor.dl_tensor.ctx.device_type = kDLCPU;
	audio.dl_managed_tensor.dl_tensor.ndim = 2;
	audio.dl_managed_tensor.dl_tensor.dtype.code = kDLInt; // DETERMINE FROM fmt
	audio.dl_managed_tensor.dl_tensor.dtype.lanes = 1;
	audio.dl_managed_tensor.dl_tensor.dtype.bits = av_get_bytes_per_sample(sample_fmt) * 8;
	audio.dl_managed_tensor.dl_tensor.shape = malloc(audio.dl_managed_tensor.dl_tensor.ndim * sizeof(int64_t));
	audio.dl_managed_tensor.dl_tensor.shape[0] = (pFormatCtx->duration / (float) AV_TIME_BASE) * audio.sample_rate;
	audio.dl_managed_tensor.dl_tensor.shape[1] = pCodecCtx->channels;
	audio.dl_managed_tensor.dl_tensor.strides = malloc(audio.dl_managed_tensor.dl_tensor.ndim * sizeof(int64_t));
	audio.dl_managed_tensor.dl_tensor.strides[0] = audio.dl_managed_tensor.dl_tensor.shape[1];
	audio.dl_managed_tensor.dl_tensor.strides[1] = 1;
	
	size_t num_samples = audio.dl_managed_tensor.dl_tensor.shape[0], num_channels = audio.dl_managed_tensor.dl_tensor.shape[1];
	size_t itemsize = audio.dl_managed_tensor.dl_tensor.dtype.lanes * audio.dl_managed_tensor.dl_tensor.dtype.bits / 8;
	
	audio.dl_managed_tensor.dl_tensor.data = calloc(num_samples * num_channels, itemsize);
	uint8_t* data_ptr = audio.dl_managed_tensor.dl_tensor.data;
	pkt = av_packet_alloc();
	while (av_read_frame(pFormatCtx, pkt) >= 0)
	{
		if (pkt->stream_index == stream_index && decode_packet(pCodecCtx, pkt, &data_ptr, itemsize) < 0)
			break;
		av_packet_unref(pkt);
	}

	pkt->data = NULL;
	pkt->size = 0;
	decode_packet(pCodecCtx, pkt, &data_ptr, itemsize);

end:
	if(pCodecCtx)
		avcodec_free_context(&pCodecCtx);
	if(pkt)
		av_packet_free(&pkt);
	return audio;
}

int main(int argc, char **argv)
{
	if (argc <= 2)
	{
		printf("Usage: %s <input file> <output file>\n", argv[0]);
		return 1;
	}
	
	struct DecodeAudio audio = decode_audio(argv[1]);
	size_t num_samples = audio.dl_managed_tensor.dl_tensor.shape[0], num_channels = audio.dl_managed_tensor.dl_tensor.shape[1];
	size_t itemsize = audio.dl_managed_tensor.dl_tensor.dtype.lanes * audio.dl_managed_tensor.dl_tensor.dtype.bits / 8;
    
	printf("ffplay -f %s -ac %d -ar %d -i %s # num samples: %d\n", audio.fmt, (int)num_channels, (int)audio.sample_rate, argv[1], (int)num_samples);
	FILE *out = fopen(argv[2], "wb");
	
	fwrite(audio.dl_managed_tensor.dl_tensor.data, itemsize, num_samples * num_channels, out);
	fclose(out);
	return 0;
}
