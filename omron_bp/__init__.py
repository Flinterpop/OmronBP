"""Read stored blood-pressure records from OMRON Bluetooth LE monitors.

Supported models are listed in :mod:`omron_bp.models`.  The wire protocol
is the OMRON proprietary "BLEsmart" transport described in
:mod:`omron_bp.transport`; readings are stored per-user in the monitor's
EEPROM and are read out as fixed-size records.
"""

from omron_bp.models import Reading

__all__ = ["Reading"]
__version__ = "0.1.1"
