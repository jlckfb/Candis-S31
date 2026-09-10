# API Reference

<div align="center">




| :1234: [CAPABILITIES](#1234-capabilities) | :floppy_disk: [SD CARD AND SPIFFS](#floppy_disk-sd-card-and-spiffs) | :musical_note: [AUDIO](#musical_note-audio) | :pager: [DISPLAY AND TOUCH](#pager-display-and-touch) | :bulb: [LEDS](#bulb-leds) | :electric_plug: [USB AND TYPE-C](#electric_plug-usb-and-type-c) | :battery: [BATTERY AND POWER](#battery-battery-and-power) | :camera: [CAMERA](#camera-camera) | 
| :-------------------------: | :-------------------------: | :-------------------------: | :-------------------------: | :-------------------------: | :-------------------------: | :-------------------------: | :-------------------------: | 

</div>



## Overview

This document describes the repository-owned Candis-S31 board runtime API.
It retains the established `bsp_*` function names and the compatibility header
`bsp/esp-bsp.h`, but its implementation and release lifecycle belong to this
repository. Only APIs supported by the Candis-S31 hardware are documented.

## General

### Pinout

Each BSP defines a set of macros for default pin assignments used by its hardware peripherals.
These macros allow users to configure or reference standard interfaces like I2C, SPI, LCD, audio, or SD cards easily.

- I2C: `BSP_I2C_*`
- Display: `BSP_LCD_*`
- Audio I2S: `BSP_I2S_*`
- USB: `BSP_USB_*`
- SD Card (MMC): `BSP_SD_*`

> [!NOTE]
> The `BSP_CAPS_*` macros list the interfaces this component supports.

### I2C

Some devices included in BSPs (e.g., sensors, displays, audio codecs) communicate via the I2C interface. In many cases, I2C is initialized automatically as part of the device setup. However, you can manually initialize or deinitialize the I2C peripheral using the following API:

```
/* Initialize the default I2C bus used by the BSP */
bsp_i2c_init();

...

/* Deinitialize the I2C bus */
bsp_i2c_deinit();
```

If you need direct access to the initialized I2C bus (e.g., to communicate with an external peripheral not handled by the BSP), you can retrieve the I2C bus handle:

```
i2c_master_bus_handle_t i2c = bsp_i2c_get_handle();
```

> [!NOTE]
> The BSP ensures that I2C initialization is performed only once, even if called multiple times. This helps avoid conflicts when multiple components rely on the same I2C bus.


## Identification

Each BSP defines an identifier macro in the form of `BSP_BOARD_*`.

### Board Name API Reference



## Macros

| Type | Name |
| ---: | :--- |
| define  | [**BSP\_BOARD\_CANDIS\_S31**](#define-bsp_board_candis_s31)  <br> |
| define  | [**BSP\_BOARD\_NAME**](#define-bsp_board_name)  "Candis-S31"<br> |
| define  | [**BSP\_BOARD\_REVISION**](#define-bsp_board_revision)  "EVT1 schematic v0.5"<br> |









## :1234: Capabilities

Each BSP defines a set of capability macros that indicate which features are supported.
You can use these macros to conditionally compile code depending on feature availability.

### Capabilities API Reference



## Macros

| Type | Name |
| ---: | :--- |
| define  | [**BSP\_CAPS\_AUDIO**](#define-bsp_caps_audio)  1<br> |
| define  | [**BSP\_CAPS\_AUDIO\_MIC**](#define-bsp_caps_audio_mic)  1<br> |
| define  | [**BSP\_CAPS\_AUDIO\_SPEAKER**](#define-bsp_caps_audio_speaker)  1<br> |
| define  | [**BSP\_CAPS\_BAT**](#define-bsp_caps_bat)  1<br> |
| define  | [**BSP\_CAPS\_BUTTONS**](#define-bsp_caps_buttons)  0<br> |
| define  | [**BSP\_CAPS\_CAMERA**](#define-bsp_caps_camera)  1<br> |
| define  | [**BSP\_CAPS\_DISPLAY**](#define-bsp_caps_display)  1<br> |
| define  | [**BSP\_CAPS\_HUMITURE**](#define-bsp_caps_humiture)  0<br> |
| define  | [**BSP\_CAPS\_IMU**](#define-bsp_caps_imu)  0<br> |
| define  | [**BSP\_CAPS\_KNOB**](#define-bsp_caps_knob)  0<br> |
| define  | [**BSP\_CAPS\_LED**](#define-bsp_caps_led)  1<br> |
| define  | [**BSP\_CAPS\_SDCARD**](#define-bsp_caps_sdcard)  1<br> |
| define  | [**BSP\_CAPS\_TOUCH**](#define-bsp_caps_touch)  1<br> |











### I2C API Reference


## Functions

| Type | Name |
| ---: | :--- |
|  esp\_err\_t | [**bsp\_i2c\_deinit**](#function-bsp_i2c_deinit) (void) <br> |
|  i2c\_master\_bus\_handle\_t | [**bsp\_i2c\_get\_handle**](#function-bsp_i2c_get_handle) (void) <br> |
|  esp\_err\_t | [**bsp\_i2c\_init**](#function-bsp_i2c_init) (void) <br> |
|  esp\_err\_t | [**bsp\_lp\_i2c\_deinit**](#function-bsp_lp_i2c_deinit) (void) <br> |
|  i2c\_master\_bus\_handle\_t | [**bsp\_lp\_i2c\_get\_handle**](#function-bsp_lp_i2c_get_handle) (void) <br> |
|  esp\_err\_t | [**bsp\_lp\_i2c\_init**](#function-bsp_lp_i2c_init) (void) <br> |

## Macros

| Type | Name |
| ---: | :--- |
| define  | [**BSP\_I2C\_NUM**](#define-bsp_i2c_num)  CONFIG\_BSP\_I2C\_NUM<br> |
| define  | [**BSP\_I2C\_SCL**](#define-bsp_i2c_scl)  GPIO\_NUM\_33<br> |
| define  | [**BSP\_I2C\_SDA**](#define-bsp_i2c_sda)  GPIO\_NUM\_34<br> |
| define  | [**BSP\_LP\_I2C\_NUM**](#define-bsp_lp_i2c_num)  CONFIG\_BSP\_LP\_I2C\_NUM<br> |
| define  | [**BSP\_LP\_I2C\_SCL**](#define-bsp_lp_i2c_scl)  GPIO\_NUM\_6<br> |
| define  | [**BSP\_LP\_I2C\_SDA**](#define-bsp_lp_i2c_sda)  GPIO\_NUM\_7<br> |



## Functions Documentation

### function `bsp_i2c_deinit`

```c
esp_err_t bsp_i2c_deinit (
    void
) 
```

### function `bsp_i2c_get_handle`

```c
i2c_master_bus_handle_t bsp_i2c_get_handle (
    void
) 
```

### function `bsp_i2c_init`

```c
esp_err_t bsp_i2c_init (
    void
) 
```


Initialize the main I2C bus. Multiple calls are allowed.
### function `bsp_lp_i2c_deinit`

```c
esp_err_t bsp_lp_i2c_deinit (
    void
) 
```

### function `bsp_lp_i2c_get_handle`

```c
i2c_master_bus_handle_t bsp_lp_i2c_get_handle (
    void
) 
```

### function `bsp_lp_i2c_init`

```c
esp_err_t bsp_lp_i2c_init (
    void
) 
```


Initialize the low-power I2C bus. Multiple calls are allowed.






## :floppy_disk: SD Card and SPIFFS

### SPIFFS Initialization / Deinitialization

Each BSP provides a simple API for mounting and unmounting the SPI Flash File System (SPIFFS).

```
/* Mount SPIFFS to the virtual file system */
bsp_spiffs_mount();

/* ... perform file operations ... */

/* Unmount SPIFFS from the virtual file system */
bsp_spiffs_unmount();
```

### SD Card Initialization / Deinitialization

The BSP offers a flexible API for working with SD cards. In addition to the default mount and unmount functions, you can also use a configuration structure or access preconfigured `host` and `slot` structures.

Mount with Default Configuration

```
/* Mount microSD card to the virtual file system */
bsp_sdcard_mount();

/* ... perform file operations ... */

/* Unmount microSD card */
bsp_sdcard_unmount();
```

Mount with Custom Configuration

The BSP provides a mount function for each SD interface:
```
bsp_sdcard_cfg_t cfg = {0};
/* Mount SD card using SDMMC interface */
bsp_sdcard_sdmmc_mount(&cfg);
```

or

```
bsp_sdcard_cfg_t cfg = {0};
/* Mount SD card using SPI interface */
bsp_sdcard_sdspi_mount(&cfg)
```

> [!NOTE]
> The Candis-S31 TF card is wired for SDMMC mode. `bsp_sdcard_sdspi_mount()` returns `ESP_ERR_NOT_SUPPORTED`.

### After Mounting

Once the SD card or SPIFFS is mounted, you can use standard file I/O functions (`fopen`, `fread`, `fwrite`, `fclose`, etc.) provided by ESP-IDF's VFS (Virtual File System).

To print basic SD card information (after mounting), you can use:
```
sdmmc_card_t *sdcard = bsp_sdcard_get_handle();
sdmmc_card_print_info(stdout, sdcard);
```

> [!TIP]
> The bsp_sdcard_get_handle() function returns a pointer to the sdmmc_card_t structure, which contains detailed information about the connected SD card.

### SD Card and SPIFFS API Reference

## Structures and Types

| Type | Name |
| ---: | :--- |
| struct | [**bsp\_sdcard\_cfg\_t**](#struct-bsp_sdcard_cfg_t) <br> |

## Functions

| Type | Name |
| ---: | :--- |
|  sdmmc\_card\_t \* | [**bsp\_sdcard\_get\_handle**](#function-bsp_sdcard_get_handle) (void) <br> |
|  void | [**bsp\_sdcard\_get\_sdmmc\_host**](#function-bsp_sdcard_get_sdmmc_host) (int slot, sdmmc\_host\_t \*config) <br> |
|  bool | [**bsp\_sdcard\_is\_inserted**](#function-bsp_sdcard_is_inserted) (void) <br> |
|  esp\_err\_t | [**bsp\_sdcard\_mount**](#function-bsp_sdcard_mount) (void) <br> |
|  void | [**bsp\_sdcard\_sdmmc\_get\_slot**](#function-bsp_sdcard_sdmmc_get_slot) (int slot, sdmmc\_slot\_config\_t \*config) <br> |
|  esp\_err\_t | [**bsp\_sdcard\_sdmmc\_mount**](#function-bsp_sdcard_sdmmc_mount) ([**bsp\_sdcard\_cfg\_t**](#struct-bsp_sdcard_cfg_t) \*cfg) <br> |
|  void | [**bsp\_sdcard\_sdspi\_get\_slot**](#function-bsp_sdcard_sdspi_get_slot) (spi\_host\_device\_t spi\_host, sdspi\_device\_config\_t \*config) <br> |
|  esp\_err\_t | [**bsp\_sdcard\_sdspi\_mount**](#function-bsp_sdcard_sdspi_mount) ([**bsp\_sdcard\_cfg\_t**](#struct-bsp_sdcard_cfg_t) \*cfg) <br> |
|  esp\_err\_t | [**bsp\_sdcard\_unmount**](#function-bsp_sdcard_unmount) (void) <br> |
|  esp\_err\_t | [**bsp\_spiffs\_mount**](#function-bsp_spiffs_mount) (void) <br> |
|  esp\_err\_t | [**bsp\_spiffs\_unmount**](#function-bsp_spiffs_unmount) (void) <br> |

## Macros

| Type | Name |
| ---: | :--- |
| define  | [**BSP\_SD\_CLK**](#define-bsp_sd_clk)  GPIO\_NUM\_24<br> |
| define  | [**BSP\_SD\_CMD**](#define-bsp_sd_cmd)  GPIO\_NUM\_25<br> |
| define  | [**BSP\_SD\_D0**](#define-bsp_sd_d0)  GPIO\_NUM\_20<br> |
| define  | [**BSP\_SD\_D1**](#define-bsp_sd_d1)  GPIO\_NUM\_21<br> |
| define  | [**BSP\_SD\_D2**](#define-bsp_sd_d2)  GPIO\_NUM\_22<br> |
| define  | [**BSP\_SD\_D3**](#define-bsp_sd_d3)  GPIO\_NUM\_23<br> |
| define  | [**BSP\_SD\_DET**](#define-bsp_sd_det)  GPIO\_NUM\_0<br> |
| define  | [**BSP\_SD\_DET\_ACTIVE\_LEVEL**](#define-bsp_sd_det_active_level)  0<br> |
| define  | [**BSP\_SD\_EN**](#define-bsp_sd_en)  BSP\_SD\_POWER\_EN<br> |
| define  | [**BSP\_SD\_MOUNT\_POINT**](#define-bsp_sd_mount_point)  CONFIG\_BSP\_SD\_MOUNT\_POINT<br> |
| define  | [**BSP\_SD\_POWER\_EN**](#define-bsp_sd_power_en)  GPIO\_NUM\_36<br> |
| define  | [**BSP\_SD\_POWER\_EN\_ACTIVE\_LEVEL**](#define-bsp_sd_power_en_active_level)  0<br> |
| define  | [**BSP\_SPIFFS\_MOUNT\_POINT**](#define-bsp_spiffs_mount_point)  CONFIG\_BSP\_SPIFFS\_MOUNT\_POINT<br> |


## Structures and Types Documentation

### struct `bsp_sdcard_cfg_t`


Variables:

-  sdmmc\_host\_t \* host  

-  const esp\_vfs\_fat\_sdmmc\_mount\_config\_t \* mount  

-  const sdmmc\_slot\_config\_t \* sdmmc  

-  const sdspi\_device\_config\_t \* sdspi  

-  union [**bsp\_sdcard\_cfg\_t**](#struct-bsp_sdcard_cfg_t) slot  


## Functions Documentation

### function `bsp_sdcard_get_handle`

```c
sdmmc_card_t * bsp_sdcard_get_handle (
    void
) 
```

### function `bsp_sdcard_get_sdmmc_host`

```c
void bsp_sdcard_get_sdmmc_host (
    int slot,
    sdmmc_host_t *config
) 
```

### function `bsp_sdcard_is_inserted`

```c
bool bsp_sdcard_is_inserted (
    void
) 
```


SDMMC storage.
### function `bsp_sdcard_mount`

```c
esp_err_t bsp_sdcard_mount (
    void
) 
```

### function `bsp_sdcard_sdmmc_get_slot`

```c
void bsp_sdcard_sdmmc_get_slot (
    int slot,
    sdmmc_slot_config_t *config
) 
```

### function `bsp_sdcard_sdmmc_mount`

```c
esp_err_t bsp_sdcard_sdmmc_mount (
    bsp_sdcard_cfg_t *cfg
) 
```

### function `bsp_sdcard_sdspi_get_slot`

```c
void bsp_sdcard_sdspi_get_slot (
    spi_host_device_t spi_host,
    sdspi_device_config_t *config
) 
```

### function `bsp_sdcard_sdspi_mount`

```c
esp_err_t bsp_sdcard_sdspi_mount (
    bsp_sdcard_cfg_t *cfg
) 
```

### function `bsp_sdcard_unmount`

```c
esp_err_t bsp_sdcard_unmount (
    void
) 
```

### function `bsp_spiffs_mount`

```c
esp_err_t bsp_spiffs_mount (
    void
) 
```


SPIFFS on the "storage" partition.
### function `bsp_spiffs_unmount`

```c
esp_err_t bsp_spiffs_unmount (
    void
) 
```







## :musical_note: Audio

### Initialization

Before using speaker or microphone features, the audio codec must be initialized.

```
/* Initialize the speaker codec */
esp_codec_dev_handle_t spk_codec_dev = bsp_audio_codec_speaker_init();

/* Initialize the microphone codec */
esp_codec_dev_handle_t mic_codec_dev = bsp_audio_codec_microphone_init();
```

Open BSP-owned handles with `bsp_audio_codec_open()`; it returns `ESP_CODEC_DEV_*` codes and supports reopening after close. Use the remaining [esp_codec_dev](https://components.espressif.com/components/espressif/esp_codec_dev) APIs for volume, gain, PCM I/O and stream close.

> [!NOTE]
> Some BSPs may only support playback (speaker) or only input (microphone). Use the capability macros (`BSP_CAPS_AUDIO`, `BSP_CAPS_AUDIO_SPEAKER`, `BSP_CAPS_AUDIO_MIC`) to check supported features.

### Example of audio usage

#### Speaker

Below is an example of audio playback using the speaker (source data not included):

```
/* Set volume to 50% */
esp_codec_dev_set_out_vol(spk_codec_dev, 50);

/* Define audio format */
esp_codec_dev_sample_info_t fs = {
    .sample_rate = wav_header.sample_rate,
    .channel = wav_header.num_channels,
    .bits_per_sample = wav_header.bits_per_sample,
};
/* Open speaker stream */
bsp_audio_codec_open(spk_codec_dev, &fs);

...
/* Play audio data */
esp_codec_dev_write(spk_codec_dev, wav_bytes, wav_bytes_len);
...

/* Close stream when done */
esp_codec_dev_close(spk_codec_dev);
```

> [!TIP]
> Audio data must be in raw PCM format. Use a decoder if playing compressed formats (e.g., WAV, MP3).

#### Microphone

Below is an example of recording audio using the microphone (destination buffer not included):

```
/* Set input gain (optional) */
esp_codec_dev_set_in_gain(mic_codec_dev, 42.0);

/* Define audio format */
esp_codec_dev_sample_info_t fs = {
    .sample_rate = 16000,
    .channel = 1,
    .bits_per_sample = 16,
};
/* Open microphone stream */
bsp_audio_codec_open(mic_codec_dev, &fs);

/* Read recorded data */
esp_codec_dev_read(mic_codec_dev, recording_buffer, BUFFER_SIZE)

...

/* Close stream when done */
esp_codec_dev_close(mic_codec_dev);
```

### Audio API Reference


## Functions

| Type | Name |
| ---: | :--- |
|  esp\_err\_t | [**bsp\_audio\_codec\_deinit**](#function-bsp_audio_codec_deinit) (esp\_codec\_dev\_handle\_t device) <br> |
|  esp\_codec\_dev\_handle\_t | [**bsp\_audio\_codec\_microphone\_init**](#function-bsp_audio_codec_microphone_init) (void) <br> |
| int | [**bsp_audio_codec_open**](#function-bsp_audio_codec_open) (esp_codec_dev_handle_t device, esp_codec_dev_sample_info_t *format) |
|  esp\_codec\_dev\_handle\_t | [**bsp\_audio\_codec\_speaker\_init**](#function-bsp_audio_codec_speaker_init) (void) <br> |
|  esp\_err\_t | [**bsp\_audio\_deinit**](#function-bsp_audio_deinit) (void) <br> |
|  const audio\_codec\_data\_if\_t \* | [**bsp\_audio\_get\_codec\_itf**](#function-bsp_audio_get_codec_itf) (void) <br> |
|  esp\_err\_t | [**bsp\_audio\_init**](#function-bsp_audio_init) (const i2s\_std\_config\_t \*i2s\_config) <br> |

## Macros

| Type | Name |
| ---: | :--- |
| define  | [**BSP\_AUDIO\_MIC\_CODEC**](#define-bsp_audio_mic_codec)  ES8389<br> |
| define  | [**BSP\_AUDIO\_PA\_EN**](#define-bsp_audio_pa_en)  GPIO\_NUM\_42<br> |
| define  | [**BSP\_AUDIO\_PA\_EN\_ACTIVE\_LEVEL**](#define-bsp_audio_pa_en_active_level)  1<br> |
| define  | [**BSP\_AUDIO\_SPEAKER\_CODEC**](#define-bsp_audio_speaker_codec)  ES8389<br> |
| define  | [**BSP\_I2S\_BCLK**](#define-bsp_i2s_bclk)  GPIO\_NUM\_18<br> |
| define  | [**BSP\_I2S\_DIN**](#define-bsp_i2s_din)  GPIO\_NUM\_44<br> |
| define  | [**BSP\_I2S\_DOUT**](#define-bsp_i2s_dout)  GPIO\_NUM\_8<br> |
| define  | [**BSP\_I2S\_DSIN**](#define-bsp_i2s_dsin)  BSP\_I2S\_DIN<br> |
| define  | [**BSP\_I2S\_LCLK**](#define-bsp_i2s_lclk)  BSP\_I2S\_LRCLK<br> |
| define  | [**BSP\_I2S\_LRCLK**](#define-bsp_i2s_lrclk)  GPIO\_NUM\_19<br> |
| define  | [**BSP\_I2S\_MCLK**](#define-bsp_i2s_mclk)  GPIO\_NUM\_35<br> |
| define  | [**BSP\_I2S\_SAMPLE\_RATE**](#define-bsp_i2s_sample_rate)  16000<br> |
| define  | [**BSP\_I2S\_SCLK**](#define-bsp_i2s_sclk)  BSP\_I2S\_BCLK<br> |
| define  | [**BSP\_I2S\_SLOT\_MODE**](#define-bsp_i2s_slot_mode)  I2S\_SLOT\_MODE\_STEREO<br> |
| define  | [**BSP\_POWER\_AMP\_IO**](#define-bsp_power_amp_io)  BSP\_AUDIO\_PA\_EN<br> |



## Functions Documentation

### function `bsp_audio_codec_open`

```c
int bsp_audio_codec_open(esp_codec_dev_handle_t device,
                         esp_codec_dev_sample_info_t *format);
```

Open a BSP-owned speaker or microphone handle. Returns `ESP_CODEC_DEV_*`,
not `esp_err_t`. Restores the official codec format-change precondition
after close, including the microphone's shared TX clock, and closes a
partially opened device on failure. Serialise open, PCM I/O, close and
deinitialization for each device.


### function `bsp_audio_codec_deinit`

```c
esp_err_t bsp_audio_codec_deinit (
    esp_codec_dev_handle_t device
) 
```

Closes and destroys the requested BSP-owned codec instance. Do not use its
handle afterward. Speaker destruction leaves the PA inactive and releases
the pin reservation for the next codec instance. `bsp_audio_deinit()` also
releases the shared I2S resources and audio supply.

### function `bsp_audio_codec_microphone_init`

```c
esp_codec_dev_handle_t bsp_audio_codec_microphone_init (
    void
) 
```

### function `bsp_audio_codec_speaker_init`

```c
esp_codec_dev_handle_t bsp_audio_codec_speaker_init (
    void
) 
```

### function `bsp_audio_deinit`

```c
esp_err_t bsp_audio_deinit (
    void
) 
```

### function `bsp_audio_get_codec_itf`

```c
const audio_codec_data_if_t * bsp_audio_get_codec_itf (
    void
) 
```

### function `bsp_audio_init`

```c
esp_err_t bsp_audio_init (
    const i2s_std_config_t *i2s_config
) 
```


I2S and ES8389 audio.






## :pager: Display and Touch

### Initialization

The Candis-S31 component provides two ways to initialize the **display**, **touch** and **LVGL**.

Simple method:

```
/* Initialize display, touch, and LVGL */
lv_display_t display = bsp_display_start();
```

Configurable method:

```
bsp_display_cfg_t cfg = {
    .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),   /* See LVGL Port for more info */
    .buffer_size = BSP_LCD_V_RES * BSP_LCD_H_RES,   /* Screen buffer size in pixels */
    .double_buffer = true,                          /* Allocate two buffers if true */
    .flags = {
        .buff_dma = true,                           /* Use DMA-capable LVGL buffer */
        .buff_spiram = false,                       /* Allocate buffer in PSRAM if true */
    }
};
cfg.lvgl_port_cfg.task_stack = 10000;   /* Example: change LVGL task stack size */
/* Initialize display, touch, and LVGL */
lv_display_t display = bsp_display_start_with_config(&cfg);
```

After initialization, you can use the [LVGL](https://docs.lvgl.io/master/) API or [LVGL Port](../esp_lvgl_port/README.md) API.

### Initialization without LVGL - NoGLIB BSP

To initialize the LCD without LVGL, use:

```
esp_lcd_panel_handle_t panel_handle;
esp_lcd_panel_io_handle_t io_handle;
const bsp_display_config_t bsp_disp_cfg = {
    .max_transfer_sz = (BSP_LCD_H_RES * 100) * sizeof(uint16_t),
};
BSP_ERROR_CHECK_RETURN_NULL(bsp_display_new(&bsp_disp_cfg, &panel_handle, &io_handle));
```

To initialize the LCD touch without LVGL, use:

```
esp_lcd_touch_handle_t tp;
bsp_touch_new(NULL, &tp);
```

After initialization, you can use the [ESP-LCD](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/lcd/index.html) API and [ESP-LCD Touch](../../vendor/idf-extra-components/esp_lcd_touch_cst820/README.md) API.

### Set Brightness

```
/* Set display brightness to 100% */
bsp_display_backlight_on();

/* Set display brightness to 0% */
bsp_display_backlight_off();

/* Set display brightness to 50% */
bsp_display_brightness_set(50);
```

> [!NOTE]
> Some boards do not support changing brightness. They return an `ESP_ERR_NOT_SUPPORTED` error.

### LVGL API Usage (only when initialized with LVGL)

All LVGL calls must be protected using lock/unlock:

```
/* Wait until other tasks finish screen operations */
bsp_display_lock(0);
...
lv_obj_t * screen = lv_disp_get_scr_act(disp_handle);
lv_obj_t * obj = lv_label_create(screen);
...
/* Unlock after screen operations are done */
bsp_display_unlock();
```

### Screen rotation (only when initialized with LVGL)

```
bsp_display_lock(0);
/* Rotate display to 90 */
bsp_display_rotate(display, LV_DISPLAY_ROTATION_90);
bsp_display_unlock();
```

> [!NOTE]
> Some LCDs do not support hardware rotation and instead use software rotation, which consumes more memory.

### Available constants

Constants like screen resolution, pin configuration, and other options are defined in the BSP header files (`{bsp_name}.h`, `display.h`, `touch.h`).
Below are some of the most relevant predefined constants:

- `BSP_LCD_H_RES` - Horizontal resolution in pixels
- `BSP_LCD_V_RES` - Vertical resolution in pixels
- `BSP_LCD_SPI_NUM` - SPI bus used by the LCD (if applicable)

### Display and Touch API Reference

## Structures and Types

| Type | Name |
| ---: | :--- |
| struct | [**bsp\_display\_cfg\_t**](#struct-bsp_display_cfg_t) <br> |
| struct | [**bsp\_display\_config\_t**](#struct-bsp_display_config_t) <br> |
| struct | [**bsp\_lcd\_handles\_t**](#struct-bsp_lcd_handles_t) <br> |
| struct | [**bsp\_touch\_config\_t**](#struct-bsp_touch_config_t) <br> |

## Functions

| Type | Name |
| ---: | :--- |
|  esp\_err\_t | [**bsp\_display\_backlight\_off**](#function-bsp_display_backlight_off) (void) <br> |
|  esp\_err\_t | [**bsp\_display\_backlight\_on**](#function-bsp_display_backlight_on) (void) <br> |
|  esp\_err\_t | [**bsp\_display\_brightness\_deinit**](#function-bsp_display_brightness_deinit) (void) <br> |
|  esp\_err\_t | [**bsp\_display\_brightness\_init**](#function-bsp_display_brightness_init) (void) <br> |
|  esp\_err\_t | [**bsp\_display\_brightness\_set**](#function-bsp_display_brightness_set) (int brightness\_percent) <br> |
|  void | [**bsp\_display\_delete**](#function-bsp_display_delete) (void) <br> |
|  esp\_err\_t | [**bsp\_display\_enter\_deep\_standby**](#function-bsp_display_enter_deep_standby) (void) <br> |
|  esp\_err\_t | [**bsp\_display\_enter\_sleep**](#function-bsp_display_enter_sleep) (void) <br> |
|  esp\_err\_t | [**bsp\_display\_enter\_sleep\_panel**](#function-bsp_display_enter_sleep_panel) (void) <br> |
|  esp\_err\_t | [**bsp\_display\_exit\_deep\_standby**](#function-bsp_display_exit_deep_standby) (void) <br> |
|  esp\_err\_t | [**bsp\_display\_exit\_sleep**](#function-bsp_display_exit_sleep) (void) <br> |
|  esp\_err\_t | [**bsp\_display\_exit\_sleep\_panel**](#function-bsp_display_exit_sleep_panel) (void) <br> |
|  lv\_indev\_t \* | [**bsp\_display\_get\_input\_dev**](#function-bsp_display_get_input_dev) (void) <br> |
|  bool | [**bsp\_display\_lock**](#function-bsp_display_lock) (uint32\_t timeout\_ms) <br> |
|  esp\_err\_t | [**bsp\_display\_new**](#function-bsp_display_new) (const [**bsp\_display\_config\_t**](#struct-bsp_display_config_t) \*config, esp\_lcd\_panel\_handle\_t \*ret\_panel, esp\_lcd\_panel\_io\_handle\_t \*ret\_io) <br> |
|  esp\_err\_t | [**bsp\_display\_new\_with\_handles**](#function-bsp_display_new_with_handles) (const [**bsp\_display\_config\_t**](#struct-bsp_display_config_t) \*config, [**bsp\_lcd\_handles\_t**](#struct-bsp_lcd_handles_t) \*ret\_handles) <br> |
|  void | [**bsp\_display\_rotate**](#function-bsp_display_rotate) (lv\_display\_t \*display, lv\_display\_rotation\_t rotation) <br> |
|  lv\_display\_t \* | [**bsp\_display\_start**](#function-bsp_display_start) (void) <br> |
|  lv\_display\_t \* | [**bsp\_display\_start\_with\_config**](#function-bsp_display_start_with_config) (const [**bsp\_display\_cfg\_t**](#struct-bsp_display_cfg_t) \*cfg) <br> |
|  esp\_err\_t | [**bsp\_display\_stop**](#function-bsp_display_stop) (void) <br> |
|  void | [**bsp\_display\_unlock**](#function-bsp_display_unlock) (void) <br> |
|  esp\_err\_t | [**bsp\_touch\_delete**](#function-bsp_touch_delete) (void) <br> |
|  esp\_lcd\_touch\_handle\_t | [**bsp\_touch\_get\_handle**](#function-bsp_touch_get_handle) (void) <br> |
|  esp\_err\_t | [**bsp\_touch\_new**](#function-bsp_touch_new) (const [**bsp\_touch\_config\_t**](#struct-bsp_touch_config_t) \*config, esp\_lcd\_touch\_handle\_t \*ret\_touch) <br> |

## Macros

| Type | Name |
| ---: | :--- |
| define  | [**BSP\_LCD\_CS**](#define-bsp_lcd_cs)  GPIO\_NUM\_10<br> |
| define  | [**BSP\_LCD\_DATA0**](#define-bsp_lcd_data0)  BSP\_LCD\_QSPI\_DATA0<br> |
| define  | [**BSP\_LCD\_DATA1**](#define-bsp_lcd_data1)  BSP\_LCD\_QSPI\_DATA1<br> |
| define  | [**BSP\_LCD\_DATA2**](#define-bsp_lcd_data2)  BSP\_LCD\_QSPI\_DATA2<br> |
| define  | [**BSP\_LCD\_DATA3**](#define-bsp_lcd_data3)  BSP\_LCD\_QSPI\_DATA3<br> |
| define  | [**BSP\_LCD\_PCLK**](#define-bsp_lcd_pclk)  BSP\_LCD\_QSPI\_CLK<br> |
| define  | [**BSP\_LCD\_PIXEL\_CLOCK\_HZ**](#define-bsp_lcd_pixel_clock_hz)  (CONFIG\_BSP\_LCD\_PIXEL\_CLOCK\_MHZ \* 1000 \* 1000)<br> |
| define  | [**BSP\_LCD\_QSPI\_CLK**](#define-bsp_lcd_qspi_clk)  GPIO\_NUM\_12<br> |
| define  | [**BSP\_LCD\_QSPI\_DATA0**](#define-bsp_lcd_qspi_data0)  GPIO\_NUM\_11<br> |
| define  | [**BSP\_LCD\_QSPI\_DATA1**](#define-bsp_lcd_qspi_data1)  GPIO\_NUM\_13<br> |
| define  | [**BSP\_LCD\_QSPI\_DATA2**](#define-bsp_lcd_qspi_data2)  GPIO\_NUM\_14<br> |
| define  | [**BSP\_LCD\_QSPI\_DATA3**](#define-bsp_lcd_qspi_data3)  GPIO\_NUM\_9<br> |
| define  | [**BSP\_LCD\_RST**](#define-bsp_lcd_rst)  GPIO\_NUM\_15<br> |
| define  | [**BSP\_LCD\_SPI\_NUM**](#define-bsp_lcd_spi_num)  SPI2\_HOST<br> |
| define  | [**BSP\_LCD\_TE**](#define-bsp_lcd_te)  GPIO\_NUM\_16<br> |
| define  | [**BSP\_LCD\_TOUCH\_INT**](#define-bsp_lcd_touch_int)  BSP\_TOUCH\_INT<br> |
| define  | [**BSP\_LCD\_TOUCH\_RST**](#define-bsp_lcd_touch_rst)  BSP\_TOUCH\_RST<br> |
| define  | [**BSP\_LCD\_VBAT\_EN**](#define-bsp_lcd_vbat_en)  GPIO\_NUM\_5<br> |
| define  | [**BSP\_LCD\_VBAT\_EN\_ACTIVE\_LEVEL**](#define-bsp_lcd_vbat_en_active_level)  1<br> |
| define  | [**BSP\_LCD\_VCI\_EN**](#define-bsp_lcd_vci_en)  GPIO\_NUM\_38<br> |
| define  | [**BSP\_LCD\_VCI\_EN\_ACTIVE\_LEVEL**](#define-bsp_lcd_vci_en_active_level)  1<br> |
| define  | [**BSP\_LCD\_X\_GAP**](#define-bsp_lcd_x_gap)  10<br> |
| define  | [**BSP\_LCD\_Y\_GAP**](#define-bsp_lcd_y_gap)  0<br> |
| define  | [**BSP\_TOUCH\_INT**](#define-bsp_touch_int)  GPIO\_NUM\_3<br> |
| define  | [**BSP\_TOUCH\_RST**](#define-bsp_touch_rst)  GPIO\_NUM\_17<br> |


## Structures and Types Documentation

### struct `bsp_display_cfg_t`


Variables:

-  unsigned int buff_dma  

-  unsigned int buff_spiram  

-  uint32\_t buffer_size  

-  unsigned int direct_mode  <br>Full-frame buffers, DIRECT render, per-cycle TE-gated band flush

-  bool double_buffer  

-  struct [**bsp\_display\_cfg\_t**](#struct-bsp_display_cfg_t) flags  

-  lvgl\_port\_cfg\_t lvgl_port_cfg  

-  unsigned int sw_rotate  

### struct `bsp_display_config_t`


Variables:

-  int max_transfer_sz  

### struct `bsp_lcd_handles_t`


Variables:

-  esp\_lcd\_panel\_handle\_t control  

-  esp\_lcd\_panel\_io\_handle\_t io  

-  esp\_lcd\_panel\_handle\_t panel  

### struct `bsp_touch_config_t`


Variables:

-  bool mirror_x  

-  bool mirror_y  

-  bool swap_xy  


## Functions Documentation

### function `bsp_display_backlight_off`

```c
esp_err_t bsp_display_backlight_off (
    void
) 
```

### function `bsp_display_backlight_on`

```c
esp_err_t bsp_display_backlight_on (
    void
) 
```

### function `bsp_display_brightness_deinit`

```c
esp_err_t bsp_display_brightness_deinit (
    void
) 
```

### function `bsp_display_brightness_init`

```c
esp_err_t bsp_display_brightness_init (
    void
) 
```

### function `bsp_display_brightness_set`

```c
esp_err_t bsp_display_brightness_set (
    int brightness_percent
) 
```

### function `bsp_display_delete`

```c
void bsp_display_delete (
    void
) 
```

### function `bsp_display_enter_deep_standby`

```c
esp_err_t bsp_display_enter_deep_standby (
    void
) 
```

### function `bsp_display_enter_sleep`

```c
esp_err_t bsp_display_enter_sleep (
    void
) 
```

### function `bsp_display_enter_sleep_panel`

```c
esp_err_t bsp_display_enter_sleep_panel (
    void
) 
```


Panel-only variants: sleep the CO5300 (TE gate, backlight, SLPIN/SLPOUT) without touching the CST820. Use these for screen-off paths that must keep the touch controller responsive (its auto-standby keeps INT wake alive); the full variants above deep-sleep the touch controller and hard-reset it on exit, which a touch-wake design cannot use.
### function `bsp_display_exit_deep_standby`

```c
esp_err_t bsp_display_exit_deep_standby (
    void
) 
```

### function `bsp_display_exit_sleep`

```c
esp_err_t bsp_display_exit_sleep (
    void
) 
```

### function `bsp_display_exit_sleep_panel`

```c
esp_err_t bsp_display_exit_sleep_panel (
    void
) 
```

### function `bsp_display_get_input_dev`

```c
lv_indev_t * bsp_display_get_input_dev (
    void
) 
```

### function `bsp_display_lock`

```c
bool bsp_display_lock (
    uint32_t timeout_ms
) 
```

### function `bsp_display_new`

```c
esp_err_t bsp_display_new (
    const bsp_display_config_t *config,
    esp_lcd_panel_handle_t *ret_panel,
    esp_lcd_panel_io_handle_t *ret_io
) 
```

### function `bsp_display_new_with_handles`

```c
esp_err_t bsp_display_new_with_handles (
    const bsp_display_config_t *config,
    bsp_lcd_handles_t *ret_handles
) 
```

### function `bsp_display_rotate`

```c
void bsp_display_rotate (
    lv_display_t *display,
    lv_display_rotation_t rotation
) 
```

### function `bsp_display_start`

```c
lv_display_t * bsp_display_start (
    void
) 
```

### function `bsp_display_start_with_config`

```c
lv_display_t * bsp_display_start_with_config (
    const bsp_display_cfg_t *cfg
) 
```

### function `bsp_display_stop`

```c
esp_err_t bsp_display_stop (
    void
) 
```


Terminal LVGL teardown: even if an LVGL remove operation fails, all BSP handles are invalidated and panel/touch pins and rails are made safe. A failed return therefore requires a reboot, not a retry with old handles.
### function `bsp_display_unlock`

```c
void bsp_display_unlock (
    void
) 
```

### function `bsp_touch_delete`

```c
esp_err_t bsp_touch_delete (
    void
) 
```

### function `bsp_touch_get_handle`

```c
esp_lcd_touch_handle_t bsp_touch_get_handle (
    void
) 
```

### function `bsp_touch_new`

```c
esp_err_t bsp_touch_new (
    const bsp_touch_config_t *config,
    esp_lcd_touch_handle_t *ret_touch
) 
```







## :bulb: LEDs

LEDs are handled similarly to buttons in BSP. The BSP uses the [led_indicator](https://components.espressif.com/components/espressif/led_indicator) component, which provides simple control over LED states and built-in effects such as blinking, breathing, and more. It also supports addressable RGB LEDs.

```
/* Initialize all LEDs */
bsp_led_indicator_create(leds, NULL, BSP_LED_NUM);

/* Set color of the first LED (for addressable RGB LEDs only) */
led_indicator_set_rgb(leds[0], SET_IRGB(0, 0x00, 0x64, 0x64));

/*
Start a predefined LED effect:
- BSP_LED_ON
- BSP_LED_OFF
- BSP_LED_BLINK_FAST
- BSP_LED_BLINK_SLOW
- BSP_LED_BREATHE_FAST
- BSP_LED_BREATHE_SLOW
*/
led_indicator_start(leds[0], BSP_LED_BREATHE_SLOW);
```

**Notes:**
- `BSP_LED_NUM` defines the total number of available LEDs on the board
- LEDs are automatically configured by the BSP (no need to set GPIO or direction manually)

### LEDs API Reference

## Structures and Types

| Type | Name |
| ---: | :--- |
| enum  | [**bsp\_led\_effect\_t**](#enum-bsp_led_effect_t)  <br> |
| enum  | [**bsp\_led\_t**](#enum-bsp_led_t)  <br> |

## Functions

| Type | Name |
| ---: | :--- |
|  esp\_err\_t | [**bsp\_led\_indicator\_create**](#function-bsp_led_indicator_create) (led\_indicator\_handle\_t led\_array, int \*led\_cnt, int led\_array\_size) <br> |
|  esp\_err\_t | [**bsp\_led\_set**](#function-bsp_led_set) (led\_indicator\_handle\_t handle, bool on) <br> |

## Macros

| Type | Name |
| ---: | :--- |
| define  | [**BSP\_LEDS\_NUM**](#define-bsp_leds_num)  1<br> |
| define  | [**BSP\_LED\_RGB\_IO**](#define-bsp_led_rgb_io)  GPIO\_NUM\_4<br> |


## Structures and Types Documentation

### enum `bsp_led_effect_t`

```c
enum bsp_led_effect_t {
    BSP_LED_ON = 0,
    BSP_LED_OFF,
    BSP_LED_BLINK_FAST,
    BSP_LED_BLINK_SLOW,
    BSP_LED_BREATHE_FAST,
    BSP_LED_BREATHE_SLOW,
    BSP_LED_MAX
};
```

### enum `bsp_led_t`

```c
enum bsp_led_t {
    BSP_LED_1 = 0,
    BSP_LED_NUM
};
```


## Functions Documentation

### function `bsp_led_indicator_create`

```c
esp_err_t bsp_led_indicator_create (
    led_indicator_handle_t led_array,
    int *led_cnt,
    int led_array_size
) 
```


Addressable RGB LED.
### function `bsp_led_set`

```c
esp_err_t bsp_led_set (
    led_indicator_handle_t handle,
    bool on
) 
```







## :electric_plug: USB

The component provides USB Host and FUSB303B Type-C control for the Type-C2 connector.

```
/* Install the USB Host library for the Type-C2 connector */
bsp_usb_host_start(BSP_USB_HOST_POWER_MODE_USB_DEV, true);

...
/* Deinitialize and stop USB */
bsp_usb_host_stop();
```

The USB and Type-C functions are declared in `bsp/candis_s31.h`.

### USB and Type-C API Reference

## Structures and Types

| Type | Name |
| ---: | :--- |
| enum  | [**bsp\_type\_c\_current\_t**](#enum-bsp_type_c_current_t)  <br> |
| enum  | [**bsp\_type\_c\_role\_t**](#enum-bsp_type_c_role_t)  <br> |
| struct | [**bsp\_type\_c\_status\_t**](#struct-bsp_type_c_status_t) <br> |
| enum  | [**bsp\_usb\_host\_power\_mode\_t**](#enum-bsp_usb_host_power_mode_t)  <br> |

## Functions

| Type | Name |
| ---: | :--- |
|  esp\_err\_t | [**bsp\_type\_c\_deinit**](#function-bsp_type_c_deinit) (void) <br> |
|  esp\_err\_t | [**bsp\_type\_c\_get\_status**](#function-bsp_type_c_get_status) ([**bsp\_type\_c\_status\_t**](#struct-bsp_type_c_status_t) \*status, bool clear\_interrupts) <br> |
|  esp\_err\_t | [**bsp\_type\_c\_init**](#function-bsp_type_c_init) (void) <br> |
|  esp\_err\_t | [**bsp\_type\_c\_set\_role**](#function-bsp_type_c_set_role) (bsp\_type\_c\_role\_t role, [**bsp\_type\_c\_current\_t**](#enum-bsp_type_c_current_t) current) <br> |
|  esp\_err\_t | [**bsp\_usb\_host\_start**](#function-bsp_usb_host_start) ([**bsp\_usb\_host\_power\_mode\_t**](#enum-bsp_usb_host_power_mode_t) mode, bool limit\_500mA) <br> |
|  esp\_err\_t | [**bsp\_usb\_host\_stop**](#function-bsp_usb_host_stop) (void) <br> |
|  esp\_err\_t | [**bsp\_usb\_otg\_power\_set**](#function-bsp_usb_otg_power_set) (bool enable, [**bsp\_type\_c\_current\_t**](#enum-bsp_type_c_current_t) current) <br> |



## Structures and Types Documentation

### enum `bsp_type_c_current_t`

```c
enum bsp_type_c_current_t {
    BSP_TYPE_C_CURRENT_DEFAULT = 0,
    BSP_TYPE_C_CURRENT_1_5_A,
    BSP_TYPE_C_CURRENT_3_0_A,
    BSP_TYPE_C_CURRENT_NONE
};
```


Board source policy accepts only USB Type-C default current (500 mA). Status reports preserve the partner advertisement, including NONE for Ra or no attachment; passing any non-default value to a board role or VBUS API fails with ESP\_ERR\_NOT\_SUPPORTED.
### enum `bsp_type_c_role_t`

```c
enum bsp_type_c_role_t {
    BSP_TYPE_C_ROLE_DISABLED = 0,
    BSP_TYPE_C_ROLE_SINK,
    BSP_TYPE_C_ROLE_SOURCE,
    BSP_TYPE_C_ROLE_DRP
};
```

### struct `bsp_type_c_status_t`


FUSB303B connection snapshot.

Variables:

-  [**bsp\_type\_c\_current\_t**](#enum-bsp_type_c_current_t) advertised_current  <br>Current advertised by the attached partner.

-  bool attached  

-  uint8\_t device_id  

-  uint8\_t device_type  

-  bool fault  

-  uint8\_t i2c_address  

-  uint8\_t interrupt  

-  uint8\_t interrupt1  

-  uint8\_t orientation  

-  bool remedy_active  

-  bsp\_type\_c\_role\_t role  

-  uint8\_t status  

-  uint8\_t status1  

-  uint8\_t type  

-  bool vbus_ok  

-  bool vbus_safe_0v  

### enum `bsp_usb_host_power_mode_t`

```c
enum bsp_usb_host_power_mode_t {
    BSP_USB_HOST_POWER_MODE_USB_DEV = 0
};
```


Power source selection kept compatible with the common ESP-BSP USB API.

## Functions Documentation

### function `bsp_type_c_deinit`

```c
esp_err_t bsp_type_c_deinit (
    void
) 
```

### function `bsp_type_c_get_status`

```c
esp_err_t bsp_type_c_get_status (
    bsp_type_c_status_t *status,
    bool clear_interrupts
) 
```

### function `bsp_type_c_init`

```c
esp_err_t bsp_type_c_init (
    void
) 
```


FUSB303B access. Every explicit role selection first clears the 5 V boost latch and never re-enables it; bsp\_usb\_otg\_power\_set(true, ...) performs the only supported Source-before-boost sequence. Native USB Host owns the role and boost lifecycle while running, so direct role/power/deinit calls then fail with ESP\_ERR\_INVALID\_STATE. Type-C2 only advertises the USB 500 mA default: other current requests fail with ESP\_ERR\_NOT\_SUPPORTED.
### function `bsp_type_c_set_role`

```c
esp_err_t bsp_type_c_set_role (
    bsp_type_c_role_t role,
    bsp_type_c_current_t current
) 
```

### function `bsp_usb_host_start`

```c
esp_err_t bsp_usb_host_start (
    bsp_usb_host_power_mode_t mode,
    bool limit_500mA
) 
```


Install or remove the native USB Host library for the Type-C2 connector. limit\_500mA must be true (the only source current this board advertises); passing false fails with ESP\_ERR\_NOT\_SUPPORTED.
### function `bsp_usb_host_stop`

```c
esp_err_t bsp_usb_host_stop (
    void
) 
```

### function `bsp_usb_otg_power_set`

```c
esp_err_t bsp_usb_otg_power_set (
    bool enable,
    bsp_type_c_current_t current
) 
```







## :battery: Battery

Some boards with battery support can measure the battery voltage using an ADC channel. BSP provides a simple API for this:

```
/* Initialize the battery voltage measurement */
bsp_voltage_init();

/* Read battery voltage in millivolts */
int voltage = bsp_voltage_battery_get();
```

### Battery and Power API Reference

## Structures and Types

| Type | Name |
| ---: | :--- |
| enum  | [**bsp\_peripheral\_t**](#enum-bsp_peripheral_t)  <br> |
| enum  | [**bsp\_pmic\_adc\_channel\_t**](#enum-bsp_pmic_adc_channel_t)  <br> |
| struct | [**bsp\_pmic\_adc\_diagnostic\_sample\_t**](#struct-bsp_pmic_adc_diagnostic_sample_t) <br> |
| struct | [**bsp\_pmic\_adc\_diagnostic\_t**](#struct-bsp_pmic_adc_diagnostic_t) <br> |
| struct | [**bsp\_pmic\_early\_snapshot\_t**](#struct-bsp_pmic_early_snapshot_t) <br> |
| enum  | [**bsp\_pmic\_early\_snapshot\_valid\_t**](#enum-bsp_pmic_early_snapshot_valid_t)  <br> |
| enum  | [**bsp\_pmic\_regulator\_t**](#enum-bsp_pmic_regulator_t)  <br> |
| struct | [**bsp\_pmic\_status\_t**](#struct-bsp_pmic_status_t) <br> |
| enum  | [**bsp\_pmic\_switch\_t**](#enum-bsp_pmic_switch_t)  <br> |
| enum  | [**bsp\_power\_domain\_t**](#enum-bsp_power_domain_t)  <br> |
| typedef esp\_err\_t(\* | [**bsp\_power\_safe\_shutdown\_cb\_t**](#typedef-bsp_power_safe_shutdown_cb_t)  <br> |

## Functions

| Type | Name |
| ---: | :--- |
|  const char \* | [**bsp\_peripheral\_name**](#function-bsp_peripheral_name) ([**bsp\_peripheral\_t**](#enum-bsp_peripheral_t) peripheral) <br> |
|  esp\_err\_t | [**bsp\_peripheral\_power\_set**](#function-bsp_peripheral_power_set) ([**bsp\_peripheral\_t**](#enum-bsp_peripheral_t) peripheral, bool enable) <br> |
|  esp\_err\_t | [**bsp\_pmic\_deinit**](#function-bsp_pmic_deinit) (void) <br> |
|  esp\_err\_t | [**bsp\_pmic\_get\_and\_clear\_interrupts**](#function-bsp_pmic_get_and_clear_interrupts) (uint8\_t status) <br> |
|  esp\_err\_t | [**bsp\_pmic\_get\_boot\_irq\_snapshot**](#function-bsp_pmic_get_boot_irq_snapshot) (uint8\_t status, bool \*valid) <br> |
|  esp\_err\_t | [**bsp\_pmic\_get\_charge\_current**](#function-bsp_pmic_get_charge_current) (uint16\_t \*milliamps) <br> |
|  esp\_err\_t | [**bsp\_pmic\_get\_charge\_voltage**](#function-bsp_pmic_get_charge_voltage) (uint16\_t \*millivolts) <br> |
|  esp\_err\_t | [**bsp\_pmic\_get\_early\_snapshot**](#function-bsp_pmic_get_early_snapshot) ([**bsp\_pmic\_early\_snapshot\_t**](#struct-bsp_pmic_early_snapshot_t) \*snapshot) <br> |
|  esp\_err\_t | [**bsp\_pmic\_get\_input\_current\_limit**](#function-bsp_pmic_get_input_current_limit) (uint16\_t \*milliamps) <br> |
|  esp\_err\_t | [**bsp\_pmic\_get\_power\_off\_source**](#function-bsp_pmic_get_power_off_source) (uint8\_t \*source) <br> |
|  esp\_err\_t | [**bsp\_pmic\_get\_power\_on\_source**](#function-bsp_pmic_get_power_on_source) (uint8\_t \*source) <br> |
|  esp\_err\_t | [**bsp\_pmic\_get\_precharge\_current**](#function-bsp_pmic_get_precharge_current) (uint16\_t \*milliamps) <br> |
|  esp\_err\_t | [**bsp\_pmic\_get\_status**](#function-bsp_pmic_get_status) ([**bsp\_pmic\_status\_t**](#struct-bsp_pmic_status_t) \*status) <br> |
|  esp\_err\_t | [**bsp\_pmic\_get\_termination\_current**](#function-bsp_pmic_get_termination_current) (uint16\_t \*milliamps, bool \*enabled) <br> |
|  esp\_err\_t | [**bsp\_pmic\_get\_vindpm**](#function-bsp_pmic_get_vindpm) (uint16\_t \*millivolts) <br> |
|  esp\_err\_t | [**bsp\_pmic\_init**](#function-bsp_pmic_init) (void) <br> |
|  esp\_err\_t | [**bsp\_pmic\_power\_off**](#function-bsp_pmic_power_off) (void) <br> |
|  esp\_err\_t | [**bsp\_pmic\_program\_battery\_model**](#function-bsp_pmic_program_battery_model) (const uint8\_t \*model, size\_t size) <br> |
|  esp\_err\_t | [**bsp\_pmic\_read\_adc\_mv**](#function-bsp_pmic_read_adc_mv) ([**bsp\_pmic\_adc\_channel\_t**](#enum-bsp_pmic_adc_channel_t) channel, uint16\_t \*millivolts) <br> |
|  esp\_err\_t | [**bsp\_pmic\_read\_battery\_model**](#function-bsp_pmic_read_battery_model) (bool from\_sram, uint8\_t \*model, size\_t size) <br> |
|  esp\_err\_t | [**bsp\_pmic\_read\_registers**](#function-bsp_pmic_read_registers) (uint8\_t register\_address, uint8\_t \*values, size\_t count) <br> |
|  esp\_err\_t | [**bsp\_pmic\_regulator\_enable**](#function-bsp_pmic_regulator_enable) ([**bsp\_pmic\_regulator\_t**](#enum-bsp_pmic_regulator_t) regulator, bool enable) <br> |
|  esp\_err\_t | [**bsp\_pmic\_regulator\_get\_voltage**](#function-bsp_pmic_regulator_get_voltage) ([**bsp\_pmic\_regulator\_t**](#enum-bsp_pmic_regulator_t) regulator, uint16\_t \*millivolts) <br> |
|  esp\_err\_t | [**bsp\_pmic\_regulator\_is\_enabled**](#function-bsp_pmic_regulator_is_enabled) ([**bsp\_pmic\_regulator\_t**](#enum-bsp_pmic_regulator_t) regulator, bool \*enabled) <br> |
|  const char \* | [**bsp\_pmic\_regulator\_name**](#function-bsp_pmic_regulator_name) ([**bsp\_pmic\_regulator\_t**](#enum-bsp_pmic_regulator_t) regulator) <br> |
|  esp\_err\_t | [**bsp\_pmic\_regulator\_set\_voltage**](#function-bsp_pmic_regulator_set_voltage) ([**bsp\_pmic\_regulator\_t**](#enum-bsp_pmic_regulator_t) regulator, uint16\_t millivolts) <br> |
|  esp\_err\_t | [**bsp\_pmic\_run\_adc\_diagnostic**](#function-bsp_pmic_run_adc_diagnostic) ([**bsp\_pmic\_adc\_diagnostic\_t**](#struct-bsp_pmic_adc_diagnostic_t) \*diagnostic) <br> |
|  esp\_err\_t | [**bsp\_pmic\_set\_charge\_current**](#function-bsp_pmic_set_charge_current) (uint16\_t milliamps) <br> |
|  esp\_err\_t | [**bsp\_pmic\_set\_charge\_voltage**](#function-bsp_pmic_set_charge_voltage) (uint16\_t millivolts) <br> |
|  esp\_err\_t | [**bsp\_pmic\_set\_input\_current\_limit**](#function-bsp_pmic_set_input_current_limit) (uint16\_t milliamps) <br> |
|  esp\_err\_t | [**bsp\_pmic\_set\_precharge\_current**](#function-bsp_pmic_set_precharge_current) (uint16\_t milliamps) <br> |
|  esp\_err\_t | [**bsp\_pmic\_set\_termination\_current**](#function-bsp_pmic_set_termination_current) (uint16\_t milliamps, bool enable) <br> |
|  esp\_err\_t | [**bsp\_pmic\_set\_vindpm**](#function-bsp_pmic_set_vindpm) (uint16\_t millivolts) <br> |
|  esp\_err\_t | [**bsp\_pmic\_switch\_enable**](#function-bsp_pmic_switch_enable) ([**bsp\_pmic\_switch\_t**](#enum-bsp_pmic_switch_t) sw, bool enable) <br> |
|  esp\_err\_t | [**bsp\_pmic\_switch\_is\_enabled**](#function-bsp_pmic_switch_is_enabled) ([**bsp\_pmic\_switch\_t**](#enum-bsp_pmic_switch_t) sw, bool \*enabled) <br> |
|  const char \* | [**bsp\_pmic\_switch\_name**](#function-bsp_pmic_switch_name) ([**bsp\_pmic\_switch\_t**](#enum-bsp_pmic_switch_t) sw) <br> |
|  esp\_err\_t | [**bsp\_power\_domain\_get**](#function-bsp_power_domain_get) ([**bsp\_power\_domain\_t**](#enum-bsp_power_domain_t) domain, bool \*enabled) <br> |
|  const char \* | [**bsp\_power\_domain\_name**](#function-bsp_power_domain_name) ([**bsp\_power\_domain\_t**](#enum-bsp_power_domain_t) domain) <br> |
|  esp\_err\_t | [**bsp\_power\_domain\_set**](#function-bsp_power_domain_set) ([**bsp\_power\_domain\_t**](#enum-bsp_power_domain_t) domain, bool enable) <br> |
|  esp\_err\_t | [**bsp\_power\_safe\_state**](#function-bsp_power_safe_state) (void) <br> |
|  esp\_err\_t | [**bsp\_power\_set\_safe\_shutdown\_callback**](#function-bsp_power_set_safe_shutdown_callback) ([**bsp\_power\_safe\_shutdown\_cb\_t**](#typedef-bsp_power_safe_shutdown_cb_t) callback, void \*user\_ctx) <br> |

## Macros

| Type | Name |
| ---: | :--- |
| define  | [**BSP\_PMIC\_ADC\_DIAGNOSTIC\_SAMPLE\_COUNT**](#define-bsp_pmic_adc_diagnostic_sample_count)  7<br> |
| define  | [**BSP\_PMIC\_RTC\_INT**](#define-bsp_pmic_rtc_int)  GPIO\_NUM\_2<br> |
| define  | [**BSP\_PMIC\_RTC\_INT\_ACTIVE\_LEVEL**](#define-bsp_pmic_rtc_int_active_level)  0<br> |
| define  | [**BSP\_PMIC\_SAFE\_CHARGE\_CURRENT\_MA**](#define-bsp_pmic_safe_charge_current_ma)  50<br> |
| define  | [**BSP\_PMIC\_SAFE\_CHARGE\_VOLTAGE\_MV**](#define-bsp_pmic_safe_charge_voltage_mv)  4200<br> |
| define  | [**BSP\_PMIC\_SAFE\_INPUT\_CURRENT\_LIMIT\_MA**](#define-bsp_pmic_safe_input_current_limit_ma)  2000<br> |
| define  | [**BSP\_PMIC\_SAFE\_PRECHARGE\_CURRENT\_MA**](#define-bsp_pmic_safe_precharge_current_ma)  50<br> |
| define  | [**BSP\_PMIC\_SAFE\_TERMINATION\_CURRENT\_MA**](#define-bsp_pmic_safe_termination_current_ma)  25<br> |
| define  | [**BSP\_TYPE\_C\_CTRL\_EN**](#define-bsp_type_c_ctrl_en)  GPIO\_NUM\_1<br> |
| define  | [**BSP\_TYPE\_C\_CTRL\_EN\_ACTIVE\_LEVEL**](#define-bsp_type_c_ctrl_en_active_level)  0<br> |
| define  | [**BSP\_TYPE\_C\_INT**](#define-bsp_type_c_int)  GPIO\_NUM\_43<br> |
| define  | [**BSP\_TYPE\_C\_INT\_ACTIVE\_LEVEL**](#define-bsp_type_c_int_active_level)  0<br> |
| define  | [**BSP\_USB\_OTG\_EN**](#define-bsp_usb_otg_en)  GPIO\_NUM\_45<br> |
| define  | [**BSP\_USB\_OTG\_EN\_ACTIVE\_LEVEL**](#define-bsp_usb_otg_en_active_level)  1<br> |


## Structures and Types Documentation

### enum `bsp_peripheral_t`

```c
enum bsp_peripheral_t {
    BSP_PERIPHERAL_DISPLAY = 0,
    BSP_PERIPHERAL_TOUCH,
    BSP_PERIPHERAL_AUDIO,
    BSP_PERIPHERAL_CAMERA,
    BSP_PERIPHERAL_SDCARD,
    BSP_PERIPHERAL_EXTERNAL_3V3,
    BSP_PERIPHERAL_COUNT
};
```


Board peripherals with a complete power sequence.
### enum `bsp_pmic_adc_channel_t`

```c
enum bsp_pmic_adc_channel_t {
    BSP_PMIC_ADC_VBAT = 0,
    BSP_PMIC_ADC_TS,
    BSP_PMIC_ADC_VBUS,
    BSP_PMIC_ADC_VSYS,
    BSP_PMIC_ADC_TDIE,
    BSP_PMIC_ADC_COUNT
};
```


Channels of the TG28\_SW SAR ADC, mirroring the driver channel list.
### struct `bsp_pmic_adc_diagnostic_sample_t`


One timestamped raw ADC acquisition. Raw values retain all 14 register bits so negative-offset/overflow signatures near 0x3fff are observable.

Variables:

-  uint32\_t elapsed_ms  

-  uint16\_t raw  

### struct `bsp_pmic_adc_diagnostic_t`


Controlled multi-channel ADC diagnostic. The BSP enables VBAT, VBUS, VSYS, and TDIE together, samples them for two seconds, then restores the caller's exact REG30 value even if a sample fails.

Variables:

-  bool enable_verified  

-  uint8\_t reg30_enabled  

-  uint8\_t reg30_original  

-  uint8\_t reg30_restored  

-  bool restore_verified  

-  size\_t sample_count  

-  [**bsp\_pmic\_adc\_diagnostic\_sample\_t**](#struct-bsp_pmic_adc_diagnostic_sample_t) samples  

### struct `bsp_pmic_early_snapshot_t`


First TG28 register values captured immediately after tg28\_sw\_create(). This happens before the BSP writes the charge profile, programs or resets the fuel gauge, clears IRQs, or changes TS configuration. The snapshot is retained until the ESP resets; PMIC deinit/reinit never overwrites it.

Variables:

-  uint8\_t adc_control  <br>REG30: ADC channel enables

-  uint8\_t irq_status  <br>REG48-4A: latched IRQ status

-  uint8\_t power_source  <br>REG20-21: power-on/off sources

-  uint8\_t status  <br>REG00-01: PMU status

-  uint8\_t valid_mask  <br>OR of [**bsp\_pmic\_early\_snapshot\_valid\_t**](#enum-bsp_pmic_early_snapshot_valid_t)

### enum `bsp_pmic_early_snapshot_valid_t`

```c
enum bsp_pmic_early_snapshot_valid_t {
    BSP_PMIC_EARLY_STATUS_VALID = 1U << 0,
    BSP_PMIC_EARLY_POWER_SOURCE_VALID = 1U << 1,
    BSP_PMIC_EARLY_ADC_CONTROL_VALID = 1U << 2,
    BSP_PMIC_EARLY_IRQ_STATUS_VALID = 1U << 3
};
```


Valid fields in [**bsp\_pmic\_early\_snapshot\_t**](#struct-bsp_pmic_early_snapshot_t). Each register group is read independently so a transient I2C failure does not discard the other power-loss evidence.
### enum `bsp_pmic_regulator_t`

```c
enum bsp_pmic_regulator_t {
    BSP_PMIC_DCDC1 = 0,
    BSP_PMIC_DCDC2,
    BSP_PMIC_DCDC3,
    BSP_PMIC_DCDC4,
    BSP_PMIC_ALDO1,
    BSP_PMIC_ALDO2,
    BSP_PMIC_ALDO3,
    BSP_PMIC_ALDO4,
    BSP_PMIC_BLDO1,
    BSP_PMIC_BLDO2,
    BSP_PMIC_CPUSLDO,
    BSP_PMIC_DLDO1,
    BSP_PMIC_DLDO2,
    BSP_PMIC_REGULATOR_COUNT
};
```


TG28\_SW rails used by the board. Must stay aligned with tg28\_sw\_regulator\_t.
### struct `bsp_pmic_status_t`


Read-only TG28\_SW power and battery snapshot.

Variables:

-  uint16\_t battery_mv  

-  uint8\_t battery_percent  <br>TG28 SOC estimate; only meaningful when fuel\_gauge\_valid is true (the factory ROM model was verified or a custom model was programmed). Treat it as meaningless otherwise; never derive a percentage from battery\_mv here.

-  bool battery_present  

-  bool charge_done  

-  bool charging  

-  uint8\_t chip_id  

-  uint8\_t common_status0  

-  uint8\_t common_status1  

-  bool fuel_gauge_reference_model  <br>True while the active valid model is the TG28 factory ROM model. False means a custom model is active or no model is valid.

-  bool fuel_gauge_valid  <br>True after the factory ROM model is verified or a custom battery model is successfully programmed in this boot.

-  bool vbus_present  

### enum `bsp_pmic_switch_t`

```c
enum bsp_pmic_switch_t {
    BSP_PMIC_SWITCH_DC1SW = 0,
    BSP_PMIC_SWITCH_DC4SW,
    BSP_PMIC_SWITCH_COUNT
};
```


TG28\_SW load-switch outputs, mirroring tg28\_sw\_power\_switch\_t.

A switch passes its input rail straight through, so no voltage programming applies. On Candis-S31 the TG28 OTP (TG28 confirmation sheet V1.3) straps the DLDO1 pin as DC1SW: its input is DCDC1 (3.3 V) and it feeds the WS2812B RGB LED. The rail is OFF after power-on and software must open it explicitly. DC4SW (DLDO2 pin) is an unconnected spare.
### enum `bsp_power_domain_t`

```c
enum bsp_power_domain_t {
    BSP_POWER_TYPE_C_CONTROL = 0,
    BSP_POWER_DISPLAY_VBAT,
    BSP_POWER_SDCARD,
    BSP_POWER_DISPLAY_VCI,
    BSP_POWER_AUDIO_PA,
    BSP_POWER_USB_OTG,
    BSP_POWER_DOMAIN_COUNT
};
```


Directly controlled board power domains.
### typedef `bsp_power_safe_shutdown_cb_t`

```c
typedef esp_err_t(* bsp_power_safe_shutdown_cb_t) (void *user_ctx);
```


Application-owned protocol teardown invoked by [**bsp\_power\_safe\_state()**](#function-bsp_power_safe_state) before any GPIO is parked or rail is removed. The callback must release active display/audio/camera/storage owners while their supplies are still present, must not call[**bsp\_power\_safe\_state()**](#function-bsp_power_safe_state) recursively, and should return its first teardown error after attempting all owned peripherals.

## Functions Documentation

### function `bsp_peripheral_name`

```c
const char * bsp_peripheral_name (
    bsp_peripheral_t peripheral
) 
```

### function `bsp_peripheral_power_set`

```c
esp_err_t bsp_peripheral_power_set (
    bsp_peripheral_t peripheral,
    bool enable
) 
```


Apply the board-defined sequence for one complete peripheral supply.
### function `bsp_pmic_deinit`

```c
esp_err_t bsp_pmic_deinit (
    void
) 
```

### function `bsp_pmic_get_and_clear_interrupts`

```c
esp_err_t bsp_pmic_get_and_clear_interrupts (
    uint8_t status
) 
```

### function `bsp_pmic_get_boot_irq_snapshot`

```c
esp_err_t bsp_pmic_get_boot_irq_snapshot (
    uint8_t status,
    bool *valid
) 
```


Copy the IRQ status snapshot captured at [**bsp\_pmic\_init()**](#function-bsp_pmic_init) time, before the init-time clear. This preserves events latched across an SoC power collapse (e.g. a regulator over-current lockout that killed 3V3 while the TG28 stayed alive on VBUS). \*valid is false if the snapshot was never captured.
### function `bsp_pmic_get_charge_current`

```c
esp_err_t bsp_pmic_get_charge_current (
    uint16_t *milliamps
) 
```

### function `bsp_pmic_get_charge_voltage`

```c
esp_err_t bsp_pmic_get_charge_voltage (
    uint16_t *millivolts
) 
```

### function `bsp_pmic_get_early_snapshot`

```c
esp_err_t bsp_pmic_get_early_snapshot (
    bsp_pmic_early_snapshot_t *snapshot
) 
```


Copy the first post-create/pre-configuration register snapshot.
### function `bsp_pmic_get_input_current_limit`

```c
esp_err_t bsp_pmic_get_input_current_limit (
    uint16_t *milliamps
) 
```

### function `bsp_pmic_get_power_off_source`

```c
esp_err_t bsp_pmic_get_power_off_source (
    uint8_t *source
) 
```


Read the TG28\_SW REG21 power-off-source latch. The register survives while the TG28 stays supplied (VBUS or battery), so after an unexpected system power cut the SoC must be revived with the PWRON key WITHOUT unplugging VBUS, then this read reveals whether the TG28 itself commanded the off.
### function `bsp_pmic_get_power_on_source`

```c
esp_err_t bsp_pmic_get_power_on_source (
    uint8_t *source
) 
```

### function `bsp_pmic_get_precharge_current`

```c
esp_err_t bsp_pmic_get_precharge_current (
    uint16_t *milliamps
) 
```

### function `bsp_pmic_get_status`

```c
esp_err_t bsp_pmic_get_status (
    bsp_pmic_status_t *status
) 
```

### function `bsp_pmic_get_termination_current`

```c
esp_err_t bsp_pmic_get_termination_current (
    uint16_t *milliamps,
    bool *enabled
) 
```

### function `bsp_pmic_get_vindpm`

```c
esp_err_t bsp_pmic_get_vindpm (
    uint16_t *millivolts
) 
```

### function `bsp_pmic_init`

```c
esp_err_t bsp_pmic_init (
    void
) 
```


TG28\_SW access. Init preserves regulator voltage/enable OTP state for the boot snapshot but clamps REG16 input current from its 1500 mA POR value to BSP\_PMIC\_SAFE\_INPUT\_CURRENT\_LIMIT\_MA, forces the charge-profile baseline (REG62/64/61/63, see the BSP\_PMIC\_SAFE\_\* constants) with exact readback verification, and programs the built-in reference battery model best-effort. Runtime requests above the input-limit default are accepted only from callers that have independently verified the connected source.
### function `bsp_pmic_power_off`

```c
esp_err_t bsp_pmic_power_off (
    void
) 
```


Power off the board through the TG28\_SW soft-PWROFF command (REG10 bit0). The PMU enters its off state as soon as the write is acknowledged: every rail but the RTCLDO shuts down, the board loses power, and the call does not return in practice. Flush any pending log output before calling.
### function `bsp_pmic_program_battery_model`

```c
esp_err_t bsp_pmic_program_battery_model (
    const uint8_t *model,
    size_t size
) 
```

### function `bsp_pmic_read_adc_mv`

```c
esp_err_t bsp_pmic_read_adc_mv (
    bsp_pmic_adc_channel_t channel,
    uint16_t *millivolts
) 
```


Read one TG28\_SW ADC channel in millivolts. The TDIE channel reports the die-temperature sensor voltage, not a temperature; EVT1 fixes TS to ground and has no battery NTC, so a TS value is never a valid battery temperature. A channel disabled at the OTP level is enabled for the measurement and restored afterwards. Prefer [**bsp\_pmic\_run\_adc\_diagnostic()**](#function-bsp_pmic_run_adc_diagnostic) when raw codes, conversion settling, or overflow validity matter.
### function `bsp_pmic_read_battery_model`

```c
esp_err_t bsp_pmic_read_battery_model (
    bool from_sram,
    uint8_t *model,
    size_t size
) 
```


Dump the TG28 fuel-gauge battery model area (128 bytes) into model. from\_sram selects the programmed/learned SRAM area, false the factory ROM area. See tg28\_sw\_read\_battery\_model() for the procedure.
### function `bsp_pmic_read_registers`

```c
esp_err_t bsp_pmic_read_registers (
    uint8_t register_address,
    uint8_t *values,
    size_t count
) 
```

### function `bsp_pmic_regulator_enable`

```c
esp_err_t bsp_pmic_regulator_enable (
    bsp_pmic_regulator_t regulator,
    bool enable
) 
```

### function `bsp_pmic_regulator_get_voltage`

```c
esp_err_t bsp_pmic_regulator_get_voltage (
    bsp_pmic_regulator_t regulator,
    uint16_t *millivolts
) 
```

### function `bsp_pmic_regulator_is_enabled`

```c
esp_err_t bsp_pmic_regulator_is_enabled (
    bsp_pmic_regulator_t regulator,
    bool *enabled
) 
```

### function `bsp_pmic_regulator_name`

```c
const char * bsp_pmic_regulator_name (
    bsp_pmic_regulator_t regulator
) 
```

### function `bsp_pmic_regulator_set_voltage`

```c
esp_err_t bsp_pmic_regulator_set_voltage (
    bsp_pmic_regulator_t regulator,
    uint16_t millivolts
) 
```

### function `bsp_pmic_run_adc_diagnostic`

```c
esp_err_t bsp_pmic_run_adc_diagnostic (
    bsp_pmic_adc_diagnostic_t *diagnostic
) 
```


Capture raw ADC data at 0/50/100/200/500/1000/2000 ms after enabling REG30 bits 0,2,3,4 in one write. The original control byte is restored and verified before return.
### function `bsp_pmic_set_charge_current`

```c
esp_err_t bsp_pmic_set_charge_current (
    uint16_t milliamps
) 
```

### function `bsp_pmic_set_charge_voltage`

```c
esp_err_t bsp_pmic_set_charge_voltage (
    uint16_t millivolts
) 
```

### function `bsp_pmic_set_input_current_limit`

```c
esp_err_t bsp_pmic_set_input_current_limit (
    uint16_t milliamps
) 
```

### function `bsp_pmic_set_precharge_current`

```c
esp_err_t bsp_pmic_set_precharge_current (
    uint16_t milliamps
) 
```


Set the REG61 precharge current (0-200 mA, 25 mA steps).
### function `bsp_pmic_set_termination_current`

```c
esp_err_t bsp_pmic_set_termination_current (
    uint16_t milliamps,
    bool enable
) 
```


Set the REG63 termination current (0-200 mA, 25 mA steps) and the termination-enable bit; disabling keeps the current code.
### function `bsp_pmic_set_vindpm`

```c
esp_err_t bsp_pmic_set_vindpm (
    uint16_t millivolts
) 
```

### function `bsp_pmic_switch_enable`

```c
esp_err_t bsp_pmic_switch_enable (
    bsp_pmic_switch_t sw,
    bool enable
) 
```


Open or close a TG28\_SW load-switch output (see [**bsp\_pmic\_switch\_t**](#enum-bsp_pmic_switch_t)). No voltage programming applies in switch mode; the switch passes its input rail straight through. BSP\_PMIC\_SWITCH\_DC1SW powers the RGB LED on this board and is OFF after power-on.
### function `bsp_pmic_switch_is_enabled`

```c
esp_err_t bsp_pmic_switch_is_enabled (
    bsp_pmic_switch_t sw,
    bool *enabled
) 
```

### function `bsp_pmic_switch_name`

```c
const char * bsp_pmic_switch_name (
    bsp_pmic_switch_t sw
) 
```

### function `bsp_power_domain_get`

```c
esp_err_t bsp_power_domain_get (
    bsp_power_domain_t domain,
    bool *enabled
) 
```


Return the last state successfully driven by bsp\_power\_domain\_set(), or ESP\_ERR\_INVALID\_STATE when this boot has not driven the domain yet.
### function `bsp_power_domain_name`

```c
const char * bsp_power_domain_name (
    bsp_power_domain_t domain
) 
```

### function `bsp_power_domain_set`

```c
esp_err_t bsp_power_domain_set (
    bsp_power_domain_t domain,
    bool enable
) 
```

### function `bsp_power_safe_state`

```c
esp_err_t bsp_power_safe_state (
    void
) 
```


Best-effort low-level shutdown. The registered application callback runs first, then direct domains, optional TG28\_SW rails, and back-feed-prone GPIOs are handled even when an earlier step fails. Returns the first error. Without a registered callback the application remains responsible for stopping every active protocol owner before entering this function.
### function `bsp_power_set_safe_shutdown_callback`

```c
esp_err_t bsp_power_set_safe_shutdown_callback (
    bsp_power_safe_shutdown_cb_t callback,
    void *user_ctx
) 
```


Install or clear (callback == NULL) the application shutdown callback. Configure it before tasks can call [**bsp\_power\_safe\_state()**](#function-bsp_power_safe_state).






## :camera: Camera

The BSP initializes the DVP pipeline and applies the board sensor timing and
image profile.

### Example Usage

The camera example is provided at [`examples/esp-idf/camera-test`](../../examples/esp-idf/camera-test). Generic sensor and DVP examples are provided by [`esp_video`](https://github.com/espressif/esp-video-components).

> [!NOTE]
> Select the fitted camera sensor in `menuconfig`.

### Camera API Reference

## Structures and Types

| Type | Name |
| ---: | :--- |
| struct | [**bsp\_camera\_cfg\_t**](#struct-bsp_camera_cfg_t) <br> |

## Functions

| Type | Name |
| ---: | :--- |
|  esp\_err\_t | [**bsp\_camera\_apply\_workaround**](#function-bsp_camera_apply_workaround) (uint32\_t v4l2\_pixel\_format) <br> |
|  esp\_err\_t | [**bsp\_camera\_start**](#function-bsp_camera_start) (const [**bsp\_camera\_cfg\_t**](#struct-bsp_camera_cfg_t) \*cfg) <br> |
|  esp\_err\_t | [**bsp\_camera\_stop**](#function-bsp_camera_stop) (void) <br> |

## Macros

| Type | Name |
| ---: | :--- |
| define  | [**BSP\_CAMERA\_D0**](#define-bsp_camera_d0)  GPIO\_NUM\_46<br> |
| define  | [**BSP\_CAMERA\_D1**](#define-bsp_camera_d1)  GPIO\_NUM\_47<br> |
| define  | [**BSP\_CAMERA\_D2**](#define-bsp_camera_d2)  GPIO\_NUM\_48<br> |
| define  | [**BSP\_CAMERA\_D3**](#define-bsp_camera_d3)  GPIO\_NUM\_49<br> |
| define  | [**BSP\_CAMERA\_D4**](#define-bsp_camera_d4)  GPIO\_NUM\_50<br> |
| define  | [**BSP\_CAMERA\_D5**](#define-bsp_camera_d5)  GPIO\_NUM\_51<br> |
| define  | [**BSP\_CAMERA\_D6**](#define-bsp_camera_d6)  GPIO\_NUM\_52<br> |
| define  | [**BSP\_CAMERA\_D7**](#define-bsp_camera_d7)  GPIO\_NUM\_53<br> |
| define  | [**BSP\_CAMERA\_DEVICE**](#define-bsp_camera_device)  ESP\_VIDEO\_DVP\_DEVICE\_NAME<br> |
| define  | [**BSP\_CAMERA\_GPIO\_XCLK**](#define-bsp_camera_gpio_xclk)  BSP\_CAMERA\_XCLK<br> |
| define  | [**BSP\_CAMERA\_HSYNC**](#define-bsp_camera_hsync)  GPIO\_NUM\_57<br> |
| define  | [**BSP\_CAMERA\_PCLK**](#define-bsp_camera_pclk)  GPIO\_NUM\_54<br> |
| define  | [**BSP\_CAMERA\_PWDN**](#define-bsp_camera_pwdn)  GPIO\_NUM\_40<br> |
| define  | [**BSP\_CAMERA\_RST**](#define-bsp_camera_rst)  GPIO\_NUM\_39<br> |
| define  | [**BSP\_CAMERA\_VSYNC**](#define-bsp_camera_vsync)  GPIO\_NUM\_56<br> |
| define  | [**BSP\_CAMERA\_XCLK**](#define-bsp_camera_xclk)  GPIO\_NUM\_55<br> |
| define  | [**BSP\_CAMERA\_XCLK\_CLOCK\_MHZ**](#define-bsp_camera_xclk_clock_mhz)  20<br> |


## Structures and Types Documentation

### struct `bsp_camera_cfg_t`


Variables:

-  uint8\_t dummy  


## Functions Documentation

### function `bsp_camera_apply_workaround`

```c
esp_err_t bsp_camera_apply_workaround (
    uint32_t v4l2_pixel_format
)
```

Reapply the board sensor override after esp\_video\_open() reloads its table.

### function `bsp_camera_start`

```c
esp_err_t bsp_camera_start (
    const bsp_camera_cfg_t *cfg
) 
```


DVP camera pipeline. Sensor support is selected in project configuration.

### function `bsp_camera_stop`

```c
esp_err_t bsp_camera_stop (
    void
) 
```









### Board Control and RTC API Reference

## Structures and Types

| Type | Name |
| ---: | :--- |
| typedef rx8130ce\_alarm\_t | [**bsp\_rtc\_alarm\_t**](#typedef-bsp_rtc_alarm_t)  <br> |
| struct | [**bsp\_rtc\_status\_t**](#struct-bsp_rtc_status_t) <br> |
| struct | [**bsp\_rtc\_time\_t**](#struct-bsp_rtc_time_t) <br> |
| typedef void(\* | [**bsp\_shared\_irq\_callback\_t**](#typedef-bsp_shared_irq_callback_t)  <br> |
| struct | [**bsp\_shared\_irq\_status\_t**](#struct-bsp_shared_irq_status_t) <br> |

## Functions

| Type | Name |
| ---: | :--- |
|  esp\_err\_t | [**bsp\_board\_init**](#function-bsp_board_init) (void) <br> |
|  esp\_err\_t | [**bsp\_rtc\_alarm\_irq\_enable**](#function-bsp_rtc_alarm_irq_enable) (bool enable) <br> |
|  esp\_err\_t | [**bsp\_rtc\_clear\_interrupt\_flags**](#function-bsp_rtc_clear_interrupt_flags) (uint8\_t \*flags) <br> |
|  esp\_err\_t | [**bsp\_rtc\_deinit**](#function-bsp_rtc_deinit) (void) <br> |
|  esp\_err\_t | [**bsp\_rtc\_get\_alarm**](#function-bsp_rtc_get_alarm) ([**bsp\_rtc\_alarm\_t**](#typedef-bsp_rtc_alarm_t) \*out\_alarm) <br> |
|  esp\_err\_t | [**bsp\_rtc\_get\_and\_clear\_alarm\_flag**](#function-bsp_rtc_get_and_clear_alarm_flag) (bool \*alarm\_flag) <br> |
|  esp\_err\_t | [**bsp\_rtc\_get\_status**](#function-bsp_rtc_get_status) ([**bsp\_rtc\_status\_t**](#struct-bsp_rtc_status_t) \*status) <br> |
|  esp\_err\_t | [**bsp\_rtc\_get\_time**](#function-bsp_rtc_get_time) ([**bsp\_rtc\_time\_t**](#struct-bsp_rtc_time_t) \*time, [**bsp\_rtc\_status\_t**](#struct-bsp_rtc_status_t) \*status) <br> |
|  esp\_err\_t | [**bsp\_rtc\_init**](#function-bsp_rtc_init) (void) <br> |
|  esp\_err\_t | [**bsp\_rtc\_set\_alarm**](#function-bsp_rtc_set_alarm) (const [**bsp\_rtc\_alarm\_t**](#typedef-bsp_rtc_alarm_t) \*alarm) <br> |
|  esp\_err\_t | [**bsp\_rtc\_set\_time**](#function-bsp_rtc_set_time) (const [**bsp\_rtc\_time\_t**](#struct-bsp_rtc_time_t) \*time) <br> |
| esp_err_t | [**bsp_shared_irq_init**](#function-bsp_shared_irq_init) (void) |
|  esp\_err\_t | [**bsp\_shared\_irq\_register\_callback**](#function-bsp_shared_irq_register_callback) ([**bsp\_shared\_irq\_callback\_t**](#typedef-bsp_shared_irq_callback_t) cb, void \*arg) <br> |
|  esp\_err\_t | [**bsp\_shared\_irq\_service**](#function-bsp_shared_irq_service) ([**bsp\_shared\_irq\_status\_t**](#struct-bsp_shared_irq_status_t) \*status) <br> |

## Macros

| Type | Name |
| ---: | :--- |
| define  | [**BSP\_FUSB303B\_I2C\_ADDRESS\_HIGH**](#define-bsp_fusb303b_i2c_address_high)  0x31<br> |
| define  | [**BSP\_FUSB303B\_I2C\_ADDRESS\_LOW**](#define-bsp_fusb303b_i2c_address_low)  0x21<br> |
| define  | [**BSP\_RX8130CE\_I2C\_ADDRESS**](#define-bsp_rx8130ce_i2c_address)  0x32<br> |
| define  | [**BSP\_TG28\_SW\_I2C\_ADDRESS**](#define-bsp_tg28_sw_i2c_address)  0x34<br> |
| define  | [**BSP\_UART0\_RX**](#define-bsp_uart0_rx)  GPIO\_NUM\_59<br> |
| define  | [**BSP\_UART0\_TX**](#define-bsp_uart0_tx)  GPIO\_NUM\_58<br> |


## Structures and Types Documentation

### typedef `bsp_rtc_alarm_t`

```c
typedef rx8130ce_alarm_t bsp_rtc_alarm_t;
```


Alarm compare settings, aliased from the RX8130CE driver.
### struct `bsp_rtc_status_t`


RX8130CE retained status flags.

Variables:

-  bool alarm  

-  bool backup_battery_full  

-  bool backup_voltage_low  

-  uint8\_t raw  

-  bool reset  

-  bool time_valid  

-  bool timer  

-  bool update  

### struct `bsp_rtc_time_t`


Calendar time stored in the RX8130CE.

Variables:

-  uint8\_t day  

-  uint8\_t hour  

-  uint8\_t minute  

-  uint8\_t month  

-  uint8\_t second  

-  uint8\_t weekday  

-  uint16\_t year  

### typedef `bsp_shared_irq_callback_t`

```c
typedef void(* bsp_shared_irq_callback_t) (void *arg);
```


Callback invoked from ISR context when the shared interrupt line asserts.
### struct `bsp_shared_irq_status_t`


Combined interrupt snapshot for the shared active-low line on GPIO2.

Variables:

-  bool line_released  

-  uint8\_t pmic  

-  uint8\_t rtc  

-  unsigned service_passes  


## Functions Documentation

### function `bsp_board_init`

```c
esp_err_t bsp_board_init (
    void
) 
```


Initialize the board in its disabled, low-risk state.
### function `bsp_rtc_alarm_irq_enable`

```c
esp_err_t bsp_rtc_alarm_irq_enable (
    bool enable
) 
```

### function `bsp_rtc_clear_interrupt_flags`

```c
esp_err_t bsp_rtc_clear_interrupt_flags (
    uint8_t *flags
) 
```

### function `bsp_rtc_deinit`

```c
esp_err_t bsp_rtc_deinit (
    void
) 
```

### function `bsp_rtc_get_alarm`

```c
esp_err_t bsp_rtc_get_alarm (
    bsp_rtc_alarm_t *out_alarm
) 
```

### function `bsp_rtc_get_and_clear_alarm_flag`

```c
esp_err_t bsp_rtc_get_and_clear_alarm_flag (
    bool *alarm_flag
) 
```


Report and clear only the RX8130CE alarm flag (AF), leaving UF/TF intact. alarm\_flag may be NULL when the caller only needs to clear AF.
### function `bsp_rtc_get_status`

```c
esp_err_t bsp_rtc_get_status (
    bsp_rtc_status_t *status
) 
```

### function `bsp_rtc_get_time`

```c
esp_err_t bsp_rtc_get_time (
    bsp_rtc_time_t *time,
    bsp_rtc_status_t *status
) 
```

### function `bsp_rtc_init`

```c
esp_err_t bsp_rtc_init (
    void
) 
```


RX8130CE access.
### function `bsp_rtc_set_alarm`

```c
esp_err_t bsp_rtc_set_alarm (
    const bsp_rtc_alarm_t *alarm
) 
```

### function `bsp_rtc_set_time`

```c
esp_err_t bsp_rtc_set_time (
    const bsp_rtc_time_t *time
) 
```

### function `bsp_shared_irq_init`

```c
esp_err_t bsp_shared_irq_init(void);
```

Initialize the GPIO ISR service once. BSP display setup and shared-line
callback registration call this automatically before registering their
handlers. The service remains available across display stop/start cycles.

### function `bsp_shared_irq_register_callback`

```c
esp_err_t bsp_shared_irq_register_callback (
    bsp_shared_irq_callback_t cb,
    void *arg
) 
```


Register a callback for the shared active-low interrupt line on GPIO2. The line is level-triggered (wire-ORed sources share it), masked in the ISR, and re-armed by [**bsp\_shared\_irq\_service()**](#function-bsp_shared_irq_service); the callback runs in ISR context and must only notify a task. Pass NULL to unregister.
### function `bsp_shared_irq_service`

```c
esp_err_t bsp_shared_irq_service (
    bsp_shared_irq_status_t *status
) 
```


Service TG28\_SW and RX8130CE until their shared interrupt line is released. When a callback is registered, the line is re-armed before returning.



