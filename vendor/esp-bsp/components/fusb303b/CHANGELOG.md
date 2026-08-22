# ChangeLog

## v0.2.0 - 2026-07-31

### Features

* Add `fusb303b_set_interrupt_mask` for the per-event Mask (0Eh) and Mask1 (0Fh) registers, with the `FUSB303B_MASK_*`/`FUSB303B_MASK1_*` bit definitions
* Add `fusb303b_read_register`/`fusb303b_write_register` raw register access for board-level policy

### Enhancements

* Cache the read-only identity registers at create time instead of re-reading them on every `fusb303b_get_status` call
* Reject a NULL handle up front in `fusb303b_get_status`, consistent with the other entry points
* Test app: move the I2C test-bus pins into `Kconfig.projbuild` (`FUSB303B_TEST_I2C_SCL`/`FUSB303B_TEST_I2C_SDA`, defaults 33/34) so the tests build on targets without GPIO34

## v0.1.0 - 2026-07-30

### Features

* Initial version: create/delete with identity check, enable, role and source-current selection, status snapshot, and interrupt read/clear
