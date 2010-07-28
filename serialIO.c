// This file is part of MatrixPilot.
//
//    http://code.google.com/p/gentlenav/
//
// Copyright 2009, 2010 MatrixPilot Team
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


#include "p30f4011.h"
#include "defines.h"
#include "definesRmat.h"

#define _ADDED_C_LIB 1 // Needed to get vsnprintf()
#include <stdio.h>
#include <stdarg.h>


union intbb voltage_milis = {0} ;
union intbb voltage_temp ;

void sio_newMsg(unsigned char);
void sio_voltage_low( unsigned char inchar ) ;
void sio_voltage_high( unsigned char inchar ) ;
void (* sio_parse ) ( unsigned char inchar ) = &sio_newMsg ;


#define SERIAL_BUFFER_SIZE 256
char serial_buffer[SERIAL_BUFFER_SIZE] ;
int sb_index = 0 ;
int end_index = 0 ;


////////////////////////////////////////////////////////////////////////////////
// 
// Initialization
//

void init_USART1(void)
{	
//	debugging/telemetry USART, runs at 19200 baud
	U1MODE = 0b0010000000000000 ; // turn off RX, used to clear errors
	U1STA  = 0b0000010100010000 ;

//Baud Rate = FCY / (16*(BRG+1))
#if ( SERIAL_OUTPUT_FORMAT == SERIAL_OSD_REMZIBI )
	// For the Remzibi OSD, the 'extra' serial port is set to 9600 baud
	U1BRG =  25 ; // 9600 baud
#else
	// Otherwise, the baud rate is set as specified below
//	U1BRG =  51 ; // 4800 baud
//	U1BRG =  25 ; // 9600 baud
	U1BRG =  12 ; // 19200 baud
#endif

	U1MODEbits.UARTEN = 1 ; // turn on uart
	U1MODEbits.ALTIO = 1 ; // alternate pins
	
	U1STAbits.UTXEN = 1 ; // turn on transmitter

	IFS0bits.U1RXIF = 0 ; // clear the interrupt
	IPC2bits.U1RXIP = 3 ; // priority 3
	IEC0bits.U1RXIE = 1 ; // turn on the interrupt

	IFS0bits.U1TXIF = 0 ; // clear the interrupt 
 	IPC2bits.U1TXIP = 4 ; // priority 4 
 	IEC0bits.U1TXIE = 1 ; // turn on the interrupt
	
	return ;
}


////////////////////////////////////////////////////////////////////////////////
// 
// Receive Serial Commands
//

void __attribute__((__interrupt__,__no_auto_psv__)) _U1RXInterrupt(void)
{
	// interrupt_save_extended_state ;
	
	indicate_loading_inter ;
	
	char rxchar = U1RXREG ;
	if ( U1STAbits.FERR ) {  init_USART1(); }
	else if ( U1STAbits.OERR ) {  init_USART1(); }
	
	IFS0bits.U1RXIF = 0 ; // clear the interrupt
	(* sio_parse) ( rxchar ) ; // parse the input byte

	// interrupt_restore_extended_state ;
	return ;
}


void sio_newMsg( unsigned char inchar )
{
	if ( inchar == 'V' )
	{
		sio_parse = &sio_voltage_high ;
	}
	else
	{
		// error ?
	}
	return ;
}


void sio_voltage_low( unsigned char inchar )
{
	voltage_temp._.B0 = inchar ;
	voltage_temp.BB = voltage_temp.BB * 2 ; // convert to voltage
	voltage_milis.BB = voltage_temp.BB ;
	sio_parse = &sio_newMsg ;
	return ;
}


void sio_voltage_high( unsigned char inchar )
{
	voltage_temp.BB = 0 ; // initialize our temp variable
	voltage_temp._.B1 = inchar ;
	sio_parse = &sio_voltage_low ;
	return ;
}


////////////////////////////////////////////////////////////////////////////////
// 
// Output Serial Data
//

// add this text to the output buffer
void serial_output( char* format, ... )
{
	va_list arglist ;
	
	va_start(arglist, format) ;
	
	int start_index = end_index ;
	int remaining = SERIAL_BUFFER_SIZE - start_index ;
	
	if (remaining > 1)
	{
		int wrote = vsnprintf( (char*)(&serial_buffer[start_index]), (size_t)remaining, format, arglist) ;
		end_index = start_index + wrote;
	}
	
	if (sb_index == 0)
	{
		IFS0bits.U1TXIF = 1 ; // trigger the TX interrupt
	}
	
	va_end(arglist);
	
	return ;
}


