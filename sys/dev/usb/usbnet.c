/*	$NetBSD: usbnet.c,v 1.2 2019/07/31 23:47:16 mrg Exp $	*/

/*
 * Copyright (c) 2019 Matthew R. Green
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Common code shared between USB ethernet drivers.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD: usbnet.c,v 1.2 2019/07/31 23:47:16 mrg Exp $");

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/kmem.h>
#include <sys/module.h>

#include <dev/usb/usbnet.h>

static int usbnet_modcmd(modcmd_t, void *);

#ifdef USB_DEBUG
#ifndef USBNET_DEBUG
#define usbnetdebug 0
#else
static int usbnetdebug = 20;

SYSCTL_SETUP(sysctl_hw_usbnet_setup, "sysctl hw.usbnet setup")
{
	int err;
	const struct sysctlnode *rnode;
	const struct sysctlnode *cnode;

	err = sysctl_createv(clog, 0, NULL, &rnode,
	    CTLFLAG_PERMANENT, CTLTYPE_NODE, "usbnet",
	    SYSCTL_DESCR("usbnet global controls"),
	    NULL, 0, NULL, 0, CTL_HW, CTL_CREATE, CTL_EOL);

	if (err)
		goto fail;

	/* control debugging printfs */
	err = sysctl_createv(clog, 0, &rnode, &cnode,
	    CTLFLAG_PERMANENT | CTLFLAG_READWRITE, CTLTYPE_INT,
	    "debug", SYSCTL_DESCR("Enable debugging output"),
	    NULL, 0, &usbnetdebug, sizeof(usbnetdebug), CTL_CREATE, CTL_EOL);
	if (err)
		goto fail;

	return;
fail:
	aprint_error("%s: sysctl_createv failed (err = %d)\n", __func__, err);
}

#endif /* USBNET_DEBUG */
#endif /* USB_DEBUG */

#define DPRINTF(FMT,A,B,C,D)	USBHIST_LOGN(usbnetdebug,1,FMT,A,B,C,D)
#define DPRINTFN(N,FMT,A,B,C,D)	USBHIST_LOGN(usbnetdebug,N,FMT,A,B,C,D)
#define USBNETHIST_FUNC()	USBHIST_FUNC()
#define USBNETHIST_CALLED(name)	USBHIST_CALLED(usbnetdebug)

/* Interrupt handling. */

static struct mbuf *
usbnet_newbuf(void)
{
	struct mbuf *m;

	MGETHDR(m, M_DONTWAIT, MT_DATA);
	if (m == NULL)
		return NULL;

	MCLGET(m, M_DONTWAIT);
	if (!(m->m_flags & M_EXT)) {
		m_freem(m);
		return NULL;
	}

	m->m_len = m->m_pkthdr.len = MCLBYTES;
	m_adj(m, ETHER_ALIGN);

	return m;
}

/*
 * usbnet_rxeof() is designed to be the done callback for rx completion.
 * it provides generic setup and finalisation, calls a different usbnet
 * rx_loop callback in the middle, which can use usbnet_enqueue() to
 * enqueue a packet for higher levels.
 */
void
usbnet_enqueue(struct usbnet * const un, uint8_t *buf, size_t buflen,
		int flags)
{
	struct ifnet *ifp = &un->un_ec.ec_if;
	struct mbuf *m;

	KASSERT(mutex_owned(&un->un_rxlock));

	m = usbnet_newbuf();
	if (m == NULL) {
		ifp->if_ierrors++;
		return;
	}

	m_set_rcvif(m, ifp);
	m->m_pkthdr.len = m->m_len = buflen;
	m->m_pkthdr.csum_flags = flags;
	memcpy(mtod(m, char *), buf, buflen);

	/* push the packet up */
	if_percpuq_enqueue(ifp->if_percpuq, m);
}

/*
 * A frame has been uploaded: pass the resulting mbuf chain up to
 * the higher level protocols.
 */
