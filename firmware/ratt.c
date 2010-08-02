#include <avr/io.h>      // this contains all the IO port definitions
#include <avr/interrupt.h>
#include <util/delay.h>
#include <avr/pgmspace.h>
#include <avr/eeprom.h>
#include <avr/wdt.h>
#include <string.h>
#include <i2c.h>
#include <stdlib.h>
#include "util.h"
#include "ratt.h"
#include "ks0108.h"
#include "glcd.h"
#include "deathclock.h"

volatile uint8_t time_s, time_m, time_h;
volatile uint8_t old_h, old_m;
volatile uint8_t timeunknown = 1;
volatile uint8_t date_m, date_d, date_y;
volatile uint8_t death_m, death_d, death_y;
volatile uint8_t alarming, alarm_on, alarm_tripped, alarm_h, alarm_m;
volatile uint8_t displaymode;
volatile uint8_t volume;
volatile uint8_t sleepmode = 0;
volatile uint8_t region;
volatile uint8_t time_format;
extern volatile uint8_t screenmutex;
volatile uint8_t minute_changed = 0, hour_changed = 0;
volatile uint8_t score_mode_timeout = 0;
volatile uint8_t score_mode = SCORE_MODE_TIME;

volatile int32_t minutes_left=0;
volatile int32_t old_minutes_left;
volatile uint8_t dc_mode;

// These store the current button states for all 3 buttons. We can 
// then query whether the buttons are pressed and released or pressed
// This allows for 'high speed incrementing' when setting the time
extern volatile uint8_t last_buttonstate, just_pressed, pressed;
extern volatile uint8_t buttonholdcounter;

extern volatile uint8_t timeoutcounter;

// How long we have been snoozing
uint16_t snoozetimer = 0;

SIGNAL(TIMER1_OVF_vect) {
  PIEZO_PORT ^= _BV(PIEZO);
}

volatile uint16_t millis = 0;
volatile uint16_t animticker, alarmticker;
SIGNAL(TIMER0_COMPA_vect) {
  if (millis)
    millis--;
  if (animticker)
    animticker--;

  if (alarming && !snoozetimer) {
    if (alarmticker == 0) {
      alarmticker = 300;
      if (TCCR1B == 0) {
	TCCR1A = 0; 
	TCCR1B =  _BV(WGM12) | _BV(CS10); // CTC with fastest timer
	TIMSK1 = _BV(TOIE1) | _BV(OCIE1A);
	OCR1A = (F_CPU / ALARM_FREQ) / 2;
      } else {
	TCCR1B = 0;
	// turn off piezo
	PIEZO_PORT &= ~_BV(PIEZO);
      }
    }
    alarmticker--;    
  }
}

uint32_t load_raw_etd(void)
{
  dc_mode = eeprom_read_byte((uint8_t *)EE_DC_MODE);
  return ETD(  eeprom_read_byte((uint8_t *)EE_DOB_MONTH),
                              eeprom_read_byte((uint8_t *)EE_DOB_DAY),
                              eeprom_read_byte((uint8_t *)EE_DOB_YEAR)+1900,
                              eeprom_read_byte((uint8_t *)EE_SET_MONTH),
                              eeprom_read_byte((uint8_t *)EE_SET_DAY),
                              eeprom_read_byte((uint8_t *)EE_SET_YEAR)+2000,
                              eeprom_read_byte((uint8_t *)EE_GENDER),
                              dc_mode,
                              BodyMassIndex( eeprom_read_byte((uint8_t *)EE_BMI_UNIT), eeprom_read_word((uint16_t *)EE_BMI_HEIGHT), eeprom_read_word((uint16_t *)EE_BMI_WEIGHT)),
                              eeprom_read_byte((uint8_t *)EE_SMOKER),
                              eeprom_read_byte((uint8_t *)EE_SET_HOUR),
                              eeprom_read_byte((uint8_t *)EE_SET_MIN),
                              eeprom_read_byte((uint8_t *)EE_SET_SEC));
}

void load_etd(void)
{
  uint32_t result = load_raw_etd();
      result -= date_diff( eeprom_read_byte((uint8_t *)EE_SET_MONTH),
                           eeprom_read_byte((uint8_t *)EE_SET_DAY),
                           eeprom_read_byte((uint8_t *)EE_SET_YEAR)+2000,
                           date_m,date_d,date_y+2000) * 1440l * ((dc_mode == DC_mode_sadistic)?4:1);
  result -= (time_h * 60) * ((dc_mode == DC_mode_sadistic)?4:1);
  result -= (time_m) * ((dc_mode == DC_mode_sadistic)?4:1);
  minutes_left = (int32_t)result;
}

