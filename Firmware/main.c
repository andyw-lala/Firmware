/*
 *
 * Initial firmware for Public Radio.
 *
 * Responsibilities include:
 *	- set up PB4 for PWM output, controlled by Timer1/OC1B
 *	- de-assertion of 4702 reset (drive PB1 high)
 *	- initialisation of 4072 receiver via i2c bus
 *		o muting of output
 *		o selection of correct operating mode (mono, etc)
 *		o output level selection
 *		o <other>
 *	- programming of receive frequency based on fields in EEPROM
 *	- unmute receiver
 *	- periodically poll RSSI and map onto OC1B output duty cycle
 *	- set up debounced button push for programming mode
 *	- consume as little power as possible when in normal operation
 *
 *	- enter manual programming mode via long button press (>= 2 sec)
 *	- led flashes 320ms on, 320mS off
 *	- interpret short button push as scan up command
 *	- automatically stop on next valid channel
 *	- wrap at band edges.
 *	- interpret long button press (>= 2 sec) as save memory command
 *	- led on solid as confirmation.
 *	- when in manual programming mode, interpret very long button
 *	  press (>= 4 sec) as factory reset request.
 *	- led flashes at 160mS on, 160mS off while waiting for comnfirmation.
 *	- confirmation is long button press (>= 2 sec)
 *	- led on solid as confirmation.
 *	- timeout of 10 secs of no button pressed aborts all tuning (will
 *	  stay tuned to new channel if in scan mode - but will power up
 *	  on the normal channel.)
 *
 * The cunning plan for Si4702 register access is as follows:
 *
 *   - remember that reads always start with register 0xA (status), and
 *     wrap around from register 0xf to 0x0.
 *   - writes on the other hand always start with register 0x2 (power config).
 *   - we should only ever need to write registers 0x2 thru 0x7
 *
 *   - We will *always* read all 16 registers, and store them in AVR RAM.
 *   - We will only ever write registers 2 - 7 (6 total), and we will write
 *     them from the same shadow storage in AVR RAM.
 *
 *   - To change a register, we will modify the shadow registers in place
 *     and then write back the entire bank of 6.
 *
 *   - The Tiny25 has limited resources, to preserve these, the shadow
 *     registers are not laid out from 0 in AVR memory, but rather from 0xA
 *     as follows:
 *
 *     +--------------+-----------+
 *     |              | High Byte |   <--- Byte offset: 0
 *     | Register 0xA |===========|
 *     |              |  Low Byte |   <--- Byte offset: 1
 *     +--------------+-----------+
 *     |              | High Byte |   <--- Byte offset: 2
 *     | Register 0xB |===========|
 *     |              |  Low Byte |   <--- Byte offset: 3
 *     +--------------+-----------+
 *     |              | High Byte |   <--- Byte offset: 4
 *     | Register 0xC |===========|
 *     |              |  Low Byte |   <--- Byte offset: 5
 *     +--------------+-----------+
 *     |              | High Byte |   <--- Byte offset: 6
 *     | Register 0xD |===========|
 *     |              |  Low Byte |   <--- Byte offset: 7
 *     +--------------+-----------+
 *     |              | High Byte |   <--- Byte offset: 8
 *     | Register 0xE |===========|
 *     |              |  Low Byte |   <--- Byte offset: 9
 *     +--------------+-----------+
 *     |              | High Byte |   <--- Byte offset: 10
 *     | Register 0xF |===========|
 *     |              |  Low Byte |   <--- Byte offset: 11
 *     +--------------+-----------+
 *     |              | High Byte |   <--- Byte offset: 12
 *     | Register 0x0 |===========|
 *     |              |  Low Byte |   <--- Byte offset: 13
 *     +--------------+-----------+
 *     |              | High Byte |   <--- Byte offset: 14
 *     | Register 0x1 |===========|
 *     |              |  Low Byte |   <--- Byte offset: 15
 *     +--------------+-----------+
 *     |              | High Byte |   <--- Byte offset: 16
 *     | Register 0x2 |===========|
 *     |              |  Low Byte |   <--- Byte offset: 17
 *     +--------------+-----------+
 *     |              | High Byte |   <--- Byte offset: 18
 *     | Register 0x3 |===========|
 *     |              |  Low Byte |   <--- Byte offset: 19
 *     +--------------+-----------+
 *     |              | High Byte |   <--- Byte offset: 20
 *     | Register 0x4 |===========|
 *     |              |  Low Byte |   <--- Byte offset: 21
 *     +--------------+-----------+
 *     |              | High Byte |   <--- Byte offset: 22
 *     | Register 0x5 |===========|
 *     |              |  Low Byte |   <--- Byte offset: 23
 *     +--------------+-----------+
 *     |              | High Byte |   <--- Byte offset: 24
 *     | Register 0x6 |===========|
 *     |              |  Low Byte |   <--- Byte offset: 25
 *     +--------------+-----------+
 *     |              | High Byte |   <--- Byte offset: 26
 *     | Register 0x7 |===========|
 *     |              |  Low Byte |   <--- Byte offset: 27
 *     +--------------+-----------+
 *     |              | High Byte |   <--- Byte offset: 28
 *     | Register 0x8 |===========|
 *     |              |  Low Byte |   <--- Byte offset: 29
 *     +--------------+-----------+
 *     |              | High Byte |   <--- Byte offset: 30
 *     | Register 0x9 |===========|
 *     |              |  Low Byte |   <--- Byte offset: 31
 *     +--------------+-----------+
 *
 *   - Because registers 2 thru 7 don't wrap in the buffer, we can use
 *     contiguous read *and* write function.
 *   - The goal here is to get the pre-processor to do as much of the work
 *     for us as possible, especially since we only ever need to access
 *     registers by name, not algorithmically.
 *   - Note that the endianness is opposite of the natural order for uint16_t
 *     on AVR, so a union overlaying 16 bit registers on the 8-bit array
 *     will not give us what we need.
 * 
 */
