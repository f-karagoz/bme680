/*
 * Copyright (c) 2019 Deomid "rojer" Ryabkov
 * All rights reserved
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "mgos_bme680.h"

#include <stdbool.h>
#include <stdio.h>

#include "mgos.h"
#include "mgos_i2c.h"

#include "bme68x.h"
#include "bsec_interface.h"

#ifndef MGOS_BME68X_BSEC_MIN_CAL_CYCLES
#define MGOS_BME68X_BSEC_MIN_CAL_CYCLES 50
#endif

struct mgos_bme68x_state {
  struct mgos_config_bme68x cfg;
  struct bme68x_dev dev;
  struct bme68x_conf tph_sett;
  struct bme68x_heatr_conf gas_sett;
  mgos_timer_id bsec_timer_id;
  mgos_timer_id meas_timer_id;
  int64_t next_ts;
  int state_save_delay_ms;
  float input_heat_source_value;
  float prev_iaq_sr;
  int iaq_cal_cycles;
};

static struct mgos_bme68x_state *s_state;

static void mgos_bsec_timer_cb(void *arg);

static BME68X_INTF_RET_TYPE bme68x_i2c_read(uint8_t reg_addr, uint8_t *reg_data, uint32_t length, void *intf_ptr)
{
  (void)intf_ptr;   // Suppress compiler warning
  // TODO check bus
  struct mgos_i2c *bus = mgos_i2c_get_bus(BME68X_I2C_ADDR_LOW); 
  // TODO get 'bus' and 'addr' (DONE)
  return mgos_i2c_read_reg_n(bus, BME68X_I2C_ADDR_LOW, reg_addr, length, reg_data) ? 0 : -1;
}

static BME68X_INTF_RET_TYPE bme68x_i2c_write(uint8_t reg_addr, const uint8_t *reg_data, uint32_t length, void *intf_ptr)
{
  (void)intf_ptr;   // Suppress compiler warning
  // TODO check bus
  struct mgos_i2c *bus = mgos_i2c_get_bus(BME68X_I2C_ADDR_LOW); 
  // TODO get 'bus' and 'addr' (DONE)
  return mgos_i2c_write_reg_n(bus, BME68X_I2C_ADDR_LOW, reg_addr, length, reg_data) ? 0 : -1;
}

/* TODO remove this 
static int8_t bme68x_i2c_write(uint8_t dev_id, uint8_t reg_addr, uint8_t *data, uint16_t len)
{
  struct mgos_i2c *bus = mgos_i2c_get_bus(dev_id >> 1);
  int addr = BME68X_I2C_ADDR_LOW + (dev_id & 1);
  return mgos_i2c_write_reg_n(bus, addr, reg_addr, len, data) ? 0 : -1;
}
*/

static void bme68x_delay_us(uint32_t period, void *intf_ptr) {
  mgos_msleep(period * 1000);
  (void)intf_ptr;               // Suppress compiler warning
}

bsec_library_return_t mgos_bsec_set_configuration_from_file(const char *file) {
  bsec_library_return_t ret;
  uint8_t work_buffer[BSEC_MAX_PROPERTY_BLOB_SIZE] = {0};
  size_t size = 0;
  char *data = cs_read_file(file, &size);
  if (data == NULL) return BSEC_E_CONFIG_FAIL;
  // Binary blob configs have 4 extra bytes at the beginning that .c and .csv
  // versions don't and the library cannot handle them.
  ret = bsec_set_configuration((uint8_t *) data + 4, (uint32_t) size - 4,
                               work_buffer, sizeof(work_buffer));
  free(data);
  return ret;
}

bsec_library_return_t mgos_bsec_set_state_from_file(const char *file) {
  bsec_library_return_t ret;
  uint8_t work_buffer[BSEC_MAX_PROPERTY_BLOB_SIZE] = {0};
  size_t size = 0;
  char *data = cs_read_file(file, &size);
  if (data == NULL) return BSEC_E_CONFIG_FAIL;
  ret = bsec_set_state((uint8_t *) data, (uint32_t) size, work_buffer,
                       sizeof(work_buffer));
  free(data);
  return ret;
}

