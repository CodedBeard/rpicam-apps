/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (C) 2020, Raspberry Pi (Trading) Ltd.
 *
 * file_output.cpp - Write output to file.
 */

#include "file_output.hpp"
#include "core/options.hpp"

FileOutput::FileOutput(VideoOptions const *options, const std::string& filename_override)
	: Output(options), fp_(nullptr), count_(0), file_start_time_ms_(0), filename_override_(filename_override)
{
}

FileOutput::~FileOutput()
{
	closeFile();
}

void FileOutput::outputBuffer(void *mem, size_t size, int64_t timestamp_us, uint32_t flags)
{
	// We need to open a new file if we're in "segment" mode and our segment is full
	// (though we have to wait for the next I frame), or if we're in "split" mode
	// and recording is being restarted (this is necessarily an I-frame already).
	if (fp_ == nullptr ||
		(options_->Get().segment && (flags & FLAG_KEYFRAME) &&
		 timestamp_us / 1000 - file_start_time_ms_ > options_->Get().segment) ||
		(options_->Get().split && (flags & FLAG_RESTART)))
	{
		closeFile();
		openFile(timestamp_us);
	}

	LOG(2, "FileOutput: output buffer " << mem << " size " << size);
	if (fp_ && size)
	{
		if (fwrite(mem, size, 1, fp_) != 1)
			throw std::runtime_error("failed to write output bytes");
		if (options_->Get().flush)
			fflush(fp_);
	}
}

void FileOutput::openFile(int64_t timestamp_us)
{
	// Use filename_override_ if provided, otherwise use options_->Get().output
	std::string output_path = filename_override_.empty() ? options_->Get().output : filename_override_;
	
	if (output_path == "-")
		fp_ = stdout;
	else if (!output_path.empty())
	{
		// Generate the next output file name.
		char filename[256];
		int n = snprintf(filename, sizeof(filename), output_path.c_str(), count_);
		count_++;
		if (options_->Get().wrap)
			count_ = count_ % options_->Get().wrap;
		if (n < 0)
			throw std::runtime_error("failed to generate filename");

		fp_ = fopen(filename, "w");
		if (!fp_)
			throw std::runtime_error("failed to open output file " + std::string(filename));
		LOG(2, "FileOutput: opened output file " << filename);

		file_start_time_ms_ = timestamp_us / 1000;
	}
}

void FileOutput::closeFile()
{
	if (fp_)
	{
		if (options_->Get().flush)
			fflush(fp_);
		if (fp_ != stdout)
			fclose(fp_);
		fp_ = nullptr;
	}
}
