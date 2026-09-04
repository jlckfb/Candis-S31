# Hardware

This directory contains the public hardware reference for Candis-S31.

| Resource | Description | Status |
|---|---|---|
| [Schematic](schematic/SCH_Schematic_3_2026-08-10.pdf) | Fabrication baseline `v0.5_260803_1544`, exported 2026-08-10 | Preliminary |
| [Pinout](pinout/README.md) | GPIO groups derived from the revision 0.5 schematic | Preliminary |
| [Bring-up notes](bring-up.md) | Checks required before fabrication and first power-on | Open |
| [As-fabricated facts](facts.md) | EVT1 pad→net→function, TG28 rail, and connector tables from the PCB_3 netlist | Verified vs PCB netlist |

EVT1 revision 0.5 (`v0.5_260803_1544`) is now on the bench. The schematic and
pinout remain the design authority; recorded measurements and behavior are
listed separately in [`facts.md`](facts.md) and [`bring-up.md`](bring-up.md).
Pin names, active levels, power sequencing, and connector behavior still need
to be checked on each board revision.

Mechanical files, fabrication outputs, and a graphical pinout will be added when they are ready for public use. Empty placeholder directories are not kept in the repository.

Third-party IC datasheets are not copied here. Use the component manufacturer's current document unless a distributable file is required to reproduce a released design.