void calc_death_date(void)
{
	uint32_t timeleft;
	death_m = eeprom_read_byte((uint8_t *)EE_SET_MONTH);
	death_d = eeprom_read_byte((uint8_t *)EE_SET_DAY);
	death_y = eeprom_read_byte((uint8_t *)EE_SET_YEAR);
	timeleft = load_raw_etd();
	
	while (timeleft >= 1440)
      {
        timeleft -= 1440;
        death_d++;  
        if ((death_d > 31) ||
               ((death_d == 31) && ((death_m == 4)||(death_m == 6)||(death_m == 9)||(death_m == 11))) ||
               ((death_d == 30) && (death_m == 2)) ||
               ((death_d == 29) && (death_m == 2) && !leapyear(2000+death_y))) {
                 death_d = 1;
                 death_m++;
            }
            if(death_m > 12)
            {
              death_m=1;
              death_y++;
            } 
      }
}

void init_eeprom(void) {	//Set eeprom to a default state.
  if(eeprom_read_byte((uint8_t *)EE_INIT) != EE_INITIALIZED) {
    eeprom_write_byte((uint8_t *)EE_ALARM_HOUR, 8);
    eeprom_write_byte((uint8_t *)EE_ALARM_MIN, 0);
    eeprom_write_byte((uint8_t *)EE_BRIGHT, OCR2A_VALUE);
    eeprom_write_byte((uint8_t *)EE_VOLUME, 1);
    eeprom_write_byte((uint8_t *)EE_REGION, REGION_US);
    eeprom_write_byte((uint8_t *)EE_TIME_FORMAT, TIME_12H);
    eeprom_write_byte((uint8_t *)EE_SNOOZE, 10);
    
    //Death Clock Variables - Initial state being set to CaitSith2's specific stats :)
    eeprom_write_byte((uint8_t *)EE_DOB_MONTH, 11);
    eeprom_write_byte((uint8_t *)EE_DOB_DAY, 14);
    eeprom_write_byte((uint8_t *)EE_DOB_YEAR, 80);
    eeprom_write_byte((uint8_t *)EE_SET_MONTH, 7);
    eeprom_write_byte((uint8_t *)EE_SET_DAY, 28);
    eeprom_write_byte((uint8_t *)EE_SET_YEAR, 10);
    eeprom_write_byte((uint8_t *)EE_SET_HOUR, 20);
    eeprom_write_byte((uint8_t *)EE_SET_MIN, 05);
    eeprom_write_byte((uint8_t *)EE_SET_SEC, 25);
    eeprom_write_byte((uint8_t *)EE_GENDER, DC_gender_male);
    eeprom_write_byte((uint8_t *)EE_DC_MODE, DC_mode_normal);
    eeprom_write_byte((uint8_t *)EE_BMI_UNIT, BMI_Imperial);
    eeprom_write_word((uint16_t *)EE_BMI_WEIGHT, 400);
    eeprom_write_word((uint16_t *)EE_BMI_HEIGHT, 78); // 6 foot 6.
    eeprom_write_byte((uint8_t *)EE_SMOKER, DC_non_smoker);
    
    eeprom_write_byte((uint8_t *)EE_INIT, EE_INITIALIZED);
  }
}

