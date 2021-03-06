/*
  spike.c
 
	Author    Adithya Baglody
 
  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.
 
  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.
 
  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/cdev.h>
#include <linux/spi/spi.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <asm/uaccess.h>
#include "spike_cc2500.h"

#define SPI_BUFF_SIZE	16
#define USER_BUFF_SIZE	128

#define SPI_BUS 1
#define SPI_BUS_CS1 1
//#define SPI_BUS_SPEED 1000000
#define SPI_BUS_SPEED 1000000
#define READ_CS_PIN 5
#define WRITE_CS_PIN 51

#define RF_SELECT_R 0
#define RF_SELECT_W 1
#define RF_SELECT_RW 2


const char this_driver_name[] = "spike";
char *debug_str="";
u8 paTable[] = {0xFF};
u8 paTableLen = 1;
static char transmit_str[USER_BUFF_SIZE];

struct spike_control {
	struct spi_message msg;
	struct spi_transfer transfer;
	u8 *tx_buff; 
	u8 *rx_buff;
};

static struct spike_control spike_ctl;

struct spike_dev {
	struct semaphore spi_sem;
	struct semaphore fop_sem;
	dev_t devt;
	struct cdev cdev;
	struct class *class;
	struct spi_device *spi_device;
	char *user_buff;
	u8 test_data;	
};

static struct spike_dev spike_dev;

//functions 
static void rf_send_strobe(u8 addr, u8 cs_pin);
static u8 rf_reg_status(u8 addr,u8 cs_pin);

//functions declare end

static void simple_write_on_spi(u8 val)
{	// the chip selct has to be taken care outside this function this just puts values on the spi
	// correct spi device is to be selected.
	spi_message_init(&spike_ctl.msg);
	spike_ctl.tx_buff[0]=val;
	spike_ctl.transfer.tx_buf = spike_ctl.tx_buff;
	spike_ctl.transfer.len = 1;
	spi_message_add_tail(&spike_ctl.transfer, &spike_ctl.msg);
	spi_sync(spike_dev.spi_device, &spike_ctl.msg);
}

static int rf_burst_write(char *str)
{
	u8 len;
// write in the overlay  0x04c 0x17 /*spi0_cs for 2nd rf module ,P9_16 */
	rf_send_strobe(CCxxx0_SIDLE,WRITE_CS_PIN);
	rf_send_strobe(CCxxx0_SFTX,WRITE_CS_PIN);
printk(KERN_ALERT "FIFO size in the write rf afeter flush %x ",rf_reg_status(CCxxx0_TXBYTES+0xC0,WRITE_CS_PIN));


gpio_set_value(WRITE_CS_PIN,0);

	simple_write_on_spi(CCxxx0_TXFIFO_Muti);

	len = strlen(str);
	simple_write_on_spi(len);
	simple_write_on_spi(0x01); // address of the reader
	spi_message_init(&spike_ctl.msg);
	//spike_ctl.tx_buff[0]="h";
	//spike_ctl.transfer.tx_buf = spike_ctl.tx_buff;
	spike_ctl.transfer.tx_buf = (u8 *) str;

	
	spike_ctl.transfer.len = len;
	spi_message_add_tail(&spike_ctl.transfer, &spike_ctl.msg);
	spi_sync(spike_dev.spi_device, &spike_ctl.msg);
gpio_set_value(WRITE_CS_PIN,1);

printk(KERN_ALERT "FIFO size in the write rf %x ",rf_reg_status(CCxxx0_TXBYTES+0xC0,WRITE_CS_PIN));
printk(KERN_ALERT "before switching to STX");
	rf_send_strobe(CCxxx0_SNOP,WRITE_CS_PIN);
	rf_send_strobe(CCxxx0_SFSTXON,WRITE_CS_PIN);
	mdelay(1);
printk (KERN_ALERT " %x the value of CCxxx0_MARCSTATE for write rf" ,rf_reg_status(CCxxx0_MARCSTATE+0xc0,WRITE_CS_PIN) );
	rf_send_strobe(CCxxx0_STX,WRITE_CS_PIN);
printk(KERN_ALERT "after switching to STX");
printk (KERN_ALERT " %x the value of CCxxx0_MARCSTATE for write rf" ,rf_reg_status(CCxxx0_MARCSTATE+0xc0,WRITE_CS_PIN) );
	rf_send_strobe(CCxxx0_SNOP,WRITE_CS_PIN);
