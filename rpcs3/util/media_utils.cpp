#include "stdafx.h"
#include "media_utils.h"
#include "logs.hpp"
#include "Utilities/StrUtil.h"
#include "Emu/Cell/Modules/cellSearch.h"
#include "Emu/System.h"

#include <random>

#ifdef _MSC_VER
#pragma warning(push, 0)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
extern "C" {
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/dict.h"
#include "libavutil/opt.h"
#include "libswresample/swresample.h"
}
constexpr int averror_eof = AVERROR_EOF; // workaround for old-style-cast error
#ifdef _MSC_VER
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif

LOG_CHANNEL(media_log, "Media");

namespace utils
{
	template <>
	std::string media_info::get_metadata(const std::string& key, const std::string& def) const
	{
		if (metadata.contains(key))
		{
			return metadata.at(key);
		}

		return def;
	}

	template <>
	s64 media_info::get_metadata(const std::string& key, const s64& def) const
	{
		if (metadata.contains(key))
		{
			s64 result{};
			if (try_to_int64(&result, metadata.at(key), smin, smax))
			{
				return result;
			}
		}

		return def;
	}

	std::string av_error_to_string(int error)
	{
		char av_error[AV_ERROR_MAX_STRING_SIZE]{};
		av_make_error_string(av_error, AV_ERROR_MAX_STRING_SIZE, error);
		return av_error;
	}

	std::pair<bool, media_info> get_media_info(const std::string& path, s32 av_media_type)
	{
		media_info info{};
		info.path = path;

		if (av_media_type == AVMEDIA_TYPE_UNKNOWN) // Let's use this for image info
		{
			const bool success = Emu.GetCallbacks().get_image_info(path, info.sub_type, info.width, info.height, info.orientation);
			if (!success) media_log.error("get_image_info: failed to get image info for '%s'", path);
			return { success, std::move(info) };
		}

		// Only print FFMPEG errors, fatals and panics
		av_log_set_level(AV_LOG_ERROR);

		AVDictionary* av_dict_opts = nullptr;
		if (int err = av_dict_set(&av_dict_opts, "probesize", "96", 0); err < 0)
		{
			media_log.error("av_dict_set: returned with error=%d='%s'", err, av_error_to_string(err));
			return { false, std::move(info) };
		}

		AVFormatContext* av_format_ctx = avformat_alloc_context();

		// Open input file
		if (int err = avformat_open_input(&av_format_ctx, path.c_str(), nullptr, &av_dict_opts); err < 0)
		{
			// Failed to open file
			av_dict_free(&av_dict_opts);
			avformat_free_context(av_format_ctx);
			media_log.notice("avformat_open_input: could not open file. error=%d='%s' file='%s'", err, av_error_to_string(err), path);
			return { false, std::move(info) };
		}
		av_dict_free(&av_dict_opts);

		// Find stream information
		if (int err = avformat_find_stream_info(av_format_ctx, nullptr); err < 0)
		{
			// Failed to load stream information
			avformat_close_input(&av_format_ctx);
			avformat_free_context(av_format_ctx);
			media_log.notice("avformat_find_stream_info: could not load stream information. error=%d='%s' file='%s'", err, av_error_to_string(err), path);
			return { false, std::move(info) };
		}

		// Derive first stream id and type from avformat context.
		// We are only interested in the first matching stream for now.
		int audio_stream_index = -1;
		int video_stream_index = -1;
		for (uint i = 0; i < av_format_ctx->nb_streams; i++)
		{
			switch (av_format_ctx->streams[i]->codecpar->codec_type)
			{
			case AVMEDIA_TYPE_AUDIO:
				if (audio_stream_index < 0)
					audio_stream_index = i;
				break;
			case AVMEDIA_TYPE_VIDEO:
				if (video_stream_index < 0)
					video_stream_index = i;
				break;
			default:
				break;
			}
		}

		// Abort if there is no natching stream or if the stream isn't the first one
		if ((av_media_type == AVMEDIA_TYPE_AUDIO && audio_stream_index != 0) ||
			(av_media_type == AVMEDIA_TYPE_VIDEO && video_stream_index != 0))
		{
			// Failed to find a stream
			avformat_close_input(&av_format_ctx);
			avformat_free_context(av_format_ctx);
			media_log.notice("Failed to match stream of type %d in file='%s'", av_media_type, path);
			return { false, std::move(info) };
		}

		// Get video info if available
		if (video_stream_index >= 0)
		{
			const AVStream* stream = av_format_ctx->streams[video_stream_index];
			info.video_av_codec_id = stream->codecpar->codec_id;
			info.video_bitrate_bps = static_cast<s32>(stream->codecpar->bit_rate);
		}

		// Get audio info if available
		if (audio_stream_index >= 0)
		{
			const AVStream* stream = av_format_ctx->streams[audio_stream_index];
			info.audio_av_codec_id = stream->codecpar->codec_id;
			info.audio_bitrate_bps = static_cast<s32>(stream->codecpar->bit_rate);
			info.sample_rate = stream->codecpar->sample_rate;
		}

		info.duration_us = av_format_ctx->duration;

		AVDictionaryEntry* tag = nullptr;
		while ((tag = av_dict_get(av_format_ctx->metadata, "", tag, AV_DICT_IGNORE_SUFFIX)))
		{
			info.metadata[tag->key] = tag->value;
		}

		avformat_close_input(&av_format_ctx);
		avformat_free_context(av_format_ctx);

		return { true, std::move(info) };
	}

