/*
 * OSIF driver - 0.1
 *
 * Copyright (c) 2007 Barry Carter <Barry.Carter@robotfuzz.com>
 *
 * Based on the i2c-tiny-usb by
 *
 * Copyright (C) 2006 Til Harbaum (Till@Harbaum.org)
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License as
 *	published by the Free Software Foundation, version 2.
 *
 * This driver is based on the 2.6.3 version of drivers/usb/usb-skeleton.c 
 * but has been rewritten to be easy to read and use, as no locks are now
 * needed anymore.
 *
 */

// #define DEBUG_IO

//#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/kref.h>
#include <asm/uaccess.h>

/* include interfaces to usb layer */
#include <linux/usb.h>

/* include interface to i2c layer */
#include <linux/i2c.h>

/* Version Information */
#define DRIVER_VERSION "v0.2"
#define DRIVER_AUTHOR  "Barry Carter <barry.carter@robotfuzz.com>"
#define DRIVER_DESC    "OSIF driver"
#define DRIVER_URL     "http://www.robotfuzz.com/"

// Generic requests
#define USBTINY_ECHO                  0	 // echo test
#define USBTINY_READ                  1	 // read byte (wIndex:address)
#define USBTINY_WRITE                 2  // write byte (wIndex:address, wValue:value)
#define USBTINY_CLR                   3	 // clear bit (wIndex:address, wValue:bitno)
#define USBTINY_SET                   4	 // set bit (wIndex:address, wValue:bitno)
// Programming requests
#define USBTINY_POWERUP               5	 // apply power (wValue:SCK-period, wIndex:RESET)
#define USBTINY_POWERDOWN             6	 // remove power from chip
#define USBTINY_SPI                   7	 // issue SPI command (wValue:c1c0, wIndex:c3c2)
#define USBTINY_POLL_BYTES            8	 // set poll bytes for write (wValue:p1p2)
#define USBTINY_FLASH_READ            9	 // read flash (wIndex:address)
#define USBTINY_FLASH_WRITE           10 // write flash (wIndex:address, wValue:timeout)
#define USBTINY_EEPROM_READ           11 // read eeprom (wIndex:address)
#define USBTINY_EEPROM_WRITE          12 // write eeprom (wIndex:address, wValue:timeout)
#define USBI2C_READ                   20 // read from i2c bus
#define USBI2C_WRITE                  21 // write to i2c bus
#define USBI2C_STOP                   22 // send stop condition
#define USBI2C_STAT                   23 // get stats from i2c action
#define USBI2C_SET_BITRATE            24 // Set the bitrate

#define STATUS_ADDRESS_ACK            0
#define STATUS_ADDRESS_NAK            2

/* i2c speed in khz. 333kHz max */
static int speed = 333;
module_param(speed, int, 0);
MODULE_PARM_DESC(speed, "I2C bus speed in KHz "
                 "(default is 333KHz)");

static int usb_read(struct i2c_adapter *i2c_adap, int cmd, 
		    int value, int index, void *data, int len);

static int usb_write(struct i2c_adapter *adapter, int cmd, 
		     int value, int index, void *data, int len);


/* ----- begin of i2c layer ---------------------------------------------- */