printk(KERN_ALERT "FIFO size in the write rf after tx enable %x ",rf_reg_status(CCxxx0_TXBYTES+0xC0,WRITE_CS_PIN));
printk(KERN_ALERT "pktstatus of read rf  after stx on%x ",rf_reg_status(CCxxx0_PKTSTATUS+0xC0,READ_CS_PIN));
return len;
}


static void spi_burstreg_read(u8 addr,u8 len)
{
gpio_set_value(READ_CS_PIN,0);
	simple_write_on_spi(addr);

	spi_message_init(&spike_ctl.msg);
	memset(spike_ctl.rx_buff, 0, SPI_BUFF_SIZE);
	spike_ctl.transfer.rx_buf=spike_ctl.rx_buff;
	spike_ctl.transfer.len = len;
	spi_message_add_tail(&spike_ctl.transfer, &spike_ctl.msg);
	spi_sync(spike_dev.spi_device, &spike_ctl.msg);
gpio_set_value(READ_CS_PIN,1);
printk(KERN_ALERT" the rx values are %x %x %x %x %x %x %x %x",spike_ctl.rx_buff[0],spike_ctl.rx_buff[1],spike_ctl.rx_buff[2],spike_ctl.rx_buff[3],spike_ctl.rx_buff[4],spike_ctl.rx_buff[5],spike_ctl.rx_buff[6],spike_ctl.rx_buff[7]);

return;
}



static u8 rf_reg_status(u8 addr,u8 cs_pin)
{
gpio_set_value(cs_pin,0);
	spi_message_init(&spike_ctl.msg);
	memset(spike_ctl.tx_buff, 0, SPI_BUFF_SIZE);
	memset(spike_ctl.rx_buff, 0, SPI_BUFF_SIZE);
	spike_ctl.tx_buff[0]=addr| READ_SINGLE;
	spike_ctl.transfer.tx_buf = spike_ctl.tx_buff;
	//spike_ctl.transfer.rx_buf=spike_ctl.rx_buff;
	spike_ctl.transfer.len = 1;
	spi_message_add_tail(&spike_ctl.transfer, &spike_ctl.msg);
	spi_sync(spike_dev.spi_device, &spike_ctl.msg);


	spi_message_init(&spike_ctl.msg);
	//spike_ctl.msg.complete = spike_completion_handler;
	//spike_ctl.msg.context = NULL;
	spike_ctl.transfer.rx_buf=spike_ctl.rx_buff;
	spike_ctl.transfer.len = 1;
	spi_message_add_tail(&spike_ctl.transfer, &spike_ctl.msg);
	spi_sync(spike_dev.spi_device, &spike_ctl.msg);
gpio_set_value(cs_pin,1);
//printk (KERN_ALERT " 2 bytes of rf_reg_status %x %x  \n",spike_ctl.rx_buff[0],spike_ctl.rx_buff[1]);
	return spike_ctl.rx_buff[0];

}
static void rf_send_strobe(u8 addr, u8 cs_pin)
{
gpio_set_value(cs_pin,0);
	spi_message_init(&spike_ctl.msg);
	//spike_ctl.msg.complete = spike_completion_handler;
	//spike_ctl.msg.context = NULL;
	spike_ctl.tx_buff[0]=addr;
	spike_ctl.transfer.tx_buf = spike_ctl.tx_buff;
	spike_ctl.transfer.rx_buf=spike_ctl.rx_buff;
	spike_ctl.transfer.len = 1;
	spi_message_add_tail(&spike_ctl.transfer, &spike_ctl.msg);
	spi_sync(spike_dev.spi_device, &spike_ctl.msg);
gpio_set_value(cs_pin,1);
	printk(KERN_ALERT " sync for strobe %x", spike_ctl.rx_buff[0]);

}

static u8 rf_read(void)
{
	u8 pktlen=0;
	spike_ctl.transfer.len=2;
printk(KERN_ALERT "pktstatus of read rf %x ",rf_reg_status(CCxxx0_PKTSTATUS+0xC0,READ_CS_PIN));
printk(KERN_ALERT "RSSI of read rf %x ",rf_reg_status(CCxxx0_RSSI+0xC0,READ_CS_PIN));
	pktlen=rf_reg_status(CCxxx0_RXBYTES|0xC0,READ_CS_PIN);
	printk(KERN_ALERT "pkt len %x",pktlen);
	//if (pktlen <=spike_ctl.transfer.len)
	{
		spi_burstreg_read(CCxxx0_RXFIFO_Muti,pktlen);
	return pktlen;
	}
	/*else
	{	spi_burstreg_read(CCxxx0_RXFIFO_Muti,spike_ctl.transfer.len); 
		return 	spike_ctl.transfer.len;	
	}*/

}