#include <inttypes.h>
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <util/atomic.h>
#include <avr/sleep.h>
#include <avr/eeprom.h>
#include <util/crc16.h>

#define	F_CPU	1000000UL
#include <util/delay.h>



#include "USI_TWI_Master.h"
#include "VccADC.h"
#include "VccProg.h"

#define FMIC_ADDRESS        (0b0010000)                // Hardcoded for this chip, "a seven bit device address equal to 0010000"

#define LED_DRIVE_BIT       PB4
#define FMIC_RESET_BIT      PB1
#define FMIC_SCLK_BIT       PB2
#define FMIC_SDIO_BIT       PB0

#define BUTTON_INPUT_BIT    PB3
#define BUTTON_PCINT_BIT    PCINT3
#define LONG_PRESS_MS       (2000)      // Hold down button this long for a long press
#define BUTTON_DEBOUNCE_MS  (50)        // How long to debounce button edges

#define LOW_BATTERY_VOLTAGE (2.1)       // Below this, we will just blink LED and not turn on 

#define SBI(port,bit) (port|=_BV(bit))
#define CBI(port,bit) (port&=~_BV(bit))
#define TBI(port,bit) (port&_BV(bit))


typedef enum {
	NORMAL = 1,
	TUNE,
	SEEK_START,
	SEEKING,
	SAVE,
	FACTORY_RESET,
	FACTORY_CONFIRM,
	TIMEOUT,
} mode;

mode current_mode = NORMAL;
mode display_mode = NORMAL;

/*
 * Timestamp of the last time the button was released.
 * Used for timeout.
 */
uint16_t last_release = 0;

#define TIMEOUT	(1000)	/* 10 seconds */

volatile uint16_t ticks;

typedef enum {
	REGISTER_00 = 12,
	REGISTER_01 = 14,
	REGISTER_02 = 16,
	REGISTER_03 = 18,
	REGISTER_04 = 20,
	REGISTER_05 = 22,
	REGISTER_06 = 24,
	REGISTER_07 = 26,
	REGISTER_08 = 28,
	REGISTER_09 = 30,
	REGISTER_10 =  0,
	REGISTER_11 =  2,
	REGISTER_12 =  4,
	REGISTER_13 =  6,
	REGISTER_14 =  8,
	REGISTER_15 = 10,
} si4702_register;

#define EEPROM_BAND		((const uint8_t *)0)
#define EEPROM_DEEMPHASIS	((const uint8_t *)1)
#define EEPROM_SPACING		((const uint8_t *)2)
#define EEPROM_CHANNEL		((const uint16_t *)3)
#define EEPROM_VOLUME		((const uint8_t *)5)
#define	EEPROM_CRC16		((const uint16_t *)14)

#define EEPROM_PARAM_SIZE	(16)

#define	EEPROM_WORKING		((const uint8_t *)0)
#define EEPROM_FACTORY		((const uint8_t *)16)

const uint8_t last_resort_param[16] PROGMEM = {
	0x00,
	0x00,
	0x00,
	0x09, 0x00,
	0x0f,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x6f, 0x6c,
} ;

/*
 * Seek thresholds - see Appendix of SiLabs AN230
 */
#define	SEEK_RSSI_THRESHOLD	(10)
#define	SEEK_SNR_THRESHOLD	(2)
#define SEEK_IMPULSE_THRESHOLD	(4)

void tune_direct(uint16_t);


/*
 * Button press callout functions.
 */
static void button_short(void)
{
	if (current_mode == TUNE) {
		current_mode = SEEK_START;
	}
}

static void button_2s(void)
{
	switch (current_mode) {
	    case NORMAL:
		current_mode = TUNE;
		break;

	    case TUNE:
		current_mode = SAVE;
		break;

	    case FACTORY_RESET:
		current_mode = FACTORY_CONFIRM;
		break;

	    default:
		break;
	}
}

static void led_2s(void)
{
	switch (current_mode) {
	    case NORMAL:
		display_mode = TUNE;
		break;

	    case TUNE:
	    case FACTORY_RESET:
		display_mode = SAVE;
		break;

	    default:
		break;
	}
}

static void button_4s(void)
{
	if (current_mode == TUNE) {
		current_mode = FACTORY_RESET;
	}
}

static void led_4s(void)
{
	if (current_mode == TUNE) {
		display_mode = FACTORY_RESET;
	}
}

struct dispatch {
	uint16_t ticks;		/* 10mS per tick */
	void (*callout)(void);
	void (*display)(void);
} ;

const struct dispatch button_dispatch[] PROGMEM = {
	{5, button_short, (void (*)(void))0},
	{200, button_2s, led_2s},
	{400, button_4s, led_4s},
	{0, (void (*)(void))0, (void (*)(void))0},
};

