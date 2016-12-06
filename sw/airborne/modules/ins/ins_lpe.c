/*
 * Copyright (C) 2016 Michal Podhradsky <michal.pohradsky@aggiemail.usu.edu>
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
 * @file modules/ins/ins_lpe.c
 *
 * Local Position Estimator
 */
#include "modules/ins/ins_lpe.h"
#include "subsystems/abi.h"
#include "mcu_periph/sys_time.h"
#include "message_pragmas.h"
#include "subsystems/imu.h"

#ifndef USE_INS_NAV_INIT
#define USE_INS_NAV_INIT TRUE
PRINT_CONFIG_MSG("USE_INS_NAV_INIT defaulting to TRUE")
#endif

/*
 * ABI bindings
 */
/** baro */
#ifndef INS_LPE_BARO_ID
#if USE_BARO_BOARD
#define INS_LPE_BARO_ID BARO_BOARD_SENDER_ID
#else
#define INS_LPE_BARO_ID ABI_BROADCAST
#endif
#endif
PRINT_CONFIG_VAR(INS_LPE_BARO_ID)
abi_event ins_lpe_baro_ev;
static void baro_cb(uint8_t sender_id, float pressure);


/** IMU (accel, body_to_imu) */
#ifndef INS_LPE_IMU_ID
#define INS_LPE_IMU_ID ABI_BROADCAST
#endif
PRINT_CONFIG_VAR(INS_LPE_IMU_ID)
static abi_event accel_ev;
static void accel_cb(uint8_t sender_id, uint32_t stamp, struct Int32Vect3 *accel);


/** ABI binding for GPS data.
 * Used for GPS ABI messages.
 */
#ifndef INS_LPE_GPS_ID
#define INS_LPE_GPS_ID GPS_MULTI_ID
#endif
PRINT_CONFIG_VAR(INS_LPE_GPS_ID)
static abi_event gps_ev;
static void gps_cb(uint8_t sender_id, uint32_t stamp, struct GpsState *gps_s);


/** ABI binding for VELOCITY_ESTIMATE.
 * Usually this is coming from opticflow.
 */
#ifndef INS_LPE_VEL_ID
#define INS_LPE_VEL_ID ABI_BROADCAST
#endif
PRINT_CONFIG_VAR(INS_LPE_VEL_ID)
static abi_event vel_est_ev;
static void vel_est_cb(uint8_t sender_id, uint32_t stamp, float x, float y, float z, float noise);


/** ABI binding for Lidar data.
 * Used for AGL ABI message with LIDAR ID
 */
#ifndef INS_LPE_LIDAR_ID
#define INS_LPE_LIDAR_ID ABI_BROADCAST // TODO: trigger a warning?
#endif
PRINT_CONFIG_VAR(INS_LPE_LIDAR_ID)
abi_event lidar_ev;
static void lidar_cb(uint8_t sender_id, float distance);


/** ABI binding for Sonar data.
 * Used for AGL ABI message with SONAR ID
 */
#ifndef INS_LPE_SONAR_ID
#define INS_LPE_SONAR_ID ABI_BROADCAST
#endif
PRINT_CONFIG_VAR(INS_LPE_SONAR_ID)
abi_event sonar_ev;
static void sonar_cb(uint8_t sender_id, float distance);


// Ins struct
struct InsLpe ins_lpe;

/**
 * Set initial values for A and B
 */
void ins_lpe_init_states(void)
{
  ins_lpe.A.matrix[X_x][X_vx] = 1; // derivative of position is velocity
  ins_lpe.A.matrix[X_y][X_vy] = 1; // derivative of position is velocity
  ins_lpe.A.matrix[X_z][X_vz] = 1; // derivative of position is velocity

  ins_lpe.B.matrix[X_vx][U_ax] = 1;
  ins_lpe.B.matrix[X_vy][U_ay] = 1;
  ins_lpe.B.matrix[X_vz][U_az] = 1;
}


/**
 *  Update A with new values from rotational matrix
 */