static void spi_debug_read(void)
{
	int status=0;
	spi_message_init(&spike_ctl.msg);
	memset(spike_ctl.rx_buff, 0, SPI_BUFF_SIZE);
	//spike_ctl.transfer.tx_buf=NULL;
	spike_ctl.transfer.rx_buf = spike_ctl.rx_buff;
	spike_ctl.transfer.len = 2;
	spi_message_add_tail(&spike_ctl.transfer, &spike_ctl.msg);
	status = spi_sync(spike_dev.spi_device, &spike_ctl.msg);
	printk(KERN_ALERT " tx_buff %x %x \n",spike_ctl.tx_buff[0],spike_ctl.tx_buff[1]);
	printk(KERN_ALERT " rx_buff %x %x \n",spike_ctl.rx_buff[0],spike_ctl.rx_buff[1]);
}

/* selection of the chip select
0- for READ_CS_PIN
1- for WRITE_CS_PIN
2- for both WRITE_CS_PIN and READ_CS_PIN
 */
static int spi_msg_tx(u8 addr,u8 value,u8 cs_select)
{
	int status=0;
	switch(cs_select)
	{
		case RF_SELECT_R: gpio_set_value(READ_CS_PIN,0); break;	
		case RF_SELECT_W: gpio_set_value(WRITE_CS_PIN,0); break;
		case RF_SELECT_RW: gpio_set_value(WRITE_CS_PIN,0);
				gpio_set_value(READ_CS_PIN,0);	
				break;
		default:
			printk(KERN_ALERT " the cs_select in spi_msg_tx is invalid");
			return -1;
	}
	spi_message_init(&spike_ctl.msg);
	memset(spike_ctl.tx_buff, 0, SPI_BUFF_SIZE);
	spike_ctl.tx_buff[0]=addr;
	spike_ctl.tx_buff[1]=value;
	spike_ctl.transfer.tx_buf = spike_ctl.tx_buff;
	//spike_ctl.transfer.rx_buf=NULL;
	spike_ctl.transfer.len = 2;
	spi_message_add_tail(&spike_ctl.transfer, &spike_ctl.msg);
	status = spi_sync(spike_dev.spi_device, &spike_ctl.msg);
	switch(cs_select)
	{
		case RF_SELECT_R : gpio_set_value(READ_CS_PIN,1); break;	
		case RF_SELECT_W : gpio_set_value(WRITE_CS_PIN,1); break;
		case RF_SELECT_RW : gpio_set_value(WRITE_CS_PIN,1);
				gpio_set_value(READ_CS_PIN,1);	
				break;
		default:
			printk(KERN_ALERT " the cs_select in spi_msg_tx is invalid");
			return -1;
	}

	return status;
	
}