void __attribute__((__interrupt__,__no_auto_psv__)) _U1TXInterrupt(void)
{
	interrupt_save_extended_state ;
	
	indicate_loading_inter ;
	
	unsigned char txchar ;
	IFS0bits.U1TXIF = 0 ; // clear the interrupt 
	txchar = serial_buffer[ sb_index++ ] ;
	if ( txchar )
	{
		U1TXREG = txchar ;
	}
	else
	{
		sb_index = 0 ;
		end_index = 0 ;
	}
	
	interrupt_restore_extended_state ;
	return ;
}


#if ( SERIAL_OUTPUT_FORMAT == SERIAL_DEBUG )

void serial_output_4hz( void )
{
	serial_output("lat: %li, long: %li, alt: %li\r\nrmat: %i, %i, %i, %i, %i, %i, %i, %i, %i\r\n" ,
		lat_gps.WW , long_gps.WW , alt_sl_gps.WW ,
		rmat[0] , rmat[1] , rmat[2] ,
		rmat[3] , rmat[4] , rmat[5] ,
		rmat[6] , rmat[7] , rmat[8]  ) ;
	return ;
}


#elif ( SERIAL_OUTPUT_FORMAT == SERIAL_ARDUSTATION )

#define BYTECIR_TO_DEGREE 92160		// (360.0/256 * 2^16)
int skip = 0 ;

extern int desiredHeight, waypointIndex ;
extern signed char desired_dir_waypoint ;

void serial_output_4hz( void )
{
	unsigned int mode ;
	struct relative2D matrix_accum ;
	union longbbbb accum ;
	int desired_dir_waypoint_deg ;  // desired_dir_waypoint converted to a bearing (0-360)
	
	long earth_pitch ;		// pitch in binary angles ( 0-255 is 360 degreres)
	long earth_roll ;		// roll of the plane with respect to earth frame
	//long earth_yaw ;		// yaw with respect to earth frame
	
	accum.WW  = ( desired_dir_waypoint * BYTECIR_TO_DEGREE ) + 32768 ;
	desired_dir_waypoint_deg  = accum._.W1 - 90 ; // "Convert UAV DevBoad Earth" to Compass Bearing
	if ( desired_dir_waypoint_deg < 0 ) desired_dir_waypoint_deg += 360 ; 

	if (flags._.GPS_steering == 0 && flags._.pitch_feedback == 0)
		mode = 1 ;
	else if (flags._.GPS_steering == 0 && flags._.pitch_feedback == 1)
		mode = 2 ;
	else if (flags._.GPS_steering == 1 && flags._.pitch_feedback == 1 && flags._.radio_on == 1)
		mode = 3 ;
	else if (flags._.GPS_steering == 1 && flags._.pitch_feedback == 1 && flags._.radio_on == 0)
		mode = 0 ;
	else
		mode = 99 ; // Unknown
	
	//  Roll
	//  Earth Frame of Reference
	matrix_accum.x = rmat[8] ;
	matrix_accum.y = rmat[6] ;
	earth_roll = rect_to_polar(&matrix_accum) ;					// binary angle (0 - 256 = 360 degrees)
	earth_roll = (-earth_roll * BYTECIR_TO_DEGREE) >> 16 ;		// switch polarity, convert to -180 - 180 degrees
	
	//  Pitch
	//  Earth Frame of Reference
	//  Note that we are using the matrix_accum.x
	//  left over from previous rect_to_polar in this calculation.
	//  so this Pitch calculation must follow the Roll calculation
	matrix_accum.y = rmat[7] ;
	earth_pitch = rect_to_polar(&matrix_accum) ;				// binary angle (0 - 256 = 360 degrees)
	earth_pitch = (-earth_pitch * BYTECIR_TO_DEGREE) >> 16 ;	// switch polarity, convert to -180 - 180 degrees
	
	// Yaw
	// Earth Frame of Reference
	// Ardustation does not use yaw in degrees
	// matrix_accum.x = rmat[4] ;
	// matrix_accum.y = rmat[1] ;
	// earth_yaw = rect_to_polar(&matrix_accum) ;				// binary angle (0 - 256 = 360 degrees)
	// earth_yaw = (earth_yaw * BYTECIR_TO_DEGREE) >> 16 ;		// switch polarity, convert to -180 - 180 degrees
	
	
	// The Ardupilot GroundStation protocol is mostly documented here:
	//    http://diydrones.com/profiles/blogs/ardupilot-telemetry-protocol
	
	if (++skip < 4)
	{
		serial_output("+++THH:%i,RLL:%li,PCH:%li,STT:%i,***\r\n",
			(int)((pwOut[THROTTLE_OUTPUT_CHANNEL] - pwTrim[THROTTLE_OUTPUT_CHANNEL])/20),
			earth_roll, earth_pitch,
			mode
		) ;
	}
	else
	{
		serial_output("!!!LAT:%li,LON:%li,SPD:%.2f,CRT:%.2f,ALT:%li,ALH:%i,CRS:%.2f,BER:%i,WPN:%i,DST:%i,BTV:%.2f***\r\n"
					  "+++THH:%i,RLL:%li,PCH:%li,STT:%i,***\r\n",
			lat_gps.WW / 10 , long_gps.WW / 10 , (float)(sog_gps.BB / 100.0), (float)(climb_gps.BB / 100.0),
			(alt_sl_gps.WW - alt_origin.WW) / 100, desiredHeight, (float)(cog_gps.BB / 100.0), desired_dir_waypoint_deg,
			waypointIndex, tofinish_line, (float)(voltage_milis.BB / 100.0), 
			(int)((pwOut[THROTTLE_OUTPUT_CHANNEL] - pwTrim[THROTTLE_OUTPUT_CHANNEL])/20),
			earth_roll, earth_pitch,
			mode
		) ;
		skip = 0 ;
	}
	
	return ;
}