static void
usbnet_rxeof(struct usbd_xfer *xfer, void * priv, usbd_status status)
{
	struct usbnet_chain *c = (struct usbnet_chain *)priv;
	struct usbnet * const un = c->unc_un;
	struct ifnet *ifp = &un->un_ec.ec_if;
	uint32_t total_len;

	mutex_enter(&un->un_rxlock);

	if (un->un_dying || un->un_stopping ||
	    status == USBD_INVAL || status == USBD_NOT_STARTED ||
	    status == USBD_CANCELLED || !(ifp->if_flags & IFF_RUNNING))
		goto out;

	if (status != USBD_NORMAL_COMPLETION) {
		if (usbd_ratecheck(&un->un_rx_notice))
			aprint_error_dev(un->un_dev, "usb errors on rx: %s\n",
			    usbd_errstr(status));
		if (status == USBD_STALLED)
			usbd_clear_endpoint_stall_async(un->un_ep[USBNET_ENDPT_RX]);
		goto done;
	}

	usbd_get_xfer_status(xfer, NULL, NULL, &total_len, NULL);

	if (total_len > un->un_cdata.uncd_rx_bufsz) {
		aprint_error_dev(un->un_dev,
		    "rxeof: too large transfer (%u > %u)\n",
		    total_len, un->un_cdata.uncd_rx_bufsz);
		goto done;
	}

	(*un->un_rx_loop_cb)(un, xfer, c, total_len);
	KASSERT(mutex_owned(&un->un_rxlock));

done:
	if (un->un_dying || un->un_stopping)
		goto out;

	mutex_exit(&un->un_rxlock);

	/* Setup new transfer. */
	usbd_setup_xfer(xfer, c, c->unc_buf, un->un_cdata.uncd_rx_bufsz,
	    un->un_rx_xfer_flags, USBD_NO_TIMEOUT, usbnet_rxeof);
	usbd_transfer(xfer);
	return;

out:
	mutex_exit(&un->un_rxlock);
}

static void
usbnet_txeof(struct usbd_xfer *xfer, void * priv, usbd_status status)
{
	struct usbnet_chain *c = (struct usbnet_chain *)priv;
	struct usbnet * const un = c->unc_un;
	struct usbnet_cdata *cd = &un->un_cdata;
	struct ifnet * const ifp = usbnet_ifp(un);

	mutex_enter(&un->un_txlock);
	if (un->un_stopping || un->un_dying) {
		mutex_exit(&un->un_txlock);
		return;
	}

	KASSERT(cd->uncd_tx_cnt > 0);
	cd->uncd_tx_cnt--;

	un->un_timer = 0;

	switch (status) {
	case USBD_NOT_STARTED:
	case USBD_CANCELLED:
		break;

	case USBD_NORMAL_COMPLETION:
		ifp->if_opackets++;
		break;

	default:

		ifp->if_oerrors++;
		if (usbd_ratecheck(&un->un_tx_notice))
			aprint_error_dev(un->un_dev, "usb error on tx: %s\n",
			    usbd_errstr(status));
		if (status == USBD_STALLED)
			usbd_clear_endpoint_stall_async(un->un_ep[USBNET_ENDPT_TX]);
		break;
	}

	mutex_exit(&un->un_txlock);

	if (status == USBD_NORMAL_COMPLETION && !IFQ_IS_EMPTY(&ifp->if_snd))
		(*ifp->if_start)(ifp);
}

static void
usbnet_start_locked(struct ifnet *ifp)
{
	struct usbnet * const un = ifp->if_softc;
	struct usbnet_cdata *cd = &un->un_cdata;
	struct mbuf *m;
	unsigned length;
	int idx;

	KASSERT(mutex_owned(&un->un_txlock));
	KASSERT(cd->uncd_tx_cnt <= cd->uncd_tx_list_cnt);

	if (!un->un_link || (ifp->if_flags & IFF_RUNNING) == 0)
		return;

	idx = cd->uncd_tx_prod;
	while (cd->uncd_tx_cnt < cd->uncd_tx_list_cnt) {
		IFQ_POLL(&ifp->if_snd, m);
		if (m == NULL)
			break;

		struct usbnet_chain *c = &un->un_cdata.uncd_tx_chain[idx];

		length = (*un->un_tx_prepare_cb)(un, m, c);
		if (length == 0) {
			ifp->if_oerrors++;
			break;
		}

		if (__predict_false(c->unc_xfer == NULL)) {
			ifp->if_oerrors++;
			break;
		}

		usbd_setup_xfer(c->unc_xfer, c, c->unc_buf, length,
		    un->un_tx_xfer_flags, 10000, usbnet_txeof);

		/* Transmit */
		usbd_status err = usbd_transfer(c->unc_xfer);
		if (err != USBD_IN_PROGRESS) {
			ifp->if_oerrors++;
			break;
		}

		IFQ_DEQUEUE(&ifp->if_snd, m);

		/*
		 * If there's a BPF listener, bounce a copy of this frame
		 * to him.
		 */
		bpf_mtap(ifp, m, BPF_D_OUT);
		m_freem(m);

		idx = (idx + 1) % cd->uncd_tx_list_cnt;
		cd->uncd_tx_cnt++;
	}
	cd->uncd_tx_prod = idx;

	/*
	 * Set a timeout in case the chip goes out to lunch.
	 */
	un->un_timer = 5;
}