int main(void) {
  uint8_t inverted = 0;
  uint8_t mcustate;
  uint8_t display_date = 0;
  uint8_t display_death_date = 0;
  int16_t scroller;

  // check if we were reset
  mcustate = MCUSR;
  MCUSR = 0;
  
  //Just in case we were reset inside of the glcd init function
  //which would happen if the lcd is not plugged in. The end result
  //of that, is it will beep, pause, for as long as there is no lcd
  //plugged in.
  wdt_disable();

  // setup uart
  uart_init(BRRL_192);
  DEBUGP("RATT Clock");

  // set up piezo
  PIEZO_DDR |= _BV(PIEZO);

  DEBUGP("clock!");
  clock_init();

  //beep(4000, 100);

  init_eeprom();
  region = eeprom_read_byte((uint8_t *)EE_REGION);
  time_format = eeprom_read_byte((uint8_t *)EE_TIME_FORMAT);
  DEBUGP("buttons!");
  initbuttons();

  setalarmstate();

  // setup 1ms timer on timer0
  TCCR0A = _BV(WGM01);
  TCCR0B = _BV(CS01) | _BV(CS00);
  OCR0A = 125;
  TIMSK0 |= _BV(OCIE0A);

  // turn backlight on
  DDRD |= _BV(3);
#ifndef BACKLIGHT_ADJUST
  PORTD |= _BV(3);
#else
  TCCR2A = _BV(COM2B1); // PWM output on pin D3
  TCCR2A |= _BV(WGM21) | _BV(WGM20); // fast PWM
  TCCR2B |= _BV(WGM22);
  OCR2A = OCR2A_VALUE;
  OCR2B = eeprom_read_byte((uint8_t *)EE_BRIGHT);
#endif

  DDRB |= _BV(5);
  beep(4000, 100);
  
  //glcdInit locks and disables interrupts in one of its functions.  If the LCD is not
  //plugged in, glcd will run forever.  For good reason, it would be desirable to know
  //that the LCD is plugged in and working correctly as a result.  This is why we are
  //using a watch dog timer.  The lcd should initialized in way less than 500 ms.
  wdt_enable(WDTO_2S);
  glcdInit();
  glcdClearScreen();
  
  glcdFillRectangle(0, 0, GLCD_XPIXELS, GLCD_YPIXELS, 1);
  for(scroller=-84;scroller<212;scroller++)
  {
    render_image (SKULL,scroller,1);
    _delay_ms(16);
    if(scroller==28)
    	_delay_ms(2000);
  }
  

  initanim();
  initdisplay(0);
  load_etd();	//Only need to do this once at power on, and once if Death Clock settings are changed.

  while (1) {
    animticker = ANIMTICK_MS;

    // check buttons to see if we have interaction stuff to deal with
	if(just_pressed && alarming)
	{
	  just_pressed = 0;
	  setsnooze();
	}
	if(display_date && !score_mode_timeout)
	{
	  score_mode = SCORE_MODE_YEAR;
	  score_mode_timeout = 3;
	  setscore();
	  display_date = 0;
	}
	
	if(display_death_date && !score_mode_timeout)
	{
	  score_mode = SCORE_MODE_DEATH_YEAR;
	  score_mode_timeout = 3;
	  setscore();
	  display_death_date = 0;
	}

    //Was formally set for just the + button.  However, because the Set button was never
    //accounted for, If the alarm was turned on, and ONLY the set button was pushed since then,
    //the alarm would not sound at alarm time, but go into a snooze immediately after going off.
    //This could potentially make you late for work, and had to be fixed.
	if (just_pressed & 0x2) {
	  just_pressed = 0;
	  if(score_mode < SCORE_MODE_DEATH_TIME)
	  {
	    display_date = 1;
	    score_mode = SCORE_MODE_DATE;
	  }
	  else
	  {
	  	display_death_date = 1;
	    calc_death_date();
	    score_mode = SCORE_MODE_DEATH_DATE;
	  }
	  score_mode_timeout = 3;
	  setscore();
	}
	
	if (just_pressed & 0x4) {
	  just_pressed = 0;
	  if(score_mode < SCORE_MODE_DEATH_TIME)
	    score_mode = SCORE_MODE_DEATH_TIME;
	  else
	  	score_mode = SCORE_MODE_TIME;
	  score_mode_timeout = 3;
	  setscore();
	}

    if (just_pressed & 0x1) {
      just_pressed = 0;
      switch(displaymode) {
      case (SHOW_TIME):
	displaymode = SET_ALARM;
	set_alarm();
	break;
      case (SET_ALARM):
	displaymode = SET_TIME;
	set_time();
	if(hour_changed) hour_changed = 0;
	timeunknown = 0;
	break;
      case SET_TIME:
	displaymode = SET_DATE;
	set_date();
	break;
      case SET_DATE:
    load_etd();	//So that the calculated time of death is fresh, after changing the date/time. :)
	displaymode = SET_REGION;
	set_region();
	break;
	  case SET_REGION:
#ifdef BACKLIGHT_ADJUST
	displaymode = SET_BRIGHTNESS;
	set_backlight();
	break;
	  case SET_BRIGHTNESS:
#endif
	displaymode = SET_DEATHCLOCK_DOB;
	set_deathclock_dob();
	break;
	  case SET_DEATHCLOCK_DOB:
	displaymode = SET_DEATHCLOCK_GENDER;
	set_deathclock_gender();
	break;
	  case SET_DEATHCLOCK_GENDER:
	displaymode = SET_DEATHCLOCK_MODE;
	set_deathclock_mode();
	break;
	  case SET_DEATHCLOCK_MODE:
	displaymode = SET_DEATHCLOCK_BMI;
	set_deathclock_bmi();
	break;
	  case SET_DEATHCLOCK_BMI:
	displaymode = SET_DEATHCLOCK_SMOKER;
	set_deathclock_smoker();
	break;
      default:
	displaymode = SHOW_TIME;
	glcdClearScreen();
	initdisplay(0);
      }

      if (displaymode == SHOW_TIME) {
	glcdClearScreen();
	initdisplay(0);
      }
    }
    step();
    if ((displaymode == SHOW_TIME)) {
    	
      if (! inverted && alarming && (time_s & 0x1)) {
	inverted = 1;
	initdisplay(inverted);
      }
      else if ((inverted && ! alarming) || (alarming && inverted && !(time_s & 0x1))) {
	inverted = 0;
	initdisplay(0);
      } else {
	PORTB |= _BV(5);
	draw(inverted);
	PORTB &= ~_BV(5);
    }
  }
  
    while (animticker);
    //uart_getchar();  // you would uncomment this so you can manually 'step'
  }

  halt();
}


