#include <stdio.h>
#include <stdlib.h>
#include <mpsse.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include "../user-i2c/user-i2c.h"

/* i2c transactions should be no larger than 32 bytes, but length field could encode up to 255 */
unsigned char buf[256];

int main(void)
{
	int fd, rc;
	char *data = NULL;
	struct i2c_umsg umsg;
	struct mpsse_context *i2cbus = MPSSE(I2C, FOUR_HUNDRED_KHZ, MSB);

	if (i2cbus != NULL && i2cbus->open) {
		printf("%s initialized at %dHz (I2C)\n", GetDescription(i2cbus), GetClock(i2cbus));
	} else {
		printf("No MPSSE device found\n");
		return 1;
	}
	fd = open("/dev/i2c-user", O_RDWR);
	if (fd == -1) {
		perror("/dev/i2c-user");
		Close(i2cbus);
		return 1;
	}
	while ((rc=ioctl(fd, UI2C_XFER, &umsg))>=0) {
		int len;
		//printf("%s %02x len %u\n", (umsg.flags&I2C_M_RD)?"read":"write", umsg.addr, umsg.len);
		Start(i2cbus);
		/* only 7-bit addressing for now */
		buf[0]=(umsg.addr<<1)|((umsg.flags&I2C_M_RD)!=0);
		Write(i2cbus, (char*)buf, 1);
		switch (GetAck(i2cbus)) {
			case ACK:
				len = umsg.len;
				if (umsg.flags&I2C_M_RD) { // read
					data = Read(i2cbus, len-1);
					if (data) {
						memcpy(buf, data, len-1);
						free(data);
						SendNacks(i2cbus);
						data = Read(i2cbus, 1);
						if (data) {
							buf[len-1] = *data;
							free(data);
						} else {
							len--;
						}
						SendAcks(i2cbus);
						write(fd, buf, len);
					} else {
						read(fd, buf, 1);
					}
				} else { // write
					read(fd, buf, len);
					Write(i2cbus, (char*)buf, len);
					GetAck(i2cbus);
				}
				if (umsg.flags & I2C_M_STOP) {
					Stop(i2cbus);
				}
				break;
			case NACK: // receive NACK, use opposite method, data don't care
				if (buf[0]&1) { // read
					read(fd, buf, 1);
				} else { // write
					write(fd, buf, 1);
				}
				break;
			default:
				goto outloop;
		}
	}
	perror("ioctl");
outloop:
	close(fd);
	Close(i2cbus);
	return 0;
}
