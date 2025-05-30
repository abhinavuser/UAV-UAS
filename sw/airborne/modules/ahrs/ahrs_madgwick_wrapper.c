/*
 * Copyright (C) 2020 Gautier Hattenberger <gautier.hattenberger@enac.fr>
 *
 * This file is part of paparazzi.
 *
 * paparazzi is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * paparazzi is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Paparazzi; see the file COPYING.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

/**
 * @file modules/ahrs/ahrs_madgwick_wrapper.c
 *
 * Paparazzi specific wrapper to run Madgwick ahrs filter.
 */

#include "modules/ahrs/ahrs_madgwick_wrapper.h"
#include "modules/ahrs/ahrs.h"
#include "modules/core/abi.h"
#include "mcu_periph/sys_time.h"
#include "message_pragmas.h"
#include "state.h"

PRINT_CONFIG_VAR(AHRS_MADGWICK_TYPE)

uint8_t ahrs_madgwick_enable;
/** last gyro msg timestamp */
static uint32_t ahrs_madgwick_last_stamp = 0;
static uint8_t ahrs_madgwick_id = AHRS_COMP_ID_MADGWICK;

static void compute_body_orientation_and_rates(void);

#if PERIODIC_TELEMETRY
#include "modules/datalink/telemetry.h"

static void send_att(struct transport_tx *trans, struct link_device *dev)
{
  /* compute eulers in int (IMU frame) */
  struct FloatEulers ltp_to_imu_euler;
  float_eulers_of_quat(&ltp_to_imu_euler, &ahrs_madgwick.quat);
  struct Int32Eulers eulers_imu;
  EULERS_BFP_OF_REAL(eulers_imu, ltp_to_imu_euler);

  /* compute Eulers in int (body frame) */
  struct FloatEulers ltp_to_body_euler;
  float_eulers_of_quat(&ltp_to_body_euler, &ahrs_madgwick.quat);
  struct Int32Eulers eulers_body;
  EULERS_BFP_OF_REAL(eulers_body, ltp_to_body_euler);

  pprz_msg_send_AHRS_EULER_INT(trans, dev, AC_ID,
                               &eulers_imu.phi,
                               &eulers_imu.theta,
                               &eulers_imu.psi,
                               &eulers_body.phi,
                               &eulers_body.theta,
                               &eulers_body.psi,
                               &ahrs_madgwick_id);
}

static void send_filter_status(struct transport_tx *trans, struct link_device *dev)
{
  uint8_t mde = 3;
  uint16_t val = 0;
  if (!ahrs_madgwick.is_aligned) { mde = 2; }
  uint32_t t_diff = get_sys_time_usec() - ahrs_madgwick_last_stamp;
  /* set lost if no new gyro measurements for 50ms */
  if (t_diff > 50000) { mde = 5; }
  pprz_msg_send_STATE_FILTER_STATUS(trans, dev, AC_ID, &ahrs_madgwick_id, &mde, &val);
}
#endif


/*
 * ABI bindings
 */
/** IMU (gyro, accel) */
#ifndef AHRS_MADGWICK_IMU_ID
#define AHRS_MADGWICK_IMU_ID ABI_BROADCAST
#endif
PRINT_CONFIG_VAR(AHRS_MADGWICK_IMU_ID)

/** magnetometer */
#ifndef AHRS_MADGWICK_MAG_ID
#define AHRS_MADGWICK_MAG_ID AHRS_MADGWICK_IMU_ID
#endif
PRINT_CONFIG_VAR(AHRS_MADGWICK_MAG_ID)

static abi_event gyro_ev;
static abi_event accel_ev;
static abi_event aligner_ev;

/**
 * Call ahrs_madgwick_propagate on new gyro measurements.
 * Since acceleration measurement is also needed for propagation,
 * use the last stored accel from #ahrs_madgwick_accel.
 */