	struct scoped_av
	{
		AVFormatContext* format = nullptr;
		const AVCodec* codec = nullptr;
		AVCodecContext* context = nullptr;
		AVFrame* frame = nullptr;
		SwrContext* swr = nullptr;
	
		~scoped_av()
		{
			// Clean up
			if (frame)
				av_frame_free(&frame);
			if (swr)
				swr_free(&swr);
			if (context)
				avcodec_close(context);
			// AVCodec is managed by libavformat, no need to free it
			// see: https://stackoverflow.com/a/18047320
			if (format)
				avformat_free_context(format);
		}
	};

	audio_decoder::audio_decoder()
	{
	}

	audio_decoder::~audio_decoder()
	{
		stop();
	}

	void audio_decoder::set_context(music_selection_context context)
	{
		m_context = std::move(context);
	}

	void audio_decoder::set_swap_endianness(bool swapped)
	{
		m_swap_endianness = swapped;
	}

	void audio_decoder::clear()
	{
		track_fully_decoded = false;
		track_fully_consumed = false;
		has_error = false;
		m_size = 0;
		duration_ms = 0;
		timestamps_ms.clear();
		data.clear();
	}

	void audio_decoder::stop()
	{
		if (m_thread)
		{
			auto& thread = *m_thread;
			thread = thread_state::aborting;
			track_fully_consumed = true;
			track_fully_consumed.notify_one();
			thread();
			m_thread.reset();
		}

		clear();
	}

