/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (C) 2020, Raspberry Pi (Trading) Ltd.
 *
 * output.cpp - video stream output base class
 */

#include "core/options.hpp"
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <curl/curl.h>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <cstdlib>    // for getenv
#include <pwd.h>      // for getpwuid
#include <sys/stat.h>
#include <unistd.h>   // for getuid

#include "circular_output.hpp"
#include "file_output.hpp"
#include "net_output.hpp"
#include "output.hpp"

Output::Output(VideoOptions const *options)
	: options_(options), fp_timestamps_(nullptr), state_(WAITING_KEYFRAME), time_offset_(0), last_timestamp_(0),
	  buf_metadata_(std::cout.rdbuf()), of_metadata_()
{
	if (!options->Get().save_pts.empty())
	{
		fp_timestamps_ = fopen(options->Get().save_pts.c_str(), "w");
		if (!fp_timestamps_)
			throw std::runtime_error("Failed to open timestamp file " + options->Get().save_pts);
		fprintf(fp_timestamps_, "# timecode format v2\n");
	}
	if (!options->Get().metadata.empty())
	{
		const std::string &filename = options_->Get().metadata;

		if (filename.compare("-"))
		{
			of_metadata_.open(filename, std::ios::out);
			buf_metadata_ = of_metadata_.rdbuf();
			start_metadata_output(buf_metadata_, options_->Get().metadata_format);
		}
	}
	if (!options->Get().webhook_url.empty())
	{
		webhook_url = options->Get().webhook_url;
	}

	enable_ = !options->Get().pause;
	max_pre_frames = static_cast<size_t>(
		std::ceil(options_->Get().pre_detection_secs * options_->Get().framerate.value())
	);

}

Output::~Output()
{
	if (fp_timestamps_)
		fclose(fp_timestamps_);
	if (!options_->Get().metadata.empty())
		stop_metadata_output(buf_metadata_, options_->Get().metadata_format);
}

void Output::Signal()
{
	enable_ = !enable_;
}

void Output::NotifyDetection(int sequence_id)
{
	detection_sequence_ = sequence_id;

	// We'll use last_timestamp_ as "current time" in microseconds,
	// but if you prefer real-time, you'll need to fetch it differently.
	int64_t now_us = last_timestamp_;

	if (!isMjpegRecording())
	{
	// Start a new MJPEG recording
		LOG(1, "Starting MJPEG recording due to detection.");
		record_start_timestamp = now_us;
		record_end_timestamp = now_us + static_cast<int64_t>(options_->Get().detection_record_secs * 1'000'000);
		startMjpegRecording(now_us);
		pending_flush_ = true;
		first_frame = true;
	}
	else
	{
		// We are already recording, so just extend our end time
		if (now_us + static_cast<int64_t>(options_->Get().detection_record_secs * 1'000'000) > record_end_timestamp)
		{
		    LOG(1, "Extending MJPEG recording due to new detection.");
		    record_end_timestamp = now_us + static_cast<int64_t>(options_->Get().detection_record_secs * 1'000'000);
		}
	}
}

void Output::OutputReady(void *mem, size_t size, int64_t timestamp_us, bool keyframe)
{
	// if a detection just armed the recorder, flush the buffer
	if (pending_flush_ && isMjpegRecording()) {
		flush_pre_buffer_to_mjpeg(timestamp_us);
		pending_flush_ = false;
	}

	if (max_pre_frames)
	{
		Frame f;
		f.bytes.assign(static_cast<uint8_t *>(mem), static_cast<uint8_t *>(mem) + size);
		f.ts = timestamp_us;
		f.keyframe = keyframe;

		pre_buffer_.push_back(std::move(f));
		while (pre_buffer_.size() > max_pre_frames) {
			pre_buffer_.pop_front();	
		}
	}


	// When output is enabled, we may have to wait for the next keyframe.
	uint32_t flags = keyframe ? FLAG_KEYFRAME : FLAG_NONE;
	if (!enable_)
		state_ = DISABLED;
	else if (state_ == DISABLED)
		state_ = WAITING_KEYFRAME;
	if (state_ == WAITING_KEYFRAME && keyframe)
		state_ = RUNNING, flags |= FLAG_RESTART;
	if (state_ != RUNNING)
		return;

	// Frig the timestamps to be continuous after a pause.
	if (flags & FLAG_RESTART)
		time_offset_ = timestamp_us - last_timestamp_;
	last_timestamp_ = timestamp_us - time_offset_;

	outputBuffer(mem, size, last_timestamp_, flags);

	// Save timestamps to a file, if that was requested.
	if (fp_timestamps_)
	{
		timestampReady(last_timestamp_);
	}

	if (!options_->Get().metadata.empty())
	{
		libcamera::ControlList metadata = metadata_queue_.front();
		write_metadata(buf_metadata_, options_->Get().metadata_format, metadata, !metadata_started_);
		metadata_started_ = true;
		metadata_queue_.pop();
	}

	if (detection_sequence_ >= 0)
	{
		LOG(1, "Attempt to call webhook");
		SendWebhook(mem, size, timestamp_us);
		detection_sequence_ = -1;
	}

	    // --- MJPEG RECORDING LOGIC STARTS HERE ---
	if (isMjpegRecording())
	{
		// Feed the raw buffer to the MJPEG file output
		// This will effectively write the frames to the .mjpeg file
		
		mjpeg_output->OutputReady(mem, size, last_timestamp_, keyframe);

		if (first_frame) {
			// write this frame to jpg as a thumbnail
			// clone the file output, change the filename to end in .jpg
			// write this frame and then close the file
			outputJpg(mem, size, timestamp_us, keyframe);
			first_frame = false;
		}

		// Check if we've gone past our record_end_timestamp_us_
		if (last_timestamp_ > record_end_timestamp)
		{
			LOG(1, "MJPEG recording window has ended.");
			stopMjpegRecording();
		}
	}
}