/*
 * This is a bit longer than I would prefer, but the hot path when there
 * is no button press activity (button == 0x0f && ticks_active == 0) is
 * pretty benign.
 *
 * TODO: explore using pin change interrup on first press to skip this
 *       code completely in normal use.
 *
 * Called every tick (10mS), implement a dispatch table of functions
 * to call if the button is pressed for variable durations.
 *
 * Debounce on the leading edge of the button press by means of the 
 * dispatch table, and on the trailing edge by waiting until 4 consecutive
 * samples have read high (switch is active low.)
 *
 * Code is slightly complicated by the progmem access semantics of avr-libc,
 * whereby we must copy each object we wish to inspect/use.
 */
void button_handle(uint8_t state)
{
	static uint16_t ticks_active = 0;
	static uint8_t button = 0xff;
	static const struct dispatch *dp = (struct dispatch *)0;

	/*
	 * Shift current button state into static variable button
	 */
	button = ((button << 1) | (state ? 1 : 0)) & 0X0f;

	if (button == 0x0f) {
		if (ticks_active) {
			/*
			 * button released & debounced for 4 ticks.
			 */
			if (dp) {
				void (*dp_callout)(void) = (void (*)(void))pgm_read_word(&(dp->callout));
				if (dp_callout != (void (*)(void))0) {
					dp_callout();
				}
				dp = (struct dispatch *)0;
			}
			ticks_active = 0;
			/*
			 * remember the time the button was released.
			 */
			last_release = ticks;
		}
	} else {
		const struct dispatch *p;
		uint16_t dp_ticks;
		/*
		 * Button active.
		 *
		 * Scan the callout table, invoking the display callout as
		 * we cross each threshold.
		 * Only call the button press callout when the button
		 * is released, so that we don't call every previous callout
		 * for longer presses.
		 */
		if (ticks_active < UINT16_MAX) {
			ticks_active++;
		}
		for (p = button_dispatch; (dp_ticks = pgm_read_word(&(p->ticks))) != 0;) {
			if (dp_ticks == ticks_active) {
				dp = p;
				void (*dp_callout)(void) = (void (*)(void))pgm_read_word(&(p->display));
				if (dp_callout != (void (*)(void))0) {
					dp_callout();
				}
			}
			p++;
		}
	}
}

void rssi2pwm(void);

/*
 * Setup Timer 0 to provide a nominal 10mS timebase.
 * This is used to schedule events and also to debounce & time button presses.
 */
void timer0_init(void)
{
	TCCR0B = 0;
	OCR0A = 155;
	TIMSK = 0x10;
	TCCR0A = 0x02;
	TCCR0B = 0x03;
}

/*
 * Once interrupts are enabled via sei(), this function will be called
 * @ approx 10mS intervals (100Hz).
 */
ISR(TIM0_COMPA_vect)
{
	ticks++;

	button_handle(PINB & 0x08);
}

/*
 * Setup Timer 1 to drive the LED using PWM on the OC1B pin (PB4)
 */
void timer1_init(void)
{
	TCCR1 = 0;
	PLLCSR = 0x80;
	OCR1C = 255;
	OCR1B = 0;
	GTCCR = 0x60;

	/*
	 * Enable PB4 (OC1B) as output.
	 */
	DDRB |= 0x10;

	TCCR1 = 0x84;
}

uint8_t shadow[32];

static inline uint16_t get_shadow_reg(si4702_register reg)
{
	return (shadow[reg] << 8) | (shadow[reg + 1]);
}

static inline void set_shadow_reg(si4702_register reg, uint16_t value)
{
	shadow[reg] = value >> 8;
	shadow[reg + 1] = value & 0xff;
}

// Read all the registers. The FM_IC starts reads at register 0x0a and then wraps around

void si4702_read_registers(void)
{
	USI_TWI_Start_Transceiver_With_Data(0x21, shadow, 32);
}

/*
 * Just write registers 2 thru 7 inclusive from the shadow array.
 */
void si4702_write_registers(void)
{
	//USI_TWI_Start_Transceiver_With_Data(0x20, &(shadow[REGISTER_02]), 12);
        
    // Only registers 0x02 - 0x07 are relevant for config, and each register is 2 bytes wide
    
    // Even though the datasheet says that the reserved bits of 0x07 must be Read before writing, 
    // The previous version of this software blindly wrote 0's and that seems to work fine. 
    
    
    USI_TWI_Start_Transceiver_With_Data(0x20, &(shadow[REGISTER_02]), (0x08 - 0x02) * 2);    
    //USI_TWI_Write_Data( FMIC_ADDRESS ,  &(shadow[REGISTER_02]) , (0x08 - 0x02) * 2 );
}

/*
 * tune_direct() -	Directly tune to the specified channel.
 */
void tune_direct(uint16_t chan)
{
	set_shadow_reg(REGISTER_03, 0x8000 | (chan & 0x01ff));

	si4702_write_registers();

	_delay_ms(160);

	set_shadow_reg(REGISTER_03, get_shadow_reg(REGISTER_03) & ~0x8000);
	si4702_write_registers();
}

uint16_t currentChanFromShadow(void) {
    return( get_shadow_reg(REGISTER_03 & 0x1ff));
}    

/*
 * update_channel() -	Update the channel stored in the working params.
 *			Just change the 2 bytes @ EEPROM_CHANNEL,
 *			recalculate the CRC, and then write that.
 */