static void
usbnet_start(struct ifnet *ifp)
{
	struct usbnet * const un = ifp->if_softc;

	mutex_enter(&un->un_txlock);
	if (!un->un_stopping)
		usbnet_start_locked(ifp);
	mutex_exit(&un->un_txlock);
}

/*
 * Chain management.
 *
 * RX and TX are identical. Keep them that way.
 */

/* Start of common RX functions */

static size_t
usbnet_rx_list_size(struct usbnet_cdata *cd)
{
	return sizeof(*cd->uncd_rx_chain) * cd->uncd_rx_list_cnt;
}

static void
usbnet_rx_list_alloc(struct usbnet *un, unsigned cnt)
{
	struct usbnet_cdata *cd = &un->un_cdata;

	cd->uncd_rx_list_cnt = cnt;
	cd->uncd_rx_chain = kmem_zalloc(usbnet_rx_list_size(cd), KM_SLEEP);
}

static void
usbnet_rx_list_free(struct usbnet *un)
{
	struct usbnet_cdata *cd = &un->un_cdata;

	if (cd->uncd_rx_chain) {
		kmem_free(cd->uncd_rx_chain, usbnet_rx_list_size(cd));
		cd->uncd_rx_chain = NULL;
	}
}

static int
usbnet_rx_list_init(struct usbnet *un, unsigned xfer_flags)
{
	struct usbnet_cdata *cd = &un->un_cdata;

	for (size_t i = 0; i < cd->uncd_rx_list_cnt; i++) {
		struct usbnet_chain *c = &cd->uncd_rx_chain[i];

		c->unc_un = un;
		if (c->unc_xfer == NULL) {
			int err = usbd_create_xfer(un->un_ep[USBNET_ENDPT_RX],
			    cd->uncd_rx_bufsz, xfer_flags, 0, &c->unc_xfer);
			if (err)
				return err;
			c->unc_buf = usbd_get_buffer(c->unc_xfer);
		}
	}

	return 0;
}

static void
usbnet_rx_list_fini(struct usbnet *un)
{
	struct usbnet_cdata *cd = &un->un_cdata;

	for (size_t i = 0; i < cd->uncd_rx_list_cnt; i++) {
		struct usbnet_chain *c = &cd->uncd_rx_chain[i];

		if (c->unc_xfer != NULL) {
			usbd_destroy_xfer(c->unc_xfer);
			c->unc_xfer = NULL;
			c->unc_buf = NULL;
		}
	}
}

/* End of common RX functions */

static void
usbnet_rx_start_pipes(struct usbnet *un, usbd_callback cb)
{
	struct usbnet_cdata *cd = &un->un_cdata;

	mutex_enter(&un->un_rxlock);
	mutex_enter(&un->un_txlock);
	un->un_stopping = false;

	for (size_t i = 0; i < cd->uncd_rx_list_cnt; i++) {
		struct usbnet_chain *c = &cd->uncd_rx_chain[i];

		usbd_setup_xfer(c->unc_xfer, c, c->unc_buf, cd->uncd_rx_bufsz,
		    un->un_rx_xfer_flags, USBD_NO_TIMEOUT, cb);
		usbd_transfer(c->unc_xfer);
	}       

	mutex_exit(&un->un_txlock);
	mutex_exit(&un->un_rxlock);
}

/* Start of common TX functions */

static size_t
usbnet_tx_list_size(struct usbnet_cdata *cd)
{
	return sizeof(*cd->uncd_tx_chain) * cd->uncd_tx_list_cnt;
}