void Output::timestampReady(int64_t timestamp)
{
	fprintf(fp_timestamps_, "%" PRId64 ".%03" PRId64 "\n", timestamp / 1000, timestamp % 1000);
	if (options_->Get().flush)
		fflush(fp_timestamps_);
}

void Output::outputBuffer(void *mem, size_t size, int64_t timestamp_us, uint32_t flags)
{
	// Supply this so that a vanilla Output gives you an object that outputs no buffers.	
}

void Output::SendWebhook(void *mem, size_t size, int64_t timestamp_us)
{
	if (webhook_url.empty())
	{
		LOG_ERROR("webhook url is empty");
		return;
	}
	// Initialize a CURL handle
	CURL *curl = curl_easy_init();

	if (!curl)
	{
		LOG_ERROR("Failed to init curl");
	}

	// Set the URL
	curl_easy_setopt(curl, CURLOPT_URL, webhook_url.c_str());

	// We’re doing an HTTP POST
	curl_easy_setopt(curl, CURLOPT_POST, 1L);

	// Pass the POST data
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, mem);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(size));

	// Example: you could add a custom header to pass the timestamp
	struct curl_slist *headers = nullptr;
	std::string hdr = "X-Frame-Timestamp: " + std::to_string(timestamp_us);
	headers = curl_slist_append(headers, hdr.c_str());
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

	// Perform the request
	LOG(1, "Calling webhook: " << webhook_url.c_str());
	CURLcode res = curl_easy_perform(curl);
	bool success = (res == CURLE_OK);
	LOG(1, "Call result: " << success);
	if (!success)
	{
		LOG_ERROR("Failed to call endpoint:" << curl_easy_strerror(res));
	}

	// Cleanup
	if (headers)
		curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
}

Output *Output::Create(VideoOptions const *options)
{
	bool libav = options->Get().codec == "libav" ||
				 (options->Get().codec == "h264" && options->GetPlatform() != Platform::VC4);
	const std::string out_file = options->Get().output;

	if (!libav && (strncmp(out_file.c_str(), "udp://", 6) == 0 || strncmp(out_file.c_str(), "tcp://", 6) == 0))
		return new NetOutput(options);
	else if (options->Get().circular)
		return new CircularOutput(options);
	else if (!out_file.empty())
		return new FileOutput(options);
	else if (!options->Get().output.empty())
	{
		LOG(1, "FileOutput created");
		return new FileOutput(options);
	}
	else
		return new Output(options);
}

void Output::MetadataReady(libcamera::ControlList &metadata)
{
	if (options_->Get().metadata.empty())
		return;

	metadata_queue_.push(metadata);
}

void start_metadata_output(std::streambuf *buf, std::string fmt)
{
	std::ostream out(buf);
	if (fmt == "json")
		out << "[" << std::endl;
}