bsec_library_return_t mgos_bsec_save_state_to_file(const char *file) {
  bsec_library_return_t ret;
  uint8_t state[BSEC_MAX_STATE_BLOB_SIZE] = {0};
  uint8_t work_buffer[BSEC_MAX_PROPERTY_BLOB_SIZE] = {0};
  uint32_t size = 0;
  ret = bsec_get_state(0, state, sizeof(state), work_buffer,
                       sizeof(work_buffer), &size);
  if (ret != BSEC_OK) return ret;
  FILE *f = fopen(file, "w");
  if (f == NULL) return BSEC_E_CONFIG_FAIL;
  ret = BSEC_OK;
  if (fwrite(state, 1, size, f) != size) {
    ret = BSEC_E_CONFIG_FAIL;
  }
  fclose(f);
  return ret;
}

void mgos_bsec_set_input_heat_source_value(float value) {
  if (s_state == NULL) return;
  s_state->input_heat_source_value = value;
}

static bsec_library_return_t mgos_bsec_set_iaq_sample_rate_int(float sr) {
  bsec_sensor_configuration_t rvs[] = {
      {.sensor_id = BSEC_OUTPUT_IAQ, .sample_rate = sr},
      {.sensor_id = BSEC_OUTPUT_STATIC_IAQ, .sample_rate = sr},
      {.sensor_id = BSEC_OUTPUT_CO2_EQUIVALENT, .sample_rate = sr},
      {.sensor_id = BSEC_OUTPUT_BREATH_VOC_EQUIVALENT, .sample_rate = sr},
      {.sensor_id = BSEC_OUTPUT_STABILIZATION_STATUS, .sample_rate = sr},
      {.sensor_id = BSEC_OUTPUT_RUN_IN_STATUS, .sample_rate = sr},
      {.sensor_id = BSEC_OUTPUT_RAW_GAS, .sample_rate = sr},
  };
  uint8_t num_rss = BSEC_MAX_PHYSICAL_SENSOR;
  bsec_sensor_configuration_t rss[BSEC_MAX_PHYSICAL_SENSOR];
  return bsec_update_subscription(rvs, ARRAY_SIZE(rvs), rss, &num_rss);
};

bsec_library_return_t mgos_bsec_set_iaq_sample_rate(float sr) {
  bsec_library_return_t ret = mgos_bsec_set_iaq_sample_rate_int(sr);
  if (ret == BSEC_OK) {
    s_state->prev_iaq_sr = sr;
  }
  return ret;
}

bsec_library_return_t mgos_bsec_set_temp_sample_rate(float sr) {
  bsec_sensor_configuration_t rvs[] = {
      {.sensor_id = BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE,
       .sample_rate = sr},
      {.sensor_id = BSEC_OUTPUT_RAW_TEMPERATURE, .sample_rate = sr},
  };
  uint8_t num_rss = BSEC_MAX_PHYSICAL_SENSOR;
  bsec_sensor_configuration_t rss[BSEC_MAX_PHYSICAL_SENSOR];
  return bsec_update_subscription(rvs, ARRAY_SIZE(rvs), rss, &num_rss);
}

bsec_library_return_t mgos_bsec_set_rh_sample_rate(float sr) {
  bsec_sensor_configuration_t rvs[] = {
      {.sensor_id = BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY,
       .sample_rate = sr},
      {.sensor_id = BSEC_OUTPUT_RAW_HUMIDITY, .sample_rate = sr},
  };
  uint8_t num_rss = BSEC_MAX_PHYSICAL_SENSOR;
  bsec_sensor_configuration_t rss[BSEC_MAX_PHYSICAL_SENSOR];
  return bsec_update_subscription(rvs, ARRAY_SIZE(rvs), rss, &num_rss);
}

bsec_library_return_t mgos_bsec_set_ps_sample_rate(float sr) {
  bsec_sensor_configuration_t rvs[] = {
      {.sensor_id = BSEC_OUTPUT_RAW_PRESSURE, .sample_rate = sr},
  };
  uint8_t num_rss = BSEC_MAX_PHYSICAL_SENSOR;
  bsec_sensor_configuration_t rss[BSEC_MAX_PHYSICAL_SENSOR];
  return bsec_update_subscription(rvs, ARRAY_SIZE(rvs), rss, &num_rss);
}