void ins_lpe_update_states(void)
{
  // R -> NED to Body
  // R^T -> body to NED
  // we need R^T
  struct FloatRMat* R = stateGetNedToBodyRMat_f();
  struct FloatRMat RT;
  // transpose
  float_rmat_inv(&RT, R);

  // derivative of velocity is accelerometer acceleration
  // (in input matrix) - bias (in body frame)
  ins_lpe.A.matrix[X_vx][X_bx] = -RMAT_ELMT(RT, 0, 0);
  ins_lpe.A.matrix[X_vx][X_by] = -RMAT_ELMT(RT, 0, 1);
  ins_lpe.A.matrix[X_vx][X_bz] = -RMAT_ELMT(RT, 0, 2);

  ins_lpe.A.matrix[X_vy][X_bx] = -RMAT_ELMT(RT, 1, 0);
  ins_lpe.A.matrix[X_vy][X_by] = -RMAT_ELMT(RT, 1, 1);
  ins_lpe.A.matrix[X_vy][X_bz] = -RMAT_ELMT(RT, 1, 2);

  ins_lpe.A.matrix[X_vz][X_bx] = -RMAT_ELMT(RT, 2, 0);
  ins_lpe.A.matrix[X_vz][X_by] = -RMAT_ELMT(RT, 2, 1);
  ins_lpe.A.matrix[X_vz][X_bz] = -RMAT_ELMT(RT, 2, 2);
}


/**
 * Fill in R and Q matrices
 */
void ins_lpe_update_params(void)
{
  // input noise covariance matrix
  ins_lpe.R.matrix[U_ax][U_ax] = ins_lpe.accel_xy_stddev * ins_lpe.accel_xy_stddev;
  ins_lpe.R.matrix[U_ay][U_ay] = ins_lpe.accel_xy_stddev * ins_lpe.accel_xy_stddev;
  ins_lpe.R.matrix[U_az][U_az] = ins_lpe.accel_z_stddev * ins_lpe.accel_z_stddev;

  // process noise power matrix
  float pn_p_sq = ins_lpe.pn_p_noise_density * ins_lpe.pn_p_noise_density;
  float pn_v_sq = ins_lpe.pn_v_noise_density * ins_lpe.pn_v_noise_density;
  ins_lpe.Q.matrix[X_x][X_x] = pn_p_sq;
  ins_lpe.Q.matrix[X_y][X_y] = pn_p_sq;
  ins_lpe.Q.matrix[X_z][X_z] = pn_p_sq;
  ins_lpe.Q.matrix[X_vx][X_vx] = pn_v_sq;
  ins_lpe.Q.matrix[X_vy][X_vy] = pn_v_sq;
  ins_lpe.Q.matrix[X_vz][X_vz] = pn_v_sq;

  // technically, the noise is in the body frame,
  // but the components are all the same, so
  // ignoring for now
  float pn_b_sq = ins_lpe.pn_b_noise_density * ins_lpe.pn_b_noise_density;
  ins_lpe.Q.matrix[X_bx][X_bx] = pn_b_sq;
  ins_lpe.Q.matrix[X_by][X_by] = pn_b_sq;
  ins_lpe.Q.matrix[X_bz][X_bz] = pn_b_sq;
}


/**
 * Reset states (matrices and vectors) back to
 * their intial value
 */
