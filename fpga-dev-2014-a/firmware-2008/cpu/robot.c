// Requirements for radio configuration:
//      When RX or TX finishes, the radio returns to IDLE (MCSM1 bits 0-3 are zero)
//      MCSM0 bits 5-4 are expected to be 01 in the radio configuration table cc1101_regs.
//
// When calibration happens:
//      Going from IDLE to RX.
//      Every lost packet time after the last forward packet is received.
//      *Not* when going from IDLE to TX.

#include "lpc2103.h"
#include "fpga.h"
#include "pins.h"
#include "robot.h"
#include "timer.h"
#include "radio.h"
#include "radio_config.h"
#include "cc1101.h"
#include "vic.h"
#include "adc.h"

//#define CHECK_BATTERY
//#define WATCHDOG

#define PLL_M       4
// P = 2^PLL_LOG2P
#define PLL_LOG2P   2

void set_flag(volatile void *flag);

// Maximum change in a motor's speed from one PD update to the next.
static const int Max_Rate = 16;

// Last forward packet
uint8_t forward_packet[Forward_Size];

// Last reverse packet
uint8_t reverse_packet[Reverse_Size];

// Board ID of this robot
uint8_t board_id = 0;

// PD coefficients
int16_t coeff_p = 0x0100, coeff_d = 0x0000;

// Commanded wheel speeds from forward packet
int8_t wheel_command[4];

// Difference in encoder counts over last 10ms
int8_t wheel_speed[4];

// Encoders after last PD update
uint16_t last_tick[4];

// Velocity errors after last PD update
int last_error[4];

// Motor speeds generated by last PD update
int output[4];

// Nonzero if it has been too long since the last forward packet
uint8_t lost_radio = 1;

uint8_t dribble = 0;

// RSSI of last valid packet received
uint8_t last_rssi = 0;

// True if the radio is sending the reverse packet
uint8_t in_reverse = 0;

// Number of times the radio has reset since the last full reconfiguration
uint8_t lost_radio_count = 0;

// Nonzero if it is time to run the PD controller (set by set_flag)
volatile int motor_update = 0;

// Nonzero if it has been too long since the last forward packet or radio calibration (set by set_flag)
volatile int radio_timeout = 0;

// Last sequence number received
uint8_t last_sequence = 0;

// One bit for each of the lasts 16 sequence numbers that has been seen
uint16_t sequence_history = 0;

// One-touch kick strength
uint8_t one_touch = 0;

// Nonzero if we have the ball
int ball_present = 0;

// Nonzero if the ball sensor has failed
int ball_sensor_fail = 0;

// Last battery value.  Raw value from the ADC.
uint16_t battery = 0;

// Value to write to FPGA_Kicker_Status
volatile uint8_t kicker_control = 0;

// This is set when the battery is low for too long
volatile int dead_battery = 0;

void clear_int(volatile void *);
void discharge(volatile void *);
void battery_dead(volatile void *);

timer_t lost_radio_timer = {100, 100, set_flag, &radio_timeout};
timer_t ball_timer = {0, 0, clear_int, &ball_present};
timer_t discharge_timer = {0, 0, discharge, 0};
timer_t dead_battery_timer = {1000, 0, set_flag, &dead_battery};

void reverse_packet_sent();

void pll_init()
{
	// Configure the PLL
	PLLCFG = (PLL_M - 1) | (PLL_LOG2P << 5);
	PLLCON = 1;
	PLLFEED = 0xaa;
	PLLFEED = 0x55;

	// Wait for the PLL to lock
	while (!(PLLSTAT & (1 << 10)))
		;

	// Connect the PLL
	PLLCON = 3;
	PLLFEED = 0xaa;
	PLLFEED = 0x55;
}

void flash_red()
{
	FIOPIN ^= IO_LED_RED;
}

void flash_orange()
{
	FIOPIN ^= IO_LED_ORANGE;
}

void flash_both()
{
	FIOPIN ^= IO_LED_RED | IO_LED_ORANGE;
}

