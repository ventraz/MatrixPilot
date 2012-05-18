// This file is part of the MatrixPilotQuad firmware.
//
//    http://code.google.com/p/gentlenav/
//
// Copyright 2009-2011 MatrixPilot Team
// See the AUTHORS.TXT file for a list of authors of MatrixPilot.
//
// MatrixPilot is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// MatrixPilot is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with MatrixPilot.  If not, see <http://www.gnu.org/licenses/>.

#include "../libDCM/libDCM.h"

#define MANUAL_DEADBAND 100 // amount of throttle before fly-by-wire controls engage
#define MAXIMUM_ERROR_INTEGRAL ((long int) 32768000 )
#define YAW_DEADBAND 50 // prevent Tx pulse variation from causing yaw drift

extern int theta[3];
extern boolean didCalibrate;
extern void matrix_normalize(int[]);
extern void MatrixRotate(int[], int[]);
extern int commanded_tilt_gain;
extern int flight_mode;
int current_flight_mode = 0;

extern union longww IMUcmx;
extern union longww IMUcmy;
extern union longww IMUcmz;
int desired_posx, desired_posy;

// these are the current KP, KD and KDD loop gains in 2.14 fractional format
// valid range [0,3.99]
unsigned int pid_gains[4];

int roll_control;
int pitch_control;
int yaw_control;
int pitch_step;
struct relative2D matrix_accum;
extern boolean udb_throttle_enable;
unsigned int earth_yaw; // yaw with respect to earth frame
unsigned int desired_heading = 0;
int accel_feedback;
int theta_previous[2] = {0, 0};
int theta_delta[2];

int pwManual[5]; // channels 1-4 are control inputs from RX
int commanded_roll;
int commanded_pitch;
int commanded_yaw;

int roll_error;
int pitch_error;
int yaw_error;
int yaw_rate_error;

//int roll_error_previous = 0 ;
//int pitch_error_previous = 0 ;
int yaw_error_previous = 0;

union longww roll_error_integral = {0};
union longww pitch_error_integral = {0};
union longww yaw_error_integral = {0};

int pos_errorx, pos_errory;


int target_orientation[9] = {RMAX, 0, 0, 0, RMAX, 0, 0, 0, RMAX};

const int yaw_command_gain = ((long) MAX_YAW_RATE)*(0.03);