bool mgos_bsec_start(void) {
  if (s_state == NULL) {
    LOG(LL_ERROR, ("BME68X sensor not initialized"));
    return false;
  }
  mgos_bsec_timer_cb(NULL);
  return true;
}

int8_t mgos_bme68x_init_dev_i2c(struct bme68x_dev *dev, int bus_no, int addr) {
  dev->intf = BME68X_I2C_INTF;
  // dev->dev_id = (bus_no << 1) | (addr & 1);
  dev->read = bme68x_i2c_read;
  dev->write = bme68x_i2c_write;
  dev->delay_us = bme68x_delay_us;
  return bme68x_init(dev);
}

static void mgos_bsec_meas_timer_cb(void *arg) {
  int8_t bme68x_status;
  uint8_t power_mode = 0;
  uint8_t n_data = 0;
  const bsec_bme_settings_t *ss = (bsec_bme_settings_t *) arg;
  int64_t ts = ss->next_call;
  s_state->meas_timer_id = MGOS_INVALID_TIMER_ID;
  if (ss->trigger_measurement) {
    while (power_mode != BME68X_SLEEP_MODE) {         // TODO inspect why we need to in sleep
      if (bme68x_get_op_mode(&power_mode, &s_state->dev) != 0) return;
    }
  }
  if (ss->process_data == 0) return;
  uint8_t num_inputs = 0;           // TODO check id 'n_data' can be used in place of this
  bsec_input_t inputs[BSEC_MAX_PHYSICAL_SENSOR];
  static struct bme68x_data data;
  // reading in forced mode
  bme68x_status = bme68x_get_data(BME68X_FORCED_MODE, &data, &n_data, &s_state->dev);
  if (bme68x_status != 0) {
    LOG(LL_ERROR, ("Failed to read sensor data: %d", bme68x_status));
    return;
  }
  if (data.status & BME68X_NEW_DATA_MSK) {
    if (ss->process_data & BSEC_PROCESS_PRESSURE) {
      inputs[num_inputs].sensor_id = BSEC_INPUT_PRESSURE;
      inputs[num_inputs].signal = data.pressure;
      inputs[num_inputs].time_stamp = ts;
      num_inputs++;
    }
    if (ss->process_data & BSEC_PROCESS_TEMPERATURE) {
      /* Place temperature sample into input struct */
      inputs[num_inputs].sensor_id = BSEC_INPUT_TEMPERATURE;
#ifdef BME68X_USE_FPU
      inputs[num_inputs].signal = data.temperature;
#else
      inputs[num_inputs].signal = data.temperature / 100.0f;
#endif
      inputs[num_inputs].time_stamp = ts;
      num_inputs++;
      inputs[num_inputs].sensor_id = BSEC_INPUT_HEATSOURCE;
      inputs[num_inputs].signal = s_state->input_heat_source_value;
      inputs[num_inputs].time_stamp = ts;
      num_inputs++;
    }
    if (ss->process_data & BSEC_PROCESS_HUMIDITY) {
      inputs[num_inputs].sensor_id = BSEC_INPUT_HUMIDITY;
#ifdef BME68X_USE_FPU
      inputs[num_inputs].signal = data.humidity;
#else
      inputs[num_inputs].signal = data.humidity / 1000.0f;
#endif
      inputs[num_inputs].time_stamp = ts;
      num_inputs++;
    }
    if (ss->process_data & BSEC_PROCESS_GAS &&
        data.status & BME68X_GASM_VALID_MSK) {
      inputs[num_inputs].sensor_id = BSEC_INPUT_GASRESISTOR;
      inputs[num_inputs].signal = data.gas_resistance;
      inputs[num_inputs].time_stamp = ts;
      num_inputs++;
    }
  }
  for (uint8_t i = 0; i < num_inputs; i++) {
    LOG(LL_VERBOSE_DEBUG,
        ("in : %d %.2f", inputs[i].sensor_id, inputs[i].signal));
  }
  struct mgos_bsec_output ev_arg = {.num_outputs = 0};
  ev_arg.num_outputs = BSEC_NUMBER_OUTPUTS;
  bsec_library_return_t bsec_status =
      bsec_do_steps(inputs, num_inputs, ev_arg.outputs, &ev_arg.num_outputs);
  LOG(LL_DEBUG, ("BSEC %lld run: %d inputs, status %d, %d outputs", ts,
                 num_inputs, bsec_status, ev_arg.num_outputs));
  for (uint8_t i = 0; i < ev_arg.num_outputs; i++) {
    const bsec_output_t *out = &ev_arg.outputs[i];
    LOG(LL_VERBOSE_DEBUG,
        ("out: %d %.2f %d", out->sensor_id, out->signal, out->accuracy));
    switch (out->sensor_id) {
      case BSEC_OUTPUT_IAQ:
        ev_arg.iaq = *out;
        break;
      case BSEC_OUTPUT_CO2_EQUIVALENT:
        ev_arg.co2 = *out;
        break;
      case BSEC_OUTPUT_BREATH_VOC_EQUIVALENT:
        ev_arg.voc = *out;
        break;
      case BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE:
        ev_arg.temp = *out;
        break;
      case BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY:
        ev_arg.rh = *out;
        break;
      case BSEC_OUTPUT_RAW_PRESSURE:
        ev_arg.ps = *out;
        break;
    }
  }
  if (s_state->cfg.bsec.iaq_auto_cal && ev_arg.iaq.time_stamp > 0) {
    if (ev_arg.iaq.accuracy < 3 &&
        s_state->iaq_cal_cycles < MGOS_BME68X_BSEC_MIN_CAL_CYCLES) {
      if (s_state->iaq_cal_cycles == 0) {
        if (ev_arg.iaq.accuracy == 2) {
          LOG(LL_INFO, ("IAQ sensor is calibrating"));
        } else {
          LOG(LL_INFO, ("IAQ sensor needs calibration"));
        }
        mgos_bsec_set_iaq_sample_rate_int(BSEC_SAMPLE_RATE_LP);
      }
      s_state->iaq_cal_cycles = MGOS_BME68X_BSEC_MIN_CAL_CYCLES;
    }
    if (ev_arg.iaq.accuracy == 3 && s_state->iaq_cal_cycles > 0) {
      s_state->iaq_cal_cycles--;
      if (s_state->iaq_cal_cycles == 0) {
        LOG(LL_INFO, ("IAQ sensor calibration complete"));
        mgos_bsec_set_iaq_sample_rate(s_state->prev_iaq_sr);
      }
    }
  }
  mgos_event_trigger(MGOS_EV_BME68X_BSEC_OUTPUT, &ev_arg);
}

