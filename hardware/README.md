# Hardware

This directory contains the public hardware reference for Candis-S31.

| Resource | Description | Status |
|---|---|---|
| [Schematic](schematic/SCH_Schematic_3_2026-08-10.pdf) | Fabrication baseline `v0.5_260803_1544`, exported 2026-08-10 | Preliminary |
| [Pinout](pinout/README.md) | GPIO groups derived from the revision 0.5 schematic | Preliminary |
| [Bring-up notes](bring-up.md) | Checks required before fabrication and first power-on | Open |
| [As-fabricated facts](facts.md) | EVT1 pad→net→function, TG28 rail, and connector tables from the PCB_3 netlist | Verified vs PCB netlist |

EVT1 revision 0.5 (`v0.5_260803_1544`) went to fabrication on 2026-08-03 but the boards have not arrived. Treat the schematic and pinout as design inputs, not as measurements from a working board. Pin names, active levels, power sequencing, and connector behavior must be checked on every received board.
The older `Easy-S31_SCH_v0.5_2026-07-28_0924.pdf` is retained only as
pre-fabrication history and must not be used for reference designators.

Mechanical files, fabrication outputs, and a graphical pinout will be added when they are ready for public use. Empty placeholder directories are not kept in the repository.

Third-party IC datasheets are not copied here. Use the component manufacturer's current document unless a distributable file is required to reproduce a released design.
