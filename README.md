# IMX462 kernel driver for NVIDIA Jetson

[![Code style](https://github.com/Kurokesu/imx462-jetson-driver/actions/workflows/code-style.yml/badge.svg)](https://github.com/Kurokesu/imx462-jetson-driver/actions/workflows/code-style.yml)
[![Version](https://img.shields.io/github/v/tag/Kurokesu/imx462-jetson-driver?sort=semver&filter=v*)](https://github.com/Kurokesu/imx462-jetson-driver/tags)
![JetPack 6.2.1](https://img.shields.io/badge/JetPack_6.2.1-L4T_36.4.4-brightgreen?logo=nvidia&logoColor=white)
![JetPack 6.2.2](https://img.shields.io/badge/JetPack_6.2.2-L4T_36.5.0-brightgreen?logo=nvidia&logoColor=white)

NVIDIA Jetson kernel driver for Sony IMX462, a 2 MP 1/2.8" STARVIS back-side illuminated CMOS sensor optimised for low-light and night-vision applications.

- 2-lane MIPI CSI-2
- 10-bit RAW output
- 1920×1080 @ 30 fps
- HCG (High Conversion Gain) mode for improved low-light SNR

![Kurokesu camera modules connected to a Jetson carrier board over CSI ribbon cables.](./docs/kurokesu-on-jetson.jpg)

*IMX462 camera modules are available at [kurokesu.com](https://www.kurokesu.com/item/462C-CSI)*

## Setup

Install required tools:

```bash
sudo apt install -y --no-install-recommends dkms
```

Clone this repository:

```bash
cd ~
git clone https://github.com/Kurokesu/imx462-jetson-driver.git
cd imx462-jetson-driver/
```

Run setup script:

```bash
sudo ./setup.sh
```

Setup script:

- Fetches NVIDIA device tree headers required for build
- Builds and installs kernel module via [DKMS](https://github.com/dell/dkms)
- Builds and copies device tree overlay (`.dtbo`) to `/boot`
- Installs ISP tuning to `/var/nvidia/nvcam/settings/`

Use Jetson-IO to configure CSI connector:

```bash
sudo /opt/nvidia/jetson-io/jetson-io.py
```

Navigate through the menu:

1. Configure Jetson CSI Connector (named "22pin" on 6.2.2, "24pin" on 6.2.1)
2. Configure for compatible hardware
3. Select port configuration:

    - `Camera IMX462-A` - cam0
    - `Camera IMX462-C` - cam1
    - `Camera IMX462 Dual` - cam0 + cam1

    ![Jetson-IO menu with Camera IMX462 Dual selected.](./docs/jetson-io-tool.png)

4. Save pin changes
5. Save and reboot to reconfigure pins

After reboot, verify sensor is detected:

```bash
sudo dmesg | grep imx462
```

Expected output (dual):

```
nv_imx462: module verification failed: signature and/or required key missing - tainting kernel
imx462 9-001a: tegracam sensor driver:imx462_v2.0.6
tegra-camrtc-capture-vi tegra-capture-vi: subdev imx462 9-001a bound
imx462 10-001a: tegracam sensor driver:imx462_v2.0.6
tegra-camrtc-capture-vi tegra-capture-vi: subdev imx462 10-001a bound
```

*Signature warning is expected since DKMS modules are unsigned.*

## Image output

### GStreamer

Single:

```bash
gst-launch-1.0 -e nvarguscamerasrc sensor-id=0 ! \
   'video/x-raw(memory:NVMM),width=1920,height=1080,framerate=30/1' ! \
   queue ! nvvidconv ! queue ! nveglglessink
```

Dual:

```bash
gst-launch-1.0 -e \
   nvarguscamerasrc sensor-id=0 ! \
      'video/x-raw(memory:NVMM),width=1920,height=1080,framerate=30/1' ! \
      queue ! nvvidconv ! queue ! nveglglessink \
   nvarguscamerasrc sensor-id=1 ! \
      'video/x-raw(memory:NVMM),width=1920,height=1080,framerate=30/1' ! \
      queue ! nvvidconv ! queue ! nveglglessink
```

### nvgstcapture

```bash
nvgstcapture-1.0 --sensor-id 0
```

### Raw v4l2

Install `v4l-utils`:

```bash
sudo apt install -y v4l-utils
```

Stream raw data to file:

```bash
v4l2-ctl -d /dev/video0 --set-fmt-video=width=1920,height=1080,pixelformat=RG10 --stream-mmap --stream-to imx462_1080p.raw --stream-count=1 --stream-skip=10 --verbose
```

View raw Bayer file:

```bash
python3 view_raw.py ./imx462_1080p.raw
```

## HCG mode

IMX462 supports High Conversion Gain (HCG) mode for improved signal-to-noise ratio in low-light conditions. Default is Low Conversion Gain (LCG).

Enable HCG:

```bash
echo 1 | sudo tee /sys/module/nv_imx462/parameters/hcg_mode
```

Switch back to LCG:

```bash
echo 0 | sudo tee /sys/module/nv_imx462/parameters/hcg_mode
```

## Test mode

IMX462 has a built-in test pattern generator for verifying data validity.

Enable test pattern:

```bash
# Horizontal color-bar chart example (test_mode = 2)
echo 2 | sudo tee /sys/module/nv_imx462/parameters/test_mode
```

Turn test pattern off:

```bash
echo 0 | sudo tee /sys/module/nv_imx462/parameters/test_mode
```

| Test pattern code | Description |
| ----------------- | ----------- |
| 0 | Off |
| 1 | Sequence Pattern 1 |
| 2 | Horizontal Color-bar Chart |
| 3 | Vertical Color-bar Chart |
| 4 | Sequence Pattern 2 |
| 5 | Gradation Pattern 1 |
| 6 | Gradation Pattern 2 |
| 7 | 000h/555h Toggle Pattern |

## ISP tuning

Tuning file carries ISP parameters calibrated for this sensor: black level, lens shading, white balance and color correction. Global `camera_overrides.isp` applies to every camera and would shadow it, so setup retires it to `camera_overrides.isp.bak`.

Setup installs a file per badge, `kurokesu_front_462CSI.isp` and `kurokesu_rear_462CSI.isp`, so each camera on a dual board resolves its own.

To restore default ISP parameters, remove tuning files and restart Argus:

```bash
sudo rm /var/nvidia/nvcam/settings/kurokesu_front_462CSI.isp
sudo rm /var/nvidia/nvcam/settings/kurokesu_rear_462CSI.isp
sudo systemctl restart nvargus-daemon
```

## Development builds

For manual builds without DKMS:

```bash
make              # build everything (dtbo + kernel module)
sudo make install # copy dtbo to /boot, rmmod + insmod
```

> [!NOTE]
> Module is loaded immediately via `insmod` but won't persist across reboots. Use `sudo ./setup.sh` for permanent installation via DKMS.

Individual targets:

```bash
make dtbo      # build only the device tree overlay
make module    # build only the kernel module
make clean     # remove build artifacts
```

Build artifacts are placed in `./build`.