void ins_lpe_reset_states(void)
{
    // initialize process matrix A 9x9
    ins_lpe.A.initialized = FALSE; // not initialized yet
    ins_lpe.A.rows = n_x; // set n_rows
    ins_lpe.A.cols = n_x; // set n_cols
    POPULATE_MATRIX_PTR(ins_lpe.A_data_ptr, ins_lpe.A_data, ins_lpe.A.rows); // populate the matrix row pointer
    ins_lpe.A.matrix = &ins_lpe.A_data_ptr[0]; // set the matrix pointer
    float_mat_zero(ins_lpe.A.matrix, ins_lpe.A.rows, ins_lpe.A.cols); // initialize matrix to zero
    ins_lpe.A.initialized = TRUE; // matrix is ready to be used

    // initialize input matrix B 9x3
    ins_lpe.B.initialized = FALSE; // not initialized yet
    ins_lpe.B.rows = n_x; // set n_rows
    ins_lpe.B.cols = n_u; // set n_cols
    POPULATE_MATRIX_PTR(ins_lpe.B_data_ptr, ins_lpe.B_data, ins_lpe.B.rows); // populate the matrix row pointer
    ins_lpe.B.matrix = &ins_lpe.B_data_ptr[0]; // set the matrix pointer
    float_mat_zero(ins_lpe.B.matrix, ins_lpe.B.rows, ins_lpe.B.cols); // initialize matrix to zero
    ins_lpe.B.initialized = TRUE; // matrix is ready to be used

    // initialize measurement noise covariance matrix R 3x3
    ins_lpe.R.initialized = FALSE; // not initialized yet
    ins_lpe.R.rows = n_u; // set n_rows
    ins_lpe.R.cols = n_u; // set n_cols
    POPULATE_MATRIX_PTR(ins_lpe.R_data_ptr, ins_lpe.R_data, ins_lpe.R.rows); // populate the matrix row pointer
    ins_lpe.R.matrix = &ins_lpe.R_data_ptr[0]; // set the matrix pointer
    float_mat_zero(ins_lpe.R.matrix, ins_lpe.R.rows, ins_lpe.R.cols); // initialize matrix to zero
    ins_lpe.R.initialized = TRUE; // matrix is ready to be used

    // initialize process noise covariance Q 9x9
    ins_lpe.Q.initialized = FALSE; // not initialized yet
    ins_lpe.Q.rows = n_x; // set n_rows
    ins_lpe.Q.cols = n_x; // set n_cols
    POPULATE_MATRIX_PTR(ins_lpe.Q_data_ptr, ins_lpe.Q_data, ins_lpe.Q.rows); // populate the matrix row pointer
    ins_lpe.Q.matrix = &ins_lpe.Q_data_ptr[0]; // set the matrix pointer
    float_mat_zero(ins_lpe.Q.matrix, ins_lpe.Q.rows, ins_lpe.Q.cols); // initialize matrix to zero
    ins_lpe.Q.initialized = TRUE; // matrix is ready to be used

    // initialize estimate error covariance P 9x9
    ins_lpe.P.initialized = FALSE; // not initialized yet
    ins_lpe.P.rows = n_x; // set n_rows
    ins_lpe.P.cols = n_x; // set n_cols
    POPULATE_MATRIX_PTR(ins_lpe.P_data_ptr, ins_lpe.P_data, ins_lpe.P.rows); // populate the matrix row pointer
    ins_lpe.P.matrix = &ins_lpe.P_data_ptr[0]; // set the matrix pointer
    float_mat_zero(ins_lpe.P.matrix, ins_lpe.P.rows, ins_lpe.P.cols); // initialize matrix to zero
    ins_lpe.P.initialized = TRUE; // matrix is ready to be used

    // initialize state vector 9x1
    memset(ins_lpe.x, 0, sizeof(ins_lpe.x));

    // initialize input vector 3x1
    memset(ins_lpe.u, 0, sizeof(ins_lpe.u));
}

/**
 * Init function
 * - Initialize variables, populate Kalman filter
 * - Bind ABI messages
 * - Initialize coordinate system
 * - Bind telemetry messages
 */
void ins_lpe_init(void)
{
  ins_lpe.initialized = FALSE;

  if (!ins_lpe.initialized)
  {
    ins_lpe_reset_states(); // reset all matrices to zero
    ins_lpe_init_states(); // initialize A and B with constants
    ins_lpe_update_states(); // set A with Rmat values
    ins_lpe_update_params(); // set R and Q with noise values
    ins_lpe.initialized = TRUE;
  }

  /*
   * Subscribe to scaled IMU measurements and attach callbacks
   */
  AbiBindMsgIMU_ACCEL_INT32(INS_LPE_IMU_ID, &accel_ev, accel_cb); // accel
  AbiBindMsgGPS(INS_LPE_GPS_ID, &gps_ev, gps_cb); // GPS
  AbiBindMsgVELOCITY_ESTIMATE(INS_LPE_VEL_ID, &vel_est_ev, vel_est_cb); // Optical flow
  AbiBindMsgAGL(INS_LPE_SONAR_ID, &sonar_ev, sonar_cb); // sonar
  AbiBindMsgAGL(INS_LPE_LIDAR_ID, &lidar_ev, lidar_cb); // lidar
  AbiBindMsgBARO_ABS(INS_LPE_BARO_ID, &ins_lpe_baro_ev, baro_cb); // baro
}