static int usb_xfer(struct i2c_adapter *adapter, struct i2c_msg *msgs, int num)
{
    unsigned char status;
    struct i2c_msg *pmsg;
    int i;
    int ret=0;

    dev_dbg(&adapter->dev, "master xfer %d messages:", num);

    for (i = 0;ret >= 0 && i < num; i++) {
        int cmd = USBI2C_READ;

        pmsg = &msgs[i];

        dev_dbg(&adapter->dev, "  %d: %s (flags %d) %d bytes to 0x%02x",
            i, pmsg->flags & I2C_M_RD ? "read" : "write", pmsg->flags,
            pmsg->len, pmsg->addr);

        /* and directly send the message */
        if(pmsg->flags & I2C_M_RD) {

            /* read data */
            if(usb_read(adapter, cmd, 
                        pmsg->flags, pmsg->addr, 
                        pmsg->buf, pmsg->len) <1) {

                dev_err(&adapter->dev, "failure reading data");
                return -EREMOTEIO;
            }
            if(usb_read(adapter, USBI2C_STOP, 
               0, 0, 
               0, 0) >0) {

                   dev_err(&adapter->dev, "failure sending STOP");
                   return -EREMOTEIO;
            }

#ifdef DEBUG_IO
            { 
                char str[32];
                int j; 
                str[0] = 0;
                for(j=0;j<pmsg->len;j++)
                sprintf(str+strlen(str), "%x ", pmsg->buf[i]);
                dev_info(&adapter->dev, "   < %s\n", str);
            }
#endif
        } else {
#ifdef DEBUG_IO
            { 
                char str[32];
                int j; 
                str[0] = 0;
                for(j=0;j<pmsg->len;j++)
                sprintf(str+strlen(str), "%x ", pmsg->buf[i]);
                dev_info(&adapter->dev, "   > %s\n", str);
            }
#endif
            cmd = USBI2C_WRITE;
            /* write data */
            if(usb_write(adapter, cmd, 
                        pmsg->flags, pmsg->addr, 
                        pmsg->buf, pmsg->len) != pmsg->len) {
                dev_err(&adapter->dev, "failure writing data");
                return -EREMOTEIO;
            }
            if(usb_read(adapter, USBI2C_STOP, 
               0, 0, 
               0, 0) >0) {

                   dev_err(&adapter->dev, "failure sending STOP");
                   return -EREMOTEIO;
               }

        }

        /* read status */
        if(usb_read(adapter, USBI2C_STAT, 0, 0, &status, 1) != 1) {
        dev_err(&adapter->dev, "failure reading status");
        return -EREMOTEIO;
        }

        dev_dbg(&adapter->dev, "  status = %d", status);
        if(status != STATUS_ADDRESS_ACK)
        return -EREMOTEIO;
    }

    return i;
}

static u32 usb_func(struct i2c_adapter *adapter)
{
        /* configure for I2c mode and SMBUS emulation */
    u32 func = I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;

    dev_info(&adapter->dev, "got adapter functionality %x\n", func);
    return func;
}

/* This is the actual algorithm we define */
static struct i2c_algorithm usb_algorithm = {
    .master_xfer	  = usb_xfer,
    .functionality  = usb_func,
};

/* ----- end of i2c layer ---------------------------------------------- */

/* ----- begin of usb layer ---------------------------------------------- */

/* the usb i2c interface uses a vid/pid pair donated by ftdi */
#define USB_OSIF_VENDOR_ID	0x1964
#define USB_OSIF_PRODUCT_ID	0x0001

/* table of devices that work with this driver */
static struct usb_device_id osif_table [] = {
    { USB_DEVICE(USB_OSIF_VENDOR_ID, USB_OSIF_PRODUCT_ID) },
    { }					/* Terminating entry */
};

MODULE_DEVICE_TABLE (usb, osif_table);

/* Structure to hold all of our device specific stuff */
struct osif {
    struct usb_device *udev;		/* the usb device for this device */
    struct usb_interface *interface;	/* the interface for this device */

    /* i2c related things */
    struct i2c_adapter      i2c_adap;
};


static void set_i2c_speed(struct osif *dev, int speed_khz);


static int usb_read(struct i2c_adapter *adapter, int cmd, 
		    int value, int index, void *data, int len) {
    struct osif *dev = (struct osif *)adapter->algo_data;
    int retval;

    /* do control transfer */
    retval = usb_control_msg(dev->udev,
                            usb_rcvctrlpipe(dev->udev, 0),
                            cmd,
                            USB_TYPE_VENDOR | USB_RECIP_INTERFACE | USB_DIR_IN,
                            value, index,
                            data,	
                            len,
                            2000);

    return retval;
}

