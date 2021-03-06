#include "vhci.h"

#include "usbip_proto.h"
#include "usbip_vhci_api.h"
#include "usbreq.h"

extern int
process_urb_req(PIRP irp, struct urb_req *urb_r);

#ifdef DBG

const char *
dbg_urb_req(struct urb_req *urb_r)
{
	static char	buf[128];

	if (urb_r == NULL)
		return "[null]";
	RtlStringCchPrintfA(buf, 128, "[%sseq:%d]", urb_r->sent ? "sent,": "", urb_r->seq_num);
	return buf;
}

#endif

void
build_setup_packet(usb_cspkt_t *csp, unsigned char direct_in, unsigned char type, unsigned char recip, unsigned char request)
{
	csp->bmRequestType.B = 0;
	csp->bmRequestType.Type = type;
	if (direct_in)
		csp->bmRequestType.Dir = BMREQUEST_DEVICE_TO_HOST;
	csp->bmRequestType.Recipient = recip;
	csp->bRequest = request;
}

struct urb_req *
find_urb_req(PPDO_DEVICE_DATA pdodata, struct usbip_header *hdr)
{
	struct urb_req	*urb_r = NULL;
	KIRQL		oldirql;
	PLIST_ENTRY	le;

	KeAcquireSpinLock(&pdodata->q_lock, &oldirql);
	for (le = pdodata->ioctl_q.Flink; le != &pdodata->ioctl_q; le = le->Flink) {
		urb_r = CONTAINING_RECORD(le, struct urb_req, list);
		if (urb_r->seq_num == hdr->base.seqnum) {
			if (IoSetCancelRoutine(urb_r->irp, NULL) == NULL) {
				/* already cancelled ? */
				urb_r = NULL;
			}
			else
				RemoveEntryList(le);
			break;
		}
	}
	KeReleaseSpinLock(&pdodata->q_lock, oldirql);

	return urb_r;
}

struct urb_req *
find_pending_urb_req(PPDO_DEVICE_DATA pdodata)
{
	PLIST_ENTRY	le;

	for (le = pdodata->ioctl_q.Flink; le != &pdodata->ioctl_q; le = le->Flink) {
		struct urb_req	*urb;

		urb = CONTAINING_RECORD(le, struct urb_req, list);
		if (!urb->sent) {
			urb->sent = TRUE;
			if (urb->seq_num != 0) {
				DBGE(DBG_GENERAL, "non-zero seq_num: %d\n", urb->seq_num);
			}
			urb->seq_num = ++(pdodata->seq_num);
			return urb;
		}
	}
	return NULL;
}

static void
remove_cancelled_urb_req(PPDO_DEVICE_DATA pdodata, PIRP irp)
{
	KIRQL	oldirql = irp->CancelIrql;
	PLIST_ENTRY	le;

	KeAcquireSpinLockAtDpcLevel(&pdodata->q_lock);

	for (le = pdodata->ioctl_q.Flink; le != &pdodata->ioctl_q; le = le->Flink) {
		struct urb_req	*urb_r;

		urb_r = CONTAINING_RECORD(le, struct urb_req, list);
		if (urb_r->irp == irp) {
			RemoveEntryList(le);
			KeReleaseSpinLock(&pdodata->q_lock, oldirql);

			DBGI(DBG_GENERAL, "urb cancelled: %s\n", dbg_urb_req(urb_r));
			ExFreeToNPagedLookasideList(&g_lookaside, urb_r);
			return;
		}
	}
	KeReleaseSpinLock(&pdodata->q_lock, oldirql);

	DBGW(DBG_GENERAL, "no matching urb req\n");
}

static void
cancel_urb_req(PDEVICE_OBJECT pdo, PIRP irp)
{
	PPDO_DEVICE_DATA	pdodata;

	pdodata = (PPDO_DEVICE_DATA)pdo->DeviceExtension;
	DBGI(DBG_GENERAL, "Irp will be cancelled: %p\n", irp);

	remove_cancelled_urb_req(pdodata, irp);

	irp->IoStatus.Status = STATUS_CANCELLED;
	IoCompleteRequest(irp, IO_NO_INCREMENT);
	IoReleaseCancelSpinLock(irp->CancelIrql);
}

static struct urb_req *
create_urb_req(PPDO_DEVICE_DATA pdodata, PIRP irp)
{
	struct urb_req	*urb_r;

	urb_r = ExAllocateFromNPagedLookasideList(&g_lookaside);
	if (urb_r == NULL) {
		DBGE(DBG_URB, "create_urb_req: out of memory\n");
		return NULL;
	}
	RtlZeroMemory(urb_r, sizeof(*urb_r));
	urb_r->pdodata = pdodata;
	urb_r->irp = irp;
	return urb_r;
}

static BOOLEAN
insert_urb_req(PPDO_DEVICE_DATA pdodata, struct urb_req *urb_r)
{
	PIRP	irp = urb_r->irp;

	IoSetCancelRoutine(irp, cancel_urb_req);
	if (irp->Cancel && IoSetCancelRoutine(irp, NULL)) {
		return FALSE;
	}
	else {
		IoMarkIrpPending(irp);
		InsertTailList(&pdodata->ioctl_q, &urb_r->list);
	}
	return TRUE;
}

NTSTATUS
submit_urb_req(PPDO_DEVICE_DATA pdodata, PIRP irp)
{
	struct urb_req	*urb_r;
	KIRQL oldirql;
	PIRP read_irp;
	NTSTATUS status = STATUS_PENDING;

	if ((urb_r = create_urb_req(pdodata, irp)) == NULL)
		return STATUS_INSUFFICIENT_RESOURCES;

	KeAcquireSpinLock(&pdodata->q_lock, &oldirql);
	read_irp = pdodata->pending_read_irp;
	pdodata->pending_read_irp = NULL;
	if (read_irp == NULL) {
		if (!insert_urb_req(pdodata, urb_r)) {
			KeReleaseSpinLock(&pdodata->q_lock, oldirql);
			ExFreeToNPagedLookasideList(&g_lookaside, urb_r);
			DBGI(DBG_URB, "submit_urb_req: urb cancelled\n");
			return STATUS_CANCELLED;
		}
		KeReleaseSpinLock(&pdodata->q_lock, oldirql);
		DBGI(DBG_URB, "submit_urb_req: urb pending\n");
		return STATUS_PENDING;
	}

	urb_r->seq_num = ++(pdodata->seq_num);
	KeReleaseSpinLock(&pdodata->q_lock, oldirql);

	read_irp->IoStatus.Status = process_urb_req(read_irp, urb_r);

	if (read_irp->IoStatus.Status == STATUS_SUCCESS) {
		KeAcquireSpinLock(&pdodata->q_lock, &oldirql);
		urb_r->sent = TRUE;
		status = insert_urb_req(pdodata, urb_r) ? STATUS_PENDING: STATUS_CANCELLED;
		KeReleaseSpinLock(&pdodata->q_lock, oldirql);
		if (status == STATUS_CANCELLED) {
			ExFreeToNPagedLookasideList(&g_lookaside, urb_r);
		}
	}
	else {
		ExFreeToNPagedLookasideList(&g_lookaside, urb_r);
		status = STATUS_INVALID_PARAMETER;
	}
	IoCompleteRequest(read_irp, IO_NO_INCREMENT);
	DBGI(DBG_URB, "submit_urb_req: urb requested: status:%s\n", dbg_ntstatus(status));
	return status;
}