void motorCntrl(void)
{
    int temp;

    int min_throttle;

    int motor_A;
    int motor_B;
    int motor_C;
    int motor_D;

    int commanded_roll_body_frame;
    int commanded_pitch_body_frame;

    int commanded_tilt[3];

    union longww long_accum;
    //	union longww accum ; // debugging temporary

    // If radio is off, use udb_pwTrim values instead of the udb_pwIn values
    for (temp = 0; temp <= 4; temp++)
        if (udb_flags._.radio_on)
            pwManual[temp] = udb_pwIn[temp];
        else
            pwManual[temp] = udb_pwTrim[temp];


    if (!didCalibrate)
    {
        // some ESCs don't arm properly unless PWM signal is present at power-up
        ////		 Leave at 0 (no PWM pulses) until calibrated.
        //		udb_pwOut[MOTOR_A_OUTPUT_CHANNEL] = 0 ;
        //		udb_pwOut[MOTOR_B_OUTPUT_CHANNEL] = 0 ;
        //		udb_pwOut[MOTOR_C_OUTPUT_CHANNEL] = 0 ;
        //		udb_pwOut[MOTOR_D_OUTPUT_CHANNEL] = 0 ;
    }
    else if (abs(pwManual[THROTTLE_INPUT_CHANNEL] - udb_pwTrim[THROTTLE_INPUT_CHANNEL]) < MANUAL_DEADBAND)
    {

        motor_A = motor_B = motor_C = motor_D = pwManual[THROTTLE_INPUT_CHANNEL];

        VectorCopy(9, target_orientation, rmat);

        commanded_roll = (pwManual[ROLL_INPUT_CHANNEL]
                - udb_pwTrim[ROLL_INPUT_CHANNEL]);
        commanded_pitch = (pwManual[PITCH_INPUT_CHANNEL]
                - udb_pwTrim[PITCH_INPUT_CHANNEL]);
        commanded_yaw = (pwManual[YAW_INPUT_CHANNEL]
                - udb_pwTrim[YAW_INPUT_CHANNEL]);
        // get heading in earth frame from rmat
        matrix_accum.x = rmat[4];
        matrix_accum.y = rmat[1];
        earth_yaw = rect_to_polar16(&matrix_accum); // binary angle (0 - 65536 = 360 degrees)
        desired_heading = earth_yaw;

#ifdef CONFIG_PLUS
        commanded_pitch_body_frame = commanded_pitch;
        commanded_roll_body_frame = commanded_roll;
#endif

#ifdef CONFIG_X
        commanded_pitch_body_frame = 3 * ((commanded_pitch - commanded_roll) / 4); // approximation to .707, not critcal
        commanded_roll_body_frame = 3 * ((commanded_pitch + commanded_roll) / 4);
#endif

        motor_A += +commanded_yaw - commanded_pitch_body_frame;
        motor_B += -commanded_yaw - commanded_roll_body_frame;
        motor_C += +commanded_yaw + commanded_pitch_body_frame;
        motor_D += -commanded_yaw + commanded_roll_body_frame;

        udb_pwOut[MOTOR_A_OUTPUT_CHANNEL] = udb_servo_pulsesat(motor_A);
        udb_pwOut[MOTOR_B_OUTPUT_CHANNEL] = udb_servo_pulsesat(motor_B);
        udb_pwOut[MOTOR_C_OUTPUT_CHANNEL] = udb_servo_pulsesat(motor_C);
        udb_pwOut[MOTOR_D_OUTPUT_CHANNEL] = udb_servo_pulsesat(motor_D);

    }
    else
    {
        commanded_roll = (pwManual[ROLL_INPUT_CHANNEL]
                - udb_pwTrim[ROLL_INPUT_CHANNEL]) * commanded_tilt_gain;
        commanded_pitch = (pwManual[PITCH_INPUT_CHANNEL]
                - udb_pwTrim[PITCH_INPUT_CHANNEL]) * commanded_tilt_gain;
        commanded_yaw = (pwManual[YAW_INPUT_CHANNEL]
                - udb_pwTrim[YAW_INPUT_CHANNEL]);

        if (commanded_yaw >= YAW_DEADBAND)
        {
            commanded_yaw -= YAW_DEADBAND;
        }
        else if (commanded_yaw <= -YAW_DEADBAND)
        {
            commanded_yaw += YAW_DEADBAND;
        }
        else
        {
            commanded_yaw = 0;
        }

#if (ENABLE_FLIGHTMODE == 1)
        // check flight mode
        if (flight_mode != current_flight_mode)
        {
            // on change of flight mode, record current IMU position
            current_flight_mode = flight_mode;
            desired_posx = IMUcmx._.W1;
            desired_posy = IMUcmy._.W1;
        }
        // if flight mode is POS_MODE, command roll and pitch to reduce position error
        if (flight_mode == POS_MODE)
        {
            pos_errorx = IMUcmx._.W1 - desired_posx;
            pos_errory = IMUcmy._.W1 - desired_posy;
            commanded_roll += POS_ERROR_GAIN * pos_errory;
            commanded_pitch -= POS_ERROR_GAIN * pos_errorx;
            if (commanded_roll > 4000)
            {
                commanded_roll = 4000;
            }
            else if (commanded_roll < -4000)
            {
                commanded_roll = -4000;
            }
            if (commanded_pitch > 4000)
            {
                commanded_pitch = 4000;
            }
            else if (commanded_pitch < -4000)
            {
                commanded_pitch = -4000;
            }
        }
#endif

        //		adjust roll and pitch commands to prevent combined tilt from exceeding 90 degrees
        commanded_tilt[0] = commanded_roll;
        commanded_tilt[1] = commanded_pitch;
        commanded_tilt[2] = RMAX;
        vector3_normalize(commanded_tilt, commanded_tilt);
        commanded_roll = commanded_tilt[0];
        commanded_pitch = commanded_tilt[1];

#ifdef CONFIG_PLUS

        commanded_pitch_body_frame = commanded_pitch;
        commanded_roll_body_frame = commanded_roll;

#endif

#ifdef CONFIG_X

        commanded_pitch_body_frame = 3 * ((commanded_pitch - commanded_roll) / 4); // approximation to .707, not critcal
        commanded_roll_body_frame = 3 * ((commanded_pitch + commanded_roll) / 4);

#endif

        //		Compute orientation errors
        roll_error = commanded_roll_body_frame + rmat[6];
        pitch_error = commanded_pitch_body_frame - rmat[7];

        // get heading in earth frame from rmat
        matrix_accum.x = rmat[4];
        matrix_accum.y = rmat[1];
        earth_yaw = rect_to_polar16(&matrix_accum); // binary angle (0 - 65536 = 360 degrees)

        // if commanded_yaw is nonzero, reset desired_heading to current heading
        // (otherwise, hold last commanded heading)
        //WTF: !!! if (commanded_yaw != 0) didn't behave as expected !!!
        if (abs(commanded_yaw) > YAW_DEADBAND)
        {
            desired_heading = earth_yaw;
        }
        // positive commanded_yaw causes positive yaw_control and decrease in earth_yaw
        // If earth_yaw increases, positive command is required to correct.
        // since desired_heading and earth_yaw are "word circular"
        // their difference is the 2's complement signed integer heading error
        // 180 degrees is 2^15; 1 degree is 182 counts; yaw_command is +/-1000
        // Add commanded yaw to heading error; yaw rate is then proportional
        // to the sum of heading error and yaw command. Full stick is equivalent
        // to a heading error of about 8 degrees
        yaw_error = (int) (earth_yaw - desired_heading);
        yaw_error += 10 * commanded_yaw;

        //		Compute the signals that are common to all 4 motors
        min_throttle = udb_pwTrim[THROTTLE_INPUT_CHANNEL];
        long_accum.WW = __builtin_mulus((unsigned int) (RMAX * ACCEL_K), accelEarth[2]);
        accel_feedback = long_accum._.W1;
        motor_A = motor_B = motor_C = motor_D = pwManual[THROTTLE_INPUT_CHANNEL] - accel_feedback;


        //		Compute the error integrals
        roll_error_integral.WW += ((__builtin_mulus((unsigned int) (256.0 * RMAX * TILT_KI / ((double) PID_HZ)), roll_error)) >> 8);
        if (roll_error_integral.WW > MAXIMUM_ERROR_INTEGRAL)
        {
            roll_error_integral.WW = MAXIMUM_ERROR_INTEGRAL;
        }
        if (roll_error_integral.WW < -MAXIMUM_ERROR_INTEGRAL)
        {
            roll_error_integral.WW = -MAXIMUM_ERROR_INTEGRAL;
        }

        pitch_error_integral.WW += ((__builtin_mulus((unsigned int) (256.0 * RMAX * TILT_KI / ((double) PID_HZ)), pitch_error)) >> 8);
        if (pitch_error_integral.WW > MAXIMUM_ERROR_INTEGRAL)
        {
            pitch_error_integral.WW = MAXIMUM_ERROR_INTEGRAL;
        }
        if (pitch_error_integral.WW < -MAXIMUM_ERROR_INTEGRAL)
        {
            pitch_error_integral.WW = -MAXIMUM_ERROR_INTEGRAL;
        }

        yaw_error_integral.WW += ((__builtin_mulus((unsigned int) (256.0 * RMAX * YAW_KI / ((double) PID_HZ)), yaw_error)) >> 8);
        if (yaw_error_integral.WW > MAXIMUM_ERROR_INTEGRAL)
        {
            yaw_error_integral.WW = MAXIMUM_ERROR_INTEGRAL;
        }
        if (yaw_error_integral.WW < -MAXIMUM_ERROR_INTEGRAL)
        {
            yaw_error_integral.WW = -MAXIMUM_ERROR_INTEGRAL;
        }

        //		Compute the derivatives
        theta_delta[0] = theta[0] - theta_previous[0];
        theta_delta[1] = theta[1] - theta_previous[1];

        // use tilt error as desired rate, with gain pid_gains[0]
        long_accum.WW = __builtin_mulus(pid_gains[0], roll_error);
        int roll_rate_error = long_accum._.W1 - (theta[1] << 2);
        roll_rate_error += roll_error_integral._.W1;

        long_accum.WW = __builtin_mulus(pid_gains[1], roll_rate_error);
        roll_control = long_accum._.W1;

        long_accum.WW = __builtin_mulus(pid_gains[2], -theta_delta[1]) << 2;
        roll_control += long_accum._.W1;

        // use tilt error as desired rate, with gain pid_gains[0]
        long_accum.WW = __builtin_mulus(pid_gains[0], pitch_error);
        int pitch_rate_error = long_accum._.W1 - (theta[0] << 2);
        pitch_rate_error += pitch_error_integral._.W1;

        long_accum.WW = __builtin_mulus(pid_gains[1], pitch_rate_error);
        pitch_control = long_accum._.W1;

        long_accum.WW = __builtin_mulus(pid_gains[2], -theta_delta[0]) << 2;
        pitch_control += long_accum._.W1;

        // use heading error * KP as desired yaw rate
        long_accum.WW = __builtin_mulus((unsigned int) (RMAX * YAW_KP), yaw_error);
        yaw_rate_error = long_accum._.W1 - (theta[2] << 2);
        yaw_rate_error += yaw_error_integral._.W1;

        long_accum.WW = __builtin_mulus((unsigned int) (RMAX * YAW_KD), yaw_rate_error);
        yaw_control = long_accum._.W1;

        // limit yaw control input to prevent loss of tilt control
        if (yaw_control > 200)
        {
            yaw_control = 200;
        }
        else if (yaw_control < -200)
        {
            yaw_control = -200;
        }

        //		Mix in the yaw, pitch, and roll signals into the motors
        motor_A += +yaw_control - pitch_control;
        motor_B += -yaw_control - roll_control;
        motor_C += +yaw_control + pitch_control;
        motor_D += -yaw_control + roll_control;

        //		Send the signals out to the motors
        udb_pwOut[MOTOR_A_OUTPUT_CHANNEL] = udb_servo_pulsesat(motor_A);
        udb_pwOut[MOTOR_B_OUTPUT_CHANNEL] = udb_servo_pulsesat(motor_B);
        udb_pwOut[MOTOR_C_OUTPUT_CHANNEL] = udb_servo_pulsesat(motor_C);
        udb_pwOut[MOTOR_D_OUTPUT_CHANNEL] = udb_servo_pulsesat(motor_D);

    }
}


#ifndef CONFIG_PLUS
#ifndef CONFIG_X
#error ("You have not selected a configuration in options.h, select either CONFIG_PLUS or CONFIG_X.")
#endif
#endif

#ifdef CONFIG_PLUS
#ifdef CONFIG_X
#error ("You have selected both CONFIG_PLUS and CONFIG_X in options.h. Select just one of them."
#endif
#endif


#if  (( ( int ) + MAX_YAW_RATE   < 50 ) || ( ( int ) + MAX_YAW_RATE > 500 ))
#error ("MAX_YAW_RATE must be between 50.0 and 500.0 degrees/second.")
#endif

#if (((int) + MAX_TILT) > 45)
#error ("MAX_TILT mus be less than or equal to 45 degrees."
#endif