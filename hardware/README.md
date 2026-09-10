# Hardware

This directory contains the public hardware reference for Candis-S31.

| Resource | Description | Status |
|---|---|---|
| [Schematic](schematic/SCH_Schematic_3_2026-08-10.pdf) | Fabrication baseline `v0.5_260803_1544`, exported 2026-08-10 | EVT1 |
| [Pinout](pinout/README.md) | GPIO groups derived from the revision 0.5 schematic | EVT1 |
| [Bring-up notes](bring-up.md) | EVT1 inspection, first power-on, and validation stages | EVT1 |
| [As-fabricated facts](facts.md) | EVT1 pad→net→function, TG28 rail, and connector tables from the PCB_3 netlist | Verified vs PCB netlist |

EVT1 revision 0.5 (`v0.5_260803_1544`) boards are fabricated and in use. The
schematic and pinout are the design authority; recorded measurements and
behavior are listed separately in [`facts.md`](facts.md) and
[`bring-up.md`](bring-up.md). Pin names, active levels, power sequencing, and
connector behavior can change between board revisions; verify them against the
schematic for the revision in hand.

Mechanical files, fabrication outputs, and a graphical pinout are not published
here. Empty placeholder directories are not kept in the repository.

Third-party IC datasheets are not copied here. Use the component manufacturer's current document unless a distributable file is required to reproduce a released design.
