/*
 * qwzctl: talk to QwzWifi the way HeliPort does (kext/heliport.h), for
 * testing and for use without HeliPort.
 *
 *   qwzctl info             driver, firmware, interface
 *   qwzctl status           state and the current network
 *   qwzctl scan             networks seen (and start a background scan)
 *   qwzctl join SSID        join; asks for the passphrase (empty = open)
 *   qwzctl leave            forget the network, scan only
 *   qwzctl power on|off     radio on/off
 *
 * The passphrase is read from the terminal, never from the command line
 * (other users can see command lines in ps).
 *
 * Build: clang -Wall -O2 -framework IOKit -framework CoreFoundation \
 *            tools/qwzctl.c -o build/qwzctl
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <readpassphrase.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../kext/heliport.h"

static const char *states[] = { "INIT", "SCAN", "AUTH", "ASSOC", "RUN" };
static const char *phys[] = { "11a", "11b", "11g", "11n", "11ac", "11ax" };

/* As HeliPort's open_adapter(): the IOEthernetController with IOClass "itlwm". */
static io_connect_t
open_driver(void)
{
	io_iterator_t it;
	io_service_t s;
	io_connect_t con = 0;

	if (IOServiceGetMatchingServices(kIOMainPortDefault,
	    IOServiceMatching("IOEthernetController"), &it) != KERN_SUCCESS)
		return 0;
	while (con == 0 && (s = IOIteratorNext(it)) != 0) {
		CFTypeRef cls = IORegistryEntryCreateCFProperty(s,
		    CFSTR("IOClass"), kCFAllocatorDefault, 0);
		if (cls != NULL) {
			if (CFGetTypeID(cls) == CFStringGetTypeID() &&
			    CFStringCompare(cls, CFSTR("itlwm"), 0) ==
			    kCFCompareEqualTo &&
			    IOServiceOpen(s, mach_task_self(), 0, &con) !=
			    KERN_SUCCESS)
				con = 0;
			CFRelease(cls);
		}
		IOObjectRelease(s);
	}
	IOObjectRelease(it);
	return con;
}

static kern_return_t
get(io_connect_t con, uint32_t id, void *buf, size_t len)
{
	return IOConnectCallStructMethod(con, id, NULL, 0, buf, &len);
}

static kern_return_t
set(io_connect_t con, uint32_t id, void *buf, size_t len)
{
	return IOConnectCallStructMethod(con, id | HP_IOCTL_MASK, buf, len,
	    NULL, NULL);
}

static void
print_mac(const uint8_t *m)
{
	printf("%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4],
	    m[5]);
}

static const char *
security(const struct hp_network_info *n)
{
	/* net80211 values: PROTO_RSN 1, PROTO_WPA 2; AKM_8021X 1, AKM_PSK 2 */
	if (n->supported_rsnprotos == 0)
		return "open/WEP";
	if (n->rsn_akms & 1)
		return "802.1X";
	if (n->rsn_akms & (2 | 8))
		return (n->supported_rsnprotos & 1) ? "WPA2-PSK" : "WPA-PSK";
	return "WPA3/other";
}

static int
cmd_info(io_connect_t con)
{
	struct hp_driver_info di;

	if (get(con, HP_IOCTL_DRIVER_INFO, &di, sizeof(di)) != KERN_SUCCESS)
		return 1;
	printf("driver:    %.32s (API %u)\nfirmware:  %.32s\ninterface: %.32s\n",
	    di.driver_version, di.version, di.fw_version,
	    di.bsd_name[0] ? di.bsd_name : "(not published yet)");
	return 0;
}

static int
cmd_status(io_connect_t con)
{
	struct hp_state st;
	struct hp_power pw;
	struct hp_sta_info si;

	if (get(con, HP_IOCTL_POWER, &pw, sizeof(pw)) != KERN_SUCCESS ||
	    get(con, HP_IOCTL_STATE, &st, sizeof(st)) != KERN_SUCCESS)
		return 1;
	printf("radio:     %s\nstate:     %s\n", pw.enabled ? "on" : "off",
	    st.state >= 0 && st.state <= 4 ? states[st.state] : "?");
	if (get(con, HP_IOCTL_STA_INFO, &si, sizeof(si)) != KERN_SUCCESS)
		return 0;
	printf("network:   \"%.32s\" bssid ", si.ssid);
	print_mac(si.bssid);
	printf("\nchannel:   %u, %u MHz, %s, MCS %d, %u Mbit/s\n", si.channel,
	    si.band_width, si.op_mode >= 0 && si.op_mode <= 5 ?
	    phys[si.op_mode] : "?", si.cur_mcs, si.rate);
	printf("signal:    %d dBm, noise %d dBm\n", si.rssi, si.noise);
	return 0;
}

static int
cmd_scan(io_connect_t con)
{
	struct hp_version_only v = { HP_IOCTL_VERSION };
	struct hp_network_info n;
	int count = 0;

	while (get(con, HP_IOCTL_SCAN_RESULT, &n, sizeof(n)) == KERN_SUCCESS) {
		printf("%4d dBm  ch %3u  ", n.rssi, n.channel);
		print_mac(n.bssid);
		printf("  %-10s \"%.32s\"\n", security(&n), n.ssid);
		count++;
	}
	printf("%d networks\n", count);
	set(con, HP_IOCTL_SCAN, &v, sizeof(v));
	return 0;
}