void update_channel(uint16_t channel)
{
	uint16_t crc = 0x0000;
	const uint8_t *src;

	eeprom_write_word((uint16_t *)EEPROM_CHANNEL, channel);

	/*
	 * Spin through the working params, up to but not including the CRC
	 * calculating the new CRC and write that.
	 */
	for (src = EEPROM_WORKING; src < (const uint8_t *)EEPROM_CRC16; src++) {
		crc = _crc16_update(crc, eeprom_read_byte(src));
	}

	eeprom_write_word((uint16_t *)EEPROM_CRC16, crc);
}

/*
 * check_param_crc() -	Check EEPROM_PARAM_SIZE bytes starting at *base,
 *			and return whether the crc (last 2 bytes) is correct
 *			or not.
 *			Return 0 if crc is good, !0 otherwise.
 */
static uint16_t check_param_crc(const uint8_t *base)
{
	uint16_t crc = 0x0000;
	uint8_t i;

	for (i = 0; i < EEPROM_PARAM_SIZE; i++, base++) {
		crc = _crc16_update(crc, eeprom_read_byte(base));
	}

	/*
	 * If CRC (last 2 bytes checked) is correct, crc will be 0x0000.
	 */
	return crc ;
}

/*
 * init_factory_param() -	Re-init the factory default area, using the
 *				16 bytes from last_resort_param[]. This really
 *				is, as the name suggests, the last resort.
 */
static void init_factory_param(void)
{
	uint8_t *dest = (uint8_t *)(EEPROM_FACTORY);
	uint8_t i;

	for (i = 0; i < sizeof(last_resort_param); i++, dest++) {
		eeprom_write_byte(dest, pgm_read_byte(&(last_resort_param[i])));
	}
}

/*
 * copy_factory_param() -	Copy the factory default parameters into the
 *				working param area  simple bulk copy of the
 *				entire 16 bytes. No check of the crc.
 */
static void  copy_factory_param(void)
{
	const uint8_t *src = EEPROM_FACTORY;
	uint8_t *dest = (uint8_t *)(EEPROM_WORKING);
	uint8_t i;

	for (i = 0; i < sizeof(last_resort_param); i++, src++, dest++) {
		eeprom_write_byte(dest, eeprom_read_byte(src));
	}
}

/*
 * check_eeprom() -	Check the CRC for the operational parameters in eeprom.
 *			If the CRC fails, check the factory parameters CRC.
 *			If that is good, copy into the operational area, if not
 *			good, then just pick some suitable defaults and write
 *			the same to both factory and operational areas.
 *
 *			The idea being that by the time this function returns,
 *			the eeprom has valid data in it that passes CRC.
 */

void check_eeprom(void)
{
	if (check_param_crc(EEPROM_WORKING)) {
		if (check_param_crc(EEPROM_FACTORY)) {
			init_factory_param();
		}
		copy_factory_param();
	}
}


void debugBlink( uint8_t b ) {
    
    while (1) {
         
         for(int p=0; p<b; p++ ) {   
            SBI( PORTB , LED_DRIVE_BIT);
            _delay_ms(200);
            CBI( PORTB , LED_DRIVE_BIT);
            _delay_ms(200);
            
         }
         
         _delay_ms(1000);            
            
    }        
}    


void binaryDebugBlink( uint16_t b ) {
    
    while (1) {
         
         for(uint16_t bitmask=0x8000; bitmask !=0; bitmask>>=1) {

            SBI( PORTB , LED_DRIVE_BIT);
            _delay_ms(200);            
             
             if (b & bitmask) {
                 _delay_ms(200);            
             }            
            CBI( PORTB , LED_DRIVE_BIT);
            
            _delay_ms(400);
            
         }
         
         _delay_ms(1000);            
            
    }        
}    