static void
usbnet_tx_list_alloc(struct usbnet *un, unsigned cnt)
{
	struct usbnet_cdata *cd = &un->un_cdata;

	cd->uncd_tx_list_cnt = cnt;
	cd->uncd_tx_chain = kmem_zalloc(usbnet_tx_list_size(cd), KM_SLEEP);
}

static void
usbnet_tx_list_free(struct usbnet *un)
{
	struct usbnet_cdata *cd = &un->un_cdata;

	if (cd->uncd_tx_chain) {
		kmem_free(cd->uncd_tx_chain, usbnet_tx_list_size(cd));
		cd->uncd_tx_chain = NULL;
	}
}

static int
usbnet_tx_list_init(struct usbnet *un, unsigned xfer_flags)
{
	struct usbnet_cdata *cd = &un->un_cdata;

	for (size_t i = 0; i < cd->uncd_tx_list_cnt; i++) {
		struct usbnet_chain *c = &cd->uncd_tx_chain[i];

		c->unc_un = un;
		if (c->unc_xfer == NULL) {
			int err = usbd_create_xfer(un->un_ep[USBNET_ENDPT_TX],
			    cd->uncd_tx_bufsz, xfer_flags, 0, &c->unc_xfer);
			if (err)
				return err;
			c->unc_buf = usbd_get_buffer(c->unc_xfer);
		}
	}

	return 0;
}

static void
usbnet_tx_list_fini(struct usbnet *un)
{
	struct usbnet_cdata *cd = &un->un_cdata;

	for (size_t i = 0; i < cd->uncd_tx_list_cnt; i++) {
		struct usbnet_chain *c = &cd->uncd_tx_chain[i];

		if (c->unc_xfer != NULL) {
			usbd_destroy_xfer(c->unc_xfer);
			c->unc_xfer = NULL;
			c->unc_buf = NULL;
		}
	}
}

/* End of common TX functions */

/* Endpoint pipe management. */

static void
usbnet_ep_close_pipes(struct usbnet *un)
{
	for (size_t i = 0; i < __arraycount(un->un_ep); i++) {
		if (un->un_ep[i] == NULL)
			continue;
		usbd_status err = usbd_close_pipe(un->un_ep[i]);
		if (err)
			aprint_error_dev(un->un_dev, "close pipe %zu: %s\n", i,
			    usbd_errstr(err));
		un->un_ep[i] = NULL;
	}
}

static usbd_status
usbnet_ep_open_pipes(struct usbnet *un)
{
	for (size_t i = 0; i < __arraycount(un->un_ep); i++) {
		if (un->un_ed[i] == 0)
			continue;
		usbd_status err = usbd_open_pipe(un->un_iface, un->un_ed[i],
		    USBD_EXCLUSIVE_USE | USBD_MPSAFE, &un->un_ep[i]);
		if (err) {
			usbnet_ep_close_pipes(un);
			return err;
		}
	}

	return USBD_NORMAL_COMPLETION;
}

static usbd_status
usbnet_ep_stop_pipes(struct usbnet *un)
{
	for (size_t i = 0; i < __arraycount(un->un_ep); i++) {
		if (un->un_ep[i] == NULL)
			continue;
		usbd_status err = usbd_abort_pipe(un->un_ep[i]);
		if (err)
			return err;
	}

	return USBD_NORMAL_COMPLETION;
}

int
usbnet_init_rx_tx(struct usbnet * const un, unsigned rxflags, unsigned txflags)
{
	struct ifnet * const ifp = usbnet_ifp(un);
	usbd_status err;

	/* Open RX and TX pipes. */
	err = usbnet_ep_open_pipes(un);
	if (err) {
		aprint_error_dev(un->un_dev, "open rx/tx pipes failed: %s\n",
		    usbd_errstr(err));
		return EIO;
	}

	/* Init RX ring. */
	if (usbnet_rx_list_init(un, rxflags)) {
		aprint_error_dev(un->un_dev, "rx list init failed\n");
		goto nobufs;
	}

	/* Init TX ring. */
	if (usbnet_tx_list_init(un, txflags)) {
		aprint_error_dev(un->un_dev, "tx list init failed\n");
		goto nobufs;
	}

	/* Start up the receive pipe(s). */
	usbnet_rx_start_pipes(un, usbnet_rxeof);

	/* Indicate we are up and running. */
	KASSERT(IFNET_LOCKED(ifp));
	ifp->if_flags |= IFF_RUNNING;

	callout_schedule(&un->un_stat_ch, hz);
	return 0;

nobufs:
	usbnet_rx_list_fini(un);
	usbnet_tx_list_fini(un);
	usbnet_ep_close_pipes(un);

	return ENOBUFS;
}