static int mgos_bme68x_run_once(int *delay_ms) {
  int8_t bme68x_status;
  int64_t ts = s_state->next_ts;
  static bsec_bme_settings_t ss = {0};
  bsec_library_return_t ret = bsec_sensor_control(ts, &ss);
  LOG(LL_DEBUG,
      ("BSEC %lld ctl: process 0x%x, ht %u dur %u ms, gas %d, po %d, to %d, ho "
       "%d, tm %d, next %lld",
       ts, (unsigned) ss.process_data, ss.heater_temperature,
       ss.heater_duration, ss.run_gas, ss.pressure_oversampling,
       ss.temperature_oversampling, ss.humidity_oversampling,
       ss.trigger_measurement, ss.next_call));
  if (ret != BSEC_OK) return ret;
  s_state->next_ts = ss.next_call;
  *delay_ms = (ss.next_call - ts) / 1000000;
  if (ss.trigger_measurement) {
    s_state->tph_sett.os_hum = ss.humidity_oversampling;
    s_state->tph_sett.os_pres = ss.pressure_oversampling;
    s_state->tph_sett.os_temp = ss.temperature_oversampling;
    s_state->gas_sett.enable = ss.run_gas;
    s_state->gas_sett.heatr_temp = ss.heater_temperature;
    s_state->gas_sett.heatr_dur = ss.heater_duration;
    bme68x_status = bme68x_set_op_mode(BME68X_FORCED_MODE, &s_state->dev);
    if (bme68x_status != BME68X_OK)
    {
      LOG(LL_ERROR, ("Failed to set BME68X %s: %d", "op mode", bme68x_status));
      return -1002;
    }
  
    bme68x_status = bme68x_set_conf(&s_state->tph_sett, &s_state->dev);
    // TODO check if we set the correct fields with the new method
    // maybe: BME680_OST_SEL=BME68X_OST_MSK ,BME68X_OSP_SEL=BME68X_OSP_MSK ,
    //        BME68X_OSH_SEL=BME68X_OSH_MSK ,BME68X_GAS_SENSOR_SEL=BME68X_GAS_RANGE_MSK
    // bme68x_status =
    //     bme68x_set_sensor_settings((BME680_OST_SEL | BME68X_OSP_SEL |
    //                                 BME68X_OSH_SEL | BME68X_GAS_SENSOR_SEL),
    //                                &s_state->dev);
    if (bme68x_status != 0) {
      LOG(LL_ERROR, ("Failed to set BME68X %s: %d", "settings", bme68x_status));
      return -1000;
    }
    bme68x_status = bme68x_set_op_mode(BME68X_FORCED_MODE, &s_state->dev);
    if (bme68x_status != BME68X_OK) {
      LOG(LL_ERROR, ("Failed to set BME68X %s: %d", "mode", bme68x_status));
      return -1001;
    }
    // Gets the meas_period as 's_state->gas_sett.heatr_dur'
    bme68x_status =  bme68x_get_heatr_conf(&s_state->gas_sett, &s_state->dev);
    if (bme68x_status != BME68X_OK) {
      LOG(LL_ERROR, ("Failed to set BME68X %s: %d", "heater duration", bme68x_status));
      return -1002;
    }
    ss.next_call = ts;
    s_state->meas_timer_id =
        mgos_set_timer(s_state->gas_sett.heatr_dur, 0, mgos_bsec_meas_timer_cb, &ss);
  } else {
    mgos_bsec_meas_timer_cb(&ss);
  }
  return BSEC_OK;
}