//smart rf settings
static void __init rf_settings(void)
{
// Product = CC2500
// Crystal accuracy = 40 ppm
// X-tal frequency = 26 MHz
// RF output power = 0 dBm
// RX filterbandwidth = 540.000000 kHz
// Deviation = 0.000000
// Return state:  Return to RX state upon leaving either TX or RX
// Datarate = 250.000000 kbps
// Modulation = (7) MSK
// Manchester enable = (0) Manchester disabled
// RF Frequency = 2433.000000 MHz
// Channel spacing = 199.950000 kHz
// Channel number = 0
// Optimization = Sensitivity
// Sync mode = (3) 30/32 sync word bits detected
// Format of RX/TX data = (0) Normal mode, use FIFOs for RX and TX
// CRC operation = (1) CRC calculation in TX and CRC check in RX enabled
// Forward Error Correction = (0) FEC disabled
// Length configuration = (1) Variable length packets, packet length configured by the first received byte after sync word.
// Packetlength = 255
// Preamble count = (2)  4 bytes
// Append status = 1
// Address check = Address check and 0 (0x00) broadcast
// CRC autoflush = true
// Device address = 1
// GDO0 signal selection = ( 0x06 ) Asserts when sync word has been sent / received, and de-asserts at the end of the packet
// GDO2 signal selection = ( 0x0E ) Carrier sense. High if RSSI level is above threshold
/// reset the rf settings
rf_send_strobe(CCxxx0_SIDLE,READ_CS_PIN);
rf_send_strobe(CCxxx0_SIDLE,WRITE_CS_PIN);

rf_send_strobe(CCxxx0_SRES,READ_CS_PIN);
rf_send_strobe(CCxxx0_SRES,WRITE_CS_PIN);
mdelay(50);   // so that the reset has worked
printk (KERN_ALERT " this is after the reset has occoured %x ",rf_reg_status(CCxxx0_RXBYTES+0xc0,READ_CS_PIN));
printk (KERN_ALERT " %x the value of CCxxx0_VERSION for read rf" ,rf_reg_status(CCxxx0_VERSION+0xc0,READ_CS_PIN) );
printk (KERN_ALERT " %x the value of CCxxx0_VERSION for write rf" ,rf_reg_status(CCxxx0_VERSION+0xc0,WRITE_CS_PIN) );
printk (KERN_ALERT " %x the value of CCxxx0_MARCSTATE for read rf" ,rf_reg_status(CCxxx0_MARCSTATE+0xc0,READ_CS_PIN) );
printk (KERN_ALERT " %x the value of CCxxx0_MARCSTATE for write rf" ,rf_reg_status(CCxxx0_MARCSTATE+0xc0,WRITE_CS_PIN) );

// Write register settings
  spi_msg_tx(CCxxx0_IOCFG2,   0x0E,RF_SELECT_RW);  // GDO2 output pin config.
  spi_msg_tx(CCxxx0_IOCFG0,   0x06,RF_SELECT_RW);  // GDO0 output pin config.
  spi_msg_tx(CCxxx0_PKTLEN,   0x3D,RF_SELECT_RW);  // Packet length.
  spi_msg_tx(CCxxx0_PKTCTRL1, 0x07,RF_SELECT_RW);  // Packet automation control. worked with 0x04
  spi_msg_tx(CCxxx0_PKTCTRL0, 0x05,RF_SELECT_RW);  // Packet automation control.
  spi_msg_tx(CCxxx0_ADDR,     0x01,RF_SELECT_R);  // Device address. READ device address is 01
  spi_msg_tx(CCxxx0_ADDR,     0x02,RF_SELECT_W);  // Device address.WRITE device address is 02
  spi_msg_tx(CCxxx0_CHANNR,   0x00,RF_SELECT_RW); // Channel number.
  spi_msg_tx(CCxxx0_FSCTRL1,  0x05,RF_SELECT_RW); // Freq synthesizer control.
  spi_msg_tx(CCxxx0_FSCTRL0,  0x00,RF_SELECT_RW); // Freq synthesizer control.
  spi_msg_tx(CCxxx0_FREQ2,    0x5D,RF_SELECT_RW); // Freq control word, high byte
  spi_msg_tx(CCxxx0_FREQ1,    0x93,RF_SELECT_RW); // Freq control word, mid byte.
  spi_msg_tx(CCxxx0_FREQ0,    0xB1,RF_SELECT_RW); // Freq control word, low byte.
  spi_msg_tx(CCxxx0_MDMCFG4,  0x2D,RF_SELECT_RW); // Modem configuration.
  spi_msg_tx(CCxxx0_MDMCFG3,  0x3B,RF_SELECT_RW); // Modem configuration.
  spi_msg_tx(CCxxx0_MDMCFG2,  0x73,RF_SELECT_RW); // Modem configuration.
  spi_msg_tx(CCxxx0_MDMCFG1,  0x22,RF_SELECT_RW); // Modem configuration.
  spi_msg_tx(CCxxx0_MDMCFG0,  0xF8,RF_SELECT_RW); // Modem configuration.
  spi_msg_tx(CCxxx0_DEVIATN,  0x00,RF_SELECT_RW); // Modem dev (when FSK mod en)
  spi_msg_tx(CCxxx0_MCSM1 ,   0x20,RF_SELECT_RW); //MainRadio Cntrl State Machine
  spi_msg_tx(CCxxx0_MCSM0 ,   0x18,RF_SELECT_RW); //MainRadio Cntrl State Machine
  spi_msg_tx(CCxxx0_FOCCFG,   0x1D,RF_SELECT_RW); // Freq Offset Compens. Config
  spi_msg_tx(CCxxx0_BSCFG,    0x1C,RF_SELECT_RW); //  Bit synchronization config.
  spi_msg_tx(CCxxx0_AGCCTRL2, 0xC7,RF_SELECT_RW); // AGC control.
  spi_msg_tx(CCxxx0_AGCCTRL1, 0x00,RF_SELECT_RW); // AGC control.
  spi_msg_tx(CCxxx0_AGCCTRL0, 0xB2,RF_SELECT_RW); // AGC control.
  spi_msg_tx(CCxxx0_FREND1,   0xB6,RF_SELECT_RW); // Front end RX configuration.
  spi_msg_tx(CCxxx0_FREND0,   0x10,RF_SELECT_RW); // Front end RX configuration.
  spi_msg_tx(CCxxx0_FSCAL3,   0xEA,RF_SELECT_RW); // Frequency synthesizer cal.
  spi_msg_tx(CCxxx0_FSCAL2,   0x0A,RF_SELECT_RW); // Frequency synthesizer cal.
  spi_msg_tx(CCxxx0_FSCAL1,   0x00,RF_SELECT_RW); // Frequency synthesizer cal.
  spi_msg_tx(CCxxx0_FSCAL0,   0x11,RF_SELECT_RW); // Frequency synthesizer cal.
  spi_msg_tx(CCxxx0_FSTEST,   0x59,RF_SELECT_RW); // Frequency synthesizer cal.
  spi_msg_tx(CCxxx0_TEST2,    0x88,RF_SELECT_RW); // Various test settings.
  spi_msg_tx(CCxxx0_TEST1,    0x31,RF_SELECT_RW); // Various test settings.
  spi_msg_tx(CCxxx0_TEST0,    0x0B,RF_SELECT_RW);  // Various test settings


spi_msg_tx(CCxxx0_PATABLE,paTable[0],RF_SELECT_RW);

}

