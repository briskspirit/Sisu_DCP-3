# Documentation Index

This directory contains maintained engineering contracts, hardware errata,
calibration records, implementation research, and reference provenance.
Completed task plans, implementation diaries, review reports, and transient
bench logs do not belong in the tracked documentation set.

## Annotation Convention

Source comments use one project-specific tag: `[BP]` marks a **bench-pending**
assumption — a value or behavior chosen from analysis, reverse engineering, or
datasheet reading that still wants verification on real hardware. A `[BP]`
claim is the current best understanding, not a measured fact.

## Maintained Contracts

- [Rev B2 hardware contract](revb2_hardware_contract.md): production target,
  pins, electrical ownership, Telit integration, and accepted hardware limits.
- [Powered-on standby](standby_power.md): quiet-clock and clock-dormant policy,
  wake sources, measured current, and regression gate.
- [Power-off design](power_off_design.md): soft-off to POWMAN P1.7 transition,
  wake behavior, and the measured off-state floor.
- [Runtime watchdog](runtime_watchdog.md): main-loop/flash lease ownership,
  reset provenance, dormant assumptions, and Rev B2 verification.
- [Modem shutdown recovery](modem_shutdown_recovery.md): graceful shutdown,
  hardware recovery escalation, and fail-closed rail ownership.
- [Battery gauge](battery_gauge_design.md): LTC2959 authority, load-normalized
  voltage filtering, warning policy, and safety boundaries.
- [Battery health and SOC supervisor](battery_learning.md): qualified capacity
  cycles, persistent provenance-tagged SOC, resistance bins, and qualification
  scope.
- [Battery charge supervisor](battery_charge_supervisor_design.md): implemented
  charge accounting, opt-in durable `/CE` enforcement, terminal provenance,
  reset continuity, and its qualification boundary.
- [CLCC call model](call_model.md): id-authoritative call state, transactions,
  projection, and the host-test oracle.
- [Storage engine](storage_engine.md): flash journals, persistent records, and
  modem-backed SMS/phonebook boundaries.
- [Net Monitor](net_monitor.md): page registry, typed query/control ownership,
  and diagnostic regression coverage.

## Calibration And Research

- [LTC2959 ACR calibration](ltc2959_acr_calibration.md): data-sheet scale,
  measured Rev B2 correction, persistence boundary, and qualification scope.
- [Nokia v6.00 battery-gauge reconstruction](battery_gauge_nokia_v600_research.md):
  ROM-grounded load compensation, Rev B2 adaptation evidence, and the origin
  of the implemented estimator and capacity/SOC extension.

## UI And Protocol Records

- [Operator names](operator_names.md): serving-PLMN database, SIM-name fallback,
  numeric fallback, and mapping provenance.
- [LCD calibration](lcd_calibration.md): Nokia controller ground truth,
  aftermarket-panel tuning, persistence, and adjustment controls.
- [Visual voicemail control SMS](visual_voicemail_sms.md): protocol
  classification, filtering, and message-waiting authority.
- [`*#0000#` service screen](service_0000.md): the intentional build-identity
  deviation from Nokia v6.00.

## Hardware Errata

- [Charger-enable recovery](revb2_charger_enable_erratum.md): Rev B2
  design-source pull-down omission, required board rework, and bounded firmware
  mitigation.
- [USB VBUS backfeed](revb2_usb_vbus_backfeed_erratum.md): mandatory prototype
  rework and the next-board requirement.

Errata remain even after a prototype is reworked. They define the required
state of every Rev B2 board and prevent a later build or assembly from silently
reintroducing the fault.

## Original References

- [Nokia 3210 reference index](nokia_3210_reference_index.md): NSE-8/NSE-9
  service-document provenance, hashes, and public sources (the documents
  themselves stay under the ignored `private/` tree).

Telit manuals and other distribution-controlled vendor material live under the
ignored `private/` tree. Original firmware build inputs live under the ignored
`firmware/` tree; only provenance and hashes are tracked.
