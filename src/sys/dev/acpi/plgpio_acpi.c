/*	$OpenBSD$ */

/*
 * Copyright (c) 2026 Ilya Voronin <ivoronin@octalwave.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * ACPI attachment for the ARM PrimeCell PL061 GPIO controller (ARMH0061).
 *
 * On platforms like AWS EC2 Nitro (arm64) the power and sleep button
 * events are delivered as GPIO-signaled ACPI events: the DSDT declares
 * _AEI pins on the PL061 and _Exx methods that notify PWRB/SLPB.
 * Registering the pins with acpi_register_gpio() is enough for the
 * existing acpibtn(4) machinery to pick the events up.
 */

#include <sys/param.h>
#include <sys/systm.h>

#include <dev/acpi/acpireg.h>
#include <dev/acpi/acpivar.h>
#include <dev/acpi/acpidev.h>
#include <dev/acpi/amltypes.h>
#include <dev/acpi/dsdt.h>

/* Registers. */
#define GPIODATA(pin)		((1 << (pin)) << 2)
#define GPIODIR			0x400
#define GPIOIS			0x404
#define GPIOIBE			0x408
#define GPIOIEV			0x40c
#define GPIOIE			0x410
#define GPIORIS			0x414
#define GPIOMIS			0x418
#define GPIOIC			0x41c
#define GPIOAFSEL		0x420

#define PLGPIO_NPINS		8

#define HREAD1(sc, reg)							\
	(bus_space_read_1((sc)->sc_iot, (sc)->sc_ioh, (reg)))
#define HWRITE1(sc, reg, val)						\
	bus_space_write_1((sc)->sc_iot, (sc)->sc_ioh, (reg), (val))
#define HSET1(sc, reg, bits)						\
	HWRITE1((sc), (reg), HREAD1((sc), (reg)) | (bits))
#define HCLR1(sc, reg, bits)						\
	HWRITE1((sc), (reg), HREAD1((sc), (reg)) & ~(bits))

struct plgpio_acpi_intrhand {
	int (*ih_func)(void *);
	void *ih_arg;
	int ih_ipl;
};

struct plgpio_acpi_softc {
	struct device		sc_dev;
	struct acpi_softc	*sc_acpi;
	struct aml_node		*sc_node;

	bus_space_tag_t		sc_iot;
	bus_space_handle_t	sc_ioh;

	void			*sc_ih;
	struct plgpio_acpi_intrhand sc_pin_ih[PLGPIO_NPINS];

	struct acpi_gpio	sc_gpio;
};

int	plgpio_acpi_match(struct device *, void *, void *);
void	plgpio_acpi_attach(struct device *, struct device *, void *);

const struct cfattach plgpio_acpi_ca = {
	sizeof(struct plgpio_acpi_softc), plgpio_acpi_match, plgpio_acpi_attach
};

const char *plgpio_acpi_hids[] = {
	"ARMH0061",
	NULL
};

int	plgpio_acpi_read_pin(void *, int);
void	plgpio_acpi_write_pin(void *, int, int);
void	plgpio_acpi_intr_establish(void *, int, int, int,
	    int (*)(void *), void *);
void	plgpio_acpi_intr_enable(void *, int);
void	plgpio_acpi_intr_disable(void *, int);
int	plgpio_acpi_intr(void *);

int
plgpio_acpi_match(struct device *parent, void *match, void *aux)
{
	struct acpi_attach_args *aaa = aux;
	struct cfdata *cf = match;

	if (aaa->aaa_naddr < 1 || aaa->aaa_nirq < 1)
		return 0;
	return acpi_matchhids(aaa, plgpio_acpi_hids, cf->cf_driver->cd_name);
}

void
plgpio_acpi_attach(struct device *parent, struct device *self, void *aux)
{
	struct acpi_attach_args *aaa = aux;
	struct plgpio_acpi_softc *sc = (struct plgpio_acpi_softc *)self;

	sc->sc_acpi = (struct acpi_softc *)parent;
	sc->sc_node = aaa->aaa_node;
	printf(" %s", sc->sc_node->name);

	printf(" addr 0x%llx/0x%llx", aaa->aaa_addr[0], aaa->aaa_size[0]);

	sc->sc_iot = aaa->aaa_bst[0];
	if (bus_space_map(sc->sc_iot, aaa->aaa_addr[0], aaa->aaa_size[0],
	    0, &sc->sc_ioh)) {
		printf(": can't map registers\n");
		return;
	}

	/* Mask and clear all pin interrupts. */
	HWRITE1(sc, GPIOIE, 0);
	HWRITE1(sc, GPIOIC, 0xff);

	printf(" irq %d", aaa->aaa_irq[0]);

	sc->sc_ih = acpi_intr_establish(aaa->aaa_irq[0],
	    aaa->aaa_irq_flags[0], IPL_BIO, plgpio_acpi_intr,
	    sc, sc->sc_dev.dv_xname);
	if (sc->sc_ih == NULL) {
		printf(": can't establish interrupt\n");
		bus_space_unmap(sc->sc_iot, sc->sc_ioh, aaa->aaa_size[0]);
		return;
	}

	sc->sc_gpio.cookie = sc;
	sc->sc_gpio.read_pin = plgpio_acpi_read_pin;
	sc->sc_gpio.write_pin = plgpio_acpi_write_pin;
	sc->sc_gpio.intr_establish = plgpio_acpi_intr_establish;
	sc->sc_gpio.intr_enable = plgpio_acpi_intr_enable;
	sc->sc_gpio.intr_disable = plgpio_acpi_intr_disable;
	sc->sc_node->gpio = &sc->sc_gpio;

	printf("\n");

	acpi_register_gpio(sc->sc_acpi, sc->sc_node);
}