static ssize_t spike_write(struct file *filp, char __user *buff, size_t count,
			loff_t *offp)
{
	ssize_t retval;
	int len;
	/*if (down_interruptible(&fop_sem))
		return -ERESTARTSYS;*/
	if (copy_from_user(transmit_str, buff, count)) {
                 retval = -EFAULT;
                 goto out;
	}
	len=rf_burst_write(transmit_str);
	printk(KERN_ALERT "the length of the rf write is %x ",len);
	*offp += len;
	retval = len;
 out:
         //up(&fop_sem);
         return retval;

}

static ssize_t spike_read(struct file *filp, char __user *buff, size_t count,
			loff_t *offp)
{
	size_t len;
	ssize_t status = 0;
	char local_buff[2];
	
	u8 pktlen=0,i;

	if (!buff) 
		return -EFAULT;

	if (*offp > 0) 
		return 0;


	// write on  the rf
	/*len=rf_burst_write(transmit_str);
	printk(KERN_ALERT "the length of the rf write is %x ",len);*/

	// done write now to read over the radio
	//mdelay(100);
	pktlen=rf_read();
//printk(KERN_ALERT " after the rf_read()");
//printk (KERN_ALERT " %x the value of CCxxx0_MARCSTATE for read rf" ,rf_reg_status(CCxxx0_MARCSTATE+0xc0,READ_CS_PIN) );
//printk (KERN_ALERT " %x the value of CCxxx0_MARCSTATE for write rf" ,rf_reg_status(CCxxx0_MARCSTATE+0xc0,WRITE_CS_PIN) );
	/*if (down_interruptible(&spike_dev.fop_sem)) 
		return -ERESTARTSYS;*/

	
		sprintf(spike_dev.user_buff,"the received values are \n");
	for(i=2;i<=spike_ctl.rx_buff[0];i++)
	{
		sprintf(local_buff,"%c",spike_ctl.rx_buff[i]);
printk(KERN_ALERT " %x ",spike_ctl.rx_buff[i]);
		strcat(spike_dev.user_buff,local_buff);
	}
	strcat(spike_dev.user_buff,"\n"); // adding a new line
		
	len = strlen(spike_dev.user_buff);
 
	if (len < count) 
		count = len;

	if (copy_to_user(buff, spike_dev.user_buff, count))  {
		printk(KERN_ALERT "spike_read(): copy_to_user() failed\n");
		status = -EFAULT;
	} else {
		*offp += count;
		status = count;
	}



	//up(&spike_dev.fop_sem);

	return status;	
}