static void mgos_bsec_timer_cb(void *arg) {
  s_state->bsec_timer_id = MGOS_INVALID_TIMER_ID;
  int delay_ms = 0;
  int ret = mgos_bme68x_run_once(&delay_ms);
  if (ret != BSEC_OK) {
    LOG(LL_ERROR, ("BSEC run failed: %d", ret));
    delay_ms = 10000;
  }
  const char *sf = s_state->cfg.bsec.state_file;
  if (sf != NULL && s_state->cfg.bsec.state_save_interval >= 0) {
    s_state->state_save_delay_ms += delay_ms;
    if (s_state->state_save_delay_ms / 1000 >=
        s_state->cfg.bsec.state_save_interval) {
      bsec_library_return_t ret = mgos_bsec_save_state_to_file(sf);
      if (ret == BSEC_OK) {
        LOG(LL_INFO, ("BSEC state saved (%s)", sf));
      } else {
        LOG(LL_INFO, ("Failed to save BSEC state (%s): %d", sf, ret));
      }
      s_state->state_save_delay_ms = 0;
    }
  }
  s_state->bsec_timer_id =
      mgos_set_timer(delay_ms, 0, mgos_bsec_timer_cb, NULL);
  (void) arg;
}

static float sr_from_str(const char *sr_str) {
  if (mgos_conf_str_empty(sr_str)) return BSEC_SAMPLE_RATE_DISABLED;
  if (strcmp(sr_str, "DIS") == 0) return BSEC_SAMPLE_RATE_DISABLED;
  if (strcmp(sr_str, "LP") == 0) return BSEC_SAMPLE_RATE_LP;
  if (strcmp(sr_str, "ULP") == 0) return BSEC_SAMPLE_RATE_ULP;
  return BSEC_SAMPLE_RATE_DISABLED;
}

