#pragma once

#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.UI.Core.h>

#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.UI.Core.h>

namespace UWP
{
	inline std::string GetLocalFolder()
	{
		return winrt::to_string(winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path());
	}
	
	// Ensure we have write access to a directory in UWP
	// This creates a test file in the directory to verify write permissions
	inline bool EnsureDirectoryWriteAccess(const std::string& directoryPath)
	{
		try
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
		catch (...)
		{
			return false;
		}
	}
}