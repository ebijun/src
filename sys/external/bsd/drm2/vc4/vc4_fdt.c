/*-
 * Copyright (c) 2026 Jun Ebihara <jun@NetBSD.org>
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
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/device.h>
#include <sys/systm.h>

#include <dev/fdt/fdtvar.h>
#include <dev/ofw/openfirm.h>

struct vc4_fdt_softc {
	device_t		sc_dev;
	int			sc_phandle;
	bus_space_tag_t		sc_bst;
	bus_space_handle_t	sc_bsh;
};

static int	vc4_fdt_match(device_t, cfdata_t, void *);
static void	vc4_fdt_attach(device_t, device_t, void *);

CFATTACH_DECL_NEW(vc4_fdt, sizeof(struct vc4_fdt_softc),
    vc4_fdt_match, vc4_fdt_attach, NULL, NULL);

static const struct device_compatible_entry compat_data[] = {
	{ .compat = "brcm,bcm2835-vc4" },
	{ .compat = "brcm,bcm2837-vc4" },
	{ .compat = "brcm,bcm2711-vc4" },
	DEVICE_COMPAT_EOL
};

static int
vc4_fdt_match(device_t parent, cfdata_t match, void *aux)
{
	struct fdt_attach_args * const faa = aux;

	return of_compatible_match(faa->faa_phandle, compat_data);
}

static void
vc4_fdt_attach(device_t parent, device_t self, void *aux)
{
	struct vc4_fdt_softc * const sc = device_private(self);
	struct fdt_attach_args * const faa = aux;
	const int phandle = faa->faa_phandle;

	sc->sc_dev = self;
	sc->sc_phandle = phandle;
	sc->sc_bst = faa->faa_bst;

	aprint_naive("\n");
	aprint_normal(": Broadcom VC4 DRMKMS\n");

	/* TODO: DRM2 device initialization and FDT binding logic */
}