SIGNAL(TIMER1_COMPA_vect) {
  PIEZO_PORT ^= _BV(PIEZO);
}

void beep(uint16_t freq, uint8_t duration) {
  // use timer 1 for the piezo/buzzer 
  TCCR1A = 0; 
  TCCR1B =  _BV(WGM12) | _BV(CS10); // CTC with fastest timer
  TIMSK1 = _BV(TOIE1) | _BV(OCIE1A);
  OCR1A = (F_CPU / freq) / 2;
  _delay_ms(duration);
  TCCR1B = 0;
  // turn off piezo
  PIEZO_PORT &= ~_BV(PIEZO);
}

// This turns on/off the alarm when the switch has been
// set. It also displays the alarm time
void setalarmstate(void) {
  DEBUGP("a");
  if (ALARM_PIN & _BV(ALARM)) { 
    if (alarm_on) {
      // turn off the alarm
      alarm_on = 0;
      alarm_tripped = 0;
      snoozetimer = 0;
      if (alarming) {
	// if the alarm is going off, we should turn it off
	// and quiet the speaker
	DEBUGP("alarm off");
	alarming = 0;
	TCCR1B = 0;
	// turn off piezo
	PIEZO_PORT &= ~_BV(PIEZO);
      } 
    }
  } else {
    // Don't display the alarm/beep if we already have
    if  (!alarm_on) {
      // alarm on!
      alarm_on = 1;
      // reset snoozing
      snoozetimer = 0;
      if(score_mode < SCORE_MODE_DEATH_TIME)
	  	score_mode = SCORE_MODE_ALARM;
	  else
	  	score_mode = SCORE_MODE_DEATH_ALARM;
	  score_mode_timeout = 3;
	  setscore();
      DEBUGP("alarm on");
    }   
  }
}


void drawArrow(uint8_t x, uint8_t y, uint8_t l) {
  glcdFillRectangle(x, y, l, 1, ON);
  glcdSetDot(x+l-2,y-1);
  glcdSetDot(x+l-2,y+1);
  glcdSetDot(x+l-3,y-2);
  glcdSetDot(x+l-3,y+2);
}

void printnumber_1d(uint8_t n, uint8_t inverted) {
  glcdWriteChar(n%10+'0', inverted);
}

void printnumber_2d(uint16_t n, uint8_t inverted) {
  glcdWriteChar(n/10+'0', inverted);
  glcdWriteChar(n%10+'0', inverted);
}

void printnumber_3d(uint16_t n, uint8_t inverted) {
  glcdWriteChar(n/100+'0', inverted);
  glcdWriteChar((n%100)/10+'0', inverted);
  glcdWriteChar(n%10+'0', inverted);
}

