# ath11k NSS WiFi-offload — upstreaming backlog

The two patches in this directory:

  * `950-ath11k-nss-wifi-offload.patch` — functional RX+TX offload hooks.
  * `961-ath11k-nss-offload-diag.patch` — all diagnostics behind `ath11k_nss_diag`
    (default off => zero datapath cost). Droppable for a clean submission.

This is the OpenWrt-NSS-series form (hardcoded NSS-reserved DDR physical
addresses). It is **not** mainline-ready. To submit to linux-wireless, the
following must be reworked:

1. **No hardcoded physical addresses.** `ATH11K_NSS_HANDOFF_PA` (0x40250000),
   `ATH11K_NSS_RELAY_PA` (0x40250400) and the EAPOL exc-ring window (0x40210000)
   must come from a `reserved-memory` / DT phandle (or the qca-nss-drv handoff
   ABI), not `#define`s. The `0x60000` mbox window size in the exc drain likewise.

2. **Capability/Kconfig gate.** The whole hook should compile out unless an
   `ATH11K_NSS_SUPPORT` Kconfig (and a per-device hw_param cap) is set, so non-IPQ
   targets carry no NSS code. Today it is always compiled and only runtime-gated.

3. **Slot allocator.** `ath11k_nss_dev_idx` is monotonic (`atomic_fetch_inc`);
   `ath11k_dp_nss_relay_free()` now clears the slot on teardown, but allocation
   still never reuses a freed slot, so repeated probe/remove eventually exhausts
   the 4 slots. Switch allocation to a free-slot scan.

4. **EAPOL exception path is a log-only PoC.** `ath11k_nss_drain_exc()` only logs;
   it does not build/deliver an skb to mac80211. Before offload can carry secured
   (4-way-handshake) links via the exception ring, this needs a real
   exc-buffer -> skb -> `ath11k_dp_rx_deliver_msdu()` path (or confirmation that
   the recycled REO path already delivers EAPOL once the reap is un-starved, in
   which case the drain can be deleted entirely).

5. **exc_map ioremap lifetime.** `ath11k_nss_exc_map` is mapped once from a
   workqueue and never unmapped (leaks on module unload). It is only mapped when
   `ath11k_nss_diag` is set, so it is low-priority, but a real submission needs a
   teardown (and to stop using a single global window shared across devices).

6. **Endianness / 32-bit assumptions.** The DDR handoff layout is packed `u32`
   words with `(u32)` casts of 64-bit `paddr`s; fine on the 32-bit-DMA IPQ SoC,
   but the layout/casts need an explicit ABI definition for review.

---

## Host side: decommission `nss-peek`

`nss-peek.ko` (built by `package/kernel/nss-loader`) is the bring-up harness — 85
module params, 27 `rig_*` modes. It must NOT ship. Everything else is diagnostics
and gets dropped.

Three-component split (mirrors qca-nss-drv: a generic loader/transport + per-
service control "clients"):
  * `nss-loader` — STAYS firmware-AGNOSTIC. Boots an arbitrary raw UBI32 image on
    a chosen core, reports liveness. Knows nothing about WiFi/REO/EDMA. Do NOT
    push downlink-arming into it (that would couple the loader to one firmware).
  * `nss-peek` — debug harness. Drop / debug-only build.
  * NEW firmware-specific WiFi-offload control client (e.g. `nss-wifi-offload`):
    the host-side counterpart to the ath11k REO/refill handoff patch. It owns the
    arming and is matched to the loaded firmware VARIANT's mailbox ABI (offsets,
    modes) — which is exactly why it can't live in the agnostic loader.

7. **Carve the irreducible "arm the downlink" path into the new WiFi-offload
   control client (NOT `nss-loader`).** The only load-bearing entrypoint is
   `rig_txinject()` (`dllive=1`, or `dlagent=N` for multi-lane). The client needs
   just this sequence, driven by config not module params:
     - set up the inject/reap EDMA rings (RXDESC `dlring`, RXFILL `dlrf`,
       `dlnent`/`dlnbuf` entries, frame buffers);
     - publish ring geometry + egress config into the fw mailbox
       (base/hp/nent/esize, `dl_idx`, `dl_wbm_desc`, `dl_cap`, `dl_ctl`,
       resolve target = client `dmac`);
     - start the reap (`go==10` for single-ring, or `dl_spsc=N` for the
       single-egress-agent) and the ARM CONS/RXFILL relay kthread;
     - steer the CPU egress queue -> the NSS ring via EDMA QID2RID (`dlqid`),
       last (wedge-safe).
   Irreducible knob set (~12, become driver/config state): `dllive`/`dlagent`,
   `dlring`, `dlrf`, `dlnent`, `dlnbuf`, `dlqid`, `dmac`, `dlwbmdesc`, `dlidx`,
   `dlcap`, `dlctl`, plus `pcibuf`/`livedev` for the 6 GHz PCIe radio.

8. **Drop the diagnostics entirely.** The other ~70 knobs / 26 rigs are not
   production: probes (`n2htest`, `h2nprobe`, `mmioprobe`, `tcmprobe`, `tcmcoh`,
   `actprobe`, `tcmreap`, `snoop`, `recycle`, `extract`, `inject`, `wbprobe`,
   `gicscan`, `rdpa`, `dropcnt`, `isr`, `hwint`, `snapshot`), benchmarks
   (`latbench`, `bench`, `hmtpace`, `dlhmt`, `dlcont`, `sustain`, `prate`,
   `samples`, `secs`…), and bisection toggles (`dlnotp`, `dryedma`, `synth`,
   `nocmpl`, `nocons`, `nohold`, `noinj`, `norecyc`, `rfimem`, `dltest`…). They
   belong in a debug-only build artifact, never in the shipped package.

9. **`dlctl` live-tuning is a bring-up crutch.** Re-reading `MB->dl_ctl` every
   batch exists only because `rmmod`/re-insmod corrupts the running fw. Once
   arming lives in the control client (reconfigure via config, no module reload),
   the runtime bisection bits can go; keep only any gate the production datapath
   genuinely needs.