/* MII management. */

/*
 * Access functions for MII.  Take the MII lock to call access MII regs.
 * Two forms: usbnet (softc) lock currently held or not.
 */
void
usbnet_lock_mii(struct usbnet *un)
{

	mutex_enter(&un->un_lock);
	un->un_refcnt++;
	mutex_exit(&un->un_lock);

	mutex_enter(&un->un_miilock);
}

void
usbnet_lock_mii_un_locked(struct usbnet *un)
{
	KASSERT(mutex_owned(&un->un_lock));

	un->un_refcnt++;
	mutex_enter(&un->un_miilock);
}

void
usbnet_unlock_mii(struct usbnet *un)
{

	mutex_exit(&un->un_miilock);
	mutex_enter(&un->un_lock);
	if (--un->un_refcnt < 0)
		cv_broadcast(&un->un_detachcv);
	mutex_exit(&un->un_lock);
}

void
usbnet_unlock_mii_un_locked(struct usbnet *un)
{
	KASSERT(mutex_owned(&un->un_lock));

	mutex_exit(&un->un_miilock);
	if (--un->un_refcnt < 0)
		cv_broadcast(&un->un_detachcv);
}

int
usbnet_miibus_readreg(device_t dev, int phy, int reg, uint16_t *val)
{
	struct usbnet * const un = device_private(dev);
	usbd_status err;

	mutex_enter(&un->un_lock);
	if (un->un_dying || un->un_phyno != phy) {
		mutex_exit(&un->un_lock);
		return EIO;
	}
	mutex_exit(&un->un_lock);

	usbnet_lock_mii(un);
	err = (*un->un_read_reg_cb)(un, phy, reg, val);
	usbnet_unlock_mii(un);

	if (err) {
		aprint_error_dev(un->un_dev, "read PHY failed: %d\n", err);
		return EIO;
	}

	return 0;
}

int
usbnet_miibus_writereg(device_t dev, int phy, int reg, uint16_t val)
{
	struct usbnet * const un = device_private(dev);
	usbd_status err;

	mutex_enter(&un->un_lock);
	if (un->un_dying || un->un_phyno != phy) {
		mutex_exit(&un->un_lock);
		return EIO;
	}
	mutex_exit(&un->un_lock);

	usbnet_lock_mii(un);
	err = (*un->un_write_reg_cb)(un, phy, reg, val);
	usbnet_unlock_mii(un);

	if (err) {
		aprint_error_dev(un->un_dev, "write PHY failed: %d\n", err);
		return EIO;
	}

	return 0;
}

void
usbnet_miibus_statchg(struct ifnet *ifp)
{
	struct usbnet * const un = ifp->if_softc;

	(*un->un_statchg_cb)(ifp);
}

static int
usbnet_media_upd(struct ifnet *ifp)
{
	struct usbnet * const un = ifp->if_softc;
	struct mii_data * const mii = usbnet_mii(un);

	if (un->un_dying)
		return EIO;

	un->un_link = false;

	if (mii->mii_instance) {
		struct mii_softc *miisc;

		LIST_FOREACH(miisc, &mii->mii_phys, mii_list)
			mii_phy_reset(miisc);
	}

	return ether_mediachange(ifp);
}

/* ioctl */

static int
usbnet_ifflags_cb(struct ethercom *ec)
{
	struct ifnet *ifp = &ec->ec_if;
	struct usbnet *un = ifp->if_softc;
	int rv = 0;

	mutex_enter(&un->un_lock);

	const int changed = ifp->if_flags ^ un->un_if_flags;
	if ((changed & ~(IFF_CANTCHANGE | IFF_DEBUG)) == 0) {
		un->un_if_flags = ifp->if_flags;
		if ((changed & IFF_PROMISC) != 0)
			rv = ENETRESET;
	} else {
		rv = ENETRESET;
	}

	mutex_exit(&un->un_lock);

	return rv;
}

