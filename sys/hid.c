/*++

Copyright (c) Microsoft Corporation.  All rights reserved.

    THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF ANY
    KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
    IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A PARTICULAR
    PURPOSE.

Module Name:

    hid.c

Abstract:

    Code for handling HID related requests

Author:


Environment:

    kernel mode only

Revision History:

--*/

#include "hidusbfx2.h"

#define REPORTID_TOUCH 0x7f
#define REPORT_TOUCH_SIZE 6 // Report ID + 5 bytes data

#define REQ_MIN_BUFFER_SIZE 64

#define INVERT_X 1
#define INVERT_Y 1

static const UCHAR g_ReportSizes[] = {
  [0x00] = 0,
  [0x01] = 4,
  [0x02] = 2,
  [0x03] = 5,
  [0x04] = 2,
  [0x07] = 4,
  [0x09] = 5,
  [0x20] = 64,
  [0x05] = 3,
  [0x06] = 3,
  [0x14] = 4,
  [0x08] = 4,
  [0x15] = 2,
};

HID_REPORT_DESCRIPTOR       G_DefaultReportDescriptor[] = {
    0x05, 0x0d,                         // USAGE_PAGE (Digitizers)
    0x09, 0x04,                         // USAGE (Touch Screen)
    0xa1, 0x01,                         // COLLECTION (Application)
    0x85, REPORTID_TOUCH,               //   REPORT_ID (Touch)
    0x09, 0x20,                         //   USAGE (Stylus)
    0xa1, 0x00,                         //   COLLECTION (Physical)
    0x09, 0x42,                         //     USAGE (Tip Switch)
    0x09, 0x32,                         //     USAGE (In Range)
    0x15, 0x00,                         //     LOGICAL_MINIMUM (0)
    0x25, 0x01,                         //     LOGICAL_MAXIMUM (1)
    0x75, 0x01,                         //     REPORT_SIZE (1)
    0x95, 0x02,                         //     REPORT_COUNT (2)
    0x81, 0x02,                         //     INPUT (Data,Var,Abs)
    0x95, 0x06,                         //     REPORT_COUNT (6)
    0x81, 0x03,                         //     INPUT (Cnst,Ary,Abs)
    0x05, 0x01,                         //     USAGE_PAGE (Generic Desktop)
    0x09, 0x30,                         //     USAGE (X)
    0x09, 0x31,                         //     USAGE (Y)
    0x15, 0x00,                         //     LOGICAL_MINIMUM (0)
    0x26, 0xff, 0x03,                   //     LOGICAL_MAXIMUM (1023)
    0x75, 0x10,                         //     REPORT_SIZE (16)
    0x95, 0x02,                         //     REPORT_COUNT (2)
    0x81, 0x02,                         //     INPUT (Data,Var,Abs)
    0xc0,                               //   END_COLLECTION
    0xc0,                               // END_COLLECTION
};

static PVOID USBPcapURBGetBufferPointer(ULONG length,
    PVOID buffer,
    PMDL  bufferMDL)
{
    UNREFERENCED_PARAMETER(length);

    ASSERT((length == 0) ||
        ((length != 0) && (buffer != NULL || bufferMDL != NULL)));

    if (buffer != NULL)
    {
        return buffer;
    }
    else if (bufferMDL != NULL)
    {
        PVOID address = MmGetSystemAddressForMdlSafe(bufferMDL,
            NormalPagePriority);
        return address;
    }
    else
    {
        //DkDbgStr("Invalid buffer!");
        return NULL;
    }
}

