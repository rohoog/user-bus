# user-bus

This repo consists of 2 parts:
- a kernel module that allows creation of an i2c bus device 'fed' by userland code (user-i2c)
- an ftdi userland i2c driver for ft232h (like) devices based on https://github.com/devttys0/libmpsse

Using the 2 together allows the use of standard i2ctools like i2cdetect to interact with i2c devices connected to the i2c-side of the ft232h.

The user-land i2c driver works for i2c busses like pseudo-tty, userfs, loop work for ttys, filesystems and blockdevices.

The protocol is really simple (too simple at the moment).  
When a userland application opens the /dev/i2c-user device, a new /dev/i2c-x is created. The userland application is supposed to read from the ope
device filedescriptor and the read returns 2 bytes: the i2c address (shifted <<1 to include the read/write bit) and the length. If the address indicates a read
transaction (`addr&1 == 1`), the next operation should be a write, returning what was read from the addressed device. If the address indicates a write (`addr&1 == 0`),
the next operation should be a read, reading what data to write to the addressed device. When the bus is not ACK-ed (no device reacts to the address, the opposite
operation should be called by the application to indicate the NACK and the length/data don't care. Each such transcation is to be started with a start condition
and ended with a stop condition. This is currently the limitation that makes this too simplistic for real-life use, as no repeated start conditions are possible.

The intention is to relieve this limitation using an ioctl. If a userland i2c bus driver wants to support repeated start conditions, then it needs to use this ioctl
that relays the in-kernel master-transfer call, allowing multiple collated i2c transactions with repeated start conditions.

## Compiling

### Kernel module
Prerequisites: enough of the the kernel source tree for the currently running kernel to build out-of-tree modules (vgl. `dnf install kernel-devel`)

`make -C user-i2c`

### FTDI driver
Prerequisites: build the https://github.com/devttys0/libmpsse library first and install it (/usr/local/lib).

`make -C ft232h-i2c`

## Using

First insmod the kernel module. This will create the /dev/i2c-user device. Then run the mpsse-i2c-user application (as root).  
Now you can use the i2cdetect utility from the i2c-tools package to see what devices are connected to your ft232h board.

Example with one [MCP23017](http://ww1.microchip.com/downloads/en/DeviceDoc/20001952C.pdf) connected:
```
[ronald@bto user-bus]$ sudo i2cdetect 5
[sudo] password for ronald: 
WARNING! This program can confuse your I2C bus, cause data loss and worse!
I will probe file /dev/i2c-5.
I will probe address range 0x03-0x77.
Continue? [Y/n] y
     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f
00:          -- -- -- -- -- -- -- -- -- -- -- -- -- 
10: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- 
20: 20 -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- 
30: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- 
40: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- 
50: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- 
60: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- 
70: -- -- -- -- -- -- -- --                         
```

## Connections

The ft232h requires 2 ports for the SDA line (ADBUS1 and ADBUS2) connected together. The SCL line only needs one port (ADBUS0). Both SDA and SCL need to be pulled
up with a resistor (4K7 suggested) to the VCC (5V or 3.3V depending on the connected device(s)). To me, that means that tf232h does not support "clock stretching",
a method that slave devices can use to slow down the I2C bus speed if the bus master honors it.