#elif ( SERIAL_OUTPUT_FORMAT == SERIAL_UDB || SERIAL_OUTPUT_FORMAT == SERIAL_UDB_EXTRA )

int telemetry_counter = 6 ;
int skip = 0 ;

#if ( SERIAL_OUTPUT_FORMAT == SERIAL_UDB_EXTRA )
int pwIn_save[MAX_INPUTS + 1] ;
int pwOut_save[MAX_OUTPUTS + 1] ;
char print_choice = 0 ;
#endif


extern int waypointIndex, air_speed_magnitude ;
extern int magFieldEarth[3] ;
extern int locationErrorEarth[3] ;

#if ( SERIAL_OUTPUT_FORMAT == SERIAL_UDB )
void serial_output_4hz( void )
#elif ( SERIAL_OUTPUT_FORMAT == SERIAL_UDB_EXTRA )
void serial_output_8hz( void )
#endif
{
	union longbbbb accum ;
	int i;

#if ( SERIAL_OUTPUT_FORMAT == SERIAL_UDB )

	// Only run through this function once per second, by skipping all but every N runs through it.
	// Saves CPU and XBee power.
	if (++skip < 4) return ;

#elif ( SERIAL_OUTPUT_FORMAT == SERIAL_UDB_EXTRA )
	// SERIAL_UDB_EXTRA expected to be used with the OpenLog which can take greater transfer speeds than Xbee
	// F2: SERIAL_UDB_EXTRA format is printed out every other time, although it is being called at 8Hz, this
	//		version will output four F2 lines every second (4Hz updates)
#endif

	skip = 0 ;
	
	switch (telemetry_counter)
	{
		// The first lines of telemetry contain info about the compile-time settings from the options.h file
		case 6:
			serial_output("F11:WIND_EST=%i:GPS_TYPE=%i:DR=%i:BOARD_TYPE=%i:AIRFRAME=%i:\r\n",
				WIND_ESTIMATION, GPS_TYPE, DEADRECKONING, BOARD_TYPE, AIRFRAME_TYPE);
			break;
		case 5:
			serial_output("F4:R_STAB=%i:P_STAB=%i:Y_STAB_R=%i:Y_STAB_A=%i:AIL_NAV=%i:RUD_NAV=%i:AH_STAB=%i:AH_WP=%i:RACE=%i:\r\n",
				ROLL_STABILIZATION, PITCH_STABILIZATION, YAW_STABILIZATION_RUDDER, YAW_STABILIZATION_AILERON,
				AILERON_NAVIGATION, RUDDER_NAVIGATION, ALTITUDEHOLD_STABILIZED, ALTITUDEHOLD_WAYPOINT, RACING_MODE) ;
			break ;
		case 4:
			serial_output("F5:YAWKP_A=%5.3f:YAWKD_A=%5.3f:ROLLKP=%5.3f:ROLLKD=%5.3f:A_BOOST=%3.1f:\r\n",
				YAWKP_AILERON, YAWKD_AILERON, ROLLKP, ROLLKD, AILERON_BOOST ) ;
			break ;
		case 3:
			serial_output("F6:P_GAIN=%5.3f:P_KD=%5.3f:RUD_E_MIX=%5.3f:ROL_E_MIX=%5.3f:E_BOOST=%3.1f:\r\n",
				PITCHGAIN, PITCHKD, RUDDER_ELEV_MIX, ROLL_ELEV_MIX, ELEVATOR_BOOST) ;
			break ;
		case 2:
			serial_output("F7:Y_KP_R=%5.4f:Y_KD_R=%5.3f:RUD_BOOST=%5.3f:RTL_PITCH_DN=%5.3f:\r\n",
				YAWKP_RUDDER, YAWKD_RUDDER, RUDDER_BOOST, RTL_PITCH_DOWN) ;
			break ;
		case 1:
			serial_output("F8:H_MAX=%6.1f:H_MIN=%6.1f:MIN_THR=%3.2f:MAX_THR=%3.2f:PITCH_MIN_THR=%4.1f:PITCH_MAX_THR=%4.1f:PITCH_ZERO_THR=%4.1f:\r\n",
				HEIGHT_TARGET_MAX, HEIGHT_TARGET_MIN, ALT_HOLD_THROTTLE_MIN, ALT_HOLD_THROTTLE_MAX,
				ALT_HOLD_PITCH_MIN, ALT_HOLD_PITCH_MAX, ALT_HOLD_PITCH_HIGH) ;
			break ;
		default:			
			// convert cpu_timer into cpu load percentage in the high word of accum
			accum.WW = __builtin_muluu( cpu_timer ,CPU_LOAD_PERCENT );
	
			// F2 below means "Format Revision 2: and is used by a Telemetry parser to invoke the right pattern matching
			// F2 is a compromise between easy reading of raw data in a file and not droppping chars in transmission.
			

#if ( SERIAL_OUTPUT_FORMAT == SERIAL_UDB )
			serial_output("F2:T%li:S%d%d%d:N%li:E%li:A%li:W%i:a%i:b%i:c%i:d%i:e%i:f%i:g%i:h%i:i%i:c%u:s%i:cpu%u:bmv%i:"
				"as%i:wvx%i:wvy%i:wvz%i:\r\n",
				tow, flags._.radio_on, flags._.nav_capable, flags._.GPS_steering,
				lat_gps.WW , long_gps.WW , alt_sl_gps.WW, waypointIndex,
				rmat[0] , rmat[1] , rmat[2] ,
				rmat[3] , rmat[4] , rmat[5] ,
				rmat[6] , rmat[7] , rmat[8] ,
				(unsigned int)cog_gps.BB, sog_gps.BB, accum._.W1, voltage_milis.BB,
				air_speed_magnitude, estimatedWind[0], estimatedWind[1],estimatedWind[2]) ;						

#elif ( SERIAL_OUTPUT_FORMAT == SERIAL_UDB_EXTRA )
			if (print_choice == 0 )
			{
				serial_output("F2:T%li:S%d%d%d:N%li:E%li:A%li:W%i:a%i:b%i:c%i:d%i:e%i:f%i:g%i:h%i:i%i:c%u:s%i:cpu%u:bmv%i:"
					"as%i:wvx%i:wvy%i:wvz%i:ma%i:mb%i:mc%i:svs%i:hd%i:",
					tow, flags._.radio_on, flags._.nav_capable, flags._.GPS_steering,
					lat_gps.WW , long_gps.WW , alt_sl_gps.WW, waypointIndex,
					rmat[0] , rmat[1] , rmat[2] ,
					rmat[3] , rmat[4] , rmat[5] ,
					rmat[6] , rmat[7] , rmat[8] ,
					(unsigned int)cog_gps.BB, sog_gps.BB, accum._.W1, voltage_milis.BB,
					air_speed_magnitude, estimatedWind[0], estimatedWind[1],estimatedWind[2], 
					magFieldEarth[0],magFieldEarth[1],magFieldEarth[2],
					svs, hdop ) ;												
				// Save  pwIn and PwOut buffers for printing next time around
				for (i=0; i <= MAX_INPUTS; i++)
					pwIn_save[i] = pwIn[i] ;
				for (i=0; i <= MAX_OUTPUTS; i++)
					pwOut_save[i] = pwOut[i] ;
				print_choice = 1 ;
			}			
			else 
			{	
				for (i= 1; i <= MAX_INPUTS; i++)
					serial_output("p%ii%i:",i,pwIn_save[i]);
				for (i= 1; i <= MAX_OUTPUTS; i++)
					serial_output("p%io%i:",i,pwOut_save[i]);
				serial_output("lex%i:ley%i:lez%i:fgs:%X\r\n",
					locationErrorEarth[0] , locationErrorEarth[1] , locationErrorEarth[2], flags.WW );
				print_choice = 0 ;
			}

#endif	
			if ((flags._.f13_print_req == 1)
#if ( SERIAL_OUTPUT_FORMAT == SERIAL_UDB_EXTRA )
					&& (print_choice == 0)
#endif
				)// The F13 line of telemetry is printed when origin has been captured and inbetween F2 lines in SERIAL_UDB_EXTRA
			{ 
				serial_output("F13:week%i:origN%li:origE%li:origA%li:\r\n", week_no, lat_origin.WW, long_origin.WW, alt_origin ) ;
				flags._.f13_print_req = 0 ;
				return ;
			}
			return ;
	}
	telemetry_counter-- ;
	return ;
}