static int
usbnet_ioctl(struct ifnet *ifp, u_long cmd, void *data)
{
	struct usbnet * const un = ifp->if_softc;
	int error;

	error = ether_ioctl(ifp, cmd, data);
	if (error == ENETRESET && un->un_ioctl_cb)
		error = (*un->un_ioctl_cb)(ifp, cmd, data);

	return error;
}

/*
 * Generic stop network function:
 *	- mark as stopping
 *	- call DD routine to stop the device
 *	- turn off running, timer, statchg callout, link
 *	- stop transfers
 *	- free RX and TX resources
 *	- close pipes
 *
 * usbnet_stop() is exported for drivers to use, expects lock held.
 *
 * usbnet_stop_ifp() is for the if_stop handler.
 */
void
usbnet_stop(struct usbnet *un, struct ifnet *ifp, int disable)
{
	KASSERT(mutex_owned(&un->un_lock));

	mutex_enter(&un->un_rxlock);
	mutex_enter(&un->un_txlock);
	un->un_stopping = true;
	mutex_exit(&un->un_txlock);
	mutex_exit(&un->un_rxlock);

	if (un->un_stop_cb)
		(*un->un_stop_cb)(ifp, disable);

	/*
	 * XXXSMP Would like to
	 *	KASSERT(IFNET_LOCKED(ifp))
	 * here but the locking order is:
	 *	ifnet -> unlock -> rxlock -> txlock
	 * and unlock is already held.
	 */
	ifp->if_flags &= ~IFF_RUNNING;
	un->un_timer = 0;

	callout_stop(&un->un_stat_ch);
	un->un_link = false;

	/* Stop transfers. */
	usbnet_ep_stop_pipes(un);

	/* Free RX/TX resources. */
	usbnet_rx_list_fini(un);
	usbnet_tx_list_fini(un);

	/* Close pipes. */
	usbnet_ep_close_pipes(un);
}

static void
usbnet_stop_ifp(struct ifnet *ifp, int disable)
{
	struct usbnet * const un = ifp->if_softc;

	mutex_enter(&un->un_lock);
	usbnet_stop(un, ifp, disable);
	mutex_exit(&un->un_lock);
}

/*
 * Generic tick task function.
 *
 * usbnet_tick() is triggered from a callout, and triggers a call to
 * usbnet_tick_task() from the usb_task subsystem.
 */
static void
usbnet_tick(void *arg)
{
	struct usbnet * const un = arg;

	mutex_enter(&un->un_lock);
	if (!un->un_stopping && !un->un_dying) {
		/* Perform periodic stuff in process context */
		usb_add_task(un->un_udev, &un->un_ticktask, USB_TASKQ_DRIVER);
	}
	mutex_exit(&un->un_lock);
}

static void
usbnet_watchdog(struct ifnet *ifp)
{
	struct usbnet * const un = ifp->if_softc;
	struct usbnet_cdata *cd = &un->un_cdata;
	usbd_status stat;

	ifp->if_oerrors++;
	aprint_error_dev(un->un_dev, "watchdog timeout\n");

	if (cd->uncd_tx_cnt > 0) {
		/*
		 * XXX index 0
		 */
		struct usbnet_chain *c = &un->un_cdata.uncd_tx_chain[0];
		usbd_get_xfer_status(c->unc_xfer, NULL, NULL, NULL, &stat);
		usbnet_txeof(c->unc_xfer, c, stat);
	}

	if (!IFQ_IS_EMPTY(&ifp->if_snd))
		(*ifp->if_start)(ifp);
}

static void
usbnet_tick_task(void *arg)
{
	struct usbnet * const un = arg;

	mutex_enter(&un->un_lock);
	if (un->un_stopping || un->un_dying) {
		mutex_exit(&un->un_lock);
		return;
	}

	struct ifnet * const ifp = usbnet_ifp(un);
	struct mii_data * const mii = usbnet_mii(un);

	un->un_refcnt++;
	mutex_exit(&un->un_lock);

	if (ifp && un->un_timer != 0 && --un->un_timer == 0)
		usbnet_watchdog(ifp);

	if (mii && ifp) {
		mii_tick(mii);

		if (!un->un_link)
			(*mii->mii_statchg)(ifp);
	}

	mutex_enter(&un->un_lock);
	if (--un->un_refcnt < 0)
		cv_broadcast(&un->un_detachcv);
	if (!un->un_stopping && !un->un_dying)
		callout_schedule(&un->un_stat_ch, hz);
	mutex_exit(&un->un_lock);
}