/**
 * Periodic function
 * - perform checks (std, errors, reset etc...)
 * - check which sensors are available
 * - check for NaN in matrices
 * - predict
 *   - accel input
 *   - correction logic (bias)
 *   - propagate
 * - update for each sensor
 * - final sanity checks & publish results / update state interface
 */
void ins_lpe_periodic(void)
{

}


/**
 *  Baro callback
 *  Copy the pressure and timestamp
 *  Set the baro new data flag
 */
static void baro_cb(uint8_t __attribute__((unused)) sender_id, float pressure)
{

}


/**
 * Sonar callback
 * Copy the distance and the timestamp
 * Set the sonar new data flag
 */
static void sonar_cb(uint8_t __attribute__((unused)) sender_id, float distance)
{
  // AGL message doesn't provide timestamp, so use current time
  ins_lpe.sonar.timestamp = get_sys_time_usec();

  // save (derotated and properly oriented value) and remove offset
  ins_lpe.sonar.agl = distance - ins_lpe.sonar.offset;

  // correct for min/max
  if (ins_lpe.sonar.agl > ins_lpe.sonar.max_distance) {
    ins_lpe.sonar.agl = ins_lpe.sonar.max_distance;
  }
  if (ins_lpe.sonar.agl < ins_lpe.sonar.min_distance) {
    ins_lpe.sonar.agl = ins_lpe.sonar.min_distance;
  }

  // update flag
  ins_lpe.sonar.data_available = TRUE;
}


/**
 * Lidar callback
 * Copy the distance and the timestamp
 * Set the lidar new data flag
 */
static void lidar_cb(uint8_t __attribute__((unused)) sender_id, float distance)
{
  // AGL message doesn't provide timestamp, so use current time
  ins_lpe.lidar.timestamp = get_sys_time_usec();

  // save (derotated and properly oriented value) and remove offset
  ins_lpe.lidar.agl = distance - ins_lpe.lidar.offset;

  // correct for min/max
  if (ins_lpe.lidar.agl > ins_lpe.lidar.max_distance) {
    ins_lpe.lidar.agl = ins_lpe.lidar.max_distance;
  }
  if (ins_lpe.lidar.agl < ins_lpe.lidar.min_distance) {
    ins_lpe.lidar.agl = ins_lpe.lidar.min_distance;
  }

  // update flag
  ins_lpe.lidar.data_available = TRUE;
}


/**
 * Accel callback
 * Copy the accel and the timestamp
 * Make sure accel is in body frame / NED frame
 * Set the new accel data flag
 */
static void accel_cb(uint8_t sender_id __attribute__((unused)),
                     uint32_t stamp, struct Int32Vect3 *accel)
{
  // get timestamp [us]
  ins_lpe.accel.timestamp = stamp;

  // derotate
  struct Int32RMat *body_to_imu_rmat = orientationGetRMat_i(&imu.body_to_imu);
  int32_rmat_transp_vmult(&ins_lpe.accel.accel_meas_body, body_to_imu_rmat, accel);
  // stateSetAccelBody_i(&ins_lpe.accel.accel_meas_body); // if we use only one INS
  int32_rmat_transp_vmult(&ins_lpe.accel.accel_meas_ltp, stateGetNedToBodyRMat_i(), &ins_lpe.accel.accel_meas_body);

  // update flag
  ins_lpe.accel.data_available = TRUE;
}



/**
 * GPS callback
 * Copy the lat/long/alt and the timestamp
 * Set the new GPS data flag
 */
static void gps_cb(uint8_t sender_id __attribute__((unused)),
                   uint32_t stamp __attribute__((unused)),
                   struct GpsState *gps_s)
{

}


/**
 * Optical flow callback
 * Copy the flow estimates and the timestamp
 * Set the new optical flow data flag
 */
static void vel_est_cb(uint8_t sender_id __attribute__((unused)),
                       uint32_t stamp,
                       float x, float y, float z,
                       float noise __attribute__((unused)))
{

}