void si4702_init(void)
{
	/*
	 * Init the Si4702 as follows:
	 *
	 * Set PB1 (/Reset on Si7202) as output, drive low.
	 * Set PB0 (SDIO on si4702) as output, drive low.
	 * Set PB1 (/Reset on Si7202) as output, drive high.
	 * Set up USI in I2C (TWI) mode, reassigns PB0 as SDA.
	 * Read all 16 registers into shadow array.
	 * Enable the oscillator (by writing to TEST1 & other registers)
	 * Wait for oscillator to start
	 * Enable the IC, set the config, tune to channel, then unmute output.
	 */

	DDRB |= 0x03;
	PORTB &= ~(0x03);
	_delay_ms(1);
	PORTB |= 0x02;
	_delay_ms(1);

	USI_TWI_Master_Initialise();
	
	//si4702_read_registers();


    // Reg 0x07 bit 15 - Crystal Oscillator Enable.
    // 0 = Disable (default).
    // 1 = Enable.
    
    // Reg 0x07 bits 13:0 - Reserved. If written, these bits should be read first and then written with their pre-existing values. Do not write during powerup.
    // BUT Datasheet also says Bits 13:0 of register 07h must be preserved as 0x0100 while in powerdown and as 0x3C04 while in powerup.
   // si4702_read_registers();  

	set_shadow_reg(REGISTER_07, 0x8100);
    //set_shadow_reg(REGISTER_07, get_shadow_reg(REGISTER_07) | 0x8000);

	si4702_write_registers();

	_delay_ms(500);

	/*
	 * Register 02 default settings:
	 *	- Softmute enable
	 *	- Mute enable
	 *	- Mono
	 *	- Wrap on band edges during seek
	 *	- Seek up
	 */
	set_shadow_reg(REGISTER_02, 0xE201);

	si4702_write_registers();

	_delay_ms(110);


    // Bits 13:0 of register 07h must be preserved as 0x0100 while in powerdown and as 0x3C04 while in powerup.
	//set_shadow_reg(REGISTER_07,  0x3C04 );


	/*
	 * Set deemphasis based on eeprom.
	 */
    
	set_shadow_reg(REGISTER_04, get_shadow_reg(REGISTER_04) | (eeprom_read_byte(EEPROM_DEEMPHASIS) ? 0x0800 : 0x0000));

	set_shadow_reg(REGISTER_05,
			(SEEK_RSSI_THRESHOLD << 8) |
			(((uint16_t)(eeprom_read_byte(EEPROM_BAND) & 0x03)) << 6) |
			(((uint16_t)(eeprom_read_byte(EEPROM_SPACING) & 0x03)) << 4));


    // JML: Never seek, so no need to set this stuff

	/*
	 * Set the seek SNR and impulse detection thresholds.
	 */
	//set_shadow_reg(REGISTER_06,
	//		(SEEK_SNR_THRESHOLD << 4) | SEEK_IMPULSE_THRESHOLD);


	/*
	 * It looks like the radio tunes to <something> once enabled.
	 * Make sure the STC bit is cleared by clearing the TUNE bit.
     *
     * JML - The clearing of TUNE bit does not seem to be nessisary, but the read_regs() is. Go figure, something must change somewhere.
     * TODO: Find out what is changed on this read and hardcode it so we can dump the whole read function.
	 */
	si4702_read_registers();
	set_shadow_reg(REGISTER_03, 0x0000);
	si4702_write_registers();

	tune_direct(eeprom_read_word(EEPROM_CHANNEL));
    //set_shadow_reg(REGISTER_03, 0x8050 );                   //for testing, frequency = 103.5 MHz = 0.200 MHz x 80 + 87.5 MHz). Write data 8050h.

	set_shadow_reg(REGISTER_05, (get_shadow_reg(REGISTER_05) & ~0x000f) |
				(eeprom_read_byte(EEPROM_VOLUME) & 0x0f));

	si4702_write_registers();
    
    /*
    
    Can you here the de-emphasis difference? I don't think so. 
    
    while (1) {

	    set_shadow_reg(REGISTER_04, get_shadow_reg(REGISTER_04) | 0x0800 );
        SBI( PORTB , LED_DRIVE_BIT );
        si4702_write_registers();
        _delay_ms(1000);
	    set_shadow_reg(REGISTER_04, get_shadow_reg(REGISTER_04) & ~0x0800 );
        CBI( PORTB , LED_DRIVE_BIT );
        si4702_write_registers();        
        _delay_ms(1000);
        
           
    } 
    
    */       
}

// Assumes FMIC_RESET is output and low