int
plgpio_acpi_read_pin(void *cookie, int pin)
{
	struct plgpio_acpi_softc *sc = cookie;

	if (pin < 0 || pin >= PLGPIO_NPINS)
		return 0;

	return !!HREAD1(sc, GPIODATA(pin));
}

void
plgpio_acpi_write_pin(void *cookie, int pin, int val)
{
	struct plgpio_acpi_softc *sc = cookie;

	if (pin < 0 || pin >= PLGPIO_NPINS)
		return;

	HWRITE1(sc, GPIODATA(pin), val ? (1 << pin) : 0);
}

void
plgpio_acpi_intr_establish(void *cookie, int pin, int flags, int level,
    int (*func)(void *), void *arg)
{
	struct plgpio_acpi_softc *sc = cookie;
	int s;

	if (pin < 0 || pin >= PLGPIO_NPINS)
		return;

	switch (flags & (LR_GPIO_MODE | LR_GPIO_POLARITY)) {
	case LR_GPIO_LEVEL | LR_GPIO_ACTLO:
	case LR_GPIO_LEVEL | LR_GPIO_ACTHI:
	case LR_GPIO_EDGE | LR_GPIO_ACTLO:
	case LR_GPIO_EDGE | LR_GPIO_ACTHI:
	case LR_GPIO_EDGE | LR_GPIO_ACTBOTH:
		break;
	default:
		printf("%s: unsupported interrupt mode/polarity\n",
		    sc->sc_dev.dv_xname);
		return;
	}

	sc->sc_pin_ih[pin].ih_func = func;
	sc->sc_pin_ih[pin].ih_arg = arg;
	sc->sc_pin_ih[pin].ih_ipl = level & ~IPL_WAKEUP;

	s = splbio();
	HCLR1(sc, GPIOAFSEL, 1 << pin);
	HCLR1(sc, GPIODIR, 1 << pin);

	if ((flags & LR_GPIO_MODE) == LR_GPIO_LEVEL)
		HSET1(sc, GPIOIS, 1 << pin);
	else
		HCLR1(sc, GPIOIS, 1 << pin);

	if ((flags & LR_GPIO_POLARITY) == LR_GPIO_ACTBOTH)
		HSET1(sc, GPIOIBE, 1 << pin);
	else
		HCLR1(sc, GPIOIBE, 1 << pin);

	if ((flags & LR_GPIO_POLARITY) == LR_GPIO_ACTHI)
		HSET1(sc, GPIOIEV, 1 << pin);
	else
		HCLR1(sc, GPIOIEV, 1 << pin);

	HWRITE1(sc, GPIOIC, 1 << pin);
	HSET1(sc, GPIOIE, 1 << pin);
	splx(s);

	if (level & IPL_WAKEUP)
		intr_set_wakeup(sc->sc_ih);
}

void
plgpio_acpi_intr_enable(void *cookie, int pin)
{
	struct plgpio_acpi_softc *sc = cookie;
	int s;

	if (pin < 0 || pin >= PLGPIO_NPINS)
		return;

	s = splbio();
	HSET1(sc, GPIOIE, 1 << pin);
	splx(s);
}

void
plgpio_acpi_intr_disable(void *cookie, int pin)
{
	struct plgpio_acpi_softc *sc = cookie;
	int s;

	if (pin < 0 || pin >= PLGPIO_NPINS)
		return;

	s = splbio();
	HCLR1(sc, GPIOIE, 1 << pin);
	splx(s);
}

int
plgpio_acpi_intr(void *arg)
{
	struct plgpio_acpi_softc *sc = arg;
	uint32_t status;
	int pin, s;

	status = HREAD1(sc, GPIOMIS);
	if (status == 0)
		return 0;

	/* Clear edge interrupts before dispatch, level ones are gated. */
	HWRITE1(sc, GPIOIC, status);

	for (pin = 0; pin < PLGPIO_NPINS; pin++) {
		if ((status & (1 << pin)) == 0)
			continue;
		if (sc->sc_pin_ih[pin].ih_func == NULL)
			continue;
		s = splraise(sc->sc_pin_ih[pin].ih_ipl);
		sc->sc_pin_ih[pin].ih_func(sc->sc_pin_ih[pin].ih_arg);
		splx(s);
	}

	return 1;
}