bool mgos_bme68x_bsec_init(void) {
  bsec_version_t v;
  bsec_library_return_t ret;
  if (bsec_init() != BSEC_OK || bsec_get_version(&v) != BSEC_OK) {
    LOG(LL_ERROR, ("BSEC init failed"));
    return false;
  }
  LOG(LL_INFO, ("BSEC %d.%d.%d.%d initialized", v.major, v.minor,
                v.major_bugfix, v.minor_bugfix));
  const char *cf = s_state->cfg.bsec.config_file;
  if (cf != NULL) {
    ret = mgos_bsec_set_configuration_from_file(cf);
    if (ret == BSEC_OK) {
      LOG(LL_INFO, ("BSEC %s loaded (%s)", "config", cf));
    } else {
      LOG(LL_WARN, ("Failed to load BSEC %s from %s: %d, will use defaults",
                    "config", cf, ret));
    }
  }
  const char *sf = s_state->cfg.bsec.state_file;
  if (sf != NULL) {
    ret = mgos_bsec_set_state_from_file(sf);
    if (ret == BSEC_OK) {
      LOG(LL_INFO, ("BSEC %s loaded (%s)", "state", sf));
    } else {
      LOG(LL_WARN, ("Failed to load BSEC %s from %s: %d, will use defaults",
                    "state", sf, ret));
    }
  }

  float iaq_sr = sr_from_str(s_state->cfg.bsec.iaq_sample_rate);
  if ((ret = mgos_bsec_set_iaq_sample_rate(iaq_sr)) != BSEC_OK) {
    LOG(LL_ERROR, ("Failed to set %s sample rate: %d", "IAQ", ret));
    return false;
  }

  float temp_sr = sr_from_str(s_state->cfg.bsec.temp_sample_rate);
  if ((ret = mgos_bsec_set_temp_sample_rate(temp_sr)) != BSEC_OK) {
    LOG(LL_ERROR, ("Failed to set %s sample rate: %d", "temp", ret));
    return false;
  }

  float rh_sr = sr_from_str(s_state->cfg.bsec.rh_sample_rate);
  if ((ret = mgos_bsec_set_rh_sample_rate(rh_sr)) != BSEC_OK) {
    LOG(LL_ERROR, ("Failed to set %s sample rate: %d", "RH", ret));
    return false;
  }

  float ps_sr = sr_from_str(s_state->cfg.bsec.ps_sample_rate);
  if ((ret = mgos_bsec_set_ps_sample_rate(ps_sr)) != BSEC_OK) {
    LOG(LL_ERROR, ("Failed to set %s sample rate: %d", "pressure", ret));
    return false;
  }

  if (iaq_sr != BSEC_SAMPLE_RATE_DISABLED ||
      temp_sr != BSEC_SAMPLE_RATE_DISABLED ||
      rh_sr != BSEC_SAMPLE_RATE_DISABLED ||
      ps_sr != BSEC_SAMPLE_RATE_DISABLED) {
    mgos_bsec_start();
  }
  return true;
}

bool mgos_bme68x_init_cfg(const struct mgos_config_bme68x *cfg) {
  if (!cfg->enable) return false;

  s_state = (struct mgos_bme68x_state *) calloc(1, sizeof(*s_state));
  if (s_state == NULL) return false;
  s_state->prev_iaq_sr = BSEC_SAMPLE_RATE_DISABLED;
  s_state->cfg = *cfg;

  int8_t bme68x_status =
      mgos_bme68x_init_dev_i2c(&s_state->dev, cfg->i2c_bus, cfg->i2c_addr);

  LOG(LL_INFO, ("BME68x @ %d/0x%x init %s", cfg->i2c_bus, cfg->i2c_addr,
                (bme68x_status == BME68X_OK ? "ok" : "failed")));
  if (bme68x_status != BME68X_OK) return false;

  if (cfg->bsec.enable && !mgos_bme68x_bsec_init()) {
    return false;
  }

  return true;
}

// Mongoose OS library initialization
bool mgos_bme68x_init(void) {
  if (!mgos_sys_config_get_bme68x_enable()) return true;
  return mgos_bme68x_init_cfg(mgos_sys_config_get_bme68x());
}