static int
cmd_join(io_connect_t con, const char *ssid)
{
	struct hp_associate as;
	kern_return_t kr;

	if (strlen(ssid) == 0 || strlen(ssid) > HP_NWID_LEN) {
		fprintf(stderr, "SSID must be 1..32 bytes\n");
		return 1;
	}
	memset(&as, 0, sizeof(as));
	as.version = HP_IOCTL_VERSION;
	memcpy(as.nwid.nwid, ssid, strlen(ssid));
	as.nwid.len = (unsigned)strlen(ssid);
	if (readpassphrase("Passphrase (empty for an open network): ",
	    as.wpa_key.key, sizeof(as.wpa_key.key), RPP_REQUIRE_TTY) == NULL) {
		fprintf(stderr, "cannot read the passphrase\n");
		return 1;
	}
	as.wpa_key.len = (unsigned)strlen(as.wpa_key.key);
	printf("joining \"%s\"...\n", ssid);
	kr = set(con, HP_IOCTL_ASSOCIATE, &as, sizeof(as));
	memset_s(&as, sizeof(as), 0, sizeof(as));
	if (kr != KERN_SUCCESS) {
		fprintf(stderr, "join failed: 0x%x\n", kr);
		return 1;
	}
	for (int i = 0; i < 20; i++) {
		struct hp_state st;

		sleep(1);
		if (get(con, HP_IOCTL_STATE, &st, sizeof(st)) == KERN_SUCCESS &&
		    st.state == 4) {
			printf("associated after %d s\n", i + 1);
			return cmd_status(con);
		}
	}
	fprintf(stderr, "not associated after 20 s (wrong passphrase?)\n");
	return 1;
}

int
main(int argc, char **argv)
{
	io_connect_t con;
	int rc = 2;

	if (argc < 2)
		goto usage;
	if ((con = open_driver()) == 0) {
		fprintf(stderr, "QwzWifi (IOClass itlwm) not found or not "
		    "opened\n");
		return 1;
	}
	if (strcmp(argv[1], "mode") == 0 && argc == 3 && strcmp(argv[2], "heliport") == 0) {
		kern_return_t error = IOConnectCallMethod(con, QWZ_IOCTL_HELIPORT_MODE,
		    NULL, 0, NULL, 0, NULL, NULL, NULL, NULL);
		if (error == kIOReturnUnsupported)
			fprintf(stderr, "This loaded driver does not support safe parked handover; reboot into Ethernet mode.\n");
		else if (error) fprintf(stderr, "mode switch: 0x%x (administrator access required)\n", error);
		else {
			io_service_t service = IO_OBJECT_NULL;
			error = IOConnectGetService(con, &service);
			int ready = 0;
			for (unsigned poll = 0; !error && poll < 300; ++poll) {
				CFTypeRef value = IORegistryEntryCreateCFProperty(service,
				    CFSTR("QwzModeSwitch"), kCFAllocatorDefault, 0);
				if (value && CFGetTypeID(value) == CFStringGetTypeID()) {
					ready = CFEqual(value, CFSTR("HeliPort"));
					if (!ready && !CFEqual(value, CFSTR("requested"))) {
						char text[160];
						if (CFStringGetCString(value, text, sizeof(text), kCFStringEncodingUTF8))
							fprintf(stderr, "mode switch: %s\n", text);
						error = kIOReturnError;
					}
				}
				if (value) CFRelease(value);
				if (ready || error) break;
				usleep(200000);
			}
			if (service) IOObjectRelease(service);
			if (!ready && !error) { fprintf(stderr, "mode switch timed out\n"); error = kIOReturnTimeout; }
			if (ready) printf("HeliPort mode ready. Native mode returns after reboot.\n");
		}
		rc = error != KERN_SUCCESS;
	} else if (strcmp(argv[1], "info") == 0)
		rc = cmd_info(con);
	else if (strcmp(argv[1], "status") == 0)
		rc = cmd_status(con);
	else if (strcmp(argv[1], "scan") == 0)
		rc = cmd_scan(con);
	else if (strcmp(argv[1], "join") == 0 && argc == 3)
		rc = cmd_join(con, argv[2]);
	else if (strcmp(argv[1], "leave") == 0) {
		struct hp_disassociate d;

		memset(&d, 0, sizeof(d));
		d.version = HP_IOCTL_VERSION;
		rc = set(con, HP_IOCTL_DISASSOCIATE, &d, sizeof(d)) != KERN_SUCCESS;
	} else if (strcmp(argv[1], "power") == 0 && argc == 3) {
		struct hp_power p = { HP_IOCTL_VERSION,
		    strcmp(argv[2], "on") == 0, 0 };
		rc = set(con, HP_IOCTL_POWER, &p, sizeof(p)) != KERN_SUCCESS;
	}
	IOServiceClose(con);
	if (rc != 2)
		return rc;
usage:
	fprintf(stderr, "usage: qwzctl info | status | scan | join SSID | "
	    "leave | power on|off | mode heliport\n");
	return 2;
}
