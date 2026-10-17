/*
 * QwzWifi compat: pci(9) on top of IOPCIDevice (via qwzmac/os.h).
 * Only memory BARs and a single MSI vector are supported; legacy INTx and
 * multi-vector MSI requests fail, so qwz uses its one-MSI configuration.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <machine/bus.h>
#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>

pcireg_t
pci_conf_read(pci_chipset_tag_t pc, pcitag_t tag, int reg)
{
	(void)tag;
	return qwzmac_pci_conf_read(pc, reg);
}

void
pci_conf_write(pci_chipset_tag_t pc, pcitag_t tag, int reg, pcireg_t val)
{
	(void)tag;
	qwzmac_pci_conf_write(pc, reg, val);
}

int
pci_get_capability(pci_chipset_tag_t pc, pcitag_t tag, int capid,
    int *offset, pcireg_t *value)
{
	pcireg_t reg;
	unsigned int ofs;
	int guard = 48;

	reg = pci_conf_read(pc, tag, PCI_COMMAND_STATUS_REG);
	if (!(reg & PCI_STATUS_CAPLIST_SUPPORT))
		return 0;

	ofs = PCI_CAPLIST_PTR(pci_conf_read(pc, tag, PCI_CAPLISTPTR_REG));
	while (ofs != 0 && guard-- > 0) {
		if ((ofs & 3) || ofs < 0x40)
			return 0;
		reg = pci_conf_read(pc, tag, ofs);
		if (PCI_CAPLIST_CAP(reg) == capid) {
			if (offset)
				*offset = ofs;
			if (value)
				*value = reg;
			return 1;
		}
		ofs = PCI_CAPLIST_NEXT(reg);
	}
	return 0;
}

int
pci_matchbyid(struct pci_attach_args *pa, const struct pci_matchid *ids,
    int nent)
{
	int i;

	for (i = 0; i < nent; i++) {
		if (PCI_VENDOR(pa->pa_id) == ids[i].pm_vid &&
		    PCI_PRODUCT(pa->pa_id) == ids[i].pm_pid)
			return 1;
	}
	return 0;
}

pcireg_t
pci_mapreg_type(pci_chipset_tag_t pc, pcitag_t tag, int reg)
{
	pcireg_t rv;

	rv = pci_conf_read(pc, tag, reg);
	if (PCI_MAPREG_TYPE(rv) == PCI_MAPREG_TYPE_MEM)
		return rv & (PCI_MAPREG_TYPE_MASK | PCI_MAPREG_MEM_TYPE_MASK);
	return PCI_MAPREG_TYPE_IO;
}

int
pci_mapreg_map(struct pci_attach_args *pa, int reg, pcireg_t type, int flags,
    bus_space_tag_t *tagp, bus_space_handle_t *handlep, bus_addr_t *basep,
    bus_size_t *sizep, bus_size_t maxsize)
{
	uintptr_t va;
	uint64_t pa_base, size;
	int error;

	(void)flags;
	if (PCI_MAPREG_TYPE(type) != PCI_MAPREG_TYPE_MEM) {
		printf("QwzWifi: pci_mapreg_map: I/O BARs unsupported\n");
		return EINVAL;
	}
	error = qwzmac_pci_map_bar(pa->pa_pc, reg, &va, &pa_base, &size);
	if (error)
		return error;
	if (maxsize != 0 && size > maxsize)
		size = maxsize;
	if (tagp)
		*tagp = 0;
	*handlep = va;
	if (basep)
		*basep = pa_base;
	if (sizep)
		*sizep = size;
	return 0;
}

int
pci_get_powerstate(pci_chipset_tag_t pc, pcitag_t tag)
{
	pcireg_t reg;
	int off;

	if (!pci_get_capability(pc, tag, PCI_CAP_PWRMGMT, &off, NULL))
		return PCI_PMCSR_STATE_D0;
	reg = pci_conf_read(pc, tag, off + PCI_PMCSR);
	return reg & PCI_PMCSR_STATE_MASK;
}

int
pci_set_powerstate(pci_chipset_tag_t pc, pcitag_t tag, int state)
{
	pcireg_t reg;
	int off, cur;

	if (!pci_get_capability(pc, tag, PCI_CAP_PWRMGMT, &off, NULL))
		return EOPNOTSUPP;
	reg = pci_conf_read(pc, tag, off + PCI_PMCSR);
	cur = reg & PCI_PMCSR_STATE_MASK;
	if (cur == state)
		return 0;
	/* QWZMAC: qwz only asks for D0; leave other transitions to IOKit. */
	if (state != PCI_PMCSR_STATE_D0) {
		printf("QwzWifi: pci_set_powerstate D%d ignored\n", state);
		return EOPNOTSUPP;
	}
	printf("QwzWifi: device was in D%d, setting D0\n", cur);
	pci_conf_write(pc, tag, off + PCI_PMCSR,
	    (reg & ~PCI_PMCSR_STATE_MASK) | state);
	delay(10000);	/* D3hot -> D0 needs 10 ms */
	return 0;
}

int
pci_intr_map(struct pci_attach_args *pa, pci_intr_handle_t *ihp)
{
	(void)pa; (void)ihp;
	return ENODEV;	/* no INTx */
}

int
pci_intr_map_msi(struct pci_attach_args *pa, pci_intr_handle_t *ihp)
{
	int index;

	index = qwzmac_pci_msi_index(pa->pa_pc);
	if (index < 0)
		return ENODEV;
	memset(ihp, 0, sizeof(*ihp));
	ihp->index = index;
	ihp->msi = 1;
	return 0;
}

int
pci_intr_map_msix(struct pci_attach_args *pa, int vec, pci_intr_handle_t *ihp)
{
	(void)pa; (void)vec; (void)ihp;
	return ENODEV;
}

int
pci_intr_map_msivec(struct pci_attach_args *pa, int vec, pci_intr_handle_t *ihp)
{
	if (vec != 0)
		return ENODEV;
	return pci_intr_map_msi(pa, ihp);
}

int
pci_intr_enable_msivec(struct pci_attach_args *pa, int num_vec)
{
	(void)pa; (void)num_vec;
	return ENODEV;	/* QWZMAC: one MSI vector only */
}

const char *
pci_intr_string(pci_chipset_tag_t pc, pci_intr_handle_t ih)
{
	(void)pc;
	return ih.msi ? "msi" : "intx";
}

void *
pci_intr_establish(pci_chipset_tag_t pc, pci_intr_handle_t ih, int level,
    int (*func)(void *), void *arg, const char *what)
{
	(void)level;
	if (!ih.msi)
		return NULL;
	return qwzmac_pci_intr_establish(pc, ih.index, func, arg, what);
}

void
pci_intr_disestablish(pci_chipset_tag_t pc, void *cookie)
{
	if (cookie != NULL)
		qwzmac_pci_intr_disestablish(pc, cookie);
}
