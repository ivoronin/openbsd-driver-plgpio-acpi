# openbsd-driver-plgpio-acpi

An ACPI attachment for the OpenBSD plgpio(4) driver (ARM PrimeCell
PL061, ACPI HID `ARMH0061`). On arm64 EC2 Nitro instances the power
and sleep buttons arrive as GPIO-signaled ACPI events on this
controller; stock OpenBSD attaches plgpio(4) only via FDT, so the
button is never seen and an EC2 console Stop/Reboot degrades into the
4-minute hard-stop timeout. With this attachment the system shuts
down cleanly within seconds.

This repository holds only the driver source under `src/sys/`. Consume
it through
[openbsd-kernel-aws](https://github.com/ivoronin/openbsd-kernel-aws),
which pins this repository in its kernel build and maintains the
kernel-side glue; ready-to-run AMIs come from
[openbsd-cloudimg](https://github.com/ivoronin/openbsd-cloudimg).

This is heavily AI-assisted code and is not headed upstream (see the
[statement on tech@](https://marc.info/?l=openbsd-tech&m=177425035627562&w=2)).
