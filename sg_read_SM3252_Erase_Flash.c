#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "sg_lib.h"
#include "sg_io_linux.h"

/* This program performs a similar READ_10 command as scsi mid-level support
   16 byte commands from lk 2.4.15 to read basic information from SM325 chip

*  Copyright (C) 2001 D. Gilbert
*  This program is free software; you can redistribute it and/or modify
*  it under the terms of the GNU General Public License as published by
*  the Free Software Foundation; either version 2, or (at your option)
*  any later version.

   Invocation: sg_read_SM3252_Erase_Flash <scsi_device>

   Version 1.02 (20020206)

   Updated by Philip Ton  on 03/29/2016
   
*/

#undef DEBUG_FLAG
#define DEBUG_FLAG1 1
#define DEBUG_FLAG2 1
#define DEBUG_FLAG3 1
#define DEBUG_FLAG4 0

#define READBB_REPLY_LEN  1024
#define READ10_REPLY_LEN  512
#define READ10_CMD_LEN    16

#define READCAP_REPLY_LEN 8
#define READCAP_CMD_LEN   10
#define BYTES_IN_MiB      1048576
#define BYTES_IN_MB       1000000

#define INQ_REPLY_LEN     96
#define INQ_CMD_LEN       6

#define EBUFF_SZ 256
#define MAX_MU   256

enum read_steps {basic_info, init_and_current_badblocks, current_spare_blocks_1, current_spare_blocks_2, read_LED, write_LED, reset_drive, erase_flash, write_10};
enum inq_read_steps {inq_basic_info, inq_unit_serial_number}; 

