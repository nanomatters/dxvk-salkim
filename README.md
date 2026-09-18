# DXVK

A Vulkan-based translation layer for Direct3D 8/9/10/11 which allows running 3D applications on Linux using Wine.

For the current status of the project, please refer to the [project wiki](https://github.com/doitsujin/dxvk/wiki).

The most recent development builds can be found [here](https://github.com/doitsujin/dxvk/actions/workflows/artifacts.yml?query=branch%3Amaster).

Release builds can be found [here](https://github.com/doitsujin/dxvk/releases).

## How to use
In order to install a DXVK package obtained from the [release](https://github.com/doitsujin/dxvk/releases) page into a given wine prefix, copy or symlink the DLLs into the following directories as follows, then open `winecfg` and manually add `native` DLL overrides for `d3d8`, `d3d9`, `d3d10core`, `d3d11` and `dxgi` under the Libraries tab.

In a default Wine prefix that would be as follows:
```
export WINEPREFIX=/path/to/wineprefix
cp x64/*.dll $WINEPREFIX/drive_c/windows/system32
cp x32/*.dll $WINEPREFIX/drive_c/windows/syswow64
winecfg
```

For a pure 32-bit Wine prefix (non default) the 32-bit DLLs instead go to the `system32` directory:
```
export WINEPREFIX=/path/to/wineprefix
cp x32/*.dll $WINEPREFIX/drive_c/windows/system32
winecfg
```

Verify that your application uses DXVK instead of wined3d by enabling the HUD (see notes below).

In order to remove DXVK from a prefix, remove the DLLs and DLL overrides, and run `wineboot -u` to restore the original DLL files.

Tools such as Steam Play, Lutris, Bottles, Heroic Launcher, etc will automatically handle setup of dxvk on their own when enabled.

#### DLL dependencies 
Listed below are the DLL requirements for using DXVK with any single API.

- d3d8: `d3d8.dll` and `d3d9.dll`
- d3d9: `d3d9.dll`
- d3d10: `d3d10core.dll`, `d3d11.dll` and `dxgi.dll`
- d3d11: `d3d11.dll` and `dxgi.dll`

### Notes on Vulkan drivers
Before reporting an issue, please check the [Wiki](https://github.com/doitsujin/dxvk/wiki/Driver-support) page on the current driver status and make sure you run a recent enough driver version for your hardware.

### Online multi-player games
Manipulation of Direct3D libraries in multi-player games may be considered cheating and can get your account **banned**. This may also apply to single-player games with an embedded or dedicated multiplayer portion. **Use at your own risk.**

### HUD
The `DXVK_HUD` environment variable controls a HUD which can display the framerate and some stat counters. It accepts a comma-separated list of the following options:
- `devinfo`: Displays the name of the GPU and the driver version.
- `gpu`: Displays all available GPU information and telemetry.
- `gpu.name`: Displays the GPU device name.
- `gpu.driver`: Displays the Vulkan driver name and version.
- `gpu.power`: Displays GPU power draw and the current power limit in watts.
- `gpu.temp`: Displays GPU temperature.
- `gpu.jtemp`: Displays GPU junction (hotspot) temperature as `GPU jtemp`.
- `gpu.load`: Displays device-reported GPU utilization.
- `gpu.clock`: Displays the current graphics clock.
- `gpu.memclock`: Displays the current memory clock.
- `gpu.vram`: Displays device-wide VRAM usage.
- `gpu.memload`: Displays memory-controller utilization.
- `gpu.pcie`: Displays the current and maximum PCIe link configuration.
- `systeminfo`: Displays the CPU, Proton build, and window system.
- `cpu`: Displays all available CPU information and telemetry.
- `cpu.name`: Displays the CPU model.
- `cpu.power`: Displays CPU package power draw and, when available, the current power limit in watts.
- `cpu.temp`: Displays CPU package temperature.
- `cpu.load`: Displays aggregate CPU utilization.
- `cpu.clock`: Displays the average current and maximum CPU clock.
- `proton`: Displays the Proton build.
- `wine`: Displays the Wine version and build.
- `winsys`: Displays the window system, active HDR color space, and direct scanout status when available.
- `fps`: Shows the frame rate averaged over each 500 ms reporting interval.
- `fps_lows`: Shows 1% and 0.1% low frame rates over the preceding seven seconds.
- `fps.graph`: Shows a compact FPS history graph, also available through the D3D12 HUD.
  Uses a rolling 25 ms average, recalculated and plotted every frame.
  The graph heading uses the same 500 ms average as the FPS counter. The plotted
  samples remain independent of this slower numeric display.
  Measures application frame updates, not GPU execution time, display cadence or generated
  frames. Short per-frame fluctuations are averaged, not plotted as instantaneous FPS.
- `graph_width=x`: Sets the graph width in HUD pixels (default `280`, range `180`–`640`).
- `graph_height=y`: Sets the plot height (default `48`, range `24`–`160`, excluding its heading).
- `graph_history=s`: Sets the history length in seconds (default `3`, range `1`–`30`).
- `graph_max=n`: Fixes the vertical range to `0`–`n` in the graph's units (FPS or ms). `0` (default) uses a fine-stepped
  automatic range, with headroom on growth and hysteresis to avoid repeated shrinking/expanding.
  Values beyond a fixed range are clipped to its edge. The heading still shows the measurement.
- `reflex`: Shows all available values from the newest completed Reflex latency report.
- `reflex.id`: Shows the Reflex frame ID.
- `reflex.interval`: Shows the interval between consecutive simulation start markers.
- `reflex.input`: Shows the input sample marker.
- `reflex.sim`: Shows the simulation end marker.
- `reflex.submit`: Shows the render submission start and end markers.
- `reflex.present`: Shows the present start and end markers.
- `reflex.driver`: Shows the driver start and end markers.
- `reflex.queue`: Shows the OS render queue start and end markers.
- `reflex.gpu`: Shows the GPU render start and end markers.
- `reflex.active`: Shows GPU active rendering time, excluding idle periods.
- `reflex.frame`: Shows the interval between consecutive GPU render completions.
- `reflex.camera`: Shows the camera construction marker.
- `reflex.copy`: Shows cross adapter copy time.
- `reflex.ai`: Shows frame generation time.
- `present_latency`: Shows all available presentation pipeline timings.
- `latency.queue`: Shows Queue latency from our pre-present timestamp to the end of the presentation request's queue operations.
- `latency.display`: Shows Output latency from queue operations ending to the selected presentation endpoint.
- `latency.present`: Shows Present latency from our pre-present timestamp to the selected presentation endpoint.
- `latency.interval`: Shows Output interval between consecutive first pixel out timestamps, when supported.
- `latency.queue.graph`: Graphs Queue latency in ms.
- `latency.display.graph`: Graphs Output latency in ms.
- `latency.present.graph`: Graphs Present latency in ms.
- `latency.interval.graph`: Graphs Output interval in ms (output cadence, not input latency).
  These graphs use complete per-present timing reports without FPS-style averaging.
  Missing measurements leave gaps, not zero values. Spacing between valid reports
  is connected. A bucket containing a missing measurement stays a gap even if it
  also contains valid samples. Numeric headings refresh every 500 ms.
  D3D12 graphs require a vkd3d-proton build with the per-frame telemetry interface.
  Older builds continue to support the text readings. No additional GPU waits are introduced.
- `frametimes`: Shows a frame time graph.
- `submissions`: Shows the number of command buffers submitted per frame.
- `drawcalls`: Shows the number of draw calls and render passes per frame.
- `pipelines`: Shows the total number of graphics and compute pipelines.
- `descriptors`: Shows the number of descriptor pools and descriptor sets.
- `memory`: Shows the amount of device memory allocated and used.
- `allocations`: Shows detailed memory chunk suballocation info.
- `gpuload`: Shows legacy DXVK-estimated GPU load. May be inaccurate.
- `version`: Shows DXVK version.
- `api`: Shows the D3D feature level used by the application.
- `cs`: Shows worker thread statistics.
- `compiler`: Shows shader compiler activity
- `samplers`: Shows the current number of sampler pairs used *[D3D9 Only]*
- `swvp`: Shows the vertex processing mode and the current number of software vertex processing shaders *[D3D9 Only]*
- `scale=x`: Scales the HUD by a factor of `x` (e.g. `1.5`)
- `opacity=y`: Adjusts the HUD opacity by a factor of `y` (e.g. `0.5`, `1.0` being fully opaque).
- `background=z`: Adjusts the black text-panel opacity (default `0.45`, `0` disables it).
  Each top/bottom text block gets one padded rectangle covering all its lines.
  Panel opacity is also multiplied by `opacity`.
- `horizontal`: Places text-based HUD items in one horizontal row.
- `newline`: Starts a new row at this point in a horizontal layout.
- `center`: Centers text-based HUD items horizontally.
- `bottom`: Places text-based HUD items after this token along the bottom edge.
- `hide`: Starts the configured HUD hidden. On Wineland, press Ctrl+Shift+O
  to show or hide it. Visibility updates once per second, including while hidden.
  This option selects no HUD elements itself and is not implied by `full`.

#### Layout

Items appear in the order listed. Use `horizontal` for rows and `newline` to
start another row.

This puts device information and FPS on the first row, with low frame rates below:

```sh
DXVK_HUD=devinfo,fps,newline,fps_lows,horizontal
```

Use `bottom` between items to split the HUD between the top and bottom edges.
Items before it stay at the top. Items after it go to the bottom.
If `bottom` is the first or last item, the whole text layout goes to the bottom.

#### Graphs

Graphs use the same placement, scaling, opacity and background settings as text.
Graph options alone do not enable a graph.

A compact FPS graph with a fixed 144 FPS scale:

```sh
DXVK_HUD=fps.graph,graph_width=240,graph_height=40,graph_max=144
```

An FPS counter with its graph below:

```sh
DXVK_HUD=fps,newline,fps.graph,horizontal
```

All four presentation latency graphs:

```sh
DXVK_HUD=latency.queue.graph,latency.display.graph,latency.present.graph,latency.interval.graph
```

Timing availability depends on the driver and compositor. See
[Presentation latency measurements](#presentation-latency-measurements) for
the timestamp sources, formulas and limitations.

#### Selecting and hiding items

- `DXVK_HUD=1` is shorthand for `DXVK_HUD=devinfo,fps`.
- `DXVK_HUD=full` enables all available HUD elements, including graphs.
- Prefix an item with `-` to exclude it from a group or from `full`.
  Exclusions win regardless of their position in the configuration.

Show GPU telemetry without the device name:

```sh
DXVK_HUD=gpu,-gpu.name
```

Start with FPS and GPU telemetry hidden:

```sh
DXVK_HUD=fps,gpu,hide
```

On Wineland, press **Ctrl+Shift+O** to show or hide the HUD.
`-hide` overrides `hide` regardless of token order.

#### Telemetry notes and rendering limits

- **Junction temperature** uses a hwmon sensor labelled `junction`, such as
  AMD's hotspot sensor. The row is hidden when no valid reading is available.
  The ordinary GPU temperature is not substituted.
- **GPU telemetry** updates once per second, even when its row or the HUD is hidden.
- **Reflex markers** are relative to the simulation start timestamp.
- **D3D12 HUD capacity** is 16,384 vertices, falling back to 8,192 with older
  presenters. Extremely large layouts can be truncated at this limit.

#### Presentation latency measurements

These readings require `VK_EXT_present_timing` and support from the driver and
presentation stack. DXVK and vkd3d-proton use the same measurement boundaries.
Durations are stored in nanoseconds and displayed in milliseconds.

- `T0` is our host timestamp shortly before calling `vkQueuePresentKHR`.
  It is not the start of the game's frame or its DXGI `Present()` call.
  In vkd3d-proton it is recorded on the command queue's submission thread.
  Any queueing between the application's `Present()` call and this point is excluded.
- `T1` is the Vulkan driver's `QUEUE_OPERATIONS_END` timestamp.
  It marks completion of that presentation request's queue operations, including
  its semaphore waits and any implicitly queued implementation work.
- `T2` is the Vulkan driver's `FIRST_PIXEL_OUT` timestamp when supported.
  It marks the first pixel's data leaving the presentation engine toward display
  hardware. It does not mean that the pixel is already visible or that the whole
  frame has been scanned out.

We retrieve `T1` and `T2` through `vkGetPastPresentationTimingEXT`. The driver
supplies the timestamps of the events, not the time at which we read the report.
Receiving a report several frames later does not add to the measured latency.
Different clock domains are calibrated into a common host-clock representation
before subtraction. We do not timestamp KMS fence notifications ourselves.

Vulkan is the reporting interface, not necessarily the original measurement
source. [Mesa 26.2.2's Wayland WSI](https://gitlab.freedesktop.org/mesa/mesa/-/blob/mesa-26.2.2/src/vulkan/wsi/wsi_common_wayland.c)
uses the compositor's `wp_presentation_feedback.presented` timestamp for first
pixel out. On KWin's DRM backend this normally comes from the kernel page-flip
completion timestamp. It describes the output frame containing the game's image,
which can be a composited frame or the game buffer in direct scanout.
It is not a measurement of panel response. KWin can use a software timestamp
when the kernel timestamp is missing or during a modeset, so hardware timing is
not guaranteed for every report. Other drivers can use different timing sources.

```text
T0                         T1                         T2
Before Vulkan present      Queue operations end       First pixel out
|                          |                          |
|------ Queue latency -----|----- Output latency -----|
|------------------- Present latency -----------------|
```

| HUD label | Calculation | Configuration keyword |
| --- | --- | --- |
| Queue latency | `T1 - T0` | `latency.queue` |
| Output latency | `T2 - T1` | `latency.display` |
| Present latency | `T2 - T0` | `latency.present` |
| Output interval | `T2(current frame) - T2(previous frame)` | `latency.interval` |

Queue latency includes submission work after `T0`, dependency waits, and queued
presentation work. Rendering may already be underway at `T0`, so its remaining
wait can contribute. This is not the complete GPU rendering time or merely the
CPU duration of `vkQueuePresentKHR`. Output latency can include compositor
scheduling, composition, and waiting for display output. The boundary does not
isolate GPU work from compositor work. Neither measurement is full input-to-screen latency.

If first pixel out is unavailable, `T2` falls back to `FIRST_PIXEL_VISIBLE`, then
`REQUEST_DEQUEUED`. The former includes display processing up to first-pixel
visibility. The latter only measures up to removal from the swapchain's internal
presentation queue and does not establish that the image reached the display.
Output interval is shown only for consecutive presents with valid, increasing
first pixel out timestamps. Missing measurements are unavailable, not zero.

Wine disables presentation-timing capabilities for surfaces eligible for its
managed dmabuf presentation path, including cross-process surfaces. Managed
swapchains return no timing reports. These surfaces can therefore show `--`
even when the host driver supports the extension. This is a Wine presentation-path
limitation, not a calibration failure or missing driver support.

Output interval measures cadence, not the time spent in the pipeline. Frames
submitted at 0, 8, and 16 ms and output at 20, 28, and 36 ms each have 20 ms
Present latency but only 8 ms Output interval. Several frames can be in flight.

For the same frame, Present latency equals Queue latency plus Output latency.
Text readings refresh every 500 ms using each field's latest valid sample, not
a 500 ms average. Fields can therefore refer to different frames and may not
add up exactly on screen. The graphs use individual per-present reports.
Three decimal places in the text are formatting, not a guarantee of microsecond accuracy.

### Logs
When used with Wine, DXVK will print log messages to `stderr`. Additionally, standalone log files can optionally be generated by setting the `DXVK_LOG_PATH` variable, where log files in the given directory will be called `app_d3d11.log`, `app_dxgi.log` etc., where `app` is the name of the game executable.

On Windows, log files will be created in the game's working directory by default, which is usually next to the game executable.

### Device filter
Some applications do not provide a method to select a different GPU. In that case, DXVK can be forced to use a given device:
- `DXVK_FILTER_DEVICE_NAME="Device Name"` Selects devices with a matching Vulkan device name, which can be retrieved with tools such as `vulkaninfo`. Matches on substrings, so "VEGA" or "AMD RADV VEGA10" is supported if the full device name is "AMD RADV VEGA10 (LLVM 9.0.0)", for example. If the substring matches more than one device, the first device matched will be used.
- `DXVK_FILTER_DEVICE_UUID="00000000000000000000000000000001"` Selects a device by matching its Vulkan device UUID, which can also be retrieved using tools such as `vulkaninfo`. The UUID must be a 32-character hexadecimal string with no dashes. This method provides more precise selection, especially when using multiple identical GPUs.

**Note:** If the device filter is configured incorrectly, it may filter out all devices and applications will be unable to create a D3D device.

### Debugging
The following environment variables can be used for **debugging** purposes.
- `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` Enables Vulkan debug layers. Highly recommended for troubleshooting rendering issues and driver crashes. Requires the Vulkan SDK to be installed on the host system.
- `DXVK_LOG_LEVEL=none|error|warn|info|debug` Controls message logging.
- `DXVK_LOG_PATH=/some/directory` Changes path where log files are stored. Set to `none` to disable log file creation entirely, without disabling logging.
- `DXVK_DEBUG=...` Enables one of various debugging modes:
  - `capture`: Default when used with certain tools. Enables dxvk-internal debug names and debug markers for render passes, shaders, etc.
  - `hang`: Detects GPU hangs or driver crashes resulting in `VK_ERROR_DEVICE_LOST` and logs failing command(s).
  - `markers`: Uses `VK_EXT_debug_utils` to forward applocation-provided resource names and debug markers to Vulkan.
  - `validation`: Enables validation debug callback. Must also set `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` on Linux.
- `DXVK_CONFIG_FILE=/xxx/dxvk.conf` Sets path to the configuration file.
- `DXVK_CONFIG="dxgi.hideAmdGpu = True; dxgi.syncInterval = 0"` Can be used to set config variables through the environment instead of a configuration file using the same syntax. `;` is used as a seperator.
- `DXVK_SHADER_CACHE=0`: Disables the internal shader cache.
- `DXVK_SHADER_CACHE_PATH=/some/directory`: Path to internal shader cache files. By default, this will use `%LOCALAPPDATA%/dxvk` in a Windows
  or Wine environment, and `$HOME/.cache` or `$XDG_CACHE_HOME` in a native Linux environment.

### Graphics Pipeline Library
On drivers which support `VK_EXT_graphics_pipeline_library` Vulkan shaders will be compiled at the time the game loads its D3D shaders, rather than at draw time. This reduces or eliminates shader compile stutter in many games when compared to the previous system.

In games that load their shaders during loading screens or in the menu, this can lead to prolonged periods of very high CPU utilization, especially on weaker CPUs. For affected games it is recommended to wait for shader compilation to finish before starting the game to avoid stutter and low performance. Shader compiler activity can be monitored with `DXVK_HUD=compiler`.

**Note:** Games which only load their D3D shaders at draw time (e.g. most Unreal Engine games) will still exhibit some stutter, although it should still be less severe than without this feature.

## Build instructions

In order to pull in all submodules that are needed for building, clone the repository using the following command:
```
git clone --recursive https://github.com/doitsujin/dxvk.git
```

### Requirements:
- [wine 10.0](https://www.winehq.org/) or newer
- [Meson](https://mesonbuild.com/) build system (at least version 0.58)
- [Mingw-w64](https://www.mingw-w64.org) compiler and headers (at least version 10.0)
- [glslang](https://github.com/KhronosGroup/glslang) compiler

### Building DLLs

#### The simple way
Inside the DXVK directory, run:
```
./package-release.sh master /your/target/directory --no-package
```

This will create a folder `dxvk-master` in `/your/target/directory`, which contains both 32-bit and 64-bit versions of DXVK, which can be set up in the same way as the release versions as noted above.

In order to preserve the build directories for development, pass `--dev-build` to the script. This option implies `--no-package`. After making changes to the source code, you can then do the following to rebuild DXVK:
```
# change to build.32 for 32-bit
cd /your/target/directory/build.64
ninja install
```

#### Compiling manually
```
# 64-bit build. For 32-bit builds, replace
# build-win64.txt with build-win32.txt
meson setup --cross-file build-win64.txt --buildtype release --prefix /your/dxvk/directory build.w64
cd build.w64
ninja install
```

The D3D8, D3D9, D3D10, D3D11 and DXGI DLLs will be located in `/your/dxvk/directory/bin`.

### Build troubleshooting
DXVK requires threading support from your mingw-w64 build environment. If you
are missing this, you may see "error: ‘std::cv_status’ has not been declared"
or similar threading related errors.

On Debian and Ubuntu, this can be resolved by using the posix alternate, which
supports threading. For example, choose the posix alternate from these
commands:
```
update-alternatives --config x86_64-w64-mingw32-gcc
update-alternatives --config x86_64-w64-mingw32-g++
update-alternatives --config i686-w64-mingw32-gcc
update-alternatives --config i686-w64-mingw32-g++
```
For non debian based distros, make sure that your mingw-w64-gcc cross compiler 
does have `--enable-threads=posix` enabled during configure. If your distro does
ship its mingw-w64-gcc binary with `--enable-threads=win32` you might have to
recompile locally or open a bug at your distro's bugtracker to ask for it. 

# DXVK Native

DXVK Native is a version of DXVK which allows it to be used natively without Wine.

This is primarily useful for game and application ports to either avoid having to write another rendering backend, or to help with port bringup during development.

[Release builds](https://github.com/doitsujin/dxvk/releases) are built using the Steam Runtime.

### How does it work?

DXVK Native replaces certain Windows-isms with a platform and framework-agnostic replacement, for example, `HWND`s can become `SDL_Window*`s, etc.
All it takes to do that is to add another WSI backend.

**Note:** DXVK Native requires a backend to be explicitly set via the `DXVK_WSI_DRIVER` environment variable. The current built-in options are `SDL3`, `SDL2`, and `GLFW`.

DXVK Native comes with a slim set of Windows header definitions required for D3D9/11 and the MinGW headers for D3D9/11.
In most cases, it will end up being plug and play with your renderer, but there may be certain teething issues such as:
- `__uuidof(type)` is supported, but `__uuidof(variable)` is not supported. Use `__uuidof_var(variable)` instead.