void si4702_init2(void)
{
	/*
	 * Init the Si4702 as follows:
	 *
	 * Set up USI in I2C (TWI) mode
     * Enable pull-ups on TWI lines
	 * Enable the oscillator (by writing to TEST1 & other registers)
	 * Wait for oscillator to start
	 * Enable the IC, set the config, tune to channel, then unmute output.
	 */


    // Let's do the magic dance to awaken the FM_IC in 2-wire mode
    // We enter with RESET low (active)
        
    // Busmode selection method 1 requires the use of the
    // GPIO3, SEN, and SDIO pins. To use this busmode
    // selection method, the GPIO3 and SDIO pins must be
    // sampled low by the device on the rising edge of RST.
    // 
    // The user may either drive the GPIO3 pin low externally,
    // or leave the pin floating. If the pin is not driven by the
    // user, it will be pulled low by an internal 1 M? resistor
    // which is active only while RST is low. The user must
    // drive the SEN and SDIO pins externally to the proper
    // state.


    // Drive SDIO low (GPIO3 will be pulled low internally by the FM_IC) 
                
    SBI( PORTB , FMIC_RESET_BIT );     // Bring FMIC and AMP out of reset
                                       // The direct bit was set to output when we first started in main()
        
    _delay_ms(1);                      // When selecting 2-wire Mode, the user must ensure that a 2-wire start condition (falling edge of SDIO while SCLK is
                                       // high) does not occur within 300 ns before the rising edge of RST.
   
                                           
    // Enable the pull-ups on the TWI lines

	USI_TWI_Master_Initialise();

    



    /*
    
    
USI_TWI_Write_Data( FMIC_ADDRESS , "\x55" , 1 );
debugBlink(5);
    
    
       si4702_read_registers();    
    

    binaryDebugBlink( get_shadow_reg(REGISTER_00) );
    
    
    if ( get_shadow_reg(REGISTER_02) != 0 ) {
        //debugBlink(2);
    }            

    set_shadow_reg(REGISTER_02 , 0xE000 );
            
    si4702_write_registers();
        
    set_shadow_reg(REGISTER_02 , 0 );    
        
    si4702_read_registers();    
    
    binaryDebugBlink( get_shadow_reg(REGISTER_02) );
    
    if ( get_shadow_reg(REGISTER_02) == 0xe000 ) {
        debugBlink(3);
    }            
    
    debugBlink(4);
    
    
*/
    
    // Reg 0x07 bit 15 - Crystal Oscillator Enable.
    // 0 = Disable (default).
    // 1 = Enable.
    
    // Reg 0x07 bits 13:0 - Reserved. If written, these bits should be read first and then written with their pre-existing values. Do not write during powerup.
    // BUT Datasheet also says Bits 13:0 of register 07h must be preserved as 0x0100 while in powerdown and as 0x3C04 while in powerup.
   // si4702_read_registers();  
     
     
    /*
    
        Write address 07h (required for crystal oscillator operation).
        Set the XOSCEN bit to power up the crystal.
        Example: Write data 8100h.
    
    */
          
    
	set_shadow_reg(REGISTER_07, 0x8100 );
	si4702_write_registers();

    /*
        http://www.silabs.com/documents/public/application-notes/AN230.pdf
        AN230 - 2.1.1 
        If using the internal oscillator option, set the XOSCEN bit. Provide a sufficient delay before
        setting the ENABLE bit to ensure that the oscillator has stabilized. The delay will vary depending on the external
        oscillator circuit and the ESR of the crystal, and it should include margin to allow for device tolerances. The
        recommended minimum delay is no less than 500 ms. A similar delay may be necessary for some external
        oscillator circuits. Determine the necessary stabilization time for the clock source in the system.
        To experimentally measure the minimum oscillator stabilization time, adjust the delay time between setting the
        XOSCEN and ENABLE bits. After powerup, use the Set Property Command described in "5.1.Si4702/03
        Commands (Si4702/03 Rev C or Later Device Only)" on page 31 to read property address 0x0700. If the delay
        exceeds the minimum oscillator stabilization time, the property value will read 0x1980 ±20%. If the property
        value is above this range, the delay time is too short. The selected delay time should include margin to allow for
        device tolerances.
    */



    /*

        Wait for crystal to power up (required for crystal oscillator operation).
        Provide a sufficient delay (minimum 500 ms) for the oscillator to stabilize. See 2.1.1. "Hardware Initialization”
        step 5.    
    
    */

    // TODO: Reduce this
	_delay_ms(600);

   /*
        6. Si4703-C19 Errata Solution 2: Set RDSD = 0x0000. Note that this is a writable register 
   */

    // TODO: Is this necessary? SHould be set to zero on power up, so maybe this only applies to coming out of sleep?

	set_shadow_reg(REGISTER_15, 0x0000 );
	si4702_write_registers();
    
    
    /*
        Write address 02h (required).
         Set the DMUTE bit to disable mute. Optionally mute can be disabled later when audio is needed.
         Set the ENABLE bit high to set the powerup state.
         Set the DISABLE bit low to set the powerup state.
        Example: Write data 4001h.    
        
    */   
    
    
	set_shadow_reg(REGISTER_02, 0x4001 );           
	si4702_write_registers();
    
    /*
    si4702_read_registers();
    
    if ( get_shadow_reg(REGISTER_02) & 0x4000 == 0x04000 ) {
        debugBlink(3);
    }            
    
    debugBlink(4);
 
    */

    /*
        Wait for device powerup (required).
        Refer to the Powerup Time specification in Table 7 "FM Characteristics" of the data sheet.
    */

    // HUH? Not in table 7 or anywhere. Thaks for nothing. 
    
    // TODO: Reduce this...
    _delay_ms(500);
    
    // Next lets set volume full blast...
    
    // Set bottom 4 bits of 05. 1111 = 0 dBFS.
	set_shadow_reg(REGISTER_05, get_shadow_reg(REGISTER_05) | 0x0f );         
	si4702_write_registers();
    
    
    // Next lets tune a station

//	set_shadow_reg(REGISTER_02, _BV(15) | ((uint8_t) ( ( 93.9-87.5) / ( 0.200 )))  );           
	//si4702_write_registers();

	set_shadow_reg(REGISTER_03, 0x8050 );           
	si4702_write_registers();
    
    _delay_ms(500);
    
	set_shadow_reg(REGISTER_03, 0x0050 );           
	si4702_write_registers();

    
    debugBlink(6);

	/*
	 * Register 02 default settings:
	 *	- Softmute enable
	 *	- Mute enable
	 *	- Mono
	 *	- Wrap on band edges during seek
	 *	- Seek up
	 */
	set_shadow_reg(REGISTER_02, 0xE201);       
// 	si4702_write_registers();
     

	/*
	 * Set deemphasis based on eeprom.
	 */
    
	set_shadow_reg(REGISTER_04, get_shadow_reg(REGISTER_04) | (eeprom_read_byte(EEPROM_DEEMPHASIS) ? 0x0800 : 0x0000));
    
	set_shadow_reg(REGISTER_05,
			(SEEK_RSSI_THRESHOLD << 8) |
			(((uint16_t)(eeprom_read_byte(EEPROM_BAND) & 0x03)) << 6) |
			(((uint16_t)(eeprom_read_byte(EEPROM_SPACING) & 0x03)) << 4));

	/*
	 * Set the seek SNR and impulse detection thresholds.
	 */
	set_shadow_reg(REGISTER_06,
			(SEEK_SNR_THRESHOLD << 4) | SEEK_IMPULSE_THRESHOLD);

	si4702_write_registers();

	_delay_ms(110);

	/*
	 * It looks like the radio tunes to <something> once enabled.
	 * Make sure the STC bit is cleared by clearing the TUNE bit.
	 */
    
    
	si4702_read_registers();
	set_shadow_reg(REGISTER_03, 0x0000);
	si4702_write_registers();


	tune_direct(eeprom_read_word(EEPROM_CHANNEL));

	set_shadow_reg(REGISTER_05, (get_shadow_reg(REGISTER_05) & ~0x000f) |
				(eeprom_read_byte(EEPROM_VOLUME) & 0x0f));

	//si4702_write_registers();
    
    
    
    while (1) {
        
        for(uint8_t chan=1; chan<100;chan++) {
            
        	//tune_direct(chan);
            debugBlink(1);
            _delay_ms(1000);
        
        };
        }        
}