void write_metadata(std::streambuf *buf, std::string fmt, libcamera::ControlList &metadata, bool first_write)
{
	std::ostream out(buf);
	const libcamera::ControlIdMap *id_map = metadata.idMap();
	if (fmt == "txt")
	{
		for (auto const &[id, val] : metadata)
			out << id_map->at(id)->name() << "=" << val.toString() << std::endl;
		out << std::endl;
	}
	else
	{
		if (!first_write)
			out << "," << std::endl;
		out << "{";
		bool first_done = false;
		for (auto const &[id, val] : metadata)
		{
			std::string arg_quote = (val.toString().find('/') != std::string::npos) ? "\"" : "";
			out << (first_done ? "," : "") << std::endl
				<< "    \"" << id_map->at(id)->name() << "\": " << arg_quote << val.toString() << arg_quote;
			first_done = true;
		}
		out << std::endl << "}";
	}
}

void stop_metadata_output(std::streambuf *buf, std::string fmt)
{
	std::ostream out(buf);
	if (fmt == "json")
		out << std::endl << "]" << std::endl;
}

static std::string expandTilde(const std::string &path)
{
    // If the path doesn’t start with '~', just return as-is:
    if (path.empty() || path[0] != '~')
        return path;

    // Attempt to read the $HOME environment variable
    const char *homeDir = std::getenv("HOME");
    if (!homeDir)
    {
        // If $HOME is not set, fall back to the passwd database
        struct passwd *pw = getpwuid(getuid());
        if (pw)
            homeDir = pw->pw_dir; 
    }

    // If homeDir is still null for any reason, just return path as-is.
    if (!homeDir)
        return path;

    // Replace just the leading '~' with homeDir
    // e.g. "~" => "/home/username", or "~/sub/dir" => "/home/username/sub/dir"
    return std::string(homeDir) + path.substr(1);
}

static std::string generateIsoTimestamp()
{
    using namespace std::chrono;

    // Get current system clock time
    system_clock::time_point now = system_clock::now();

    // Convert to time_t for localtime
    std::time_t t = system_clock::to_time_t(now);
    std::tm tmLocal = *std::localtime(&t);

    // Extract fractional milliseconds
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

    // Build the formatted string
    // e.g. 2025-01-24-23-04-01-123
    std::ostringstream oss;
    oss << std::put_time(&tmLocal, "%Y-%m-%d-%H-%M-%S-")
        << std::setw(3) << std::setfill('0') << ms.count();

    return oss.str();
}

static std::string generateDateString()
{
    using namespace std::chrono;
    system_clock::time_point now = system_clock::now();
    std::time_t t = system_clock::to_time_t(now);
    std::tm tmLocal = *std::localtime(&t);

    // Format: yyyy-MM-dd
    std::ostringstream oss;
    oss << std::put_time(&tmLocal, "%Y-%m-%d");
    return oss.str();
}

static void createDirectoryIfNeeded(const std::string &path)
{
    struct stat st;
    if (stat(path.c_str(), &st) == 0)
    {
        // If stat succeeded, check if it's already a directory
        if (S_ISDIR(st.st_mode))
            return; // Directory already exists
        // If it exists but not a directory, handle error or rename
    }

    // Directory doesn’t exist, so create it
    if (mkdir(path.c_str(), 0755) != 0)
    {
        // mkdir failed
        LOG_ERROR("Failed to create directory: " << path << ", error: " << strerror(errno));
    }
}

void Output::startMjpegRecording(int64_t first_detection_timestamp)
{
    // We'll create a new VideoOptions for MJPEG recording.
    // We can't copy VideoOptions because it has unique_ptrs, so we create fresh and set what we need.
	if (mjpeg_opts_ == nullptr)
	{
		mjpeg_opts_ = std::make_unique<VideoOptions>();
		// Copy the essential settings from the original options
		mjpeg_opts_->Set().codec = "mjpeg";
		mjpeg_opts_->Set().quality = options_->Get().quality;
		mjpeg_opts_->Set().width = options_->Get().width;
		mjpeg_opts_->Set().height = options_->Get().height;
	}

    // Build a filename using the configured path (defaults to "~")
	// Expand the user-configured path. If it starts with '~', we resolve it.
    std::string basePath = expandTilde(options_->Get().detection_record_path);
    if (basePath.empty())
        basePath = "."; // fallback if something unexpected happens

	// 2) Generate YYYY-MM-DD
    std::string dateFolder = generateDateString();

    // 3) Create subfolder if it doesn't exist
    std::string subFolderPath = basePath + "/" + dateFolder;
    createDirectoryIfNeeded(subFolderPath);


    // Generate an ISO-like filename, e.g. 2025-01-24-23-04-01-123.mjpeg
    std::string isoName = generateIsoTimestamp() + ".mjpeg";

    // Combine them: "basePath/isoName"
    std::string fullFilename = subFolderPath + "/" + isoName;

    // Use that new path/filename in the secondary FileOutput
    mjpeg_opts_->Set().output = fullFilename;
    mjpeg_output.reset(Output::Create(mjpeg_opts_.get()));

	// flush the pre buffer to the mjpeg output
	//flush_pre_buffer_to_mjpeg();

    LOG(1, "Started MJPEG file output at: " << mjpeg_opts_->Get().output);
}

