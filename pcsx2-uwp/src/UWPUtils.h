#pragma once

#include <winerror.h>
#include <winrt/Windows.Storage.h>

namespace UWP
{
	template <typename T>
	inline auto* GetABI(const T& object)
	{
		return reinterpret_cast<winrt::impl::abi_t<winrt::default_interface<T>>*>(winrt::get_abi(object));
	}

	template <typename T>
	inline bool IsAsyncOperationPending(const T& operation)
	{
		const auto info = operation.template try_as<winrt::Windows::Foundation::IAsyncInfo>();
		std::int32_t status;
		return info && SUCCEEDED(GetABI(info)->get_Status(&status)) &&
		       status == static_cast<std::int32_t>(winrt::Windows::Foundation::AsyncStatus::Started);
	}

	/// Get the local folder for the app
	inline std::string GetLocalFolder()
	{
		return winrt::to_string(winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path());
	}

	// Ensure we have write access to a directory in UWP
	// This creates a test file in the directory to verify write permissions
	inline bool EnsureDirectoryWriteAccess(const std::string& directoryPath)
	{
		// Create a test file path
		std::string testFilePath = directoryPath + "\\write_test.tmp";

		// Try to create and write to the file
		FILE* fp = fopen(testFilePath.c_str(), "wb");
		if (!fp)
			return false;

		// Write a single byte
		const char testByte = 1;
		bool success = (fwrite(&testByte, 1, 1, fp) == 1);
		fclose(fp);

		// Clean up the test file
		if (success)
			remove(testFilePath.c_str());

		return success;
	}
	/// Play a sound file asynchronously
	bool PlaySoundAsync(const char* path);
} // namespace UWP