void clear_int(volatile void *x)
{
	*(int *)x = 0;
}

void discharge(volatile void *arg)
{
	fpga_write(FPGA_Kick, 0xff);
	
	// Re-enable the charger
	kicker_control |= 8;
}

void kick(int strength)
{
	fpga_write(FPGA_Kick, strength);
	
	if (strength < 0x80)
	{
		// Disable the charger
		kicker_control &= ~8;
		fpga_write(FPGA_Kicker_Status, kicker_control);
		
		// Start a timer to do a full kick to fully discharge the caps so the charger is guaranteed to restart.
		discharge_timer.time_left = 250;
	}
}

void got_ball()
{
	ball_present = 1;
	ball_timer.time_left = 500;
	
	if (one_touch)
	{
		kick(one_touch);
	}
}

void ball_sense()
{
	uint16_t ball_adc = adc_read(6);
	
	if (ball_adc >= 0x180 && !ball_sensor_fail)
	{
		FIOCLR = IO_LED_ORANGE;
		got_ball();
	} else {
		FIOSET = IO_LED_ORANGE;
	}
}

void radio_configure()
{
	radio_command(SIDLE);
	
	radio_select();
	for (int i = 0; i < sizeof(cc1101_regs); ++i)
	{
		spi_write(cc1101_regs[i]);
	}
	radio_deselect();
	
	radio_write(IOCFG2, 6 | GDOx_INVERT);
	radio_clear_gdo2();
	
	radio_command(SFRX);
	radio_command(SRX);
}

void forward_packet_received()
{
	uint8_t bytes = radio_read(RXBYTES);
	
	FIOPIN ^= IO_LED_RED;
	
	if (bytes != (Forward_Size + 3))
	{
		// Bad CRC, so the packet was flushed (or the radio got misconfigured).
		radio_command(SFRX);
		radio_command(SRX);
		return;
	}

	// Read the packet from the radio
	radio_select();
	spi_write(RXFIFO | CC_READ | CC_BURST);
	uint8_t pktlen = spi_write(SNOP);
	for (int i = 0; i < Forward_Size; ++i)
	{
		forward_packet[i] = spi_write(SNOP);
	}
	
	// Read status bytes
	uint8_t rssi = spi_write(SNOP);
	uint8_t status = spi_write(SNOP);
	radio_deselect();
	
	if (!(status & 0x80) || pktlen != Forward_Size)
	{
		// Bad CRC
		//
		// Autoflush is supposed to be on so this should never happen.
		// If we get here and autoflush is on, this means some bytes have been lost
		// and the status byte isn't really the status byte.
		radio_command(SFRX);
		radio_command(SRX);
		return;
	}
	
	last_rssi = rssi;
	
	uint8_t reverse_id = forward_packet[0] & 15;
	
	// Update sequence number history
	uint8_t sequence = forward_packet[0] >> 4;
	
#if 0
	// Kick/chip selection
	kicker_control &= ~0x20;
	if (forward_packet[1] & 0x10)
	{
	    kicker_control |= 0x20;
	}
	fpga_write(FPGA_Kicker_Status, kicker_control);
#endif
	
	// Clear history bits for missed packets
	for (int i = (last_sequence + 1) & 15; i != sequence; i = (i + 1) & 15)
	{
		sequence_history &= ~(1 << i);
	}
	
	// Set the history bit for this packet
	sequence_history |= 1 << sequence;
	
	// Save this packet's sequence number for next time
	last_sequence = sequence;
	
	// Count lost packets
	int lost_packets = 0;
	for (int i = 0; i < 16; ++i)
	{
		if (!(sequence_history & (1 << i)))
		{
			++lost_packets;
		}
	}
	
	// Reset lost radio timer
	lost_radio_timer.time_left = lost_radio_timer.reset;
	lost_radio = 0;
	lost_radio_count = 0;
	radio_timeout = 0;
	
	// Clear motor commands in case this robot's ID does not appear in the packet
	for (int i = 0; i < 4; ++i)
	{
		wheel_command[i] = 0;
	}
	dribble = 0;

	// Get motor commands from the packet
	int offset = 1;
	one_touch = 0;
	int found = 0;
	for (int slot = 0; slot < 5; ++slot)
	{
		if ((forward_packet[offset + 4] & 0x0f) == board_id)
		{
			for (int i = 0; i < 4; ++i)
			{
				wheel_command[i] = (int8_t)forward_packet[offset + i];
			}
			dribble = forward_packet[offset + 4] >> 4;
			one_touch = forward_packet[offset + 5];
			found = 1;
		}
		offset += 6;
	}

	// Only toggle the radio LED if data for this robot was present
// 	if (found)
// 	{
// 		FIOPIN ^= IO_LED_RED;
// 	} else {
// 		FIOPIN |= IO_LED_RED;
// 	}

	if (reverse_id == board_id)
	{
		// Build and send a reverse packet
		radio_command(SFTX);
		
		uint8_t fault = fpga_read(FPGA_Fault) & 0x1f;
		
		// Clear fault bits
		fpga_write(FPGA_Fault, fault);
		
		reverse_packet[0] = (lost_packets << 4) | board_id;
		reverse_packet[1] = last_rssi;
		reverse_packet[2] = 0x00;
		reverse_packet[3] = battery >> 2;
		reverse_packet[4] = fpga_read(FPGA_Kicker_Status);
		
		// Fault bits
		reverse_packet[5] = fault;
		
		if (ball_present)
		{
			reverse_packet[5] |= 1 << 5;
		}
		
		if (ball_sensor_fail)
		{
			reverse_packet[5] |= 1 << 6;
		}
		
		reverse_packet[10] = 0;
		for (int i = 0; i < 4; ++i)
		{
		    reverse_packet[6 + i] = last_tick[i];
		    reverse_packet[10] |= (last_tick[i] & 0x300) >> (8 - i * 2);
		}

		radio_select();
		spi_write(TXFIFO | CC_BURST);
		spi_write(Reverse_Size);
		for (int i = 0; i < Reverse_Size; ++i)
		{
			spi_write(reverse_packet[i]);
		}
		radio_deselect();

		// Start transmitting.  When this finishes, the radio will automatically switch to RX
		// without calibrating (because it doesn't go through IDLE).
		radio_command(STX);
		
		in_reverse = 1;
	} else {
		// Get ready to receive another forward packet
		radio_command(SRX);
	}
}