static VOID
LecTouchIoInternalDeviceControlComplete(
    IN WDFREQUEST Request,
    IN WDFIOTARGET Target,
    IN PWDF_REQUEST_COMPLETION_PARAMS Params,
    IN WDFCONTEXT Context
)
{
    UNREFERENCED_PARAMETER(Context);

    WDFDEVICE           device;
    PDEVICE_EXTENSION   devContext = NULL;

    device = WdfIoTargetGetDevice(Target);
    devContext = GetDeviceContext(device);

    PURB pUrb = (PURB)IoGetCurrentIrpStackLocation(WdfRequestWdmGetIrp(Request))->Parameters.Others.Argument1;

    switch (pUrb->UrbHeader.Function) 
    {

    case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER:
    {
        // Add our own touch report based on parsed stylus reports.

        struct _URB_BULK_OR_INTERRUPT_TRANSFER *req = (struct _URB_BULK_OR_INTERRUPT_TRANSFER *)pUrb;

        PUCHAR buf = (PUCHAR)USBPcapURBGetBufferPointer(req->TransferBufferLength,
            req->TransferBuffer, req->TransferBufferMDL);

        if (!buf) {
            break;
        }

        BOOLEAN touching = FALSE;
        for (size_t idx = 0; idx < req->TransferBufferLength;) {
            if (buf[idx] == 0x01) { // Stylus
                // Update the latest coordinates.
                LONG x = ((((LONG)buf[idx + 1] & 0x0c) << 6) | (LONG)buf[idx + 2]);
                LONG y = ((((LONG)buf[idx + 1] & 0x03) << 8) | (LONG)buf[idx + 3]);
                if (INVERT_X) {
                  x = (1 << 10) - x - 1;
                }
                if (INVERT_Y) {
                  y = (1 << 10) - y - 1;
                }
                // Only accept a point if the previous point is close to current point.
                if (!devContext->RecentChange ||
                        (labs(devContext->LastX - x) < 256 &&
                         labs(devContext->LastY - y) < 256)) {
                    devContext->LastX = x;
                    devContext->LastY = y;
                }
                devContext->RecentChange = touching = TRUE;
            }

            const UCHAR report_size = (buf[idx] < sizeof(g_ReportSizes) / sizeof(g_ReportSizes[0])) ?
                g_ReportSizes[buf[idx]] : 0;
            if (report_size) {
                idx += report_size;
            } else {
                break;
            }
        }

        if (devContext->RecentChange && (REQ_MIN_BUFFER_SIZE - req->TransferBufferLength) >= REPORT_TOUCH_SIZE) {
            devContext->RecentChange = touching;

            // We have room to add our own report.
            size_t idx = req->TransferBufferLength;
            buf[idx++] = REPORTID_TOUCH;
            buf[idx++] = (touching ? 0x03 : 0x00); // Tip Switch + In Range
            buf[idx++] = (UCHAR)devContext->LastX;
            buf[idx++] = (UCHAR)(devContext->LastX >> 8);
            buf[idx++] = (UCHAR)devContext->LastY;
            buf[idx++] = (UCHAR)(devContext->LastY >> 8);
            req->TransferBufferLength = idx;
        }

        break;
    }

    case URB_FUNCTION_CONTROL_TRANSFER:
    {
        struct _URB_CONTROL_TRANSFER* pTransfer = (struct _URB_CONTROL_TRANSFER*) pUrb;

        DbgPrint("pDescriptorRequest->TransferBufferLength = %d\n", pTransfer->TransferBufferLength);

        if (pTransfer->SetupPacket[0] == 0x80 && pTransfer->SetupPacket[1] == 0x06 &&
                pTransfer->SetupPacket[2] == 0x00 && pTransfer->SetupPacket[3] == 0x02 &&
                pTransfer->SetupPacket[4] == 0x00 && pTransfer->SetupPacket[5] == 0x00 &&
                pTransfer->SetupPacket[6] == 0x22 && pTransfer->SetupPacket[7] == 0x00) {
            // Configuration Descriptor
            PUCHAR buf = (PUCHAR)USBPcapURBGetBufferPointer(pTransfer->TransferBufferLength,
                pTransfer->TransferBuffer, pTransfer->TransferBufferMDL);

            size_t length = ((size_t)buf[0x1a] << 8) | (size_t)buf[0x19];
            length += sizeof(G_DefaultReportDescriptor); // Append to existing report descriptor.

            buf[0x1a] = (UCHAR)(length >> 8);
            buf[0x19] = (UCHAR)(length & 0xFF);
        }
        else if (pTransfer->SetupPacket[0] == 0x81 && pTransfer->SetupPacket[1] == 0x06 &&
                pTransfer->SetupPacket[2] == 0x00 && pTransfer->SetupPacket[3] == 0x22 &&
                pTransfer->SetupPacket[4] == 0x00 && pTransfer->SetupPacket[5] == 0x00) {
            // HID Report Descriptor
            // NOTE: Reallocating TransferBuffer is useless because it's a pointer provided by the upper driver.
            // But since we reported a sizeof(G_DefaultReportDescriptor) length, it should be allocated the right size.
            // Only the lower USB driver set TransferBufferLength back to 241, the original Report Descriptor size, which can be misleading.

            size_t append_size = ((size_t)pTransfer->SetupPacket[7] << 8) | (size_t)pTransfer->SetupPacket[6];
            if (append_size > sizeof(G_DefaultReportDescriptor)) {
                append_size = sizeof(G_DefaultReportDescriptor);
            }

            PUCHAR buf = (PUCHAR)USBPcapURBGetBufferPointer(pTransfer->TransferBufferLength,
                pTransfer->TransferBuffer, pTransfer->TransferBufferMDL);

            RtlCopyMemory(&buf[pTransfer->TransferBufferLength], &G_DefaultReportDescriptor[0], append_size);
            pTransfer->TransferBufferLength += append_size;
        }

        break;
    }

    default:
        break;
    }

    WdfRequestComplete(Request, Params->IoStatus.Status);
}