int main(int argc, char * argv[])
{
    FILE *pFile;
    time_t rawtime;
    struct tm * timeinfo;
    int sg_fd, k, ok, i, j, FBlk;
    sg_io_hdr_t io_hdr;
    char * file_name = 0;
    char ebuff[EBUFF_SZ];
    unsigned char sense_buffer[32];
    unsigned char FourBytes[4], TwoBytes[2];
    unsigned char Viking[] = "VT";
    unsigned char filename[22];
    unsigned char *ptr2Buffer;

    unsigned char r10CmdBlk[9][READ10_CMD_LEN] =
             { {0xF0, 0x20, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0},
               {0xF0, 0x0A, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0},
               {0x28, 0x00, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0},
               {0xF0, 0xAA, 0, 0, 0, 0, 0, 0x10, 0, 0, 0, 1, 0, 0, 0, 0},
               {0xF0, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0},
               {0xF1, 0x03, 0, 0, 0, 0, 0, 0, 0x20, 0, 0, 1, 0, 0, 0, 0},
               {0xF0, 0x2C, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
               {0xF0, 0x0C, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
               {0x8A, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0} };
//               {0x2A, 0x00, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0} };
    unsigned char inBuff[READ10_REPLY_LEN];
    unsigned char inBuffBB[READBB_REPLY_LEN];
    unsigned int Total_MU=0, Total_LBA=0, LBA_per_MU=0, HalfLBA_per_MU=0, mu, lba, SLBA, LED_result=0;
    unsigned short Current_BadBlock[MAX_MU], Initial_BadBlock[MAX_MU], Total_DataBlock[MAX_MU];
    unsigned short Initial_SpareBlock[MAX_MU], Current_SpareBlock[MAX_MU];

    unsigned char inqCmdBlk [2][INQ_CMD_LEN] =
             { {0x12, 0, 0, 0, INQ_REPLY_LEN, 0}, {0x12, 0, 0x80, 0, INQ_REPLY_LEN, 0} };
    unsigned char inqBuff[INQ_REPLY_LEN];
    unsigned char VendorID[8], ProductID[16], ProductRevision[4], UnitSerialNumber[18], UnitProductNumber[18], SMIChip[8];

    unsigned char capCmdBlk [READCAP_CMD_LEN] =
              {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    unsigned char capBuff[READCAP_REPLY_LEN];
    unsigned int  BlockSize=0, DiskSize=0, countRead=0, countWrite=0;
    unsigned char LED_Status_Byte=0, LED_Ready=0, LED_Busy=0;
    int loop;
    
    time( &rawtime );
    timeinfo = localtime( &rawtime );
    
    for (k = 1; k < argc; ++k) {
        if (*argv[k] == '-') {
            printf("Unrecognized switch: %s\n", argv[k]);
            file_name = 0;
            break;
        }
        else if (0 == file_name)
            file_name = argv[k];
        else {
            printf("too many arguments\n");
            file_name = 0;
            break;
        }
    }
    if (0 == file_name) {
        printf("Usage: 'sg_read_SM3252_LED <sg_device>'\n");
        return 1;
    }

    if ((sg_fd = open(file_name, O_RDWR)) < 0) {
        snprintf(ebuff, EBUFF_SZ,
                 "sg_read_SM325: error opening file: %s", file_name);
        perror(ebuff);
        return 1;
    }
    /* Just to be safe, check we have a new sg device by trying an ioctl */
    if ((ioctl(sg_fd, SG_GET_VERSION_NUM, &k) < 0) || (k < 30000)) {
        printf("sg_read_SM325: %s doesn't seem to be a new sg device\n",
               file_name);
        close(sg_fd);
        return 1;
    }

    /* 1. Prepare INQUIRY command for Vendor ID, Product ID, Product Revision  0x12 */
    /**************************************************************************/
    {
    printf("1. INQUIRY command 0x12 for Vendor ID\n");
    memset(&io_hdr, 0, sizeof(sg_io_hdr_t));
    io_hdr.interface_id = 'S';
    io_hdr.cmd_len = sizeof(inqCmdBlk[inq_basic_info]);
    /* io_hdr.iovec_count = 0; */  /* memset takes care of this */
    io_hdr.mx_sb_len = sizeof(sense_buffer);
    io_hdr.dxfer_direction = SG_DXFER_FROM_DEV;
    io_hdr.dxfer_len = INQ_REPLY_LEN;
    io_hdr.dxferp = inqBuff;
    io_hdr.cmdp = inqCmdBlk[inq_basic_info];
    io_hdr.sbp = sense_buffer;
    io_hdr.timeout = 20000;     /* 20000 millisecs == 20 seconds */
    /* io_hdr.flags = 0; */     /* take defaults: indirect IO, etc */
    /* io_hdr.pack_id = 0; */
    /* io_hdr.usr_ptr = NULL; */

    if (ioctl(sg_fd, SG_IO, &io_hdr) < 0) {
        perror("sg_simple1: Inquiry SG_IO ioctl error");
        close(sg_fd);
        return 1;
    }

    /* now for the error processing */
    ok = 0;
    switch (sg_err_category3(&io_hdr)) {
    case SG_LIB_CAT_CLEAN:
        ok = 1;
        break;
    case SG_LIB_CAT_RECOVERED:
        printf("Recovered error on INQUIRY, continuing\n");
        ok = 1;
        break;
    default: /* won't bother decoding other categories */
        sg_chk_n_print3("INQUIRY command error", &io_hdr, 1);
        break;
    }

    if (ok) { /* output result if it is available */
        char * p = (char *)inqBuff;
        int f = (int)*(p + 7);
#ifdef DEBUG_FLAG
	    printf(" inquiry buffer  00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F 10 11 12 13 14 15 16 17 18 19 1A 1B 1C 1D 1E 1F\n");
	    printf("                 -----------------------------------------------------------------------------------------------\n");
   		memcpy( inqBuff, io_hdr.dxferp, sizeof(inqBuff));
	    for (i=0; i<16; i++)  /* 16 rows */
	    {
	      printf("       %3d-%3d = ", i*j, (i*j)+31);

	      for (j=0; j<32; j++)
	         printf("%02X ", inqBuff[(i*32)+j]);
	   
	      printf("\n");
	    }
        printf("\n");
#endif
   		memcpy( VendorID, p + 8, sizeof(VendorID));
   		memcpy( ProductID, p + 16, sizeof(ProductID));
   		memcpy( ProductRevision, p + 32, sizeof(ProductRevision));
    }
    }

    /* 2. Prepare INQUIRY command for Unit Serial Number  0x12 */
    /*****************************************************/
    {
        printf("2. INQUIRY command 0x12 for Serial Number\n");
    io_hdr.cmd_len = sizeof(inqCmdBlk[inq_unit_serial_number]);
    io_hdr.cmdp = inqCmdBlk[inq_unit_serial_number];

    if (ioctl(sg_fd, SG_IO, &io_hdr) < 0) {
        perror("sg_simple1: Inquiry SG_IO ioctl error");
        close(sg_fd);
        return 1;
    }

    /* now for the error processing */
    ok = 0;
    switch (sg_err_category3(&io_hdr)) {
    case SG_LIB_CAT_CLEAN:
        ok = 1;
        break;
    case SG_LIB_CAT_RECOVERED:
        printf("Recovered error on INQUIRY, continuing\n");
        ok = 1;
        break;
    default: /* won't bother decoding other categories */
        sg_chk_n_print3("INQUIRY command error", &io_hdr, 1);
        break;
    }

    if (ok) { /* output result if it is available */
        char * p = (char *)inqBuff;
        int f = (int)*(p + 7);
#ifdef DEBUG_FLAG
        printf("Unit Serial Number: %.16s \n", p + 4);
	    printf(" inquiry buffer  00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F 10 11 12 13 14 15 16 17 18 19 1A 1B 1C 1D 1E 1F\n");
	    printf("                 -----------------------------------------------------------------------------------------------\n");
   		memcpy( inqBuff, io_hdr.dxferp, sizeof(inqBuff));
	    for (i=0; i<16; i++)  /* 16 rows */
	    {
	      printf("       %3d-%3d = ", i*j, (i*j)+31);

	      for (j=0; j<32; j++)
	         printf("%02X ", inqBuff[(i*32)+j]);
	   
	      printf("\n");
	    }
        printf("\n");
#endif
   		memcpy( UnitSerialNumber, p + 4, sizeof(UnitSerialNumber));
    }
    }

    /* 3. Prepare READ CAPACITY command for Block Size and Disk Size  0x25 */
    /*****************************************************************/
    {
    printf("3. READ CAPACITY command 0x25 for Block Size and Disk Size\n");
    io_hdr.cmd_len = sizeof(capCmdBlk);
    io_hdr.dxfer_len = READCAP_REPLY_LEN;
    io_hdr.dxferp = capBuff;
    io_hdr.cmdp = capCmdBlk;

    if (ioctl(sg_fd, SG_IO, &io_hdr) < 0) {
        perror("sg_simple1: READ CAPACITY SG_IO ioctl error");
        close(sg_fd);
        return 1;
    }

    /* now for the error processing */
    ok = 0;
    switch (sg_err_category3(&io_hdr)) {
    case SG_LIB_CAT_CLEAN:
        ok = 1;
        break;
    case SG_LIB_CAT_RECOVERED:
        printf("Recovered error on READ CAPACITY, continuing\n");
        ok = 1;
        break;
    default: /* won't bother decoding other categories */
        sg_chk_n_print3("READ CAPACITY command error", &io_hdr, 1);
        break;
    }

    if (ok) { /* output result if it is available */
#ifdef DEBUG_FLAG
	    printf(" readcap buffer  00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F 10 11 12 13 14 15 16 17 18 19 1A 1B 1C 1D 1E 1F\n");
	    printf("                 -----------------------------------------------------------------------------------------------\n");
   		memcpy( capBuff, io_hdr.dxferp, sizeof(capBuff));
        printf("                 ");
        for (j=0; j<8; j++)
	        printf("%02X ", capBuff[j]);
	    printf("\n");
#endif
        FourBytes[0] = capBuff[7];
        FourBytes[1] = capBuff[6];
        FourBytes[2] = capBuff[5];
        FourBytes[3] = capBuff[4];
        BlockSize  = *(int *)FourBytes;
        FourBytes[0] = capBuff[3];
        FourBytes[1] = capBuff[2];
        FourBytes[2] = capBuff[1];
        FourBytes[3] = capBuff[0];
        DiskSize  = ((*(int *)FourBytes) + 1) * BlockSize;
    }
    }

    /* 4. Prepare READ_10 command for reading basic information  0xF0 */
    /************************************************************/
    {
    printf("4. READ Bad Block command 0xF0 for basic information\n");
    memset(&io_hdr, 0, sizeof(sg_io_hdr_t));
    io_hdr.interface_id = 'S';
    io_hdr.cmd_len = sizeof(r10CmdBlk[basic_info]);
    /* io_hdr.iovec_count = 0; */  /* memset takes care of this */
    io_hdr.mx_sb_len = sizeof(sense_buffer);
    io_hdr.dxfer_direction = SG_DXFER_FROM_DEV;
    io_hdr.dxfer_len = READ10_REPLY_LEN;
    io_hdr.dxferp = inBuff;
    io_hdr.cmdp = r10CmdBlk[basic_info];
    io_hdr.sbp = sense_buffer;
    io_hdr.timeout = 20000;     /* 20000 millisecs == 20 seconds */
    /* io_hdr.flags = 0; */     /* take defaults: indirect IO, etc */
    /* io_hdr.pack_id = 0; */
    /* io_hdr.usr_ptr = NULL; */

    if (ioctl(sg_fd, SG_IO, &io_hdr) < 0) {
	   perror("sg_read_SM325: Inquiry SG_IO ioctl error");
	   close(sg_fd);
	   return 1;
    }

    /* now for the error processing */
    ok = 0;
    switch (sg_err_category3(&io_hdr)) {
       case SG_LIB_CAT_CLEAN:
	      ok = 1;
	      break;
       case SG_LIB_CAT_RECOVERED:
	      printf("Recovered error on READ_10, continuing\n");
	      ok = 1;
	      break;
       default: /* won't bother decoding other categories */
	      sg_chk_n_print3("READ_10 command error", &io_hdr, 1);
	      break;
    }

    if (ok) { /* output result if it is available */
	    memcpy( sense_buffer, io_hdr.sbp, sizeof(sense_buffer));

#ifdef DEBUG_FLAG
	    printf("\n  STEP 1: READ BASIC INFORMATION\n");
	    /* Print out io_hdr.deferp */
	    printf("   reply buffer  00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F 10 11 12 13 14 15 16 17 18 19 1A 1B 1C 1D 1E 1F\n");
	    printf("                 -----------------------------------------------------------------------------------------------\n");
	    memcpy( inBuff, io_hdr.dxferp, sizeof(inBuff));
	    for (i=0; i<8; i++)  /* 8 rows */
	    {
	      printf("       %3d-%3d = ", i*j, (i*j)+31);

	      for (j=0; j<32; j++)
	         printf("%02X ", inBuff[(i*32+)j]);
	   
	      printf("\n");
	    }
        printf("\n");
#endif
        Total_MU   = inBuff[1];
        FourBytes[0] = inBuff[0x17];
        FourBytes[1] = inBuff[0x16];
        FourBytes[2] = inBuff[0x15];
        FourBytes[3] = inBuff[0x14];
        Total_LBA  = *(int *)FourBytes;
        LBA_per_MU = Total_LBA / Total_MU;
        HalfLBA_per_MU = LBA_per_MU / 2;

#ifdef DEBUG_FLAG1
        printf("Total MU       = %d\n", Total_MU);
        printf("Total LBA      = %d (0x%X)\n", Total_LBA, Total_LBA);
        printf("LBA per MU     = %d (0x%X)\n", LBA_per_MU, LBA_per_MU);
        printf("HalfLBA per MU = %d (0x%X)\n\n", HalfLBA_per_MU, HalfLBA_per_MU);
        printf("Done\n");
#endif
    }
    }

    /* 5. Prepare READ_10 command to get Initial and Current BadBlock numbers for each MU  0xF0 */
    /**************************************************************************************/
{
    printf("\n  STEP 5: READ EACH MU INITIAL AND CURRENT BADBLOCKS\n");
    io_hdr.cmd_len = sizeof(r10CmdBlk[init_and_current_badblocks]);
    io_hdr.dxfer_len = READBB_REPLY_LEN;
    io_hdr.dxferp = inBuffBB;

    /* Loop through each MU */
    for (mu=0; mu<Total_MU; mu++)
    {
        /* Loop through each FBlk */
        for (FBlk=0x3FF; FBlk>=0; FBlk--)
        {
            TwoBytes[0] = (FBlk >> 8) & 0x03;
            TwoBytes[1] = FBlk & 0xFF;
            ptr2Buffer = &r10CmdBlk[init_and_current_badblocks][2];
            memcpy( ptr2Buffer, TwoBytes, 2);
            TwoBytes[0] = ((unsigned short)mu) & 0xFF;
            ptr2Buffer = &r10CmdBlk[init_and_current_badblocks][6];
            memcpy( ptr2Buffer, TwoBytes, 1);
            io_hdr.cmdp = r10CmdBlk[init_and_current_badblocks];
#ifdef DEBUG_FLAG
            printf("Cmd buffer  00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F\n");
            printf("            -----------------------------------------------\n");
            printf("r10CmdBlk = ");
            for (j=0; j<16; j++)
               printf("%02X ", r10CmdBlk[init_and_current_badblocks][j]);
            printf("\n");
#endif
            if (ioctl(sg_fd, SG_IO, &io_hdr) < 0) {
               perror("sg_read_SM325: Inquiry SG_IO ioctl error");
               close(sg_fd);
            return 1;
            }

            /* now for the error processing */
            ok = 0;
            switch (sg_err_category3(&io_hdr)) {
               case SG_LIB_CAT_CLEAN:
                  ok = 1;
                  break;
               case SG_LIB_CAT_RECOVERED:
                  printf("Recovered error on READ_10, continuing\n");
                  ok = 1;
                  break;
               default: /* won't bother decoding other categories */
                  sg_chk_n_print3("READ_10 command error", &io_hdr, 1);
                  break;
            }

            if (ok) { /* output result if it is available */
               printf("\n   PROCESSING MU NUMBER: %d\n", mu);
               printf("READ_10 duration=%u millisecs, resid=%d, msg_status=%d \n",
                   io_hdr.duration, io_hdr.resid, (int)io_hdr.msg_status);

               /* Check the result to see if this MU has any BadBlock */
               memcpy( sense_buffer, io_hdr.sbp, sizeof(sense_buffer));
               memcpy( inBuffBB, io_hdr.dxferp, sizeof(inBuffBB));

#ifdef DEBUG_FLAG
               /* Print out io_hdr.deferp Reply Buffer */
               printf("   reply buffer  00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F 10 11 12 13 14 15 16 17 18 19 1A 1B 1C 1D 1E 1F\n");
               printf("                 -----------------------------------------------------------------------------------------------\n");
               for (i=0; i<3; i++) {
                  printf("   0x%3X-0x%3X = ", 0x100+i*j, 0x100+(i*j)+31);
                  for (j=0; j<32; j++)
                     printf("%02X ", inBuffBB[0x100+i+j]);
                  printf("\n");
               }
               printf("\n");
#endif
               ptr2Buffer = &inBuffBB[0x114];
               if ((inBuffBB[0x114] == 0x53) &&  /* "S" */
                   (inBuffBB[0x115] == 0x4D) &&  /* "M" */
                   (inBuffBB[0x116] == 0x33) &&  /* "3" */
                   (inBuffBB[0x117] == 0x32) &&  /* "2" */
                   (inBuffBB[0x118] == 0x35) &&  /* "5" */
                   (inBuffBB[0x200] == 0xE1) &&
                   ((inBuffBB[0x210] & 0x48) == 0))
               {
                  TwoBytes[0] = inBuffBB[0x101];
                  TwoBytes[1] = inBuffBB[0x100];
                  Current_BadBlock[mu] = *(unsigned short *)TwoBytes;
                  TwoBytes[0] = inBuffBB[0x105];
                  TwoBytes[1] = inBuffBB[0x104];
                  Initial_BadBlock[mu] = Current_BadBlock[mu] - (*(unsigned short *)TwoBytes);
                  TwoBytes[0] = inBuffBB[0x113];
                  TwoBytes[1] = inBuffBB[0x112];
                  Total_DataBlock[mu] = *(unsigned short *)TwoBytes;
                  memcpy( SMIChip, ptr2Buffer, sizeof(SMIChip));

                  printf("Current MU = %d\n", mu);
                  printf("Current_BadBlock   = %d (0x%04X)\n", Current_BadBlock[mu], Current_BadBlock[mu]);
                  printf("Initial_BadBlock   = %d (0x%04X)\n", Initial_BadBlock[mu], Initial_BadBlock[mu]);
                  printf("Total_DataBlock    = %d (0x%04X)\n\n", Total_DataBlock[mu], Total_DataBlock[mu]);

                  break;
               }
            }
        }  /* end of for loop each FBlk */

        /* 5+. Calculate Initial Spare Numbers for each MU */
        /**************************************************/
        if (mu == 0)
        {
           Initial_SpareBlock[mu] = 1014 - Total_DataBlock[mu] - Initial_BadBlock[mu];
        }
        else
        {
           Initial_SpareBlock[mu] = 1020 - Total_DataBlock[mu] - Initial_BadBlock[mu];
        }

    }  /* end of for loop each mu */
}

    /* 6. Get Current Spare Numbers for each MU  0x28 */
    /********************************************/
    {
    printf("\n  STEP 6: READ CURRENT SPARE BLOCKS FOR EACH MU 0x28\n");
    io_hdr.cmd_len = sizeof(r10CmdBlk[current_spare_blocks_1]);
    io_hdr.dxfer_len = READ10_REPLY_LEN;
    io_hdr.dxferp = inBuff;

    for (mu=0; mu<Total_MU; mu++)
    {
        SLBA = (LBA_per_MU * mu) + HalfLBA_per_MU;

        FourBytes[0] = (SLBA >> 24) & 0xFF;
        FourBytes[1] = (SLBA >> 16) & 0xFF;
        FourBytes[2] = (SLBA >> 8) & 0xFF;
        FourBytes[3] = SLBA & 0xFF;
        ptr2Buffer = &r10CmdBlk[current_spare_blocks_1][2];
        memcpy( ptr2Buffer, FourBytes, 4);
        io_hdr.cmdp = r10CmdBlk[current_spare_blocks_1];
        printf("Cmd buffer  00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F\n");
        printf("            -----------------------------------------------\n");
        printf("r10CmdBlk = ");
        for (j=0; j<16; j++)
           printf("%02X ", r10CmdBlk[current_spare_blocks_1][j]);
        printf("\n");

        if (ioctl(sg_fd, SG_IO, &io_hdr) < 0) {
           perror("sg_read_SM325: Inquiry SG_IO ioctl error");
           close(sg_fd);
        return 1;
        }

        /* now for the error processing */
        ok = 0;
        switch (sg_err_category3(&io_hdr)) {
           case SG_LIB_CAT_CLEAN:
              ok = 1;
              break;
           case SG_LIB_CAT_RECOVERED:
              printf("Recovered error on READ_10, continuing\n");
              ok = 1;
              break;
           default: /* won't bother decoding other categories */
              sg_chk_n_print3("READ_10 command error", &io_hdr, 1);
              break;
        }

        if (ok) { /* output result if it is available */
           printf("\n   PROCESSING MU NUMBER: %d\n", mu);
           printf("READ_10 duration=%u millisecs, resid=%d, msg_status=%d \n",
               io_hdr.duration, io_hdr.resid, (int)io_hdr.msg_status);

           /* Check the result to see if this MU has any BadBlock */
           memcpy( sense_buffer, io_hdr.sbp, sizeof(sense_buffer));
           memcpy( inBuff, io_hdr.dxferp, sizeof(inBuff));

#ifdef DEBUG_FLAG
           /* Print out io_hdr.deferp Reply Buffer */
           printf("   reply buffer  00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F 10 11 12 13 14 15 16 17 18 19 1A 1B 1C 1D 1E 1F\n");
           printf("                 -----------------------------------------------------------------------------------------------\n");
           for (i=0; i<3; i++) {
              printf("   0x%3X-0x%3X = ", 0x60+i*j, 0x60+(i*j)+31);
              for (j=0; j<32; j++)
                 printf("%02X ", inBuff[0x60+i+j]);
              printf("\n");
           }
           printf("\n");
#endif
           Current_SpareBlock[mu] = inBuff[0x65];

           printf("Current MU = %d\n", mu);
           printf("Current_SpareBlock   = %d (0x%02X)\n", Current_SpareBlock[mu], Current_SpareBlock[mu]);
        }

        /*  Host will now read the second command to get current spare blocks numbers */
        io_hdr.cmdp = r10CmdBlk[current_spare_blocks_2];
#ifdef DEBUG_FLAG
        printf("Cmd buffer  00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F\n");
        printf("            -----------------------------------------------\n");
        printf("r10CmdBlk = ");
        for (j=0; j<16; j++)
           printf("%02X ", r10CmdBlk[current_spare_blocks_2][j]);
        printf("\n");
#endif
        if (ioctl(sg_fd, SG_IO, &io_hdr) < 0) {
           perror("sg_read_SM325: Inquiry SG_IO ioctl error");
           close(sg_fd);
        return 1;
        }

        /* now for the error processing */
        ok = 0;
        switch (sg_err_category3(&io_hdr)) {
           case SG_LIB_CAT_CLEAN:
              ok = 1;
              break;
           case SG_LIB_CAT_RECOVERED:
              printf("Recovered error on READ_10, continuing\n");
              ok = 1;
              break;
           default: /* won't bother decoding other categories */
              sg_chk_n_print3("READ_10 command error", &io_hdr, 1);
              break;
        }

        if (ok) { /* output result if it is available */
           printf("\n   PROCESSING MU NUMBER: %d\n", mu);
           printf("READ_10 0x28 duration=%u millisecs, resid=%d, msg_status=%d \n",
               io_hdr.duration, io_hdr.resid, (int)io_hdr.msg_status);

           /* Check the result to see if this MU has any BadBlock */
           memcpy( sense_buffer, io_hdr.sbp, sizeof(sense_buffer));
           memcpy( inBuff, io_hdr.dxferp, sizeof(inBuff));

#ifdef DEBUG_FLAG
           /* Print out io_hdr.deferp Reply Buffer */
           printf("   reply buffer  00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F 10 11 12 13 14 15 16 17 18 19 1A 1B 1C 1D 1E 1F\n");
           printf("                 -----------------------------------------------------------------------------------------------\n");
           for (i=0; i<3; i++) {
              printf("   0x%3X-0x%3X = ", 0x60+i*j, 0x60+(i*j)+31);
              for (j=0; j<32; j++)
                 printf("%02X ", inBuff[0x60+i+j]);
              printf("\n");
           }
           printf("\n");
#endif
           Current_SpareBlock[mu] = inBuff[0x65];

           printf("Current MU = %d\n", mu);
           printf("Current_SpareBlock   = %d (0x%02X)\n", Current_SpareBlock[mu], Current_SpareBlock[mu]);
        }

    }  /* end of for loop each mu */
    }

    /* 6+. Write (10) command for each MU  0x2A */
    /********************************************
    {
        printf("\n  STEP 6+: WRITE (10) COMMAND FOR EACH MU 0x2A\n");
    io_hdr.cmd_len = sizeof(r10CmdBlk[write_10]);
    io_hdr.dxfer_len = READ10_REPLY_LEN;
    io_hdr.dxferp = inBuff;

    /* Write to all LBA *
    for (lba=0; lba<LBA_per_MU; lba++)
//    for (lba=0; lba<10; lba++)
    {
        SLBA = lba;

        FourBytes[0] = (SLBA >> 24) & 0xFF;
        FourBytes[1] = (SLBA >> 16) & 0xFF;
        FourBytes[2] = (SLBA >> 8) & 0xFF;
        FourBytes[3] = SLBA & 0xFF;
        ptr2Buffer = &r10CmdBlk[write_10][2];

        memcpy( ptr2Buffer, FourBytes, 4);
        io_hdr.dxfer_direction = SG_DXFER_TO_DEV;
        io_hdr.cmdp = r10CmdBlk[write_10];
        printf("Write 10 r10CmdBlk = ");
        for (j=0; j<16; j++)
           printf("%02X ", r10CmdBlk[write_10][j]);
//        printf("\n");

        if (ioctl(sg_fd, SG_IO, &io_hdr) < 0) {
           perror("sg_read_SM325: Inquiry SG_IO ioctl error");
           close(sg_fd);
        return 1;
        }

        // now for the error processing
        ok = 0;
        switch (sg_err_category3(&io_hdr)) {
           case SG_LIB_CAT_CLEAN:
              ok = 1;
              break;
           case SG_LIB_CAT_RECOVERED:
              printf("Recovered error on READ_10, continuing\n");
              ok = 1;
              break;
           default: // won't bother decoding other categories
              sg_chk_n_print3("WRITE_10 command error", &io_hdr, 1);
              break;
        }

        if (ok) { // output result if it is available
//           printf("\n   PROCESSING MU NUMBER: %d\n", mu);
           printf(" duration=%u millisecs, resid=%d, msg_status=%d \n",
               io_hdr.duration, io_hdr.resid, (int)io_hdr.msg_status);

           // Check the result to see if this MU has any BadBlock
           memcpy( sense_buffer, io_hdr.sbp, sizeof(sense_buffer));
           memcpy( inBuff, io_hdr.dxferp, sizeof(inBuff));

#ifdef DEBUG_FLAG
           // Print out io_hdr.deferp Reply Buffer
           printf("   reply buffer  00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F 10 11 12 13 14 15 16 17 18 19 1A 1B 1C 1D 1E 1F\n");
           printf("                 -----------------------------------------------------------------------------------------------\n");
           for (i=0; i<3; i++) {
              printf("   0x%3X-0x%3X = ", 0x60+i*j, 0x60+(i*j)+31);
              for (j=0; j<32; j++)
                 printf("%02X ", inBuff[0x60+i+j]);
              printf("\n");
           }
           printf("\n");
#endif
           Current_SpareBlock[mu] = inBuff[0x65];

           if ((lba % 100) == 0)
           {
               printf("Current lba = %d\n", lba);
           }
#ifdef DEBUG_FLAG
           printf("Current MU = %d\n", mu);
           printf("Current_SpareBlock   = %d (0x%02X)\n", Current_SpareBlock[mu], Current_SpareBlock[mu]);
#endif
        }

        //  Host will now read the second command to get current spare blocks numbers
        io_hdr.cmdp = r10CmdBlk[current_spare_blocks_2];
        io_hdr.dxfer_direction = SG_DXFER_FROM_DEV;
#ifdef DEBUG_FLAG
        printf("Cmd buffer  00          04          08          0C       0F\n");
        printf("r10CmdBlk = ");
        for (j=0; j<16; j++)
           printf("%02X ", r10CmdBlk[current_spare_blocks_2][j]);
        printf("\n");
#endif

        if (ioctl(sg_fd, SG_IO, &io_hdr) < 0) {
           perror("sg_read_SM325: Inquiry SG_IO ioctl error");
           close(sg_fd);
        return 1;
        }

        // now for the error processing
        ok = 0;
        switch (sg_err_category3(&io_hdr)) {
           case SG_LIB_CAT_CLEAN:
              ok = 1;
              break;
           case SG_LIB_CAT_RECOVERED:
              printf("Recovered error on READ_10, continuing\n");
              ok = 1;
              break;
           default: // won't bother decoding other categories
              sg_chk_n_print3("READ_10 command error", &io_hdr, 1);
              break;
        }

        if (ok) { // output result if it is available
#ifdef DEBUG_FLAG
           printf("\n   PROCESSING MU NUMBER: %d\n", mu);
           printf("READ_10 for current spare blocks: duration=%u millisecs, resid=%d, msg_status=%d \n",
               io_hdr.duration, io_hdr.resid, (int)io_hdr.msg_status);
#endif

           // Check the result to see if this MU has any BadBlock
           memcpy( sense_buffer, io_hdr.sbp, sizeof(sense_buffer));
           memcpy( inBuff, io_hdr.dxferp, sizeof(inBuff));

#ifdef DEBUG_FLAG
           // Print out io_hdr.deferp Reply Buffer
           printf("   reply buffer  00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F 10 11 12 13 14 15 16 17 18 19 1A 1B 1C 1D 1E 1F\n");
           printf("                 -----------------------------------------------------------------------------------------------\n");
           for (i=0; i<3; i++) {
              printf("   0x%3X-0x%3X = ", 0x60+i*j, 0x60+(i*j)+31);
              for (j=0; j<32; j++)
                 printf("%02X ", inBuff[0x60+i+j]);
              printf("\n");
           }
           printf("\n");
#endif
           Current_SpareBlock[mu] = inBuff[0x65];

#ifdef DEBUG_FLAG
           printf("Current MU = %d\n", mu);
           printf("Current_SpareBlock   = %d (0x%02X)\n", Current_SpareBlock[mu], Current_SpareBlock[mu]);
#endif
        }

    }  // end of for loop each mu
    }
*/

    /* 6+. Write (16) command for each MU  0x8A */
    /********************************************/
    {
        printf("\n  STEP 6+: WRITE (16) COMMAND FOR EACH MU 0x8A\n");
    io_hdr.cmd_len = sizeof(r10CmdBlk[write_10]);
    io_hdr.dxferp = inBuff;
    io_hdr.iovec_count = 0;   // memset should take care of this

    for (loop=1; loop<=10; loop++)
    {
    /* Write to all LBA */
    for (lba=0; lba<LBA_per_MU; lba++)
//    for (lba=0; lba<10; lba++)
    {
        SLBA = lba;

        FourBytes[0] = (SLBA >> 24) & 0xFF;
        FourBytes[1] = (SLBA >> 16) & 0xFF;
        FourBytes[2] = (SLBA >> 8) & 0xFF;
        FourBytes[3] = SLBA & 0xFF;
        ptr2Buffer = &r10CmdBlk[write_10][6];

        memcpy( ptr2Buffer, FourBytes, 4);
        io_hdr.dxfer_len = READ10_CMD_LEN;
        io_hdr.dxfer_direction = SG_DXFER_TO_DEV;
        io_hdr.cmdp = r10CmdBlk[write_10];
        printf("Write 16 r10CmdBlk = ");
        for (j=0; j<16; j++)
           printf("%02X ", r10CmdBlk[write_10][j]);
//        printf("\n");

        if (ioctl(sg_fd, SG_IO, &io_hdr) < 0) {
           perror("sg_read_SM325: Inquiry SG_IO ioctl error");
           close(sg_fd);
        return 1;
        }

        /* now for the error processing */
        ok = 0;
        switch (sg_err_category3(&io_hdr)) {
           case SG_LIB_CAT_CLEAN:
              ok = 1;
              break;
           case SG_LIB_CAT_RECOVERED:
              printf("Recovered error on WRITE_16, continuing\n");
              ok = 1;
              break;
           default: // won't bother decoding other categories, unless it's a DID_ERROR
              if (io_hdr.host_status == 0x07) { // DID_ERROR is ok
                ok = 1;
              }
              else {
                sg_chk_n_print3("WRITE_16 command error", &io_hdr, 1);
                break;
              }
        }

        if (ok) { /* output result if it is available */
//           printf("\n   PROCESSING MU NUMBER: %d\n", mu);
           printf(" duration=%u millisecs, resid=%d, msg_status=%d \n",
               io_hdr.duration, io_hdr.resid, (int)io_hdr.msg_status);

           /* Check the result to see if this MU has any BadBlock */
           memcpy( sense_buffer, io_hdr.sbp, sizeof(sense_buffer));
           memcpy( inBuff, io_hdr.dxferp, sizeof(inBuff));

#ifdef DEBUG_FLAG
           /* Print out io_hdr.deferp Reply Buffer */
           printf("   reply buffer  00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F 10 11 12 13 14 15 16 17 18 19 1A 1B 1C 1D 1E 1F\n");
           printf("                 -----------------------------------------------------------------------------------------------\n");
           for (i=0; i<3; i++) {
              printf("   0x%3X-0x%3X = ", 0x60+i*j, 0x60+(i*j)+31);
              for (j=0; j<32; j++)
                 printf("%02X ", inBuff[0x60+i+j]);
              printf("\n");
           }
           printf("\n");
#endif
           Current_SpareBlock[mu] = inBuff[0x65];

           if ((lba % 100) == 0)
           {
               printf("Current lba = %d of loop number %d\n", lba, loop);
           }
#ifdef DEBUG_FLAG
           printf("Current MU = %d\n", mu);
           printf("Current_SpareBlock   = %d (0x%02X)\n", Current_SpareBlock[mu], Current_SpareBlock[mu]);
#endif
        }

        /*  Host will now read the second command to get current spare blocks numbers */
        io_hdr.cmdp = r10CmdBlk[current_spare_blocks_2];
        io_hdr.dxfer_len = READ10_REPLY_LEN;
        io_hdr.dxfer_direction = SG_DXFER_FROM_DEV;
#ifdef DEBUG_FLAG
        printf("Cmd buffer  00          04          08          0C       0F\n");
        printf("r10CmdBlk = ");
        for (j=0; j<16; j++)
           printf("%02X ", r10CmdBlk[current_spare_blocks_2][j]);
        printf("\n");
#endif

        if (ioctl(sg_fd, SG_IO, &io_hdr) < 0) {
           perror("sg_read_SM325: Inquiry SG_IO ioctl error");
           close(sg_fd);
        return 1;
        }

        /* now for the error processing */
        ok = 0;
        switch (sg_err_category3(&io_hdr)) {
           case SG_LIB_CAT_CLEAN:
              ok = 1;
              break;
           case SG_LIB_CAT_RECOVERED:
              printf("Recovered error on READ_10, continuing\n");
              ok = 1;
              break;
           default: /* won't bother decoding other categories */
              sg_chk_n_print3("READ_10 command error", &io_hdr, 1);
              break;
        }

        if (ok) { /* output result if it is available */
#ifdef DEBUG_FLAG
           printf("\n   PROCESSING MU NUMBER: %d\n", mu);
           printf("READ_10 for current spare blocks: duration=%u millisecs, resid=%d, msg_status=%d \n",
               io_hdr.duration, io_hdr.resid, (int)io_hdr.msg_status);
#endif

           /* Check the result to see if this MU has any BadBlock */
           memcpy( sense_buffer, io_hdr.sbp, sizeof(sense_buffer));
           memcpy( inBuff, io_hdr.dxferp, sizeof(inBuff));

#ifdef DEBUG_FLAG
           /* Print out io_hdr.deferp Reply Buffer */
           printf("   reply buffer  00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F 10 11 12 13 14 15 16 17 18 19 1A 1B 1C 1D 1E 1F\n");
           printf("                 -----------------------------------------------------------------------------------------------\n");
           for (i=0; i<3; i++) {
              printf("   0x%3X-0x%3X = ", 0x60+i*j, 0x60+(i*j)+31);
              for (j=0; j<32; j++)
                 printf("%02X ", inBuff[0x60+i+j]);
              printf("\n");
           }
           printf("\n");
#endif
           Current_SpareBlock[mu] = inBuff[0x65];

#ifdef DEBUG_FLAG
           printf("Current MU = %d\n", mu);
           printf("Current_SpareBlock   = %d (0x%02X)\n", Current_SpareBlock[mu], Current_SpareBlock[mu]);
#endif
        }

    }  /* end of for loop each mu */
    }  // loop for 10 times
    }

    /* 7. Prepare READ_10 command for reading LED setting information  0xF0 */
    /************************************************************/
    {
    printf("7. READ Bad Block command 0xF0 for reading LED setting information\n");
    memset(&io_hdr, 0, sizeof(sg_io_hdr_t));
    io_hdr.interface_id = 'S';
    io_hdr.cmd_len = sizeof(r10CmdBlk[read_LED]);
    /* io_hdr.iovec_count = 0; */  /* memset takes care of this */
    io_hdr.mx_sb_len = sizeof(sense_buffer);
    io_hdr.dxfer_direction = SG_DXFER_FROM_DEV;
    io_hdr.dxfer_len = READ10_REPLY_LEN;
    io_hdr.dxferp = inBuff;
    io_hdr.cmdp = r10CmdBlk[read_LED];
    io_hdr.sbp = sense_buffer;
    io_hdr.timeout = 20000;     /* 20000 millisecs == 20 seconds */
    /* io_hdr.flags = 0; */     /* take defaults: indirect IO, etc */
    /* io_hdr.pack_id = 0; */
    /* io_hdr.usr_ptr = NULL; */

    if (ioctl(sg_fd, SG_IO, &io_hdr) < 0) {
	   perror("sg_read_SM325: Inquiry SG_IO ioctl error");
	   close(sg_fd);
	   return 1;
    }

    /* now for the error processing */
    ok = 0;
    switch (sg_err_category3(&io_hdr)) {
       case SG_LIB_CAT_CLEAN:
	      ok = 1;
	      break;
       case SG_LIB_CAT_RECOVERED:
	      printf("Recovered error on READ_10, continuing\n");
	      ok = 1;
	      break;
       default: /* won't bother decoding other categories */
	      sg_chk_n_print3("READ_10 command error", &io_hdr, 1);
	      break;
    }

    if (ok) { /* output result if it is available */
	    memcpy( sense_buffer, io_hdr.sbp, sizeof(sense_buffer));

#ifdef DEBUG_FLAG
	    printf("\n  STEP 2: READ LED SETTING INFORMATION\n");
	    /* Print out io_hdr.deferp */
	    printf("   reply buffer  00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F 10 11 12 13 14 15 16 17 18 19 1A 1B 1C 1D 1E 1F\n");
	    printf("                 -----------------------------------------------------------------------------------------------\n");
	    memcpy( inBuff, io_hdr.dxferp, sizeof(inBuff));
	    for (i=0; i<32; i++)  /* 32 rows */
	    {
	      printf("       %3d-%3d = ", i*j, (i*j)+15);

	      for (j=0; j<16; j++)
	         printf("%02X ", inBuff[(i*16)+j]);
	   
	      printf("\n");
	      printf("Char   %3d-%3d = ", i*j, (i*j)+15);

	      for (j=0; j<16; j++)
	         printf("%2c ", inBuff[(i*16)+j]);
	   
	      printf("\n");
	    }
        printf("\n");
#endif
        for (i=0; i<18; i++)
        {
	        UnitProductNumber[i] = inBuff[86+(i*2)];
        }
        
        strcpy(filename, UnitProductNumber);
        strcat(filename, ".txt");
        
        pFile=fopen(filename, "a");
        if(pFile==NULL)
        {
            printf("Error opening log file.\n");
        }
    
        if (strncmp(Viking, VendorID, 2) != 0)
        {
            printf("NO RECONFIG - Not a Viking drive.\n");
            fprintf(pFile, "%s, %s, %s, %s", UnitProductNumber, UnitSerialNumber, VendorID, asctime(timeinfo));
            fclose(pFile);
            return 0;
        }
        
        LED_Status_Byte = inBuff[0x187];
        LED_Ready       = (LED_Status_Byte & 0x06) >> 1;
        LED_Busy        = (LED_Status_Byte & 0x60) >> 5;

#ifdef DEBUG_FLAG
        printf("LED_Status_Byte = 0x%X\n", LED_Status_Byte);
        printf("LED_Ready       = %d\n", LED_Ready);
        printf("LED_Busy        = %d\n", LED_Busy);
        printf("Done\n");
#endif
    }
    }

    /* 8. Prepare READ_10 command for writing LED setting information  0xF1 */
    /************************************************************/
    {
    printf("8. READ Bad Block command 0xF1 for writing LED setting information\n");
#ifdef DEBUG_FLAG
	printf("\n  STEP 3: WRITE LED SETTING INFORMATION\n");
#endif
    if (inBuff[0x187] == 0x82)
    {
        LED_result = 0;
//        printf("Already configured ");
    }
    else if (inBuff[0x187] == 0x80)
    {
        inBuff[0x187] = 0x82;
#ifdef DEBUG_FLAG
        printf("Updating the CID table...\n");
#endif
    }
    
    memset(&io_hdr, 0, sizeof(sg_io_hdr_t));
    io_hdr.interface_id = 'S';
    io_hdr.cmd_len = sizeof(r10CmdBlk[write_LED]);
    /* io_hdr.iovec_count = 0; */  /* memset takes care of this */
    io_hdr.mx_sb_len = sizeof(sense_buffer);
    io_hdr.dxfer_direction = SG_DXFER_TO_DEV;
    io_hdr.dxfer_len = READ10_REPLY_LEN;
    io_hdr.dxferp = inBuff;
    io_hdr.cmdp = r10CmdBlk[write_LED];
    io_hdr.sbp = sense_buffer;
    io_hdr.timeout = 20000;     /* 20000 millisecs == 20 seconds */
    /* io_hdr.flags = 0; */     /* take defaults: indirect IO, etc */
    /* io_hdr.pack_id = 0; */
    /* io_hdr.usr_ptr = NULL; */

    if (ioctl(sg_fd, SG_IO, &io_hdr) < 0) {
	   perror("sg_read_SM325: Inquiry SG_IO ioctl error");
	   close(sg_fd);
	   return 1;
    }

    /* now for the error processing */
    ok = 0;
    switch (sg_err_category3(&io_hdr)) {
       case SG_LIB_CAT_CLEAN:
	      ok = 1;
	      break;
       case SG_LIB_CAT_RECOVERED:
	      printf("Recovered error on READ_10, continuing\n");
	      ok = 1;
	      break;
       default: /* won't bother decoding other categories */
	      sg_chk_n_print3("READ_10 command error", &io_hdr, 1);
	      break;
    }

    if (ok) { /* output result if it is available */
	    memcpy( sense_buffer, io_hdr.sbp, sizeof(sense_buffer));

#ifdef DEBUG_FLAG
	    /* Print out io_hdr.deferp */
	    printf("   reply buffer  00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F 10 11 12 13 14 15 16 17 18 19 1A 1B 1C 1D 1E 1F\n");
	    printf("                 -----------------------------------------------------------------------------------------------\n");
	    memcpy( inBuff, io_hdr.dxferp, sizeof(inBuff));
	    for (i=0; i<16; i++)  /* 16 rows */
	    {
	      printf("       %3d-%3d = ", i*j, (i*j)+31);

	      for (j=0; j<32; j++)
	         printf("%02X ", inBuff[(i*32)+j]);
	   
	      printf("\n");
	    }
        printf("\n");
#endif
        LED_Status_Byte = inBuff[0x187];
        LED_Ready       = (LED_Status_Byte & 0x06) >> 1;
        LED_Busy        = (LED_Status_Byte & 0x60) >> 5;

#ifdef DEBUG_FLAG
        printf("LED_Status_Byte = 0x%X\n", LED_Status_Byte);
        printf("LED_Ready       = %d\n", LED_Ready);
        printf("LED_Busy        = %d\n", LED_Busy);
        printf("Done\n");
#endif
    }
    }

    /* 9. Prepare READ_10 command for reading LED setting information  0xF0 */
    /************************************************************/
    {
    printf("9. READ Bad Block command 0xF0 for reading LED setting information\n");
    memset(&io_hdr, 0, sizeof(sg_io_hdr_t));
    io_hdr.interface_id = 'S';
    io_hdr.cmd_len = sizeof(r10CmdBlk[read_LED]);
    /* io_hdr.iovec_count = 0; */  /* memset takes care of this */
    io_hdr.mx_sb_len = sizeof(sense_buffer);
    io_hdr.dxfer_direction = SG_DXFER_FROM_DEV;
    io_hdr.dxfer_len = READ10_REPLY_LEN;
    io_hdr.dxferp = inBuff;
    io_hdr.cmdp = r10CmdBlk[read_LED];
    io_hdr.sbp = sense_buffer;
    io_hdr.timeout = 20000;     /* 20000 millisecs == 20 seconds */
    /* io_hdr.flags = 0; */     /* take defaults: indirect IO, etc */
    /* io_hdr.pack_id = 0; */
    /* io_hdr.usr_ptr = NULL; */

    if (ioctl(sg_fd, SG_IO, &io_hdr) < 0) {
	   perror("sg_read_SM325: Inquiry SG_IO ioctl error");
	   close(sg_fd);
	   return 1;
    }

    /* now for the error processing */
    ok = 0;
    switch (sg_err_category3(&io_hdr)) {
       case SG_LIB_CAT_CLEAN:
	      ok = 1;
	      break;
       case SG_LIB_CAT_RECOVERED:
	      printf("Recovered error on READ_10, continuing\n");
	      ok = 1;
	      break;
       default: /* won't bother decoding other categories */
	      sg_chk_n_print3("READ_10 command error", &io_hdr, 1);
	      break;
    }

    if (ok) { /* output result if it is available */
	    memcpy( sense_buffer, io_hdr.sbp, sizeof(sense_buffer));

#ifdef DEBUG_FLAG
	    printf("\n  STEP 4: READ LED SETTING INFORMATION AFTER A WRITE\n");
	    /* Print out io_hdr.deferp */
	    printf("   reply buffer  00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F 10 11 12 13 14 15 16 17 18 19 1A 1B 1C 1D 1E 1F\n");
	    printf("                 -----------------------------------------------------------------------------------------------\n");
	    memcpy( inBuff, io_hdr.dxferp, sizeof(inBuff));
	    for (i=0; i<16; i++)  /* 16 rows */
	    {
	      printf("       %3d-%3d = ", i*j, (i*j)+31);

	      for (j=0; j<32; j++)
	         printf("%02X ", inBuff[(i*32)+j]);
	   
	      printf("\n");
	    }
        printf("\n");
#endif
        LED_Status_Byte = inBuff[0x187];
        LED_Ready       = (LED_Status_Byte & 0x06) >> 1;
        LED_Busy        = (LED_Status_Byte & 0x60) >> 5;

        if (inBuff[0x187] == 0x82)
        {
            LED_result = 1;
//            printf("PASSED.\n");
//            fprintf(pFile, "%s, %s, PASSED, %s", UnitProductNumber, UnitSerialNumber, asctime(timeinfo));
        }
        else 
        {
            LED_result = 2;
            printf("FAILED - Re-test or reject.\n");
            fprintf(pFile, "%s, %s, FAILED, %s", UnitProductNumber, UnitSerialNumber, asctime(timeinfo));
        }
    }
    }
    
    /* 10. Prepare READ_10 command for reset the drive  0xF0 */
    /************************************************************/
    {
    printf("10. READ Bad Block command 0xF0 for reset the eUSB drive\n");
#ifdef DEBUG_FLAG
	printf("\n  STEP 5: RESET THE USB DRIVE...\n");
//#endif
    memset(&io_hdr, 0, sizeof(sg_io_hdr_t));
    io_hdr.interface_id = 'S';
    io_hdr.cmd_len = sizeof(r10CmdBlk[reset_drive]);
    /* io_hdr.iovec_count = 0; */  /* memset takes care of this */
    io_hdr.mx_sb_len = sizeof(sense_buffer);
    io_hdr.dxfer_direction = SG_DXFER_TO_DEV;
    io_hdr.dxfer_len = READ10_REPLY_LEN;
    io_hdr.dxferp = inBuff;
    io_hdr.cmdp = r10CmdBlk[reset_drive];
    io_hdr.sbp = sense_buffer;
    io_hdr.timeout = 20000;     /* 20000 millisecs == 20 seconds */
    /* io_hdr.flags = 0; */     /* take defaults: indirect IO, etc */
    /* io_hdr.pack_id = 0; */
    /* io_hdr.usr_ptr = NULL; */

    if (ioctl(sg_fd, SG_IO, &io_hdr) < 0) {
	   perror("sg_read_SM325: Inquiry SG_IO ioctl error");
	   close(sg_fd);
	   return 1;
    }

    /* now for the error processing */
    ok = 0;
    switch (sg_err_category3(&io_hdr)) {
       case SG_LIB_CAT_CLEAN:
	      ok = 1;
	      break;
       case SG_LIB_CAT_RECOVERED:
	      printf("Recovered error on READ_10, continuing\n");
	      ok = 1;
	      break;
       default: /* won't bother decoding other categories */
	      break;
    }

    if (ok) { /* output result if it is available */
	    memcpy( sense_buffer, io_hdr.sbp, sizeof(sense_buffer));

//#ifdef DEBUG_FLAG
	    /* Print out io_hdr.deferp */
	    printf("   reply buffer  00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F 10 11 12 13 14 15 16 17 18 19 1A 1B 1C 1D 1E 1F\n");
	    printf("                 -----------------------------------------------------------------------------------------------\n");
	    memcpy( inBuff, io_hdr.dxferp, sizeof(inBuff));
	    for (i=0; i<16; i++)  /* 16 rows */
	    {
	      printf("       %3d-%3d = ", i*j, (i*j)+31);

	      for (j=0; j<32; j++)
	         printf("%02X ", inBuff[(i*32)+j]);
	   
	      printf("\n");
	    }
        printf("\n");
//#endif
    }
#endif
}

    /* 11. Prepare READ_10 command for reading LED setting information  0xF0 */
    /************************************************************/
    {
    printf("11. READ Bad Block command 0xF0 for reading LED setting information\n");
    memset(&io_hdr, 0, sizeof(sg_io_hdr_t));
    io_hdr.interface_id = 'S';
    io_hdr.cmd_len = sizeof(r10CmdBlk[read_LED]);
    /* io_hdr.iovec_count = 0; */  /* memset takes care of this */
    io_hdr.mx_sb_len = sizeof(sense_buffer);
    io_hdr.dxfer_direction = SG_DXFER_FROM_DEV;
    io_hdr.dxfer_len = READ10_REPLY_LEN;
    io_hdr.dxferp = inBuff;
    io_hdr.cmdp = r10CmdBlk[read_LED];
    io_hdr.sbp = sense_buffer;
    io_hdr.timeout = 20000;     /* 20000 millisecs == 20 seconds */
    /* io_hdr.flags = 0; */     /* take defaults: indirect IO, etc */
    /* io_hdr.pack_id = 0; */
    /* io_hdr.usr_ptr = NULL; */

    if (ioctl(sg_fd, SG_IO, &io_hdr) < 0) {
       perror("sg_read_SM325: Inquiry SG_IO ioctl error");
       close(sg_fd);
       return 1;
    }

    /* now for the error processing */
    ok = 0;
    switch (sg_err_category3(&io_hdr)) {
       case SG_LIB_CAT_CLEAN:
          ok = 1;
          break;
       case SG_LIB_CAT_RECOVERED:
          printf("Recovered error on READ_10, continuing\n");
          ok = 1;
          break;
       default: /* won't bother decoding other categories */
          sg_chk_n_print3("READ_10 command error", &io_hdr, 1);
          break;
    }

    if (ok) { /* output result if it is available */
        memcpy( sense_buffer, io_hdr.sbp, sizeof(sense_buffer));

#ifdef DEBUG_FLAG
        printf("\n  STEP 2: READ LED SETTING INFORMATION\n");
        /* Print out io_hdr.deferp */
        printf("   reply buffer  00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F 10 11 12 13 14 15 16 17 18 19 1A 1B 1C 1D 1E 1F\n");
        printf("                 -----------------------------------------------------------------------------------------------\n");
        memcpy( inBuff, io_hdr.dxferp, sizeof(inBuff));
        for (i=0; i<32; i++)  /* 32 rows */
        {
          printf("       %3d-%3d = ", i*j, (i*j)+15);

          for (j=0; j<16; j++)
             printf("%02X ", inBuff[(i*16)+j]);

          printf("\n");
          printf("Char   %3d-%3d = ", i*j, (i*j)+15);

          for (j=0; j<16; j++)
             printf("%2c ", inBuff[(i*16)+j]);

          printf("\n");
        }
        printf("\n");
#endif
        for (i=0; i<18; i++)
        {
            UnitProductNumber[i] = inBuff[86+(i*2)];
        }

        strcpy(filename, UnitProductNumber);
        strcat(filename, ".txt");

        pFile=fopen(filename, "a");
        if(pFile==NULL)
        {
            printf("Error opening log file.\n");
        }

        if (strncmp(Viking, VendorID, 2) != 0)
        {
            printf("NO RECONFIG - Not a Viking drive.\n");
            fprintf(pFile, "%s, %s, %s, %s", UnitProductNumber, UnitSerialNumber, VendorID, asctime(timeinfo));
            fclose(pFile);
            return 0;
        }

        LED_Status_Byte = inBuff[0x187];
        LED_Ready       = (LED_Status_Byte & 0x06) >> 1;
        LED_Busy        = (LED_Status_Byte & 0x60) >> 5;

#ifdef DEBUG_FLAG
        printf("LED_Status_Byte = 0x%X\n", LED_Status_Byte);
        printf("LED_Ready       = %d\n", LED_Ready);
        printf("LED_Busy        = %d\n", LED_Busy);
        printf("Done\n");
#endif
    }
    }


    /******************************/
    /*    Print out the results   */
    /******************************/
#ifdef DEBUG_FLAG1
    printf("\n   *********** THE RESULT IS: **********\n\n");

    printf("Vendor Identification  : %.8s\n", VendorID);
    printf("Product Identification : %.16s\n", ProductID);
    printf("Product Revision Level : %.4s\n", ProductRevision);
    printf("Unit Serial Number     : %.16s\n", UnitSerialNumber);
    printf("Block Size : %d Bytes\n", BlockSize);
    printf("Disk Size  : %.2f MiB or %.2f MB\n\n", (float)(DiskSize / BYTES_IN_MiB), (float)(DiskSize / BYTES_IN_MB));

    switch (LED_result)
    {
       case 0:
          printf("The drive has already been updated.\n");
          break;
       case 1:
          printf("PASSED.\n");
          break;
       case 2:
          printf("FAILED.  Re-test the drive or send to RMA.\n");
          break;
       default:   
          break;
    }
#endif
    
    fclose(pFile);
    close(sg_fd);
    return 0;
}