static int
usbnet_init(struct ifnet *ifp)
{
	struct usbnet * const un = ifp->if_softc;

	return (*un->un_init_cb)(ifp);
}

/* Autoconf management. */

/*
 * usbnet_attach() and usbnet_attach_ifp() perform setup of the relevant
 * 'usbnet'.  The first is enough to enable device access (eg, endpoints
 * are connected and commands can be sent), and the second connects the
 * device to the system networking.
 *
 * Always call usbnet_detach(), even if usbnet_attach_ifp() is skippped.
 * Also usable as driver detach directly.
 */
void
usbnet_attach(struct usbnet *un,
	      const char *detname,	/* detach cv name */
	      unsigned rx_list_cnt,	/* size of rx chain list */
	      unsigned tx_list_cnt)	/* size of tx chain list */
{

	KASSERT(un->un_tx_prepare_cb);
	KASSERT(un->un_rx_loop_cb);
	KASSERT(un->un_init_cb);
	KASSERT(un->un_cdata.uncd_rx_bufsz);
	KASSERT(un->un_cdata.uncd_tx_bufsz);
	KASSERT(rx_list_cnt);
	KASSERT(tx_list_cnt);

	ether_set_ifflags_cb(&un->un_ec, usbnet_ifflags_cb);

	usb_init_task(&un->un_ticktask, usbnet_tick_task, un, USB_TASKQ_MPSAFE);
	callout_init(&un->un_stat_ch, CALLOUT_MPSAFE); 
	callout_setfunc(&un->un_stat_ch, usbnet_tick, un);

	mutex_init(&un->un_miilock, MUTEX_DEFAULT, IPL_NONE);
	mutex_init(&un->un_txlock, MUTEX_DEFAULT, IPL_SOFTUSB);
	mutex_init(&un->un_rxlock, MUTEX_DEFAULT, IPL_SOFTUSB);
	mutex_init(&un->un_lock, MUTEX_DEFAULT, IPL_NONE);
	cv_init(&un->un_detachcv, detname);

	rnd_attach_source(&un->un_rndsrc, device_xname(un->un_dev),
	    RND_TYPE_NET, RND_FLAG_DEFAULT);

	usbnet_rx_list_alloc(un, rx_list_cnt);
	usbnet_tx_list_alloc(un, tx_list_cnt);

	un->un_attached = true;
}

static void
usbnet_attach_mii(struct usbnet *un, int mii_flags)
{
	struct mii_data * const mii = &un->un_mii;
	struct ifnet *ifp = usbnet_ifp(un);

	mii->mii_ifp = ifp;
	mii->mii_readreg = usbnet_miibus_readreg;
	mii->mii_writereg = usbnet_miibus_writereg;
	mii->mii_statchg = usbnet_miibus_statchg;
	mii->mii_flags = MIIF_AUTOTSLEEP;

	un->un_ec.ec_mii = mii;
	ifmedia_init(&mii->mii_media, 0, usbnet_media_upd, ether_mediastatus);
	mii_attach(un->un_dev, mii, 0xffffffff, MII_PHY_ANY,
		   MII_OFFSET_ANY, mii_flags);

	if (LIST_FIRST(&mii->mii_phys) == NULL) {
		ifmedia_add(&mii->mii_media, IFM_ETHER | IFM_NONE, 0, NULL);
		ifmedia_set(&mii->mii_media, IFM_ETHER | IFM_NONE);
	} else
		ifmedia_set(&mii->mii_media, IFM_ETHER | IFM_AUTO);

	usbd_add_drv_event(USB_EVENT_DRIVER_ATTACH, un->un_udev, un->un_dev);

	if (!pmf_device_register(un->un_dev, NULL, NULL))
		aprint_error_dev(un->un_dev, "couldn't establish power handler\n");
}