/*

To place device in power down mode:

1. (Optional) Set the AHIZEN bit high to maintain a dc bias of
0.5 x VIO volts at the LOUT and ROUT pins while in
powerdown, but preserve the states of the other bits in
Register 07h. Note that in powerup the LOUT and ROUT
pins are set to the common mode voltage specified in
Table 8 on page 12, regardless of the state of AHIZEN.

2. Set the ENABLE bit high and the DISABLE bit high to
place the device in powerdown mode. Note that all register
states are maintained so long as VIO is supplied and the
RST pin is high.

3. (Optional) Remove RCLK.

4. Remove VA and VD supplies as needed.                                                                     

We will only do #2

*/


void si4702_shutdown(void) {

	set_shadow_reg(REGISTER_02, _BV(6) | _BV(0) );      // Set both ENABLE and DISABLE to enter powerdown mode
	si4702_write_registers();    
    
}    


void debug_slowblink(void) {
    while (1) {
            
        SBI( PORTB , LED_DRIVE_BIT);
        _delay_ms(100);
        CBI( PORTB , LED_DRIVE_BIT);
        _delay_ms(900);
            
    }    
}    

void debug_fastblink(void) {
    while (1) {
            
        SBI( PORTB , LED_DRIVE_BIT);
        _delay_ms(100);
        CBI( PORTB , LED_DRIVE_BIT);
        _delay_ms(100);
            
    }    
}    


// Goto bed, will only wake up on button press interrupt (if enabled)

void deepSleep(void) {    
	set_sleep_mode( SLEEP_MODE_PWR_DOWN );
    sleep_enable();
    sleep_cpu();        // Good night    
}   



// Setup pin change interrupt on button 

void initButton(void) 
{
 
     SBI( PORTB , BUTTON_INPUT_BIT);              // Enable pull up on button 
    
    /*
        PCIE: Pin Change Interrupt Enable
        When the PCIE bit is set (one) and the I-bit in the Status Register (SREG) is set (one), pin change interrupt is
        enabled. Any change on any enabled PCINT[5:0] pin will cause an interrupt. The corresponding interrupt of Pin
        Change Interrupt Request is executed from the PCI Interrupt Vector.         
    */
    
    SBI( GIMSK , PCIE );   // turns on pin change interrupts
    
    /*
        PCINT[5:0]: Pin Change Enable Mask 5:0
        Each PCINT[5:0] bit selects whether pin change interrupt is enabled on the corresponding I/O pin. If PCINT[5:0] is
        set and the PCIE bit in GIMSK is set, pin change interrupt is enabled on the corresponding I/O pin. If PCINT[5:0] is
        cleared, pin change interrupt on the corresponding I/O pin is disabled.    
    */
    
    SBI( PCMSK , BUTTON_PCINT_BIT );
    
    sei();                 // enables interrupts
}    


// Returns true if button is down

static inline uint8_t buttonDown(void) {
    return( !TBI( PINB , BUTTON_INPUT_BIT ));           // Button is pulled high, short to ground when pressed
}    


// Called on button press

/*

    How it works. 
    
    Short press advances to netx station.

*/

ISR( PCINT0_vect )
{
    
    _delay_ms( BUTTON_DEBOUNCE_MS );        // Debounce down
    
    if (buttonDown()) {                     // We only care about button down events
        
        uint16_t currentChan = currentChanFromShadow();

        uint16_t countdown = LONG_PRESS_MS;
    
        while (countdown && buttonDown()) {       // Spin until either long press timeout or they let go
            _delay_ms(1);          
            countdown--;
        }        
    
        if (countdown) {                            // Did not timeout, so short press
        
            // Advance to next station
        
            tune_direct(  currentChan + 1 );
        
        
        } else {            // Initiated a long-press save to EEPROM            


            // User feedback of long press with 200ms flash on LED
            SBI(PORTB , LED_DRIVE_BIT );                         
            
            update_channel( currentChan );            
            _delay_ms(200);
            
            CBI(PORTB , LED_DRIVE_BIT );
    
           
        }    
        
        while (buttonDown());                   // Wait for button release
        
        _delay_ms( BUTTON_DEBOUNCE_MS );        // Debounce up
        
    }    
}