uint8_t readi2ctime(void) {
  uint8_t regaddr = 0, r;
  uint8_t clockdata[8];
  
  // check the time from the RTC
  r = i2cMasterSendNI(0xD0, 1, &regaddr);

  if (r != 0) {
    DEBUG(putstring("Reading i2c data: ")); DEBUG(uart_putw_dec(r)); DEBUG(putstring_nl(""));
    while(1) {
      beep(4000, 100);
      _delay_ms(100);
      beep(4000, 100);
      _delay_ms(1000);
    }
  }

  r = i2cMasterReceiveNI(0xD0, 7, &clockdata[0]);

  if (r != 0) {
    DEBUG(putstring("Reading i2c data: ")); DEBUG(uart_putw_dec(r)); DEBUG(putstring_nl(""));
    while(1) {
      beep(4000, 100);
      _delay_ms(100);
      beep(4000, 100);
      _delay_ms(1000);
    }
  }

  time_s = ((clockdata[0] >> 4) & 0x7)*10 + (clockdata[0] & 0xF);
  time_m = ((clockdata[1] >> 4) & 0x7)*10 + (clockdata[1] & 0xF);
  if (clockdata[2] & _BV(6)) {
    // "12 hr" mode
    time_h = ((clockdata[2] >> 5) & 0x1)*12 + 
      ((clockdata[2] >> 4) & 0x1)*10 + (clockdata[2] & 0xF);
  } else {
    time_h = ((clockdata[2] >> 4) & 0x3)*10 + (clockdata[2] & 0xF);
  }
  
  date_d = ((clockdata[4] >> 4) & 0x3)*10 + (clockdata[4] & 0xF);
  date_m = ((clockdata[5] >> 4) & 0x1)*10 + (clockdata[5] & 0xF);
  date_y = ((clockdata[6] >> 4) & 0xF)*10 + (clockdata[6] & 0xF);

  return clockdata[0] & 0x80;
}

void writei2ctime(uint8_t sec, uint8_t min, uint8_t hr, uint8_t day,
		  uint8_t date, uint8_t mon, uint8_t yr) {
  uint8_t clockdata[8] = {0,0,0,0,0,0,0,0};

  clockdata[0] = 0; // address
  clockdata[1] = i2bcd(sec);  // s
  clockdata[2] = i2bcd(min);  // m
  clockdata[3] = i2bcd(hr); // h
  clockdata[4] = i2bcd(day);  // day
  clockdata[5] = i2bcd(date);  // date
  clockdata[6] = i2bcd(mon);  // month
  clockdata[7] = i2bcd(yr); // year
  
  uint8_t r = i2cMasterSendNI(0xD0, 8, &clockdata[0]);

  //DEBUG(putstring("Writing i2c data: ")); DEBUG(uart_putw_dec()); DEBUG(putstring_nl(""));

  if (r != 0) {
    while(1) {
      beep(4000, 100);
      _delay_ms(100);
      beep(4000, 100);
      _delay_ms(1000);
    }
  }

}

