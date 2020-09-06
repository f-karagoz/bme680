# BME680 sensor driver library for Mongoose OS

## Overview

This library integrates the low-level [driver](https://github.com/BoschSensortec/BME680_driver)
and the [BSEC](https://www.bosch-sensortec.com/bst/products/all_products/bsec) software library
which provides high-level sensor control to obtain relaible air quality sensor data.

This library provides the necessary glue code for both to work under Mongoose OS and a number of helper functions.

When sensor output is ready,i an `MGOS_EV_BME680_BSEC_OUTPUT` event is triggered which receives a structure containing sensor outputs (see `struct mgos_bsec_output` definition in [mgos_bme680.h](include/mgos_bme680.h)).

### Platform support

Currently only supported on ESP8266 and ESP32 platforms, ARM support is a `TODO`.

## Quick Start

The library is configured through the `bme680` configuration section, defined [here](mos.yml).

Several things need to be done to obtain readings from the sensor:

 - I2C must be enabled, set `i2c.enable=true` (currently only I2C interface is supported).
 - `bme680.i2c_addr` must be set to the correct address. It's either 0x76 or 0x77 depending on the state of the address selection pin.
 - `bme680.enable` needs to be set ot `true` for library to be initalized at all.

With these and the rest of the settings left in their default state, you should get readings from all the sensors at 3 second interval.

## IAQ sensor accuracy

IAQ sensor requires calibration before producting accurate values. Values with accuracy value less than 3 are unreliable.

By default the library will perform calibration automatically (still may take up to 30 minutes to complete).

## Configuration details

A number of options are provided for more advanced control of the sensor behavior.

 - `bme680.bsec.enable`: normally it is advisable to use the BSEC library to process raw values returned by the sensor.
   Turning this off will enable you to either use the sensor directly (reference to the dev ice can be obtained via `mgos_bme680_get_global()`) or initialize drive the BSEC library yourself (e.g. for managing multiple sensors).
 - `bme680.bsec.config_file`: BSEC library comes with a number of pre-generated configuration profiles that can be loaded to improve accurcy of the measurements. These are contained in the [config subdirectory](BSEC_1.4.7.4_Generic_Release/config/) and come as binary blobs, CSV files or C source code. Take the `bsec_iaq.config` file from the appropriate subdirectory and copy it to the device filesystem (or include in your firmware's initial filesystem image). You can also include several and switch between them by adjusting the value of this setting.
 - `bme680.bsec.state_file`, `bme680.bsec.state_save_interval`: BSEC library performs estimations over long periods of time and the accuracy of its output relies on long-term state that it keeps. It is therefore necessary to make sure it is persisted across device restarts. Mos integration code will load BSEC state from the `state_file` on initialization and save it every `state_save_interval` seconds. Set `state_file` to empty to disable loading of state, set interval to a negative value to disable automatically saving it. You can still use `mgos_bsec_set_state_from_file()` and `mgos_bsec_save_state_to_file()` to load and save the state to a file manually.
 - `bme680.bsec.{iaq,temp,rh,ps}_sample_rate`: Set sampling rates for different parts of the BME680 multi-sensor. Each can be individually disabled (empty string), sampled at 3s interval (`LP`) or every 300s (`ULP`). In particular, since gas sensor uses heater extensively, setting it to `ULP` will save considerable amount of power.
 - `bme680.bsec.iaq_auto_cal`: if IAQ sensor is enabled (`bme680.bsec.iaq_sample_rate` is not empty) and this option is enabled, mos will automatically raise sampling rate of the IAQ sensor to 3s until accuracy reaches 3 (and stays there for a while). It will then return the sampling rate to whatever it was set to previously. So in practice this only matters if IAQ sensor is confiugred for ULP rate.

## Example

With mOS library providing the integration, getting samples from the sensor is very simple - all you need to do is subscribe to the event:

```c
#include "mgos.h"
#include "mgos_bme680.h"

static void bme680_output_cb(int ev, void *ev_data, void *arg) {
  const struct mgos_bsec_output *out = (struct mgos_bsec_output *) ev_data;
  double ts = out->temp.time_stamp / 1000000000.0;
  float ps_kpa = out->ps.signal / 1000.0f;
  float ps_mmhg = out->ps.signal / 133.322f;
  if (out->iaq.time_stamp > 0) {
    LOG(LL_INFO,
        ("%.2f IAQ %.2f (acc %d) T %.2f RH %.2f P %.2f kPa (%.2f mmHg)", ts,
         out->iaq.signal, out->iaq.accuracy, out->temp.signal, out->rh.signal,
         ps_kpa, ps_mmhg));
  } else {
    LOG(LL_INFO, ("%.2f T %.2f RH %.2f P %.2f kPa (%.2f mmHg)", ts,
                  out->temp.signal, out->rh.signal, ps_kpa, ps_mmhg));
  }
  (void) ev;
  (void) arg;
}

enum mgos_app_init_result mgos_app_init(void) {
  mgos_event_add_handler(MGOS_EV_BME680_BSEC_OUTPUT, bme680_output_cb, NULL);
  return MGOS_APP_INIT_SUCCESS;
}
```

Output:

```
[Aug 26 23:00:59.324] mgos_i2c_gpio_maste:250 I2C GPIO init ok (SDA: 4, SCL: 5, freq: 100000)
[Aug 26 23:00:59.348] mgos_bme680.c:466       BME680 @ 0/0x77 init ok
[Aug 26 23:00:59.353] mgos_bme680.c:396       BSEC 1.4.7.4 initialized
[Aug 26 23:00:59.364] mgos_bme680.c:404       Failed to load BSEC config from bsec_iaq.config: -33, will use defaults
[Aug 26 23:00:59.377] mgos_bme680.c:414       Failed to load BSEC state from bsec.state: -33, will use defaults
...
[Aug 26 23:01:00.337] mgos_init.c:36          Init done, RAM: 51152 total, 41996 free, 42000 min free
[Aug 26 23:01:00.352] mgos_bme680.c:281       IAQ sensor requires calibration
[Aug 26 23:01:00.356] main.c:13               0.68 IAQ 25.00 (acc 0) T 27.07 RH 57.51 P 101.76 kPa (763.28 mmHg)
[Aug 26 23:01:02.641] main.c:13               3.68 IAQ 25.00 (acc 0) T 26.98 RH 57.87 P 101.76 kPa (763.29 mmHg)
[Aug 26 23:01:05.645] main.c:13               6.69 IAQ 25.00 (acc 0) T 26.99 RH 57.88 P 101.76 kPa (763.28 mmHg)
[Aug 26 23:01:08.650] main.c:13               9.69 IAQ 25.00 (acc 0) T 27.01 RH 57.84 P 101.76 kPa (763.28 mmHg)
...
[Aug 26 23:05:54.095] main.c:13               295.11 IAQ 25.00 (acc 0) T 26.72 RH 58.46 P 101.76 kPa (763.27 mmHg)
[Aug 26 23:05:56.868] mgos_bme680.c:368       BSEC state saved (bsec.state)
[Aug 26 23:05:57.100] main.c:13               298.11 IAQ 25.00 (acc 0) T 26.72 RH 58.46 P 101.76 kPa (763.26 mmHg)
...
[Aug 26 23:08:12.326] main.c:13               433.32 IAQ 51.91 (acc 1) T 26.70 RH 58.46 P 101.76 kPa (763.25 mmHg)
[Aug 26 23:08:15.331] main.c:13               436.33 IAQ 250.00 (acc 2) T 26.71 RH 58.41 P 101.76 kPa (763.27 mmHg)
...
[Aug 26 23:16:46.136] main.c:13               947.09 IAQ 49.84 (acc 3) T 26.72 RH 58.90 P 101.76 kPa (763.23 mmHg)
[Aug 26 23:16:49.140] mgos_bme680.c:289       IAQ sensor calibration complete
[Aug 26 23:16:49.144] main.c:13               950.10 IAQ 50.42 (acc 3) T 26.72 RH 58.92 P 101.76 kPa (763.23 mmHg)
[Aug 26 23:16:52.145] main.c:13               953.10 IAQ 50.01 (acc 3) T 26.73 RH 58.88 P 101.76 kPa (763.23 mmHg)
[Aug 26 23:16:55.150] main.c:13               956.10 IAQ 50.95 (acc 3) T 26.73 RH 58.87 P 101.76 kPa (763.23 mmHg)
...
```

## License

See [here](LICENSE.md).
