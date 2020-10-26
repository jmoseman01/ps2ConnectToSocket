/*
# _____     ___ ____     ___ ____
#  ____|   |    ____|   |        | |____|
# |     ___|   |____ ___|    ____| |    \    PS2DEV Open Source Project.
#-----------------------------------------------------------------------
# Copyright 2001-2004, ps2dev - http://www.ps2dev.org
# Licenced under Academic Free License version 2.0
# Review ps2sdk README & LICENSE files for further details.
#
*/

#include <stdio.h>
#include <kernel.h>
#include <iopcontrol.h>
#include <iopheap.h>
#include <debug.h>
#include <netman.h>
#include <ps2ip.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <sbv_patches.h>
#include <string.h>

#include <tamtypes.h>
#include <stdio.h>
#include <sifrpc.h>
#include <gs_privileged.h>
#include <libpad.h>
#include <string.h>

extern unsigned char DEV9_irx[];
extern unsigned int size_DEV9_irx;

extern unsigned char SMAP_irx[];
extern unsigned int size_SMAP_irx;

extern unsigned char NETMAN_irx[];
extern unsigned int size_NETMAN_irx;

#define DEBUG_BGCOLOR(col) *((u64 *) 0x120000e0) = (u64) (col)

static char* padTypeStr[] = {	"Unsupported controller", "Mouse", "Nejicon",
						"Konami Gun", "Digital", "Analog", "Namco Gun",
						"DualShock"};

static char *padBuf[2];
static u32 portConnected[2];
static u32 paddata[2];
static u32 old_pad[2];
static u32 new_pad[2];
static u8 actDirect[2][6] = { {0,0,0,0,0,0}, {0,0,0,0,0,0}};

static void wait_vsync()
{
	// Enable the vsync interrupt.
	*GS_REG_CSR |= GS_SET_CSR(0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0);

	// Wait for the vsync interrupt.
	while (!(*GS_REG_CSR & (GS_SET_CSR(0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0)))) { }

	// Disable the vsync interrupt.
	*GS_REG_CSR &= ~GS_SET_CSR(0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0);
}

static void loadmodules(int free)
{
	s32 ret;

	if(free == 1)
	{
		if((ret = SifLoadModule("host0:sio2man.irx", 0, NULL)) < 0)
		{
			printf("Failed to load sio2man.irx module (%d)\n", ret);
			SleepThread();
		}

		if((ret = SifLoadModule("host0:padman.irx", 0, NULL)) < 0)
		{
			printf("Failed to load padman.irx module (%d)\n", ret);
			SleepThread();
		}
	}
	else
	{
		if((ret = SifLoadModule("rom0:XSIO2MAN", 0, NULL)) < 0)
		{
			printf("Failed to load XSIO2MAN module (%d)\n", ret);
			SleepThread();
		}

		if((ret = SifLoadModule("rom0:XPADMAN", 0, NULL)) < 0)
		{
			printf("Failed to load XPADMAN module (%d)\n", ret);
			SleepThread();
		}
	}
}

static void padWait(int port)
{
	/* Wait for request to complete. */
	while(padGetReqState(port, 0) != PAD_RSTAT_COMPLETE)
		wait_vsync();

	/* Wait for pad to be stable. */
	while(padGetState(port, 0) != PAD_STATE_STABLE)
		wait_vsync();
}

static void padStartAct(int port, int act, int speed)
{
	if(actDirect[port][act] != speed)
	{
		actDirect[port][act] = speed;

		padSetActDirect(port, 0, actDirect[port]);
		padWait(port);
	}
}

static void padStopAct(int port, int act)
{
	padStartAct(port, act, 0);
}

static int ethApplyNetIFConfig(int mode)
{
	int result;
	//By default, auto-negotiation is used.
	static int CurrentMode = NETMAN_NETIF_ETH_LINK_MODE_AUTO;

	if(CurrentMode != mode)
	{	//Change the setting, only if different.
		if((result = NetManSetLinkMode(mode)) == 0)
			CurrentMode = mode;
	}else
		result = 0;

	return result;
}

static void EthStatusCheckCb(s32 alarm_id, u16 time, void *common)
{
	iWakeupThread(*(int*)common);
}