static void gyro_cb(uint8_t sender_id __attribute__((unused)),
                   uint32_t stamp, struct Int32Rates *gyro)
{
  struct FloatRates gyro_f;
  RATES_FLOAT_OF_BFP(gyro_f, *gyro);

#if USE_AUTO_AHRS_FREQ || !defined(AHRS_PROPAGATE_FREQUENCY)
  PRINT_CONFIG_MSG("Calculating dt for AHRS Madgwick propagation.")
  /* timestamp in usec when last callback was received */
  static uint32_t last_stamp = 0;

  if (last_stamp > 0 && ahrs_madgwick.is_aligned) {
    float dt = (float)(stamp - last_stamp) * 1e-6;
    ahrs_madgwick_propagate(&gyro_f, dt);
    compute_body_orientation_and_rates();
  }
  last_stamp = stamp;
#else
  PRINT_CONFIG_MSG("Using fixed AHRS_PROPAGATE_FREQUENCY for AHRS Madgwick propagation.")
  PRINT_CONFIG_VAR(AHRS_PROPAGATE_FREQUENCY)
  const float dt = 1. / (AHRS_PROPAGATE_FREQUENCY);
  if (ahrs_madgwick.is_aligned) {
    ahrs_madgwick_propagate(&gyro_f, dt);
    compute_body_orientation_and_rates();
  }
#endif

  ahrs_madgwick_last_stamp = stamp;
}

static void accel_cb(uint8_t sender_id __attribute__((unused)),
                     uint32_t stamp __attribute__((unused)),
                     struct Int32Vect3 *accel)
{
  if (ahrs_madgwick.is_aligned) {
    struct FloatVect3 accel_f;
    ACCELS_FLOAT_OF_BFP(accel_f, *accel);
    ahrs_madgwick_update_accel(&accel_f);
  }
}

static void aligner_cb(uint8_t __attribute__((unused)) sender_id,
                       uint32_t stamp __attribute__((unused)),
                       struct Int32Rates *lp_gyro, struct Int32Vect3 *lp_accel,
                       struct Int32Vect3 *lp_mag __attribute__((unused)))
{
  if (!ahrs_madgwick.is_aligned) {
    /* convert to float */
    struct FloatRates gyro_f;
    RATES_FLOAT_OF_BFP(gyro_f, *lp_gyro);
    struct FloatVect3 accel_f;
    ACCELS_FLOAT_OF_BFP(accel_f, *lp_accel);
    ahrs_madgwick_align(&gyro_f, &accel_f);
    compute_body_orientation_and_rates();
  }
}

/**
 * Compute body orientation and rates from imu orientation and rates
 */
static void compute_body_orientation_and_rates(void)
{
  /* Set state */
  stateSetNedToBodyQuat_f(MODULE_AHRS_MADGWICK_ID, &ahrs_madgwick.quat);

  /* compute body rates */
  stateSetBodyRates_f(MODULE_AHRS_MADGWICK_ID, &ahrs_madgwick.rates);
}


void ahrs_madgwick_wrapper_init(void)
{
  ahrs_madgwick_init();
  if (AHRS_MADGWICK_TYPE == AHRS_PRIMARY) {
    ahrs_madgwick_wrapper_enable(1);
  } else {
    ahrs_madgwick_wrapper_enable(0);
  }

  /*
   * Subscribe to scaled IMU measurements and attach callbacks
   */
  AbiBindMsgIMU_GYRO(AHRS_MADGWICK_IMU_ID, &gyro_ev, gyro_cb);
  AbiBindMsgIMU_ACCEL(AHRS_MADGWICK_IMU_ID, &accel_ev, accel_cb);
  AbiBindMsgIMU_LOWPASSED(ABI_BROADCAST, &aligner_ev, aligner_cb);

#if PERIODIC_TELEMETRY
  register_periodic_telemetry(DefaultPeriodic, PPRZ_MSG_ID_AHRS_EULER_INT, send_att);
  register_periodic_telemetry(DefaultPeriodic, PPRZ_MSG_ID_STATE_FILTER_STATUS, send_filter_status);
#endif
}

void ahrs_madgwick_wrapper_enable(uint8_t enable)
{
  if (enable) {
    stateSetInputFilter(STATE_INPUT_ATTITUDE, MODULE_AHRS_MADGWICK_ID);
    stateSetInputFilter(STATE_INPUT_RATES, MODULE_AHRS_MADGWICK_ID);
  }
  ahrs_madgwick_enable = enable;
}