void
usbnet_attach_ifp(struct usbnet *un,
		  bool have_mii,		/* setup MII */
		  unsigned if_flags,		/* additional if_flags */
		  unsigned if_extflags,		/* additional if_extflags */
		  int mii_flags)		/* additional mii_attach flags */
{
	struct ifnet *ifp = usbnet_ifp(un);

	KASSERT(un->un_attached);

	IFQ_SET_READY(&ifp->if_snd);

	ifp->if_softc = un;
	strlcpy(ifp->if_xname, device_xname(un->un_dev), IFNAMSIZ);
	ifp->if_flags = if_flags;
	ifp->if_extflags = IFEF_MPSAFE | if_extflags;
	ifp->if_ioctl = usbnet_ioctl;
	ifp->if_start = usbnet_start;
	ifp->if_init = usbnet_init;
	ifp->if_stop = usbnet_stop_ifp;

	IFQ_SET_READY(&ifp->if_snd);

	if (have_mii)
		usbnet_attach_mii(un, mii_flags);

	/* Attach the interface. */
	if_attach(ifp);
	ether_ifattach(ifp, un->un_eaddr);
}

int
usbnet_detach(device_t self, int flags)
{
	struct usbnet * const un = device_private(self);
	struct ifnet *ifp = usbnet_ifp(un);
	struct mii_data *mii = usbnet_mii(un);

	mutex_enter(&un->un_lock);
	un->un_dying = true;
	mutex_exit(&un->un_lock);

	/* Detached before attached finished, so just bail out. */
	if (!un->un_attached)
		return 0;

	callout_halt(&un->un_stat_ch, NULL);
	usb_rem_task_wait(un->un_udev, &un->un_ticktask, USB_TASKQ_DRIVER, NULL);

	if (ifp->if_flags & IFF_RUNNING) {
		IFNET_LOCK(ifp);
		usbnet_stop_ifp(ifp, 1);
		IFNET_UNLOCK(ifp);
	}

	mutex_enter(&un->un_lock);
	un->un_refcnt--;
	while (un->un_refcnt > 0) {
		/* Wait for processes to go away */
		cv_wait(&un->un_detachcv, &un->un_lock);
	}
	mutex_exit(&un->un_lock);

	usbnet_rx_list_free(un);
	usbnet_tx_list_free(un);

	callout_destroy(&un->un_stat_ch);
	rnd_detach_source(&un->un_rndsrc);

	if (mii) {
		mii_detach(mii, MII_PHY_ANY, MII_OFFSET_ANY);
		ifmedia_delete_instance(&mii->mii_media, IFM_INST_ANY);
	}
	if (ifp->if_softc) {
		ether_ifdetach(ifp);
		if_detach(ifp);
	}

	cv_destroy(&un->un_detachcv);
	mutex_destroy(&un->un_lock);
	mutex_destroy(&un->un_rxlock);
	mutex_destroy(&un->un_txlock);
	mutex_destroy(&un->un_miilock);

	pmf_device_deregister(un->un_dev);

	usbd_add_drv_event(USB_EVENT_DRIVER_DETACH, un->un_udev, un->un_dev);

	return 0;
}

int
usbnet_activate(device_t self, devact_t act)
{
	struct usbnet * const un = device_private(self);
	struct ifnet * const ifp = usbnet_ifp(un);

	switch (act) {
	case DVACT_DEACTIVATE:
		if_deactivate(ifp);

		mutex_enter(&un->un_lock);
		un->un_dying = true;
		mutex_exit(&un->un_lock);

		mutex_enter(&un->un_rxlock);
		mutex_enter(&un->un_txlock);
		un->un_stopping = true;
		mutex_exit(&un->un_txlock);
		mutex_exit(&un->un_rxlock);

		return 0;
	default:
		return EOPNOTSUPP;
	}
}

MODULE(MODULE_CLASS_MISC, usbnet, NULL);

static int
usbnet_modcmd(modcmd_t cmd, void *arg)
{
	switch (cmd) {
	case MODULE_CMD_INIT:
	case MODULE_CMD_FINI:
		return 0;
	case MODULE_CMD_STAT:
	case MODULE_CMD_AUTOUNLOAD:
	default:
		return ENOTTY;
	}
}