static int WaitValidNetState(int (*checkingFunction)(void))
{
	int ThreadID, retry_cycles;

	// Wait for a valid network status;
	ThreadID = GetThreadId();
	for(retry_cycles = 0; checkingFunction() == 0; retry_cycles++)
	{	//Sleep for 1000ms.
		SetAlarm(1000 * 16, &EthStatusCheckCb, &ThreadID);
		SleepThread();

		if(retry_cycles >= 10)	//10s = 10*1000ms
			return -1;
	}

	return 0;
}

static int ethGetNetIFLinkStatus(void)
{
	return(NetManIoctl(NETMAN_NETIF_IOCTL_GET_LINK_STATUS, NULL, 0, NULL, 0) == NETMAN_NETIF_ETH_LINK_STATE_UP);
}

static int ethWaitValidNetIFLinkState(void)
{
	return WaitValidNetState(&ethGetNetIFLinkStatus);
}

static int ethApplyIPConfig(int use_dhcp, const struct ip4_addr *ip, const struct ip4_addr *netmask, const struct ip4_addr *gateway, const struct ip4_addr *dns)
{
	t_ip_info ip_info;
	const ip_addr_t *dns_curr;
	int result;

	//SMAP is registered as the "sm0" device to the TCP/IP stack.
	if ((result = ps2ip_getconfig("sm0", &ip_info)) >= 0)
	{
		//Obtain the current DNS server settings.
		dns_curr = dns_getserver(0);

		//Check if it's the same. Otherwise, apply the new configuration.
		if ((use_dhcp != ip_info.dhcp_enabled)
		    ||	(!use_dhcp &&
			 (!ip_addr_cmp(ip, (struct ip4_addr *)&ip_info.ipaddr) ||
			 !ip_addr_cmp(netmask, (struct ip4_addr *)&ip_info.netmask) ||
			 !ip_addr_cmp(gateway, (struct ip4_addr *)&ip_info.gw) ||
			 !ip_addr_cmp(dns, dns_curr))))
		{
			if (use_dhcp)
			{
				ip_info.dhcp_enabled = 1;
			}
			else
			{	//Copy over new settings if DHCP is not used.
				ip_addr_set((struct ip4_addr *)&ip_info.ipaddr, ip);
				ip_addr_set((struct ip4_addr *)&ip_info.netmask, netmask);
				ip_addr_set((struct ip4_addr *)&ip_info.gw, gateway);

				ip_info.dhcp_enabled = 0;
			}

			//Update settings.
			result = ps2ip_setconfig(&ip_info);
			if (!use_dhcp)
				dns_setserver(0, dns);
		}
		else
			result = 0;
	}

	return result;
}

static void ethPrintIPConfig(void)
{
	t_ip_info ip_info;
	const ip_addr_t *dns_curr;
	u8 ip_address[4], netmask[4], gateway[4], dns[4];

	//SMAP is registered as the "sm0" device to the TCP/IP stack.
	if (ps2ip_getconfig("sm0", &ip_info) >= 0)
	{
		//Obtain the current DNS server settings.
		dns_curr = dns_getserver(0);

		ip_address[0] = ip4_addr1((struct ip4_addr *)&ip_info.ipaddr);
		ip_address[1] = ip4_addr2((struct ip4_addr *)&ip_info.ipaddr);
		ip_address[2] = ip4_addr3((struct ip4_addr *)&ip_info.ipaddr);
		ip_address[3] = ip4_addr4((struct ip4_addr *)&ip_info.ipaddr);

		netmask[0] = ip4_addr1((struct ip4_addr *)&ip_info.netmask);
		netmask[1] = ip4_addr2((struct ip4_addr *)&ip_info.netmask);
		netmask[2] = ip4_addr3((struct ip4_addr *)&ip_info.netmask);
		netmask[3] = ip4_addr4((struct ip4_addr *)&ip_info.netmask);

		gateway[0] = ip4_addr1((struct ip4_addr *)&ip_info.gw);
		gateway[1] = ip4_addr2((struct ip4_addr *)&ip_info.gw);
		gateway[2] = ip4_addr3((struct ip4_addr *)&ip_info.gw);
		gateway[3] = ip4_addr4((struct ip4_addr *)&ip_info.gw);

		dns[0] = ip4_addr1(dns_curr);
		dns[1] = ip4_addr2(dns_curr);
		dns[2] = ip4_addr3(dns_curr);
		dns[3] = ip4_addr4(dns_curr);

		scr_printf(	"IP:\t%d.%d.%d.%d\n"
				"NM:\t%d.%d.%d.%d\n"
				"GW:\t%d.%d.%d.%d\n"
				"DNS:\t%d.%d.%d.%d\n",
					ip_address[0], ip_address[1], ip_address[2], ip_address[3],
					netmask[0], netmask[1], netmask[2], netmask[3],
					gateway[0], gateway[1], gateway[2], gateway[3],
					dns[0], dns[1], dns[2], dns[3]);

		scr_printf("I ran!\n");
	}
	else
	{
		scr_printf("Unable to read IP address.\n");
	}
}

