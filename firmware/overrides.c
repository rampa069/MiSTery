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
#include "timer.h"
#include "diskimg.h"
#include "spi_sd.h"

#include "c64keys.c"
#include "acsi.c"

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
	"P1,Storage;"
	"P1S0U,ST ,Floppy A:;"
	"P1S1U,ST ,Floppy B:;"
	"P1O67,Write protect,Off,A:,B:,Both;"
//	"O45,CPU,68000,68020;"
	"P1OAB,Hard disks,None,Unit 0,Unit 1,Both;"
	"P1S2U,HDFVHD,Hardfile 0;"
	"P1S3U,HDFVHD,Hardfile 1;"
	"P2,ST Configuration;"
	"P2O13,RAM (need Hard Reset),512K,1MB,2MB,4MB,8MB,14MB;"
	"P2F,IMGROM,Load ROM;"
	"P2O8,Video mode,Mono,Colour;"
	"P2ONO,Chipset,ST,STE,MegaSTE;"
	"P2OJ,ST Blitter,Off,On;"
	"P3,Sound & Video;"
	"P3OKL,Scanlines,Off,25%,50%,75%;"
	"P3OT,Composite blend,Off,On;"
	"P3OM,Stereo sound,Off,On;"
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
int LoadROM(const char *fn);
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


void setromtype(const char *filename)
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
	coretype=0; //DIRECTUPLOAD;
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
#if 0
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

#endif

unsigned char initmouse[]=
{	
	0x1,0xff, // Send 1 byte reset sequence
	0x82,	// Wait for two bytes in return (in addition to the normal acknowledge byte)
//	1,0xf4,0, // Uncomment this line to leave the mouse in 3-byte mode
	8,0xf3,200,0xf3,100,0xf3,80,0xf2,1, // Send PS/2 wheel mode knock sequence...
	0x81,	// Receive device ID (should be 3 for wheel mice)
	1,0xf4,0	// Enable reporting.
};

void handlemouse(int reset)
{
	int byte;
	static int delay=0;
	static int timeout;
	static int init=0;
	static int idx=0;
	static int txcount=0;
	static int rxcount=0;
	if(reset)
		idx=0;

	if(!CheckTimer(delay))
		return;
	delay=GetTimer(20);

	if(!idx)
	{
		while(PS2MouseRead()>-1)
			; // Drain the buffer;
		txcount=initmouse[idx++];
		rxcount=0;
	}
	else
	{
		if(rxcount)
		{
			int q=PS2MouseRead();
			if(q>-1)
			{
//				printf("Received %x\n",q);
				--rxcount;
			}
			else if(CheckTimer(timeout))
				idx=0;
	
			if(!txcount && !rxcount)
			{
				int next=initmouse[idx++];
				if(next&0x80)
				{
					rxcount=next&0x7f;
//					printf("Receiving %x bytes",rxcount);
				}
				else
				{
					txcount=next;
//					printf("Sending %x bytes",txcount);
				}
			}
		}
		else if(txcount)
		{
			PS2MouseWrite(initmouse[idx++]);
			--txcount;
			rxcount=1;
			timeout=GetTimer(3500);	//3.5 seconds
		}
	}
}
#if 0
void DebugRow(int row, char *info)
{
	OsdWriteStart(row,0,0);
	OsdPutChar(' ');
	OsdPuts(info);
	OsdWriteEnd();
}

char debugtxt[32];

#define HEXDIGIT(x) ('0'+(x) + ((x)>9 ? 'A'-'9'-1 : 0))

void DebugMouse()
{
	int txcount;
	int rxcount;
	int debugrow;
	int timeout;
	int timedout;
	int timeouts=9;
	char *debugptr;

	Menu_ShowHide(1);

	while(PS2MouseRead()>-1)
		; // Drain the buffer;

	while(timeouts)
	{
		char *ptr=initmouse;

		strcpy(debugtxt,"  Mouse Debug: ");
		debugtxt[0]=HEXDIGIT(timeouts);
		DebugRow(0,debugtxt);
		memset(debugtxt,0,32);
		debugrow=1;

		debugptr=debugtxt;
		txcount=*ptr++;
		timedout=0;
		--timeouts;
		while(txcount && !timedout)
		{
			PS2MouseWrite(*ptr++);
			--txcount;
			rxcount=1;

			timeout=GetTimer(3500);	//3.5 seconds
			while(rxcount)
			{
				int q=PS2MouseRead();
				if(q>-1)
				{
					printf("Received %x\n",q);
					*debugptr++=HEXDIGIT(q>>4);
					*debugptr++=HEXDIGIT(q&15);
					--rxcount;
				}
				else if(CheckTimer(timeout))
				{
					timedout=1;
					*debugptr++='T';
					--rxcount;
				}
	
				printf("Debug text: %s\n",debugtxt);
				DebugRow(debugrow,debugtxt);
				printf("tx %d, rx %d\n",txcount,rxcount);
				if(!txcount && !rxcount)
				{
					int next=*ptr++;
					if(next&0x80)
					{
						rxcount=next&0x7f;
						printf("Receiving %x bytes",rxcount);
					}
					else
					{
						txcount=next;
						++debugrow;
						debugptr=debugtxt;
						memset(debugptr,0,32);
						printf("Sending %x bytes",txcount);
					}
					if(!txcount && !rxcount)
						timeouts=0;
				}
			}
		}
	}
}
#endif

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
	int u=unit-'0';

	switch(unit)
	{
		/* ROM images */
		case 0:
			if(filename)
			{
				sendstatus(statusword|1);
				setromtype(filename);
				LoadROM(filename);
			}
			break;
		/* Floppy images */
		case '0':
		case '1':
			diskimg_mount(0,u);				
			toggle_wp(u);
			diskimg_mount(filename,u);				
			break;
		/* Hard disk images */
		case '2':
		case '3':
			diskimg_mount(filename,u);
			if(diskimg[u].valid)
				statusword|=(TOS_ACSI0_ENABLE<<(u-2));
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
		// FIXME - complain about missing ROM here
	}

	EnableInterrupts();
	handlemouse(1);

	while(1)
	{
		handlemouse(0);
		Menu_Run();

#if 0
		if((TestKey(KEY_ESC) && TestKey(KEY_LCTRL))
		{
			DebugMouse();
		}
#endif

		c64keys_inthandler();

#ifdef CONFIG_DISKIMG
		diskimg_poll();
		mist_get_dmastate();
#endif
	}

	return(0);
}

