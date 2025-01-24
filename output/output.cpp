/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (C) 2020, Raspberry Pi (Trading) Ltd.
 *
 * output.cpp - video stream output base class
 */

#include "core/options.hpp"
#include <cinttypes>
#include <stdexcept>
#include <curl/curl.h>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <cstdlib>    // for getenv
#include <pwd.h>      // for getpwuid
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
	if (!options->webhook_url.empty())
	{
		webhook_url = options->webhook_url;
	}

	enable_ = !options->Get().pause;
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
		record_end_timestamp = now_us + static_cast<int64_t>(options_->detection_record_secs * 1'000'000);
		startMjpegRecording(now_us);
	}
	else
	{
		// We are already recording, so just extend our end time
		if (now_us + static_cast<int64_t>(options_->detection_record_secs * 1'000'000) > record_end_timestamp)
		{
		    LOG(1, "Extending MJPEG recording due to new detection.");
		    record_end_timestamp = now_us + static_cast<int64_t>(options_->detection_record_secs * 1'000'000);
		}
	}
}

void Output::OutputReady(void *mem, size_t size, int64_t timestamp_us, bool keyframe)
{
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
		return;
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
	CURLcode res = curl_easy_perform(curl);
	bool success = (res == CURLE_OK);
	LOG(1, "Call result: " << success);
	if (!success)
	{
		LOG_ERROR("Failed to call endpoint:" << curl_easy_strerror(res));
        //std::cerr << "[Output] curl_easy_perform() failed: "
          //        << curl_easy_strerror(res) << std::endl;
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


void Output::startMjpegRecording(int64_t first_detection_timestamp)
{
    // We'll clone the existing VideoOptions but tweak them for MJPEG.
    // You might want to handle other fields, e.g. resolution, etc.
    VideoOptions mjpeg_opts = *options_;
    mjpeg_opts.codec = "mjpeg";

       // Build a filename using the configured path (defaults to "~")
// Expand the user-configured path. If it starts with '~', we resolve it.
    std::string basePath = expandTilde(mjpeg_opts.detection_record_path);
    if (basePath.empty())
        basePath = "."; // fallback if something unexpected happens

    // Generate an ISO-like filename, e.g. 2025-01-24-23-04-01-123.mjpeg
    std::string isoName = generateIsoTimestamp() + ".mjpeg";

    // Combine them: "basePath/isoName"
    std::string fullFilename = basePath + "/" + isoName;

    // Use that new path/filename in the secondary FileOutput
    mjpeg_opts.output = fullFilename;
    mjpeg_output.reset(Output::Create(&mjpeg_opts));

    LOG(1, "Started MJPEG file output at: " << fullFilename);
}

void Output::stopMjpegRecording()
{
    if (!isMjpegRecording())
        return;

    LOG(1, "Stopping MJPEG recording: " << record_start_timestamp);

    // Deleting or resetting the pointer will close the file in FileOutput’s destructor.
    mjpeg_output.reset();
}