static int usb_write(struct i2c_adapter *adapter, int cmd, 
		    int value, int index, void *data, int len) {
    struct osif *dev = (struct osif *)adapter->algo_data;
    int retval;

    /* do control transfer */
    retval = usb_control_msg(dev->udev,
                            usb_sndctrlpipe(dev->udev, 0),
                            cmd,
                            USB_TYPE_VENDOR | USB_RECIP_INTERFACE,
                            value, index,
                            data,	
                            len,
                            2000);

    return retval;
}

static void osif_free(struct osif *dev) {	
    usb_put_dev(dev->udev);
    kfree (dev);
}

static int osif_probe(struct usb_interface *interface, 
                             const struct usb_device_id *id) {
    struct osif *dev = NULL;
    int retval = -ENOMEM;
    u16 version;

    dev_dbg(&interface->dev, "probing usb device");

    /* allocate memory for our device state and initialize it */
    dev = kmalloc(sizeof(*dev), GFP_KERNEL);
    if (dev == NULL) {
        dev_err(&interface->dev, "Out of memory");
        goto error;
    }

    /* clear memory */
    memset(dev, 0, sizeof(*dev));

    dev->udev = usb_get_dev(interface_to_usbdev(interface));
    dev->interface = interface;
    /* save our data pointer in this interface device */
    usb_set_intfdata(interface, dev);

    version = le16_to_cpu(dev->udev->descriptor.bcdDevice);
    dev_info(&interface->dev, "version %x.%02x found at bus %03d address %03d\n", 
        version>>8, version&0xff, 
        dev->udev->bus->busnum, dev->udev->devnum);

    /* setup i2c adapter description */
    dev->i2c_adap.owner	  = THIS_MODULE;
    dev->i2c_adap.class	  = I2C_CLASS_HWMON;
    dev->i2c_adap.algo	  = &usb_algorithm;
    dev->i2c_adap.algo_data = dev;
    sprintf(dev->i2c_adap.name, "OSIF at bus %03d device %03d", 
            dev->udev->bus->busnum, dev->udev->devnum);

    set_i2c_speed(dev, speed);
       
    /* and finally attach to i2c layer */
    i2c_add_adapter(&(dev->i2c_adap));

    /* inform user about successful attachment to i2c layer */
    dev_info(&dev->i2c_adap.dev, "connected OSIF device\n");

    return 0;

    error:

    if(dev)
        osif_free(dev);

    return retval;
}

static void osif_disconnect(struct usb_interface *interface)
{
    struct osif *dev = usb_get_intfdata(interface);

    i2c_del_adapter(&(dev->i2c_adap));

    usb_set_intfdata(interface, NULL);

    osif_free(dev);

    dev_dbg(&interface->dev, "disconnected");
}

static struct usb_driver osif_driver = {
    .name       = "Open Source InterFace OSIF",
    .probe      = osif_probe,
    .disconnect = osif_disconnect,
    .id_table   = osif_table,
};

module_usb_driver(osif_driver);

/* ----- end of usb layer ---------------------------------------------- */


static void set_i2c_speed(struct osif *dev, int speed_khz)
{
    int twbr;
    int twps = 1;
    int bitrate_hz = speed_khz * 1000;
 
    // Calculate the new bitrate based on the formula
    // br scl = cpu / (16 + 2(TWBR)) . 4 ^TWPS
    if (bitrate_hz < 26000)
    {
        twps = 4;
    }
    //int poww = pow(4, twps);
    twbr = (((12000000/bitrate_hz)/twps)-16)/2 ;

    if (usb_write(&dev->i2c_adap, USBI2C_SET_BITRATE, twbr, twps, NULL, 0) != 0) {
        dev_err(&dev->i2c_adap.dev,
                "failure setting bitrate to %dKHz\n", speed_khz);
    }
    else {
        dev_info(&dev->interface->dev, "Setting speed to %dKHz, twbr %d, twps %d\n", speed_khz, twbr, (twps == 1 ? 0 : twps));
    }
}

MODULE_AUTHOR( DRIVER_AUTHOR );
MODULE_DESCRIPTION( DRIVER_DESC );
MODULE_LICENSE("GPL");