	void audio_decoder::decode()
	{
		stop();

		media_log.notice("audio_decoder: %d entries in playlist. Start decoding...", m_context.playlist.size());

		const auto decode_track = [this](const std::string& path)
		{
			media_log.notice("audio_decoder: decoding %s", path);
			scoped_av av;

			// Get format from audio file
			av.format = avformat_alloc_context();
			if (int err = avformat_open_input(&av.format, path.c_str(), nullptr, nullptr); err < 0)
			{
				media_log.error("audio_decoder: Could not open file '%s'. Error: %d='%s'", path, err, av_error_to_string(err));
				has_error = true;
				return;
			}
			if (int err = avformat_find_stream_info(av.format, nullptr); err < 0)
			{
				media_log.error("audio_decoder: Could not retrieve stream info from file '%s'. Error: %d='%s'", path, err, av_error_to_string(err));
				has_error = true;
				return;
			}

			// Find the first audio stream
			AVStream* stream = nullptr;
			unsigned int stream_index;
			for (stream_index = 0; stream_index < av.format->nb_streams; stream_index++)
			{
				if (av.format->streams[stream_index]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
				{
					stream = av.format->streams[stream_index];
					break;
				}
			}
			if (!stream)
			{
				media_log.error("audio_decoder: Could not retrieve audio stream from file '%s'", path);
				has_error = true;
				return;
			}

			// Find decoder
			av.codec = avcodec_find_decoder(stream->codecpar->codec_id);
			if (!av.codec)
			{
				media_log.error("audio_decoder: Failed to find decoder for stream #%u in file '%s'", stream_index, path);
				has_error = true;
				return;
			}

			// Allocate context
			av.context = avcodec_alloc_context3(av.codec);
			if (!av.context)
			{
				media_log.error("audio_decoder: Failed to allocate context for stream #%u in file '%s'", stream_index, path);
				has_error = true;
				return;
			}

			// Open decoder
			if (int err = avcodec_open2(av.context, av.codec, nullptr); err < 0)
			{
				media_log.error("audio_decoder: Failed to open decoder for stream #%u in file '%s'. Error: %d='%s'", stream_index, path, err, av_error_to_string(err));
				has_error = true;
				return;
			}

			// Prepare resampler
			av.swr = swr_alloc();
			if (!av.swr)
			{
				media_log.error("audio_decoder: Failed to allocate resampler for stream #%u in file '%s'", stream_index, path);
				has_error = true;
				return;
			}

			const int dst_channels = 2;
			const u64 dst_channel_layout = AV_CH_LAYOUT_STEREO;
			const AVSampleFormat dst_format = AV_SAMPLE_FMT_FLT;

			int set_err = 0;
			if ((set_err = av_opt_set_int(av.swr, "in_channel_count", stream->codecpar->channels, 0)) ||
				(set_err = av_opt_set_int(av.swr, "out_channel_count", dst_channels, 0)) ||
				(set_err = av_opt_set_channel_layout(av.swr, "in_channel_layout", stream->codecpar->channel_layout, 0)) ||
				(set_err = av_opt_set_channel_layout(av.swr, "out_channel_layout", dst_channel_layout, 0)) ||
				(set_err = av_opt_set_int(av.swr, "in_sample_rate", stream->codecpar->sample_rate, 0)) ||
				(set_err = av_opt_set_int(av.swr, "out_sample_rate", sample_rate, 0)) ||
				(set_err = av_opt_set_sample_fmt(av.swr, "in_sample_fmt", static_cast<AVSampleFormat>(stream->codecpar->format), 0)) ||
				(set_err = av_opt_set_sample_fmt(av.swr, "out_sample_fmt", dst_format, 0)))
			{
				media_log.error("audio_decoder: Failed to set resampler options: Error: %d='%s'", set_err, av_error_to_string(set_err));
				has_error = true;
				return;
			}

			if (int err = swr_init(av.swr); err < 0 || !swr_is_initialized(av.swr))
			{
				media_log.error("audio_decoder: Resampler has not been properly initialized: %d='%s'", err, av_error_to_string(err));
				has_error = true;
				return;
			}

			// Prepare to read data
			av.frame = av_frame_alloc();
			if (!av.frame)
			{
				media_log.error("audio_decoder: Error allocating the frame");
				has_error = true;
				return;
			}

			duration_ms = stream->duration / 1000;
			
			AVPacket packet{};
			av_init_packet(&packet);

			// Iterate through frames
			while (thread_ctrl::state() != thread_state::aborting && av_read_frame(av.format, &packet) >= 0)
			{
				if (int err = avcodec_send_packet(av.context, &packet); err < 0)
				{
					media_log.error("audio_decoder: Queuing error: %d='%s'", err, av_error_to_string(err));
					has_error = true;
					return;
				}

				while (thread_ctrl::state() != thread_state::aborting)
				{
					if (int err = avcodec_receive_frame(av.context, av.frame); err < 0)
					{
						if (err == AVERROR(EAGAIN) || err == averror_eof)
							break;

						media_log.error("audio_decoder: Decoding error: %d='%s'", err, av_error_to_string(err));
						has_error = true;
						return;
					}

					// Resample frames
					u8* buffer;
					const int align = 1;
					const int buffer_size = av_samples_alloc(&buffer, nullptr, dst_channels, av.frame->nb_samples, dst_format, align);
					if (buffer_size < 0)
					{
						media_log.error("audio_decoder: Error allocating buffer: %d='%s'", buffer_size, av_error_to_string(buffer_size));
						has_error = true;
						return;
					}

					const int frame_count = swr_convert(av.swr, &buffer, av.frame->nb_samples, const_cast<const uint8_t**>(av.frame->data), av.frame->nb_samples);
					if (frame_count < 0)
					{
						media_log.error("audio_decoder: Error converting frame: %d='%s'", frame_count, av_error_to_string(frame_count));
						has_error = true;
						if (buffer)
							av_free(buffer);
						return;
					}

					// Append resampled frames to data
					{
						std::scoped_lock lock(m_mtx);
						data.resize(m_size + buffer_size);

						if (m_swap_endianness)
						{
							// The format is float 32bit per channel.
							const auto write_byteswapped = [](const void* src, void* dst) -> void
							{
								*static_cast<f32*>(dst) = *static_cast<const be_t<f32>*>(src);
							};

							for (size_t i = 0; i < (buffer_size - sizeof(f32)); i += sizeof(f32))
							{
								write_byteswapped(buffer + i, data.data() + m_size + i);
							}
						}
						else
						{
							memcpy(&data[m_size], buffer, buffer_size);
						}

						const u32 timestamp_ms = stream->time_base.den ? (1000 * av.frame->best_effort_timestamp * stream->time_base.num) / stream->time_base.den : 0;
						timestamps_ms.push_back({m_size, timestamp_ms});
						m_size += buffer_size;
					}

					if (buffer)
						av_free(buffer);

					media_log.notice("audio_decoder: decoded frame_count=%d buffer_size=%d timestamp_us=%d", frame_count, buffer_size, av.frame->best_effort_timestamp);
				}
			}
		};

		m_thread = std::make_unique<named_thread<std::function<void()>>>("Music Decode Thread", [this, decode_track]()
		{
			for (const std::string& track : m_context.playlist)
			{
				media_log.notice("audio_decoder: playlist entry: %s", track);
			}

			if (m_context.playlist.empty())
			{
				media_log.error("audio_decoder: Can not play empty playlist");
				has_error = true;
				return;
			}

			m_context.current_track = m_context.first_track;

			if (m_context.context_option == CELL_SEARCH_CONTEXTOPTION_SHUFFLE && m_context.playlist.size() > 1)
			{
				// Shuffle once if necessary
				media_log.notice("audio_decoder: shuffling initial playlist...");
				auto engine = std::default_random_engine{};
				std::shuffle(std::begin(m_context.playlist), std::end(m_context.playlist), engine);
			}

			while (thread_ctrl::state() != thread_state::aborting)
			{
				ensure(m_context.current_track < m_context.playlist.size());
				media_log.notice("audio_decoder: about to decode: %s (index=%d)", m_context.playlist.at(m_context.current_track), m_context.current_track);

				decode_track(m_context.playlist.at(m_context.current_track));
				track_fully_decoded = true;

				if (has_error)
				{
					media_log.notice("audio_decoder: stopping with error...");
					break;
				}

				// Let's only decode one track at a time. Wait for the consumer to finish reading the track.
				media_log.notice("audio_decoder: waiting until track is consumed...");
				thread_ctrl::wait_on(track_fully_consumed, false);
				track_fully_consumed = false;
			}

			media_log.notice("audio_decoder: finished playlist");
		});
	}

	u32 audio_decoder::set_next_index(bool next)
	{
		return m_context.step_track(next);
	}
}