static VOID
LecTouchForwardRequest(
    IN WDFDEVICE Device,
    IN WDFREQUEST Request
)
{
    WDF_REQUEST_SEND_OPTIONS options;
    PDEVICE_EXTENSION          devContext;
    NTSTATUS                 status;

    //
    // Get the context that we setup during DeviceAdd processing
    //
    devContext = GetDeviceContext(Device);

    //
    // We're just going to be passing this request on with 
    //  zero regard for what happens to it. Therefore, we'll
    //  use the WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET option
    //
    WDF_REQUEST_SEND_OPTIONS_INIT(
        &options,
        WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET);


    //
    // And send it!
    // 
    if (!WdfRequestSend(Request,
        devContext->TargetToSendRequestsTo,
        &options)) {

        //
        // Oops! Something bad happened, complete the request
        //
        status = WdfRequestGetStatus(Request);
        WdfRequestComplete(Request, status);
    }
    return;
}

VOID
HidFx2EvtInternalDeviceControl(
    IN WDFQUEUE     Queue,
    IN WDFREQUEST   Request,
    IN size_t       OutputBufferLength,
    IN size_t       InputBufferLength,
    IN ULONG        IoControlCode
    )
/*++

Routine Description:

    This event is called when the framework receives 
    IRP_MJ_INTERNAL DEVICE_CONTROL requests from the system.

Arguments:

    Queue - Handle to the framework queue object that is associated
            with the I/O request.
    Request - Handle to a framework request object.

    OutputBufferLength - length of the request's output buffer,
                        if an output buffer is available.
    InputBufferLength - length of the request's input buffer,
                        if an input buffer is available.

    IoControlCode - the driver-defined or system-defined I/O control code
                    (IOCTL) that is associated with the request.
Return Value:

    VOID

--*/

{
    NTSTATUS            status = STATUS_SUCCESS;
    WDFDEVICE           device;
    PDEVICE_EXTENSION   devContext = NULL;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    device = WdfIoQueueGetDevice(Queue);
    devContext = GetDeviceContext(device);

    if (IoControlCode == IOCTL_INTERNAL_USB_SUBMIT_URB)
    {
        PURB pUrb = (PURB)IoGetCurrentIrpStackLocation(WdfRequestWdmGetIrp(Request))->Parameters.Others.Argument1;

        switch (pUrb->UrbHeader.Function)
        {

        case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER:
            {
                struct _URB_BULK_OR_INTERRUPT_TRANSFER *req = (struct _URB_BULK_OR_INTERRUPT_TRANSFER *)pUrb;
                
                if (req->TransferBufferLength < REQ_MIN_BUFFER_SIZE) {
                    // Assume we have a minimum buffer size.
                    break;
                }
            }
            // Fall-through

        case URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE:
        case URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE:
            {
                WdfRequestFormatRequestUsingCurrentType(Request);

                WdfRequestSetCompletionRoutine(Request, LecTouchIoInternalDeviceControlComplete, NULL);

                if (!WdfRequestSend(Request, devContext->TargetToSendRequestsTo, NULL)) {
                    // Oops! Something bad happened, complete the request
                    status = WdfRequestGetStatus(Request);
                    WdfRequestComplete(Request, status);
                }
                return;
            }

        default:
            break;
        }
    }

    LecTouchForwardRequest(WdfIoQueueGetDevice(Queue), Request);
}