void reverse_packet_sent()
{
	radio_command(SRX);

	in_reverse = 0;
}

void check_battery()
{
#ifdef CHECK_BATTERY
    battery = adc_read(5);
    
    //FIXME - Proper threshold
    if (battery > 682)
    {
	// Reset the dead battery timer.
	// If this function isn't called often enough or the battery is low for one second,
	// everything will shut down.
	dead_battery_timer.time_left = 1000;
    } else if (dead_battery)
    {
	// Battery is dead
	FIOPIN &= ~IO_LED_RED;
	FIOPIN |= IO_LED_ORANGE;
	
	timer_t t1 = {500, 500, flash_both, 0};
	timer_register(&t1);

	while (1)
	{
#ifdef WATCHDOG
	    // Reset the watchdog timer
	    WDFEED = 0xaa;
	    WDFEED = 0x55;
#endif

	    for (int i = 0; i < 5; ++i)
	    {
		fpga_write(i, 0);
	    }
	}
    }

#else
    dead_battery_timer.time_left = 1000;
#endif
}

int main()
{
	vic_init();
	pll_init();
	adc_init();

	// Configure GPIO
	SCS = 1;
	APBDIV = 1;

	FIOSET = IO_WR | IO_RD | IO_ALE | IO_LED_RED | IO_LED_ORANGE;
	FIODIR = IO_WR | IO_RD | IO_ALE | IO_LED_RED | IO_LED_ORANGE | IO_BUZZER;
	
	timer_init();
	
	// Initialize and test FPGA
	FIOPIN &= ~IO_LED_RED & ~IO_LED_ORANGE;

	timer_t t1 = {70, 70, flash_both, 0};
	timer_register(&t1);
	fpga_init();
	timer_unregister(&t1);

	// Initialize and test radio
	FIOPIN &= ~IO_LED_RED & ~IO_LED_ORANGE;
	radio_init();
	FIOPIN |= IO_LED_RED | IO_LED_ORANGE;
	
	radio_configure();
	
	// Clear motor faults.
	//
	// All motors indicate a fault when the FPGA is reset because
	// the initial state of the hall efffect sensors is not known.
	// Faults can be cleared one FPGA clock after hall inputs become valid.
	fpga_write(FPGA_Fault, 0xff);
	
	// Start timers used in normal operation
	timer_t motor_timer = {5, 5, set_flag, &motor_update};
	timer_register(&motor_timer);
	
	timer_register(&lost_radio_timer);
	timer_register(&ball_timer);
	timer_register(&discharge_timer);
	timer_register(&dead_battery_timer);
	
	// Enable the charger
	kicker_control |= 8;
	
#ifdef WATCHDOG
	// Start watchdog timer
	WDTC = 14745600;        // 0.25s
	WDMOD = 3;
	WDFEED = 0xaa;
	WDFEED = 0x55;
#endif

	while (1)
	{
		fpga_write(FPGA_Kicker_Status, kicker_control);
		
		board_id = fpga_read(FPGA_Switches) & 15;
		
		ball_sense();
		
		if (radio_int_gdo2())
		{
			radio_clear_gdo2();
			if (in_reverse)
			{
				reverse_packet_sent();
			} else {
				forward_packet_received();
			}
		}
		
		if (radio_timeout)
		{
			FIOSET = IO_LED_RED;
			radio_timeout = 0;
			lost_radio = 1;

			++lost_radio_count;
			if (lost_radio_count == 10)
			{
				lost_radio_count = 0;
				radio_configure();
			} else {
				radio_command(SIDLE);
				radio_command(SFRX);
				radio_command(SRX);
			}
		}
		
		if (motor_update)
		{
			motor_update = 0;
			
			if (board_id & 8)
			{
			    check_battery();
			} else {
			    // Keep resetting this timer so we don't immediately die if the switch is changed.
			    dead_battery_timer.time_left = 1000;
			}

#ifdef WATCHDOG
			// Reset the watchdog timer
			WDFEED = 0xaa;
			WDFEED = 0x55;
#endif

			// Wheel speed control
			fpga_write(FPGA_Encoder_Latch, 0);
			int addr = FPGA_Encoder_Low;
			for (int i = 0; i < 4; ++i)
			{
				int command = wheel_command[i];
				uint16_t tick = fpga_read(addr) | (fpga_read(addr + 1) << 8);
				addr += 2;
				
				wheel_speed[i] = tick - last_tick[i];
				last_tick[i] = tick;
				
				if (!lost_radio)
				{
					int error = command - wheel_speed[i];
					int deriv = error - last_error[i];
					last_error[i] = error;
					int delta = (coeff_p * error + coeff_d * deriv) / 256;
					
					// Rate limit
					if (delta > Max_Rate)
					{
						delta = Max_Rate;
					} else if (delta < -Max_Rate)
					{
						delta = -Max_Rate;
					}
					
					// Saturation
					output[i] += delta;
					
					//TEST
					output[i] = wheel_command[i];
					
					if (output[i] > 127)
					{
						output[i] = 127;
					} else if (output[i] < -127)
					{
						output[i] = -127;
					}
					
					// Convert signed speed to sign-magnitude form
					uint8_t motor;
					if (output[i] < 0)
					{
						motor = 0x80 | -output[i];
					} else {
						motor = output[i];
					}
					
					fpga_write(i, motor);
				} else {
					fpga_write(i, 0);
				}
			}
		}
		
		if (!lost_radio)
		{
			fpga_write(4, 0x80 | (dribble << 3));
		} else {
			fpga_write(4, 0);
		}
	}
}
