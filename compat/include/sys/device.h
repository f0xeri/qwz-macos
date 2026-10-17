/*
 * QwzWifi compat: OpenBSD <sys/device.h> (autoconf subset).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef _QWZMAC_SYS_DEVICE_H_
#define _QWZMAC_SYS_DEVICE_H_

#include <sys/types.h>
#include <sys/queue.h>

enum devclass {
	DV_DULL,
	DV_CPU,
	DV_DISK,
	DV_IFNET,
	DV_TAPE,
	DV_TTY
};

#define DVACT_DEACTIVATE	1
#define DVACT_QUIESCE		2
#define DVACT_SUSPEND		3
#define DVACT_RESUME		4
#define DVACT_WAKEUP		5
#define DVACT_POWERDOWN		6

struct device {
	enum devclass	dv_class;
	TAILQ_ENTRY(device) dv_list;
	struct cfdata	*dv_cfdata;
	int		dv_unit;
	char		dv_xname[16];
	struct device	*dv_parent;
	int		dv_flags;
	int		dv_ref;
};

#define DVF_ACTIVE	0x0001

struct cfattach {
	size_t	ca_devsize;
	int	(*ca_match)(struct device *, void *, void *);
	void	(*ca_attach)(struct device *, struct device *, void *);
	int	(*ca_detach)(struct device *, int);
	int	(*ca_activate)(struct device *, int);
};

struct cfdriver {
	void	**cd_devs;
	char	*cd_name;
	enum devclass cd_class;
	int	cd_mode;
	int	cd_ndevs;
};

/* Firmware is embedded in the kext; *buf is malloc(9)ed (M_DEVBUF). */
int	loadfirmware(const char *, u_char **, size_t *);

/* Runs the hook in its own kernel thread (there is no mountroot phase). */
void	config_mountroot(struct device *, void (*)(struct device *));

#endif /* _QWZMAC_SYS_DEVICE_H_ */
