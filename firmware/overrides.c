#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "config.h"

#include "configstring.h"
#include "keyboard.h"
#include "uart.h"
#include "spi.h"
#include "minfat.h"
#include "interrupts.h"
#include "ps2.h"
#include "user_io.h"
#include "osd.h"
#include "menu.h"
#include "font.h"
#include "cue_parser.h"
#include "pcecd.h"


#define MIST_SET_CONTROL 0x04

extern unsigned int statusword; /* So we can toggle the write-protect bits and provoke diskchanges. */

/* Override this since the status is sent to dataio, not userio */
void sendstatus(int statusword)
{
	SPI(0xff);
	SPI_ENABLE(HW_SPI_FPGA);
	SPI(MIST_SET_CONTROL); // Read conf string command
	SPI(statusword>>24);
	SPI(statusword>>16);
	SPI(statusword>>8);
	SPI(statusword);
	SPI_DISABLE(HW_SPI_FPGA);
}

// STe always has a blitter
// wire       viking_en = system_ctrl[28];
// wire [8:0] acsi_enable = system_ctrl[17:10];


char *configstring="Atari ST;;"
	"S0U,ST ,Floppy A:;"
	"S1U,ST ,Floppy B:;"
	"O67,Write protect,Off,A:,B:,Both;"
//	"O45,CPU,68000,68020;"
	"F,IMGROM,Load ROM;"
	"P1,ST Configuration;"
	"P1O13,RAM (need Hard Reset),512K,1MB,2MB,4MB,8MB,14MB;"
	"P1O8,Video mode,Mono,Colour;"
	"P1ONO,Chipset,ST,STE,MegaSTE;"
	"P1OJ,ST Blitter,Off,On;"
	"P1OM,Stereo sound,Off,On;"
	"P1OKL,Scanlines,Off,25%,50%,75%;"
	"P1OT,Composite blend,Off,On;"
	"T0,Reset (Hold for hard reset);"
	"V,v3.40.";
static char *cfgptr;

int configstring_next()
{
	char result=0;
	if(cfgptr)
		result=*cfgptr++;
	if(!result)
		cfgptr=0;
	return(result);
}

void configstring_begin()
{
	cfgptr=configstring;
}


/* Key -> gamepad mapping.  We override this to swap buttons A and B for NES. */

unsigned char joy_keymap[]=
{
	KEY_CAPSLOCK,
	KEY_LSHIFT,
	KEY_LCTRL,
	KEY_ALT,
	KEY_W,
	KEY_S,
	KEY_A,
	KEY_D,
	KEY_ENTER,
	KEY_RSHIFT,
	KEY_RCTRL,
	KEY_ALTGR,
	KEY_UPARROW,
	KEY_DOWNARROW,
	KEY_LEFTARROW,
	KEY_RIGHTARROW,
};

#define DIRECTUPLOAD 0x10

/* Initial ROM */
const char *bootrom_name="TOS     IMG";
extern unsigned char coretype;
extern unsigned char romtype;
extern fileTYPE file;

void clearram(int size)
{
	int i;
	SPI_ENABLE(HW_SPI_FPGA);
	SPI(SPI_FPGA_FILE_INDEX);
	SPI(0x3); /* Memory clear */
	SPI_DISABLE(HW_SPI_FPGA);

	SPI_ENABLE(HW_SPI_FPGA);
	SPI(SPI_FPGA_FILE_TX);
	SPI(0x01); /* Upload */
	SPI_DISABLE(HW_SPI_FPGA);

	SPI_ENABLE_FAST_INT(HW_SPI_FPGA);
	SPI(SPI_FPGA_FILE_TX_DAT);
	for(i=0;i<size;++i)
		SPI(0x00);
	SPI_DISABLE(HW_SPI_FPGA);

	SPI_ENABLE(HW_SPI_FPGA);
	SPI(SPI_FPGA_FILE_TX);
	SPI(0x00); /* End Upload */
	SPI_DISABLE(HW_SPI_FPGA);
}


void setromtype(char *filename)
{
	if(FileOpen(&file,filename))
	{
		switch(file.size>>10)
		{
			case 256:
				romtype=0;
//				puts("256k");
				break;
			case 192:
//				puts("192k");
				romtype=1;
				break;
			default:
				break;
		}
	}
}


char *autoboot()
{
	char *result=0;
	int s;
	coretype=0;//DIRECTUPLOAD;
	romtype=1;
	configstring_index=0;
	sendstatus(1);
	clearram(16384);
	setromtype(bootrom_name);
	LoadROM(bootrom_name);

	sendstatus(0);
	s=GetTimer(400);
	while(!CheckTimer(s))
		;
	sendstatus(1);
	s=GetTimer(100);
	while(!CheckTimer(s))
		;
	sendstatus(0);
	return(result);
}

void buildmenu(int offset);

unsigned char mouseinit[]=
{	
	0xff,
//	0xf4,0, // Uncomment this line to leave the mouse in 3-byte mode
	0xf3,200,
	0xf3,100,
	0xf3,80,
	0xf2,0x01,
	0xf4,0
};
void handlemouse()
{
	int byte;
	static int resp;
	static int init=0;
	if(!init)
	{
		while(PS2MouseRead()>-1)
			; // Drain the buffer;
		++init;
	}
	else if(init&1)
	{
		byte=mouseinit[init>>1];
		if(byte)
		{
//			printf("Sending %x, expecting %d bytes\n",byte,resp);
			if(byte>1)
				PS2MouseWrite(byte);
			++init;
		}
	}
	else
	{
		byte=PS2MouseRead();
		if(byte==0xaa || byte==0xfa || byte==0x03)
			++init;
//		if(byte>=0)
//			printf("response: %x\n",byte);
	}
}

void cycle(int row);
void toggle(int row)
{
	cycle(row);
	if(menu_longpress)
		clearram(16384);
	cycle(row);
}

#define STATUS_WP_UNIT0 6
#define STATUS_WP_UNIT1 7

void toggle_wp(int unit)
{
	unsigned int s=statusword;
	if(unit)
		s^=1<<STATUS_WP_UNIT1;
	else
		s^=1<<STATUS_WP_UNIT0;
//	printf("Sending alt status %x\n",s);
	sendstatus(s);

	s=GetTimer(500);
	while(!CheckTimer(s))
		;
}


void loadimage(char *filename,int unit)
{
	switch(unit)
	{
		case 0:
			if(filename)
			{
				sendstatus(statusword|1);
				setromtype(filename);
				LoadROM(filename);
			}
			break;
		case '0':
		case '1':
			diskimg_mount(0,unit-'0');				
			toggle_wp(unit-'0');
			diskimg_mount(filename,unit-'0');				
			break;
	}
	sendstatus(statusword);
}


int main(int argc,char **argv)
{
	int havesd;
	int i,c;
	int osd=0;
	char *err;

	PS2Init();

	SPI(0xff);

	if(havesd=sd_init() && FindDrive())
		puts("OK");

	buildmenu(0);

	if(err=autoboot())
	{
	}

	EnableInterrupts();

	while(1)
	{
		handlemouse();
		Menu_Run();

#ifdef CONFIG_DISKIMG
		diskimg_poll();
#endif
	}

	return(0);
}