static void ethPrintLinkStatus(void)
{
	int mode, baseMode;

	//SMAP is registered as the "sm0" device to the TCP/IP stack.
	scr_printf("Link:\t");
	if (NetManIoctl(NETMAN_NETIF_IOCTL_GET_LINK_STATUS, NULL, 0, NULL, 0) == NETMAN_NETIF_ETH_LINK_STATE_UP)
		scr_printf("Up\n");
	else
		scr_printf("Down\n");

	scr_printf("Mode:\t");
	mode = NetManIoctl(NETMAN_NETIF_IOCTL_ETH_GET_LINK_MODE, NULL, 0, NULL, 0);

	//NETMAN_NETIF_ETH_LINK_MODE_PAUSE is a flag, so file it off first.
	baseMode = mode & (~NETMAN_NETIF_ETH_LINK_DISABLE_PAUSE);
	switch(baseMode)
	{
		case NETMAN_NETIF_ETH_LINK_MODE_10M_HDX:
			scr_printf("10M HDX");
			break;
		case NETMAN_NETIF_ETH_LINK_MODE_10M_FDX:
			scr_printf("10M FDX");
			break;
		case NETMAN_NETIF_ETH_LINK_MODE_100M_HDX:
			scr_printf("100M HDX");
			break;
		case NETMAN_NETIF_ETH_LINK_MODE_100M_FDX:
			scr_printf("100M FDX");
			break;
		default:
			scr_printf("Unknown");
	}
	if(!(mode & NETMAN_NETIF_ETH_LINK_DISABLE_PAUSE))
		scr_printf(" with ");
	else
		scr_printf(" without ");
	scr_printf("Flow Control\n");
}

void resolveAddress( struct sockaddr_in *server, int w, int x, int y, int z)
{
	char port[6] = "10000"; // default port of 80(HTTP)
	int i = 0;

	scr_printf("octets %d %d %d %d\n",w,x,y,z);
	IP4_ADDR( (struct ip4_addr *)&(server->sin_addr) ,w,x,y,z );
	scr_printf("%X\n",server->sin_addr);

    i = (int) strtol(port, NULL, 10); // set the port
	server->sin_port = htons(i);
	scr_printf("port %d\n", i);

	server->sin_family = AF_INET;
}