int main(void)
{
	//PORTB |= 0x08;	/* Enable pull up on PB3 for Button */ 

    // Set up the reset line to the FM_IC and AMP first so they are quiet. 
    // This eliminates the need for the external pull-down on this line. 
    
    
    SBI( DDRB , FMIC_RESET_BIT);    // drive reset low, makes them sleep            
        
    // TODO: Enable DDR on LED pin but for now just use pull-up
	SBI( DDRB , LED_DRIVE_BIT);    // Set LED pin to output, will default to low (LED off) on startup
    
    
	//PORTB |= 0x08;	/* Enable pull up on PB3 for Button */ 
    
	//DDRB |= _BV(LED_DRIVE_BIT);     /* Set LED pin PB4 to output */
    
    // Flash LED briefly just to visually indicate successful power up
    SBI( PORTB , LED_DRIVE_BIT);
    _delay_ms(100);
    CBI( PORTB , LED_DRIVE_BIT);
    
    
	//DDRB |= _BV(OCR1B);     /* Set LED pin PB4 to output */
    
    adc_on();
    
    if (!VCC_GT(LOW_BATTERY_VOLTAGE)) {
        
        adc_off();  // Mind as well save some power 
        
        // indicate dead battery with a 10%, 1Hz blink
        
        // TODO: Probably only need to blink for a minute and then go into deep sleep?
            
        while (1) {
            
            SBI( PORTB , LED_DRIVE_BIT);
            _delay_ms(100);
            CBI( PORTB , LED_DRIVE_BIT);
            _delay_ms(900);
            
        }
        
        // We are not driving any pins except the RESET to keep the FM_IC and AMP asleep
        // and the LED which is low, so no Wasted power
        
        deepSleep();
        // Never get here since button int not enabled.
        // 
        
    }   
    
    
    if (programmingVoltagePresent()) {          
        
        // Ok, we are currently being powered by a programmer since a battery could not make the voltage go this high
        
        while (1) {
            
            
            uint8_t r1;
            
            // Try to read in two bytes from the programmer. 
            // This will timeout and fail if we wait longer then 40ms
            // in which case we will start listening again for next transmit
                        
            r1=readPbyte();
            
            if (r1>=0) {
                
                uint8_t r2;
                
                r2=readPbyte();
                    
                if (r2>=0) {
                    
                    // If we got here, then we received two bytes from the programmer
                    
                    uint16_t channel = (r1<<8) | (r2);      // build the channel word, MSB first. 
                       
                    update_channel(channel);                // Update the EEPROM and recalc the CRC
                    
                    // Done programming! Signal to the world with a fast blink!
                    
                    while (1) {

                        PORTB|=_BV(LED_DRIVE_BIT);
                        _delay_ms(50);
                        PORTB&=~_BV(LED_DRIVE_BIT);
                        _delay_ms(50);
                        
                    }
                    
                }
                
            }                                                            
            
            
        }
        
        // Never get here, there is no way out of programming mode except power cycle                    
        
    }     
    
    
    adc_off();      /// All done with the ADC, so same a bit of power      
    

    // Normal operation from here (good battery voltage, not connected to a programmer)
    
       
                        
    //_delay_ms(5000);                        
	//timer0_init();

	//timer1_init();

    // TODO: Instate this test
	//check_eeprom();

	si4702_init();
    
    // TODO: Soft fade in the volume to avoid the "click" at turnon?
    //while(1);
        
    
    initButton();
    
    while(1);

	//sei();
    
    
    // TODO: Set up the button ISR to catch presses when sleeping. 
    
    // disable the FM chip
        
    //si4702_shutdown();
                     
    //PORTB &= ~_BV(1);       // Drive RESET low on the amp and FM chips, puts them to sleep
    
    //DDRB = _BV(1);         // Only drive reset.
    
    // TODO: DO the breathing LED here for a while, driven off the timer Overflow
    //while (1);   
    
    deepSleep();
    
        

	/*
	 * Set the sleep mode to idle when sleep_mode() is called.
	 * This stops the CPU, but keeps all the peripherals and
	 * the main oscillator running.
	 * Most importantly, Timers 0 & 1 and i2c keep going.
	 */
	set_sleep_mode(SLEEP_MODE_IDLE);
     

	while(1) {
		uint16_t current_chan;

		/*
		 * Foreground loop, where most of the intricate work gets done.
		 * Operations like reading and writing to the 4702 via i2c
		 * should happen here and not in interrupt context.
		 */

		/*
		 * Sleep until woken by Timer 0. That means this loop will
		 * run approx once per 10mS (less often if it takes
		 * longer than that to get through this loop - duh.)
		 */

		sleep_mode();

		/*
		 * Button pushes will manipulate current_mode, track
		 * and act upon the transitions here.
		 */
		ATOMIC_BLOCK(ATOMIC_RESTORESTATE)
		{
			switch (current_mode) {

			    case SAVE:
				if (current_chan != eeprom_read_word(EEPROM_CHANNEL)) {
					update_channel(current_chan);
				}
				display_mode = current_mode = NORMAL;
				break;

			    case FACTORY_CONFIRM:
				copy_factory_param();
				tune_direct(eeprom_read_word(EEPROM_CHANNEL));
				display_mode = current_mode = NORMAL;
				break;

			    default:
				break;
			}

			switch (display_mode) {
			    case TUNE:
				/*
				 * When in tune mode, flash 320mS on/off
				 */
				OCR1B = (ticks & 32) ? OCR1C : 0;
				break;

			    case FACTORY_RESET:
				/*
				 * When in factory reset mode, flash
				 * 160mS on/off.
				 */
				OCR1B = (ticks & 16) ? OCR1C : 0;
				break;

			    case SAVE:
				/*
				 * When ready to save current channel
				 * or confirm factory reset, on solid.
				 */
				OCR1B = OCR1C;
				break;

			}

			if ((current_mode != NORMAL) &&
					((ticks - last_release) > TIMEOUT)) {
				display_mode = current_mode = NORMAL;
			}
		}

	}
}
