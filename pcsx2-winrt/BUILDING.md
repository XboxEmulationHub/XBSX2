# Guide to Install Visual Studio 2022 and Build XBSX2

## Step 1: Download and Install Visual Studio 2022 Community

1. Visit the [Visual Studio Downloads](https://visualstudio.microsoft.com/downloads/) page.
2. Under "Community 2022", click the `Free download` button.
3. Once the installer is downloaded, open it to start the installation process.
4. In the Visual Studio Installer, select **Visual Studio Community 2022** and click `Install`.

## Step 2: Install UWP Components

1. When the installer prompts you to select the workloads, check the **Windows application development** workload:
    - This includes the **Windows 11 SDK**, and **Windows app runtime**.

2. (Optional) You can also select other workloads depending on your needs, such as:
    - `.NET Desktop Development` for WPF/WinForms applications.
    - `Desktop development with C++` if you plan to work with C++ UWP applications.

3. Under the **Individual Components** tab (if needed), ensure the following are checked:
    - **C++ (v143) Universal Windows Platform tools**
    - **Universal Windows Platform Tools**

4. Click `Install` to begin the installation process.

You should now have Visual Studio 2022 set up for UWP development!

## Step 3: Clone the XBSX2 Repository

1. Install Git if you haven't already from [Git Downloads](https://git-scm.com/downloads).
2. Open a Command Prompt or Terminal.
3. Clone the `XBSX2` repository by running the following command:
    ```bash
    git clone https://github.com/EmulationCollective/XBSX2.git --recurse-submodules
    ```

## Step 4: Get Dependencies

1. Download the dependencies from the latest PCSX2 Windows dependencies release:  
   [PCSX2 Windows Dependencies](https://github.com/PCSX2/pcsx2-windows-dependencies/releases/tag/latest-windows-dependencies).
   
2. Extract the downloaded archive.

3. Move the `deps` folder from the extracted archive to the root of the `XBSX2` repository

4. Download `patches.zip` from [PCSX2s GitHub](https://github.com/PCSX2/pcsx2_patches/releases/tag/latest); we'll need it later.

## Step 5: Open the Solution File

1. Navigate to the root folder of the `XBSX2` repository.
2. Open the `PCSX2_qt.sln` file in **Visual Studio 2022**.

## Step 6: Set XBSX2 as the Startup Project

1. In the **Solution Explorer** window, locate the `XBSX2` project.
2. Right-click on the `XBSX2` project and select **Set as Startup Project**.

## Step 7: Change Build Configuration to Release

1. At the top of Visual Studio, find the build configuration dropdown, which might be set to `Debug` by default.
2. Change the configuration from `Debug` to `Release` to optimize the build for performance.
3. When you try to build, you **will** encounter an error such as:
```
Payload file C:\Users\Stern\Documents\XBSX2\pcsx2-winrt\resources\patches.zip’ does not exist.
```
To fix this:
1. Locate the `patches.zip` file that you downloaded earlier.
2. Move it to the following directory inside your cloned repository: `XBSX2\pcsx2-winrt\Resources`.
3. After placing the file, try building the project again.

## Step 8: Configure for Xbox Deployment

1. Right-click the **XBSX2** project in the **Solution Explorer**.
2. Select **Properties** from the context menu.
3. In the **Properties** window, go to the **Debugging** section.
4. Change the **Target Device** dropdown from `Local Machine` to `Remote Machine`.
5. In the **Machine Name** field, enter your Xbox’s IP address (found in the **Dev Home** app under `Network`). Example: `192.168.1.1`.

## Step 9: Build the Project

1. Right-click the solution in the **Solution Explorer** and select `Build Solution` or press `Ctrl+Shift+B`.
2. Visual Studio will begin building the project in `Release` mode for deployment to the Xbox.

## Step 10: Deploy and Run on Xbox

1. After building, Visual Studio will ask for a **PIN**. Enter the **Visual Studio PIN** from the **Dev Home** app on your Xbox (found in the **Remote Access** section).
2. Once the deployment is complete, the application should automatically launch on your Xbox in **Developer Mode**.


---

The XBSX2 project is now deployed to your Xbox! If any issues arise during deployment, check this repository’s [Issues](https://github.com/EmulationCollective/XBSX2/issues) page on GitHub for troubleshooting tips or reach out to the community for support.