int main(int argc, char *argv[])
{
	struct ip4_addr IP, NM, GW, DNS;
	struct sockaddr_in * server = malloc(sizeof *server);
	int rc;
	int EthernetLinkMode;
	int sockHandle;

	//Reboot IOP
	SifInitRpc(0);
	while(!SifIopReset("", 0)){};
	while(!SifIopSync()){};

	//Initialize SIF services
	SifInitRpc(0);
	SifLoadFileInit();
	SifInitIopHeap();
	sbv_patch_enable_lmb();

	//Load modules
	SifExecModuleBuffer(DEV9_irx, size_DEV9_irx, 0, NULL, NULL);
	SifExecModuleBuffer(NETMAN_irx, size_NETMAN_irx, 0, NULL, NULL);
	SifExecModuleBuffer(SMAP_irx, size_SMAP_irx, 0, NULL, NULL);

	//Initialize NETMAN
	NetManInit();

	init_scr();

	//The network interface link mode/duplex can be set.
	EthernetLinkMode = NETMAN_NETIF_ETH_LINK_MODE_AUTO;

	//Attempt to apply the new link setting.
	if(ethApplyNetIFConfig(EthernetLinkMode) != 0) {
		scr_printf("Error: failed to set link mode.\n");
		goto end;
	}

	//Initialize IP address.
	IP4_ADDR(&IP, 192, 168, 0, 200);
	IP4_ADDR(&NM, 255, 255, 255, 0);
	IP4_ADDR(&GW, 192, 168, 0, 1);
	//DNS is not required if the DNS service is not used, but this demo will show how it is done.
	IP4_ADDR(&DNS, 192, 168, 0, 1);

	//Initialize the TCP/IP protocol stack.
	ps2ipInit(&IP, &NM, &GW);
	dns_setserver(0, &DNS);	//Set DNS server

	//Change IP address
	ethApplyIPConfig(0, &IP, &NM, &GW, &DNS);

	//Wait for the link to become ready.
	scr_printf("Waiting for connection...\n");
	if(ethWaitValidNetIFLinkState() != 0) {
		scr_printf("Error: failed to get valid link status.\n");
		goto end;
	}

	scr_printf("Initialized:\n");
	ethPrintLinkStatus();
	ethPrintIPConfig();

	//At this point, network support has been initialized and the PS2 can be pinged.
	scr_printf("I ran!\n");

	resolveAddress( server, 192, 168, 0, 132);
	if((sockHandle = socket( AF_INET, SOCK_STREAM, IPPROTO_TCP )) < 0)
	{
		scr_printf("socket failed");
	}
	rc = connect( sockHandle, (struct sockaddr *) server, sizeof(*server));
	scr_printf("connect %d\n", rc);

	u32 port;
	struct padButtonStatus buttons;
	int dualshock[2];
	int acts[2];

	SifInitRpc(0);

	printf("libpadx sample");

	if((argc == 2) && (strncmp(argv[1], "free", 4) == 0))
	{
		printf(" - Using PS2SDK sio2man.irx and padman.irx modules.\n");
		loadmodules(1);
	}
	else
	{
		printf(" - Using ROM XSIO2MAN and XPADMAN modules.\n");
		printf("Start this sample with 'free' as an argument to load sio2man.irx and padman.irx\n");
		printf("Example: ps2client execee host:padx_sample.elf free\n");
		loadmodules(0);
	}




	padInit(0);

	padBuf[0] = memalign(64, 256);
	padBuf[1] = memalign(64, 256);

	old_pad[0] = 0;
	old_pad[1] = 0;

	portConnected[0] = 0;
	portConnected[1] = 0;

	dualshock[0] = 0;
	dualshock[1] = 0;

	acts[0] = 0;
	acts[1] = 0;

	padPortOpen(0, 0, padBuf[0]);
	padPortOpen(1, 0, padBuf[1]);

	while(1)
	{
		for(port=0; port < 1; port++)
		{
			s32 state = padGetState(port, 0);

			if((state == PAD_STATE_STABLE) && (portConnected[port] == 0))
			{
				u32 i;
				u8 mTable[8];
				u32 ModeCurId;
				u32 ModeCurOffs;
				u32 ModeCurExId;
				u32 ModeTableNum = padInfoMode(port, 0, PAD_MODETABLE, -1);

				scr_printf("Controller (%i) connected\n", port);

				/* Check if dualshock and if so, activate analog mode */
				for(i = 0; i < ModeTableNum; i++)
					mTable[i] = padInfoMode(port, 0, PAD_MODETABLE, i);

				/* Works for dualshock2 */
				if((mTable[0] == 4) && (mTable[1] == 7) && (ModeTableNum == 2))
					dualshock[port] = 1;

				/* Active and lock analog mode */
				if(dualshock[port] == 1)
				{
					padSetMainMode(port, 0, PAD_MMODE_DUALSHOCK, PAD_MMODE_LOCK);
					padWait(port);
				}

				ModeCurId = padInfoMode(port, 0, PAD_MODECURID, 0);
				ModeCurOffs = padInfoMode(port, 0, PAD_MODECUROFFS, 0);
				ModeCurExId = padInfoMode(port, 0, PAD_MODECUREXID, 0);
				ModeTableNum = padInfoMode(port, 0, PAD_MODETABLE, -1);
				acts[port] = padInfoAct(port, 0, -1, 0);

				scr_printf("  ModeCurId      : %i (%s)\n", (int)ModeCurId, padTypeStr[ModeCurId]);
				scr_printf("  ModeCurExId    : %i\n", (int)ModeCurExId);
				scr_printf("  ModeTable      : ");

				for(i = 0; i < ModeTableNum; i++)
				{
					mTable[i] = padInfoMode(port, 0, PAD_MODETABLE, i);
					scr_printf("%i ", (int)mTable[i]);
				}

				scr_printf("\n");
				scr_printf("  ModeTableNum   : %i\n", (int)ModeTableNum);
				scr_printf("  ModeCurOffs    : %i\n", (int)ModeCurOffs);
				scr_printf("  NumOfAct       : %i\n", (int)acts[port]);
				scr_printf("  PressMode      : %i\n", (int)padInfoPressMode(port, 0));


				if(acts[port] > 0)
				{
					u8 actAlign[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
					u32 i;

					/* Set offsets for motor parameters for SetActDirect. */
					for(i=0; i < acts[port]; i++)
						actAlign[i] = i;

					padSetActAlign(port, 0, actAlign);
					padWait(port);
				}

				scr_printf("  EnterPressMode : %i\n", (int)padEnterPressMode(port, 0));
				padWait(port);

				scr_printf("Ready\n");

				portConnected[port] = 1;
			}

			if((state == PAD_STATE_DISCONN) && (portConnected[port] == 1))
			{
				scr_printf("Controller (%i) disconnected\n", port);
				portConnected[port] = 0;
			}

			if(portConnected[port] == 1)
			{
				s32 ret = padRead(port, 0, &buttons);

				if(ret != 0)
				{
					paddata[port] = 0xffff ^ buttons.btns;

					new_pad[port] = paddata[port] & ~old_pad[port];
					old_pad[port] = paddata[port];

					// Values 50 and 200 used because my controllers are worn out :-)
					if((buttons.ljoy_h <= 50) || (buttons.ljoy_h >= 200)) printf("Left Analog  X: %i\n", (int)buttons.ljoy_h);
					if((buttons.ljoy_v <= 50) || (buttons.ljoy_v >= 200)) printf("Left Analog  Y: %i\n", (int)buttons.ljoy_v);
					if((buttons.rjoy_h <= 50) || (buttons.rjoy_h >= 200)) printf("Right Analog X: %i\n", (int)buttons.rjoy_h);
					if((buttons.rjoy_v <= 50) || (buttons.rjoy_v >= 200)) printf("Right Analog Y: %i\n", (int)buttons.rjoy_v);


					if(new_pad[port]) printf("Controller (%i) button(s) pressed: ", (int)port);
	            	if(new_pad[port] & PAD_LEFT){
						scr_printf("LEFT ");
					}		
					if(new_pad[port] & PAD_RIGHT) 		scr_printf("RIGHT ");
					if(new_pad[port] & PAD_UP) 			scr_printf("UP ");
					if(new_pad[port] & PAD_DOWN) 		scr_printf("DOWN ");
					if(new_pad[port] & PAD_START) 		scr_printf("START ");
					if(new_pad[port] & PAD_SELECT) 		scr_printf("SELECT ");
					if(new_pad[port] & PAD_SQUARE) 		scr_printf("SQUARE (Pressure: %i) ", (int)buttons.square_p);
					if(new_pad[port] & PAD_TRIANGLE)	scr_printf("TRIANGLE (Pressure: %i) ", (int)buttons.triangle_p);
					if(new_pad[port] & PAD_CIRCLE)		scr_printf("CIRCLE (Pressure: %i) ", (int)buttons.circle_p);
					if(new_pad[port] & PAD_CROSS)		scr_printf("CROSS (Pressure: %i) ", (int)buttons.cross_p);
					if(new_pad[port] & PAD_L1)
					{
						printf("L1 (Start Little Motor) ");
						padStartAct(port, 0, 1);
					}
					if(new_pad[port] & PAD_L2)
					{
						printf("L2 (Stop Little Motor) ");
						padStartAct(port, 0, 0);
					}
					if(new_pad[port] & PAD_L3)			printf("L3 ");
					if(new_pad[port] & PAD_R1)
					{
						printf("R1 (Start Big Motor) ");
						padStartAct(port, 1, 255);
					}
					if(new_pad[port] & PAD_R2)
					{
						printf("R2 (Stop Big Motor) ");
						padStopAct(port, 1);
					}
					if(new_pad[port] & PAD_R3)			printf("R3 ");

					if(new_pad[port]) {
						rc = send( sockHandle, &new_pad,  3, 0 );	
						printf("\n");
					}
				}

			}
		}
		wait_vsync();
	}

	SleepThread();

end:
	//To cleanup, just call these functions.
	ps2ipDeinit();
	NetManDeinit();

	//Deinitialize SIF services
	SifExitRpc();

	return 0;
}

