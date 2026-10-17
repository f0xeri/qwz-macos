/*
 * QwzWifi compat: OpenBSD pci(9), real signatures.
 * pci_chipset_tag_t carries the IOPCIDevice (as void *); pcitag_t is unused.
 * Only single-MSI mode is offered: pci_intr_enable_msivec() fails, which
 * makes qwz pick its one-MSI configuration.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef _QWZMAC_PCIVAR_H_
#define _QWZMAC_PCIVAR_H_

#include <sys/types.h>
#include <machine/bus.h>
#include <dev/pci/pcireg.h>

typedef void *pci_chipset_tag_t;	/* QWZMAC: IOPCIDevice * */
typedef u_long pcitag_t;
typedef uint32_t pcireg_t;

typedef struct {
	pcitag_t	tag;
	int		line;
	int		pin;
	int		index;		/* QWZMAC: IOKit interrupt index */
	int		msi;
} pci_intr_handle_t;

struct pci_attach_args {
	bus_space_tag_t pa_iot;
	bus_space_tag_t pa_memt;
	bus_dma_tag_t	pa_dmat;
	bus_dma_tag_t	pa_dmat64;
	pci_chipset_tag_t pa_pc;
	int		pa_bus;
	int		pa_device;
	int		pa_function;
	pcitag_t	pa_tag;
	pcireg_t	pa_id;
	pcireg_t	pa_class;
	pcireg_t	pa_subsys;
	int		pa_intrpin;
	int		pa_intrline;
	int		pa_flags;
};

struct pci_matchid {
	uint16_t	pm_vid;
	uint16_t	pm_pid;
};

pcireg_t pci_conf_read(pci_chipset_tag_t, pcitag_t, int);
void	pci_conf_write(pci_chipset_tag_t, pcitag_t, int, pcireg_t);
int	pci_get_capability(pci_chipset_tag_t, pcitag_t, int, int *, pcireg_t *);
int	pci_matchbyid(struct pci_attach_args *, const struct pci_matchid *, int);
pcireg_t pci_mapreg_type(pci_chipset_tag_t, pcitag_t, int);
int	pci_mapreg_map(struct pci_attach_args *, int, pcireg_t, int,
	    bus_space_tag_t *, bus_space_handle_t *, bus_addr_t *,
	    bus_size_t *, bus_size_t);
int	pci_set_powerstate(pci_chipset_tag_t, pcitag_t, int);
int	pci_get_powerstate(pci_chipset_tag_t, pcitag_t);

int	pci_intr_map(struct pci_attach_args *, pci_intr_handle_t *);
int	pci_intr_map_msi(struct pci_attach_args *, pci_intr_handle_t *);
int	pci_intr_map_msix(struct pci_attach_args *, int, pci_intr_handle_t *);
int	pci_intr_map_msivec(struct pci_attach_args *, int, pci_intr_handle_t *);
int	pci_intr_enable_msivec(struct pci_attach_args *, int);
const char *pci_intr_string(pci_chipset_tag_t, pci_intr_handle_t);
void	*pci_intr_establish(pci_chipset_tag_t, pci_intr_handle_t, int,
	    int (*)(void *), void *, const char *);
void	pci_intr_disestablish(pci_chipset_tag_t, void *);

#endif /* _QWZMAC_PCIVAR_H_ */