volatile uint8_t border_tick;
// runs at about 30 hz
uint8_t t2divider1 = 0, t2divider2 = 0;
SIGNAL (TIMER2_OVF_vect) {
  wdt_reset();
#ifdef BACKLIGHT_ADJUST
  if (t2divider1 == TIMER2_RETURN) {
#else
  if (t2divider1 == 5) {
#endif
    t2divider1 = 0;
  } else {
    t2divider1++;
    return;
  }
  border_tick++;
  //This occurs at 6 Hz

  uint8_t last_s = time_s;
  uint8_t last_m = time_m;
  uint8_t last_h = time_h;
  uint8_t last_dc_sadistic_s = ((dc_mode == DC_mode_sadistic)?(time_s/15):0);

  readi2ctime();
  
  if (time_h != last_h) {
    hour_changed = 1; 
    old_h = last_h;
    old_m = last_m;
    old_minutes_left = minutes_left;
    if(minutes_left>0)
      minutes_left-=((dc_mode==DC_mode_sadistic)?4:1);
  } else if (time_m != last_m) {
    minute_changed = 1;
    old_m = last_m;
    old_minutes_left = minutes_left;
    if(minutes_left>0)
      minutes_left-=((dc_mode==DC_mode_sadistic)?4:1);
  } else if ((dc_mode == DC_mode_sadistic) && (minutes_left != 0)) {
  	if (((dc_mode == DC_mode_sadistic)?(time_s/15):0) != last_dc_sadistic_s) {
  	  minute_changed = 1;
  	  old_m = last_m;
      old_minutes_left = minutes_left;
  	}
  }

  if (time_s != last_s) {
    if(alarming && snoozetimer)
	  snoozetimer--;

    if(score_mode_timeout) {
	  score_mode_timeout--;
	  if(!score_mode_timeout) {
	  	if(score_mode >= SCORE_MODE_DEATH_TIME)
	  	  score_mode = SCORE_MODE_DEATH_TIME;
	  	else
	      score_mode = SCORE_MODE_TIME;
	    if(hour_changed) {
	      time_h = old_h;
	      time_m = old_m;
	    } else if (minute_changed) {
	      time_m = old_m;
	    }
	    setscore();
	    if(hour_changed || minute_changed) {
	      time_h = last_h;
	      time_m = last_m;
	    }
	  }
	}


    DEBUG(putstring("**** "));
    DEBUG(uart_putw_dec(time_h));
    DEBUG(uart_putchar(':'));
    DEBUG(uart_putw_dec(time_m));
    DEBUG(uart_putchar(':'));
    DEBUG(uart_putw_dec(time_s));
    DEBUG(putstring_nl("****"));
  }

  if (((displaymode == SET_ALARM) ||
       (displaymode == SET_DATE) ||
       (displaymode == SET_REGION) ||
       (displaymode == SET_BRIGHTNESS)) &&
      (!screenmutex) ) {
      glcdSetAddress(MENU_INDENT + 10*6, 2);
      print_timehour(time_h, NORMAL);
      glcdWriteChar(':', NORMAL);
      printnumber_2d(time_m, NORMAL);
      glcdWriteChar(':', NORMAL);
      printnumber_2d(time_s, NORMAL);

      if (time_format == TIME_12H) {
	glcdWriteChar(' ', NORMAL);
	if (time_h >= 12) {
	  glcdWriteChar('P', NORMAL);
	} else {
	  glcdWriteChar('A', NORMAL);
	}
      }
  }

  // check if we have an alarm set
  if (alarm_on && (time_s == 0) && (time_m == alarm_m) && (time_h == alarm_h)) {
    DEBUG(putstring_nl("ALARM TRIPPED!!!"));
    alarm_tripped = 1;
  }
  
  //And wait till the score changes to actually set the alarm off.
  if(!minute_changed && !hour_changed && alarm_tripped) {
  	 DEBUG(putstring_nl("ALARM GOING!!!!"));
  	 alarming = 1;
  	 alarm_tripped = 0;
  }

  if (t2divider2 == 6) {
    t2divider2 = 0;
  } else {
    t2divider2++;
    return;
  }

  if (buttonholdcounter) {
    buttonholdcounter--;
  }

  if (timeoutcounter) {
    timeoutcounter--;
  }
}

uint8_t leapyear(uint16_t y) {
  return ( (!(y % 4) && (y % 100)) || !(y % 400));
}

void tick(void) {


}



inline uint8_t i2bcd(uint8_t x) {
  return ((x/10)<<4) | (x%10);
}


void clock_init(void) {
  // talk to clock
  i2cInit();


  if (readi2ctime()) {
    DEBUGP("uh oh, RTC was off, lets reset it!");
    writei2ctime(0, 0, 12, 0, 1, 1, 9); // noon 1/1/2009
   }

  readi2ctime();

  DEBUG(putstring("\n\rread "));
  DEBUG(uart_putw_dec(time_h));
  DEBUG(uart_putchar(':'));
  DEBUG(uart_putw_dec(time_m));
  DEBUG(uart_putchar(':'));
  DEBUG(uart_putw_dec(time_s));

  DEBUG(uart_putchar('\t'));
  DEBUG(uart_putw_dec(date_d));
  DEBUG(uart_putchar('/'));
  DEBUG(uart_putw_dec(date_m));
  DEBUG(uart_putchar('/'));
  DEBUG(uart_putw_dec(date_y));
  DEBUG(putstring_nl(""));

  alarm_m = eeprom_read_byte((uint8_t *)EE_ALARM_MIN) % 60;
  alarm_h = eeprom_read_byte((uint8_t *)EE_ALARM_HOUR) % 24;


  //ASSR |= _BV(AS2); // use crystal

  TCCR2B = _BV(CS22) | _BV(CS21) | _BV(CS20); // div by 1024
  // overflow ~30Hz = 8MHz/(255 * 1024)

  // enable interrupt
  TIMSK2 = _BV(TOIE2);

  sei();
}

void setsnooze(void) {
  //snoozetimer = eeprom_read_byte((uint8_t *)EE_SNOOZE);
  //snoozetimer *= 60; // convert minutes to seconds
  snoozetimer = MAXSNOOZE;
  TCCR1B = 0;
  // turn off piezo
  PIEZO_PORT &= ~_BV(PIEZO);
  DEBUGP("snooze");
  //displaymode = SHOW_SNOOZE;
  //_delay_ms(1000);
  displaymode = SHOW_TIME;
}