static int spike_open(struct inode *inode, struct file *filp)
{	
	int status = 0;

	/*if (down_interruptible(&spike_dev.fop_sem)) 
		return -ERESTARTSYS;*/

	if (!spike_dev.user_buff) {
		spike_dev.user_buff = kmalloc(USER_BUFF_SIZE, GFP_KERNEL);
		if (!spike_dev.user_buff) 
			status = -ENOMEM;
	}	
//rx enable;
rf_send_strobe(CCxxx0_SIDLE,READ_CS_PIN);
//rf_send_strobe(CCxxx0_SFRX,READ_CS_PIN);
rf_send_strobe(CCxxx0_SFSTXON,READ_CS_PIN);
mdelay(1);
rf_send_strobe(CCxxx0_SRX,READ_CS_PIN);
printk(KERN_ALERT" SRX has be sent");
printk (KERN_ALERT " %x the value of CCxxx0_MARCSTATE for read rf" ,rf_reg_status(CCxxx0_MARCSTATE+0xc0,READ_CS_PIN) );
rf_send_strobe(CCxxx0_SNOP,READ_CS_PIN);

	//up(&spike_dev.fop_sem);

	return status;
}

static int spike_probe(struct spi_device *spi_device)
{
	if (down_interruptible(&spike_dev.spi_sem))
		return -EBUSY;

	spike_dev.spi_device = spi_device;

	up(&spike_dev.spi_sem);

	return 0;
}

static int spike_remove(struct spi_device *spi_device)
{
	if (down_interruptible(&spike_dev.spi_sem))
		return -EBUSY;
	
	spike_dev.spi_device = NULL;

	up(&spike_dev.spi_sem);

	return 0;
}

static int __init add_spike_device_to_bus(void)
{
	struct spi_master *spi_master;
	struct spi_device *spi_device;
	struct device *pdev;
	char buff[64];
	int status = 0;

	spi_master = spi_busnum_to_master(SPI_BUS);
	if (!spi_master) {
		printk(KERN_ALERT "spi_busnum_to_master(%d) returned NULL\n",
			SPI_BUS);
		printk(KERN_ALERT "Missing modprobe omap2_mcspi?\n");
		return -1;
	}

	spi_device = spi_alloc_device(spi_master);
	if (!spi_device) { 
		put_device(&spi_master->dev);
		printk(KERN_ALERT "spi_alloc_device() failed\n");
		return -1;
	}

	spi_device->chip_select = SPI_BUS_CS1;

	/* Check whether this SPI bus.cs is already claimed */
	snprintf(buff, sizeof(buff), "%s.%u", 
			dev_name(&spi_device->master->dev),
			spi_device->chip_select);

	pdev = bus_find_device_by_name(spi_device->dev.bus, NULL, buff);
 	if (pdev) {
		/* We are not going to use this spi_device, so free it */ 
		spi_dev_put(spi_device);
		
		/* 
		 * There is already a device configured for this bus.cs  
		 * It is okay if it us, otherwise complain and fail.
		 */
		if (pdev->driver && pdev->driver->name && 
				strcmp(this_driver_name, pdev->driver->name)) {
			printk(KERN_ALERT 
				"Driver [%s] already registered for %s\n",
				pdev->driver->name, buff);
			status = -1;
		} 
	} else {
		spi_device->max_speed_hz = SPI_BUS_SPEED;
		spi_device->mode = SPI_MODE_0;
		spi_device->bits_per_word = 8;
		spi_device->irq = -1;
		spi_device->controller_state = NULL;
		spi_device->controller_data = NULL;
		strlcpy(spi_device->modalias, this_driver_name, SPI_NAME_SIZE);
		
		status = spi_add_device(spi_device);		
		if (status < 0) {	
			spi_dev_put(spi_device);
			printk(KERN_ALERT "spi_add_device() failed: %d\n", 
				status);		
		}				
	}

	put_device(&spi_master->dev);

	return status;
}

static struct spi_driver spike_driver = {
	.driver = {
		.name =	this_driver_name,
		.owner = THIS_MODULE,
	},
	.probe = spike_probe,
	.remove = spike_remove,	
};