void Output::flush_pre_buffer_to_mjpeg(int64_t cutoff_ts)
{
	if(!isMjpegRecording() || pre_buffer_.empty())
		return;

	// for (auto &f : pre_buffer_) {
	// 	mjpeg_output->OutputReady(f.bytes.data(), f.bytes.size(), f.ts - time_offset_, f.keyframe);
	// }

	for (auto it = pre_buffer_.begin(); it != pre_buffer_.end(); ++it) {
		if (it->ts >= cutoff_ts) {
			break;
		}


	 	mjpeg_output->OutputReady(it->bytes.data(), it->bytes.size(), it->ts - time_offset_, it->keyframe);
	}

	pre_buffer_.clear();
}


void Output::outputJpg(void *mem, size_t size, int64_t timestamp_us, bool keyframe)
{
	// TODO should this be running on a seperate thread to prevent slowing down the stream?
	std::unique_ptr<VideoOptions> jpeg_opts_ = std::make_unique<VideoOptions>();
	
	// Copy essential settings from mjpeg_opts_
	jpeg_opts_->Set().codec = "mjpeg";
	jpeg_opts_->Set().quality = mjpeg_opts_->Get().quality;
	jpeg_opts_->Set().width = mjpeg_opts_->Get().width;
	jpeg_opts_->Set().height = mjpeg_opts_->Get().height;
	
	std::string newExtension = ".jpg";
	// Find the last occurrence of '.'
    size_t dotPos = mjpeg_opts_->Get().output.find_last_of('.');

    if (dotPos != std::string::npos) {
        // Replace the extension: keep everything before the dot and append the new extension
        jpeg_opts_->Set().output = mjpeg_opts_->Get().output.substr(0, dotPos) + newExtension;
    }
	
	LOG(1, "Creating thumbnail:" << jpeg_opts_->Get().output);
    jpeg_output.reset(Output::Create(jpeg_opts_.get()));
	jpeg_output->OutputReady(mem, size, timestamp_us, keyframe);
	jpeg_output.reset();
}

void Output::stopMjpegRecording()
{
    if (!isMjpegRecording())
        return;

    LOG(1, "Stopping MJPEG recording, StartTime: " << record_start_timestamp << " EndTime: " << record_end_timestamp);

    // Deleting or resetting the pointer will close the file in FileOutput's destructor.
    mjpeg_output.reset();

    // Make a local copy of the raw filename, then clear our member.
    // This ensures if we start a new recording soon, we won't overwrite it.
    std::string rawFile = mjpeg_opts_->Get().output;
    mjpeg_opts_->Set().output.clear();

    // 2) Construct a final container filename
    //    Example: replace ".mjpeg" with ".mp4" or ".avi"
    std::string outFilename;
    {
        std::size_t pos = rawFile.rfind(".");
        if (pos == std::string::npos)
            outFilename = rawFile + ".mp4"; // fallback
        else
            outFilename = rawFile.substr(0, pos) + ".mp4";
    }

    // 3) Build the ffmpeg command. For MJPEG->MP4, something like:
    //    ffmpeg -y -f mjpeg -i "raw.mjpeg" -c copy "output.mp4"
    //    If you prefer an AVI container (often better for MJPEG):
    //    ffmpeg -y -f mjpeg -i "raw.mjpeg" -c copy "output.avi"
    //
	std::string cmd = "ffmpeg -i \"" + rawFile + "\" -c:v libx264 -preset medium -crf 23 -pix_fmt yuv420p -c:a copy \"" + outFilename +"\"";


    // 4) Launch a background thread to run ffmpeg and remove the raw file if successful
    std::thread([cmd, rawFile, outFilename]() {
        LOG(1, "Running ffmpeg in a background thread to raw MJPEG to mp4: " << cmd);
        int ret = std::system(cmd.c_str());

        if (ret == 0)
        {
            // On success, remove the original raw .mjpeg
            LOG(1, "Successfully created " << outFilename << ", removing raw file " << rawFile);
            std::remove(rawFile.c_str());
        }
        else
        {
            // On failure, keep the raw file so you don't lose data
            LOG_ERROR("ffmpeg command failed with code " << ret 
                      << ", raw MJPEG file retained at " << rawFile);
        }
    }).detach(); // Detach the thread (no blocking, main thread continues)
}