#elif ( SERIAL_OUTPUT_FORMAT == SERIAL_OSD_REMZIBI )

void serial_output_4hz( void )
{
	// TODO: Output interesting information for OSD.
	// But first we'll have to implement a buffer for passthrough characters to avoid
	// output corruption, or generate NMEA ourselves here.
	return ;
}

#elif ( SERIAL_OUTPUT_FORMAT == SERIAL_MAGNETOMETER )

extern void rxMagnetometer(void) ;
extern int magFieldBody[3] ;
extern unsigned char magreg[6] ;
extern int magFieldEarth[3] ;
extern int magOffset[3] ;
extern int magGain[3] ;
extern int offsetDelta[3] ;
extern int rawMagCalib[3] ;
extern int magMessage ;

extern union longww HHIntegral ;

#define OFFSETSHIFT 1

extern int I2ERROR ;
extern int I2messages ;
extern int I2interrupts ;
/*
void serial_output_4hz( void )
{
	serial_output("MagMessage: %i\r\nI2CCON: %X, I2CSTAT: %X, I2ERROR: %X\r\nMessages: %i\r\nInterrupts: %i\r\n\r\n" ,
		magMessage ,
		I2CCON , I2CSTAT , I2ERROR ,
		I2messages, I2interrupts ) ;
	return ;
}
*/

void serial_output_4hz( void )
{
	serial_output("MagOffset: %i, %i, %i\r\nMagBody: %i, %i, %i\r\nMagEarth: %i, %i, %i\r\nMagGain: %i, %i, %i\r\nCalib: %i, %i, %i\r\nMagMessage: %i\r\nTotalMsg: %i\r\nI2CCON: %X, I2CSTAT: %X, I2ERROR: %X\r\n\r\n" ,
		magOffset[0]>>OFFSETSHIFT , magOffset[1]>>OFFSETSHIFT , magOffset[2]>>OFFSETSHIFT ,
		magFieldBody[0] , magFieldBody[1] , magFieldBody[2] ,
		magFieldEarth[0] , magFieldEarth[1] , magFieldEarth[2] ,
		magGain[0] , magGain[1] , magGain[2] ,
		rawMagCalib[0] , rawMagCalib[1] , rawMagCalib[2] ,
		magMessage ,
		I2messages ,
		I2CCON , I2CSTAT , I2ERROR ) ;
	return ;
}


#else // If SERIAL_OUTPUT_FORMAT is set to SERIAL_NONE, or is not set

void serial_output_4hz( void )
{
	return ;
}

#endif