static int __init spike_init_spi(void)
{
	int error;

	spike_ctl.tx_buff = kmalloc(SPI_BUFF_SIZE, GFP_KERNEL | GFP_DMA);
	if (!spike_ctl.tx_buff) {
		error = -ENOMEM;
		goto spike_init_error;
	}

	spike_ctl.rx_buff = kmalloc(SPI_BUFF_SIZE, GFP_KERNEL | GFP_DMA);
	if (!spike_ctl.rx_buff) {
		error = -ENOMEM;
		goto spike_init_error;
	}

	error = spi_register_driver(&spike_driver);
	if (error < 0) {
		printk(KERN_ALERT "spi_register_driver() failed %d\n", error);
		goto spike_init_error;
	}

	error = add_spike_device_to_bus();
	if (error < 0) {
		printk(KERN_ALERT "add_spike_to_bus() failed\n");
		spi_unregister_driver(&spike_driver);
		goto spike_init_error;	
	}

	return 0;

spike_init_error:

	if (spike_ctl.tx_buff) {
		kfree(spike_ctl.tx_buff);
		spike_ctl.tx_buff = 0;
	}

	if (spike_ctl.rx_buff) {
		kfree(spike_ctl.rx_buff);
		spike_ctl.rx_buff = 0;
	}
	
	return error;
}

static const struct file_operations spike_fops = {
	.owner =	THIS_MODULE,
	.read = 	spike_read,
	.write = 	spike_write,
	.open =		spike_open,	
};

static int __init spike_init_cdev(void)
{
	int error;

	spike_dev.devt = MKDEV(0, 0);

	error = alloc_chrdev_region(&spike_dev.devt, 0, 1, this_driver_name);
	if (error < 0) {
		printk(KERN_ALERT "alloc_chrdev_region() failed: %d \n", 
			error);
		return -1;
	}

	cdev_init(&spike_dev.cdev, &spike_fops);
	spike_dev.cdev.owner = THIS_MODULE;
	
	error = cdev_add(&spike_dev.cdev, spike_dev.devt, 1);
	if (error) {
		printk(KERN_ALERT "cdev_add() failed: %d\n", error);
		unregister_chrdev_region(spike_dev.devt, 1);
		return -1;
	}	

	return 0;
}

static int __init spike_init_class(void)
{
	spike_dev.class = class_create(THIS_MODULE, this_driver_name);

	if (!spike_dev.class) {
		printk(KERN_ALERT "class_create() failed\n");
		return -1;
	}

	if (!device_create(spike_dev.class, NULL, spike_dev.devt, NULL, 	
			this_driver_name)) {
		printk(KERN_ALERT "device_create(..., %s) failed\n",
			this_driver_name);
		class_destroy(spike_dev.class);
		return -1;
	}

	return 0;
}

static int __init spike_init(void)
{
	int err;
	memset(&spike_dev, 0, sizeof(spike_dev));
	memset(&spike_ctl, 0, sizeof(spike_ctl));

	sema_init(&spike_dev.spi_sem, 1);
	sema_init(&spike_dev.fop_sem, 1);
	
	if (spike_init_cdev() < 0) 
		goto fail_1;
	
	if (spike_init_class() < 0)  
		goto fail_2;

	if (spike_init_spi() < 0) 
		goto fail_3;

	//mdelay(1000);
 err = gpio_request_one(READ_CS_PIN, GPIOF_OUT_INIT_HIGH,"spike"); 
 err = gpio_request_one(WRITE_CS_PIN, GPIOF_OUT_INIT_HIGH,"spike"); 
printk( KERN_ALERT "%d is the error from the request",err);
	rf_settings(); 		//rf init
	return 0;

fail_3:
	device_destroy(spike_dev.class, spike_dev.devt);
	class_destroy(spike_dev.class);

fail_2:
	cdev_del(&spike_dev.cdev);
	unregister_chrdev_region(spike_dev.devt, 1);

fail_1:
	return -1;
}
module_init(spike_init);

static void __exit spike_exit(void)
{
	spi_unregister_device(spike_dev.spi_device);
	spi_unregister_driver(&spike_driver);

	device_destroy(spike_dev.class, spike_dev.devt);
	class_destroy(spike_dev.class);

	cdev_del(&spike_dev.cdev);
	unregister_chrdev_region(spike_dev.devt, 1);

gpio_free(READ_CS_PIN);
gpio_free(WRITE_CS_PIN);

	if (spike_ctl.tx_buff)
		kfree(spike_ctl.tx_buff);

	if (spike_ctl.rx_buff)
		kfree(spike_ctl.rx_buff);

	if (spike_dev.user_buff)
		kfree(spike_dev.user_buff);
}
module_exit(spike_exit);

MODULE_AUTHOR("Adithya Baglody");
MODULE_DESCRIPTION("spike module - an example SPI driver");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.2");
